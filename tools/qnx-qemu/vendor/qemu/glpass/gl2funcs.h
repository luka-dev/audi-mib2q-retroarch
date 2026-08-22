/*
 * Generic GLES2 passthrough — function IDs + wire layout, shared guest/host.
 *
 * Instead of a bespoke opcode per GL call, the whole GLES2 surface goes through
 * ONE wire op (OP_glGeneric) carrying: [u16 func][u8 nargs][u8 flags][i32
 * args[nargs]][u32 datalen][data]. The shim packs each call's scalar args as
 * i32 words (floats bit-cast to i32) plus an optional trailing data blob
 * (pixels / vertices / matrices / indices). The host unpacks by func id and
 * calls real GL. Round-trip calls (gen*, get*, *Location, *Status) set
 * GL2_RET and receive a reply frame.
 *
 * This is how the ~57 functions libeal needs get added compactly rather than
 * ~5 edits each. Float args are transported as their IEEE-754 bit pattern in an
 * i32 slot; the host bit-casts back.
 */
#ifndef GLPASS_GL2FUNCS_H
#define GLPASS_GL2FUNCS_H

#include <stdint.h>

#define GL2_RET 0x01   /* flags: this call returns a value via reply frame */

enum gl2_func {
    GL2_glActiveTexture = 1,
    GL2_glBindBuffer, GL2_glBindFramebuffer, GL2_glBindRenderbuffer, GL2_glBindTexture,
    GL2_glBlendFunc, GL2_glBlendFuncSeparate,
    GL2_glBufferData, GL2_glBufferSubData,           /* data */
    GL2_glCheckFramebufferStatus,                    /* ret */
    GL2_glClearStencil, GL2_glColorMask,
    GL2_glCompressedTexImage2D,                      /* data */
    GL2_glDeleteBuffers, GL2_glDeleteFramebuffers, GL2_glDeleteRenderbuffers, GL2_glDeleteTextures, /* data(names) */
    GL2_glDepthFunc, GL2_glDepthMask, GL2_glDetachShader, GL2_glDisableVertexAttribArray,
    GL2_glDrawElements,                              /* data(indices) */
    GL2_glFlush, GL2_glFramebufferRenderbuffer, GL2_glFramebufferTexture2D,
    GL2_glGenBuffers, GL2_glGenFramebuffers, GL2_glGenRenderbuffers, GL2_glGenTextures, /* ret(names) */
    GL2_glGetAttribLocation,                         /* ret, data(name) */
    GL2_glGetFloatv, GL2_glGetIntegerv,              /* ret */
    GL2_glGetString,                                 /* ret(text) */
    GL2_glGetTexParameterfv, GL2_glGetTexParameteriv,/* ret */
    GL2_glLineWidth, GL2_glPixelStorei,
    GL2_glReadPixels,                                /* ret(pixels) */
    GL2_glRenderbufferStorage, GL2_glSampleCoverage, GL2_glScissor,
    GL2_glStencilFunc, GL2_glStencilMask, GL2_glStencilOp,
    GL2_glTexImage2D, GL2_glTexSubImage2D,           /* data(pixels) */
    GL2_glTexParameterf, GL2_glTexParameteri,
    GL2_glUniform1fv, GL2_glUniform1i, GL2_glUniform2f, GL2_glUniform3f,
    GL2_glUniform3fv, GL2_glUniform4f, GL2_glUniform4fv,
    GL2_glUniformMatrix2fv, GL2_glUniformMatrix3fv,  /* data(values) */
    GL2_glVertexAttribPointerVBO,                    /* index,size,type,norm,stride,offset (VBO mode) */
    GL2_glUniformConsts,                             /* a3xx const-file -> ir3_c[]: args base,count; data floats */
    GL2_FUNC_MAX
};

/* wire header for OP_glGeneric */
typedef struct {
    uint16_t func;
    uint8_t  nargs;
    uint8_t  flags;
    uint32_t datalen;
    /* int32_t args[nargs]; uint8_t data[datalen]; */
} gl2_hdr_t;

static inline float  gl2_i2f(int32_t i){ union{int32_t i;float f;}u; u.i=i; return u.f; }
static inline int32_t gl2_f2i(float f){ union{int32_t i;float f;}u; u.f=f; return u.i; }

#endif
