/*
 * Backend vtable: the decoder turns wire frames back into calls on one of
 * these. Two implementations:
 *   - backend_trace: records calls as text, no GL. Used by the loopback
 *     selftest so the pipeline is verifiable in a terminal with no display.
 *   - backend_gl  : real OpenGL via GLFW (host/backend_gl.c). The runnable
 *     renderer.
 *
 * Round-trip ops return a value through *reply (the decoder ships it back).
 */
#ifndef GLPASS_BACKEND_H
#define GLPASS_BACKEND_H

#include <stdint.h>

typedef struct glp_backend {
    void *st;

    /* EGL */
    void (*eglGetDisplay)(void *st, uint32_t id);
    int  (*eglInitialize)(void *st, int *major, int *minor);     /* round-trip */
    int  (*eglChooseConfig)(void *st, const int32_t *attribs, uint32_t n, uint32_t *cfg); /* rt */
    void (*eglCreateContext)(void *st, uint32_t config, uint32_t share);
    void (*eglCreateWindowSurface)(void *st, uint32_t config, uint32_t win, int w, int h);
    void (*eglMakeCurrent)(void *st);
    void (*eglSwapBuffers)(void *st);

    /* GLES2 */
    void   (*viewport)(void *st, int x, int y, int w, int h);
    void   (*clearColor)(void *st, float r, float g, float b, float a);
    void   (*clear)(void *st, uint32_t mask);
    uint32_t (*createShader)(void *st, uint32_t type);           /* round-trip */
    void   (*shaderSource)(void *st, uint32_t shader, const char *src);
    void   (*compileShader)(void *st, uint32_t shader);
    int32_t (*getShaderiv)(void *st, uint32_t shader, uint32_t pname); /* round-trip */
    uint32_t (*createProgram)(void *st);                         /* round-trip */
    void   (*attachShader)(void *st, uint32_t program, uint32_t shader);
    void   (*bindAttribLocation)(void *st, uint32_t program, uint32_t index, const char *name);
    void   (*linkProgram)(void *st, uint32_t program);
    int32_t (*getProgramiv)(void *st, uint32_t program, uint32_t pname); /* round-trip */
    void   (*useProgram)(void *st, uint32_t program);
    void   (*enableVertexAttribArray)(void *st, uint32_t index);
    void   (*vertexAttribPointer)(void *st, uint32_t index, int size, uint32_t type,
                                  uint8_t norm, int stride, const void *data, uint32_t nbytes);
    void   (*drawArrays)(void *st, uint32_t mode, int first, int count);

    /* batch 2 */
    void    (*enable)(void *st, uint32_t cap);
    void    (*disable)(void *st, uint32_t cap);
    void    (*cullFace)(void *st, uint32_t mode);
    int32_t (*getUniformLocation)(void *st, uint32_t program, const char *name); /* round-trip */
    void    (*uniform1f)(void *st, int loc, float v);
    void    (*uniform2fv)(void *st, int loc, int count, const float *v);
    void    (*uniformMatrix4fv)(void *st, int loc, int count, const float *v);
} glp_backend;

/* Decode one frame and dispatch to backend. For round-trip ops, writes the
 * reply into `reply` (cap bytes) and returns reply length; else returns 0. */
uint32_t glp_dispatch(glp_backend *be, uint16_t op,
                      const void *payload, uint32_t len,
                      void *reply, uint32_t cap);

#endif /* GLPASS_BACKEND_H */
