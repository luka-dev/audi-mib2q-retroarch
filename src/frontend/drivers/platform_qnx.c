/* RetroArch - A frontend for libretro.
 * Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 * Copyright (C) 2011-2017 - Daniel De Matteis
 *
 * RetroArch is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Found-
 * ation, either version 3 of the License, or (at your option) any later version.
 *
 * RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with RetroArch.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <libgen.h>
#include <dirent.h>
#include <signal.h>
#include <math.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/file.h>

#include <boolean.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "../../defaults.h"
#include "../../dynamic.h"
#include "../../paths.h"
#include "../../verbosity.h"

/* ------------------------------------------------------------------------
 * MHI2Q lifecycle: one-way command channel HMI -> emulator.
 *
 *   SIGUSR1 = pause   (context switch away: call/nav/media took the display)
 *   SIGUSR2 = resume
 *   SIGRTMIN = one-shot pause (stock Java audio listener reported focus loss)
 *   SIGTERM = graceful close (BACK / ignition-off / out-of-context timeout)
 *   SIGKILL = force kill (HOLD BACK / watchdog) - uncatchable by design
 *
 * Design notes (see README "Lifecycle / focus state machine"):
 *  - We do NO work in an async signal handler. A process-targeted signal on QNX
 *    lands on whatever thread isn't blocking it (could be audio or a DSI
 *    callback thread), and SRAM/EGL/QSA/DSI/mutex/logging are not
 *    async-signal-safe. So: block these signals in the main thread BEFORE
 *    RetroArch spawns its threads (they inherit the mask), then one dedicated
 *    lifecycle thread does sigwaitinfo() - normal thread context - and only
 *    flips atomics. The real work (SRAM flush, core unload, EGL teardown)
 *    happens in the runloop, which polls these atomics every frame via
 *    gfx_ctx_qnx_check_window().
 *  - Signals coalesce and are delivered by priority, so we carry a *desired
 *    state* (paused yes/no), never an edge count.
 * ---------------------------------------------------------------------- */

/* written by the lifecycle thread, polled by the runloop/video thread */
volatile sig_atomic_t qnx_lifecycle_quit   = 0; /* 1 = quit requested       */
volatile sig_atomic_t qnx_lifecycle_paused = 0; /* 1 = desired state paused */
volatile sig_atomic_t qnx_audio_focus_lost_edge = 0;

static pthread_t qnx_lifecycle_tid;
static bool      qnx_lifecycle_running = false;
static int       qnx_lock_fd           = -1;

static void *qnx_lifecycle_thread(void *unused)
{
   sigset_t set;
   sigemptyset(&set);
   sigaddset(&set, SIGTERM);
   sigaddset(&set, SIGINT);
   sigaddset(&set, SIGUSR1);
   sigaddset(&set, SIGUSR2);
   sigaddset(&set, SIGRTMIN);

   for (;;)
   {
      siginfo_t info;
      int sig = sigwaitinfo(&set, &info);
      if (sig < 0)
      {
         if (errno == EINTR)
            continue;           /* interrupted: just wait again */
         break;
      }

      switch (sig)
      {
         case SIGUSR1:
            qnx_lifecycle_paused = 1;
            break;
         case SIGUSR2:
            qnx_lifecycle_paused = 0;
            break;
         case SIGRTMIN:
            qnx_audio_focus_lost_edge = 1;
            break;
         case SIGTERM:
         case SIGINT:
            /* Only flag it. The runloop sees this via check_window() and runs
             * the real clean-exit path: CMD_EVENT_SAVE_FILES (SRAM) + core
             * unload + exit. If the runloop is wedged (e.g. stuck in
             * eglSwapBuffers), nothing here can help - that is exactly what
             * the HMI's SIGKILL escalation (HOLD BACK / watchdog) backstops. */
            qnx_lifecycle_quit = 1;
            return NULL;
         default:
            break;
      }
   }
   return NULL;
}

/* ---- single-instance lock: serialize BACK -> immediate relaunch -----------
 * The old instance must FULLY close (SRAM written) before a new one runs.
 * flock is released by the kernel even on SIGKILL, so no stale lock. The lock
 * is the authority; the PID written inside is diagnostic only. */

#define QNX_LOCK_DEFAULT "/fs/sda0/retroarch/ra.lock"
#define QNX_LOCK_WAIT_MS 8000    /* then escalate rather than hang the UI */

static bool qnx_lock_acquire(void)
{
   char buf[32];
   const char *path = getenv("RA_LOCK_PATH");
   int waited_ms    = 0;

   if (!path || !*path)
      path = QNX_LOCK_DEFAULT;

   if ((qnx_lock_fd = open(path, O_RDWR | O_CREAT, 0644)) < 0)
   {
      RARCH_WARN("[QNX]: cannot open lock %s (%s) - no launch serialization.\n",
            path, strerror(errno));
      return false;
   }
   /* don't leak the lock into a spawned child unless it's meant to hold it */
   fcntl(qnx_lock_fd, F_SETFD, FD_CLOEXEC);

   /* Write our PID FIRST, unconditionally: the HMI reads it to signal us
    * (pause/close), and that MUST work even where flock doesn't. On this unit
    * neither the FAT32 SD nor tmpfs (/tmp -> /dev/shmem) supports flock, so the
    * old "only write PID once flock succeeds" left the lockfile EMPTY and the
    * HMI unable to close RetroArch. */
   ftruncate(qnx_lock_fd, 0);
   snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
   write(qnx_lock_fd, buf, strlen(buf));
   ra_dbg("=== platform init: lock acquired, pid=%d written (tracer alive) ===",
         (int)getpid());

   /* Best-effort serialization via flock. If the fs can't lock, keep the PID
    * file + fd (for signaling) and just skip serialization -- never bail. */
   for (;;)
   {
      if (flock(qnx_lock_fd, LOCK_EX | LOCK_NB) == 0)
         break;                                    /* got the lock */

      if (errno == EINTR)
         continue;                                 /* woken by a signal */

      if (errno == ENOSYS || errno == EOPNOTSUPP)
      {
         RARCH_ERR("[QNX]: flock unsupported on %s - serialization DISABLED "
               "(PID still written; HMI can signal).\n", path);
         break;                                    /* keep fd + PID, proceed */
      }

      if (errno != EWOULDBLOCK && errno != EAGAIN)
      {
         RARCH_WARN("[QNX]: flock(%s) failed: %s - proceeding without it.\n",
               path, strerror(errno));
         break;                                    /* keep fd + PID, proceed */
      }

      /* Held by an instance still tearing down (flushing SRAM). Wait for it. */
      if (waited_ms >= QNX_LOCK_WAIT_MS)
      {
         RARCH_ERR("[QNX]: previous instance still holds %s after %d ms - "
               "it is wedged; HMI should SIGKILL it (HOLD BACK).\n",
               path, waited_ms);
         break;                                    /* proceed anyway */
      }
      usleep(100 * 1000);
      waited_ms += 100;
   }

   if (waited_ms)
      RARCH_LOG("[QNX]: waited %d ms for the previous instance to finish.\n",
            waited_ms);
   return true;
}

static void qnx_lock_release(void)
{
   if (qnx_lock_fd < 0)
      return;
   flock(qnx_lock_fd, LOCK_UN);
   close(qnx_lock_fd);
   qnx_lock_fd = -1;
}

/* Force IEEE VFP mode: clear Flush-to-Zero (FPSCR bit24) + Default-NaN (bit25).
 * A -mfpu=neon build can come up in "RunFast" mode (DN|FZ, see <arm/vfp.h>
 * ARM_VFP_RUNFAST_MODE); the device's softfp libm (powf = exp(y*log(x)) with an
 * iterative refinement loop) can then fail to converge on flushed denormals and
 * HANG. Legacy FMRX/FMXR mnemonics (gas 2.19 accepts them; vmrs/vmsr it does
 * not). Cheap; safe to call more than once. */
void ra_fpu_force_ieee(void)
{
   unsigned int fpscr;
   __asm__ volatile("fmrx %0, fpscr" : "=r"(fpscr));
   fpscr &= ~((1u << 24) | (1u << 25));   /* FZ | DN */
   __asm__ volatile("fmxr fpscr, %0" : : "r"(fpscr));
}

static void frontend_qnx_init(void *data)
{
   ra_fpu_force_ieee();     /* before ANY libm call (config_set_defaults->powf) */
   /* Warm up the ra_math.c forwarders now so their one-time dlopen("libm.so.2")
    * never fires later from INSIDE egl14.so (if the Adreno driver calls expf/pow
    * during eglGetDisplay, a re-entrant dlopen mid-init could fault). */
   { volatile float wf = expf(0.0f) + powf(2.0f, 2.0f);
     volatile double wd = exp(0.0) + pow(2.0, 2.0); (void)wf; (void)wd; }
   verbosity_enable();
   /* Serialize against a previous instance that may still be flushing SRAM
    * (BACK immediately followed by re-open from the menu). */
   qnx_lock_acquire();
}

static void frontend_qnx_shutdown(bool unused)
{
   qnx_lock_release();
}

/* ---- frontend_ctx signal-handler slots ---------------------------------- */

static void frontend_qnx_install_signal_handlers(void)
{
   sigset_t set;

   sigemptyset(&set);
   sigaddset(&set, SIGTERM);
   sigaddset(&set, SIGINT);
   sigaddset(&set, SIGUSR1);
   sigaddset(&set, SIGUSR2);
   sigaddset(&set, SIGRTMIN);

   /* Block in THIS thread first: every thread RetroArch spawns afterwards
    * inherits the mask, so the lifecycle signals can only ever be consumed by
    * our dedicated sigwaitinfo() thread - never delivered asynchronously into
    * the audio/video/DSI threads. (Also guards against a mask inherited from
    * the HMI's spawn leaving them unblocked.) */
   pthread_sigmask(SIG_BLOCK, &set, NULL);

   if (qnx_lifecycle_running)
      return;
   if (pthread_create(&qnx_lifecycle_tid, NULL, qnx_lifecycle_thread, NULL) == 0)
   {
      pthread_detach(qnx_lifecycle_tid);
      qnx_lifecycle_running = true;
      RARCH_LOG("[QNX]: lifecycle thread up "
            "(SIGUSR1=pause SIGUSR2=resume SIGRTMIN=audio-pause "
            "SIGTERM=save+quit).\n");
   }
   else
      RARCH_ERR("[QNX]: could not start lifecycle thread: %s\n", strerror(errno));
}

static int  frontend_qnx_get_signal_handler_state(void)
{
   return (int)qnx_lifecycle_quit;
}

static void frontend_qnx_set_signal_handler_state(int value)
{
   qnx_lifecycle_quit = value;
}

static void frontend_qnx_destroy_signal_handler_state(void)
{
   qnx_lifecycle_quit = 0;
}

static void frontend_qnx_get_env_settings(int *argc, char *argv[],
      void *data, void *params_data)
{
   /* MHI2Q layout (see retroarch-qnx README, Filesystem section):
    *   data_path = immutable app root -> cores/assets/autoconfig
    *   user_path = SD writable root -> config/system/saves/states/logs
    *   tmp_path  = tmpfs
    * Executable cores remain pinned to appimg by retroarch.cfg. Both roots are
    * overridable so a different mounted media layout needs no rebuild. */
   const char *env_data = getenv("RA_DATA_DIR");
   const char *env_user = getenv("RA_USER_DIR");
   const char *env_config = getenv("RA_CONFIG_PATH");
   char data_path[PATH_MAX];
   char user_path[PATH_MAX];
   char tmp_path[PATH_MAX];

   strlcpy(data_path,
         (env_data && *env_data) ? env_data : "/mnt/app/root/retroarch",
         sizeof(data_path));
   strlcpy(user_path,
         (env_user && *env_user) ? env_user : "/fs/sda0/retroarch",
         sizeof(user_path));
   strlcpy(tmp_path, "/tmp", sizeof(tmp_path));

   /* app data */
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE], data_path,
         "cores", sizeof(g_defaults.dirs[DEFAULT_DIR_CORE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_ASSETS], data_path,
         "assets", sizeof(g_defaults.dirs[DEFAULT_DIR_ASSETS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_AUTOCONFIG], data_path,
         "autoconfig", sizeof(g_defaults.dirs[DEFAULT_DIR_AUTOCONFIG]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_DATABASE], data_path,
         "database/rdb", sizeof(g_defaults.dirs[DEFAULT_DIR_DATABASE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE_INFO], data_path,
         "info", sizeof(g_defaults.dirs[DEFAULT_DIR_CORE_INFO]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_OVERLAY], data_path,
         "overlays", sizeof(g_defaults.dirs[DEFAULT_DIR_OVERLAY]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_OSK_OVERLAY], data_path,
         "overlays/keyboards", sizeof(g_defaults.dirs[DEFAULT_DIR_OSK_OVERLAY]));
   /* user data */
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CHEATS], user_path,
         "cheats", sizeof(g_defaults.dirs[DEFAULT_DIR_CHEATS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG], user_path,
         "config", sizeof(g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_MENU_CONTENT], user_path,
         "content", sizeof(g_defaults.dirs[DEFAULT_DIR_MENU_CONTENT]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE_ASSETS], user_path,
         "downloads", sizeof(g_defaults.dirs[DEFAULT_DIR_CORE_ASSETS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_AUDIO_FILTER], user_path,
         "filters/audio", sizeof(g_defaults.dirs[DEFAULT_DIR_AUDIO_FILTER]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_PLAYLIST], user_path,
         "playlists", sizeof(g_defaults.dirs[DEFAULT_DIR_PLAYLIST]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_REMAP], g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG],
         "remaps", sizeof(g_defaults.dirs[DEFAULT_DIR_REMAP]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SRAM], user_path,
         "saves", sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SCREENSHOT], user_path,
         "screenshots", sizeof(g_defaults.dirs[DEFAULT_DIR_SCREENSHOT]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], user_path,
         "states", sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SYSTEM], user_path,
         "system", sizeof(g_defaults.dirs[DEFAULT_DIR_SYSTEM]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_WALLPAPERS], user_path,
         "wallpapers", sizeof(g_defaults.dirs[DEFAULT_DIR_WALLPAPERS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_LOGS], user_path,
         "logs", sizeof(g_defaults.dirs[DEFAULT_DIR_LOGS]));

   /* tmp */
   strlcpy(g_defaults.dirs[DEFAULT_DIR_CACHE],
         tmp_path, sizeof(g_defaults.dirs[DEFAULT_DIR_CACHE]));

   /* history and main config */
   strlcpy(g_defaults.dirs[DEFAULT_DIR_CONTENT_HISTORY],
         user_path, sizeof(g_defaults.dirs[DEFAULT_DIR_CONTENT_HISTORY]));
   if (env_config && *env_config)
      strlcpy(g_defaults.path_config, env_config,
            sizeof(g_defaults.path_config));
   else
      fill_pathname_join(g_defaults.path_config, user_path,
            FILE_PATH_MAIN_CONFIG, sizeof(g_defaults.path_config));

   #ifndef IS_SALAMANDER
   dir_check_defaults("custom.ini");
#endif
}

enum frontend_architecture frontend_qnx_get_arch(void)
{
   return FRONTEND_ARCH_ARM;
}

frontend_ctx_driver_t frontend_ctx_qnx = {
   frontend_qnx_get_env_settings,
   frontend_qnx_init,
   NULL,                         /* deinit */
   NULL,                         /* exitspawn */
   NULL,                         /* process_args */
   NULL,                         /* exec */
   NULL,                         /* set_fork */
   frontend_qnx_shutdown,
   NULL,                         /* get_name */
   NULL,                         /* get_os */
   NULL,                         /* load_content */
   frontend_qnx_get_arch,        /* get_architecture */
   NULL,                         /* get_powerstate */
   NULL,                         /* parse_drive_list */
   NULL,                         /* get_total_mem */
   NULL,                         /* get_free_mem */
   frontend_qnx_install_signal_handlers,
   frontend_qnx_get_signal_handler_state,
   frontend_qnx_set_signal_handler_state,
   frontend_qnx_destroy_signal_handler_state,
   NULL,                         /* attach_console */
   NULL,                         /* detach_console */
   NULL,                         /* get_lakka_version */
   NULL,                         /* set_screen_brightness */
   NULL,                         /* watch_path_for_changes */
   NULL,                         /* check_for_path_changes */
   NULL,                         /* set_sustained_performance_mode */
   NULL,                         /* get_cpu_model_name */
   NULL,                         /* get_user_language */
   NULL,                         /* is_narrator_running */
   NULL,                         /* accessibility_speak */
   NULL,                         /* set_gamemode        */
   NULL, /* get_display_type */
   "qnx",                        /* ident               */
   NULL                          /* get_video_driver    */
};
