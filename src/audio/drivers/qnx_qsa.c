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

/* QSA (QNX Sound Architecture) audio driver for MHI2Q.
 *
 * PCM transport only. It deliberately does NOT touch the `MS_ENT` mixer switch:
 * that switch is the head unit's *entertainment source selector* (one mplN -> amp
 * at a time) and belongs to the audio manager. gpSP grabbed it directly because it
 * had no native DSI; doing that fights the HMI over the selector and desyncs the
 * volume/mute/ducking model. Routing/focus is requested by the injected Java
 * state through the stock Media focus/HMIAudio/ATIP-route services — see the
 * README audio section.
 *
 * Default device is `/dev/snd/mpl1_int_ent` = VIRTUALCHANNEL_ENT_INTMEDIA, the
 * internal-media entertainment channel (semantically what an emulator is, and
 * definitely ACDB-provisioned). Note it is a **6-channel** device (mpl5_dio_ent is
 * 2ch), so stereo is upmixed to the device's native voice count here.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/asoundlib.h>

#include <boolean.h>
#include <retro_miscellaneous.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

#define QSA_DEFAULT_DEVICE "/dev/snd/mpl1_int_ent"
#define QSA_READY_DEFAULT  "/tmp/retroarch.pcm.ready"
#define QSA_RING_MIN_FRAGS 8
#define QSA_RING_MAX_FRAGS 16
#define QSA_INITIAL_PREFILL_MAX 8
#define QSA_RECOVERY_PREFILL 3

typedef struct qsa_audio
{
   snd_pcm_t *pcm;
   uint8_t   *mixbuf;        /* S16 stereo -> native format/N-voice scratch */
   size_t     mixbuf_frames;
   uint8_t   *fragbuf;       /* only full hardware fragments reach QSA */
   size_t     fragbuf_len;
   size_t     fragbuf_cap;
   int        frag_size;     /* bytes */
   int        frags;         /* actual hardware fragment count */
   int        voices;        /* device native channel count (6 on mpl1) */
   int        format;        /* SND_PCM_SFMT_* */
   int        sample_bytes;  /* 2 for S16, 4 for S32 */
   bool       swap_endian;
   unsigned   rate;
   bool       nonblock;
   bool       started;
   unsigned   underruns;
   unsigned   write_errors;
   unsigned   short_writes;
   unsigned   backpressure_events;
} qsa_audio_t;

static const char *qsa_ready_path(void)
{
   const char *path = getenv("RA_QNX_AUDIO_READY_PATH");
   return (path && *path) ? path : QSA_READY_DEFAULT;
}

static void qsa_ready_clear(void)
{
   unlink(qsa_ready_path());
}

static bool qsa_ready_set(void)
{
   const char *path = qsa_ready_path();
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      RARCH_ERR("[QSA]: cannot create ready marker %s: %s.\n",
            path, strerror(errno));
      return false;
   }
   if (write(fd, "ready\n", 6) != 6)
   {
      RARCH_ERR("[QSA]: cannot write ready marker %s: %s.\n",
            path, strerror(errno));
      close(fd);
      unlink(path);
      return false;
   }
   close(fd);
   return true;
}

/* RetroArch gives this driver signed 16-bit stereo. The MHI2Q mpl devices may
 * expose either S16 or S32 (some firmware revisions advertise mpl1 as S32
 * only), so choose a real advertised linear format and convert explicitly. */
static bool qsa_pick_format(int formats, qsa_audio_t *qsa)
{
   if (formats & SND_PCM_FMT_S16_LE)
   {
      qsa->format       = SND_PCM_SFMT_S16_LE;
      qsa->sample_bytes = 2;
      qsa->swap_endian  = false;
      return true;
   }
   if (formats & SND_PCM_FMT_S32_LE)
   {
      qsa->format       = SND_PCM_SFMT_S32_LE;
      qsa->sample_bytes = 4;
      qsa->swap_endian  = false;
      return true;
   }
   if (formats & SND_PCM_FMT_S16_BE)
   {
      qsa->format       = SND_PCM_SFMT_S16_BE;
      qsa->sample_bytes = 2;
      qsa->swap_endian  = true;
      return true;
   }
   if (formats & SND_PCM_FMT_S32_BE)
   {
      qsa->format       = SND_PCM_SFMT_S32_BE;
      qsa->sample_bytes = 4;
      qsa->swap_endian  = true;
      return true;
   }
   return false;
}

static uint16_t qsa_bswap16(uint16_t v)
{
   return (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t qsa_bswap32(uint32_t v)
{
   return ((v & 0x000000ffU) << 24)
        | ((v & 0x0000ff00U) << 8)
        | ((v & 0x00ff0000U) >> 8)
        | ((v & 0xff000000U) >> 24);
}

static void qsa_store_sample(const qsa_audio_t *qsa, uint8_t *dst, int16_t s)
{
   if (qsa->sample_bytes == 2)
   {
      uint16_t v = (uint16_t)s;
      if (qsa->swap_endian)
         v = qsa_bswap16(v);
      memcpy(dst, &v, sizeof(v));
   }
   else
   {
      /* Put S16 in the most-significant 16 bits of signed S32, preserving
       * full scale. Multiplication avoids left-shifting a negative value. */
      uint32_t v = (uint32_t)((int32_t)s * 65536);
      if (qsa->swap_endian)
         v = qsa_bswap32(v);
      memcpy(dst, &v, sizeof(v));
   }
}

/* Write complete silence fragments without recursing through recovery. */
static bool qsa_prefill(qsa_audio_t *qsa, int fragments)
{
   uint8_t *silence;
   int done = 0;

   if (!qsa || !qsa->pcm || qsa->frag_size <= 0 || fragments <= 0)
      return false;
   if (!(silence = (uint8_t*)calloc(1, (size_t)qsa->frag_size)))
      return false;

   while (done < fragments)
   {
      size_t offset = 0;
      int retries = 0;
      while (offset < (size_t)qsa->frag_size)
      {
         int written = snd_pcm_write(qsa->pcm, silence + offset,
               (int)((size_t)qsa->frag_size - offset));
         if (written > 0)
         {
            offset += (size_t)written;
            retries = 0;
            continue;
         }
         if (written == 0)
            qsa->short_writes++;
         else
            qsa->write_errors++;
         if (++retries >= 3)
            break;
      }
      if (offset != (size_t)qsa->frag_size)
         break;
      done++;
   }

   free(silence);
   RARCH_LOG("[QSA]: prefilled %d/%d silence fragments.\n", done, fragments);
   return done == fragments;
}

/* After an underrun the channel sits in UNDERRUN/READY and refuses writes until
 * it is prepared again. Re-prime it with silence so the CSD session does not
 * fall into a start/stop loop. */
static bool qsa_recover(qsa_audio_t *qsa)
{
   snd_pcm_channel_status_t st;

   memset(&st, 0, sizeof(st));
   st.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_status(qsa->pcm, &st) < 0)
      return false;

   if (     st.status == SND_PCM_STATUS_UNDERRUN
         || st.status == SND_PCM_STATUS_READY)
   {
      qsa->underruns++;
      if (snd_pcm_channel_prepare(qsa->pcm,
               SND_PCM_CHANNEL_PLAYBACK) < 0)
         return false;
      RARCH_WARN("[QSA]: recovering underrun #%u (status=%d free=%u).\n",
            qsa->underruns, (int)st.status, (unsigned)st.free);
      if (!qsa_prefill(qsa, QSA_RECOVERY_PREFILL))
      {
         snd_pcm_playback_flush(qsa->pcm);
         return false;
      }
      return true;
   }
   return true;
}

static size_t qsa_device_to_input_bytes(const qsa_audio_t *qsa, size_t bytes)
{
   size_t frame_bytes;
   if (!qsa || qsa->voices <= 0 || qsa->sample_bytes <= 0)
      return 0;
   frame_bytes = (size_t)qsa->voices * (size_t)qsa->sample_bytes;
   return (bytes / frame_bytes) * (2 * sizeof(int16_t));
}

static void *qsa_init(const char *device, unsigned rate, unsigned latency,
      unsigned block_frames, unsigned *new_rate)
{
   snd_pcm_channel_info_t   cinfo;
   snd_pcm_channel_params_t params;
   snd_pcm_channel_setup_t  setup;
   const char *dev  = device;
   unsigned hw_rate = rate;
   size_t target_bytes;
   int requested_frags;
   int frame_bytes;
   qsa_audio_t *qsa = (qsa_audio_t*)calloc(1, sizeof(qsa_audio_t));

   qsa_ready_clear();
   if (!qsa)
      return NULL;

   if (!dev || !*dev)
   {
      dev = getenv("RA_QNX_AUDIO_DEV");
      if (!dev || !*dev)
         dev = QSA_DEFAULT_DEVICE;
   }

   if (snd_pcm_open_name(&qsa->pcm, (char*)dev, SND_PCM_OPEN_PLAYBACK) < 0)
   {
      RARCH_ERR("[QSA]: cannot open \"%s\".\n", dev);
      free(qsa);
      return NULL;
   }
   RARCH_LOG("[QSA]: opened \"%s\".\n", dev);

   /* Keep DMA buffer handling consistent with snd_pcm_write(). */
   snd_pcm_plugin_set_disable(qsa->pcm, PLUGIN_DISABLE_MMAP);

   memset(&cinfo, 0, sizeof(cinfo));
   cinfo.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_info(qsa->pcm, &cinfo) < 0)
   {
      RARCH_ERR("[QSA]: snd_pcm_channel_info failed.\n");
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   /* Use the device's NATIVE voice count: mpl1_int_ent is 6ch, mpl5_dio_ent 2ch.
    * Writing 2ch into a 6ch device produces garbage/silence, so we upmix. */
   qsa->voices = (cinfo.min_voices > 0) ? cinfo.min_voices : 2;
   if (cinfo.max_voices > 0 && qsa->voices > cinfo.max_voices)
      qsa->voices = cinfo.max_voices;
   if (qsa->voices < 1)
      qsa->voices = 2;

   if (!qsa_pick_format((int)cinfo.formats, qsa))
   {
      RARCH_ERR("[QSA]: no signed S16/S32 PCM format in mask 0x%x.\n",
            (unsigned)cinfo.formats);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   if (cinfo.min_rate > 0 && hw_rate < (unsigned)cinfo.min_rate)
      hw_rate = (unsigned)cinfo.min_rate;
   if (cinfo.max_rate > 0 && hw_rate > (unsigned)cinfo.max_rate)
      hw_rate = (unsigned)cinfo.max_rate;
   if (cinfo.min_rate > 0 && cinfo.min_rate == cinfo.max_rate)
      hw_rate = (unsigned)cinfo.min_rate;

   frame_bytes = qsa->voices * qsa->sample_bytes;
   qsa->frag_size = (cinfo.min_fragment_size > 0)
      ? cinfo.min_fragment_size : 4096;
   if (block_frames > 0)
   {
      size_t requested = (size_t)block_frames * (size_t)frame_bytes;
      if (requested > (size_t)qsa->frag_size)
         qsa->frag_size = (int)requested;
   }
   if (cinfo.max_fragment_size > 0
         && qsa->frag_size > cinfo.max_fragment_size)
      qsa->frag_size = cinfo.max_fragment_size;
   qsa->frag_size -= qsa->frag_size % frame_bytes;
   if (qsa->frag_size < frame_bytes)
      qsa->frag_size = frame_bytes;

   /* Convert RetroArch's requested latency into a bounded hardware ring depth.
    * The working gpSP trace proves that this HU needs at least eight fragments
    * (the driver commonly returns 10) to absorb render-thread jitter without
    * CSD start/stop cycling. */
   if (!latency)
      latency = 64;
   target_bytes = ((size_t)hw_rate * latency / 1000) * (size_t)frame_bytes;
   requested_frags = (int)((target_bytes + qsa->frag_size - 1)
         / (size_t)qsa->frag_size);
   if (requested_frags < QSA_RING_MIN_FRAGS)
      requested_frags = QSA_RING_MIN_FRAGS;
   if (requested_frags > QSA_RING_MAX_FRAGS)
      requested_frags = QSA_RING_MAX_FRAGS;
   qsa->frags = requested_frags;

   memset(&params, 0, sizeof(params));
   params.channel                = SND_PCM_CHANNEL_PLAYBACK;
   params.mode                   = SND_PCM_MODE_BLOCK;
   params.start_mode             = SND_PCM_START_DATA;
   params.stop_mode              = SND_PCM_STOP_STOP;
   params.format.interleave      = 1;
   params.format.format          = qsa->format;
   params.format.rate            = hw_rate;
   params.format.voices          = qsa->voices;
   params.buf.block.frag_size    = qsa->frag_size;
   params.buf.block.frags_min    = MIN(QSA_RING_MIN_FRAGS, qsa->frags);
   params.buf.block.frags_max    = qsa->frags;

   if (snd_pcm_channel_params(qsa->pcm, &params) < 0)
   {
      RARCH_ERR("[QSA]: snd_pcm_channel_params failed (rate %u, %d voices).\n",
            hw_rate, qsa->voices);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   if (snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK) < 0)
   {
      RARCH_ERR("[QSA]: initial channel prepare failed.\n");
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   memset(&setup, 0, sizeof(setup));
   setup.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_setup(qsa->pcm, &setup) < 0)
   {
      RARCH_ERR("[QSA]: snd_pcm_channel_setup readback failed.\n");
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }
   if (!setup.format.interleave || setup.format.format != qsa->format)
   {
      RARCH_ERR("[QSA]: device changed requested PCM layout "
            "(interleave=%d format=%d, wanted format=%d).\n",
            (int)setup.format.interleave, setup.format.format, qsa->format);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   qsa->frag_size = setup.buf.block.frag_size;
   if (setup.buf.block.frags > 0)
      qsa->frags = setup.buf.block.frags;
   if (setup.format.voices > 0)
      qsa->voices = setup.format.voices;
   qsa->rate = setup.format.rate > 0 ? setup.format.rate : hw_rate;
   frame_bytes = qsa->voices * qsa->sample_bytes;
   if (qsa->voices <= 0 || qsa->frag_size <= 0 || frame_bytes <= 0
         || qsa->frag_size % frame_bytes != 0)
   {
      RARCH_ERR("[QSA]: invalid negotiated geometry: voices=%d "
            "fragment=%d frame=%d.\n", qsa->voices, qsa->frag_size,
            frame_bytes);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }
   if (new_rate)
      *new_rate = qsa->rate;

   RARCH_LOG("[QSA]: %u Hz, %d voices, S%d%s, %d x %d-byte fragments.\n",
         qsa->rate, qsa->voices, qsa->sample_bytes * 8,
         qsa->swap_endian ? " BE" : " LE", qsa->frags, qsa->frag_size);

   /* Four fragments are enough for the largest normal RetroArch batch while
    * keeping the software queue bounded; the hardware ring remains the clock. */
   qsa->fragbuf_cap = (size_t)qsa->frag_size * 4;
   qsa->fragbuf = (uint8_t*)malloc(qsa->fragbuf_cap);
   if (!qsa->fragbuf)
   {
      RARCH_ERR("[QSA]: cannot allocate %u-byte fragment accumulator.\n",
            (unsigned)qsa->fragbuf_cap);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   {
      int prefill = qsa->frags - 2;
      if (prefill > QSA_INITIAL_PREFILL_MAX)
         prefill = QSA_INITIAL_PREFILL_MAX;
      if (prefill < 2)
         prefill = 2;
      if (!qsa_prefill(qsa, prefill))
      {
         RARCH_ERR("[QSA]: initial PCM prefill failed.\n");
         snd_pcm_playback_flush(qsa->pcm);
         snd_pcm_close(qsa->pcm);
         free(qsa->fragbuf);
         free(qsa);
         return NULL;
      }
      qsa->started = true;
      if (!qsa_ready_set())
      {
         qsa->started = false;
         RARCH_ERR("[QSA]: initial PCM handshake failed.\n");
         snd_pcm_playback_flush(qsa->pcm);
         snd_pcm_close(qsa->pcm);
         free(qsa->fragbuf);
         free(qsa);
         return NULL;
      }
   }

   RARCH_LOG("[QSA]: PCM ready marker published; HMI may fade in connection 20.\n");

   return qsa;
}

static bool qsa_flush_fragments(qsa_audio_t *qsa)
{
   bool progressed = false;

   while (qsa->fragbuf_len >= (size_t)qsa->frag_size)
   {
      size_t offset = 0;
      int failures = 0;

      while (offset < (size_t)qsa->frag_size)
      {
         int written = snd_pcm_write(qsa->pcm,
               qsa->fragbuf + offset,
               (int)((size_t)qsa->frag_size - offset));
         if (written > 0)
         {
            offset += (size_t)written;
            progressed = true;
            failures = 0;
            continue;
         }

         if (written == 0)
            qsa->short_writes++;
         else
            qsa->write_errors++;
         qsa_recover(qsa);
         if (++failures >= (qsa->nonblock ? 1 : 3))
            break;
      }

      if (offset)
      {
         qsa->fragbuf_len -= offset;
         if (qsa->fragbuf_len)
            memmove(qsa->fragbuf, qsa->fragbuf + offset,
                  qsa->fragbuf_len);
      }
      if (offset < (size_t)qsa->frag_size)
         break;
   }

   return progressed;
}

static ssize_t qsa_write(void *data, const void *s, size_t len)
{
   qsa_audio_t   *qsa    = (qsa_audio_t*)data;
   const int16_t *in     = (const int16_t*)s;
   size_t frames         = len / (2 * sizeof(int16_t));   /* incoming stereo */
   size_t device_frame_bytes;
   const void *out       = s;
   size_t out_bytes      = len;
   size_t accepted       = 0;

   if (!qsa || !qsa->pcm || !qsa->started || !frames)
      return 0;

   device_frame_bytes = (size_t)qsa->voices * (size_t)qsa->sample_bytes;

   /* Convert/upmix stereo -> the device's native format and voice count. */
   if (qsa->voices != 2 || qsa->sample_bytes != 2 || qsa->swap_endian)
   {
      size_t f, v;
      if (qsa->mixbuf_frames < frames)
      {
         uint8_t *nb = (uint8_t*)realloc(qsa->mixbuf,
               frames * qsa->voices * qsa->sample_bytes);
         if (!nb)
            return 0;
         qsa->mixbuf        = nb;
         qsa->mixbuf_frames = frames;
      }
      for (f = 0; f < frames; f++)
      {
         int16_t l = in[f * 2];
         int16_t r = in[f * 2 + 1];
         for (v = 0; v < (size_t)qsa->voices; v++)
         {
            int16_t sample = qsa->voices == 1
               ? (int16_t)(((int32_t)l + (int32_t)r) / 2)
               : ((v & 1) ? r : l);
            qsa_store_sample(qsa,
                  qsa->mixbuf + (f * qsa->voices + v) * qsa->sample_bytes,
                  sample);
         }
      }
      out       = qsa->mixbuf;
      out_bytes = frames * qsa->voices * qsa->sample_bytes;
   }

   /* QSA on this unit is stable only when fed hardware-fragment-sized writes.
    * Feed an arbitrarily large RetroArch block through a bounded accumulator;
    * in nonblocking mode report exact partial progress when the ring is full. */
   while (accepted < out_bytes)
   {
      size_t space;
      size_t chunk;

      qsa_flush_fragments(qsa);
      space = qsa->fragbuf_cap - qsa->fragbuf_len;
      if (!space)
      {
         qsa->backpressure_events++;
         if (qsa->backpressure_events <= 4
               || (qsa->backpressure_events % 120) == 0)
            RARCH_WARN("[QSA]: accumulator full (%u/%u), applying backpressure.\n",
                  (unsigned)qsa->fragbuf_len,
                  (unsigned)qsa->fragbuf_cap);
         break;
      }

      chunk = MIN(space, out_bytes - accepted);
      /* Never accept a partial native frame. RetroArch retries everything we
       * do not report; byte-granular acceptance would otherwise duplicate the
       * tail of a converted S16-stereo frame after a short QSA write. */
      chunk -= chunk % device_frame_bytes;
      if (!chunk)
      {
         qsa->backpressure_events++;
         break;
      }
      memcpy(qsa->fragbuf + qsa->fragbuf_len,
            (const uint8_t*)out + accepted, chunk);
      qsa->fragbuf_len += chunk;
      accepted += chunk;
   }
   qsa_flush_fragments(qsa);

   /* Everything reported here is either in QSA's ring or our accumulator.
    * Convert progress back to RetroArch's signed-S16 stereo byte domain. */
   return (ssize_t)MIN(len, qsa_device_to_input_bytes(qsa, accepted));
}

static bool qsa_stop(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (qsa)
      qsa->started = false;
   qsa_ready_clear();
   if (qsa && qsa->pcm)
   {
      snd_pcm_playback_flush(qsa->pcm);
      qsa->fragbuf_len = 0;
   }
   return true;
}

static bool qsa_start(void *data, bool is_shutdown)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   (void)is_shutdown;
   if (!qsa || !qsa->pcm)
      return false;
   if (qsa->started)
      return true;
   qsa_ready_clear();
   qsa->fragbuf_len = 0;
   /* A failed/partial prior prefill can leave silence queued even though the
    * ready marker was never published. Start every recovery from a blank ring. */
   snd_pcm_playback_flush(qsa->pcm);
   if (snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK) < 0)
   {
      RARCH_ERR("[QSA]: focus recovery prepare failed; will retry.\n");
      /* Keep RetroArch's global audio-active flag set. Java will send another
       * recovery edge because the ready marker was not published. */
      return true;
   }
   if (!qsa_prefill(qsa, QSA_RECOVERY_PREFILL))
   {
      RARCH_ERR("[QSA]: focus recovery prefill failed; will retry.\n");
      snd_pcm_playback_flush(qsa->pcm);
      return true;
   }
   qsa->started = true;
   if (!qsa_ready_set())
   {
      qsa->started = false;
      snd_pcm_playback_flush(qsa->pcm);
      RARCH_ERR("[QSA]: focus recovery handshake failed; will retry.\n");
      return true;
   }
   return true;
}

static bool qsa_alive(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   return qsa && qsa->started;
}

static void qsa_set_nonblock_state(void *data, bool toggle)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa || !qsa->pcm)
      return;
   qsa->nonblock = toggle;
   snd_pcm_nonblock_mode(qsa->pcm, toggle ? 1 : 0);
}

static void qsa_free(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   qsa_ready_clear();
   if (!qsa)
      return;
   if (qsa->pcm)
   {
      snd_pcm_playback_flush(qsa->pcm);
      snd_pcm_close(qsa->pcm);
   }
   free(qsa->mixbuf);
   free(qsa->fragbuf);
   RARCH_LOG("[QSA]: closed (underruns=%u errors=%u shorts=%u backpressure=%u).\n",
         qsa->underruns, qsa->write_errors, qsa->short_writes,
         qsa->backpressure_events);
   free(qsa);
}

static bool qsa_use_float(void *data) { return false; }

static size_t qsa_write_avail(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   snd_pcm_channel_status_t st;
   size_t device_avail = 0;
   size_t local_avail;

   if (!qsa || !qsa->pcm || !qsa->started)
      return 0;
   memset(&st, 0, sizeof(st));
   st.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_status(qsa->pcm, &st) == 0 && st.free > 0)
      device_avail = (size_t)st.free;
   local_avail = qsa->fragbuf_cap - qsa->fragbuf_len;
   return qsa_device_to_input_bytes(qsa, device_avail + local_avail);
}

static size_t qsa_buffer_size(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa)
      return 0;
   return qsa_device_to_input_bytes(qsa,
         (size_t)qsa->frag_size * (size_t)qsa->frags
               + qsa->fragbuf_cap);
}

audio_driver_t audio_qsa = {
   qsa_init,
   qsa_write,
   qsa_stop,
   qsa_start,
   qsa_alive,
   qsa_set_nonblock_state,
   qsa_free,
   qsa_use_float,
   "qsa",
   NULL, /* device_list_new  */
   NULL, /* device_list_free */
   qsa_write_avail,
   qsa_buffer_size,
   NULL  /* write_raw */
};
