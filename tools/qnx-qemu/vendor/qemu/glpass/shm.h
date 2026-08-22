/*
 * Shared-memory SPSC transport — the high-bandwidth path that makes a full GUI
 * feasible on QEMU WITHOUT any virtio/PCI driver in QNX 6.5.
 *
 * How it works end-to-end:
 *   QEMU: -object memory-backend-file,id=m,size=...,
 *         mem-path=build/render_work/runtime/qnx.ram,share=on
 *         -machine virt,memory-backend=m
 *     => the ENTIRE guest RAM is an mmap'd file on the host.
 *   Guest (QNX): the shim maps a reserved physical page with mmap_device_memory()
 *     and writes GL command frames into a ring there — ordinary memory stores.
 *   Host (macOS): mmaps that project-local RAM file and finds the same ring at
 *     (ring_phys - ram_base),
 *     and consumes frames at memory speed. No kernel driver on either side.
 *
 * Two SPSC byte rings in one region:
 *   c2h  guest -> host  (GL command frames)
 *   h2c  host  -> guest (round-trip replies)
 * Frame format identical to the socket path: [u16 op][u32 len][payload].
 * Sync is pure polling on head/tail indices + a full barrier; no interrupts,
 * so nothing platform-specific is required.
 *
 * This same file compiles under clang (macOS host + loopback test) and qcc
 * (QNX guest); only the mapping of the region differs (file mmap vs
 * mmap_device_memory), handled by the caller.
 */
#ifndef GLPASS_SHM_H
#define GLPASS_SHM_H

#include <stdint.h>
#include <string.h>

#define SHM_MAGIC    0x47534852u  /* "GSHR" */
#define SHM_VERSION  1u
#define SHM_RING     (1u << 22)   /* 4 MiB per direction (carries full HMI textures) */
/* A frame may be larger than the ring: shm_put()/shm_get() stream it while the
 * peer advances the opposite cursor. Real Kanzi VFD uploads reach 4.78 MiB. */
#define SHM_FRAME_MAX (8u << 20)

typedef struct {
    volatile uint32_t magic, version;
    volatile uint32_t host_ready, guest_ready;
    volatile uint32_t c2h_head, c2h_tail;   /* guest writes head, host writes tail */
    volatile uint32_t h2c_head, h2c_tail;   /* host writes head, guest writes tail  */
    uint32_t _pad[8];
    uint8_t  c2h[SHM_RING];
    uint8_t  h2c[SHM_RING];
} shm_region;

#define SHM_SIZE ((uint32_t)sizeof(shm_region))

static inline void shm_barrier(void) { __sync_synchronize(); }

/* Idle hint while spin-waiting for the peer. On the QNX guest under QEMU
 * -icount, a tight spin burns virtual time AND starves the other guest
 * threads (persistence/comm) sharing the vCPU -> their virtual-time timeouts
 * fire spuriously. Yielding lets those threads run while we wait for the
 * real-time host's reply. Host (macOS) keeps spinning (dedicated server). */
#if defined(__QNXNTO__) || defined(__QNX__)
#include <time.h>
/* Block (don't busy-spin) while waiting for the real-time host. Under QEMU
 * -icount a true sleep yields the vCPU so QEMU advances virtual time by exactly
 * the sleep quantum (deterministic) instead of by however many spin
 * instructions execute — and fully frees the CPU for the persistence/comm
 * threads so their virtual-time connection timeouts don't fire. */
static inline void shm_idle(void) {
    struct timespec ts = { 0, 200000 };  /* 200 us */
    nanosleep(&ts, 0);
}
#else
/* Host (macOS): originally a tight spin ("dedicated server"). But when the Mac is
 * shared (IDE + the QNX build VM + hours of TCG), a 100%-spin glpass starves the
 * qemu vCPU -> the guest boot crawls and the HMI-watchdog window gets tighter. A
 * tiny yield frees the core for qemu while the guest is mid-boot (ring empty); once
 * frames flow the ring is rarely empty so GL latency is barely affected. */
#include <time.h>
static inline void shm_idle(void) {
    struct timespec ts = { 0, 60000 };  /* 60 us */
    nanosleep(&ts, 0);
}
#endif

/* available bytes to read / free bytes to write in a ring */
static inline uint32_t shm_used(uint32_t head, uint32_t tail) { return head - tail; }
static inline uint32_t shm_free(uint32_t head, uint32_t tail) { return SHM_RING - (head - tail); }

/* spin-write n bytes into ring[base] using *head (producer) against *tail.
 * returns 0; spins until space (single producer). `abort_flag` (if non-NULL)
 * breaks the spin when it goes zero (peer died) -> returns -1. */
static inline int shm_put(uint8_t *base, volatile uint32_t *head, volatile uint32_t *tail,
                          const void *buf, uint32_t n, volatile uint32_t *abort_flag) {
    const uint8_t *p = (const uint8_t*)buf; uint32_t done = 0;
    while (done < n) {
        uint32_t h = *head, t = *tail;
        uint32_t freeb = shm_free(h, t);
        if (!freeb) { if (abort_flag && !*abort_flag) return -1; shm_idle(); continue; }
        uint32_t chunk = n - done; if (chunk > freeb) chunk = freeb;
        uint32_t off = h & (SHM_RING - 1);
        uint32_t first = SHM_RING - off; if (first > chunk) first = chunk;
        memcpy(base + off, p + done, first);
        if (chunk > first) memcpy(base, p + done + first, chunk - first);
        shm_barrier();
        *head = h + chunk;
        done += chunk;
    }
    return 0;
}

/* spin-read n bytes from ring[base] using *head (against *tail consumer) */
static inline int shm_get(uint8_t *base, volatile uint32_t *head, volatile uint32_t *tail,
                          void *buf, uint32_t n, volatile uint32_t *abort_flag) {
    uint8_t *p = (uint8_t*)buf; uint32_t done = 0;
    while (done < n) {
        uint32_t h = *head, t = *tail;
        uint32_t avail = shm_used(h, t);
        if (!avail) { if (abort_flag && !*abort_flag) return -1; shm_idle(); continue; }
        uint32_t chunk = n - done; if (chunk > avail) chunk = avail;
        uint32_t off = t & (SHM_RING - 1);
        uint32_t first = SHM_RING - off; if (first > chunk) first = chunk;
        memcpy(p + done, base + off, first);
        if (chunk > first) memcpy(p + done + first, base, chunk - first);
        shm_barrier();
        *tail = t + chunk;
        done += chunk;
    }
    return 0;
}

/* frame helpers: [u16 op][u32 len][payload] over a ring direction */
static inline int shm_put_frame(uint8_t *base, volatile uint32_t *head, volatile uint32_t *tail,
                                uint16_t op, const void *payload, uint32_t len, volatile uint32_t *ab) {
    uint8_t hdr[6];
    hdr[0]=op; hdr[1]=op>>8; hdr[2]=len; hdr[3]=len>>8; hdr[4]=len>>16; hdr[5]=len>>24;
    if (shm_put(base, head, tail, hdr, 6, ab) < 0) return -1;
    if (len && shm_put(base, head, tail, payload, len, ab) < 0) return -1;
    return 0;
}
static inline int shm_get_frame(uint8_t *base, volatile uint32_t *head, volatile uint32_t *tail,
                                uint16_t *op, void *buf, uint32_t cap, uint32_t *len, volatile uint32_t *ab) {
    uint8_t hdr[6];
    if (shm_get(base, head, tail, hdr, 6, ab) < 0) return -1;
    *op = hdr[0] | (hdr[1]<<8);
    *len = (uint32_t)hdr[2] | ((uint32_t)hdr[3]<<8) | ((uint32_t)hdr[4]<<16) | ((uint32_t)hdr[5]<<24);
    int oversized = *len > cap;
    uint32_t take = *len < cap ? *len : cap;
    if (take && shm_get(base, head, tail, buf, take, ab) < 0) return -1;
    uint32_t rest = *len - take; uint8_t sink[256];
    while (rest) { uint32_t c = rest < sizeof sink ? rest : sizeof sink;
                   if (shm_get(base, head, tail, sink, c, ab) < 0) return -1; rest -= c; }
    return oversized ? -2 : 0;
}

#endif /* GLPASS_SHM_H */
