/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* MHI2Q (Audi) QNX 6.5 EGL/GLES2 context.
 *
 * The Kanzi HMI owns QNX Screen; an app does NOT create its own Screen window.
 * Instead we go through the firmware's libdisplayinit.so (same path gpSP /
 * pcsx_rearmed use), which wraps screen_create_window + registers a compositor
 * "displayable" and hands back an EGLNativeWindowType. RetroArch uses its own
 * full-screen display context (default 90) containing only displayable 43
 * (DIGITAL_VIDEOPLAYER_1). This deliberately excludes DISPLAYABLE_HMI (16),
 * including the persistent lower status bar. The Java SystemSMM state still
 * owns entry/exit and restores the previous OEM context on every transition.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_EGL
#include <EGL/egl.h>
#include "../common/egl_common.h"
#endif

#include "../../configuration.h"
#include "../../verbosity.h"
#include "../../command.h"
#include "../../audio/audio_driver.h"
#include "../../frontend/frontend_driver.h"

/* Lifecycle atomics, defined in frontend/drivers/platform_qnx.c (which the
 * griffin unity TU pulls in later). SIGUSR1/2 set the desired pause state,
 * SIGTERM sets quit; we reconcile against them once per frame here, on the
 * main thread - never in a signal handler. */
extern volatile sig_atomic_t qnx_lifecycle_paused;
extern volatile sig_atomic_t qnx_audio_focus_command_edge;
extern volatile sig_atomic_t qnx_audio_focus_fallback_state;

#define QNX_AUDIO_DESIRED_DEFAULT "/tmp/retroarch.audio.desired"

static int qnx_read_audio_focus_desired(void)
{
   char state = '\0';
   int fd = open(QNX_AUDIO_DESIRED_DEFAULT, O_RDONLY);
   if (fd >= 0)
   {
      ssize_t got = read(fd, &state, 1);
      close(fd);
      if (got == 1 && (state == '0' || state == '1'))
         return state == '1' ? 1 : 0;
   }
   return (int)qnx_audio_focus_fallback_state;
}

/* libdisplayinit.so entry points (dlopen'd at runtime; not in the SDP sysroot). */
typedef void (*display_init_fn)(int, int);
typedef int  (*display_create_window_fn)(EGLDisplay, EGLConfig, int, int, int,
      EGLNativeWindowType *, int *);
typedef int  (*display_create_window_nbuffers_fn)(EGLDisplay, EGLConfig,
      int, int, int, int, EGLNativeWindowType *, int *);
typedef int  (*display_get_resolution_fn)(int *, int *);

/* libdisplayinit's EGLNativeWindowType is the underlying screen_window_t.
 * Keep Screen opaque here: the stock QNX 6.5 SDP used by this build does not
 * ship the firmware's Screen headers/libraries, so all entry points are
 * resolved from /proc/boot/libscreen.so.1 at runtime. */
typedef void *qnx_screen_window_t;
typedef void *qnx_screen_display_t;
typedef void *qnx_screen_context_t;
typedef int (*screen_get_window_property_iv_fn)(qnx_screen_window_t, int,
      int *);
typedef int (*screen_get_window_property_pv_fn)(qnx_screen_window_t, int,
      void **);
typedef int (*screen_set_window_property_iv_fn)(qnx_screen_window_t, int,
      const int *);
typedef int (*screen_create_window_buffers_fn)(qnx_screen_window_t, int);
typedef int (*screen_destroy_window_buffers_fn)(qnx_screen_window_t);
typedef int (*screen_get_context_property_iv_fn)(qnx_screen_context_t, int,
      int *);
typedef int (*screen_get_context_property_pv_fn)(qnx_screen_context_t, int,
      void **);
typedef int (*screen_wait_vsync_fn)(qnx_screen_display_t);

enum
{
   QNX_SCREEN_PROPERTY_BUFFER_COUNT = 4,
   QNX_SCREEN_PROPERTY_DISPLAY       = 11,
   QNX_SCREEN_PROPERTY_SWAP_INTERVAL = 45,
   QNX_SCREEN_PROPERTY_RENDER_BUFFER_COUNT = 53,
   QNX_SCREEN_PROPERTY_DISPLAY_COUNT = 59,
   QNX_SCREEN_PROPERTY_DISPLAYS      = 60,
   QNX_SCREEN_PROPERTY_CONTEXT       = 95
};

#define QNX_SCREEN_VSYNC_TIMEOUT_NS UINT64_C(25000000)
#define QNX_SOFTWARE_REFRESH_NS     UINT64_C(16666667)

static const char *g_displib_paths[] = {
   "/eso/lib/libdisplayinit.so",
   "/mnt/app/eso/lib/libdisplayinit.so",
   "libdisplayinit.so",
   NULL
};

static const char *g_screenlib_paths[] = {
   "/proc/boot/libscreen.so.1",
   "libscreen.so.1",
   NULL
};

typedef struct
{
#ifdef HAVE_EGL
   egl_ctx_data_t egl;
#endif
   void                    *displib;
   void                    *screenlib;
   EGLNativeWindowType      native_window;
   qnx_screen_display_t     screen_display;
   screen_get_window_property_iv_fn screen_get_window_property_iv;
   screen_set_window_property_iv_fn screen_set_window_property_iv;
   screen_create_window_buffers_fn screen_create_window_buffers;
   screen_destroy_window_buffers_fn screen_destroy_window_buffers;
   screen_wait_vsync_fn     screen_wait_vsync;
   int                      kd_window;
   int                      displayable_id;
   int                      context_id;   /* our own display-manager context */
   int                      display_id;    /* physical display (0 = main) */
   bool                     routed;        /* did we switch the display to us? */
   unsigned                 width;
   unsigned                 height;
   bool                     resize;
   bool                     vsync_requested;
   bool                     screen_vsync_capable;
   bool                     screen_vsync_failed;
   uint64_t                 software_vsync_deadline_ns;
} qnx_ctx_data_t;

static void ra_dbg(const char *fmt, ...);

static int qnx_requested_screen_buffers(void)
{
   const char *value = getenv("RA_QNX_SCREEN_BUFFERS");
   int requested     = (value && *value) ? atoi(value) : 3;

   if (requested < 2 || requested > 4)
   {
      ra_dbg("ignoring invalid RA_QNX_SCREEN_BUFFERS=%d (valid 2..4)",
            requested);
      requested = 3;
   }

   return requested;
}

static uint64_t qnx_monotonic_time_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
         (uint64_t)now.tv_nsec;
}

/* Cap presentation at the panel's nominal 60 Hz if explicit Screen vsync is
 * unavailable. This also keeps a video_vsync=true configuration fail-safe:
 * the runloop must never become unlimited merely because Screen rejected the
 * display handle. */
static void qnx_wait_software_vsync(qnx_ctx_data_t *qnx)
{
   uint64_t now = qnx_monotonic_time_ns();

   if (!now)
      return;

   if (!qnx->software_vsync_deadline_ns ||
       now > qnx->software_vsync_deadline_ns + QNX_SOFTWARE_REFRESH_NS * 4)
      qnx->software_vsync_deadline_ns = now + QNX_SOFTWARE_REFRESH_NS;

   if (now < qnx->software_vsync_deadline_ns)
   {
      uint64_t remaining = qnx->software_vsync_deadline_ns - now;
      struct timespec req;
      struct timespec rem;

      req.tv_sec  = (time_t)(remaining / UINT64_C(1000000000));
      req.tv_nsec = (long)(remaining % UINT64_C(1000000000));
      while (nanosleep(&req, &rem) != 0 && errno == EINTR)
         req = rem;
   }

   now = qnx_monotonic_time_ns();
   do
      qnx->software_vsync_deadline_ns += QNX_SOFTWARE_REFRESH_NS;
   while (qnx->software_vsync_deadline_ns <= now);
}

/* screen_wait_vsync() in the HU's libscreen.so.1 is a single MsgSend. Arm a
 * QNX kernel timeout for both SEND and REPLY states before entering it, so a
 * missing display-manager acknowledgement can delay one frame by at most
 * 25 ms instead of freezing RetroArch's render/input thread forever. */
static bool qnx_wait_screen_vsync(qnx_ctx_data_t *qnx)
{
   _Uint64t timeout_ns = (_Uint64t)QNX_SCREEN_VSYNC_TIMEOUT_NS;

   if (!qnx->screen_vsync_capable || qnx->screen_vsync_failed)
      return false;

   if (TimerTimeout(CLOCK_MONOTONIC,
            _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
            NULL, &timeout_ns, NULL) != 0)
   {
      ra_dbg("Screen vsync disabled: TimerTimeout failed errno=%d", errno);
      qnx->screen_vsync_failed = true;
      return false;
   }

   if (qnx->screen_wait_vsync(qnx->screen_display) != 0)
   {
      ra_dbg("Screen vsync disabled: screen_wait_vsync failed errno=%d", errno);
      qnx->screen_vsync_failed = true;
      return false;
   }

   return true;
}

/* Resolve the physical Screen display belonging to libdisplayinit's native
 * window. A window on the default display is allowed to report DISPLAY=NULL;
 * in that case obtain the first display from its owning Screen context. */
static void qnx_init_screen_vsync(qnx_ctx_data_t *qnx)
{
   const char **p;
   screen_get_window_property_pv_fn get_window_pv = NULL;
   screen_get_context_property_iv_fn get_context_iv = NULL;
   screen_get_context_property_pv_fn get_context_pv = NULL;
   qnx_screen_context_t screen_context = NULL;

   for (p = g_screenlib_paths; *p && !qnx->screenlib; p++)
      qnx->screenlib = dlopen(*p, RTLD_NOW | RTLD_GLOBAL);
   if (!qnx->screenlib)
   {
      ra_dbg("Screen vsync unavailable: dlopen(libscreen.so.1): %s", dlerror());
      return;
   }

   get_window_pv = (screen_get_window_property_pv_fn)dlsym(qnx->screenlib,
         "screen_get_window_property_pv");
   qnx->screen_get_window_property_iv =
         (screen_get_window_property_iv_fn)dlsym(qnx->screenlib,
               "screen_get_window_property_iv");
   qnx->screen_set_window_property_iv =
         (screen_set_window_property_iv_fn)dlsym(qnx->screenlib,
               "screen_set_window_property_iv");
   qnx->screen_create_window_buffers =
         (screen_create_window_buffers_fn)dlsym(qnx->screenlib,
               "screen_create_window_buffers");
   qnx->screen_destroy_window_buffers =
         (screen_destroy_window_buffers_fn)dlsym(qnx->screenlib,
               "screen_destroy_window_buffers");
   get_context_iv = (screen_get_context_property_iv_fn)dlsym(qnx->screenlib,
         "screen_get_context_property_iv");
   get_context_pv = (screen_get_context_property_pv_fn)dlsym(qnx->screenlib,
         "screen_get_context_property_pv");
   qnx->screen_wait_vsync = (screen_wait_vsync_fn)dlsym(qnx->screenlib,
         "screen_wait_vsync");

   if (!get_window_pv || !qnx->screen_wait_vsync)
   {
      ra_dbg("Screen vsync unavailable: get_window=%p wait_vsync=%p",
            (void*)get_window_pv, (void*)qnx->screen_wait_vsync);
      return;
   }

   if (get_window_pv((qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_DISPLAY,
            (void**)&qnx->screen_display) != 0)
   {
      ra_dbg("Screen DISPLAY query failed errno=%d", errno);
      qnx->screen_display = NULL;
   }

   if (!qnx->screen_display && get_context_iv && get_context_pv &&
       get_window_pv((qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_CONTEXT, (void**)&screen_context) == 0 &&
       screen_context)
   {
      int display_count = 0;
      if (get_context_iv(screen_context, QNX_SCREEN_PROPERTY_DISPLAY_COUNT,
               &display_count) == 0 && display_count > 0 && display_count < 32)
      {
         void **displays = (void**)calloc((size_t)display_count, sizeof(void*));
         if (displays)
         {
            if (get_context_pv(screen_context, QNX_SCREEN_PROPERTY_DISPLAYS,
                     displays) == 0)
               qnx->screen_display = (qnx_screen_display_t)displays[0];
            free(displays);
         }
      }
   }

   qnx->screen_vsync_capable = qnx->screen_display != NULL;
   ra_dbg("Screen vsync probe: display=%p capable=%d",
         qnx->screen_display, (int)qnx->screen_vsync_capable);
   if (qnx->screen_get_window_property_iv)
   {
      int interval = -1;
      int buffers = -1;
      int render_buffers = -1;
      qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_SWAP_INTERVAL, &interval);
      qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_BUFFER_COUNT, &buffers);
      qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_RENDER_BUFFER_COUNT, &render_buffers);
      ra_dbg("Screen window initial interval=%d buffers=%d render=%d",
            interval, buffers, render_buffers);
   }
}

/* The legacy display_create_window() creates a two-buffer native window. On
 * this Screen/Adreno
 * stack, interval zero alone does not make that window asynchronous: while
 * the compositor owns the posted buffer and GLES renders into the other one,
 * eglSwapBuffers() has no third render buffer to dequeue and waits for the
 * next scanout. That costs almost a full 16.7 ms on software-rendered cores;
 * core work then lands on top and turns a 60 Hz core into ~55 Hz, which in
 * turn starves real-time audio even though QSA and the resampler are healthy.
 *
 * Prefer display_create_window_nbuffers() so three buffers exist from the
 * outset. This function validates the result and is also the compatibility
 * fallback for firmware which exports only the legacy wrapper. Keep fallback
 * resizing transactional: if the requested allocation fails, restore the
 * original count. Continuing with a bufferless native window can fault the
 * vendor EGL driver, so report failure if even restoration fails. */
static bool qnx_configure_screen_buffers(qnx_ctx_data_t *qnx, int requested)
{
   int original      = 0;
   int readback      = 0;

   if (!qnx->screen_get_window_property_iv ||
       !qnx->screen_create_window_buffers ||
       !qnx->screen_destroy_window_buffers)
   {
      ra_dbg("Screen buffer resize unavailable: get=%p create=%p destroy=%p",
            (void*)qnx->screen_get_window_property_iv,
            (void*)qnx->screen_create_window_buffers,
            (void*)qnx->screen_destroy_window_buffers);
      return true;
   }

   if (qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_RENDER_BUFFER_COUNT, &original) != 0 ||
       original <= 0)
   {
      ra_dbg("Screen render-buffer count query failed errno=%d", errno);
      return true;
   }
   if (original == requested)
   {
      ra_dbg("Screen render buffers already %d", original);
      return true;
   }

   if (qnx->screen_destroy_window_buffers(
            (qnx_screen_window_t)qnx->native_window) != 0)
   {
      ra_dbg("Screen destroy %d buffers failed errno=%d; keeping original",
            original, errno);
      return true;
   }

   if (qnx->screen_create_window_buffers(
            (qnx_screen_window_t)qnx->native_window, requested) == 0)
   {
      qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_RENDER_BUFFER_COUNT, &readback);
      ra_dbg("Screen render buffers recreated %d -> %d (readback=%d)",
            original, requested, readback);
      return true;
   }

   ra_dbg("Screen create %d buffers failed errno=%d; restoring %d",
         requested, errno, original);
   if (qnx->screen_create_window_buffers(
            (qnx_screen_window_t)qnx->native_window, original) == 0)
   {
      ra_dbg("Screen render buffers restored to %d", original);
      return true;
   }

   ra_dbg("FATAL Screen buffer restoration to %d failed errno=%d",
         original, errno);
   return false;
}

/* Set Screen as well as EGL. eglsub-screen.so keeps its own swap state, while
 * the native window created by libdisplayinit also has a Screen property. The
 * firmware accepts eglSwapInterval(0) without consistently updating the
 * latter, so keep both sides explicit. Calling this before eglCreateSurface()
 * also prevents the subdriver from snapshotting libdisplayinit's default 1. */
static void qnx_set_native_swap_interval(qnx_ctx_data_t *qnx, int interval)
{
   if (qnx->screen_set_window_property_iv &&
       qnx->screen_set_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_SWAP_INTERVAL, &interval) != 0)
      ra_dbg("Screen swap interval=%d failed errno=%d", interval, errno);
   else if (qnx->screen_get_window_property_iv)
   {
      int readback = -1;
      qnx->screen_get_window_property_iv(
            (qnx_screen_window_t)qnx->native_window,
            QNX_SCREEN_PROPERTY_SWAP_INTERVAL, &readback);
      ra_dbg("Screen swap interval requested=%d readback=%d",
            interval, readback);
   }
}

/* Declare a private context containing only our video layer and route the main
 * display to it. Context 25 is not full-screen: stock MIB2High defines it as
 * { DISPLAYABLE_HMI 16, DIGITAL_VIDEOPLAYER_1 43 }, so the HMI/status bar stays
 * above the video. Context 90={43} is the proven gpSP/PCSX path and leaves the
 * Java state machine responsible for restoring the previous OEM context. */
static bool qnx_route_context(qnx_ctx_data_t *qnx)
{
   char cmd[128];
   snprintf(cmd, sizeof(cmd), "/eso/bin/apps/dmdt dc %d %d",
         qnx->context_id, qnx->displayable_id);
   if (system(cmd) != 0)
   {
      RARCH_WARN("[QNX]: '%s' failed — context %d not declared.\n",
            cmd, qnx->context_id);
      ra_dbg("FAIL declare display context: %s", cmd);
      return false;
   }

   snprintf(cmd, sizeof(cmd), "/eso/bin/apps/dmdt sc %d %d",
         qnx->display_id, qnx->context_id);
   if (system(cmd) != 0)
   {
      RARCH_WARN("[QNX]: '%s' failed — context %d not routed.\n",
            cmd, qnx->context_id);
      ra_dbg("FAIL route display context: %s", cmd);
      return false;
   }

   qnx->routed = true;
   ra_dbg("routed display %d -> context %d {%d}", qnx->display_id,
         qnx->context_id, qnx->displayable_id);
   RARCH_LOG("[QNX]: routed display %d to context %d {%d}.\n",
         qnx->display_id, qnx->context_id, qnx->displayable_id);
   return true;
}

static void gfx_ctx_qnx_destroy(void *data)
{
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   if (!qnx)
      return;

#ifdef HAVE_EGL
   egl_destroy(&qnx->egl);
#endif
   if (qnx->displib)
      dlclose(qnx->displib);
   if (qnx->screenlib)
      dlclose(qnx->screenlib);

   free(data);
}

/* TEMP bring-up tracer: RARCH_LOG is a no-op in this build's boot path (verbosity
 * fp never wired to stderr), so trace display init straight to a file with fflush
 * per line. Read /tmp/ra_display.log over ssh. Remove once video is confirmed. */
static void ra_dbg(const char *fmt, ...)
{
   char buf[512];
   int n, fd;
   va_list ap;
   va_start(ap, fmt);
   n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if (n > (int)sizeof(buf) - 2)
      n = sizeof(buf) - 2;
   buf[n++] = '\n';
   /* raw open/write (NOT stdio): on this build RA's FILE* logging is dead on
    * boot, but raw syscalls work (the lockfile proves it). */
   fd = open("/tmp/ra_display.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
   if (fd < 0)
      return;
   write(fd, buf, (size_t)n);
   close(fd);
}

static unsigned qnx_screen_dimension(const char *name, unsigned fallback)
{
   char *end                    = NULL;
   const char *value           = getenv(name);
   unsigned long parsed_value;

   if (!value || !*value)
      return fallback;

   parsed_value = strtoul(value, &end, 10);
   if (end == value || *end != '\0' || parsed_value == 0 || parsed_value > 8192)
   {
      RARCH_WARN("[QNX]: ignoring invalid %s='%s'.\n", name, value);
      return fallback;
   }

   return (unsigned)parsed_value;
}

static void *gfx_ctx_qnx_init(void *video_driver)
{
   EGLint major, minor, n;
   const char **p;
   display_init_fn            display_init            = NULL;
   display_create_window_fn   display_create_window   = NULL;
   display_create_window_nbuffers_fn display_create_window_nbuffers = NULL;
   display_get_resolution_fn  display_get_resolution  = NULL;
   const char                *env_disp                = getenv("RA_QNX_DISPLAYABLE_ID");
   int                        requested_buffers       = qnx_requested_screen_buffers();
   int                        res_w = 0, res_h = 0;

   EGLint context_attributes[] = {
#ifdef HAVE_OPENGLES2
      EGL_CONTEXT_CLIENT_VERSION, 2,
#elif defined(HAVE_OPENGLES3)
      EGL_CONTEXT_CLIENT_VERSION, 3,
#endif
      EGL_NONE
   };
   const EGLint attribs[] = {
#ifdef HAVE_OPENGLES2
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
#elif defined(HAVE_OPENGLES3)
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
#endif
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      /* Match the proven standalone gpSP selection. Asking for one bit makes
       * EGL choose the native window format instead of forcing a particular
       * 32-bit config onto the display-manager layer. */
      EGL_RED_SIZE,   1,
      EGL_GREEN_SIZE, 1,
      EGL_BLUE_SIZE,  1,
      EGL_ALPHA_SIZE, 1,
      EGL_NONE
   };

   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)calloc(1, sizeof(*qnx));
   if (!qnx)
      return NULL;

   /* 43 = DISPLAYABLE_DIGITAL_VIDEOPLAYER_1 (free, chrome-less). Override with
    * RA_QNX_DISPLAYABLE_ID; 200 mimics gpSP's dedicated full-overlay layer. */
   qnx->displayable_id = (env_disp && *env_disp) ? atoi(env_disp) : 43;
   {
      /* our own display-manager context (gpSP used 90) + physical display 0 */
      const char *env_ctx  = getenv("RA_QNX_CONTEXT_ID");
      const char *env_dsp  = getenv("RA_QNX_DISPLAY_ID");
      qnx->context_id = (env_ctx && *env_ctx) ? atoi(env_ctx) : 90;
      qnx->display_id = (env_dsp && *env_dsp) ? atoi(env_dsp) : 0;
   }

   ra_dbg("=== gfx_ctx_qnx_init: displayable=%d ctx=%d disp=%d ===",
         qnx->displayable_id, qnx->context_id, qnx->display_id);

#ifdef HAVE_EGL
   /* gpSP/pcsx_rearmed order: bring EGL up (eglGetDisplay/Initialize/ChooseConfig)
    * BEFORE display_init(0,0). Calling display_init first faults the Adreno
    * egl14.so inside eglGetDisplay (SIGSEGV). */
   if (!egl_init_context(&qnx->egl, EGL_NONE, EGL_DEFAULT_DISPLAY,
            &major, &minor, &n, attribs, NULL))
   {
      ra_dbg("FAIL egl_init_context");
      goto egl_error;
   }
   ra_dbg("ok egl_init_context (EGL %d.%d, dpy=%p config=%p n=%d)",
         (int)major, (int)minor, (void*)qnx->egl.dpy, (void*)qnx->egl.config, (int)n);
   {
      EGLint red = 0, green = 0, blue = 0, alpha = 0, buffer = 0, config_id = 0;
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_RED_SIZE, &red);
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_GREEN_SIZE, &green);
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_BLUE_SIZE, &blue);
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_ALPHA_SIZE, &alpha);
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_BUFFER_SIZE, &buffer);
      eglGetConfigAttrib(qnx->egl.dpy, qnx->egl.config, EGL_CONFIG_ID, &config_id);
      RARCH_LOG("[QNX]: EGL config id=%d rgba=%d/%d/%d/%d buffer=%d.\n",
            (int)config_id, (int)red, (int)green, (int)blue, (int)alpha,
            (int)buffer);
      ra_dbg("EGL config id=%d rgba=%d/%d/%d/%d buffer=%d",
            (int)config_id, (int)red, (int)green, (int)blue, (int)alpha,
            (int)buffer);
   }
#endif

   RARCH_LOG("[QNX]: loading libdisplayinit (displayable=%d)...\n",
         qnx->displayable_id);
   for (p = g_displib_paths; *p && !qnx->displib; p++)
      qnx->displib = dlopen(*p, RTLD_NOW | RTLD_GLOBAL);
   if (!qnx->displib)
   {
      ra_dbg("FAIL dlopen(libdisplayinit.so): %s", dlerror());
      RARCH_ERR("[QNX]: dlopen(libdisplayinit.so) failed: %s\n", dlerror());
      goto error;
   }
   ra_dbg("ok dlopen libdisplayinit");

   display_init           = (display_init_fn)          dlsym(qnx->displib, "display_init");
   display_create_window  = (display_create_window_fn) dlsym(qnx->displib, "display_create_window");
   display_create_window_nbuffers = (display_create_window_nbuffers_fn)
         dlsym(qnx->displib, "display_create_window_nbuffers");
   display_get_resolution = (display_get_resolution_fn)dlsym(qnx->displib, "display_get_resolution");
   if (!display_init || (!display_create_window && !display_create_window_nbuffers))
   {
      ra_dbg("FAIL dlsym: display_init=%p create=%p create_nbuffers=%p",
            (void*)display_init, (void*)display_create_window,
            (void*)display_create_window_nbuffers);
      RARCH_ERR("[QNX]: missing display_init/window-creation symbols.\n");
      goto error;
   }
   ra_dbg("ok dlsym (init=%p cw=%p cwnb=%p getres=%p)",
         (void*)display_init, (void*)display_create_window,
         (void*)display_create_window_nbuffers, (void*)display_get_resolution);

   display_init(0, 0);
   ra_dbg("ok display_init(0,0) returned");

   if (display_get_resolution && display_get_resolution(&res_w, &res_h) == 0
         && res_w > 0 && res_h > 0)
      ra_dbg("native display resolution -> %dx%d", res_w, res_h);

   /* DIGITAL_VIDEOPLAYER_1 is rendered as a 1024x480 layer. Keep this
    * independent of the HMI's reported logical resolution (often 800x480). */
   qnx->width  = qnx_screen_dimension("RA_QNX_SCREEN_W", 1024);
   qnx->height = qnx_screen_dimension("RA_QNX_SCREEN_H", 480);

   ra_dbg("resolution -> %ux%u", qnx->width, qnx->height);
#ifdef HAVE_EGL
   {
      /* The vendor return value is UNRELIABLE (nonzero yet a valid native
       * window) -- gpSP/pcsx validate the OUTPUT (native_window), not the ret. */
      int dcw_ret;

      /* Reverse engineering the exact MU1316 libdisplayinit shows that the
       * legacy seven-argument wrapper hard-codes two buffers, then delegates to
       * this eight-argument implementation. Ask the firmware to allocate three
       * from the outset, before EGL references the window. This is safer than
       * destroying buffers behind eglsub-screen and lets core work overlap the
       * compositor's scanout instead of extending every nominal 16.7 ms frame. */
      if (display_create_window_nbuffers)
         dcw_ret = display_create_window_nbuffers(qnx->egl.dpy,
               qnx->egl.config, (int)qnx->width, (int)qnx->height,
               qnx->displayable_id, requested_buffers,
               &qnx->native_window, &qnx->kd_window);
      else
         dcw_ret = display_create_window(qnx->egl.dpy, qnx->egl.config,
               (int)qnx->width, (int)qnx->height, qnx->displayable_id,
               &qnx->native_window, &qnx->kd_window);
      ra_dbg("display_create_window%s buffers=%d ret=%d native_window=%p kd_window=%d",
            display_create_window_nbuffers ? "_nbuffers" : "",
            display_create_window_nbuffers ? requested_buffers : 2,
            dcw_ret, (void*)qnx->native_window, qnx->kd_window);
      if (qnx->native_window == 0)
      {
         RARCH_ERR("[QNX]: display_create_window produced no native window "
               "(displayable=%d).\n", qnx->displayable_id);
         goto error;
      }
      if (dcw_ret != 0)
         RARCH_WARN("[QNX]: ignoring display_create_window ret=%d; window is valid.\n",
               dcw_ret);
   }

   /* gpSP's working order is strict: declare/route the displayable after its
    * native window exists, but before EGL creates a surface/context for that
    * window. Creating the surface while the OEM context is still active makes
    * this Adreno stack throttle the first GL submission of every frame to the
    * old compositor cadence (~30 Hz), even with eglSwapInterval(0). */
   if (!qnx_route_context(qnx))
      goto error;

   qnx_init_screen_vsync(qnx);
   if (!qnx_configure_screen_buffers(qnx, requested_buffers))
      goto error;
   qnx_set_native_swap_interval(qnx, 0);

   if (!egl_create_context(&qnx->egl, context_attributes))
   {
      ra_dbg("FAIL egl_create_context");
      goto egl_error;
   }
   ra_dbg("ok egl_create_context");

   if (!egl_create_surface(&qnx->egl, (void*)qnx->native_window))
   {
      ra_dbg("FAIL egl_create_surface");
      goto egl_error;
   }
   ra_dbg("ok egl_create_surface");
#endif

   ra_dbg("=== EGL context up: %ux%u displayable=%d -- SUCCESS ===",
         qnx->width, qnx->height, qnx->displayable_id);
   RARCH_LOG("[QNX]: EGL context up: %ux%u, displayable=%d.\n",
         qnx->width, qnx->height, qnx->displayable_id);
   return qnx;

#ifdef HAVE_EGL
egl_error:
   ra_dbg("=== egl_error: giving up display init ===");
   egl_report_error();
#endif
error:
   ra_dbg("=== gfx_ctx_qnx_init FAILED -> returning NULL (video driver has no context) ===");
   gfx_ctx_qnx_destroy(qnx);
   return NULL;
}

static void gfx_ctx_qnx_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   if (!qnx)
      return;
#ifdef HAVE_EGL
   egl_get_video_size(&qnx->egl, width, height);
#endif
   if (!*width || !*height)
   {
      *width  = qnx->width;
      *height = qnx->height;
   }
}

static void gfx_ctx_qnx_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   static int applied_paused = 0;
   unsigned new_width = 0, new_height = 0;
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   int want_paused     = (int)qnx_lifecycle_paused;

   /* SIGTERM (BACK / ignition / timeout) -> RetroArch's normal quit path,
    * which flushes SRAM and unloads the core. Same idiom as the drm/mali/
    * vivante context drivers. */
   *quit = (bool)frontend_driver_get_signal_handler_state();

   /* Reconcile to the *desired* pause state rather than counting signal edges
    * (signals coalesce). Runs on the main thread, so command_event is safe. */
   if (want_paused != applied_paused)
   {
      command_event(want_paused ? CMD_EVENT_PAUSE : CMD_EVENT_UNPAUSE, NULL);
      applied_paused = want_paused;
      RARCH_LOG("[QNX]: lifecycle -> %s\n", want_paused ? "PAUSED" : "RUNNING");

      /* On pause (park-assist / rear-cam / call / nav stole the display), the
       * frame is now frozen -- immediately persist game memory so a later
       * power-off, timeout, or SIGKILL loses nothing: SRAM (in-game battery
       * saves) + a full save-state (resume exactly where we froze). Cheap
       * memory->file writes, no race (loop is halted). Resume stays live -- the
       * state is insurance, not auto-loaded, unless savestate_auto_load is set
       * for a seamless restore after an unclean shutdown. */
      if (want_paused)
      {
         command_event(CMD_EVENT_SAVE_FILES, NULL);
         command_event(CMD_EVENT_SAVE_STATE, NULL);
         RARCH_LOG("[QNX]: paused -> saved SRAM + state (safe to power-off/kill)\n");
      }
   }
   /* Audio focus is NOT lifecycle: losing it does not mean the user left, so it
    * must not drive the desired-state reconcile above. It gets one edge-
    * triggered pause and nothing more -- deliberately no self-unpause. The user
    * can carry on (silently) or leave; audio comes back on its own once the
    * source that took it releases it. With gfx widgets enabled the pause shows
    * as a standing indicator, so the silence is explained rather than looking
    * like a fault. */
   if (__sync_lock_test_and_set(&qnx_audio_focus_command_edge, 0))
   {
      int audio_desired = qnx_read_audio_focus_desired();
      if (audio_desired == 0)
      {
         command_event(CMD_EVENT_PAUSE, NULL);
         audio_driver_stop();
         RARCH_LOG("[QNX]: stock HMI audio focus taken -> core paused, QSA stopped\n");
      }
      else if (audio_desired == 1)
      {
         audio_driver_start(false);
         RARCH_LOG("[QNX]: stock HMI audio focus restored -> QSA restarted "
               "(core remains paused)\n");
      }
      else
         RARCH_WARN("[QNX]: ignored audio-focus signal without desired state\n");
   }

#ifdef HAVE_EGL
   egl_get_video_size(&qnx->egl, &new_width, &new_height);
#endif
   if (!new_width || !new_height)
   {
      new_width  = qnx->width;
      new_height = qnx->height;
   }

   if (new_width != *width || new_height != *height)
   {
      *width  = new_width;
      *height = new_height;
      *resize = true;
   }
}

static bool gfx_ctx_qnx_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen) { return true; }

static void gfx_ctx_qnx_input_driver(void *data,
      const char *joypad_name,
      input_driver_t **input, void **input_data)
{
   void *qnxinput = input_driver_init_wrap(&input_qnx, joypad_name);
   *input         = qnxinput ? &input_qnx : NULL;
   *input_data    = qnxinput;
}

static enum gfx_ctx_api gfx_ctx_qnx_get_api(void *data) { return GFX_CTX_OPENGL_ES_API; }

static bool gfx_ctx_qnx_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   return (api == GFX_CTX_OPENGL_ES_API);
}

static bool gfx_ctx_qnx_has_focus(void *data) { return true; }

static bool gfx_ctx_qnx_suppress_screensaver(void *data, bool enable) { return false; }

static bool gfx_ctx_qnx__get_metrics(void *data,
    enum display_metric_types type, float *value)
{
   switch (type)
   {
      case DISPLAY_METRIC_MM_WIDTH:
      case DISPLAY_METRIC_MM_HEIGHT:
         return false;
      case DISPLAY_METRIC_DPI:
         *value = 160.0f; /* stable Ozone scaling for the 1024x480 HU layer */
         break;
      case DISPLAY_METRIC_NONE:
      default:
         *value = 0;
         return false;
   }
   return true;
}

static void gfx_ctx_qnx_set_swap_interval(void *data, int swap_interval)
{
#ifdef HAVE_EGL
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   /* Adreno EGL interval 1 blocks forever on this display-manager layer after
    * the first post. Always keep EGL asynchronous; when RetroArch requests
    * vsync, gfx_ctx_qnx_swap_buffers() performs a bounded physical Screen
    * wait before the interval-0 post instead. */
   qnx->vsync_requested             = swap_interval > 0;
   qnx->screen_vsync_failed         = false;
   qnx->software_vsync_deadline_ns  = 0;
   egl_set_swap_interval(&qnx->egl, 0);
   /* This firmware's graphics.conf may initialize the native window to
    * interval 1. The Adreno EGL implementation accepts eglSwapInterval(0),
    * but does not reliably propagate it to libscreen for a window created by
    * libdisplayinit. In that state every eglSwapBuffers() still blocks for a
    * full display period, and the core work performed before the post reduces
    * a nominal 60 Hz core to ~54 FPS. Set the underlying property explicitly;
    * requested vsync is implemented by our bounded screen_wait_vsync() path. */
   qnx_set_native_swap_interval(qnx, 0);
   ra_dbg("swap interval requested=%d, EGL=0, Screen=0, Screen vsync=%s",
         swap_interval, qnx->vsync_requested ?
         (qnx->screen_vsync_capable ? "enabled" : "software fallback") :
         "disabled");
#endif
}

static void gfx_ctx_qnx_swap_buffers(void *data)
{
#ifdef HAVE_EGL
   static unsigned trace_swaps;
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   /* Paused (HMI switched context away): don't present at all, so the layer
    * costs 0 GPU while it is not visible. The compositor keeps showing the
    * last posted buffer; nothing else has to change. */
   if (qnx_lifecycle_paused)
      return;
   if (trace_swaps < 4)
      ra_dbg("swap %u begin", trace_swaps);

   if (qnx->vsync_requested)
   {
      if (!qnx_wait_screen_vsync(qnx))
         qnx_wait_software_vsync(qnx);
      else
         qnx->software_vsync_deadline_ns = 0;
   }

   egl_swap_buffers(&qnx->egl);
   if (trace_swaps < 4)
      ra_dbg("swap %u end", trace_swaps);
   trace_swaps++;
#endif
}

static void gfx_ctx_qnx_bind_hw_render(void *data, bool enable)
{
#ifdef HAVE_EGL
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   egl_bind_hw_render(&qnx->egl, enable);
#endif
}

static uint32_t gfx_ctx_qnx_get_flags(void *data)
{
   uint32_t flags = 0;
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);
   return flags;
}

static void gfx_ctx_qnx_set_flags(void *data, uint32_t flags) { }

static bool gfx_ctx_qnx_create_surface(void *data)
{
#ifdef HAVE_EGL
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   return egl_create_surface(&qnx->egl, (void*)qnx->native_window);
#else
   return false;
#endif
}

static bool gfx_ctx_qnx_destroy_surface(void *data)
{
#ifdef HAVE_EGL
   qnx_ctx_data_t *qnx = (qnx_ctx_data_t*)data;
   return egl_destroy_surface(&qnx->egl);
#else
   return false;
#endif
}

const gfx_ctx_driver_t gfx_ctx_qnx = {
   gfx_ctx_qnx_init,
   gfx_ctx_qnx_destroy,
   gfx_ctx_qnx_get_api,
   gfx_ctx_qnx_bind_api,
   gfx_ctx_qnx_set_swap_interval,
   gfx_ctx_qnx_set_video_mode,
   gfx_ctx_qnx_get_video_size,
   NULL, /* get_refresh_rate */
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   gfx_ctx_qnx__get_metrics,
   NULL,
   NULL, /* update_title */
   gfx_ctx_qnx_check_window,
   NULL, /* set_resize */
   gfx_ctx_qnx_has_focus,
   gfx_ctx_qnx_suppress_screensaver,
   false, /* has_windowed */
   gfx_ctx_qnx_swap_buffers,
   gfx_ctx_qnx_input_driver,
#ifdef HAVE_EGL
   egl_get_proc_address,
#else
   NULL,
#endif
   NULL,
   NULL,
   NULL,
   "egl_qnx",
   gfx_ctx_qnx_get_flags,
   gfx_ctx_qnx_set_flags,
   gfx_ctx_qnx_bind_hw_render,
   NULL,
   NULL,
   gfx_ctx_qnx_create_surface,
   gfx_ctx_qnx_destroy_surface
};
