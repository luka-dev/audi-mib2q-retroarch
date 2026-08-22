/*
 * Host consumer for the shared-memory ring. Polls c2h for command frames,
 * dispatches into a backend, and writes round-trip replies back on h2c.
 * Same decoder/backend as every other path. Runs until guest_ready clears.
 */
#include <stdint.h>
#include <stdio.h>
#include "backend.h"
#include "../protocol.h"
#include "../wire.h"
#include "../shm.h"

int glp_serve_shm(shm_region *r, glp_backend *be, void (*on_swap)(void *u), void *u) {
    /* The ring streams frames larger than its own 4 MiB capacity. Real Kanzi
     * glBufferData records reach 4.78 MiB, so the old 2 MiB receive buffer
     * truncated payload while still passing the original length to dispatch. */
    static unsigned char payload[SHM_FRAME_MAX];
    unsigned char reply[256];
    r->host_ready = 1;
    shm_barrier();
    for (;;) {
        uint16_t op = 0; uint32_t len = 0;
        int got = shm_get_frame(r->c2h, &r->c2h_head, &r->c2h_tail, &op,
                                payload, sizeof payload, &len, &r->guest_ready);
        if (got == -1)
            return 0; /* guest gone */
        if (got == -2) {
            fprintf(stderr, "[shm] DROP frame op=%u len=%u cap=%u\n",
                    op, len, (unsigned)sizeof payload);
            continue;
        }
#ifdef GLPASS_SHM_TRACE
        fprintf(stderr, "[shm op=%u len=%u]\n", op, len);
#endif
        uint32_t rlen = glp_dispatch(be, op, payload, len, reply, sizeof reply);
        if (rlen) {
            fprintf(stderr, "[reply op=%u rlen=%u val=%u]\n", op, rlen,
                    rlen>=4 ? *(uint32_t*)reply : 0);
            shm_put_frame(r->h2c, &r->h2c_head, &r->h2c_tail, WIRE_OP_REPLY,
                          reply, rlen, &r->guest_ready);
        }
        if (op == OP_eglSwapBuffers && on_swap) on_swap(u);
    }
}
