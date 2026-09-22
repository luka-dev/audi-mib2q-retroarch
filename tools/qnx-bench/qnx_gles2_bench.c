/* qnx_gles2_bench -- standalone EGL/GLES2 microbenchmark for Audi MHI2Q.
 *
 * This intentionally follows RetroArch's QNX display path:
 *
 *   eglGetDisplay/eglInitialize/eglChooseConfig
 *   libdisplayinit.so:display_init/display_create_window_nbuffers
 *   QNX Screen native window, native swap interval 0
 *   EGL ES2 context + window surface
 *
 * Most scenarios render into a 480x272 FBO.  For every sample we measure:
 *
 *   call_ms   - time spent issuing GLES calls (including driver blocking)
 *   finish_ms - additional time spent in glFinish()
 *   total_ms  - call_ms + finish_ms
 *
 * A large call_ms with a small finish_ms points at synchronous driver work or
 * GPU backpressure inside the GLES calls.  A small call_ms with a large
 * finish_ms points at asynchronously queued GPU work.  This is still a timing
 * experiment, not a hardware-counter profiler, but it separates the two cases
 * much better than timing retro_run() as a whole.
 */

#define _POSIX_C_SOURCE 200112L

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define BENCH_VERSION "2"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef GL_BINNING_CONTROL_HINT_QCOM
#define GL_BINNING_CONTROL_HINT_QCOM       0x8FB0
#define GL_CPU_OPTIMIZED_QCOM              0x8FB1
#define GL_GPU_OPTIMIZED_QCOM              0x8FB2
#define GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM 0x8FB3
#endif
#ifndef GL_WRITEONLY_RENDERING_QCOM
#define GL_WRITEONLY_RENDERING_QCOM        0x8823
#endif

typedef void (*get_driver_controls_qcom_fn)(GLint *, GLsizei, GLuint *);
typedef void (*get_driver_control_string_qcom_fn)(GLuint, GLsizei,
      GLsizei *, char *);
typedef void (*enable_driver_control_qcom_fn)(GLuint);
typedef void (*discard_framebuffer_ext_fn)(GLenum, GLsizei, const GLenum *);

typedef void (*display_init_fn)(int, int);
typedef int (*display_create_window_fn)(EGLDisplay, EGLConfig, int, int, int,
      EGLNativeWindowType *, int *);
typedef int (*display_create_window_nbuffers_fn)(EGLDisplay, EGLConfig,
      int, int, int, int, EGLNativeWindowType *, int *);
typedef int (*display_get_resolution_fn)(int *, int *);

typedef void *screen_window_t;
typedef int (*screen_get_window_property_iv_fn)(screen_window_t, int, int *);
typedef int (*screen_set_window_property_iv_fn)(screen_window_t, int,
      const int *);

enum
{
   SCREEN_PROPERTY_BUFFER_COUNT        = 4,
   SCREEN_PROPERTY_SWAP_INTERVAL       = 45,
   SCREEN_PROPERTY_RENDER_BUFFER_COUNT = 53
};

struct options
{
   unsigned surface_w;
   unsigned surface_h;
   unsigned render_w;
   unsigned render_h;
   unsigned frames;
   unsigned warmup;
   int buffers;
   int displayable;
   int context;
   int display;
   int cpu;
   bool route;
   bool leave_routed;
   bool list_driver_controls;
   bool writeonly;
   const char *scenario;
   const char *csv_path;
   const char *driver_control;
   const char *binning;
   const char *fbo_format;
};

struct platform
{
   EGLDisplay egl_display;
   EGLConfig egl_config;
   EGLContext egl_context;
   EGLSurface egl_surface;
   EGLNativeWindowType native_window;
   void *display_lib;
   void *screen_lib;
   int kd_window;
   int actual_buffers;
   bool routed;
};

struct renderer
{
   GLuint program;
   GLuint batch_program;
   GLuint vbo;
   GLuint batch_vbo;
   GLuint textures[2];
   GLuint fbo;
   GLuint fbo_texture;
   GLint u_xform;
   GLint u_p1;
   GLint u_p2;
   GLint u_p3;
   GLint u_sampler;
   GLint batch_u_data;
   GLint batch_u_sampler;
   unsigned render_w;
   unsigned render_h;
   unsigned surface_w;
   unsigned surface_h;
   unsigned char *upload_pixels;
   unsigned char *readback_pixels;
};

enum scenario_kind
{
   SC_CLEAR,
   SC_DRAW,
   SC_DRAW_BATCHED,
   SC_UNIFORM,
   SC_UNIFORM_CACHED,
   SC_UNIFORM_VEC4FV,
   SC_TEXTURE,
   SC_TEXTURE_CACHED,
   SC_STATE,
   SC_STATE_CACHED,
   SC_PROGRAM,
   SC_PPSSPP_LIKE,
   SC_PPSSPP_OPTIMIZED,
   SC_FILL,
   SC_FILL_DISCARD,
   SC_UPLOAD,
   SC_READBACK,
   SC_SWAP
};

struct scenario
{
   const char *name;
   enum scenario_kind kind;
   unsigned count;
   const char *unit;
};

static const struct scenario scenarios[] = {
   { "clear",           SC_CLEAR,          1, "pixels" },
   { "draw_100",        SC_DRAW,         100, "draws" },
   { "draw_500",        SC_DRAW,         500, "draws" },
   { "draw_1000",       SC_DRAW,        1000, "draws" },
   { "draw_batched_10", SC_DRAW_BATCHED,  10, "logical_draws" },
   { "draw_batched_1",  SC_DRAW_BATCHED,   1, "logical_draws" },
   { "uniform_1000x4",  SC_UNIFORM,     1000, "draws" },
   { "uniform_cached8", SC_UNIFORM_CACHED,1000, "draws" },
   { "uniform_vec4fv",  SC_UNIFORM_VEC4FV,1000, "draws" },
   { "texture_1000",    SC_TEXTURE,     1000, "draws" },
   { "texture_cached8", SC_TEXTURE_CACHED,1000, "draws" },
   { "state_1000",      SC_STATE,       1000, "draws" },
   { "state_cached8",   SC_STATE_CACHED,1000, "draws" },
   { "program_1000",    SC_PROGRAM,     1000, "draws" },
   { "ppsspp_like",     SC_PPSSPP_LIKE, 1000, "draws" },
   { "ppsspp_optimized",SC_PPSSPP_OPTIMIZED,1000, "draws" },
   { "fill_1x",         SC_FILL,           1, "pixels" },
   { "fill_8x",         SC_FILL,           8, "pixels" },
   { "fill_32x",        SC_FILL,          32, "pixels" },
   { "fill_32x_discard",SC_FILL_DISCARD,  32, "pixels" },
   { "upload_4mb",      SC_UPLOAD,       256, "bytes" },
   { "readback",        SC_READBACK,       1, "bytes" },
   { "swap",            SC_SWAP,           1, "frames" },
};

static volatile sig_atomic_t interrupted;
static uint64_t timer_cycles_per_second;
static uint64_t timer_reported_cycles_per_second;
static unsigned timer_tick_shift;
static discard_framebuffer_ext_fn discard_framebuffer_ext;

static void signal_handler(int sig)
{
   (void)sig;
   interrupted = 1;
}

static uint64_t timer_ticks(void)
{
   if (timer_cycles_per_second)
      return (uint64_t)ClockCycles() >> timer_tick_shift;
   else
   {
      struct timespec ts;
      if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
         return 0;
      return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
            (uint64_t)ts.tv_nsec;
   }
}

static uint64_t reference_monotonic_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
         (uint64_t)ts.tv_nsec;
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
   uint64_t whole;
   uint64_t remainder;
   if (!timer_cycles_per_second)
      return ticks;
   whole = ticks / timer_cycles_per_second;
   remainder = ticks % timer_cycles_per_second;
   return whole * UINT64_C(1000000000) +
         remainder * UINT64_C(1000000000) / timer_cycles_per_second;
}

static void timer_init(void)
{
   const struct qtime_entry *qtime = SYSPAGE_ENTRY(qtime);
   struct timespec delay;
   uint64_t cycles_start;
   uint64_t cycles_end;
   uint64_t ns_start;
   uint64_t ns_end;
   double calibrated;
   uint64_t effective_reported;

   if (qtime)
      timer_reported_cycles_per_second = qtime->cycles_per_sec;

   /* MU1316 represents the emulated ARM ClockCycles counter as Q32: the raw
    * syspage value is 0x0066ff3000000000 and every observed counter delta has
    * its low 32 bits clear. Normalize it back to its 6.75 MHz timebase. */
   effective_reported = timer_reported_cycles_per_second;
   if (effective_reported > UINT64_C(1000000000000) &&
       (effective_reported & UINT64_C(0xffffffff)) == 0 &&
       (effective_reported >> 32) >= 1000 &&
       (effective_reported >> 32) <= UINT64_C(1000000000000))
   {
      timer_tick_shift = 32;
      effective_reported >>= 32;
   }

   /* This firmware exposes a fixed-point-looking cycles_per_sec value in the
    * public syspage (pidin prints 0x0066ff3000000000), while ClockCycles()
    * returns the underlying counter directly. Calibrate that counter once
    * against CLOCK_MONOTONIC over a long interval. The reference clock has a
    * coarse ~1 ms quantum, but over 250 ms its error is below one percent. */
   delay.tv_sec = 0;
   delay.tv_nsec = 250000000L;
   ns_start = reference_monotonic_ns();
   cycles_start = (uint64_t)ClockCycles();
   while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
   cycles_end = (uint64_t)ClockCycles();
   ns_end = reference_monotonic_ns();
   if (cycles_end > cycles_start && ns_end > ns_start)
   {
      calibrated = (double)((cycles_end - cycles_start) >> timer_tick_shift) *
            1e9 /
            (double)(ns_end - ns_start);
      fprintf(stderr,
         "qnx_gles2_bench: timer calibration cycle_delta=%llu ns_delta=%llu "
         "estimate=%.0f\n",
         (unsigned long long)(cycles_end - cycles_start),
         (unsigned long long)(ns_end - ns_start), calibrated);
      if (calibrated >= 1000.0 && calibrated <= 1e12)
      {
         /* Prefer the firmware's exact declared rate once its Q32 encoding is
          * normalized. Calibration is only a sanity check against it. */
         timer_cycles_per_second = effective_reported ? effective_reported :
               (uint64_t)(calibrated + 0.5);
      }
   }
   else
      fprintf(stderr,
         "qnx_gles2_bench: timer calibration invalid start=%llu end=%llu "
         "ns_start=%llu ns_end=%llu\n",
         (unsigned long long)cycles_start,
         (unsigned long long)cycles_end,
         (unsigned long long)ns_start, (unsigned long long)ns_end);

   if (timer_cycles_per_second)
      fprintf(stderr,
         "qnx_gles2_bench: timer ClockCycles, frequency=%llu "
         "syspage_raw=%llu shift=%u effective=%llu cycles/second\n",
         (unsigned long long)timer_cycles_per_second,
         (unsigned long long)timer_reported_cycles_per_second,
         timer_tick_shift,
         (unsigned long long)effective_reported);
   else
      fprintf(stderr, "qnx_gles2_bench: timer CLOCK_MONOTONIC fallback\n");
}

static uint64_t elapsed_ns(uint64_t start, uint64_t end)
{
   if (end < start)
      return 0;
   return ticks_to_ns(end - start);
}

static const char *safe_gl_string(GLenum name)
{
   const GLubyte *s = glGetString(name);
   return s ? (const char *)s : "(null)";
}

static const char *safe_egl_string(EGLDisplay display, EGLint name)
{
   const char *s = eglQueryString(display, name);
   return s ? s : "(null)";
}

static bool extension_present(const char *extensions, const char *name)
{
   size_t n;
   const char *p;
   if (!extensions || !name || !*name || strchr(name, ' '))
      return false;
   n = strlen(name);
   p = extensions;
   while ((p = strstr(p, name)) != NULL)
   {
      if ((p == extensions || p[-1] == ' ') &&
          (p[n] == '\0' || p[n] == ' '))
         return true;
      p += n;
   }
   return false;
}

static bool configure_driver(const struct options *o)
{
   const char *extensions = safe_gl_string(GL_EXTENSIONS);
   bool need_controls = o->driver_control || o->list_driver_controls;

   discard_framebuffer_ext = NULL;
   if (extension_present(extensions, "GL_EXT_discard_framebuffer"))
      discard_framebuffer_ext = (discard_framebuffer_ext_fn)
            eglGetProcAddress("glDiscardFramebufferEXT");

   if (need_controls)
   {
      get_driver_controls_qcom_fn get_controls;
      get_driver_control_string_qcom_fn get_string;
      enable_driver_control_qcom_fn enable_control;
      GLuint *ids = NULL;
      GLint count = 0;
      GLint i;
      bool found = false;

      if (!extension_present(extensions, "GL_QCOM_driver_control"))
      {
         fprintf(stderr,
            "qnx_gles2_bench: GL_QCOM_driver_control is unavailable\n");
         return false;
      }
      get_controls = (get_driver_controls_qcom_fn)
            eglGetProcAddress("glGetDriverControlsQCOM");
      get_string = (get_driver_control_string_qcom_fn)
            eglGetProcAddress("glGetDriverControlStringQCOM");
      enable_control = (enable_driver_control_qcom_fn)
            eglGetProcAddress("glEnableDriverControlQCOM");
      if (!get_controls || !get_string || !enable_control)
      {
         fprintf(stderr,
            "qnx_gles2_bench: driver-control entry points are missing\n");
         return false;
      }

      while (glGetError() != GL_NO_ERROR) { }
      get_controls(&count, 0, NULL);
      if (glGetError() != GL_NO_ERROR || count <= 0 || count > 256)
      {
         fprintf(stderr,
            "qnx_gles2_bench: invalid driver-control count %d\n",
            (int)count);
         return false;
      }
      ids = (GLuint *)calloc((size_t)count, sizeof(*ids));
      if (!ids)
         return false;
      get_controls(&count, count, ids);
      for (i = 0; i < count; i++)
      {
         char description[768];
         GLsizei length = 0;
         size_t requested_len = o->driver_control ?
               strlen(o->driver_control) : 0;
         description[0] = '\0';
         description[sizeof(description) - 1] = '\0';
         get_string(ids[i], (GLsizei)sizeof(description) - 1, &length,
               description);
         if (length >= 0 && length < (GLsizei)sizeof(description))
            description[length] = '\0';
         fprintf(stderr, "qnx_gles2_bench: driver control %u: %s\n",
               (unsigned)ids[i], description);
         if (o->driver_control &&
             !strncmp(description, o->driver_control, requested_len) &&
             (description[requested_len] == '\0' ||
              description[requested_len] == ' '))
         {
            enable_control(ids[i]);
            if (glGetError() != GL_NO_ERROR)
            {
               fprintf(stderr,
                  "qnx_gles2_bench: enabling driver control %s failed\n",
                  o->driver_control);
               free(ids);
               return false;
            }
            fprintf(stderr,
               "qnx_gles2_bench: enabled driver control %s (id=%u)\n",
               o->driver_control, (unsigned)ids[i]);
            found = true;
         }
      }
      free(ids);
      if (o->driver_control && !found)
      {
         fprintf(stderr, "qnx_gles2_bench: driver control %s not found\n",
               o->driver_control);
         return false;
      }
   }

   if (strcmp(o->binning, "default"))
   {
      GLenum mode = !strcmp(o->binning, "cpu") ? GL_CPU_OPTIMIZED_QCOM :
            !strcmp(o->binning, "gpu") ? GL_GPU_OPTIMIZED_QCOM :
            GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM;
      if (!extension_present(extensions, "GL_QCOM_binning_control"))
      {
         fprintf(stderr,
            "qnx_gles2_bench: GL_QCOM_binning_control is unavailable\n");
         return false;
      }
      while (glGetError() != GL_NO_ERROR) { }
      glHint(GL_BINNING_CONTROL_HINT_QCOM, mode);
      if (glGetError() != GL_NO_ERROR)
      {
         fprintf(stderr, "qnx_gles2_bench: binning hint %s failed\n",
               o->binning);
         return false;
      }
      fprintf(stderr, "qnx_gles2_bench: binning hint=%s\n", o->binning);
   }

   if (o->writeonly)
   {
      if (!extension_present(extensions, "GL_QCOM_writeonly_rendering"))
      {
         fprintf(stderr,
            "qnx_gles2_bench: GL_QCOM_writeonly_rendering is unavailable\n");
         return false;
      }
      while (glGetError() != GL_NO_ERROR) { }
      glEnable(GL_WRITEONLY_RENDERING_QCOM);
      if (glGetError() != GL_NO_ERROR)
      {
         fprintf(stderr, "qnx_gles2_bench: write-only hint failed\n");
         return false;
      }
      fprintf(stderr, "qnx_gles2_bench: write-only rendering enabled\n");
   }
   return true;
}

static void usage(const char *argv0)
{
   size_t i;
   fprintf(stderr,
      "usage: %s [options]\n"
      "\n"
      "  --scenario NAME     one scenario or all (default all)\n"
      "  --frames N          measured samples per scenario (default 30)\n"
      "  --warmup N          warmup samples per scenario (default 10)\n"
      "  --render WxH        offscreen render size (default 480x272)\n"
      "  --surface WxH       QNX Screen surface size (default 1024x480)\n"
      "  --buffers N         native Screen buffers, 2..3 (default 3)\n"
      "  --displayable N     displayable id (default 43, same as RA)\n"
      "  --context N         routed context id (default 90, same as RA)\n"
      "  --display N         physical display id (default 0)\n"
      "  --cpu N             optionally pin submit thread to CPU N\n"
      "  --no-affinity       leave it schedulable on every CPU (default)\n"
      "  --fbo-format NAME   rgba8, rgb565 or rgba4 (default rgba8)\n"
      "  --binning NAME      default, cpu, gpu or direct (default default)\n"
      "  --writeonly         enable GL_QCOM_writeonly_rendering hint\n"
      "  --driver-control N  enable a GL_QCOM_driver_control by name\n"
      "  --list-driver-controls  enumerate driver controls and exit\n"
      "  --route             route context while testing; restored with dmdt sb\n"
      "  --leave-routed      let a supervising wrapper restore the context\n"
      "  --csv FILE          write CSV to FILE instead of stdout\n"
      "  --list              list scenario names\n"
      "  -h, --help          show this help\n"
      "\n"
      "Scenarios:\n", argv0);
   for (i = 0; i < ARRAY_SIZE(scenarios); i++)
      fprintf(stderr, "  %s\n", scenarios[i].name);
}

static bool parse_u32(const char *text, unsigned min_value,
      unsigned max_value, unsigned *out)
{
   char *end = NULL;
   unsigned long value;
   errno = 0;
   value = strtoul(text, &end, 10);
   if (errno || end == text || *end || value < min_value || value > max_value)
      return false;
   *out = (unsigned)value;
   return true;
}

static bool parse_i32(const char *text, int min_value, int max_value, int *out)
{
   char *end = NULL;
   long value;
   errno = 0;
   value = strtol(text, &end, 10);
   if (errno || end == text || *end || value < min_value || value > max_value)
      return false;
   *out = (int)value;
   return true;
}

static bool parse_size(const char *text, unsigned *w, unsigned *h)
{
   const char *x = strchr(text, 'x');
   char left[32];
   size_t n;
   if (!x)
      x = strchr(text, 'X');
   if (!x || x == text || !x[1])
      return false;
   n = (size_t)(x - text);
   if (n >= sizeof(left))
      return false;
   memcpy(left, text, n);
   left[n] = '\0';
   return parse_u32(left, 16, 4096, w) && parse_u32(x + 1, 16, 4096, h);
}

static bool scenario_exists(const char *name)
{
   size_t i;
   if (!strcmp(name, "all"))
      return true;
   for (i = 0; i < ARRAY_SIZE(scenarios); i++)
      if (!strcmp(name, scenarios[i].name))
         return true;
   return false;
}

static int parse_options(int argc, char **argv, struct options *o)
{
   int i;
   memset(o, 0, sizeof(*o));
   o->surface_w  = 1024;
   o->surface_h  = 480;
   o->render_w   = 480;
   o->render_h   = 272;
   o->frames     = 30;
   o->warmup     = 10;
   o->buffers    = 3;
   o->displayable = 43;
   o->context    = 90;
   o->display    = 0;
   o->cpu        = -1;
   o->scenario   = "all";
   o->binning    = "default";
   o->fbo_format = "rgba8";

   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i];
      const char *next = i + 1 < argc ? argv[i + 1] : NULL;
      unsigned temp_u;
      int temp_i;

      if (!strcmp(a, "-h") || !strcmp(a, "--help"))
      {
         usage(argv[0]);
         return 1;
      }
      if (!strcmp(a, "--list"))
      {
         size_t j;
         for (j = 0; j < ARRAY_SIZE(scenarios); j++)
            puts(scenarios[j].name);
         return 1;
      }
      if (!strcmp(a, "--route"))
      {
         o->route = true;
         continue;
      }
      if (!strcmp(a, "--leave-routed"))
      {
         o->leave_routed = true;
         continue;
      }
      if (!strcmp(a, "--list-driver-controls"))
      {
         o->list_driver_controls = true;
         continue;
      }
      if (!strcmp(a, "--writeonly"))
      {
         o->writeonly = true;
         continue;
      }
      if (!strcmp(a, "--no-affinity"))
      {
         o->cpu = -1;
         continue;
      }
      if (!strcmp(a, "--scenario") && next)
      {
         o->scenario = argv[++i];
         continue;
      }
      if (!strcmp(a, "--csv") && next)
      {
         o->csv_path = argv[++i];
         continue;
      }
      if (!strcmp(a, "--driver-control") && next)
      {
         o->driver_control = argv[++i];
         continue;
      }
      if (!strcmp(a, "--binning") && next)
      {
         o->binning = argv[++i];
         if (strcmp(o->binning, "default") && strcmp(o->binning, "cpu") &&
             strcmp(o->binning, "gpu") && strcmp(o->binning, "direct"))
            goto invalid;
         continue;
      }
      if (!strcmp(a, "--fbo-format") && next)
      {
         o->fbo_format = argv[++i];
         if (strcmp(o->fbo_format, "rgba8") &&
             strcmp(o->fbo_format, "rgb565") &&
             strcmp(o->fbo_format, "rgba4"))
            goto invalid;
         continue;
      }
      if (!strcmp(a, "--render") && next)
      {
         if (!parse_size(argv[++i], &o->render_w, &o->render_h))
            goto invalid;
         continue;
      }
      if (!strcmp(a, "--surface") && next)
      {
         if (!parse_size(argv[++i], &o->surface_w, &o->surface_h))
            goto invalid;
         continue;
      }
      if (!strcmp(a, "--frames") && next)
      {
         if (!parse_u32(argv[++i], 1, 10000, &temp_u))
            goto invalid;
         o->frames = temp_u;
         continue;
      }
      if (!strcmp(a, "--warmup") && next)
      {
         if (!parse_u32(argv[++i], 0, 1000, &temp_u))
            goto invalid;
         o->warmup = temp_u;
         continue;
      }
      if (!strcmp(a, "--buffers") && next)
      {
         if (!parse_i32(argv[++i], 2, 3, &temp_i))
            goto invalid;
         o->buffers = temp_i;
         continue;
      }
      if (!strcmp(a, "--displayable") && next)
      {
         if (!parse_i32(argv[++i], 0, 4096, &temp_i))
            goto invalid;
         o->displayable = temp_i;
         continue;
      }
      if (!strcmp(a, "--context") && next)
      {
         if (!parse_i32(argv[++i], 0, 4096, &temp_i))
            goto invalid;
         o->context = temp_i;
         continue;
      }
      if (!strcmp(a, "--display") && next)
      {
         if (!parse_i32(argv[++i], 0, 31, &temp_i))
            goto invalid;
         o->display = temp_i;
         continue;
      }
      if (!strcmp(a, "--cpu") && next)
      {
         if (!parse_i32(argv[++i], 0, 31, &temp_i))
            goto invalid;
         o->cpu = temp_i;
         continue;
      }

invalid:
      fprintf(stderr, "qnx_gles2_bench: invalid argument near '%s'\n", a);
      usage(argv[0]);
      return -1;
   }

   if (!scenario_exists(o->scenario))
   {
      fprintf(stderr, "qnx_gles2_bench: unknown scenario '%s'\n", o->scenario);
      return -1;
   }
   if (o->leave_routed && !o->route)
   {
      fprintf(stderr, "qnx_gles2_bench: --leave-routed requires --route\n");
      return -1;
   }
   return 0;
}

static void *dlopen_first(const char *const *paths)
{
   size_t i;
   void *handle = NULL;
   for (i = 0; paths[i] && !handle; i++)
      handle = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
   return handle;
}

static bool dmdt_command(const char *verb, int a, int b, int c,
      bool four_args)
{
   char command[160];
   int n;
   if (four_args)
      n = snprintf(command, sizeof(command),
            "/eso/bin/apps/dmdt %s %d %d %d", verb, a, b, c);
   else if (b >= 0)
      n = snprintf(command, sizeof(command),
            "/eso/bin/apps/dmdt %s %d %d", verb, a, b);
   else
      n = snprintf(command, sizeof(command),
            "/eso/bin/apps/dmdt %s %d", verb, a);
   if (n < 0 || n >= (int)sizeof(command))
      return false;
   fprintf(stderr, "qnx_gles2_bench: %s\n", command);
   return system(command) == 0;
}

static bool route_context(struct platform *p, const struct options *o)
{
   if (!dmdt_command("dc", o->context, 16, o->displayable, true))
      return false;
   if (!dmdt_command("sc", o->display, o->context, -1, false))
      return false;
   p->routed = true;
   return true;
}

static void restore_buffered_context(struct platform *p,
      const struct options *o)
{
   if (!p->routed || o->leave_routed)
      return;
   if (!dmdt_command("sb", o->display, -1, -1, false))
      fprintf(stderr, "qnx_gles2_bench: WARNING: buffered context restore failed\n");
   p->routed = false;
}

static void platform_destroy(struct platform *p, const struct options *o)
{
   if (p->egl_display != EGL_NO_DISPLAY)
   {
      eglMakeCurrent(p->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
      if (p->egl_surface != EGL_NO_SURFACE)
         eglDestroySurface(p->egl_display, p->egl_surface);
      if (p->egl_context != EGL_NO_CONTEXT)
         eglDestroyContext(p->egl_display, p->egl_context);
      eglTerminate(p->egl_display);
   }
   restore_buffered_context(p, o);
   if (p->screen_lib)
      dlclose(p->screen_lib);
   if (p->display_lib)
      dlclose(p->display_lib);
   memset(p, 0, sizeof(*p));
   p->egl_display = EGL_NO_DISPLAY;
   p->egl_context = EGL_NO_CONTEXT;
   p->egl_surface = EGL_NO_SURFACE;
}

static bool platform_init(struct platform *p, const struct options *o)
{
   static const char *const display_paths[] = {
      "/eso/lib/libdisplayinit.so",
      "/mnt/app/eso/lib/libdisplayinit.so",
      "libdisplayinit.so",
      NULL
   };
   static const char *const screen_paths[] = {
      "/proc/boot/libscreen.so.1",
      "libscreen.so.1",
      NULL
   };
   const EGLint config_attribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 1,
      EGL_GREEN_SIZE, 1,
      EGL_BLUE_SIZE, 1,
      EGL_ALPHA_SIZE, 1,
      EGL_NONE
   };
   const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
   };
   display_init_fn display_init;
   display_create_window_fn create_window;
   display_create_window_nbuffers_fn create_window_nbuffers;
   display_get_resolution_fn get_resolution;
   screen_get_window_property_iv_fn screen_get_iv;
   screen_set_window_property_iv_fn screen_set_iv;
   EGLint major = 0, minor = 0, count = 0;
   EGLint config_id = 0, red = 0, green = 0, blue = 0, alpha = 0;
   int native_w = 0, native_h = 0;
   int create_result;
   int zero = 0;

   memset(p, 0, sizeof(*p));
   p->egl_display = EGL_NO_DISPLAY;
   p->egl_context = EGL_NO_CONTEXT;
   p->egl_surface = EGL_NO_SURFACE;
   p->actual_buffers = -1;

   if (!getenv("GRAPHICS_ROOT"))
      setenv("GRAPHICS_ROOT", "/proc/boot/", 0);
   if (!getenv("IPL_CONFIG_DIR"))
      setenv("IPL_CONFIG_DIR", "/etc/eso/production", 0);
   unsetenv("EGL_PLATFORM");

   if (!eglBindAPI(EGL_OPENGL_ES_API))
   {
      fprintf(stderr, "qnx_gles2_bench: eglBindAPI failed: 0x%x\n",
            (unsigned)eglGetError());
      return false;
   }

   /* This ordering is mandatory on the MHI2Q Adreno stack. */
   p->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (p->egl_display == EGL_NO_DISPLAY ||
       !eglInitialize(p->egl_display, &major, &minor) ||
       !eglChooseConfig(p->egl_display, config_attribs, &p->egl_config, 1,
            &count) || count < 1)
   {
      fprintf(stderr, "qnx_gles2_bench: EGL display/config init failed: 0x%x\n",
            (unsigned)eglGetError());
      goto fail;
   }

   eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_CONFIG_ID, &config_id);
   eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_RED_SIZE, &red);
   eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_GREEN_SIZE, &green);
   eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_BLUE_SIZE, &blue);
   eglGetConfigAttrib(p->egl_display, p->egl_config, EGL_ALPHA_SIZE, &alpha);
   fprintf(stderr,
      "qnx_gles2_bench: EGL %d.%d config=%d rgba=%d/%d/%d/%d\n",
      (int)major, (int)minor, (int)config_id, (int)red, (int)green,
      (int)blue, (int)alpha);

   p->display_lib = dlopen_first(display_paths);
   if (!p->display_lib)
   {
      fprintf(stderr, "qnx_gles2_bench: dlopen(libdisplayinit): %s\n",
            dlerror());
      goto fail;
   }
   display_init = (display_init_fn)dlsym(p->display_lib, "display_init");
   create_window = (display_create_window_fn)dlsym(p->display_lib,
         "display_create_window");
   create_window_nbuffers = (display_create_window_nbuffers_fn)dlsym(
         p->display_lib, "display_create_window_nbuffers");
   get_resolution = (display_get_resolution_fn)dlsym(p->display_lib,
         "display_get_resolution");
   if (!display_init || (!create_window && !create_window_nbuffers))
   {
      fprintf(stderr, "qnx_gles2_bench: libdisplayinit symbols missing\n");
      goto fail;
   }

   display_init(0, 0);
   if (get_resolution && get_resolution(&native_w, &native_h) == 0)
      fprintf(stderr, "qnx_gles2_bench: native display %dx%d\n",
            native_w, native_h);

   if (create_window_nbuffers)
      create_result = create_window_nbuffers(p->egl_display, p->egl_config,
            (int)o->surface_w, (int)o->surface_h, o->displayable, o->buffers,
            &p->native_window, &p->kd_window);
   else
      create_result = create_window(p->egl_display, p->egl_config,
            (int)o->surface_w, (int)o->surface_h, o->displayable,
            &p->native_window, &p->kd_window);
   fprintf(stderr,
      "qnx_gles2_bench: create%s requested_buffers=%d ret=%d window=%p kd=%d\n",
      create_window_nbuffers ? "_nbuffers" : "", o->buffers, create_result,
      (void *)p->native_window, p->kd_window);
   if (!p->native_window)
      goto fail;

   if (o->route && !route_context(p, o))
   {
      fprintf(stderr, "qnx_gles2_bench: display routing failed\n");
      goto fail;
   }

   p->screen_lib = dlopen_first(screen_paths);
   if (p->screen_lib)
   {
      screen_get_iv = (screen_get_window_property_iv_fn)dlsym(p->screen_lib,
            "screen_get_window_property_iv");
      screen_set_iv = (screen_set_window_property_iv_fn)dlsym(p->screen_lib,
            "screen_set_window_property_iv");
      if (screen_set_iv && screen_set_iv((screen_window_t)p->native_window,
               SCREEN_PROPERTY_SWAP_INTERVAL, &zero) != 0)
         fprintf(stderr,
            "qnx_gles2_bench: Screen swap interval 0 failed errno=%d\n",
            errno);
      if (screen_get_iv)
      {
         int buffer_count = -1;
         screen_get_iv((screen_window_t)p->native_window,
               SCREEN_PROPERTY_BUFFER_COUNT, &buffer_count);
         screen_get_iv((screen_window_t)p->native_window,
               SCREEN_PROPERTY_RENDER_BUFFER_COUNT, &p->actual_buffers);
         fprintf(stderr,
            "qnx_gles2_bench: Screen buffers=%d render_buffers=%d\n",
            buffer_count, p->actual_buffers);
      }
   }

   p->egl_context = eglCreateContext(p->egl_display, p->egl_config,
         EGL_NO_CONTEXT, context_attribs);
   if (p->egl_context == EGL_NO_CONTEXT)
   {
      fprintf(stderr, "qnx_gles2_bench: eglCreateContext failed: 0x%x\n",
            (unsigned)eglGetError());
      goto fail;
   }
   p->egl_surface = eglCreateWindowSurface(p->egl_display, p->egl_config,
         p->native_window, NULL);
   if (p->egl_surface == EGL_NO_SURFACE ||
       !eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface,
            p->egl_context))
   {
      fprintf(stderr, "qnx_gles2_bench: EGL surface/current failed: 0x%x\n",
            (unsigned)eglGetError());
      goto fail;
   }
   if (!eglSwapInterval(p->egl_display, 0))
      fprintf(stderr, "qnx_gles2_bench: eglSwapInterval(0) failed: 0x%x\n",
            (unsigned)eglGetError());

   fprintf(stderr, "qnx_gles2_bench: EGL vendor: %s\n",
         safe_egl_string(p->egl_display, EGL_VENDOR));
   fprintf(stderr, "qnx_gles2_bench: EGL version: %s\n",
         safe_egl_string(p->egl_display, EGL_VERSION));
   fprintf(stderr, "qnx_gles2_bench: GL vendor: %s\n",
         safe_gl_string(GL_VENDOR));
   fprintf(stderr, "qnx_gles2_bench: GL renderer: %s\n",
         safe_gl_string(GL_RENDERER));
   fprintf(stderr, "qnx_gles2_bench: GL version: %s\n",
         safe_gl_string(GL_VERSION));
   return true;

fail:
   platform_destroy(p, o);
   return false;
}

static GLuint compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   GLint ok = GL_FALSE;
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[2048];
      GLsizei n = 0;
      glGetShaderInfoLog(shader, sizeof(log), &n, log);
      fprintf(stderr, "qnx_gles2_bench: shader compile failed: %.*s\n",
            (int)n, log);
      glDeleteShader(shader);
      return 0;
   }
   return shader;
}

static GLuint create_program(bool batched_uniforms)
{
   static const char *vertex_source_separate =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "uniform vec4 u_xform;\n"
      "uniform vec4 u_p1;\n"
      "uniform vec4 u_p2;\n"
      "uniform vec4 u_p3;\n"
      "varying vec2 v_uv;\n"
      "varying vec4 v_tint;\n"
      "void main() {\n"
      "  float tiny = (u_p1.x + u_p2.y + u_p3.z) * 0.000001;\n"
      "  vec2 p = a_pos * u_xform.xy + u_xform.zw + vec2(tiny);\n"
      "  gl_Position = vec4(p, 0.0, 1.0);\n"
      "  v_uv = a_uv + u_p2.zw * 0.000001;\n"
      "  v_tint = vec4(0.45, 0.55, 0.65, 1.0) +\n"
      "           (u_p1 + u_p2 + u_p3) * 0.000001;\n"
      "}\n";
   static const char *vertex_source_batched =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "uniform vec4 u_data[4];\n"
      "varying vec2 v_uv;\n"
      "varying vec4 v_tint;\n"
      "void main() {\n"
      "  float tiny = (u_data[1].x + u_data[2].y +\n"
      "                u_data[3].z) * 0.000001;\n"
      "  vec2 p = a_pos * u_data[0].xy + u_data[0].zw + vec2(tiny);\n"
      "  gl_Position = vec4(p, 0.0, 1.0);\n"
      "  v_uv = a_uv + u_data[2].zw * 0.000001;\n"
      "  v_tint = vec4(0.45, 0.55, 0.65, 1.0) +\n"
      "           (u_data[1] + u_data[2] + u_data[3]) * 0.000001;\n"
      "}\n";
   static const char *fragment_source =
      "precision mediump float;\n"
      "uniform sampler2D u_tex;\n"
      "varying vec2 v_uv;\n"
      "varying vec4 v_tint;\n"
      "void main() {\n"
      "  vec4 t = texture2D(u_tex, v_uv);\n"
      "  gl_FragColor = vec4(t.rgb * v_tint.rgb + t.bgr * 0.05, 0.55);\n"
      "}\n";
   const char *vertex_source = batched_uniforms ? vertex_source_batched :
         vertex_source_separate;
   GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   GLuint program;
   GLint ok = GL_FALSE;
   if (!vs || !fs)
      return 0;
   program = glCreateProgram();
   glAttachShader(program, vs);
   glAttachShader(program, fs);
   glBindAttribLocation(program, 0, "a_pos");
   glBindAttribLocation(program, 1, "a_uv");
   glLinkProgram(program);
   glDeleteShader(vs);
   glDeleteShader(fs);
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok)
   {
      char log[2048];
      GLsizei n = 0;
      glGetProgramInfoLog(program, sizeof(log), &n, log);
      fprintf(stderr, "qnx_gles2_bench: program link failed: %.*s\n",
            (int)n, log);
      glDeleteProgram(program);
      return 0;
   }
   return program;
}

static void fill_texture(unsigned char *pixels, unsigned seed)
{
   unsigned x, y;
   for (y = 0; y < 64; y++)
   {
      for (x = 0; x < 64; x++)
      {
         size_t i = ((size_t)y * 64 + x) * 4;
         unsigned checker = ((x >> 3) ^ (y >> 3)) & 1;
         pixels[i + 0] = (unsigned char)(checker ? 220 : 30 + seed);
         pixels[i + 1] = (unsigned char)((x * 3 + seed) & 255);
         pixels[i + 2] = (unsigned char)((y * 5 + seed * 2) & 255);
         pixels[i + 3] = 255;
      }
   }
}

static bool renderer_init(struct renderer *r, const struct options *o)
{
   static const GLfloat vertices[] = {
      -1.0f, -1.0f, 0.0f, 0.0f,
       1.0f, -1.0f, 1.0f, 0.0f,
      -1.0f,  1.0f, 0.0f, 1.0f,
       1.0f,  1.0f, 1.0f, 1.0f,
   };
   static const GLfloat corner_x[6] = { -1, 1, -1, -1, 1, 1 };
   static const GLfloat corner_y[6] = { -1, -1, 1, 1, -1, 1 };
   unsigned char pixels[64 * 64 * 4];
   GLfloat *batch_vertices;
   GLenum fbo_format = GL_RGBA;
   GLenum fbo_type = GL_UNSIGNED_BYTE;
   unsigned i, j;

   memset(r, 0, sizeof(*r));
   r->render_w = o->render_w;
   r->render_h = o->render_h;
   r->surface_w = o->surface_w;
   r->surface_h = o->surface_h;
   r->program = create_program(false);
   r->batch_program = create_program(true);
   if (!r->program || !r->batch_program)
      return false;

   glGenBuffers(1, &r->vbo);
   glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

   batch_vertices = (GLfloat *)malloc(1000 * 6 * 4 * sizeof(GLfloat));
   if (!batch_vertices)
      return false;
   for (i = 0; i < 1000; i++)
   {
      GLfloat x = -0.94f + (GLfloat)(i % 20) * 0.099f;
      GLfloat y = -0.90f + (GLfloat)((i / 20) % 10) * 0.195f;
      for (j = 0; j < 6; j++)
      {
         size_t v = ((size_t)i * 6 + j) * 4;
         batch_vertices[v + 0] = x + corner_x[j] * 0.045f;
         batch_vertices[v + 1] = y + corner_y[j] * 0.085f;
         batch_vertices[v + 2] = (corner_x[j] + 1.0f) * 0.5f;
         batch_vertices[v + 3] = (corner_y[j] + 1.0f) * 0.5f;
      }
   }
   glGenBuffers(1, &r->batch_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, r->batch_vbo);
   glBufferData(GL_ARRAY_BUFFER, 1000 * 6 * 4 * sizeof(GLfloat),
         batch_vertices, GL_STATIC_DRAW);
   free(batch_vertices);

   glGenTextures(2, r->textures);
   for (i = 0; i < 2; i++)
   {
      fill_texture(pixels, i * 47);
      glBindTexture(GL_TEXTURE_2D, r->textures[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, pixels);
   }

   glGenTextures(1, &r->fbo_texture);
   glBindTexture(GL_TEXTURE_2D, r->fbo_texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   if (!strcmp(o->fbo_format, "rgb565"))
   {
      fbo_format = GL_RGB;
      fbo_type = GL_UNSIGNED_SHORT_5_6_5;
   }
   else if (!strcmp(o->fbo_format, "rgba4"))
      fbo_type = GL_UNSIGNED_SHORT_4_4_4_4;
   glTexImage2D(GL_TEXTURE_2D, 0, fbo_format, (GLsizei)r->render_w,
         (GLsizei)r->render_h, 0, fbo_format, fbo_type, NULL);
   glGenFramebuffers(1, &r->fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
         GL_TEXTURE_2D, r->fbo_texture, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
   {
      fprintf(stderr, "qnx_gles2_bench: %ux%u FBO is incomplete\n",
            r->render_w, r->render_h);
      return false;
   }

   r->upload_pixels = (unsigned char *)malloc(64 * 64 * 4);
   r->readback_pixels = (unsigned char *)malloc(
         (size_t)r->render_w * r->render_h * 4);
   if (!r->upload_pixels || !r->readback_pixels)
      return false;
   fill_texture(r->upload_pixels, 91);

   glUseProgram(r->program);
   r->u_xform = glGetUniformLocation(r->program, "u_xform");
   r->u_p1 = glGetUniformLocation(r->program, "u_p1");
   r->u_p2 = glGetUniformLocation(r->program, "u_p2");
   r->u_p3 = glGetUniformLocation(r->program, "u_p3");
   r->u_sampler = glGetUniformLocation(r->program, "u_tex");
   if (r->u_xform < 0 || r->u_p1 < 0 || r->u_p2 < 0 ||
       r->u_p3 < 0 || r->u_sampler < 0)
   {
      fprintf(stderr, "qnx_gles2_bench: required shader uniform optimized out\n");
      return false;
   }

   glUseProgram(r->batch_program);
   r->batch_u_data = glGetUniformLocation(r->batch_program, "u_data[0]");
   r->batch_u_sampler = glGetUniformLocation(r->batch_program, "u_tex");
   if (r->batch_u_data < 0 || r->batch_u_sampler < 0)
   {
      fprintf(stderr,
         "qnx_gles2_bench: required batched shader uniform optimized out\n");
      return false;
   }
   glUniform1i(r->batch_u_sampler, 0);

   glUseProgram(r->program);
   glUniform1i(r->u_sampler, 0);
   glUniform4f(r->u_xform, 0.05f, 0.05f, 0.0f, 0.0f);
   glUniform4f(r->u_p1, 0, 0, 0, 0);
   glUniform4f(r->u_p2, 0, 0, 0, 0);
   glUniform4f(r->u_p3, 0, 0, 0, 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, r->textures[0]);
   glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
         (const void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
         (const void *)(2 * sizeof(GLfloat)));
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_BLEND);
   glClearColor(0.03f, 0.04f, 0.06f, 1.0f);
   glFinish();
   if (glGetError() != GL_NO_ERROR)
   {
      fprintf(stderr, "qnx_gles2_bench: GLES setup reported an error\n");
      return false;
   }
   return true;
}

static void renderer_destroy(struct renderer *r)
{
   if (r->fbo)
      glDeleteFramebuffers(1, &r->fbo);
   if (r->fbo_texture)
      glDeleteTextures(1, &r->fbo_texture);
   if (r->textures[0] || r->textures[1])
      glDeleteTextures(2, r->textures);
   if (r->vbo)
      glDeleteBuffers(1, &r->vbo);
   if (r->batch_vbo)
      glDeleteBuffers(1, &r->batch_vbo);
   if (r->program)
      glDeleteProgram(r->program);
   if (r->batch_program)
      glDeleteProgram(r->batch_program);
   free(r->upload_pixels);
   free(r->readback_pixels);
   memset(r, 0, sizeof(*r));
}

static void renderer_common_state(struct renderer *r, bool surface)
{
   glBindFramebuffer(GL_FRAMEBUFFER, surface ? 0 : r->fbo);
   glViewport(0, 0, surface ? (GLsizei)r->surface_w : (GLsizei)r->render_w,
         surface ? (GLsizei)r->surface_h : (GLsizei)r->render_h);
   glUseProgram(r->program);
   glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
         (const void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
         (const void *)(2 * sizeof(GLfloat)));
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, r->textures[0]);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_BLEND);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   glUniform4f(r->u_xform, 0.05f, 0.05f, 0.0f, 0.0f);
   glUniform4f(r->u_p1, 0, 0, 0, 0);
   glUniform4f(r->u_p2, 0, 0, 0, 0);
   glUniform4f(r->u_p3, 0, 0, 0, 0);
}

static void uniform_data(unsigned index, GLfloat data[16])
{
   float x = -0.94f + (float)(index % 20) * 0.099f;
   float y = -0.90f + (float)((index / 20) % 10) * 0.195f;
   float f = (float)(index & 255);
   data[0] = 0.045f; data[1] = 0.085f; data[2] = x; data[3] = y;
   data[4] = f; data[5] = f * 0.5f; data[6] = 1.0f; data[7] = 2.0f;
   data[8] = 3.0f; data[9] = f * 0.25f; data[10] = 5.0f; data[11] = 6.0f;
   data[12] = 7.0f; data[13] = 8.0f; data[14] = f * 0.125f;
   data[15] = 10.0f;
}

static void set_separate_uniforms(struct renderer *r, unsigned index)
{
   GLfloat data[16];
   uniform_data(index, data);
   glUniform4f(r->u_xform, data[0], data[1], data[2], data[3]);
   glUniform4f(r->u_p1, data[4], data[5], data[6], data[7]);
   glUniform4f(r->u_p2, data[8], data[9], data[10], data[11]);
   glUniform4f(r->u_p3, data[12], data[13], data[14], data[15]);
}

static void draw_with_uniforms(struct renderer *r, unsigned index)
{
   set_separate_uniforms(r, index);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_with_uniform_array(struct renderer *r, unsigned index)
{
   GLfloat data[16];
   uniform_data(index, data);
   glUniform4fv(r->batch_u_data, 4, data);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void run_scenario_once(struct renderer *r, struct platform *p,
      const struct scenario *s)
{
   unsigned i;
   bool surface = s->kind == SC_SWAP;
   renderer_common_state(r, surface);

   switch (s->kind)
   {
      case SC_CLEAR:
         glClear(GL_COLOR_BUFFER_BIT);
         break;

      case SC_DRAW:
         for (i = 0; i < s->count; i++)
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         break;

      case SC_DRAW_BATCHED:
      {
         unsigned vertices_per_batch = 6000 / s->count;
         glBindBuffer(GL_ARRAY_BUFFER, r->batch_vbo);
         glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
               (const void *)0);
         glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
               (const void *)(2 * sizeof(GLfloat)));
         glUniform4f(r->u_xform, 1.0f, 1.0f, 0.0f, 0.0f);
         glUniform4f(r->u_p1, 0, 0, 0, 0);
         glUniform4f(r->u_p2, 0, 0, 0, 0);
         glUniform4f(r->u_p3, 0, 0, 0, 0);
         for (i = 0; i < s->count; i++)
            glDrawArrays(GL_TRIANGLES, (GLint)(i * vertices_per_batch),
                  (GLsizei)vertices_per_batch);
         break;
      }

      case SC_UNIFORM:
         for (i = 0; i < s->count; i++)
            draw_with_uniforms(r, i);
         break;

      case SC_UNIFORM_CACHED:
         for (i = 0; i < s->count; i++)
         {
            if ((i & 7) == 0)
               set_separate_uniforms(r, i);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         break;

      case SC_UNIFORM_VEC4FV:
         glUseProgram(r->batch_program);
         for (i = 0; i < s->count; i++)
            draw_with_uniform_array(r, i);
         break;

      case SC_TEXTURE:
         for (i = 0; i < s->count; i++)
         {
            glBindTexture(GL_TEXTURE_2D, r->textures[i & 1]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         break;

      case SC_TEXTURE_CACHED:
         for (i = 0; i < s->count; i++)
         {
            if ((i & 7) == 0)
               glBindTexture(GL_TEXTURE_2D, r->textures[(i >> 3) & 1]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         break;

      case SC_STATE:
         glEnable(GL_BLEND);
         for (i = 0; i < s->count; i++)
         {
            if (i & 1)
               glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            else
               glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         glDisable(GL_BLEND);
         break;

      case SC_STATE_CACHED:
         glEnable(GL_BLEND);
         for (i = 0; i < s->count; i++)
         {
            if ((i & 7) == 0)
               glBlendFunc((i & 8) ? GL_SRC_ALPHA : GL_ONE,
                     GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         glDisable(GL_BLEND);
         break;

      case SC_PROGRAM:
         for (i = 0; i < s->count; i++)
         {
            glUseProgram(r->program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         break;

      case SC_PPSSPP_LIKE:
         glEnable(GL_BLEND);
         for (i = 0; i < s->count; i++)
         {
            /* Around 1000 draws, 4000 uniform calls, 125 texture binds and
             * 125 state changes -- close to the command density observed in
             * the heavy God of War frame, but with deliberately cheap pixels. */
            if ((i & 7) == 0)
            {
               glBindTexture(GL_TEXTURE_2D, r->textures[(i >> 3) & 1]);
               glBlendFunc((i & 8) ? GL_SRC_ALPHA : GL_ONE,
                     GL_ONE_MINUS_SRC_ALPHA);
            }
            draw_with_uniforms(r, i);
         }
         glDisable(GL_BLEND);
         break;

      case SC_PPSSPP_OPTIMIZED:
         glUseProgram(r->batch_program);
         glEnable(GL_BLEND);
         for (i = 0; i < s->count; i++)
         {
            /* Upper-bound workaround: coalesce four uniform calls into one
             * array upload, and emit it only when the synthetic state group
             * changes. Texture and blend state use the same 1-in-8 cadence. */
            if ((i & 7) == 0)
            {
               GLfloat data[16];
               glBindTexture(GL_TEXTURE_2D, r->textures[(i >> 3) & 1]);
               glBlendFunc((i & 8) ? GL_SRC_ALPHA : GL_ONE,
                     GL_ONE_MINUS_SRC_ALPHA);
               uniform_data(i, data);
               glUniform4fv(r->batch_u_data, 4, data);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         }
         glDisable(GL_BLEND);
         break;

      case SC_FILL:
      case SC_FILL_DISCARD:
         glUniform4f(r->u_xform, 1.0f, 1.0f, 0.0f, 0.0f);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         for (i = 0; i < s->count; i++)
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         glDisable(GL_BLEND);
         if (s->kind == SC_FILL_DISCARD && discard_framebuffer_ext)
         {
            const GLenum attachment = GL_COLOR_ATTACHMENT0;
            discard_framebuffer_ext(GL_FRAMEBUFFER, 1, &attachment);
         }
         break;

      case SC_UPLOAD:
         glBindTexture(GL_TEXTURE_2D, r->textures[0]);
         for (i = 0; i < s->count; i++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RGBA,
                  GL_UNSIGNED_BYTE, r->upload_pixels);
         break;

      case SC_READBACK:
         glClear(GL_COLOR_BUFFER_BIT);
         glReadPixels(0, 0, (GLsizei)r->render_w, (GLsizei)r->render_h,
               GL_RGBA, GL_UNSIGNED_BYTE, r->readback_pixels);
         break;

      case SC_SWAP:
         glClear(GL_COLOR_BUFFER_BIT);
         glUniform4f(r->u_xform, 1.0f, 1.0f, 0.0f, 0.0f);
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
         eglSwapBuffers(p->egl_display, p->egl_surface);
         break;
   }
}

static uint64_t scenario_work(const struct renderer *r,
      const struct scenario *s)
{
   switch (s->kind)
   {
      case SC_CLEAR:
         return (uint64_t)r->render_w * r->render_h;
      case SC_FILL:
      case SC_FILL_DISCARD:
         return (uint64_t)r->render_w * r->render_h * s->count;
      case SC_DRAW_BATCHED:
         return 1000;
      case SC_UPLOAD:
         return (uint64_t)64 * 64 * 4 * s->count;
      case SC_READBACK:
         return (uint64_t)r->render_w * r->render_h * 4;
      default:
         return s->count;
   }
}

static int compare_u64(const void *a, const void *b)
{
   uint64_t av = *(const uint64_t *)a;
   uint64_t bv = *(const uint64_t *)b;
   return av < bv ? -1 : av > bv ? 1 : 0;
}

static uint64_t percentile(const uint64_t *sorted, unsigned n,
      unsigned percent)
{
   unsigned index;
   if (!n)
      return 0;
   index = (unsigned)(((uint64_t)(n - 1) * percent + 50) / 100);
   return sorted[index];
}

static bool measure_scenario(struct renderer *r, struct platform *p,
      const struct options *o, const struct scenario *s, FILE *csv)
{
   uint64_t *call_ns = NULL, *finish_ns = NULL, *total_ns = NULL;
   uint64_t sum_call = 0, sum_finish = 0, sum_total = 0, max_total = 0;
   uint64_t work = scenario_work(r, s);
   unsigned gl_errors = 0;
   unsigned i;
   bool ok = false;

   call_ns = (uint64_t *)calloc(o->frames, sizeof(*call_ns));
   finish_ns = (uint64_t *)calloc(o->frames, sizeof(*finish_ns));
   total_ns = (uint64_t *)calloc(o->frames, sizeof(*total_ns));
   if (!call_ns || !finish_ns || !total_ns)
      goto done;

   renderer_common_state(r, s->kind == SC_SWAP);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   while (glGetError() != GL_NO_ERROR) { }

   fprintf(stderr, "qnx_gles2_bench: BEGIN %-16s warmup=%u samples=%u\n",
         s->name, o->warmup, o->frames);
   for (i = 0; i < o->warmup && !interrupted; i++)
   {
      run_scenario_once(r, p, s);
      glFinish();
   }

   for (i = 0; i < o->frames && !interrupted; i++)
   {
      uint64_t t0 = timer_ticks();
      uint64_t t1, t2;
      GLenum error;
      run_scenario_once(r, p, s);
      t1 = timer_ticks();
      glFinish();
      t2 = timer_ticks();
      call_ns[i] = elapsed_ns(t0, t1);
      finish_ns[i] = elapsed_ns(t1, t2);
      total_ns[i] = elapsed_ns(t0, t2);
      sum_call += call_ns[i];
      sum_finish += finish_ns[i];
      sum_total += total_ns[i];
      if (total_ns[i] > max_total)
         max_total = total_ns[i];
      error = glGetError();
      if (error != GL_NO_ERROR)
      {
         gl_errors++;
         fprintf(stderr,
            "qnx_gles2_bench: GL error 0x%x in %s sample %u\n",
            (unsigned)error, s->name, i);
      }
   }
   if (interrupted)
      goto done;

   qsort(call_ns, o->frames, sizeof(*call_ns), compare_u64);
   qsort(finish_ns, o->frames, sizeof(*finish_ns), compare_u64);
   qsort(total_ns, o->frames, sizeof(*total_ns), compare_u64);

   fprintf(csv,
      "%s,%u,%llu,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%u\n",
      s->name, o->frames, (unsigned long long)work, s->unit,
      sum_call / 1e6 / o->frames,
      percentile(call_ns, o->frames, 50) / 1e6,
      percentile(call_ns, o->frames, 95) / 1e6,
      sum_finish / 1e6 / o->frames,
      percentile(finish_ns, o->frames, 95) / 1e6,
      sum_total / 1e6 / o->frames,
      percentile(total_ns, o->frames, 95) / 1e6,
      max_total / 1e6,
      sum_total ? (double)work * o->frames * 1e9 / sum_total : 0.0,
      gl_errors);
   fflush(csv);

   fprintf(stderr,
      "qnx_gles2_bench: END   %-16s call=%7.3f ms finish=%7.3f ms "
      "total=%7.3f ms p95=%7.3f ms %s/s=%.3f\n",
      s->name, sum_call / 1e6 / o->frames,
      sum_finish / 1e6 / o->frames, sum_total / 1e6 / o->frames,
      percentile(total_ns, o->frames, 95) / 1e6, s->unit,
      sum_total ? (double)work * o->frames * 1e9 / sum_total : 0.0);
   ok = gl_errors == 0;

done:
   free(call_ns);
   free(finish_ns);
   free(total_ns);
   return ok;
}

int main(int argc, char **argv)
{
   struct options o;
   struct platform p;
   struct renderer r;
   FILE *csv = stdout;
   size_t i;
   int parse_result;
   int exit_code = 1;
   bool renderer_ready = false;

   parse_result = parse_options(argc, argv, &o);
   if (parse_result > 0)
      return 0;
   if (parse_result < 0)
      return 2;

   if (o.cpu >= 0)
   {
      unsigned requested_mask = 1u << (unsigned)o.cpu;
      unsigned runmask = requested_mask;
      if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET, &runmask) == -1)
         fprintf(stderr,
            "qnx_gles2_bench: CPU%d affinity failed: %s; timer may be coarse\n",
            o.cpu, strerror(errno));
      else
         fprintf(stderr, "qnx_gles2_bench: submit thread pinned to CPU%d\n",
               o.cpu);
   }
   timer_init();

   signal(SIGINT, signal_handler);
   signal(SIGTERM, signal_handler);

   if (!platform_init(&p, &o))
      return 1;
   if (!configure_driver(&o))
      goto done;
   if (o.list_driver_controls)
   {
      exit_code = 0;
      goto done;
   }
   if (!renderer_init(&r, &o))
   {
      fprintf(stderr, "qnx_gles2_bench: renderer setup failed\n");
      goto done;
   }
   renderer_ready = true;

   if (o.csv_path)
   {
      csv = fopen(o.csv_path, "w");
      if (!csv)
      {
         fprintf(stderr, "qnx_gles2_bench: cannot open CSV %s: %s\n",
               o.csv_path, strerror(errno));
         goto done;
      }
   }

   fprintf(csv, "# qnx_gles2_bench_version=%s\n", BENCH_VERSION);
   fprintf(csv, "# gl_vendor=%s\n", safe_gl_string(GL_VENDOR));
   fprintf(csv, "# gl_renderer=%s\n", safe_gl_string(GL_RENDERER));
   fprintf(csv, "# gl_version=%s\n", safe_gl_string(GL_VERSION));
   fprintf(csv, "# surface=%ux%u\n", o.surface_w, o.surface_h);
   fprintf(csv, "# render=%ux%u\n", o.render_w, o.render_h);
   fprintf(csv, "# fbo_format=%s\n", o.fbo_format);
   fprintf(csv, "# requested_buffers=%d\n", o.buffers);
   fprintf(csv, "# actual_render_buffers=%d\n", p.actual_buffers);
   fprintf(csv, "# routed=%d\n", o.route ? 1 : 0);
   fprintf(csv, "# cpu_affinity=%d\n", o.cpu);
   fprintf(csv, "# driver_control=%s\n",
         o.driver_control ? o.driver_control : "none");
   fprintf(csv, "# binning=%s\n", o.binning);
   fprintf(csv, "# writeonly=%d\n", o.writeonly ? 1 : 0);
   fprintf(csv, "# discard_framebuffer_ext=%d\n",
         discard_framebuffer_ext ? 1 : 0);
   fprintf(csv, "# timer=%s\n",
         timer_cycles_per_second ? "ClockCycles" : "CLOCK_MONOTONIC");
   fprintf(csv, "# timer_cycles_per_second=%llu\n",
         (unsigned long long)timer_cycles_per_second);
   fprintf(csv, "# timer_syspage_raw=%llu\n",
         (unsigned long long)timer_reported_cycles_per_second);
   fprintf(csv, "# timer_tick_shift=%u\n", timer_tick_shift);
   fprintf(csv,
      "scenario,samples,work_per_sample,work_unit,call_mean_ms,call_p50_ms,"
      "call_p95_ms,finish_mean_ms,finish_p95_ms,total_mean_ms,total_p95_ms,"
      "total_max_ms,work_per_second,gl_errors\n");

   exit_code = 0;
   for (i = 0; i < ARRAY_SIZE(scenarios) && !interrupted; i++)
   {
      if (strcmp(o.scenario, "all") && strcmp(o.scenario, scenarios[i].name))
         continue;
      if (!measure_scenario(&r, &p, &o, &scenarios[i], csv))
         exit_code = 1;
   }
   if (interrupted)
   {
      fprintf(stderr, "qnx_gles2_bench: interrupted\n");
      exit_code = 130;
   }

done:
   if (csv && csv != stdout)
      fclose(csv);
   if (renderer_ready)
      renderer_destroy(&r);
   platform_destroy(&p, &o);
   return exit_code;
}
