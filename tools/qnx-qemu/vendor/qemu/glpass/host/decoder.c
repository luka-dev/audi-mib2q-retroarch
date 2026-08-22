#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "backend.h"
#include "../protocol.h"

/* batch-3 generic dispatch (host/gl2_dispatch.c) — calls real GL directly */
uint32_t glp_gl2_dispatch(const void *payload, uint32_t len, void *reply, uint32_t cap);

/* diagnostics: last wire op seen by the decoder (covers EVERY op, dedicated +
 * generic), so the server's crash handler can name the call that died. */
volatile uint16_t glp_last_op = 0xffff;
volatile uint32_t glp_last_op_len = 0;
volatile unsigned long glp_dispatch_count = 0;

static uint32_t put(void *reply, uint32_t cap, const void *src, uint32_t n) {
    if (n > cap) n = cap;
    memcpy(reply, src, n);
    return n;
}

uint32_t glp_dispatch(glp_backend *be, uint16_t op,
                      const void *payload, uint32_t len,
                      void *reply, uint32_t cap) {
    void *st = be->st;
    glp_last_op = op; glp_last_op_len = len; glp_dispatch_count++;
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("GLP_TRACE"); trace = e ? atoi(e) : 0; }
    if (trace && op != OP_glGeneric) {   /* generic ops trace themselves with detail */
        fprintf(stderr, "[op#%lu] OP=%u len=%u\n", glp_dispatch_count, op, len); fflush(stderr);
    }
    switch (op) {
    /* --- EGL --- */
    case OP_eglGetDisplay:
        be->eglGetDisplay(st, *(const uint32_t*)payload); return 0;
    case OP_eglInitialize: {
        int major=0, minor=0, ok = be->eglInitialize(st, &major, &minor);
        int32_t r[3] = { ok, major, minor };
        return put(reply, cap, r, sizeof r);
    }
    case OP_eglChooseConfig: {
        uint32_t cfg=0;
        int ok = be->eglChooseConfig(st, (const int32_t*)payload,
                                     len/sizeof(int32_t), &cfg);
        int32_t r[2] = { ok, (int32_t)cfg };
        return put(reply, cap, r, sizeof r);
    }
    case OP_eglCreateContext: {
        const uint32_t *p = payload; be->eglCreateContext(st, p[0], p[1]); return 0;
    }
    case OP_eglCreateWindowSurface: {
        const int32_t *p = payload;
        be->eglCreateWindowSurface(st, (uint32_t)p[0], (uint32_t)p[1], p[2], p[3]); return 0;
    }
    case OP_eglMakeCurrent: be->eglMakeCurrent(st); return 0;
    case OP_eglSwapBuffers: be->eglSwapBuffers(st); return 0;
    case OP_eglDestroySurface:
    case OP_eglDestroyContext:
    case OP_eglTerminate: return 0; /* no-ops for milestone 0 */

    /* --- GLES2 --- */
    case OP_glViewport: {
        const glp_viewport_t *v = payload; be->viewport(st, v->x, v->y, v->w, v->h); return 0;
    }
    case OP_glClearColor: {
        const glp_clearcolor_t *c = payload; be->clearColor(st, c->r, c->g, c->b, c->a); return 0;
    }
    case OP_glClear: {
        const glp_clear_t *c = payload; be->clear(st, c->mask); return 0;
    }
    case OP_glCreateShader: {
        const glp_createshader_t *s = payload;
        uint32_t name = be->createShader(st, s->type);
        return put(reply, cap, &name, sizeof name);
    }
    case OP_glShaderSource: {
        const uint32_t *h = payload;          /* [shader][slen][text] */
        const char *src = (const char*)payload + sizeof(uint32_t)*2;
        be->shaderSource(st, h[0], src); return 0;
    }
    case OP_glCompileShader: {
        const glp_shader_t *s = payload; be->compileShader(st, s->shader); return 0;
    }
    case OP_glGetShaderiv: {
        const glp_getshaderiv_t *q = payload;
        int32_t r = be->getShaderiv(st, q->shader, q->pname);
        return put(reply, cap, &r, sizeof r);
    }
    case OP_glCreateProgram: {
        uint32_t name = be->createProgram(st);
        return put(reply, cap, &name, sizeof name);
    }
    case OP_glAttachShader: {
        const glp_attach_t *a = payload; be->attachShader(st, a->program, a->shader); return 0;
    }
    case OP_glBindAttribLocation: {
        const uint32_t *h = payload;          /* [program][index][name] */
        const char *name = (const char*)payload + sizeof(uint32_t)*2;
        be->bindAttribLocation(st, h[0], h[1], name); return 0;
    }
    case OP_glLinkProgram: {
        const glp_program_t *p = payload; be->linkProgram(st, p->program); return 0;
    }
    case OP_glGetProgramiv: {
        const glp_getprogramiv_t *q = payload;
        int32_t r = be->getProgramiv(st, q->program, q->pname);
        return put(reply, cap, &r, sizeof r);
    }
    case OP_glUseProgram: {
        const glp_program_t *p = payload; be->useProgram(st, p->program); return 0;
    }
    case OP_glEnableVertexAttribArray: {
        const glp_attribidx_t *a = payload; be->enableVertexAttribArray(st, a->index); return 0;
    }
    case OP_glVertexAttribPointer: {
        const glp_vap_t *h = payload;
        const void *data = (const uint8_t*)payload + sizeof(glp_vap_t);
        be->vertexAttribPointer(st, h->index, h->size, h->type, h->normalized,
                                h->stride, data, h->bytes); return 0;
    }
    case OP_glDrawArrays: {
        const glp_drawarrays_t *d = payload; be->drawArrays(st, d->mode, d->first, d->count); return 0;
    }

    /* batch 2 */
    case OP_glEnable:  { const glp_cap_t *c = payload; if (be->enable)  be->enable(st, c->cap);  return 0; }
    case OP_glDisable: { const glp_cap_t *c = payload; if (be->disable) be->disable(st, c->cap); return 0; }
    case OP_glCullFace:{ const glp_cap_t *c = payload; if (be->cullFace)be->cullFace(st, c->cap);return 0; }
    case OP_glGetUniformLocation: {
        const uint32_t *h = payload;                 /* [program][name] */
        const char *name = (const char*)payload + sizeof(uint32_t);
        int32_t r = be->getUniformLocation ? be->getUniformLocation(st, h[0], name) : -1;
        return put(reply, cap, &r, sizeof r);
    }
    case OP_glUniform1f: {
        const glp_uniform1f_t *u = payload; if (be->uniform1f) be->uniform1f(st, u->loc, u->v); return 0;
    }
    case OP_glUniform2fv: {
        const glp_uniformv_t *h = payload;
        const float *v = (const float*)((const uint8_t*)payload + sizeof(glp_uniformv_t));
        if (be->uniform2fv) be->uniform2fv(st, h->loc, h->count, v); return 0;
    }
    case OP_glUniformMatrix4fv: {
        const glp_uniformv_t *h = payload;
        const float *v = (const float*)((const uint8_t*)payload + sizeof(glp_uniformv_t));
        if (be->uniformMatrix4fv) be->uniformMatrix4fv(st, h->loc, h->count, v); return 0;
    }

    /* batch 3: generic GLES2 — handled directly against real GL, no vtable */
    case OP_glGeneric:
        return glp_gl2_dispatch(payload, len, reply, cap);

    default: return 0;
    }
}
