/* Minimal libretro core for MHI2Q bring-up (MS3).
 * Proves the core pipeline: builds to a QNX armle-v7 .so with the right EABI,
 * exports the full retro_* API, and produces a video frame each retro_run().
 * Real cores (gpsp, pcsx_rearmed) are the same ABI/recipe, just bigger.
 *
 * Draws a moving vertical colour bar into an RGB565 framebuffer so a frame is
 * observably "running". No input, no audio. */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "libretro.h"

#define FB_W 320
#define FB_H 240

static uint16_t          fb[FB_W * FB_H];
static unsigned          frame;
static retro_video_refresh_t   video_cb;
static retro_environment_t     environ_cb;
static retro_input_poll_t      input_poll_cb;
static retro_input_state_t     input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   /* RGB565 pixel format */
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}
void retro_set_video_refresh(retro_video_refresh_t cb)       { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)         { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)             { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)           { input_state_cb = cb; }

void retro_init(void) { frame = 0; }
void retro_deinit(void) { }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "MHI2Q TestCore";
   info->library_version  = "0.1";
   info->need_fullpath    = false;
   info->valid_extensions = "bin|test";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps          = 60.0;
   info->timing.sample_rate  = 44100.0;
   info->geometry.base_width   = FB_W;
   info->geometry.base_height  = FB_H;
   info->geometry.max_width    = FB_W;
   info->geometry.max_height   = FB_H;
   info->geometry.aspect_ratio = (float)FB_W / (float)FB_H;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_reset(void) { frame = 0; }

void retro_run(void)
{
   unsigned x, y;
   if (input_poll_cb)
      input_poll_cb();

   for (y = 0; y < FB_H; y++)
   {
      for (x = 0; x < FB_W; x++)
      {
         unsigned bar = (x + frame) & 0xff;
         /* RGB565 gradient that scrolls with the frame counter */
         uint16_t r = (bar >> 3) & 0x1f;
         uint16_t g = ((y >> 2) & 0x3f);
         uint16_t b = ((frame >> 1) & 0x1f);
         fb[y * FB_W + x] = (r << 11) | (g << 5) | b;
      }
   }
   frame++;
   if (video_cb)
      video_cb(fb, FB_W, FB_H, FB_W * sizeof(uint16_t));
}

bool retro_load_game(const struct retro_game_info *game) { (void)game; return true; }
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n)
{ (void)t; (void)i; (void)n; return false; }
void retro_unload_game(void) { }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t n)   { (void)d; (void)n; return false; }
bool retro_unserialize(const void *d, size_t n) { (void)d; (void)n; return false; }
void *retro_get_memory_data(unsigned id)  { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
