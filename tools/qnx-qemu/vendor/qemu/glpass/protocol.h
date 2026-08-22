/*
 * qnx-gl-passthrough — wire protocol
 *
 * Shared between the QNX guest shim (libEGL/libGLESv2 replacement) and the
 * macOS host renderer. Pure C89, no platform headers, so the SAME file
 * compiles under qcc (QNX armle) and clang (macOS).
 *
 * Framing on the wire (little-endian, the guest is armle = LE, host x64/arm64
 * are LE too, so no byte-swap needed for milestone 0):
 *
 *     [u16 opcode][u32 payload_len][payload bytes...]
 *
 * The payload layout is per-opcode and defined by the gl_cmd_* encoders in
 * guest/encoder.c. Anything variable-length (shader source, vertex data) is
 * length-prefixed inside the payload.
 *
 * Return values: most GLES calls in the dmminimal subset are "fire and
 * forget" (void, or return a name the guest can predict). The few that need a
 * real answer from the host (eglInitialize, eglChooseConfig, glGetError,
 * glGetShaderiv, glCreateShader/Program) are handled as round-trips: the guest
 * blocks on a reply frame [u32 reply_len][reply bytes]. Round-trips are the
 * slow path; keep them rare.
 */
#ifndef QNX_GL_PASSTHROUGH_PROTOCOL_H
#define QNX_GL_PASSTHROUGH_PROTOCOL_H

#include <stdint.h>

#define GLPASS_MAGIC   0x47504153u /* "GPAS" */
#define GLPASS_VERSION 1u

/* Opcodes. Keep stable once shipped; append only. */
enum glpass_op {
    /* --- EGL --- */
    OP_eglGetDisplay = 1,
    OP_eglInitialize,            /* round-trip: -> {EGLBoolean, major, minor} */
    OP_eglChooseConfig,          /* round-trip: -> {EGLBoolean, config_id}    */
    OP_eglCreateContext,
    OP_eglCreateWindowSurface,
    OP_eglMakeCurrent,
    OP_eglSwapBuffers,           /* host presents the frame */
    OP_eglDestroySurface,
    OP_eglDestroyContext,
    OP_eglTerminate,

    /* --- GLES2 --- */
    OP_glViewport = 64,
    OP_glClearColor,
    OP_glClear,
    OP_glCreateShader,           /* round-trip: -> {GLuint name} */
    OP_glShaderSource,           /* payload carries the source text */
    OP_glCompileShader,
    OP_glGetShaderiv,            /* round-trip: -> {GLint} */
    OP_glGetShaderInfoLog,       /* round-trip: -> {text} */
    OP_glCreateProgram,          /* round-trip: -> {GLuint name} */
    OP_glAttachShader,
    OP_glBindAttribLocation,     /* payload carries the name string */
    OP_glLinkProgram,
    OP_glGetProgramiv,           /* round-trip: -> {GLint} */
    OP_glGetProgramInfoLog,      /* round-trip: -> {text} */
    OP_glUseProgram,
    OP_glDeleteShader,
    OP_glDeleteProgram,
    OP_glEnableVertexAttribArray,
    OP_glVertexAttribPointer,    /* payload carries the client vertex array */
    OP_glDrawArrays,

    /* --- batch 2: mandelbox (uniforms + state) --- */
    OP_glEnable,
    OP_glDisable,
    OP_glCullFace,
    OP_glGetUniformLocation,     /* round-trip: -> {GLint location} */
    OP_glUniform1f,
    OP_glUniform2fv,             /* payload carries count*2 floats */
    OP_glUniformMatrix4fv,       /* payload carries count*16 floats */

    /* --- batch 3: generic GLES2 passthrough (gl2funcs.h) --- */
    OP_glGeneric,                /* [gl2_hdr_t][i32 args][data]; round-trip if GL2_RET */

    OP_MAX
};

/* Fixed-size payload structs for the simple ops. Variable-length ops
 * (ShaderSource, VertexAttribPointer, BindAttribLocation) are encoded by hand
 * in encoder.c because they carry a trailing blob. */

typedef struct { int32_t x, y, w, h; }                 glp_viewport_t;
typedef struct { float r, g, b, a; }                   glp_clearcolor_t;
typedef struct { uint32_t mask; }                      glp_clear_t;
typedef struct { uint32_t type; }                      glp_createshader_t;
typedef struct { uint32_t shader; }                    glp_shader_t;
typedef struct { uint32_t shader, pname; }             glp_getshaderiv_t;
typedef struct { uint32_t program, shader; }           glp_attach_t;
typedef struct { uint32_t program, pname; }            glp_getprogramiv_t;
typedef struct { uint32_t program; }                   glp_program_t;
typedef struct { uint32_t index; }                     glp_attribidx_t;
typedef struct { uint32_t mode; int32_t first, count; } glp_drawarrays_t;

/* glVertexAttribPointer: header followed by `bytes` of client array data.
 * For milestone 0 the guest snapshots the whole client array on the Draw call
 * (the dmminimal app uses a static stack array), so we ship it inline. */
typedef struct {
    uint32_t index;
    int32_t  size;     /* components per vertex (1..4) */
    uint32_t type;     /* GL_FLOAT etc. */
    uint8_t  normalized;
    uint8_t  _pad[3];
    int32_t  stride;
    uint32_t bytes;    /* trailing blob length */
    /* uint8_t data[bytes] */
} glp_vap_t;

/* batch 2 */
typedef struct { uint32_t cap; }                glp_cap_t;       /* enable/disable/cullface */
typedef struct { int32_t loc; float v; }        glp_uniform1f_t;
typedef struct { int32_t loc; int32_t count; }  glp_uniformv_t;  /* + count*N floats trailing */

#endif /* QNX_GL_PASSTHROUGH_PROTOCOL_H */
