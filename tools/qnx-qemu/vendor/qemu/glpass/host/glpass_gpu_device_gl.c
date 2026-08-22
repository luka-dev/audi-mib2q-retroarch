/*
 * In-QEMU virt-gpu render entry — OpenGL (backend_gl) edition.
 *
 * Uses the COMPLETE, correct backend_gl renderer (the one glpass_serve_shm uses
 * to render the real HMI menu) inside the qemu render thread, via a headless CGL
 * context — no GLFW window (GLFW needs the main thread; CGL offscreen works on a
 * background thread). backend_gl renders GL-1.20 shaders, so we request a LEGACY
 * CGL profile. The guest's framebuffer 0 (the final composite target) is
 * redirected to our offscreen FBO via gl2_dispatch's glp_default_fbo.
 *
 * Trade-off vs the ANGLE path: OpenGL is deprecated on macOS (but works), while
 * the ANGLE/Metal backend is future-proof but its render machinery is unfinished.
 * This gives a working in-qemu GPU device NOW.
 */
#include <stdio.h>
#include <stdlib.h>
#include <OpenGL/OpenGL.h>   /* CGL */
#include <OpenGL/gl.h>       /* legacy GL 2.1 + ARB_framebuffer_object */
#include "backend.h"
#include "../shm.h"
#include "glpass_raw_frame.h"

#define SERVE_W 1024
#define SERVE_H 480

extern glp_backend *glp_backend_gl(void);
extern int  glp_serve_shm(shm_region *r, glp_backend *be, void (*on_swap)(void *), void *u);
extern int  glp_default_fbo;   /* gl2_dispatch.c: guest FB0 -> this FBO when nonzero */
extern unsigned glp_present_fbo(void); /* latest full-size memory-backed RT */

static GLuint g_fbo, g_color, g_depth;
static glpass_raw_frame *g_raw_frame;
static uint32_t g_raw_sequence;

static uint32_t frame_hash(const unsigned char *px, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= px[i];
        h *= 16777619u;
    }
    return h;
}

static void write_rgb_ppm(const char *path, const unsigned char *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
        fwrite(&px[y * w * 3], 1, (size_t)w * 3, f);
    }
    fclose(f);
}

/* Optional present hook. The qemu virt-gpu device installs this to receive each
 * rendered RGB frame (W*H*3, bottom-up) so it can blit it to the on-screen
 * console. NULL in the standalone/offscreen path. */
static void (*g_present)(void *u, const unsigned char *rgb, int w, int h);
static void *g_present_u;
void glpass_gpu_set_present(void (*cb)(void *, const unsigned char *, int, int),
                            void *u)
{
    g_present = cb;
    g_present_u = u;
}

static int gl_offscreen_init(int w, int h)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFAColorSize,  (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,  (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize,  (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = 0; GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix) {
        fprintf(stderr, "[gl-dev] CGLChoosePixelFormat failed\n"); return 1;
    }
    CGLContextObj ctx = 0;
    CGLError e = CGLCreateContext(pix, 0, &ctx);
    CGLDestroyPixelFormat(pix);
    if (e != kCGLNoError || !ctx) {
        fprintf(stderr, "[gl-dev] CGLCreateContext failed (%d)\n", e); return 1;
    }
    CGLSetCurrentContext(ctx);
    fprintf(stderr, "[gl-dev] CGL context up: GL_VERSION=%s\n", glGetString(GL_VERSION));

    /* Offscreen FBO = the default render target (the guest binds FB0 to composite). */
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gl-dev] FBO incomplete: 0x%x\n", st); return 1;
    }
    glp_default_fbo = (int)g_fbo;   /* redirect guest glBindFramebuffer(0) -> g_fbo */
    glViewport(0, 0, w, h);
    fprintf(stderr, "[gl-dev] offscreen FBO %u ready (%dx%d)\n", g_fbo, w, h);
    return 0;
}

static void on_swap(void *u)
{
    (void)u;
    glFinish();
    int W = SERVE_W, H = SERVE_H;
    unsigned char *px = malloc((size_t)W * H * 3);
    if (!px) return;
    GLint prev_fbo = 0;
    GLuint present_fbo = (GLuint)glp_present_fbo();
    uint32_t raw_sequence = g_raw_frame ? g_raw_frame->sequence : 0;
    int have_raw = g_raw_frame &&
        g_raw_frame->magic == GLPASS_RAW_FRAME_MAGIC &&
        g_raw_frame->version == GLPASS_RAW_FRAME_VERSION &&
        g_raw_frame->format == GLPASS_RAW_FRAME_RGBA8888 &&
        g_raw_frame->width == (uint32_t)W &&
        g_raw_frame->height == (uint32_t)H &&
        g_raw_frame->stride >= (uint32_t)W * 4u &&
        g_raw_frame->data_bytes >= (uint32_t)W * H * 4u;
    if (have_raw) {
        int is_new_raw = raw_sequence != g_raw_sequence;
        __sync_synchronize();
        for (int y = 0; y < H; y++) {
            const unsigned char *src = g_raw_frame->pixels +
                (size_t)y * g_raw_frame->stride;
            unsigned char *dst = px + (size_t)(H - 1 - y) * W * 3;
            for (int x = 0; x < W; x++) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        __sync_synchronize();
        g_raw_sequence = raw_sequence;
        present_fbo = 0;
        if (is_new_raw)
            fprintf(stderr, "[gl-dev] presenting Rust RGBA mailbox frame=%u\n",
                    raw_sequence);
    } else {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, present_fbo);
        glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    }

    /* Hand the frame to the on-screen console (if the qemu device wired one). */
    if (g_present) g_present(g_present_u, px, W, H);

    long nonblack = 0;
    for (int i = 0; i < W * H * 3; i++) if (px[i] > 8) nonblack++;
    static long best = -1; static int n = 0; ++n;
    const char *keep_dir = getenv("GLP_FRAME_DUMP_DIR");
    static uint32_t last_hash;
    static int have_hash, kept;
    uint32_t hash = frame_hash(px, (size_t)W * H * 3);
    if (keep_dir && *keep_dir && kept < 256 &&
        (!have_hash || hash != last_hash)) {
        char name[1024];
        snprintf(name, sizeof name, "%s/frame_%04d_fbo%u_%08x.ppm",
                 keep_dir, n, present_fbo, hash);
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int y = H - 1; y >= 0; y--)
                fwrite(&px[y * W * 3], 1, W * 3, f);
            fclose(f);
            kept++;
            fprintf(stderr, "[FRAMEKEEP] frame=%d fbo=%u hash=%08x "
                    "nonblack=%ld file=%s\n",
                    n, present_fbo, hash, nonblack, name);
        }
    }
    last_hash = hash;
    have_hash = 1;
    int is_best = nonblack > best;
    if (is_best) {
        best = nonblack;
    }

    /*
     * Native QEMU presentation above is entirely memory-to-memory and needs no
     * PPM. Preserve the old best/latest files only as an explicit diagnostic;
     * callers can point them at /tmp to avoid project or SSD churn.
     */
    const char *legacy_dir = getenv("GLP_LEGACY_PPM_DIR");
    if (legacy_dir && *legacy_dir) {
        char name[1024];
        if (is_best) {
            snprintf(name, sizeof name, "%s/shm_frame_gpu.ppm", legacy_dir);
            write_rgb_ppm(name, px, W, H);
        }
        snprintf(name, sizeof name, "%s/shm_frame_gpu_latest.ppm", legacy_dir);
        write_rgb_ppm(name, px, W, H);
    }

    fprintf(stderr, "[gl-dev: frame %d rendered fbo=%u nonblack=%ld best=%ld]\n",
            n, present_fbo, nonblack, best);
    free(px);
}

/* Called by the qemu virt-gpu render thread with the device RAM ptr (shm_region). */
int glpass_gpu_render_gl(void *shm)
{
    fprintf(stderr, "[gl-dev] glpass_gpu_render_gl: init offscreen GL %dx%d\n", SERVE_W, SERVE_H);
    g_raw_frame = (glpass_raw_frame *)((unsigned char *)shm +
                                       GLPASS_RAW_FRAME_OFFSET);
    if (gl_offscreen_init(SERVE_W, SERVE_H) != 0) {
        fprintf(stderr, "[gl-dev] GL init FAILED\n"); return 2;
    }
    fprintf(stderr, "[gl-dev] GL up; entering glp_serve_shm (backend_gl)\n");
    glp_serve_shm((shm_region *)shm, glp_backend_gl(), on_swap, 0);
    fprintf(stderr, "[gl-dev] glp_serve_shm returned (guest closed channel)\n");
    return 0;
}
