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
#include <time.h>
#include <unistd.h>

#include <sys/asoundlib.h>
#ifdef __QNX__
#include <sys/neutrino.h>
#endif
#include <boolean.h>
#include <queues/fifo_queue.h>
#include <rthreads/rthreads.h>
#include <retro_miscellaneous.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

#define QSA_DEFAULT_DEVICE "/dev/snd/mpl1_int_ent"
#define QSA_READY_DEFAULT  "/tmp/retroarch.pcm.ready"
#define QSA_RING_MIN_FRAGS 8
#define QSA_RING_MAX_FRAGS 16
#define QSA_INITIAL_PREFILL_MAX 10
#define QSA_RECOVERY_PREFILL 8
#define QSA_SOFTWARE_QUEUE_FRAGS 16
#define QSA_STARVATION_POLL_US 4000
#define QSA_HW_LOW_WATER_FRAGS 2
#define QSA_CONCEAL_FADE_MS 5

typedef struct qsa_audio
{
   snd_pcm_t *pcm;
   uint8_t   *mixbuf;        /* S16 stereo -> native format/N-voice scratch */
   size_t     mixbuf_frames;
   fifo_buffer_t *fifo;      /* real-PCM delay/reserve in native bytes */
   uint8_t   *fragbuf;       /* worker's one-fragment steady-state buffer */
   uint8_t   *gapbuf;        /* click-free last-sample -> silence fragment */
   uint8_t   *burstbuf;      /* tightly submitted startup/recovery PCM */
   size_t     burstbuf_cap;
   size_t     fifo_cap;
   slock_t   *fifo_lock;
   slock_t   *pcm_lock;
   scond_t   *fifo_readable;
   scond_t   *fifo_writable;
   sthread_t *worker;
   int        frag_size;     /* bytes */
   int        frags;         /* actual hardware fragment count */
   int        voices;        /* device native channel count (6 on mpl1) */
   int        format;        /* SND_PCM_SFMT_* */
   int        sample_bytes;  /* 2 for S16, 4 for S32 */
   bool       swap_endian;
   unsigned   rate;
   bool       nonblock;
   volatile bool running;
   volatile bool started;
   volatile bool primed;
   unsigned   underruns;
   unsigned   write_errors;
   unsigned   short_writes;
   unsigned   backpressure_events;
   unsigned   rate_control_queries;
   volatile unsigned producer_bytes;
   volatile unsigned worker_fragments;
   volatile unsigned producer_calls;
   volatile unsigned producer_idle_max_us;
   volatile unsigned producer_write_max_us;
   volatile unsigned producer_input_max_bytes;
   volatile unsigned concealment_fragments;
   unsigned   concealment_events;
   bool       concealing;
   uint64_t   producer_last_return_ns;
   unsigned   producer_bytes_logged;
   unsigned   worker_fragments_logged;
   unsigned   producer_calls_logged;
   unsigned   concealment_fragments_logged;
} qsa_audio_t;

static void qsa_worker_loop(void *data);

static uint64_t qsa_monotonic_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static unsigned qsa_elapsed_us(uint64_t then_ns, uint64_t now_ns)
{
   uint64_t elapsed_us;
   if (!then_ns || now_ns <= then_ns)
      return 0;
   elapsed_us = (now_ns - then_ns) / 1000ULL;
   return elapsed_us > (uint64_t)UINT32_MAX
      ? UINT32_MAX : (unsigned)elapsed_us;
}

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

static int32_t qsa_load_native_sample(const qsa_audio_t *qsa,
      const uint8_t *src)
{
   if (qsa->sample_bytes == 2)
   {
      uint16_t v;
      memcpy(&v, src, sizeof(v));
      if (qsa->swap_endian)
         v = qsa_bswap16(v);
      return (int32_t)(int16_t)v;
   }
   else
   {
      uint32_t v;
      memcpy(&v, src, sizeof(v));
      if (qsa->swap_endian)
         v = qsa_bswap32(v);
      return (int32_t)v;
   }
}

static void qsa_store_native_sample(const qsa_audio_t *qsa, uint8_t *dst,
      int32_t sample)
{
   if (qsa->sample_bytes == 2)
   {
      uint16_t v = (uint16_t)(int16_t)sample;
      if (qsa->swap_endian)
         v = qsa_bswap16(v);
      memcpy(dst, &v, sizeof(v));
   }
   else
   {
      uint32_t v = (uint32_t)sample;
      if (qsa->swap_endian)
         v = qsa_bswap32(v);
      memcpy(dst, &v, sizeof(v));
   }
}

/* If a core stops producing for longer than the 160 ms hardware ring, keep
 * the QSA channel running rather than entering its expensive UNDERRUN state.
 * The first synthetic fragment ramps the final real sample of every voice to
 * zero over 5 ms; later fragments are silence. This cannot invent audio that
 * the stalled core did not produce, but it avoids both a click and the
 * prepare/re-prime freeze. */
static void qsa_prepare_gap_fragment(qsa_audio_t *qsa,
      const uint8_t *last_real_fragment)
{
   size_t frame_bytes = (size_t)qsa->voices * (size_t)qsa->sample_bytes;
   size_t total_frames = (size_t)qsa->frag_size / frame_bytes;
   size_t fade_frames = (size_t)qsa->rate * QSA_CONCEAL_FADE_MS / 1000;
   const uint8_t *last_frame;
   size_t frame;
   int voice;

   memset(qsa->gapbuf, 0, (size_t)qsa->frag_size);
   if (!last_real_fragment || !total_frames)
      return;
   if (!fade_frames || fade_frames > total_frames)
      fade_frames = total_frames;
   last_frame = last_real_fragment + (total_frames - 1) * frame_bytes;

   for (frame = 0; frame < fade_frames; frame++)
   {
      int64_t numerator = (int64_t)(fade_frames - frame);
      for (voice = 0; voice < qsa->voices; voice++)
      {
         const uint8_t *src = last_frame +
               (size_t)voice * (size_t)qsa->sample_bytes;
         uint8_t *dst = qsa->gapbuf + frame * frame_bytes +
               (size_t)voice * (size_t)qsa->sample_bytes;
         int64_t sample = qsa_load_native_sample(qsa, src);
         qsa_store_native_sample(qsa, dst,
               (int32_t)(sample * numerator / (int64_t)fade_frames));
      }
   }
}

/* Resume real PCM from concealment without a zero-to-waveform discontinuity.
 * The fade is applied to a private worker fragment after it leaves the FIFO,
 * so queued source audio and RetroArch's accounting remain unchanged. */
static void qsa_fade_in_fragment(qsa_audio_t *qsa, uint8_t *fragment)
{
   size_t frame_bytes = (size_t)qsa->voices * (size_t)qsa->sample_bytes;
   size_t total_frames = (size_t)qsa->frag_size / frame_bytes;
   size_t fade_frames = (size_t)qsa->rate * QSA_CONCEAL_FADE_MS / 1000;
   size_t frame;
   int voice;

   if (!fragment || !total_frames)
      return;
   if (!fade_frames || fade_frames > total_frames)
      fade_frames = total_frames;

   for (frame = 0; frame < fade_frames; frame++)
   {
      int64_t numerator = (int64_t)(frame + 1);
      for (voice = 0; voice < qsa->voices; voice++)
      {
         uint8_t *sample_ptr = fragment + frame * frame_bytes +
               (size_t)voice * (size_t)qsa->sample_bytes;
         int64_t sample = qsa_load_native_sample(qsa, sample_ptr);
         qsa_store_native_sample(qsa, sample_ptr,
               (int32_t)(sample * numerator / (int64_t)fade_frames));
      }
   }
}

/* Submit exactly one complete fragment without entering recovery recursively.
 * Only the worker touches PCM during playback. pcm_lock lets stop/start flush
 * or prepare the channel after the current (at most one-fragment) blocking
 * write, without racing io-audio. */
static bool qsa_write_fragment_raw(qsa_audio_t *qsa, const uint8_t *fragment)
{
   size_t offset = 0;
   int retries   = 0;

   while (offset < (size_t)qsa->frag_size)
   {
      int written;

      if (!qsa->running || !qsa->started)
         return false;
      slock_lock(qsa->pcm_lock);
      if (!qsa->running || !qsa->started)
      {
         slock_unlock(qsa->pcm_lock);
         return false;
      }
      written = snd_pcm_write(qsa->pcm, fragment + offset,
            (int)((size_t)qsa->frag_size - offset));
      slock_unlock(qsa->pcm_lock);
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
   return offset == (size_t)qsa->frag_size;
}

/* Copy an exact amount from the real-PCM queue. Playback may be stopped while
 * this waits (startup/recovery), but the producer remains free to fill the
 * queue. This is the property the old same-thread design could not provide. */
static bool qsa_worker_take(qsa_audio_t *qsa, uint8_t *dst, size_t bytes,
      size_t min_ready)
{
   if (min_ready < bytes)
      min_ready = bytes;
   if (min_ready > qsa->fifo_cap)
      min_ready = qsa->fifo_cap;

   slock_lock(qsa->fifo_lock);
   while (qsa->running && qsa->started
         && FIFO_READ_AVAIL(qsa->fifo) < min_ready)
      scond_wait(qsa->fifo_readable, qsa->fifo_lock);

   if (!qsa->running || !qsa->started)
   {
      slock_unlock(qsa->fifo_lock);
      return false;
   }

   fifo_read(qsa->fifo, dst, bytes);
   scond_signal(qsa->fifo_writable);
   slock_unlock(qsa->fifo_lock);
   return true;
}

/* Last-resort recovery. Normally qsa_worker_take_steady() prevents UNDERRUN by
 * feeding a low-water concealment fragment before the hardware ring empties.
 * If QSA still reports UNDERRUN/READY, never wait for the software FIFO here:
 * take whatever real PCM already exists and pad a complete eight-fragment
 * burst with the click-free gap followed by silence. The full burst avoids the
 * old shallow prepare/write/underrun loop, while eliminating the former
 * 128-256 ms wait before prepare. */
static bool qsa_recover(qsa_audio_t *qsa, const uint8_t *failed_fragment,
      bool failed_is_concealment)
{
   snd_pcm_channel_status_t st;
   size_t queued = 0;
   size_t real_bytes = 0;
   size_t wanted_bytes;
   int target;
   int fifo_real_fragments;
   int real_fragments;
   int synthetic_fragments;
   int i;

   slock_lock(qsa->pcm_lock);
   memset(&st, 0, sizeof(st));
   st.channel = SND_PCM_CHANNEL_PLAYBACK;
   if (snd_pcm_channel_status(qsa->pcm, &st) < 0)
   {
      slock_unlock(qsa->pcm_lock);
      return false;
   }
   slock_unlock(qsa->pcm_lock);

   if (     st.status == SND_PCM_STATUS_UNDERRUN
         || st.status == SND_PCM_STATUS_READY)
   {
      target = MIN(qsa->frags, QSA_RECOVERY_PREFILL);
      if (target < 2)
         target = 2;

      qsa->underruns++;
      slock_lock(qsa->fifo_lock);
      queued = FIFO_READ_AVAIL(qsa->fifo);
      slock_unlock(qsa->fifo_lock);
      if (qsa->underruns <= 8 || (qsa->underruns % 60) == 0)
         RARCH_WARN("[QSA]: recovering underrun #%u (status=%d free=%u, "
               "queued=%u fragments); immediate %d-fragment burst.\n",
               qsa->underruns, (int)st.status, (unsigned)st.free,
               (unsigned)(queued / (size_t)qsa->frag_size), target);

      wanted_bytes = (size_t)(target - 1) * (size_t)qsa->frag_size;
      slock_lock(qsa->fifo_lock);
      if (!qsa->running || !qsa->started)
      {
         slock_unlock(qsa->fifo_lock);
         return false;
      }
      real_bytes = MIN(FIFO_READ_AVAIL(qsa->fifo), wanted_bytes);
      real_bytes -= real_bytes % (size_t)qsa->frag_size;
      fifo_real_fragments = (int)(real_bytes / (size_t)qsa->frag_size);

      if (failed_is_concealment)
      {
         /* Put the synthetic gap first and any newly available real PCM at
          * the end of the burst. Playback can then continue with real PCM
          * instead of playing real data followed by another silence gap. */
         synthetic_fragments = target - fifo_real_fragments;
         memcpy(qsa->burstbuf, failed_fragment, (size_t)qsa->frag_size);
         for (i = 1; i < synthetic_fragments; i++)
            memset(qsa->burstbuf + (size_t)i * (size_t)qsa->frag_size,
                  0, (size_t)qsa->frag_size);
         if (real_bytes)
            fifo_read(qsa->fifo, qsa->burstbuf +
                  (size_t)synthetic_fragments * (size_t)qsa->frag_size,
                  real_bytes);
         real_fragments = fifo_real_fragments;
      }
      else
      {
         memcpy(qsa->burstbuf, failed_fragment, (size_t)qsa->frag_size);
         if (real_bytes)
            fifo_read(qsa->fifo, qsa->burstbuf + qsa->frag_size, real_bytes);
         real_fragments = 1 + fifo_real_fragments;
         synthetic_fragments = target - real_fragments;
         for (i = real_fragments; i < target; i++)
         {
            uint8_t *dst = qsa->burstbuf +
                  (size_t)i * (size_t)qsa->frag_size;
            if (i == real_fragments && !qsa->concealing)
               memcpy(dst, qsa->gapbuf, (size_t)qsa->frag_size);
            else
               memset(dst, 0, (size_t)qsa->frag_size);
         }
      }
      if (real_bytes)
         scond_signal(qsa->fifo_writable);
      slock_unlock(qsa->fifo_lock);

      if (failed_is_concealment && real_fragments > 0)
         qsa_fade_in_fragment(qsa, qsa->burstbuf +
               (size_t)synthetic_fragments * (size_t)qsa->frag_size);

      if (synthetic_fragments > 0 && !failed_is_concealment)
         qsa->concealing = true;
      else if (real_fragments > 0)
      {
         qsa_prepare_gap_fragment(qsa, qsa->burstbuf +
               (size_t)(target - 1) * (size_t)qsa->frag_size);
         qsa->concealing = false;
      }

      slock_lock(qsa->pcm_lock);
      if (!qsa->running || !qsa->started
            || snd_pcm_channel_prepare(qsa->pcm,
                  SND_PCM_CHANNEL_PLAYBACK) < 0)
      {
         slock_unlock(qsa->pcm_lock);
         return false;
      }
      slock_unlock(qsa->pcm_lock);

      for (i = 0; i < target; i++)
         if (!qsa_write_fragment_raw(qsa,
                  qsa->burstbuf + (size_t)i * (size_t)qsa->frag_size))
            goto failed;
      __sync_add_and_fetch(&qsa->worker_fragments, (unsigned)target);

      qsa->primed = true;
      if (qsa->underruns <= 8 || (qsa->underruns % 60) == 0)
         RARCH_LOG("[QSA]: recovered immediately with %d real + %d "
               "concealment fragments.\n", real_fragments,
               target - real_fragments);
      return true;

failed:
      slock_lock(qsa->pcm_lock);
      snd_pcm_playback_flush(qsa->pcm);
      slock_unlock(qsa->pcm_lock);
      return false;
   }
   return false;
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

   /* The worker continuously transfers a deep real-PCM reserve into QSA while
    * the core/GL thread is briefly stalled. The producer blocks when this
    * queue fills, so the card still becomes RetroArch's clock; unlike the old
    * async driver, DRC observes this queue directly instead of adding empty
    * hardware-ring space and masking starvation. */
   qsa->fifo_cap      = (size_t)qsa->frag_size * QSA_SOFTWARE_QUEUE_FRAGS;
   qsa->burstbuf_cap  = (size_t)qsa->frag_size * (size_t)qsa->frags;
   qsa->fifo          = fifo_new(qsa->fifo_cap);
   qsa->fragbuf       = (uint8_t*)malloc((size_t)qsa->frag_size);
   qsa->gapbuf        = (uint8_t*)calloc(1, (size_t)qsa->frag_size);
   qsa->burstbuf      = (uint8_t*)malloc(qsa->burstbuf_cap);
   qsa->fifo_lock     = slock_new();
   qsa->pcm_lock      = slock_new();
   qsa->fifo_readable = scond_new();
   qsa->fifo_writable = scond_new();
   if (!qsa->fifo || !qsa->fragbuf || !qsa->gapbuf || !qsa->burstbuf
         || !qsa->fifo_lock || !qsa->pcm_lock
         || !qsa->fifo_readable || !qsa->fifo_writable)
   {
      RARCH_ERR("[QSA]: cannot allocate the %u-byte worker queue.\n",
            (unsigned)qsa->fifo_cap);
      snd_pcm_close(qsa->pcm);
      fifo_free(qsa->fifo);
      free(qsa->fragbuf);
      free(qsa->gapbuf);
      free(qsa->burstbuf);
      scond_free(qsa->fifo_readable);
      scond_free(qsa->fifo_writable);
      slock_free(qsa->fifo_lock);
      slock_free(qsa->pcm_lock);
      free(qsa);
      return NULL;
   }
   qsa->running = true;
   qsa->started = true;
   qsa->primed  = false;
   qsa->worker  = sthread_create(qsa_worker_loop, qsa);
   if (!qsa->worker)
   {
      RARCH_ERR("[QSA]: cannot start PCM worker.\n");
      qsa->running = false;
      snd_pcm_close(qsa->pcm);
      fifo_free(qsa->fifo);
      free(qsa->fragbuf);
      free(qsa->gapbuf);
      free(qsa->burstbuf);
      scond_free(qsa->fifo_readable);
      scond_free(qsa->fifo_writable);
      slock_free(qsa->fifo_lock);
      slock_free(qsa->pcm_lock);
      free(qsa);
      return NULL;
   }
   if (!qsa_ready_set())
   {
      qsa->started = false;
      qsa->running = false;
      scond_signal(qsa->fifo_readable);
      sthread_join(qsa->worker);
      RARCH_ERR("[QSA]: initial PCM handshake failed.\n");
      snd_pcm_close(qsa->pcm);
      fifo_free(qsa->fifo);
      free(qsa->fragbuf);
      free(qsa->gapbuf);
      free(qsa->burstbuf);
      scond_free(qsa->fifo_readable);
      scond_free(qsa->fifo_writable);
      slock_free(qsa->fifo_lock);
      slock_free(qsa->pcm_lock);
      free(qsa);
      return NULL;
   }

   RARCH_LOG("[QSA]: hybrid PCM: %d-fragment real-audio reserve + blocking "
         "worker; DRC tracks the reserve only.\n", QSA_SOFTWARE_QUEUE_FRAGS);
   RARCH_LOG("[QSA]: PCM ready marker published; HMI may fade in connection 20.\n");

   return qsa;
}

static bool qsa_worker_submit(qsa_audio_t *qsa, const uint8_t *fragment,
      bool is_concealment)
{
   if (qsa_write_fragment_raw(qsa, fragment))
   {
      __sync_add_and_fetch(&qsa->worker_fragments, 1u);
      return true;
   }
   return qsa->running && qsa->started
      && qsa_recover(qsa, fragment, is_concealment);
}

/* Do not start START_DATA with silence or a shallow queue. Wait until a full
 * real-audio burst exists, remove it atomically, then submit it tightly. */
static bool qsa_worker_prime(qsa_audio_t *qsa)
{
   int target = MIN(qsa->frags, QSA_INITIAL_PREFILL_MAX);
   int attempt;
   int i;

   if (!qsa_worker_take(qsa, qsa->burstbuf,
            (size_t)target * (size_t)qsa->frag_size, qsa->fifo_cap))
      return false;

   for (attempt = 0; attempt < 2; attempt++)
   {
      for (i = 0; i < target; i++)
         if (!qsa_write_fragment_raw(qsa,
                  qsa->burstbuf + (size_t)i * (size_t)qsa->frag_size))
            break;
      if (i == target)
      {
         __sync_add_and_fetch(&qsa->worker_fragments, (unsigned)target);
         qsa_prepare_gap_fragment(qsa, qsa->burstbuf +
               (size_t)(target - 1) * (size_t)qsa->frag_size);
         qsa->concealing = false;
         qsa->primed = true;
         RARCH_LOG("[QSA]: worker started with %d/%d real PCM fragments.\n",
               target, target);
         return true;
      }

      slock_lock(qsa->pcm_lock);
      snd_pcm_playback_flush(qsa->pcm);
      if (qsa->running && qsa->started)
         snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK);
      slock_unlock(qsa->pcm_lock);
   }

   return false;
}

/* Wait for real PCM while there is still plenty of data in the hardware ring.
 * Once only QSA_HW_LOW_WATER_FRAGS remain, submit exactly one concealment
 * fragment. Re-checking status after every fragment prevents us from filling
 * the ring with silence just as the producer wakes up. */
static bool qsa_worker_take_steady(qsa_audio_t *qsa, uint8_t *dst,
      bool *is_concealment)
{
   size_t ring_bytes = (size_t)qsa->frags * (size_t)qsa->frag_size;
   size_t low_water_free = ring_bytes >
         (size_t)QSA_HW_LOW_WATER_FRAGS * (size_t)qsa->frag_size
         ? ring_bytes - (size_t)QSA_HW_LOW_WATER_FRAGS *
               (size_t)qsa->frag_size : 0;

   *is_concealment = false;
   while (qsa->running && qsa->started)
   {
      bool timed_out = false;
      snd_pcm_channel_status_t st;

      slock_lock(qsa->fifo_lock);
      if (FIFO_READ_AVAIL(qsa->fifo) >= (size_t)qsa->frag_size)
      {
         bool resume_from_concealment = qsa->concealing;
         fifo_read(qsa->fifo, dst, (size_t)qsa->frag_size);
         scond_signal(qsa->fifo_writable);
         slock_unlock(qsa->fifo_lock);
         if (resume_from_concealment)
            qsa_fade_in_fragment(qsa, dst);
         qsa_prepare_gap_fragment(qsa, dst);
         qsa->concealing = false;
         return true;
      }
      if (!qsa->running || !qsa->started)
      {
         slock_unlock(qsa->fifo_lock);
         return false;
      }
      timed_out = !scond_wait_timeout(qsa->fifo_readable, qsa->fifo_lock,
            QSA_STARVATION_POLL_US);
      /* Close the timeout/signal race while the producer is still excluded. */
      if (FIFO_READ_AVAIL(qsa->fifo) >= (size_t)qsa->frag_size)
      {
         bool resume_from_concealment = qsa->concealing;
         fifo_read(qsa->fifo, dst, (size_t)qsa->frag_size);
         scond_signal(qsa->fifo_writable);
         slock_unlock(qsa->fifo_lock);
         if (resume_from_concealment)
            qsa_fade_in_fragment(qsa, dst);
         qsa_prepare_gap_fragment(qsa, dst);
         qsa->concealing = false;
         return true;
      }
      slock_unlock(qsa->fifo_lock);
      if (!timed_out)
         continue;

      memset(&st, 0, sizeof(st));
      st.channel = SND_PCM_CHANNEL_PLAYBACK;
      slock_lock(qsa->pcm_lock);
      if (!qsa->running || !qsa->started)
      {
         slock_unlock(qsa->pcm_lock);
         return false;
      }
      if (snd_pcm_channel_status(qsa->pcm, &st) < 0)
      {
         slock_unlock(qsa->pcm_lock);
         continue;
      }
      slock_unlock(qsa->pcm_lock);

      if (st.status == SND_PCM_STATUS_UNDERRUN ||
            st.status == SND_PCM_STATUS_READY ||
            (size_t)st.free >= low_water_free)
      {
         if (!qsa->concealing)
         {
            memcpy(dst, qsa->gapbuf, (size_t)qsa->frag_size);
            qsa->concealing = true;
            qsa->concealment_events++;
         }
         else
            memset(dst, 0, (size_t)qsa->frag_size);
         qsa->concealment_fragments++;
         *is_concealment = true;
         return true;
      }
   }
   return false;
}

static void qsa_worker_loop(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;

#ifdef __QNX__
   {
      /* PCSX already reserves CPU1 for CDR+SPU and CPU2 for its async GPU.
       * CPU3 carries only the intermittent dynarec compiler and gives this
       * short, periodic transport worker far more reliable service time. */
      unsigned requested_runmask = 1u << 3;
      unsigned runmask           = requested_runmask;
      if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET, &runmask) == -1)
         RARCH_WARN("[QSA]: worker CPU1 affinity failed: %s.\n",
               strerror(errno));
      else
         RARCH_LOG("[QSA]: worker pinned to CPU3 (mask 0x%x).\n",
               requested_runmask);
   }
#endif

   while (qsa->running)
   {
      bool is_concealment = false;
      if (!qsa->started)
      {
         slock_lock(qsa->fifo_lock);
         while (qsa->running && !qsa->started)
            scond_wait(qsa->fifo_readable, qsa->fifo_lock);
         slock_unlock(qsa->fifo_lock);
         continue;
      }

      if (!qsa->primed)
      {
         if (!qsa_worker_prime(qsa) && qsa->running && qsa->started)
            RARCH_ERR("[QSA]: real-PCM startup prime failed; retrying.\n");
         continue;
      }

      if (!qsa_worker_take_steady(qsa, qsa->fragbuf, &is_concealment))
         continue;
      if (!qsa_worker_submit(qsa, qsa->fragbuf, is_concealment)
            && qsa->running && qsa->started)
      {
         RARCH_ERR("[QSA]: worker PCM write failed; rebuilding reserve.\n");
         qsa->primed = false;
      }
   }
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
   uint64_t entry_ns;
   uint64_t return_ns;
   unsigned elapsed_us;

   if (!qsa || !qsa->pcm || !qsa->started || !frames)
      return 0;

   entry_ns = qsa_monotonic_ns();
   elapsed_us = qsa_elapsed_us(qsa->producer_last_return_ns, entry_ns);
   if (elapsed_us > qsa->producer_idle_max_us)
      qsa->producer_idle_max_us = elapsed_us;
   qsa->producer_calls++;
   if (len > qsa->producer_input_max_bytes)
      qsa->producer_input_max_bytes = (unsigned)MIN(len,
            (size_t)UINT32_MAX);

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
         {
            qsa->producer_last_return_ns = qsa_monotonic_ns();
            return 0;
         }
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

   /* The worker is the only PCM writer and always submits whole fragments.
    * In blocking mode, a full real-PCM reserve blocks this producer until the
    * card consumes a fragment through the worker. That restores the intended
    * audio_sync clock chain without sacrificing stall protection. */
   while (accepted < out_bytes)
   {
      size_t space;
      size_t chunk;

      slock_lock(qsa->fifo_lock);
      while (qsa->running && qsa->started
            && FIFO_WRITE_AVAIL(qsa->fifo) < device_frame_bytes
            && !qsa->nonblock)
      {
         qsa->backpressure_events++;
         scond_wait(qsa->fifo_writable, qsa->fifo_lock);
      }

      if (!qsa->running || !qsa->started)
      {
         slock_unlock(qsa->fifo_lock);
         break;
      }

      space = FIFO_WRITE_AVAIL(qsa->fifo);
      if (space < device_frame_bytes)
      {
         slock_unlock(qsa->fifo_lock);
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
         slock_unlock(qsa->fifo_lock);
         break;
      }
      fifo_write(qsa->fifo, (const uint8_t*)out + accepted, chunk);
      accepted += chunk;
      scond_signal(qsa->fifo_readable);
      slock_unlock(qsa->fifo_lock);
   }

   /* Everything reported here is either in QSA's ring or our software queue.
    * Convert progress back to RetroArch's signed-S16 stereo byte domain. */
   __sync_add_and_fetch(&qsa->producer_bytes, (unsigned)accepted);
   return_ns  = qsa_monotonic_ns();
   elapsed_us = qsa_elapsed_us(entry_ns, return_ns);
   if (elapsed_us > qsa->producer_write_max_us)
      qsa->producer_write_max_us = elapsed_us;
   qsa->producer_last_return_ns = return_ns;
   return (ssize_t)MIN(len, qsa_device_to_input_bytes(qsa, accepted));
}

static bool qsa_stop(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa)
      return true;

   qsa_ready_clear();
   slock_lock(qsa->fifo_lock);
   qsa->started = false;
   qsa->primed  = false;
   qsa->concealing = false;
   fifo_clear(qsa->fifo);
   scond_broadcast(qsa->fifo_readable);
   scond_broadcast(qsa->fifo_writable);
   slock_unlock(qsa->fifo_lock);

   /* A worker write can hold pcm_lock for at most one 16 ms fragment. */
   slock_lock(qsa->pcm_lock);
   if (qsa->pcm)
      snd_pcm_playback_flush(qsa->pcm);
   slock_unlock(qsa->pcm_lock);
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

   /* Start every focus recovery from a blank, prepared START_DATA ring. Real
    * PCM will prime it atomically once the worker reserve is ready. */
   slock_lock(qsa->pcm_lock);
   snd_pcm_playback_flush(qsa->pcm);
   if (snd_pcm_channel_prepare(qsa->pcm, SND_PCM_CHANNEL_PLAYBACK) < 0)
   {
      slock_unlock(qsa->pcm_lock);
      RARCH_ERR("[QSA]: focus recovery prepare failed; will retry.\n");
      /* Keep RetroArch's global audio-active flag set. Java will send another
       * recovery edge because the ready marker was not published. */
      return true;
   }
   slock_unlock(qsa->pcm_lock);
   if (!qsa_ready_set())
   {
      slock_lock(qsa->pcm_lock);
      snd_pcm_playback_flush(qsa->pcm);
      slock_unlock(qsa->pcm_lock);
      RARCH_ERR("[QSA]: focus recovery handshake failed; will retry.\n");
      return true;
   }
   slock_lock(qsa->fifo_lock);
   fifo_clear(qsa->fifo);
   qsa->primed  = false;
   qsa->concealing = false;
   qsa->started = true;
   scond_signal(qsa->fifo_readable);
   slock_unlock(qsa->fifo_lock);
   RARCH_LOG("[QSA]: focus restored; awaiting real-PCM startup reserve.\n");
   return true;
}

static bool qsa_alive(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa)
      return false;
   return qsa->started;
}

static void qsa_set_nonblock_state(void *data, bool toggle)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa || !qsa->pcm)
      return;
   qsa->nonblock = toggle;
}

static void qsa_free(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   qsa_ready_clear();
   if (!qsa)
      return;

   slock_lock(qsa->fifo_lock);
   qsa->started = false;
   qsa->running = false;
   qsa->primed  = false;
   scond_broadcast(qsa->fifo_readable);
   scond_broadcast(qsa->fifo_writable);
   slock_unlock(qsa->fifo_lock);

   slock_lock(qsa->pcm_lock);
   if (qsa->pcm)
      snd_pcm_playback_flush(qsa->pcm);
   slock_unlock(qsa->pcm_lock);

   if (qsa->worker)
      sthread_join(qsa->worker);

   if (qsa->pcm)
      snd_pcm_close(qsa->pcm);
   free(qsa->mixbuf);
   fifo_free(qsa->fifo);
   free(qsa->fragbuf);
   free(qsa->gapbuf);
   free(qsa->burstbuf);
   scond_free(qsa->fifo_readable);
   scond_free(qsa->fifo_writable);
   slock_free(qsa->fifo_lock);
   slock_free(qsa->pcm_lock);
   RARCH_LOG("[QSA]: closed (underruns=%u conceal=%u/%u errors=%u shorts=%u "
         "producer_waits=%u).\n",
         qsa->underruns, qsa->concealment_events,
         qsa->concealment_fragments, qsa->write_errors, qsa->short_writes,
         qsa->backpressure_events);
   free(qsa);
}

static bool qsa_use_float(void *data) { return false; }

static size_t qsa_write_avail(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   size_t local_avail = 0;

   if (!qsa || !qsa->pcm || !qsa->fifo_lock)
      return 0;

   slock_lock(qsa->fifo_lock);
   if (qsa->started)
      local_avail = FIFO_WRITE_AVAIL(qsa->fifo);
   slock_unlock(qsa->fifo_lock);
   return qsa_device_to_input_bytes(qsa, local_avail);
}

static size_t qsa_buffer_size(void *data)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   if (!qsa)
      return 0;
   return qsa_device_to_input_bytes(qsa, qsa->fifo_cap);
}

/* DRC controls the elastic queue between the emulation thread and the blocking
 * QSA worker. The hardware ring is deliberately excluded: adding its free
 * space hid reserve starvation in the original async implementation. */
static bool qsa_rate_control_state(void *data, size_t *avail,
      size_t *buffer_size)
{
   qsa_audio_t *qsa = (qsa_audio_t*)data;
   size_t local_avail = 0;
   bool started       = false;

   if (!qsa || !qsa->fifo || !qsa->fifo_lock || !avail || !buffer_size)
      return false;

   *buffer_size = qsa_device_to_input_bytes(qsa, qsa->fifo_cap);
   slock_lock(qsa->fifo_lock);
   started = qsa->started;
   if (started)
      local_avail = FIFO_WRITE_AVAIL(qsa->fifo);
   slock_unlock(qsa->fifo_lock);
   if (!started)
      *avail = *buffer_size / 2; /* neutral while OEM focus has stopped PCM */
   else
   {
      /* Keep the elastic reserve deliberately near full. Generic RetroArch
       * DRC considers half of buffer_size neutral. Adding half a queue to the
       * real free-space sample moves that neutral point to FIFO-full while
       * preserving the correct sign: as reserve drains, the resampler makes
       * progressively more output. Clamp at the normal callback range. This
       * gives a slow core enough standing PCM to cover 50-70 ms CD/GPU stalls
       * instead of converging at a fragile half-empty queue. */
      size_t control_avail = MIN(qsa->fifo_cap,
            local_avail + qsa->fifo_cap / 2);
      *avail = qsa_device_to_input_bytes(qsa, control_avail);
      qsa->rate_control_queries++;
      if ((qsa->rate_control_queries % 300) == 0)
      {
         unsigned produced = qsa->producer_bytes;
         unsigned written  = qsa->worker_fragments;
         unsigned calls    = qsa->producer_calls;
         RARCH_LOG("[QSA]: DRC reserve=%u/%u fragments (free=%u), "
               "production=%u worker=%u fragments/window; "
               "calls=%u conceal=%u idle_max=%u.%03u ms write_max=%u.%03u ms "
               "input_max=%u frames.\n",
               (unsigned)((qsa->fifo_cap - local_avail) /
                     (size_t)qsa->frag_size), QSA_SOFTWARE_QUEUE_FRAGS,
               (unsigned)(local_avail / (size_t)qsa->frag_size),
               (produced - qsa->producer_bytes_logged) /
                     (unsigned)qsa->frag_size,
               written - qsa->worker_fragments_logged,
               calls - qsa->producer_calls_logged,
               qsa->concealment_fragments -
                     qsa->concealment_fragments_logged,
               qsa->producer_idle_max_us / 1000,
               qsa->producer_idle_max_us % 1000,
               qsa->producer_write_max_us / 1000,
               qsa->producer_write_max_us % 1000,
               qsa->producer_input_max_bytes /
                     (unsigned)(2 * sizeof(int16_t)));
         qsa->producer_bytes_logged     = produced;
         qsa->worker_fragments_logged   = written;
         qsa->producer_calls_logged     = calls;
         qsa->concealment_fragments_logged = qsa->concealment_fragments;
         qsa->producer_idle_max_us      = 0;
         qsa->producer_write_max_us     = 0;
         qsa->producer_input_max_bytes  = 0;
      }
   }
   return *buffer_size > 0;
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
   NULL, /* write_raw */
   qsa_rate_control_state
};
