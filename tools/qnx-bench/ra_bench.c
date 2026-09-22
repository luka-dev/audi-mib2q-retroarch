/* ra_bench -- headless libretro benchmark host for QNX 6.5 (MHI2Q).
 *
 * Runs a core for a fixed number of frames with no window, no EGL and no
 * input, and writes one CSV row per frame. It exists so an emulator change can
 * be measured unattended over ssh instead of asking someone to play.
 *
 * PPSSPP can run this way with `ppsspp_backend=none`, which selects
 * LibretroSoftwareContext (GPUCORE_SOFTWARE) and never asks for a hardware
 * context. That is useful for controlled CPU/JIT/HLE comparisons, but it is a
 * different renderer from normal play. It deliberately does NOT exercise the
 * Adreno or the QNX GLES driver. Use qnx_gles2_bench for graphics-path
 * microbenchmarks and the real RetroArch frontend for end-to-end measurements.
 *
 * Timings are per retro_run() call. Audio is counted, never played, so the run
 * is paced by nothing at all and finishes as fast as the CPU allows.
 *
 * ponytail: only the environment commands the QNX cores actually issue are
 * handled; unknown ones return false, which is a legal libretro answer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <dlfcn.h>

#include "libretro.h"

#define MAX_OPTIONS 256

struct opt_kv
{
   char *key;
   char *value;
};

static struct opt_kv  g_options[MAX_OPTIONS];
static unsigned       g_option_count;
static bool           g_options_dirty = true;

static char           g_system_dir[512];
static char           g_save_dir[512];
static const char    *g_content_path;

static uint64_t       g_audio_frames;      /* stereo frames handed to us     */
static uint64_t       g_video_frames;      /* non-duped frames the core drew */
static unsigned       g_log_lines;

static FILE          *g_logfile;

static uint64_t monotonic_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------- options */

/* Same "key = value" shape RetroArch writes, so a core-options file lifted
 * straight off the SD card can be handed to the bench unchanged. Quotes are
 * optional; anything after '#' is a comment. */
static void options_load(const char *path)
{
   char line[1024];
   FILE *f = fopen(path, "r");

   if (!f)
   {
      fprintf(stderr, "ra_bench: cannot read options file %s\n", path);
      return;
   }

   while (fgets(line, sizeof(line), f))
   {
      char *hash, *eq, *key, *value, *end;

      if ((hash = strchr(line, '#')))
         *hash = '\0';
      if (!(eq = strchr(line, '=')))
         continue;
      *eq = '\0';

      key   = line;
      value = eq + 1;

      while (*key == ' ' || *key == '\t')
         key++;
      for (end = key + strlen(key); end > key &&
            (end[-1] == ' ' || end[-1] == '\t'); end--)
         end[-1] = '\0';

      while (*value == ' ' || *value == '\t' || *value == '"')
         value++;
      for (end = value + strlen(value); end > value &&
            (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
             end[-1] == '\t' || end[-1] == '"'); end--)
         end[-1] = '\0';

      if (!*key || g_option_count >= MAX_OPTIONS)
         continue;

      g_options[g_option_count].key   = strdup(key);
      g_options[g_option_count].value = strdup(value);
      g_option_count++;
   }

   fclose(f);
   fprintf(stderr, "ra_bench: %u core option(s) from %s\n",
         g_option_count, path);
}

static const char *options_get(const char *key)
{
   unsigned i;
   for (i = 0; i < g_option_count; i++)
      if (!strcmp(g_options[i].key, key))
         return g_options[i].value;
   return NULL;
}

/* ------------------------------------------------------------- callbacks */

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   static const char *tag[] = { "DBG", "INF", "WRN", "ERR" };
   va_list ap;

   if (!g_logfile)
      return;
   if (level == RETRO_LOG_DEBUG)      /* far too chatty to keep */
      return;

   g_log_lines++;
   fprintf(g_logfile, "[%s] ", level < 4 ? tag[level] : "???");
   va_start(ap, fmt);
   vfprintf(g_logfile, fmt, ap);
   va_end(ap);
}

static bool environment_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         var->value = options_get(var->key);
         return var->value != NULL;
      }

      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data   = g_options_dirty;
         g_options_dirty = false;
         return true;

      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         /* Any format is fine; the frames are counted, never looked at. */
         return true;

      case RETRO_ENVIRONMENT_SET_HW_RENDER:
         /* Refuse, so a core offering both paths picks its software one.
          * With ppsspp_backend=none this is never reached. */
         return false;

      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
         *(const char**)data = g_system_dir;
         return true;

      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = g_save_dir;
         return true;

      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;

      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;

      case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
      case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
      case RETRO_ENVIRONMENT_SET_VARIABLES:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
      case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
         return true;

      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = 0;   /* plain GET_VARIABLE is all we serve */
         return true;

      case RETRO_ENVIRONMENT_GET_LANGUAGE:
         *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
         return true;

      default:
         return false;
   }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   (void)w; (void)h; (void)pitch;
   if (data)            /* NULL means "same frame again" */
      g_video_frames++;
}

static void audio_sample_cb(int16_t l, int16_t r)
{
   (void)l; (void)r;
   g_audio_frames++;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
   (void)data;
   g_audio_frames += frames;
   return frames;
}

static void input_poll_cb(void) { }

static int16_t input_state_cb(unsigned port, unsigned device,
      unsigned index, unsigned id)
{
   (void)port; (void)device; (void)index; (void)id;
   return 0;
}

/* ------------------------------------------------------------------- core */

struct core
{
   void *handle;
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   bool (*load_game)(const struct retro_game_info*);
   void (*unload_game)(void);
   void (*run)(void);
   void (*get_system_av_info)(struct retro_system_av_info*);
   size_t (*serialize_size)(void);
   bool (*serialize)(void*, size_t);
   bool (*unserialize)(const void*, size_t);
   unsigned (*api_version)(void);
};

#define SYM(field, name) do {                                            \
      *(void**)(&c->field) = dlsym(c->handle, name);                     \
      if (!c->field) {                                                   \
         fprintf(stderr, "ra_bench: core is missing %s\n", name);        \
         return false;                                                   \
      }                                                                  \
   } while (0)

static bool core_open(struct core *c, const char *path)
{
   memset(c, 0, sizeof(*c));
   if (!(c->handle = dlopen(path, RTLD_LAZY)))
   {
      fprintf(stderr, "ra_bench: dlopen(%s): %s\n", path, dlerror());
      return false;
   }

   SYM(set_environment,        "retro_set_environment");
   SYM(set_video_refresh,      "retro_set_video_refresh");
   SYM(set_audio_sample,       "retro_set_audio_sample");
   SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
   SYM(set_input_poll,         "retro_set_input_poll");
   SYM(set_input_state,        "retro_set_input_state");
   SYM(init,                   "retro_init");
   SYM(deinit,                 "retro_deinit");
   SYM(load_game,              "retro_load_game");
   SYM(unload_game,            "retro_unload_game");
   SYM(run,                    "retro_run");
   SYM(get_system_av_info,     "retro_get_system_av_info");
   SYM(api_version,            "retro_api_version");

   /* Optional: only needed for --state-in / --state-out. */
   *(void**)(&c->serialize_size) = dlsym(c->handle, "retro_serialize_size");
   *(void**)(&c->serialize)      = dlsym(c->handle, "retro_serialize");
   *(void**)(&c->unserialize)    = dlsym(c->handle, "retro_unserialize");
   return true;
}

static bool state_load(struct core *c, const char *path)
{
   long   size;
   void  *buf;
   FILE  *f;
   bool   ok;

   if (!c->unserialize)
   {
      fprintf(stderr, "ra_bench: core has no retro_unserialize\n");
      return false;
   }
   if (!(f = fopen(path, "rb")))
   {
      fprintf(stderr, "ra_bench: cannot open state %s\n", path);
      return false;
   }
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size <= 0 || !(buf = malloc((size_t)size)))
   {
      fclose(f);
      return false;
   }
   ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
   fclose(f);
   ok = ok && c->unserialize(buf, (size_t)size);
   free(buf);
   fprintf(stderr, "ra_bench: state %s (%ld bytes) %s\n",
         path, size, ok ? "loaded" : "REJECTED");
   return ok;
}

static bool state_save(struct core *c, const char *path)
{
   size_t size;
   void  *buf;
   FILE  *f;
   bool   ok;

   if (!c->serialize || !c->serialize_size)
      return false;
   if (!(size = c->serialize_size()) || !(buf = malloc(size)))
      return false;
   if (!c->serialize(buf, size) || !(f = fopen(path, "wb")))
   {
      free(buf);
      return false;
   }
   ok = fwrite(buf, 1, size, f) == size;
   fclose(f);
   free(buf);
   fprintf(stderr, "ra_bench: state written to %s (%u bytes)\n",
         path, (unsigned)size);
   return ok;
}

/* ------------------------------------------------------------------- main */

static void usage(const char *argv0)
{
   fprintf(stderr,
      "usage: %s --core PATH --content PATH [options]\n"
      "\n"
      "  --frames N        frames to measure (default 1800)\n"
      "  --warmup N        frames to run untimed first (default 300)\n"
      "  --options FILE    core options, RetroArch \"key = value\" format\n"
      "  --system DIR      system/BIOS directory\n"
      "  --save DIR        save directory\n"
      "  --state-in FILE   load a raw core state before measuring\n"
      "  --state-out FILE  write a raw core state after the warmup\n"
      "  --csv FILE        per-frame timings (default stdout)\n"
      "  --log FILE        core log output (default discarded)\n"
      "\n"
      "States are raw retro_serialize() blobs, not RetroArch .state files\n"
      "(those are rzip-compressed unless savestate_file_compression=false).\n"
      "Produce one with --state-out and reuse it with --state-in.\n",
      argv0);
}

int main(int argc, char **argv)
{
   const char *core_path = NULL, *content = NULL, *options_path = NULL;
   const char *state_in = NULL, *state_out = NULL;
   const char *csv_path = NULL, *log_path = NULL;
   unsigned frames = 1800, warmup = 300, i;
   struct retro_system_av_info av;
   struct retro_game_info game;
   struct core c;
   FILE *csv = stdout;
   uint64_t *frame_us;
   uint64_t run_start, total_us = 0, worst_us = 0;
   uint64_t audio_at_start;

   snprintf(g_system_dir, sizeof(g_system_dir), ".");
   snprintf(g_save_dir,   sizeof(g_save_dir),   ".");

   for (i = 1; i < (unsigned)argc; i++)
   {
      const char *a = argv[i];
      const char *next = (i + 1 < (unsigned)argc) ? argv[i + 1] : NULL;

      if      (!strcmp(a, "--core")       && next) core_path    = argv[++i];
      else if (!strcmp(a, "--content")    && next) content      = argv[++i];
      else if (!strcmp(a, "--options")    && next) options_path = argv[++i];
      else if (!strcmp(a, "--state-in")   && next) state_in     = argv[++i];
      else if (!strcmp(a, "--state-out")  && next) state_out    = argv[++i];
      else if (!strcmp(a, "--csv")        && next) csv_path     = argv[++i];
      else if (!strcmp(a, "--log")        && next) log_path     = argv[++i];
      else if (!strcmp(a, "--frames")     && next) frames = (unsigned)atoi(argv[++i]);
      else if (!strcmp(a, "--warmup")     && next) warmup = (unsigned)atoi(argv[++i]);
      else if (!strcmp(a, "--system")     && next)
         snprintf(g_system_dir, sizeof(g_system_dir), "%s", argv[++i]);
      else if (!strcmp(a, "--save")       && next)
         snprintf(g_save_dir, sizeof(g_save_dir), "%s", argv[++i]);
      else { usage(argv[0]); return 2; }
   }

   if (!core_path || !content || !frames)
   {
      usage(argv[0]);
      return 2;
   }

   if (options_path)
      options_load(options_path);
   if (log_path && !(g_logfile = fopen(log_path, "w")))
      fprintf(stderr, "ra_bench: cannot write log %s\n", log_path);

   if (!core_open(&c, core_path))
      return 1;

   fprintf(stderr, "ra_bench: libretro API %u\n", c.api_version());

   c.set_environment(environment_cb);
   c.init();
   c.set_video_refresh(video_cb);
   c.set_audio_sample(audio_sample_cb);
   c.set_audio_sample_batch(audio_batch_cb);
   c.set_input_poll(input_poll_cb);
   c.set_input_state(input_state_cb);

   g_content_path = content;
   memset(&game, 0, sizeof(game));
   game.path = content;
   if (!c.load_game(&game))
   {
      fprintf(stderr, "ra_bench: retro_load_game failed for %s\n", content);
      return 1;
   }

   c.get_system_av_info(&av);
   fprintf(stderr, "ra_bench: %ux%u, %.2f fps, %.0f Hz audio\n",
         av.geometry.base_width, av.geometry.base_height,
         av.timing.fps, av.timing.sample_rate);

   for (i = 0; i < warmup; i++)
      c.run();
   fprintf(stderr, "ra_bench: warmup %u frames done\n", warmup);

   if (state_out)
      state_save(&c, state_out);
   if (state_in && !state_load(&c, state_in))
      return 1;

   if (csv_path && !(csv = fopen(csv_path, "w")))
   {
      fprintf(stderr, "ra_bench: cannot write csv %s\n", csv_path);
      return 1;
   }

   if (!(frame_us = calloc(frames, sizeof(*frame_us))))
      return 1;

   /* Reset the counters so warmup and state loading stay out of the numbers. */
   g_video_frames = 0;
   audio_at_start = g_audio_frames;

   run_start = monotonic_ns();
   for (i = 0; i < frames; i++)
   {
      uint64_t t0 = monotonic_ns();
      c.run();
      frame_us[i] = (monotonic_ns() - t0) / 1000;
      total_us   += frame_us[i];
      if (frame_us[i] > worst_us)
         worst_us = frame_us[i];
   }
   {
      uint64_t wall_us   = (monotonic_ns() - run_start) / 1000;
      uint64_t audio     = g_audio_frames - audio_at_start;
      double   wall_s    = wall_us / 1e6;
      double   host_fps  = wall_s > 0 ? frames / wall_s : 0;
      /* Emulated seconds the core produced, measured by its own audio output
       * rather than by our clock -- the honest speed number. */
      double   emu_s     = av.timing.sample_rate > 0 ?
                           audio / av.timing.sample_rate : 0;

      fprintf(csv, "frame,us\n");
      for (i = 0; i < frames; i++)
         fprintf(csv, "%u,%llu\n", i, (unsigned long long)frame_us[i]);
      if (csv != stdout)
         fclose(csv);

      fprintf(stderr,
         "\n== ra_bench summary ==\n"
         "  frames          %u\n"
         "  wall            %.2f s\n"
         "  host fps        %.2f\n"
         "  mean frame      %.2f ms\n"
         "  worst frame     %.2f ms\n"
         "  video frames    %llu (%.1f%% duped)\n"
         "  audio frames    %llu = %.2f emulated s\n"
         "  speed           %.1f%% of real time\n"
         "  core log lines  %u\n",
         frames, wall_s, host_fps,
         total_us / 1000.0 / frames, worst_us / 1000.0,
         (unsigned long long)g_video_frames,
         frames ? 100.0 * (frames - g_video_frames) / frames : 0.0,
         (unsigned long long)audio, emu_s,
         wall_s > 0 ? 100.0 * emu_s / wall_s : 0.0,
         g_log_lines);
   }

   free(frame_us);
   c.unload_game();
   c.deinit();
   if (g_logfile)
      fclose(g_logfile);
   return 0;
}
