/*
 * Host side of the generic GLES2 passthrough (OP_glGeneric / gl2funcs.h).
 *
 * The guest packs every batch-3 GL call as [gl2_hdr_t][i32 args][data]; here we
 * unpack by func id and call real desktop GL (the GLFW 2.1 context is already
 * current). Float args arrive as IEEE-754 bit patterns in i32 slots. Round-trip
 * calls (GL2_RET) write their result into `reply` and we return its length.
 *
 * FBO / renderbuffer entry points: macOS exposes ARB_framebuffer_object in the
 * legacy 2.1 context, so the core names from <OpenGL/glext.h> link fine.
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/* The in-qemu virt-gpu GL device renders to an offscreen FBO; the guest's
 * framebuffer 0 (its final composite target) is redirected here. 0 = the context
 * default (GLFW window / ANGLE pbuffer); set nonzero only by glpass_gpu_device_gl. */
int glp_default_fbo = 0;
#include <stdlib.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include "../gl2funcs.h"

#define AI(k) (ai[k])
#define AF(k) gl2_i2f(ai[k])

/* diagnostics: last opcode + a running counter, so a SIGSEGV handler in the
 * server (glp_diag_dump) can name the GL call that crashed the host. With
 * GLP_TRACE=1 every op is logged to stderr (slow, but pinpoints hangs). */
volatile uint32_t glp_last_func = 0xffffffff;
volatile uint32_t glp_last_args[8];
volatile unsigned long glp_op_count = 0;

/* Apple's legacy GL crashes in gleRunVertexSubmitImmediate when an *enabled*
 * generic vertex-attribute array has no VBO bound and a NULL client pointer
 * (offset 0 from glVertexAttribPointer while ARRAY_BUFFER==0). Kanzi's geometry
 * is fully VBO-backed, but some attribute slots get enabled without a valid
 * pointer; that one bad slot null-derefs the whole draw. Before every draw,
 * scan the attribute slots and disable any enabled-but-clientside-NULL one so
 * the draw uses only the real VBO attributes. */
#ifndef GL_VERTEX_ATTRIB_ARRAY_ENABLED
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED        0x8622
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_VERTEX_ATTRIB_ARRAY_POINTER        0x8645
#endif
void glp_guard_attribs(const char *who) {
    GLint maxattr = 16;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxattr);
    if (maxattr <= 0 || maxattr > 64) maxattr = 16;
    for (GLint i = 0; i < maxattr; i++) {
        GLint enabled = 0, binding = 0;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &binding);
        if (binding != 0) continue;            /* real VBO-backed attribute: keep */
        void *ptr = (void*)1;
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        if (ptr == 0) {
            static int warned[64] = {0};
            if (i < 64 && !warned[i]) { warned[i] = 1;
                fprintf(stderr, "[guard] %s: attr %d enabled, no VBO, NULL ptr -> disabling\n", who, i);
                fflush(stderr); }
            glDisableVertexAttribArray(i);
        }
    }
}

/* macOS legacy GL 2.1 cannot use GL_LUMINANCE/GL_LUMINANCE_ALPHA/GL_ALPHA as
 * FBO color attachments (not color-renderable). Kanzi's CPU-rasterized glyph
 * atlases use these formats AND attach them to FBOs (or run a renderability
 * check) -> glCheckFramebufferStatus returns INCOMPLETE -> the engine deletes
 * the texture and NULLs its font-image object -> later free of NULL crashes
 * (the text-render SIGSEGV). Fix (per codex+gemini): transparently virtualize
 * these textures as RGBA8 (color-renderable) by expanding the pixel data to
 * RGBA with identical GLES luminance sampling semantics. */
#ifndef GL_LUMINANCE
#define GL_LUMINANCE        0x1909
#define GL_LUMINANCE_ALPHA  0x190A
#define GL_ALPHA            0x1906
#endif
#ifndef GL_RGBA8
#define GL_RGBA8            0x8058
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
static int g_unpack_align = 4;
static int is_lum_fmt(GLenum f){ return f==GL_LUMINANCE||f==GL_LUMINANCE_ALPHA||f==GL_ALPHA; }
/* Track texture format per id + which texture is bound to each unit, so we can
 * see (and steer) which texture the substitute's single 'Texture' sampler ends
 * up reading — the menu font labels go rainbow when it lands on a non-glyph. */
#define GLP_MAXTEX 8192
static int g_active_unit = 0;
static int g_unit_tex[16];
static int g_tex_fmt[GLP_MAXTEX];   /* internalformat per texture id (0=unknown) */
static unsigned char g_tex_uploaded[GLP_MAXTEX]; /* 1 = filled by glTexImage2D (an atlas/icon),
    0 = never uploaded (an FBO color attachment / layer-composition texture). */
static unsigned short g_tex_w[GLP_MAXTEX], g_tex_h[GLP_MAXTEX];
unsigned glp_unit_tex_size(int unit){ if(unit<0||unit>=16) return 0; int tx=g_unit_tex[unit];
    return (tx>=0&&tx<GLP_MAXTEX)? ((unsigned)g_tex_w[tx]<<16|g_tex_h[tx]) : 0; }
int glp_unit_tex_fmt(int unit){ if(unit<0||unit>=16) return 0; int t=g_unit_tex[unit];
    return (t>=0&&t<GLP_MAXTEX)? g_tex_fmt[t] : 0; }
int glp_unit_is_glyph(int unit){ return is_lum_fmt((GLenum)glp_unit_tex_fmt(unit)); }
/* the unit currently holding a glyph (LUMINANCE/LA) texture, or -1 */
int glp_glyph_unit(void){ for(int u=0;u<16;u++) if(is_lum_fmt((GLenum)glp_unit_tex_fmt(u))) return u; return -1; }
/* is the texture bound to `unit` an uploaded atlas (icon/font) vs an FBO? Used to
 * pick the Kanzi colorize-atlas fragment path (r=colorize,g=alpha,b=grey) instead
 * of plain passthrough/composite -> fixes the rainbow icons/labels. */
int glp_unit_uploaded(int unit){ if(unit<0||unit>=16) return 0; int t=g_unit_tex[unit];
    return (t>=0&&t<GLP_MAXTEX)? g_tex_uploaded[t] : 0; }
/* colorize-atlas = an uploaded RGB/RGBA atlas (the Kanzi font/icon material packs
 * r=colorize,g=alpha,b=grey into 3 distinct channels -> must be RGB/RGBA). A
 * LUMINANCE/LA glyph atlas (r==g==b) is NOT colorize-format: applying the colorize
 * math (L*Color + L) over-brightens it to white, so those stay plain passthrough. */
int glp_unit_colorize(int unit){ if(!glp_unit_uploaded(unit)) return 0;
    int f=glp_unit_tex_fmt(unit); return (f==0x1907/*RGB*/ || f==0x1908/*RGBA*/); }
/* expand guest L/LA/A rows (honoring guest unpack alignment) to tightly packed
 * RGBA: L->(L,L,L,255), LA->(L,L,L,A), A->(255,255,255,A). NULL src -> NULL
 * (allocate-only). caller frees. */
static unsigned char *expand_rgba(GLenum fmt, int w, int h, const unsigned char *src, int align){
    if(!src || w<=0 || h<=0) return 0;
    int bpp = (fmt==GL_LUMINANCE_ALPHA)?2:1;
    int srow = ((w*bpp + align-1)/align)*align;
    unsigned char *out = (unsigned char*)malloc((size_t)w*h*4);
    if(!out) return 0;
    for(int y=0;y<h;y++){
        const unsigned char *s = src + (size_t)y*srow;
        unsigned char *d = out + (size_t)y*w*4;
        for(int x=0;x<w;x++){
            unsigned char L,A;
            if(fmt==GL_LUMINANCE){ L=s[x]; A=255; }     /* real-device GLES: LUMINANCE.a = 1.0 */
            else if(fmt==GL_ALPHA){ L=0; A=s[x]; }       /* real-device GLES: ALPHA.rgb = 0 */
            else { L=s[2*x]; A=s[2*x+1]; }
            d[4*x]=L; d[4*x+1]=L; d[4*x+2]=L; d[4*x+3]=A;
        }
    }
    return out;
}

/* FNV-1a over the RGBA bytes — a stable content id for a texture (the KZB is
 * static, so the same asset uploads identically every boot -> same hash). */
static unsigned long glp_tex_hash(const unsigned char *rgba, int w, int h){
    unsigned long hsh=1469598103934665603UL;
    for(long i=0;i<(long)w*h*4;i++){ hsh^=rgba[i]; hsh*=1099511628211UL; }
    return hsh;
}

/* Diagnostics are project artifacts, not scratch files: /tmp is routinely
 * cleaned between engineering sessions.  run_hmi.sh supplies an absolute
 * GLP_TEX_DUMP_DIR; the relative fallback is useful when launching from the
 * repository root. */
const char *glp_tex_dump_dir(void){
    const char *p=getenv("GLP_TEX_DUMP_DIR");
    return (p && *p) ? p : "build/render_work/textures/current";
}
static const char *glp_texswap_dir(void){
    const char *p=getenv("GLP_TEXSWAP_DIR");
    return (p && *p) ? p : "build/render_work/texswap";
}
static void glp_mkdirs(const char *path){
    char tmp[1024];
    size_t n=path ? strlen(path) : 0;
    if(!n || n>=sizeof tmp) return;
    memcpy(tmp,path,n+1);
    for(char *p=tmp+1;*p;p++) if(*p=='/'){
        *p=0; (void)mkdir(tmp,0755); *p='/';
    }
    (void)mkdir(tmp,0755);
}

/* Dump an uploaded texture (RGBA8) to the project render-work directory so the
 * render-graph's `tex` ids become self-describing AND each carries its content
 * hash (the key to edit it). Gated by GLP_RG_DUMP. */
static void glp_tex_dump(int id, int w, int h, const unsigned char *rgba){
    if(!getenv("GLP_RG_DUMP") || !rgba || w<=1 || h<=1 || (long)w*h>16777216) return;
    static int made=0; if(!made){made=1; glp_mkdirs(glp_tex_dump_dir());}
    static int seq=0; seq++;
    if (w >= 1000 && h >= 400) {
        long rgb_nz=0, alpha_nz=0, alpha_opaque=0;
        unsigned amin=255, amax=0;
        for (long i=0; i<(long)w*h; i++) {
            const unsigned char *p=rgba+i*4;
            unsigned a=p[3];
            if (p[0] || p[1] || p[2]) rgb_nz++;
            if (a) alpha_nz++;
            if (a==255) alpha_opaque++;
            if (a<amin) amin=a;
            if (a>amax) amax=a;
        }
        fprintf(stderr,
                "[TEXDUMP] seq=%d tex=%d %dx%d rgb_nz=%ld alpha_nz=%ld "
                "alpha_opaque=%ld alpha_range=%u..%u\n",
                seq,id,w,h,rgb_nz,alpha_nz,alpha_opaque,amin,amax);
    }
    fprintf(stderr,"[TEXMAP] seq=%d tex=%d %dx%d\n",seq,id,w,h);
    char path[1200];
    snprintf(path,sizeof path,"%s/tex_%03d_id%d_%dx%d.ppm",
             glp_tex_dump_dir(),seq,id,w,h);
    FILE*f=fopen(path,"w"); if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(long i=0;i<(long)w*h;i++){ fputc(rgba[i*4],f); fputc(rgba[i*4+1],f); fputc(rgba[i*4+2],f); }
    fclose(f);
}
/* EDIT->PREVIEW: if an edited replacement in GLP_TEXSWAP_DIR exists
 * for this texture's content, return its RGBA (original alpha kept) to upload
 * INSTEAD of the engine's pixels. This is how a studio edit reflects live in the
 * real engine's frame — no KZB rewrite, no software renderer. Gated by GLP_TEXSWAP. */
static const unsigned char* glp_tex_swap(const unsigned char *rgba, int w, int h){
    if(!getenv("GLP_TEXSWAP") || !rgba || w<=0 || h<=0) return NULL;
    char path[1200]; snprintf(path,sizeof path,"%s/%016lx.ppm",
                              glp_texswap_dir(),glp_tex_hash(rgba,w,h));
    FILE*f=fopen(path,"r"); if(!f) return NULL;
    int pw=0,ph=0,mx=0; if(fscanf(f,"P6 %d %d %d",&pw,&ph,&mx)!=3||pw!=w||ph!=h){ fclose(f); return NULL; }
    fgetc(f);                                    /* one whitespace after maxval */
    static unsigned char *buf=NULL; free(buf); buf=(unsigned char*)malloc((size_t)w*h*4);
    for(long i=0;i<(long)w*h;i++){ int r=fgetc(f),g=fgetc(f),b=fgetc(f);
        buf[i*4]=(unsigned char)r; buf[i*4+1]=(unsigned char)g; buf[i*4+2]=(unsigned char)b; buf[i*4+3]=rgba[i*4+3]; }
    fclose(f); fprintf(stderr,"[texswap] replaced %dx%d texture from %s\n",w,h,path); return buf;
}

uint32_t glp_gl2_dispatch(const void *payload, uint32_t len,
                          void *reply, uint32_t cap) {
    if (len < sizeof(gl2_hdr_t)) return 0;
    const gl2_hdr_t *h = (const gl2_hdr_t*)payload;
    const int32_t *ai = (const int32_t*)((const uint8_t*)payload + sizeof(gl2_hdr_t));
    const void *data  = (const uint8_t*)ai + (uint32_t)h->nargs * 4;
    uint32_t dl = h->datalen;
    const void *D = dl ? data : 0;

    glp_last_func = h->func; glp_op_count++;
    for (int _i = 0; _i < 8 && _i < (int)h->nargs; _i++) glp_last_args[_i] = (uint32_t)ai[_i];
    static int trace = -1;
    if (trace < 0) { const char *e = getenv("GLP_TRACE"); trace = e ? atoi(e) : 0; }
    if (trace) { fprintf(stderr, "[op#%lu] func=%u nargs=%u dl=%u a0=%d a1=%d a2=%d\n",
                         glp_op_count, h->func, h->nargs, h->datalen,
                         h->nargs>0?ai[0]:0, h->nargs>1?ai[1]:0, h->nargs>2?ai[2]:0); fflush(stderr); }

    switch (h->func) {
    /* --- state (no data) --- */
    case GL2_glActiveTexture: { int u=AI(0)-0x84C0; if(u>=0&&u<16) g_active_unit=u; glActiveTexture(AI(0)); return 0; }
    case GL2_glBindBuffer: glBindBuffer(AI(0), AI(1)); return 0;
    case GL2_glBindFramebuffer: { int fb=AI(1); if(fb==0) fb=glp_default_fbo; static long n=0; if(n<400){n++; fprintf(stderr,"[FBO] bindFramebuffer fb=%d->%d\n",AI(1),fb);} glBindFramebuffer(AI(0), fb); return 0; }
    case GL2_glBindRenderbuffer: glBindRenderbuffer(AI(0), AI(1)); return 0;
    case GL2_glBindTexture: { static int c=0; if(c<8){c++; fprintf(stderr,"[bindTex] target=0x%x tex=%d\n",AI(0),AI(1));}
        if(g_active_unit>=0&&g_active_unit<16) g_unit_tex[g_active_unit]=AI(1);
        glBindTexture(AI(0), AI(1)); return 0; }
    case GL2_glBlendFunc: glBlendFunc(AI(0), AI(1)); return 0;
    case GL2_glBlendFuncSeparate: glBlendFuncSeparate(AI(0), AI(1), AI(2), AI(3)); return 0;
    case GL2_glClearStencil: glClearStencil(AI(0)); return 0;
    case GL2_glColorMask: glColorMask(AI(0), AI(1), AI(2), AI(3)); return 0;
    case GL2_glDepthFunc: glDepthFunc(AI(0)); return 0;
    case GL2_glDepthMask: glDepthMask(AI(0)); return 0;
    case GL2_glDetachShader: glDetachShader(AI(0), AI(1)); return 0;
    case GL2_glDisableVertexAttribArray: glDisableVertexAttribArray(AI(0)); return 0;
    case GL2_glFlush: glFlush(); return 0;
    case GL2_glFramebufferRenderbuffer: glFramebufferRenderbuffer(AI(0), AI(1), AI(2), AI(3)); return 0;
    case GL2_glFramebufferTexture2D: glFramebufferTexture2D(AI(0), AI(1), AI(2), AI(3), AI(4)); return 0;
    case GL2_glLineWidth: glLineWidth(AF(0)); return 0;
    case GL2_glPixelStorei: if(AI(0)==GL_UNPACK_ALIGNMENT) g_unpack_align=AI(1); glPixelStorei(AI(0), AI(1)); return 0;
    case GL2_glRenderbufferStorage: glRenderbufferStorage(AI(0), AI(1), AI(2), AI(3)); return 0;
    case GL2_glSampleCoverage: glSampleCoverage(AF(0), AI(1)); return 0;
    case GL2_glScissor: glScissor(AI(0), AI(1), AI(2), AI(3)); return 0;
    case GL2_glStencilFunc: glStencilFunc(AI(0), AI(1), AI(2)); return 0;
    case GL2_glStencilMask: glStencilMask(AI(0)); return 0;
    case GL2_glStencilOp: glStencilOp(AI(0), AI(1), AI(2)); return 0;
    case GL2_glTexParameterf: glTexParameterf(AI(0), AI(1), AF(2)); return 0;
    case GL2_glTexParameteri: glTexParameteri(AI(0), AI(1), AI(2)); return 0;
    case GL2_glUniform1i: { glUniform1i(AI(0), AI(1));
        /* detect engine layer-composition built-ins: if Kanzi binds the
         * LayerRender/Composition sampler, switch the substitute to the real
         * alpha-preserving layer composite (fixes the drum drop-shadow/gloss). */
        { extern void glp_note_sampler(int); glp_note_sampler(AI(0)); }
        /* mirror Kanzi's sampler->unit binding onto our substitute 'Texture'.
         * FIX: the menu font labels bind the glyph atlas (LUMINANCE) on one unit
         * AND a color/gradient on another, then set both samplers; mirroring the
         * LAST one landed 'Texture' on the gradient -> rainbow. Prefer the unit
         * holding a glyph texture if any is bound; else use the just-set unit
         * (image/icon draws have no glyph -> unaffected). */
        /* sampler-mirror REMOVED: every shader now arrives as real Kanzi GLSL (the
         * binary substitute is unused), so Kanzi sets each sampler to the right unit
         * via its own glUniform1i (relayed just above). The old mirror force-set the
         * 'Texture' sampler to AI(1)/glyph-unit, which corrupted multi-sampler real
         * shaders — that was the drum-vs-border trade-off. Let Kanzi's binding stand. */
        return 0; }
    case GL2_glUniform2f: glUniform2f(AI(0), AF(1), AF(2)); return 0;
    case GL2_glUniform3f: glUniform3f(AI(0), AF(1), AF(2), AF(3)); return 0;
    case GL2_glUniform4f: glUniform4f(AI(0), AF(1), AF(2), AF(3), AF(4)); return 0;

    /* --- data-carrying --- */
    case GL2_glBufferData: glBufferData(AI(0), (GLsizeiptr)AI(1), D, AI(2)); return 0;
    case GL2_glBufferSubData: glBufferSubData(AI(0), (GLintptr)AI(1), (GLsizeiptr)AI(2), D); return 0;
    case GL2_glCompressedTexImage2D: {
        static int c=0; if(c<20){c++; fprintf(stderr,"[texC] ifmt=0x%x %dx%d sz=%d data=%p\n",AI(2),AI(3),AI(4),AI(6),D);}
        glCompressedTexImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), D); return 0; }
    case GL2_glTexImage2D: {
        GLenum fmt=AI(6);
        int tid = (g_active_unit>=0&&g_active_unit<16)? g_unit_tex[g_active_unit] : 0;
        if(tid>=0&&tid<GLP_MAXTEX){ g_tex_fmt[tid]=(int)fmt; if(D) g_tex_uploaded[tid]=1; g_tex_w[tid]=(unsigned short)AI(3); g_tex_h[tid]=(unsigned short)AI(4); }
        static int c=0; if(c<20){c++; fprintf(stderr,"[texI] ifmt=0x%x %dx%d fmt=0x%x ty=0x%x data=%p%s\n",AI(2),AI(3),AI(4),AI(6),AI(7),D, is_lum_fmt(fmt)?" ->RGBA8":"");}
        if(is_lum_fmt(fmt) && AI(7)==0x1401 /*UBYTE*/){
            unsigned char *rgba=expand_rgba(fmt,AI(3),AI(4),(const unsigned char*)D,g_unpack_align);
            glp_tex_dump(tid,AI(3),AI(4),rgba);
            const unsigned char *up=glp_tex_swap(rgba,AI(3),AI(4)); if(!up) up=rgba;   /* edit->preview */
            glPixelStorei(GL_UNPACK_ALIGNMENT,4);
            glTexImage2D(AI(0), AI(1), GL_RGBA8, AI(3), AI(4), 0, 0x1908/*RGBA*/, 0x1401, up);
            free(rgba);
        } else if(fmt==0x1908 && AI(7)==0x1401){    /* RGBA/UBYTE: dump + swappable */
            glp_tex_dump(tid,AI(3),AI(4),(const unsigned char*)D);
            const unsigned char *up=glp_tex_swap((const unsigned char*)D,AI(3),AI(4));
            glTexImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), up?up:D);
        } else {
            glTexImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), D);
        }
        GLenum e=glGetError(); if(e){static int w=0;if(w<10){w++;fprintf(stderr,"[texI] glErr=0x%x\n",e);}} return 0; }
    case GL2_glTexSubImage2D: {
        GLenum fmt=AI(6);
        static int c=0; if(c<10){c++; fprintf(stderr,"[texSub] %dx%d fmt=0x%x ty=0x%x data=%p%s\n",AI(4),AI(5),AI(6),AI(7),D, is_lum_fmt(fmt)?" ->RGBA8":"");}
        if(is_lum_fmt(fmt) && AI(7)==0x1401){
            unsigned char *rgba=expand_rgba(fmt,AI(4),AI(5),(const unsigned char*)D,g_unpack_align);
            glPixelStorei(GL_UNPACK_ALIGNMENT,4);
            glTexSubImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), 0x1908/*RGBA*/, 0x1401, rgba);
            free(rgba);
        } else {
            glTexSubImage2D(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), D);
        }
        return 0; }
    case GL2_glDeleteBuffers: glDeleteBuffers(AI(0), (const GLuint*)D); return 0;
    case GL2_glDeleteFramebuffers: glDeleteFramebuffers(AI(0), (const GLuint*)D); return 0;
    case GL2_glDeleteRenderbuffers: glDeleteRenderbuffers(AI(0), (const GLuint*)D); return 0;
    case GL2_glDeleteTextures: glDeleteTextures(AI(0), (const GLuint*)D); return 0;
    case GL2_glDrawElements: {
        glp_guard_attribs("glDrawElements");
        { extern void glp_apply_blend(unsigned); extern unsigned glp_cur_prog(void); extern void glp_set_layermode(void); extern void glp_rg_dump(unsigned,int); glp_apply_blend(glp_cur_prog()); glp_set_layermode(); glp_rg_dump(AI(0),AI(1)); }
        static long nde=0; ++nde;
        while(glGetError()!=GL_NO_ERROR){}   /* drain sticky errors to isolate the draw */
        { extern void glp_program_probe(const char *,int,int,int,uintptr_t);
          glp_program_probe("before",AI(0),AI(1),AI(2),(uintptr_t)AI(3)); }
        int coverage_token;
        { extern int glp_coverage_probe_begin(int,int);
          coverage_token=glp_coverage_probe_begin(AI(0),AI(1)); }
        glDrawElements(AI(0), AI(1), AI(2), (const void*)(intptr_t)AI(3));
        { extern void glp_coverage_probe_end(int);
          glp_coverage_probe_end(coverage_token); }
        { extern void glp_program_probe(const char *,int,int,int,uintptr_t);
          glp_program_probe("after",AI(0),AI(1),AI(2),(uintptr_t)AI(3)); }
        { extern void glp_note_rt_write(void); glp_note_rt_write(); }
        { extern void glp_post_draw_probe(void); glp_post_draw_probe(); }
        { extern void glp_post_car_probe(int); glp_post_car_probe(AI(1)); }
        { extern void glp_post_614_probe(int); glp_post_614_probe(AI(1)); }
        if(nde<=160||nde%500==0){ extern unsigned glp_cur_prog(void);
            GLint fb=0,vp[4]={0}; glGetIntegerv(0x8CA6,&fb); glGetIntegerv(GL_VIEWPORT,vp);
            GLint tex0=0, act=0; glGetIntegerv(GL_ACTIVE_TEXTURE,&act);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0);
            glActiveTexture((GLenum)act);
            GLboolean dt=glIsEnabled(GL_DEPTH_TEST), bl=glIsEnabled(GL_BLEND), sc=glIsEnabled(0x0C11), cf=glIsEnabled(GL_CULL_FACE);
            GLenum e=glGetError();
            fprintf(stderr,"[draw] DE #%ld mode=0x%x count=%d prog=%u fbo=%d tex0=%d vp=%d,%d,%d,%d depth=%d blend=%d scis=%d cull=%d err=0x%x\n",
                nde,AI(0),AI(1),glp_cur_prog(),fb,tex0,
                vp[0],vp[1],vp[2],vp[3],dt,bl,sc,cf,e); }
        return 0; }
    case GL2_glUniform1fv: glUniform1fv(AI(0), AI(1), (const GLfloat*)D); return 0;
    case GL2_glUniform3fv: glUniform3fv(AI(0), AI(1), (const GLfloat*)D); return 0;
    case GL2_glUniform4fv: glUniform4fv(AI(0), AI(1), (const GLfloat*)D); return 0;
    case GL2_glUniformConsts: {   /* a3xx const-file -> ir3_c[base..]; args base,count; data floats */
        extern unsigned glp_cur_prog(void); extern int glp_ir3_c_loc(unsigned);
        unsigned prog = glp_cur_prog();
        int loc = prog ? glp_ir3_c_loc(prog) : -1;
        if (loc >= 0) glUniform4fv(loc + AI(0), AI(1), (const GLfloat*)D);
        return 0; }
    case GL2_glUniformMatrix2fv: glUniformMatrix2fv(AI(0), AI(1), AI(2), (const GLfloat*)D); return 0;
    case GL2_glUniformMatrix3fv: glUniformMatrix3fv(AI(0), AI(1), AI(2), (const GLfloat*)D); return 0;
    case GL2_glVertexAttribPointerVBO: {  /* offset into the currently-bound ARRAY_BUFFER */
        /* Mapped binary16 inputs are expanded honestly to float32 by
         * pm4gl_setup_attribs.  Never feed an unexpected raw OES half array to
         * Apple's Metal-backed GL 2.1: GL_HALF_FLOAT_ARB was observed to crash
         * inside gleRunVertexSubmitImmediate rather than report a GL error. */
        if (AI(2) == 0x8D61 /* GL_HALF_FLOAT_OES */) {
            static int warned;
            if (!warned++) fprintf(stderr,
                "[vap-vbo] unexpected raw GL_HALF_FLOAT_OES attr %d -> disabled\n",
                AI(0));
            glDisableVertexAttribArray(AI(0));
            return 0;
        }
        GLint ab=0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &ab);
        { static int w[16]={0}; int idx=AI(0); if(idx>=0&&idx<16&&!w[idx]){w[idx]=1;
            fprintf(stderr,"[vap-vbo] attr %d sz=%d ty=0x%x stride=%d offset=%d ARRAY_BUFFER_BINDING=%d\n",
                    idx,AI(1),AI(2),AI(4),AI(5),ab);} }
        glVertexAttribPointer(AI(0), AI(1), AI(2), (GLboolean)AI(3), AI(4),
                              (const void*)(intptr_t)AI(5)); return 0;
    }

    /* --- round-trip (returns) --- */
    case GL2_glCheckFramebufferStatus: {
        GLenum r = glCheckFramebufferStatus(AI(0));
        int32_t v = (int32_t)r; memcpy(reply, &v, 4); return 4;
    }
    case GL2_glGenBuffers: {
        GLsizei n = AI(0); if ((uint32_t)n*4 > cap) n = cap/4;
        glGenBuffers(n, (GLuint*)reply); return (uint32_t)n*4;
    }
    case GL2_glGenFramebuffers: {
        GLsizei n = AI(0); if ((uint32_t)n*4 > cap) n = cap/4;
        glGenFramebuffers(n, (GLuint*)reply); return (uint32_t)n*4;
    }
    case GL2_glGenRenderbuffers: {
        GLsizei n = AI(0); if ((uint32_t)n*4 > cap) n = cap/4;
        glGenRenderbuffers(n, (GLuint*)reply); return (uint32_t)n*4;
    }
    case GL2_glGenTextures: {
        GLsizei n = AI(0); if ((uint32_t)n*4 > cap) n = cap/4;
        glGenTextures(n, (GLuint*)reply); return (uint32_t)n*4;
    }
    case GL2_glGetAttribLocation: {
        GLint r = glGetAttribLocation(AI(0), (const char*)D);
        memcpy(reply, &r, 4); return 4;
    }
    case GL2_glGetFloatv: {
        GLfloat tmp[16]; memset(tmp,0,sizeof tmp); glGetFloatv(AI(0), tmp);
        uint32_t n = cap < sizeof tmp ? cap : sizeof tmp; memcpy(reply, tmp, n); return n;
    }
    case GL2_glGetIntegerv: {
        GLint tmp[16]; memset(tmp,0,sizeof tmp); glGetIntegerv(AI(0), tmp);
        uint32_t n = cap < sizeof tmp ? cap : sizeof tmp; memcpy(reply, tmp, n); return n;
    }
    case GL2_glGetString: {
        const GLubyte *s = glGetString(AI(0));
        const char *str = s ? (const char*)s : "";
        uint32_t n = (uint32_t)strlen(str) + 1; if (n > cap) n = cap;
        memcpy(reply, str, n); return n;
    }
    case GL2_glGetTexParameterfv: {
        GLfloat tmp[4]={0}; glGetTexParameterfv(AI(0), AI(1), tmp);
        uint32_t n = cap < sizeof tmp ? cap : sizeof tmp; memcpy(reply, tmp, n); return n;
    }
    case GL2_glGetTexParameteriv: {
        GLint tmp[4]={0}; glGetTexParameteriv(AI(0), AI(1), tmp);
        uint32_t n = cap < sizeof tmp ? cap : sizeof tmp; memcpy(reply, tmp, n); return n;
    }
    case GL2_glReadPixels: {
        /* x,y,w,h,f,ty -> pixels written straight into reply */
        glReadPixels(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), reply);
        return cap; /* guest sized the reply buffer to the exact pixel count */
    }
    default:
        /* pm4gl PM4GL_GL2_* extension ops live above GL2_FUNC_MAX (viewport,
         * clear, blend, glProgramGLSL/glUseProgram-by-slot). Route them to the
         * backend's honest-translator consumer. */
        if (h->func > GL2_FUNC_MAX) {
            extern int glp_gl2x(unsigned x, const int32_t *ai, int nargs,
                                const void *data, unsigned dl);
            /* glp_gl2x() returns a boolean "handled", not a reply byte count.
             * PM4GL extension records are fire-and-forget.  Returning that 1
             * through glp_gl2_dispatch made server_shm enqueue a bogus one-byte
             * WIRE_OP_REPLY for every viewport/state/RT operation; the PM4 guest
             * never consumes such replies, so h2c leaked and the host also
             * flooded the serial log with "[reply op=91 rlen=1]". */
            (void)glp_gl2x(h->func - GL2_FUNC_MAX, ai, h->nargs,
                           data, h->datalen);
            return 0;
        }
        return 0;
    }
}
