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
 * state through the stock HMIAudioService — see the README audio section.
 *
 * Default device is `/dev/snd/mpl1_int_ent` = VIRTUALCHANNEL_ENT_INTMEDIA, the
 * internal-media entertainment channel (semantically what an emulator is, and
 * definitely ACDB-provisioned). Note it is a **6-channel** device (mpl5_dio_ent is
 * 2ch), so stereo is upmixed to the device's native voice count here.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/asoundlib.h>

#include <boolean.h>
#include <retro_miscellaneous.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

#define QSA_DEFAULT_DEVICE "/dev/snd/mpl1_int_ent"
#define QSA_PREFILL_FRAGS  2

typedef struct qsa_audio
{
   snd_pcm_t *pcm;
   uint8_t   *mixbuf;        /* S16 stereo -> native format/N-voice scratch */
   size_t     mixbuf_frames;
   int        frag_size;     /* bytes */
   int        frags;         /* actual hardware fragment count */
   int        voices;        /* device native channel count (6 on mpl1) */
   int        format;        /* SND_PCM_SFMT_* */
   int        sample_bytes;  /* 2 for S16, 4 for S32 */
   bool       swap_endian;
   unsigned   rate;
   bool       nonblock;
   bool       started;
} qsa_audio_t;

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

/* After an underrun the channel sits in UNDERRUN/READY and refuses writes until
 * it is prepared again. Re-prime it with a little silence so we don't
 * immediately underrun on the next fragment. */
static void qsa_recover(qsa_audio_t *qsa)
{
   snd_pcm_channel_status_t st;

   memset(&st, 0, sizeof(st));
   st.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_status(qsa->pcm, &st) < 0)
      return;

   if (     st.status == SND_PCM_STATUS_UNDERRUN
         || st.status == SND_PCM_STATUS_READY)
   {
      int i;
      void *silence;

      snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK);

      if (qsa->frag_size <= 0)
         return;
      if (!(silence = calloc(1, qsa->frag_size)))
         return;
      for (i = 0; i < QSA_PREFILL_FRAGS; i++)
         if (snd_pcm_write(qsa->pcm, silence, qsa->frag_size) != qsa->frag_size)
            break;
      free(silence);
   }
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
    * Four fragments is the floor; the MHI2Q needs more headroom than desktop
    * ALSA to avoid CSD start/stop cycling when the render thread jitters. */
   if (!latency)
      latency = 64;
   target_bytes = ((size_t)hw_rate * latency / 1000) * (size_t)frame_bytes;
   requested_frags = (int)((target_bytes + qsa->frag_size - 1)
         / (size_t)qsa->frag_size);
   if (requested_frags < 4)
      requested_frags = 4;
   if (requested_frags > 16)
      requested_frags = 16;
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
   params.buf.block.frags_min    = MIN(4, qsa->frags);
   params.buf.block.frags_max    = qsa->frags;

   if (snd_pcm_channel_params(qsa->pcm, &params) < 0)
   {
      RARCH_ERR("[QSA]: snd_pcm_channel_params failed (rate %u, %d voices).\n",
            hw_rate, qsa->voices);
      snd_pcm_close(qsa->pcm);
      free(qsa);
      return NULL;
   }

   snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK);

   memset(&setup, 0, sizeof(setup));
   setup.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_setup(qsa->pcm, &setup) == 0)
   {
      qsa->frag_size = setup.buf.block.frag_size;
      if (setup.buf.block.frags > 0)
         qsa->frags = setup.buf.block.frags;
      if (setup.format.rate > 0)
         qsa->rate = setup.format.rate;
   }
   if (qsa->frag_size <= 0)
      qsa->frag_size = 4096;
   if (!qsa->rate)
      qsa->rate = hw_rate;

   if (new_rate)
      *new_rate = qsa->rate;

   RARCH_LOG("[QSA]: %u Hz, %d voices, S%d%s, %d x %d-byte fragments.\n",
         qsa->rate, qsa->voices, qsa->sample_bytes * 8,
         qsa->swap_endian ? " BE" : " LE", qsa->frags, qsa->frag_size);

   qsa->started = true;

   return qsa;
}

static ssize_t qsa_write(void *data, const void *s, size_t len)
{
   qsa_audio_t   *qsa    = (qsa_audio_t*)data;
   const int16_t *in     = (const int16_t*)s;
   size_t frames         = len / (2 * sizeof(int16_t));   /* incoming stereo */
   const void *out       = s;
   size_t out_bytes      = len;
   int written;

   if (!qsa || !qsa->pcm || !qsa->started || !frames)
      return 0;

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

   written = snd_pcm_write(qsa->pcm, out, (int)out_bytes);
   if (written < 0)
   {
      qsa_recover(qsa);
      return 0;
   }
   if (!written)
      return 0;

   /* Report actual progress in the caller's signed-16 stereo byte domain.
    * RetroArch will retry the remainder after a short/nonblocking write. */
   return (ssize_t)MIN(len,
         qsa_device_to_input_bytes(qsa, (size_t)written));
}

static bool qsa_stop(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (qsa && qsa->pcm)
   {
      snd_pcm_playback_flush(qsa->pcm);
      snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK);
   }
   if (qsa)
      qsa->started = false;
   return true;
}

static bool qsa_start(void *data, bool is_shutdown)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   (void)is_shutdown;
   if (qsa && qsa->pcm)
      snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK);
   if (qsa)
      qsa->started = true;
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
   if (!qsa)
      return;
   if (qsa->pcm)
   {
      snd_pcm_playback_flush(qsa->pcm);
      snd_pcm_close(qsa->pcm);
   }
   free(qsa->mixbuf);
   free(qsa);
}

static bool qsa_use_float(void *data) { return false; }

static size_t qsa_write_avail(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   snd_pcm_channel_status_t st;

   if (!qsa || !qsa->pcm)
      return 0;
   memset(&st, 0, sizeof(st));
   st.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_status(qsa->pcm, &st) < 0)
      return 0;
   if (st.free <= 0)
      return 0;
   return qsa_device_to_input_bytes(qsa, (size_t)st.free);
}

static size_t qsa_buffer_size(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa)
      return 0;
   return qsa_device_to_input_bytes(qsa,
         (size_t)qsa->frag_size * (size_t)qsa->frags);
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
