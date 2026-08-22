/*
 * Real-GL backend: maps the GLES2 subset onto desktop OpenGL 2.1 (what macOS
 * gives a legacy GLFW context). This is the host renderer that will sit behind
 * virtio-serial in milestone 1; here it is driven in-process by the same
 * encoder for a headless, pixel-verifiable proof.
 *
 * GLES->GL2.1 deltas handled:
 *   - strip "precision ...;" qualifiers (GLSL ES only) from shader source
 *   - client-side vertex arrays are legal in 2.1, so glVertexAttribPointer
 *     with a data pointer works directly; we keep the last array alive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include "backend.h"

typedef struct {
    unsigned char *vbuf[16];   /* per-attribute client array (kept alive for the draw) */
} gl_state;

static gl_state g_gl;

/* Translate GLSL-ES (Kanzi's shaders) to what a legacy desktop GLSL 1.20
 * context accepts: drop "precision ...;" statement lines, and #define away the
 * inline lowp/mediump/highp qualifiers (desktop 1.20 has no precision keywords).
 * A #version line, if present, must stay first, so the preamble goes after it.
 * Same approach emugl/ANGLE use for ES-on-desktop. */
/* desktop GLSL 1.20: precision keywords #define'd away; explicit #version so
 * Apple's compiler doesn't default to 110 (which rejects some constructs). */
static const char *GLES_PREAMBLE =
    "#version 120\n#define lowp\n#define mediump\n#define highp\n"
    /* GL_OES_EGL_image_external (rearview camera / video) shaders declare
     * `samplerExternalOES`; desktop GL 2.1 has no such type -> compile+link fail
     * (programs 21/36). Our textures are virtualized to RGBA8 anyway, so mapping it
     * to a plain sampler2D makes texture2D() work unchanged. (#extension dropped below.) */
    "#define samplerExternalOES sampler2D\n";

static char *strip_precision(const char *src) {
    size_t n = strlen(src);
    char *out = malloc(n + strlen(GLES_PREAMBLE) + 32), *w = out;
    const char *p = src;

    /* We emit our own #version 120 from the preamble, so DROP any #version the
     * shader declares (Kanzi ES shaders are mostly version-less anyway). */
    memcpy(w, GLES_PREAMBLE, strlen(GLES_PREAMBLE)); w += strlen(GLES_PREAMBLE);

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p + 1) : strlen(p);
        const char *s = p; while (*s==' '||*s=='\t') s++;
        /* Drop GLSL-ES-only / Kanzi-offline-compiler directives that Apple's
         * desktop GLSL compiler rejects (marking the shader "corrupt" at link):
         *   - "precision ...;" statements (no precision keywords on desktop)
         *   - "#pragma profilepragma blendoperation(...)" (Kanzi blend hint; the
         *     EAL sets real blend state via glBlendFunc at runtime, so dropping
         *     it changes nothing visually)
         *   - any "#version" the shader declares (we emit our own above) */
        int drop = (strncmp(s, "precision", 9) == 0)
                || (strncmp(s, "#pragma", 7) == 0)
                || (strncmp(s, "#extension", 10) == 0)   /* GL_OES_* not needed on desktop 2.1 */
                || (strncmp(s, "#version", 8) == 0);
        if (!drop) { memcpy(w, p, linelen); w += linelen; }
        if (!eol) break; p = eol + 1;
    }
    *w = 0; return out;
}

static void g_getDisplay(void *st, uint32_t id){ (void)st;(void)id; }
static int  g_initialize(void *st,int*ma,int*mi){ (void)st; *ma=1;*mi=4; return 1; }
static int  g_chooseConfig(void *st,const int32_t*a,uint32_t n,uint32_t*c){ (void)st;(void)a;(void)n; *c=1; return 1; }
static void g_createCtx(void *st,uint32_t c,uint32_t s){ (void)st;(void)c;(void)s; }
static void g_createWin(void *st,uint32_t c,uint32_t w,int ww,int hh){ (void)st;(void)c;(void)w;(void)ww;(void)hh; }
static void g_makeCurrent(void *st){ (void)st; }
extern void glp_rg_frame(void);
void glp_dump_layers(void);   /* fwd; defined after g_rt below */
void glp_note_rt_write(void); /* fwd; RT graph is defined below legacy callbacks */
static long g_swap_count;
static int glp_layer_dump_frame_ready(void){
    const char *e=getenv("PM4GL_DUMPLAYERS_FRAME");
    long min_frame=e ? strtol(e,NULL,10) : 0;
    return min_frame <= 0 || g_swap_count >= min_frame;
}
static void g_swap(void *st){ (void)st; g_swap_count++;
    if(g_swap_count<=60) fprintf(stderr,"[FRAME] swap #%ld\n",g_swap_count);
    if(getenv("PM4GL_DUMPLAYERS")) glp_dump_layers();
    glp_rg_frame(); glFlush(); }

static void g_viewport(void *st,int x,int y,int w,int h){ (void)st; glViewport(x,y,w,h); }
static void g_clearColor(void *st,float r,float g,float b,float a){ (void)st; glClearColor(r,g,b,a); }
static void g_clear(void *st,uint32_t m){ (void)st; static long n=0; if(n<400){n++; GLint fb=0; glGetIntegerv(0x8CA6,&fb); fprintf(stderr,"[CLEAR] mask=0x%x curFBO=%d\n",m,fb);} glClear(m); if(m&GL_COLOR_BUFFER_BIT) glp_note_rt_write(); }
static uint32_t g_createShader(void *st,uint32_t ty){ (void)st; return glCreateShader(ty); }
/* Track which shaders got REAL (non-empty) source from the EAL. strip_precision
 * always prepends a ~43-byte "#version 120 + #define" preamble, so even an empty
 * shader ends up with GL_SHADER_SOURCE_LENGTH>1 — which fooled inject_substitute
 * into skipping it (the shader then "links" with only a preamble, no main() ->
 * "Compiled shader was corrupt"). Use this table instead of source length. */
#define MAXSHADER 8192
static unsigned char g_shader_has_src[MAXSHADER];

/* Shader sources are durable reverse-engineering evidence. Keep them beside
 * the other render-work captures instead of in host /tmp, which is routinely
 * cleaned between sessions. run_hmi.sh supplies an absolute project path. */
static const char *glp_shader_dump_dir(void){
    const char *p=getenv("GLP_SHADER_DUMP_DIR");
    return (p && *p) ? p : "build/render_work/shaders/current";
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

/* Per-material blend, parsed from the Kanzi
 *   #pragma profilepragma blendoperation(gl_FragColor, rgbEq, rgbSrc, rgbDst, aEq, aSrc, aDst)
 * which strip_precision drops. EAL does NOT always set glBlendFunc per material
 * (it relies on this pragma), so without it some materials (e.g. the rotary
 * drop-shadow) blend wrong -> opaque black over the drum. We parse it here and
 * apply glBlendEquationSeparate/glBlendFuncSeparate per draw for that program. */
typedef struct { int valid; unsigned rgbEq,rgbSrc,rgbDst,aEq,aSrc,aDst; } glp_blend_t;
static glp_blend_t g_shader_blend[MAXSHADER];
#define GBMAXPROG 4096
static glp_blend_t g_prog_blend[GBMAXPROG];
static unsigned glp_blend_enum(const char *s, int n){
    struct { const char*k; unsigned v; } m[] = {
        {"GL_FUNC_ADD",0x8006},{"GL_FUNC_SUBTRACT",0x800A},{"GL_FUNC_REVERSE_SUBTRACT",0x800B},
        {"GL_ZERO",0},{"GL_ONE",1},
        {"GL_SRC_COLOR",0x0300},{"GL_ONE_MINUS_SRC_COLOR",0x0301},
        {"GL_DST_COLOR",0x0306},{"GL_ONE_MINUS_DST_COLOR",0x0307},
        {"GL_SRC_ALPHA",0x0302},{"GL_ONE_MINUS_SRC_ALPHA",0x0303},
        {"GL_DST_ALPHA",0x0304},{"GL_ONE_MINUS_DST_ALPHA",0x0305},
        {"GL_SRC_ALPHA_SATURATE",0x0308},{"GL_CONSTANT_COLOR",0x8001},
        {"GL_ONE_MINUS_CONSTANT_COLOR",0x8002},{"GL_CONSTANT_ALPHA",0x8003},
        {"GL_ONE_MINUS_CONSTANT_ALPHA",0x8004} };
    for(unsigned i=0;i<sizeof m/sizeof m[0];i++)
        if((int)strlen(m[i].k)==n && strncmp(s,m[i].k,(size_t)n)==0) return m[i].v;
    return 0xFFFFFFFFu;
}
/* parse the 6 blend args after the output arg; returns 1 if a full blendoperation found */
static int glp_parse_blend(const char *src, glp_blend_t *b){
    const char *p = strstr(src, "blendoperation(");
    if(!p) return 0;
    p += 15;                              /* past "blendoperation(" */
    const char *comma = strchr(p, ','); if(!comma) return 0;
    p = comma + 1;                        /* skip the output arg (gl_FragColor) */
    unsigned *out[6] = {&b->rgbEq,&b->rgbSrc,&b->rgbDst,&b->aEq,&b->aSrc,&b->aDst};
    for(int i=0;i<6;i++){
        while(*p==' '||*p=='\t') p++;
        const char *tok=p; while(*p && *p!=',' && *p!=')') p++;
        int n=(int)(p-tok); while(n>0 && (tok[n-1]==' '||tok[n-1]=='\t')) n--;
        unsigned v=glp_blend_enum(tok,n); if(v==0xFFFFFFFFu) return 0;
        *out[i]=v;
        if(*p==',') p++; else if(*p==')'){ if(i<5) return 0; }
    }
    b->valid=1; return 1;
}
void glp_apply_blend(unsigned prog){
    if(prog<GBMAXPROG && g_prog_blend[prog].valid){
        glp_blend_t *b=&g_prog_blend[prog];
        glBlendEquationSeparate(b->rgbEq,b->aEq);
        glBlendFuncSeparate(b->rgbSrc,b->rgbDst,b->aSrc,b->aDst);
    }
}

static void g_shaderSource(void *st,uint32_t s,const char*src){
    if(s<MAXSHADER) glp_parse_blend(src, &g_shader_blend[s]);   /* parse blendoperation */
    (void)st; char *fixed = strip_precision(src);
    /* a shader has REAL source only if the EAL sent non-whitespace text */
    { const char *q=src; while(*q==' '||*q=='\t'||*q=='\n'||*q=='\r') q++;
      if(s<MAXSHADER) g_shader_has_src[s] = (*q!=0); }
    /* dump every received source to a project file for offline inspection */
    { static int made=0; if(!made){made=1; glp_mkdirs(glp_shader_dump_dir());}
      char path[1200]; snprintf(path,sizeof path,"%s/sh_%03u.glsl",glp_shader_dump_dir(),s);
      FILE*f=fopen(path,"w"); if(f){ fprintf(f,"==RAW srclen=%zu==\n%s\n==FIXED==\n%s\n",strlen(src),src,fixed); fclose(f);} }
    fprintf(stderr,"[g_shaderSource] shader %u srclen=%zu fixedlen=%zu\n", s, strlen(src), strlen(fixed));
    const char *arr = fixed; glShaderSource(s, 1, &arr, 0); free(fixed);
}
static void g_compile(void *st,uint32_t s){ (void)st; glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; GLsizei n=0; glGetShaderInfoLog(s,sizeof log,&n,log);
        fprintf(stderr,"[g_compile] shader %u COMPILE FAILED: %.*s\n",s,(int)n,log); } }
static int32_t g_getShaderiv(void *st,uint32_t s,uint32_t p){ (void)st; GLint v=0; glGetShaderiv(s,p,&v);
    if(!v){ char log[512]; glGetShaderInfoLog(s,sizeof log,0,log); fprintf(stderr,"shader %u: %s\n",s,log);} return v; }
static uint32_t g_createProgram(void *st){ (void)st; uint32_t p=glCreateProgram(); GLenum e=glGetError(); fprintf(stderr,"[g_createProgram] -> %u (glErr=0x%x)\n",p,e); return p; }
#define MAXPROG 4096
static unsigned char g_prog_tex[MAXPROG];   /* program binds kzTextureCoordinate0? */
static void g_attach(void *st,uint32_t p,uint32_t s){ (void)st; glAttachShader(p,s); }
static void g_bindAttr(void *st,uint32_t p,uint32_t i,const char*nm){ (void)st; glBindAttribLocation(p,i,nm);
    if(p<MAXPROG && nm && strstr(nm,"kzTextureCoordinate0")) g_prog_tex[p]=1;
    static int n=0; if(n<16){n++; fprintf(stderr,"[g_bindAttr] prog %u loc %u name '%s'\n",p,i,nm);} }
/* current program (for gl2_dispatch sampler mirroring of legacy paths). */
static int g_subst_samp[MAXPROG];
static int g_subst_colorize[MAXPROG];   /* location of the glpColorize uniform, -1 if none */
static int g_subst_layermode[MAXPROG];  /* location of glpLayerMode uniform, -1 if none */
static int g_subst_lr_loc[MAXPROG];     /* location of LayerRenderTexture sampler, -1 if none */
static int g_subst_lc_loc[MAXPROG];     /* location of LayerCompositionTexture sampler, -1 if none */
static unsigned char g_prog_layermode[MAXPROG]; /* 0=atlas, 1=layer-render, 2=layer-composition */
static unsigned char g_prog_exact[MAXPROG]; /* real PM4/ir3 program, never a legacy substitute */
static uint32_t g_cur_prog = 0;
int  glp_cur_subst_sampler(void){
    return (g_cur_prog<MAXPROG && !g_prog_exact[g_cur_prog])
         ? g_subst_samp[g_cur_prog] : -1;
}

/* ir3_bind_sampler_name() deliberately preserves the hardware texture index:
 * sampler/texture #N is emitted as uniform `texN`.  Unlike fixed-function
 * Adreno state, a newly linked GL program initializes every sampler uniform to
 * texture unit zero.  Explicitly restore the a3xx mapping or multi-texture
 * shaders silently sample tex0 for tex1/tex2 as well. */
static void pm4gl_bind_ir3_sampler_units(GLuint prog){
    GLint previous=0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&previous);
    glUseProgram(prog);
    for(int unit=0; unit<16; unit++){
        char name[16];
        snprintf(name,sizeof name,"tex%d",unit);
        GLint loc=glGetUniformLocation(prog,name);
        if(loc>=0){
            glUniform1i(loc,unit);
            fprintf(stderr,"[SAMP] prog=%u %s -> unit=%d\n",prog,name,unit);
        }
    }
    glUseProgram((GLuint)previous);
}
unsigned glp_cur_prog(void){ return g_cur_prog; }

/* ===== honest PM4->GL2 translator extension-op consumer =========================
 * build/pm4_gl2 emits, beyond the standard GL2_* records, a set of composite/
 * extension ops (enum pm4gl_gl2x in pm4gl.h, all = GL2_FUNC_MAX + 1 + k):
 *   1 glViewport 2 glDepthRangef 3 glClear 4 glClearColor 5 glEnable 6 glDisable
 *   7 glCullFace 8 glFrontFace 9 glBlendEquationSeparate 10 glEnableVertexAttribArray
 *   11 glUseProgram(slot) 12 glUniformMatrix4fv 13 glProgramGLSL(slot; "VS\0FS\0")
 * gl2_dispatch routes ops > GL2_FUNC_MAX here. Without this the decompiled ir3
 * program is never compiled/linked/bound and every frame is a black clear. */
static GLuint g_slot_prog[256];   /* pm4gl program slot -> linked GL program */
static float x_f(int32_t i){ union{int32_t i;float f;}u; u.i=i; return u.f; }
static GLuint pm4gl_compile(GLenum type, const char *src){
    char *fixed = strip_precision(src);
    GLuint s = glCreateShader(type);
    const char *p = fixed; glShaderSource(s, 1, &p, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok){ char log[1024]; glGetShaderInfoLog(s, sizeof log, 0, log);
        fprintf(stderr, "[pm4gl] %s COMPILE FAIL: %s\n---SRC-BEGIN---\n%s\n---SRC-END---\n", type==GL_VERTEX_SHADER?"VS":"FS", log, fixed); }
    free(fixed);
    return s;
}
/* Parse the payload after `attribute ` / `varying `. GLSL ES permits an
 * optional precision qualifier between storage and type:
 *     varying mediump vec4 v0;
 * Treating `mediump` as the type produced `varying mediump vec4;` and an
 * assignment to the type name. */
static const char *pm4gl_decl_token(const char *p, char *out, int cap){
    while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
    int i=0;
    while(*p && *p!=' ' && *p!='\t' && *p!='\n' && *p!='\r' &&
          *p!=';' && *p!='[' && i<cap-1) out[i++]=*p++;
    out[i]=0;
    return p;
}
static int pm4gl_parse_decl(const char *p, char *qual, int qcap,
                            char *ty, int tcap, char *nm, int ncap){
    char first[16];
    qual[0]=ty[0]=nm[0]=0;
    p=pm4gl_decl_token(p,first,sizeof first);
    if(!strcmp(first,"lowp")||!strcmp(first,"mediump")||!strcmp(first,"highp")){
        strncpy(qual,first,(size_t)qcap-1); qual[qcap-1]=0;
        p=pm4gl_decl_token(p,ty,tcap);
    } else {
        strncpy(ty,first,(size_t)tcap-1); ty[tcap-1]=0;
    }
    (void)pm4gl_decl_token(p,nm,ncap);
    return ty[0] && nm[0];
}
/* Bind real attributes in declaration order.  The three appended bridge
 * attributes use stable high locations shared with vfd_draw.c.  They must not
 * be based on VFD nattr: exact ir3 dead-input elimination means a draw can have
 * two VFD streams but only one declared VS attribute. */
#define GLP_FIXED_ATTR_BASE 13u
static int pm4gl_vfd_attr_loc(const char *name, unsigned *loc){
    static const char prefix[] = "ir3_in_r";
    if(strncmp(name,prefix,sizeof prefix-1)!=0) return 0;
    const char *p=name+sizeof prefix-1;
    if(*p<'0'||*p>'9') return 0;
    char *end=NULL;
    unsigned long r=strtoul(p,&end,10);
    if(!end||*end||r>=GLP_FIXED_ATTR_BASE) return 0;
    *loc=(unsigned)r;
    return 1;
}
static void pm4gl_bind_attrs(GLuint prog, const char *vs){
    const char *s = vs; unsigned loc = 0;
    while ((s = strstr(s, "attribute "))){
        char q[16],ty[16],nm[64];
        unsigned vfdloc=0;
        s += 10;
        if(!pm4gl_parse_decl(s,q,sizeof q,ty,sizeof ty,nm,sizeof nm))
            continue;
        if(pm4gl_vfd_attr_loc(nm,&vfdloc))
            glBindAttribLocation(prog,vfdloc,nm);
        else if(!strcmp(nm,"a_glp_rectpos"))
            glBindAttribLocation(prog, GLP_FIXED_ATTR_BASE + 0u, nm);
        else if(!strcmp(nm,"a_pscoord"))
            glBindAttribLocation(prog, GLP_FIXED_ATTR_BASE + 1u, nm);
        else if(!strcmp(nm,"a_glp_texcoord"))
            glBindAttribLocation(prog, GLP_FIXED_ATTR_BASE + 2u, nm);
        else if(loc < GLP_FIXED_ATTR_BASE)
            glBindAttribLocation(prog, loc++, nm);
        else
            fprintf(stderr,
                    "[pm4gl] too many real VS attributes; cannot bind '%s'\n",
                    nm);
    }
}
/* Desktop GLSL 1.20 requires every FS `varying` to be written by the VS, or the
 * link fails with "Input of fragment shader 'vN' not written by vertex shader".
 * The ir3 decompiler recovers FS varyings from bary.f/flat.b, but the matching
 * VS may emit no varying outputs at all. Inject, into the VS: a matching varying
 * decl (before `void main`) + a passthrough write (before the final `}`). The
 * The a3xx fixed-function inputs which are absent from the decompiled VS are
 * appended after the real VFD attributes in a stable order.  The first missing
 * vec2 is the VPC point/sprite coordinate; further missing vec2 inputs use an
 * explicit alias of real VFD stream 1 (the captured Kanzi UV stream). Other
 * types still fall back to the first same-type VS attribute or zero. Returns a
 * newly-malloc'd VS. */
static const char *pm4gl_zero_for(const char *ty){
    if(!strcmp(ty,"float")) return "0.0";
    if(!strcmp(ty,"vec2"))  return "vec2(0.0)";
    if(!strcmp(ty,"vec3"))  return "vec3(0.0)";
    return "vec4(0.0)";                         /* vec4 / fallback */
}
/* first VS `attribute <ty> <name>;` whose type == ty; name copied into out (n). */
static int pm4gl_attr_of_type(const char *vs, const char *ty, char *out, int n){
    const char *s = vs;
    while((s = strstr(s, "attribute "))){
        char q[16],at[16],nm[64]; s += 10;
        if(!pm4gl_parse_decl(s,q,sizeof q,at,sizeof at,nm,sizeof nm)) continue;
        if(nm[0] && !strcmp(at,ty)){ strncpy(out,nm,n-1); out[n-1]=0; return 1; }
    }
    return 0;
}
static const char *pm4gl_swizzle_for(const char *ty){
    if(!strcmp(ty,"float")) return ".x";
    if(!strcmp(ty,"vec2"))  return ".xy";
    if(!strcmp(ty,"vec3"))  return ".xyz";
    return "";                                   /* vec4: whole */
}
/* name of the FIRST VS attribute (= the position stream). */
static int pm4gl_first_attr(const char *vs, char *out, int n){
    const char *s = strstr(vs, "attribute ");
    if(!s) return 0;
    char q[16],ty[16],nm[64];
    if(!pm4gl_parse_decl(s+10,q,sizeof q,ty,sizeof ty,nm,sizeof nm)) return 0;
    strncpy(out,nm,(size_t)n-1); out[n-1]=0;
    return out[0]!=0;
}
/* Exact external I/O is emitted as one scalar `vN` per VPC location.  The
 * observed a3xx PS_REPL=0x99999999 maps even locations to S and odd locations
 * to T (01,10 repeated).  Before scalar linkage landed, the equivalent missing
 * vec2 happened to use a_pscoord; leaving scalar v0/v1 at the generic 0.0
 * fallback made every RECTLIST sample exactly texel (0,0), producing uniform
 * black/17/73 surfaces despite coherent atlas uploads. */
static int pm4gl_scalar_varying_component(const char *name, const char *base,
                                          char *out, int n){
    if (!name || name[0] != 'v' || name[1] < '0' || name[1] > '9')
        return 0;
    char *end = NULL;
    unsigned long loc = strtoul(name + 1, &end, 10);
    if (!end || *end || loc > 127ul)
        return 0;
    snprintf(out, (size_t)n, "%s.%c", base, (loc & 1ul) ? 'y' : 'x');
    return 1;
}
static int pm4gl_scalar_varying_pscoord(const char *name, char *out, int n){
    return pm4gl_scalar_varying_component(name, "a_pscoord", out, n);
}
static char *pm4gl_reconcile_varyings(const char *vs, const char *fs){
    struct { char qual[16]; char type[16]; char name[32]; } miss[32]; int nmiss=0;
    struct { char name[32]; char value[32]; } ps_scalar[32]; int nps_scalar=0;
    const char *p = fs;
    while((p = strstr(p, "varying "))){
        char qual[16],ty[16],nm[32]; p += 8;
        if(!pm4gl_parse_decl(p,qual,sizeof qual,ty,sizeof ty,nm,sizeof nm)) continue;
        /* PS_REPL happens after the real VS.  Track every scalar VPC location,
         * including ones the VS already declares/writes, so RECTLIST can
         * override an exported zero just as the a3xx fixed-function stage does. */
        if(!strcmp(ty,"float") && nps_scalar<32){
            char value[32];
            if(pm4gl_scalar_varying_pscoord(nm,value,sizeof value)){
                int dup=0;
                for(int k=0;k<nps_scalar;k++)
                    if(!strcmp(ps_scalar[k].name,nm)){dup=1;break;}
                if(!dup){
                    strncpy(ps_scalar[nps_scalar].name,nm,31);
                    ps_scalar[nps_scalar].name[31]=0;
                    strncpy(ps_scalar[nps_scalar].value,value,31);
                    ps_scalar[nps_scalar].value[31]=0;
                    nps_scalar++;
                }
            }
        }
        /* already declared by the VS? compare against the VS's varying names */
        int have=0; const char *q=vs;
        while((q=strstr(q,"varying "))){
            const char *e=strchr(q,';'); if(!e) break;
            const char *t=e; while(t>q && t[-1]!=' ' && t[-1]!='\t' && t[-1]!='\n') t--;
            size_t L=(size_t)(e-t);
            if(L==strlen(nm) && strncmp(t,nm,L)==0){ have=1; break; }
            q=e+1;
        }
        if(have) continue;
        int dup=0; for(int k=0;k<nmiss;k++) if(!strcmp(miss[k].name,nm)){dup=1;break;}
        if(dup) continue;
        if(nmiss<32){ strncpy(miss[nmiss].qual,qual,15); miss[nmiss].qual[15]=0;
                      strncpy(miss[nmiss].type,ty,15); miss[nmiss].type[15]=0;
                      strncpy(miss[nmiss].name,nm,31); miss[nmiss].name[31]=0; nmiss++; }
    }
    const char *pm = strstr(vs, "void main");
    const char *pb = strrchr(vs, '}');
    if(!pm || !pb || pb < pm) return strdup(vs);     /* unexpected shape: leave as-is */
    /* The FIRST missing vec2 varying is the FS texcoord that a3xx may replace
     * via VPC_VARYING_PS_REPL.  Its ordinary value, like further missing vec2
     * inputs, comes from the real VFD stream 1; only glp_rectmode replaces the
     * first one with point/sprite S/T after the VS. Declare all three inputs
     * AFTER the real attributes, in an unconditional stable order.  Linkage
     * assigns them fixed locations 13/14/15 (rectpos/pscoord/VFD1 UV), shared
     * with pm4gl_setup_attribs. glp_rectmode is set only for the fixed-function
     * RECTLIST path; normal geometry keeps gl_Position. */
    int stc = -1;
    for(int k=0;k<nmiss;k++) if(!strcmp(miss[k].type,"vec2")){ stc=k; break; }
    char *out = malloc(strlen(vs) + (size_t)nmiss*192 +
                       (size_t)nps_scalar*128 + 448), *w = out;
    size_t head=(size_t)(pm-vs); memcpy(w,vs,head); w+=head;               /* up to void main */
    w += sprintf(w,
        "attribute vec2 a_glp_rectpos;\n"
        "attribute vec2 a_pscoord;\n"
        "attribute vec2 a_glp_texcoord;\n"
        "uniform float glp_rectmode;\n"
        "uniform float glp_rtflip;\n");
    for(int k=0;k<nmiss;k++) w += sprintf(w,"varying %s%s%s %s;\n",
        miss[k].qual, miss[k].qual[0]?" ":"", miss[k].type, miss[k].name);
    size_t mid=(size_t)(pb-pm); memcpy(w,pm,mid); w+=mid;                  /* main body up to final } */
    for(int k=0;k<nmiss;k++){
        char attr[64], val[96];
        if(!strcmp(miss[k].type,"float") &&
           pm4gl_scalar_varying_component(miss[k].name,"a_glp_texcoord",
                                          val,sizeof val))
            ;                                                               /* real VFD stream-1 UV */
        else if(!strcmp(miss[k].type,"vec2"))
            snprintf(val,sizeof val,"a_glp_texcoord");                     /* real VFD stream-1 UV */
        else if(pm4gl_attr_of_type(vs, miss[k].type, attr, sizeof attr))
            snprintf(val,sizeof val,"%s",attr);
        else
            snprintf(val,sizeof val,"%s",pm4gl_zero_for(miss[k].type));
        w += sprintf(w,"    %s = %s;\n", miss[k].name, val);
    }
    if(nps_scalar || stc>=0){
        w += sprintf(w,"    if (glp_rectmode > 0.5) {\n");
        if(stc>=0)
            w += sprintf(w,
                "        %s = vec2(a_pscoord.x, mix(a_pscoord.y, "
                "1.0-a_pscoord.y, glp_rtflip));\n", miss[stc].name);
        for(int k=0;k<nps_scalar;k++){
            if(!strcmp(ps_scalar[k].value,"a_pscoord.y"))
                w += sprintf(w,
                    "        %s = mix(a_pscoord.y, 1.0-a_pscoord.y, "
                    "glp_rtflip);\n", ps_scalar[k].name);
            else
                w += sprintf(w,"        %s = %s;\n",
                             ps_scalar[k].name, ps_scalar[k].value);
        }
        w += sprintf(w,"    }\n");
    }
    w += sprintf(w,"    if (glp_rectmode > 1.5) gl_Position = vec4(a_glp_rectpos, 0.0, 1.0);\n");
    strcpy(w, pb);                                                        /* final } onward */
    (void)pm4gl_swizzle_for; (void)pm4gl_first_attr;
    fprintf(stderr,"[pm4gl] reconciled %d FS varying(s) into VS\n", nmiss);
    return out;
}
/* WALL3: per-render-target FBO map. The guest speaks PM4 (never glBindFramebuffer)
 * and renders each Kanzi layer to its own offscreen surface (RB_MRT_BUF_BASE
 * gpuaddr), then composites the layers onto the main framebuffer. We give each
 * layer gpuaddr its OWN texture-backed FBO so layers stop stomping the one shared
 * FBO, and bind that texture when the layer is later sampled (glBindTexGA). */
#define RT_MAX 128
#define GLP_DISPLAY_GA 0x10000000u
static struct {
    uint32_t ga;
    GLuint fbo, tex, snapshot;
    int w, h;
    unsigned char written;  /* draw/clear has produced ordered GL contents */
    unsigned char ready;    /* a GMEM resolve made the memory surface visible */
} g_rt[RT_MAX];
static int g_rt_n;
static int g_cur_rt = -1;
static int g_present_rt = -1;
/* The reusable collapsed-GMEM texture only grows, but each render pass still
 * has its own logical extent.  Keep those two concepts separate: using the
 * 1024x480 backing as the viewport for a later 614x314 offscreen pass scales
 * its geometry into the wrong surface before the 614x314 resolve crops it. */
static int g_cur_rt_w = 1024, g_cur_rt_h = 480;   /* logical extent of this pass */
static int g_pm4_tile_render;                       /* current viewport is a tile sweep */
static GLuint g_transparent_tex;
extern int glp_default_fbo;

static int glp_rt_index(uint32_t ga){
    for (int i=0;i<g_rt_n;i++) if (g_rt[i].ga==ga) return i;
    return -1;
}

/* Allocate (or grow) one texture-backed system-memory surface.  gpuaddr zero is
 * intentional: it is our explicit backing for the collapsed on-chip GMEM, no
 * longer an alias for the QEMU presentation FBO.  Never shrink an allocation;
 * Kanzi reuses GMEM for 16x8, 1024x96, and 1024x480 logical passes. */
static int glp_rt_ensure(uint32_t ga, int w, int h){
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int idx = glp_rt_index(ga);
    if (idx < 0) {
        if (g_rt_n >= RT_MAX) return -1;
        idx = g_rt_n++;
        memset(&g_rt[idx], 0, sizeof g_rt[idx]);
        g_rt[idx].ga = ga;
        glGenTextures(1, &g_rt[idx].tex);
        glBindTexture(GL_TEXTURE_2D, g_rt[idx].tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &g_rt[idx].fbo);
    }
    if (w > g_rt[idx].w || h > g_rt[idx].h) {
        int nw = w > g_rt[idx].w ? w : g_rt[idx].w;
        int nh = h > g_rt[idx].h ? h : g_rt[idx].h;
        size_t bytes = (size_t)nw * (size_t)nh * 4u;
        unsigned char *zero = bytes ? (unsigned char *)calloc(1, bytes) : NULL;
        glBindTexture(GL_TEXTURE_2D, g_rt[idx].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nw, nh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zero);
        free(zero);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt[idx].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_rt[idx].tex, 0);
        g_rt[idx].w = nw;
        g_rt[idx].h = nh;
        g_rt[idx].written = 0;
        g_rt[idx].ready = 0;
        if (g_rt[idx].snapshot) {
            glDeleteTextures(1, &g_rt[idx].snapshot);
            g_rt[idx].snapshot = 0;
        }
    }
    if (getenv("GLP_RG_DUMP") && w == 614 && h == 314)
        fprintf(stderr,
                "[RT614ENSURE] ga=0x%08x idx=%d fbo=%u tex=%u "
                "backing=%dx%d written=%u ready=%u\n",
                ga,idx,g_rt[idx].fbo,g_rt[idx].tex,g_rt[idx].w,g_rt[idx].h,
                g_rt[idx].written,g_rt[idx].ready);
    return idx;
}

static GLuint glp_transparent_texture(GLenum target){
    if (!g_transparent_tex) {
        static const unsigned char zero[4] = {0, 0, 0, 0};
        glGenTextures(1, &g_transparent_tex);
        glBindTexture(target, g_transparent_tex);
        glTexImage2D(target, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zero);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return g_transparent_tex;
}

/* GL commands are processed in order, so a completed draw/clear is immediately
 * sampleable by a later draw to another RT.  It is not yet a memory-surface
 * resolve, hence `ready` remains reserved for glResolveToGA. */
void glp_note_rt_write(void){
    if (g_cur_rt >= 0 && g_cur_rt < g_rt_n)
        g_rt[g_cur_rt].written = 1;
}

/* Bounded forensic probe for the first real 512x128 preparatory blit.  Its
 * uploaded source is known-good, while the destination RT was observed black.
 * Query the state that desktop GL actually sees (not the guest shadow) and
 * read the attachment immediately after the draw, before a resolve/composite
 * can hide the producer failure.  GLP_RG_DUMP already denotes a diagnostic
 * boot, so keep this behind the same switch and cap it hard. */
void glp_post_draw_probe(void){
    static int n;
    if (!getenv("GLP_RG_DUMP") || n >= 8 ||
        g_cur_rt < 0 || g_cur_rt >= g_rt_n ||
        g_cur_rt_w != 512 || g_cur_rt_h != 128)
        return;
    n++;

    GLint actual_prog=0, fbo=0, vp[4]={0}, sc[4]={0}, act=GL_TEXTURE0;
    GLboolean cm[4]={0};
    glGetIntegerv(GL_CURRENT_PROGRAM, &actual_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    GLenum fbstat=glCheckFramebufferStatus(GL_FRAMEBUFFER);

    GLint rloc=actual_prog ? glGetUniformLocation((GLuint)actual_prog,
                                                  "glp_rectmode") : -1;
    GLfloat rectmode=-1.0f;
    if (rloc >= 0) glGetUniformfv((GLuint)actual_prog, rloc, &rectmode);

    fprintf(stderr,
            "[POST512] n=%d tracked_prog=%u actual_prog=%d rtga=0x%08x "
            "fbo=%d status=0x%x rectloc=%d rect=%.3g "
            "vp=%d,%d+%dx%d sc=%d,%d+%dx%d scis=%d blend=%d "
            "mask=%d%d%d%d\n",
            n,g_cur_prog,actual_prog,g_rt[g_cur_rt].ga,fbo,fbstat,
            rloc,rectmode,vp[0],vp[1],vp[2],vp[3],
            sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_SCISSOR_TEST),glIsEnabled(GL_BLEND),
            cm[0],cm[1],cm[2],cm[3]);

    static const char *aname[] = {
        "a_glp_rectpos", "a_pscoord", "a_glp_texcoord"
    };
    for (unsigned i=0; i<sizeof(aname)/sizeof(aname[0]); i++) {
        GLint loc=actual_prog ? glGetAttribLocation((GLuint)actual_prog,
                                                    aname[i]) : -1;
        GLint en=0, buf=0, size=0, stride=0, type=0;
        if (loc >= 0) {
            glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
            glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
            glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                &stride);
            glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        }
        fprintf(stderr,
                "[POST512ATTR] %s loc=%d en=%d buf=%d size=%d stride=%d type=0x%x\n",
                aname[i],loc,en,buf,size,stride,type);
    }

    if (actual_prog) {
        GLint nu=0;
        glGetProgramiv((GLuint)actual_prog, GL_ACTIVE_UNIFORMS, &nu);
        for (GLint i=0; i<nu; i++) {
            char name[64]; GLsizei len=0; GLint sz=0; GLenum ty=0;
            glGetActiveUniform((GLuint)actual_prog, (GLuint)i, sizeof name,
                               &len, &sz, &ty, name);
            if (ty != GL_SAMPLER_2D) continue;
            GLint loc=glGetUniformLocation((GLuint)actual_prog, name), unit=-1;
            if (loc >= 0) glGetUniformiv((GLuint)actual_prog, loc, &unit);
            GLint tex=0, tw=0, th=0;
            if (unit >= 0 && unit < 16) {
                glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
                if (tex) {
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                             GL_TEXTURE_WIDTH, &tw);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                                             GL_TEXTURE_HEIGHT, &th);
                }
            }
            fprintf(stderr,
                    "[POST512SAMP] %s loc=%d unit=%d tex=%d wh=%dx%d\n",
                    name,loc,unit,tex,tw,th);
        }
    }
    glActiveTexture((GLenum)act);

    size_t npix=(size_t)g_cur_rt_w*(size_t)g_cur_rt_h;
    unsigned char *px=(unsigned char *)malloc(npix*4u);
    if (px) {
        glReadPixels(0,0,g_cur_rt_w,g_cur_rt_h,GL_RGBA,GL_UNSIGNED_BYTE,px);
        size_t rgbnz=0, anz=0, aopaque=0;
        unsigned rmax=0,gmax=0,bmax=0,amin=255,amax=0;
        uint32_t hash=2166136261u;
        for (size_t i=0; i<npix; i++) {
            const unsigned char *p=px+i*4u;
            if (p[0] || p[1] || p[2]) rgbnz++;
            if (p[3]) anz++;
            if (p[3] == 255) aopaque++;
            if (p[0] > rmax) rmax=p[0];
            if (p[1] > gmax) gmax=p[1];
            if (p[2] > bmax) bmax=p[2];
            if (p[3] < amin) amin=p[3];
            if (p[3] > amax) amax=p[3];
            for (int c=0; c<4; c++) {
                hash ^= p[c];
                hash *= 16777619u;
            }
        }
        fprintf(stderr,
                "[POST512PIX] rgbnz=%lu anz=%lu opaque=%lu "
                "rgbmax=%u,%u,%u arange=%u..%u hash=%08x err=0x%x\n",
                (unsigned long)rgbnz,(unsigned long)anz,(unsigned long)aopaque,
                rmax,gmax,bmax,amin,amax,hash,glGetError());
        extern const char *glp_tex_dump_dir(void);
        char path[1200];
        snprintf(path,sizeof path,"%s/post512_%02d.ppm",
                 glp_tex_dump_dir(),n);
        FILE *f=fopen(path,"wb");
        if (f) {
            fprintf(f,"P6\n%d %d\n255\n",g_cur_rt_w,g_cur_rt_h);
            for (int y=g_cur_rt_h-1; y>=0; y--)
                for (int x=0; x<g_cur_rt_w; x++) {
                    const unsigned char *p=px+
                        ((size_t)y*(size_t)g_cur_rt_w+(size_t)x)*4u;
                    fwrite(p,1,3,f);
                }
            fclose(f);
        }
        free(px);
    }
}

/* The real 3D car mesh is the distinctive 11070-index draw.  Sample the
 * collapsed GMEM attachment immediately after each of its first tile replays,
 * so we can distinguish "shader produced no pixels" from a later resolve or
 * composition overwrite. */
void glp_post_car_probe(int count){
    static int n;
    if (!getenv("GLP_RG_DUMP") || count != 11070 || n >= 10 ||
        g_cur_rt < 0 || g_cur_rt >= g_rt_n)
        return;
    n++;

    GLint actual=0,fbo=0,vp[4]={0},sc[4]={0},act=GL_TEXTURE0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&actual);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&act);
    fprintf(stderr,
            "[POSTCAR] n=%d tracked=%u actual=%d rtga=0x%08x wh=%dx%d "
            "fbo=%d vp=%d,%d+%dx%d sc=%d,%d+%dx%d scis=%d blend=%d\n",
            n,g_cur_prog,actual,g_rt[g_cur_rt].ga,g_cur_rt_w,g_cur_rt_h,
            fbo,vp[0],vp[1],vp[2],vp[3],sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_SCISSOR_TEST),glIsEnabled(GL_BLEND));
    for (int unit=0; unit<3; unit++) {
        GLint tex=0,tw=0,th=0;
        glActiveTexture((GLenum)(GL_TEXTURE0+unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);
        if (tex) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
        }
        fprintf(stderr,"[POSTCARTEX] unit=%d tex=%d wh=%dx%d\n",
                unit,tex,tw,th);
    }
    glActiveTexture((GLenum)act);

    int w=g_cur_rt_w, h=g_cur_rt_h;
    size_t npix=(size_t)w*(size_t)h;
    unsigned char *px=(w>0 && h>0) ?
        (unsigned char *)malloc(npix*4u) : NULL;
    if (!px) return;
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    size_t rgbnz=0,anz=0;
    int x0=w,y0=h,x1=-1,y1=-1;
    unsigned maxrgb=0,amax=0;
    uint32_t hash=2166136261u;
    for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
        const unsigned char *p=px+
            ((size_t)y*(size_t)w+(size_t)x)*4u;
        if (p[0] || p[1] || p[2]) {
            rgbnz++;
            if (x<x0)x0=x; if (x>x1)x1=x;
            if (y<y0)y0=y; if (y>y1)y1=y;
        }
        if (p[3]) anz++;
        if (p[0]>maxrgb)maxrgb=p[0];
        if (p[1]>maxrgb)maxrgb=p[1];
        if (p[2]>maxrgb)maxrgb=p[2];
        if (p[3]>amax)amax=p[3];
        for(int c=0;c<4;c++){hash^=p[c];hash*=16777619u;}
    }
    fprintf(stderr,
            "[POSTCARPIX] rgbnz=%lu anz=%lu bbox=%d,%d-%d,%d "
            "maxrgb=%u amax=%u hash=%08x err=0x%x\n",
            (unsigned long)rgbnz,(unsigned long)anz,x0,y0,x1,y1,
            maxrgb,amax,hash,glGetError());
    extern const char *glp_tex_dump_dir(void);
    char path[1200];
    snprintf(path,sizeof path,"%s/postcar_%02d.ppm",
             glp_tex_dump_dir(),n);
    FILE *f=fopen(path,"wb");
    if (f) {
        fprintf(f,"P6\n%d %d\n255\n",w,h);
        for (int y=h-1;y>=0;y--)
            for (int x=0;x<w;x++)
                fwrite(px+((size_t)y*(size_t)w+(size_t)x)*4u,1,3,f);
        fclose(f);
    }
    free(px);
}

static void glp_probe_rt614(const char *where, int count){
    static int n;
    if (!getenv("GLP_RG_DUMP") || n >= 30 ||
        g_cur_rt < 0 || g_cur_rt >= g_rt_n ||
        g_cur_rt_w != 614 || g_cur_rt_h != 314)
        return;
    n++;
    GLint actual=0,fbo=0,vp[4]={0},sc[4]={0},act=GL_TEXTURE0;
    GLfloat clear[4]={0};
    glGetIntegerv(GL_CURRENT_PROGRAM,&actual);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&act);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);
    fprintf(stderr,
            "[POST614] n=%d where=%s count=%d tracked=%u actual=%d "
            "fbo=%d vp=%d,%d+%dx%d sc=%d,%d+%dx%d scis=%d blend=%d "
            "clear=%.3g,%.3g,%.3g,%.3g\n",
            n,where,count,g_cur_prog,actual,fbo,
            vp[0],vp[1],vp[2],vp[3],sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_SCISSOR_TEST),glIsEnabled(GL_BLEND),
            clear[0],clear[1],clear[2],clear[3]);
    if(actual){
        static const int ci[] = {0,128,129};
        for(unsigned i=0;i<sizeof(ci)/sizeof(ci[0]);i++){
            char name[32];
            GLfloat v[4]={-99,-99,-99,-99};
            snprintf(name,sizeof name,"ir3_c[%d]",ci[i]);
            GLint loc=glGetUniformLocation((GLuint)actual,name);
            if(loc>=0)glGetUniformfv((GLuint)actual,loc,v);
            fprintf(stderr,
                    "[POST614CONST] c%d loc=%d v=%.7g,%.7g,%.7g,%.7g\n",
                    ci[i],loc,v[0],v[1],v[2],v[3]);
        }
    }
    for(int unit=0;unit<4;unit++){
        GLint tex=0,tw=0,th=0;
        glActiveTexture((GLenum)(GL_TEXTURE0+unit));
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);
        if(tex){
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
        }
        fprintf(stderr,"[POST614TEX] unit=%d tex=%d wh=%dx%d\n",
                unit,tex,tw,th);
    }
    glActiveTexture((GLenum)act);

    int w=g_cur_rt_w,h=g_cur_rt_h;
    size_t npix=(size_t)w*(size_t)h;
    unsigned char *px=(unsigned char *)malloc(npix*4u);
    if(!px)return;
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    size_t rgbnz=0,anz=0,white=0;
    int x0=w,y0=h,x1=-1,y1=-1;
    unsigned maxrgb=0,amax=0;
    uint32_t hash=2166136261u;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        const unsigned char *p=px+
            ((size_t)y*(size_t)w+(size_t)x)*4u;
        if(p[0]||p[1]||p[2]){
            rgbnz++; if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;if(y>y1)y1=y;
        }
        if(p[3])anz++;
        if(p[0]==255&&p[1]==255&&p[2]==255&&p[3]==255)white++;
        if(p[0]>maxrgb)maxrgb=p[0];if(p[1]>maxrgb)maxrgb=p[1];
        if(p[2]>maxrgb)maxrgb=p[2];if(p[3]>amax)amax=p[3];
        for(int c=0;c<4;c++){hash^=p[c];hash*=16777619u;}
    }
    fprintf(stderr,
            "[POST614PIX] where=%s rgbnz=%lu anz=%lu white=%lu "
            "bbox=%d,%d-%d,%d maxrgb=%u amax=%u hash=%08x err=0x%x\n",
            where,(unsigned long)rgbnz,(unsigned long)anz,
            (unsigned long)white,x0,y0,x1,y1,maxrgb,amax,hash,glGetError());
    extern const char *glp_tex_dump_dir(void);
    char path[1200];
    snprintf(path,sizeof path,"%s/post614_%02d_%s.ppm",
             glp_tex_dump_dir(),n,where);
    FILE *f=fopen(path,"wb");
    if(f){
        fprintf(f,"P6\n%d %d\n255\n",w,h);
        for(int y=h-1;y>=0;y--)for(int x=0;x<w;x++)
            fwrite(px+((size_t)y*(size_t)w+(size_t)x)*4u,1,3,f);
        fclose(f);
    }
    free(px);
}

void glp_post_614_probe(int count){
    glp_probe_rt614("draw",count);
}

/*
 * The first honest material shader is GL program 33 (three real samplers);
 * program 36 consumes its result.  Probe those draws at the desktop-GL
 * boundary, where we can distinguish three otherwise identical black-frame
 * causes: empty source textures, zero fragment coverage, or a later overwrite.
 * This is read-only diagnostics, bounded to the first four before/after pairs
 * and enabled only for an explicit render-graph diagnostic boot.
 */
static void glp_quad51_probe(const char *phase, int mode, int count, int type,
                             uintptr_t indices){
    static int pairs;
    static int background_dumped;
    if (!getenv("GLP_RG_DUMP") || g_cur_prog != 51 || pairs >= 35)
        return;
    int before = phase && phase[0] == 'b';
    GLint actual=0, old_array=0, old_element=0;
    GLint vp[4]={0},sc[4]={0},front=0,cull=0;
    GLint old_active=GL_TEXTURE0,tex0=0,tex0_w=0,tex0_h=0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&actual);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&old_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&old_element);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    glGetIntegerv(GL_FRONT_FACE,&front);
    glGetIntegerv(GL_CULL_FACE_MODE,&cull);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0);
    if (tex0) {
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tex0_w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&tex0_h);
    }
    glActiveTexture((GLenum)old_active);
    fprintf(stderr,
            "[GLQUAD51] pair=%d phase=%s actual=%d mode=0x%x count=%d "
            "type=0x%x indices=0x%lx abo=%d ebo=%d tex0=%d/%dx%d "
            "vp=%d,%d+%dx%d sc=%d,%d+%dx%d "
            "scis=%d cull=%d/0x%x front=0x%x depth=%d blend=%d\n",
            pairs+1,phase,actual,mode,count,type,(unsigned long)indices,
            old_array,old_element,tex0,tex0_w,tex0_h,
            vp[0],vp[1],vp[2],vp[3],
            sc[0],sc[1],sc[2],sc[3],glIsEnabled(GL_SCISSOR_TEST),
            glIsEnabled(GL_CULL_FACE),cull,front,
            glIsEnabled(GL_DEPTH_TEST),glIsEnabled(GL_BLEND));

    int dump_state = before && actual &&
        (pairs == 0 || (!background_dumped && tex0_w == 1154 && tex0_h == 480));
    if (dump_state) {
        if (tex0_w == 1154 && tex0_h == 480)
            background_dumped = 1;
        GLint na=0;
        glGetProgramiv((GLuint)actual,GL_ACTIVE_ATTRIBUTES,&na);
        for (GLint i=0; i<na; i++) {
            char name[96]; GLsizei len=0; GLint sz=0; GLenum aty=0;
            glGetActiveAttrib((GLuint)actual,(GLuint)i,sizeof name,
                              &len,&sz,&aty,name);
            GLint loc=glGetAttribLocation((GLuint)actual,name);
            GLint en=0,buf=0,as=0,stride=0,vt=0,norm=0,bsize=0;
            void *ptr=NULL;
            float words[16]={0};
            if (loc >= 0) {
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_ENABLED,&en);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buf);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_SIZE,&as);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_STRIDE,&stride);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_TYPE,&vt);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&norm);
                glGetVertexAttribPointerv((GLuint)loc,
                                          GL_VERTEX_ATTRIB_ARRAY_POINTER,&ptr);
            }
            if (buf) {
                glBindBuffer(GL_ARRAY_BUFFER,(GLuint)buf);
                glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&bsize);
                size_t off=(size_t)(uintptr_t)ptr;
                size_t bytes=bsize>(GLint)off ? (size_t)bsize-off : 0u;
                if(bytes>sizeof words)bytes=sizeof words;
                if(bytes)glGetBufferSubData(GL_ARRAY_BUFFER,(GLintptr)off,
                                            (GLsizeiptr)bytes,words);
            }
            fprintf(stderr,
                    "[GLQUAD51ATTR] name=%s loc=%d en=%d buf=%d bsize=%d "
                    "size=%d stride=%d type=0x%x norm=%d ptr=0x%lx "
                    "f=%.8g,%.8g,%.8g,%.8g|%.8g,%.8g,%.8g,%.8g|"
                    "%.8g,%.8g,%.8g,%.8g|%.8g,%.8g,%.8g,%.8g\n",
                    name,loc,en,buf,bsize,as,stride,vt,norm,
                    (unsigned long)(uintptr_t)ptr,
                    words[0],words[1],words[2],words[3],
                    words[4],words[5],words[6],words[7],
                    words[8],words[9],words[10],words[11],
                    words[12],words[13],words[14],words[15]);
        }
        if (old_element) {
            GLint bsize=0;
            unsigned char raw[32]={0};
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);
            glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER,GL_BUFFER_SIZE,
                                   &bsize);
            size_t bytes=bsize>(GLint)indices
                ? (size_t)bsize-(size_t)indices : 0u;
            if(bytes>sizeof raw)bytes=sizeof raw;
            if(bytes)glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                                        (GLintptr)indices,
                                        (GLsizeiptr)bytes,raw);
            fprintf(stderr,
                    "[GLQUAD51IDX] ebo=%d bsize=%d raw="
                    "%02x%02x%02x%02x %02x%02x%02x%02x "
                    "%02x%02x%02x%02x %02x%02x%02x%02x\n",
                    old_element,bsize,
                    raw[0],raw[1],raw[2],raw[3],
                    raw[4],raw[5],raw[6],raw[7],
                    raw[8],raw[9],raw[10],raw[11],
                    raw[12],raw[13],raw[14],raw[15]);
        }
        glBindBuffer(GL_ARRAY_BUFFER,(GLuint)old_array);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);
        {
            float c[4][4]={{0}};
            for (int i=0;i<4;i++) {
                char name[32];
                snprintf(name,sizeof name,"ir3_c[%d]",i);
                GLint loc=glGetUniformLocation((GLuint)actual,name);
                if(loc>=0) glGetUniformfv((GLuint)actual,loc,c[i]);
            }
            fprintf(stderr,
                    "[GLQUAD51CONST] tex0=%d/%dx%d "
                    "c0=%.8g,%.8g,%.8g,%.8g "
                    "c1=%.8g,%.8g,%.8g,%.8g "
                    "c2=%.8g,%.8g,%.8g,%.8g "
                    "c3=%.8g,%.8g,%.8g,%.8g\n",
                    tex0,tex0_w,tex0_h,
                    c[0][0],c[0][1],c[0][2],c[0][3],
                    c[1][0],c[1][1],c[1][2],c[1][3],
                    c[2][0],c[2][1],c[2][2],c[2][3],
                    c[3][0],c[3][1],c[3][2],c[3][3]);
        }
        {
            GLint active=GL_TEXTURE0,tex=0,tw=0,th=0;
            glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);
            if(tex){
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
            }
            size_t np=(tw>0&&th>0)?(size_t)tw*(size_t)th:0u;
            unsigned char *px=np?(unsigned char*)malloc(np*4u):NULL;
            size_t rgbnz=0,anz=0; unsigned amin=255,amax=0;
            if(px){
                glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
                for(size_t p=0;p<np;p++){
                    const unsigned char *q=px+p*4u;
                    if(q[0]||q[1]||q[2])rgbnz++;
                    if(q[3])anz++;
                    if(q[3]<amin)amin=q[3];if(q[3]>amax)amax=q[3];
                }
            }
            fprintf(stderr,
                    "[GLQUAD51TEX] tex=%d wh=%dx%d rgbnz=%lu anz=%lu "
                    "alpha=%u..%u\n",
                    tex,tw,th,(unsigned long)rgbnz,(unsigned long)anz,
                    px?amin:0,px?amax:0);
            free(px);
            glActiveTexture((GLenum)active);
        }
    }
    if (g_cur_rt_w > 0 && g_cur_rt_h > 0) {
        size_t np=(size_t)g_cur_rt_w*(size_t)g_cur_rt_h;
        unsigned char *px=(unsigned char*)malloc(np*4u);
        size_t rgbnz=0,anz=0;
        unsigned maxrgb=0,amax=0;
        if(px){
            glReadPixels(0,0,g_cur_rt_w,g_cur_rt_h,GL_RGBA,
                         GL_UNSIGNED_BYTE,px);
            for(size_t p=0;p<np;p++){
                const unsigned char *q=px+p*4u;
                if(q[0]||q[1]||q[2])rgbnz++;
                if(q[3])anz++;
                if(q[0]>maxrgb)maxrgb=q[0];if(q[1]>maxrgb)maxrgb=q[1];
                if(q[2]>maxrgb)maxrgb=q[2];if(q[3]>amax)amax=q[3];
            }
        }
        fprintf(stderr,
                "[GLQUAD51PIX] pair=%d phase=%s rgbnz=%lu anz=%lu "
                "maxrgb=%u amax=%u\n",
                pairs+1,phase,(unsigned long)rgbnz,(unsigned long)anz,
                maxrgb,amax);
        free(px);
    }
    if (!before) {
        pairs++;
    }
    (void)glGetError();
}

/*
 * The striped car/icon material is exact program 54 (slot 17, FS e7b9cf3c).
 * Its VS consumes two assembled GPR inputs rather than one conventional
 * position+UV pair:
 *   r0.xy -> texture coordinates, r0.zw + r1.x -> clip position.
 * Inspect the arrays after VFD-GPR assembly, at the real indexed vertices.
 * This is deliberately host-side and read-only so one diagnostic boot can
 * distinguish corrupt source bytes from a wrong REGID/lane assembly.
 */
static void glp_asset54_probe(const char *phase, int mode, int count, int type,
                              uintptr_t indices){
    static int dumps;
    static long last_swap=-1;
    static GLint last_tw=-1,last_th=-1;
    const char *probe_env=getenv("GLP_HMI_ASSET_PROBE");
    char *end=NULL;
    long start=probe_env?strtol(probe_env,&end,10):0;
    if(!probe_env || !phase || phase[0]!='b' || g_cur_prog!=54 ||
       dumps>=32 || end==probe_env || *end!='\0' || g_swap_count<start)
        return;

    GLint old_active=GL_TEXTURE0,tex0=0,tw=0,th=0;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0);
    if(tex0){
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
    }
    glActiveTexture((GLenum)old_active);
    if(last_swap!=g_swap_count){
        last_swap=g_swap_count;
        last_tw=last_th=-1;
    }
    if(tw==last_tw && th==last_th)
        return;
    last_tw=tw; last_th=th; dumps++;

    GLint old_array=0,old_element=0,vp[4]={0},sc[4]={0};
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&old_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&old_element);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);

    uint32_t ix[4]={0,1,2,3};
    unsigned ni=count<4? (unsigned)count : 4u;
    if(old_element && ni){
        GLint bsize=0;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER,GL_BUFFER_SIZE,&bsize);
        size_t elem=(type==GL_UNSIGNED_BYTE)?1u:
                    (type==GL_UNSIGNED_SHORT)?2u:4u;
        if((size_t)indices+ni*elem<=(size_t)bsize){
            unsigned char raw[16]={0};
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,(GLintptr)indices,
                               (GLsizeiptr)(ni*elem),raw);
            for(unsigned i=0;i<ni;i++){
                if(elem==1u) ix[i]=raw[i];
                else if(elem==2u){
                    uint16_t v; memcpy(&v,raw+i*2u,2u); ix[i]=v;
                }else memcpy(&ix[i],raw+i*4u,4u);
            }
        }
    }
    fprintf(stderr,
            "[ASSET54] swap=%ld dump=%d tex=%d/%dx%d mode=0x%x count=%d "
            "type=0x%x indices=0x%lx ebo=%d ix=%u,%u,%u,%u "
            "vp=%d,%d+%dx%d sc=%d,%d+%dx%d blend=%d\n",
            g_swap_count,dumps,tex0,tw,th,mode,count,type,
            (unsigned long)indices,
            old_element,ix[0],ix[1],ix[2],ix[3],
            vp[0],vp[1],vp[2],vp[3],sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_BLEND));

    GLint na=0;
    glGetProgramiv((GLuint)g_cur_prog,GL_ACTIVE_ATTRIBUTES,&na);
    for(GLint ai=0;ai<na;ai++){
        char name[96]; GLsizei nl=0; GLint asz=0; GLenum aty=0;
        glGetActiveAttrib((GLuint)g_cur_prog,(GLuint)ai,sizeof name,
                          &nl,&asz,&aty,name);
        GLint loc=glGetAttribLocation((GLuint)g_cur_prog,name);
        GLint en=0,buf=0,comps=0,stride=0,vt=0,norm=0,bsize=0;
        void *ptr=NULL;
        if(loc>=0){
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&en);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buf);
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_SIZE,&comps);
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&stride);
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_TYPE,&vt);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&norm);
            glGetVertexAttribPointerv((GLuint)loc,
                                      GL_VERTEX_ATTRIB_ARRAY_POINTER,&ptr);
        }
        float v[4][4]={{0}};
        if(en && buf && vt==GL_FLOAT){
            glBindBuffer(GL_ARRAY_BUFFER,(GLuint)buf);
            glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&bsize);
            size_t step=stride>0?(size_t)stride:(size_t)comps*sizeof(float);
            size_t base=(size_t)(uintptr_t)ptr;
            for(unsigned k=0;k<ni;k++){
                size_t off=base+(size_t)ix[k]*step;
                size_t nb=(size_t)comps*sizeof(float);
                if(nb>sizeof v[k])nb=sizeof v[k];
                if(off+nb<=(size_t)bsize)
                    glGetBufferSubData(GL_ARRAY_BUFFER,(GLintptr)off,
                                       (GLsizeiptr)nb,v[k]);
            }
        }
        fprintf(stderr,
                "[ASSET54ATTR] name=%s loc=%d en=%d buf=%d bsize=%d "
                "size=%d stride=%d type=0x%x norm=%d ptr=0x%lx "
                "v0=(%.8g,%.8g,%.8g,%.8g) "
                "v1=(%.8g,%.8g,%.8g,%.8g) "
                "v2=(%.8g,%.8g,%.8g,%.8g) "
                "v3=(%.8g,%.8g,%.8g,%.8g)\n",
                name,loc,en,buf,bsize,comps,stride,vt,norm,
                (unsigned long)(uintptr_t)ptr,
                v[0][0],v[0][1],v[0][2],v[0][3],
                v[1][0],v[1][1],v[1][2],v[1][3],
                v[2][0],v[2][1],v[2][2],v[2][3],
                v[3][0],v[3][1],v[3][2],v[3][3]);
    }
    glBindBuffer(GL_ARRAY_BUFFER,(GLuint)old_array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);

    for(int ci=0;ci<6;ci++){
        int reg=ci<4?ci:(ci==4?128:129);
        char un[32]; GLfloat c[4]={0};
        snprintf(un,sizeof un,"ir3_c[%d]",reg);
        GLint loc=glGetUniformLocation((GLuint)g_cur_prog,un);
        if(loc>=0)glGetUniformfv((GLuint)g_cur_prog,loc,c);
        fprintf(stderr,"[ASSET54CONST] c%d=(%.8g,%.8g,%.8g,%.8g)\n",
                reg,c[0],c[1],c[2],c[3]);
    }
    {
        GLfloat c[4]={0};
        GLint loc=glGetUniformLocation((GLuint)g_cur_prog,"ir3_c[263]");
        if(loc>=0)glGetUniformfv((GLuint)g_cur_prog,loc,c);
        fprintf(stderr,"[ASSET54CONST] c263=(%.8g,%.8g,%.8g,%.8g)\n",
                c[0],c[1],c[2],c[3]);
    }
    (void)glGetError();
}

/*
 * Program 18 composites the already-rendered 1024/1068x480 HMI layers.  Its
 * ir3 VS consumes assembled r0/r1 for position, while the FS texcoord is a
 * fixed-function VPC input absent from the VS binary.  Dump both the assembled
 * arrays and the explicit a_glp_texcoord bridge once per source texture.  This
 * catches a wrong VFD-stream alias without modifying the draw or substituting
 * either shader.
 */
static void glp_layer18_probe(const char *phase, int mode, int count, int type,
                              uintptr_t indices){
    static GLint seen[8];
    static int nseen;
    if(!getenv("GLP_HMI_LAYER_PROBE") || !phase || g_cur_prog!=18)
        return;

    GLint old_active=GL_TEXTURE0,tex0=0,tw=0,th=0;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0);
    if(tex0){
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
    }
    glActiveTexture((GLenum)old_active);
    if(phase[0]=='a'){
        static int after_n;
        if(tw>=1068 && th>=480 && after_n<5 &&
           g_cur_rt_w>0 && g_cur_rt_h>0){
            int w=g_cur_rt_w,h=g_cur_rt_h;
            size_t np=(size_t)w*(size_t)h;
            unsigned char *px=(unsigned char *)malloc(np*4u);
            if(px){
                glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
                extern const char *glp_tex_dump_dir(void);
                char path[1200];
                snprintf(path,sizeof path,
                         "%s/layer18_after_%02d_tex%d.ppm",
                         glp_tex_dump_dir(),++after_n,tex0);
                FILE *f=fopen(path,"wb");
                if(f){
                    fprintf(f,"P6\n%d %d\n255\n",w,h);
                    for(int y=h-1;y>=0;y--)
                        for(int x=0;x<w;x++)
                            fwrite(px+((size_t)y*(size_t)w+(size_t)x)*4u,
                                   1,3,f);
                    fclose(f);
                }
                free(px);
            }
        }
        return;
    }
    if(phase[0]!='b' || nseen>=8)
        return;
    for(int i=0;i<nseen;i++) if(seen[i]==tex0) return;
    seen[nseen++]=tex0;

    GLint old_array=0,old_element=0,vp[4]={0},sc[4]={0};
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&old_array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&old_element);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    uint32_t ix[4]={0,1,2,3};
    unsigned ni=count<4?(unsigned)count:4u;
    if(old_element && ni){
        GLint bsize=0;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER,GL_BUFFER_SIZE,&bsize);
        size_t elem=(type==GL_UNSIGNED_BYTE)?1u:
                    (type==GL_UNSIGNED_SHORT)?2u:4u;
        if((size_t)indices+ni*elem<=(size_t)bsize){
            unsigned char raw[16]={0};
            glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER,(GLintptr)indices,
                               (GLsizeiptr)(ni*elem),raw);
            for(unsigned i=0;i<ni;i++){
                if(elem==1u) ix[i]=raw[i];
                else if(elem==2u){
                    uint16_t v; memcpy(&v,raw+i*2u,2u); ix[i]=v;
                }else memcpy(&ix[i],raw+i*4u,4u);
            }
        }
    }
    fprintf(stderr,
            "[LAYER18] dump=%d tex=%d/%dx%d mode=0x%x count=%d "
            "type=0x%x indices=0x%lx ebo=%d ix=%u,%u,%u,%u "
            "vp=%d,%d+%dx%d sc=%d,%d+%dx%d blend=%d\n",
            nseen,tex0,tw,th,mode,count,type,(unsigned long)indices,
            old_element,ix[0],ix[1],ix[2],ix[3],
            vp[0],vp[1],vp[2],vp[3],sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_BLEND));

    if(tex0 && tw>0 && th>0 &&
       (size_t)tw*(size_t)th<=16u*1024u*1024u){
        size_t np=(size_t)tw*(size_t)th;
        unsigned char *px=(unsigned char *)malloc(np*4u);
        size_t rgbnz=0,anz=0,aopaque=0,rgb_without_alpha=0;
        uint64_t rgbsum=0,asum=0;
        unsigned amin=255,amax=0;
        if(px){
            glActiveTexture(GL_TEXTURE0);
            glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
            glActiveTexture((GLenum)old_active);
            for(size_t p=0;p<np;p++){
                const unsigned char *q=px+p*4u;
                int rgb=q[0]||q[1]||q[2];
                if(rgb)rgbnz++;
                if(q[3])anz++;
                if(q[3]==255)aopaque++;
                if(rgb&&!q[3])rgb_without_alpha++;
                rgbsum+=(uint64_t)q[0]+q[1]+q[2];
                asum+=q[3];
                if(q[3]<amin)amin=q[3];
                if(q[3]>amax)amax=q[3];
            }
            fprintf(stderr,
                    "[LAYER18TEX] tex=%d wh=%dx%d rgbnz=%lu anz=%lu "
                    "opaque=%lu rgb_a0=%lu rgbsum=%llu asum=%llu "
                    "arange=%u..%u err=0x%x\n",
                    tex0,tw,th,(unsigned long)rgbnz,(unsigned long)anz,
                    (unsigned long)aopaque,(unsigned long)rgb_without_alpha,
                    (unsigned long long)rgbsum,(unsigned long long)asum,
                    amin,amax,glGetError());
            if(tw>=1024 && th>=480){
                extern const char *glp_tex_dump_dir(void);
                char path[1200];
                snprintf(path,sizeof path,"%s/layer18_tex%d_alpha.pgm",
                         glp_tex_dump_dir(),tex0);
                FILE *f=fopen(path,"wb");
                if(f){
                    fprintf(f,"P5\n%d %d\n255\n",tw,th);
                    for(int y=th-1;y>=0;y--)
                        for(int x=0;x<tw;x++)
                            fputc(px[((size_t)y*(size_t)tw+(size_t)x)*4u+3u],
                                  f);
                    fclose(f);
                }
            }
            free(px);
        }
    }

    GLint na=0;
    glGetProgramiv((GLuint)g_cur_prog,GL_ACTIVE_ATTRIBUTES,&na);
    for(GLint ai=0;ai<na;ai++){
        char name[96]; GLsizei nl=0; GLint asz=0; GLenum aty=0;
        glGetActiveAttrib((GLuint)g_cur_prog,(GLuint)ai,sizeof name,
                          &nl,&asz,&aty,name);
        GLint loc=glGetAttribLocation((GLuint)g_cur_prog,name);
        GLint en=0,buf=0,comps=0,stride=0,vt=0,norm=0,bsize=0;
        void *ptr=NULL;
        if(loc>=0){
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&en);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buf);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_SIZE,&comps);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_STRIDE,&stride);
            glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_TYPE,&vt);
            glGetVertexAttribiv((GLuint)loc,
                                GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&norm);
            glGetVertexAttribPointerv((GLuint)loc,
                                      GL_VERTEX_ATTRIB_ARRAY_POINTER,&ptr);
        }
        float v[4][4]={{0}};
        if(en && buf && vt==GL_FLOAT){
            glBindBuffer(GL_ARRAY_BUFFER,(GLuint)buf);
            glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&bsize);
            size_t step=stride>0?(size_t)stride:(size_t)comps*sizeof(float);
            size_t base=(size_t)(uintptr_t)ptr;
            for(unsigned k=0;k<ni;k++){
                size_t off=base+(size_t)ix[k]*step;
                size_t nb=(size_t)comps*sizeof(float);
                if(nb>sizeof v[k])nb=sizeof v[k];
                if(off+nb<=(size_t)bsize)
                    glGetBufferSubData(GL_ARRAY_BUFFER,(GLintptr)off,
                                       (GLsizeiptr)nb,v[k]);
            }
        }
        fprintf(stderr,
                "[LAYER18ATTR] name=%s loc=%d en=%d buf=%d bsize=%d "
                "size=%d stride=%d type=0x%x norm=%d ptr=0x%lx "
                "v0=(%.8g,%.8g,%.8g,%.8g) "
                "v1=(%.8g,%.8g,%.8g,%.8g) "
                "v2=(%.8g,%.8g,%.8g,%.8g) "
                "v3=(%.8g,%.8g,%.8g,%.8g)\n",
                name,loc,en,buf,bsize,comps,stride,vt,norm,
                (unsigned long)(uintptr_t)ptr,
                v[0][0],v[0][1],v[0][2],v[0][3],
                v[1][0],v[1][1],v[1][2],v[1][3],
                v[2][0],v[2][1],v[2][2],v[2][3],
                v[3][0],v[3][1],v[3][2],v[3][3]);
    }
    glBindBuffer(GL_ARRAY_BUFFER,(GLuint)old_array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,(GLuint)old_element);
    for(int ci=0;ci<6;ci++){
        int reg=ci<4?ci:(ci==4?128:263);
        char un[32]; GLfloat c[4]={0};
        snprintf(un,sizeof un,"ir3_c[%d]",reg);
        GLint loc=glGetUniformLocation((GLuint)g_cur_prog,un);
        if(loc>=0)glGetUniformfv((GLuint)g_cur_prog,loc,c);
        fprintf(stderr,"[LAYER18CONST] c%d=(%.8g,%.8g,%.8g,%.8g)\n",
                reg,c[0],c[1],c[2],c[3]);
    }
    (void)glGetError();
}

static void glp_mem2gmem_dump_ppm(const char *path,
                                  const unsigned char *rgba, int w, int h){
    if(!path || !rgba || w<=0 || h<=0) return;
    FILE *f=fopen(path,"wb");
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    unsigned char *row=(unsigned char *)malloc((size_t)w*3u);
    if(!row){ fclose(f); return; }
    for(int y=h-1;y>=0;y--){
        const unsigned char *src=rgba+(size_t)y*(size_t)w*4u;
        for(int x=0;x<w;x++){
            row[x*3+0]=src[x*4+0];
            row[x*3+1]=src[x*4+1];
            row[x*3+2]=src[x*4+2];
        }
        fwrite(row,1,(size_t)w*3u,f);
    }
    free(row);
    fclose(f);
}

/*
 * Program 3 is the real fd3 tile mem2gmem helper.  When a later animation
 * frame loses already-presented menu pixels, capture its exact display input
 * and collapsed-GMEM output around the draw.  GLP_HMI_MEM2GMEM_PROBE is the
 * first already-presented Screen frame to retain; the hard cap keeps the
 * diagnostic bounded and it is completely observational.
 */
static void glp_mem2gmem_probe(const char *phase){
    static unsigned seq;
    static int pending;
    static uint32_t last_tex_hash;
    static int have_tex_hash;
    const char *start_env=getenv("GLP_HMI_MEM2GMEM_PROBE");
    const char *dir=getenv("GLP_TEX_DUMP_DIR");
    if(!start_env || !*start_env || !dir || !*dir || !phase ||
       g_cur_prog!=3 || seq>=256)
        return;
    char *end=NULL;
    long start=strtol(start_env,&end,10);
    if(end==start_env || *end!='\0' || g_swap_count<start) return;

    int before=phase[0]=='b';
    if(before){
        GLint old_active=GL_TEXTURE0,tex0=0,tw=0,th=0,sc[4]={0};
        glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0);
        if(tex0){
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
        }
        if(glIsEnabled(GL_SCISSOR_TEST)) glGetIntegerv(GL_SCISSOR_BOX,sc);
        if(!tex0 || tw<1024 || th<480){
            glActiveTexture((GLenum)old_active);
            pending=0;
            return;
        }
        size_t tex_bytes=(size_t)tw*(size_t)th*4u;
        unsigned char *tex_px=(unsigned char *)malloc(tex_bytes);
        if(!tex_px){
            glActiveTexture((GLenum)old_active);
            pending=0;
            return;
        }
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,tex_px);
        uint32_t tex_hash=2166136261u;
        for(size_t i=0;i<tex_bytes;i++){
            tex_hash^=tex_px[i];
            tex_hash*=16777619u;
        }
        if(have_tex_hash && tex_hash==last_tex_hash){
            free(tex_px);
            glActiveTexture((GLenum)old_active);
            pending=0;
            return;
        }
        last_tex_hash=tex_hash;
        have_tex_hash=1;
        unsigned n=++seq;
        char path[1200];
        snprintf(path,sizeof path,
                 "%s/mem2gmem_%03u_swap%04ld_tex%d_input_%08x.ppm",
                 dir,n,g_swap_count,tex0,tex_hash);
        glp_mem2gmem_dump_ppm(path,tex_px,tw,th);
        free(tex_px);
        if(g_cur_rt_w>0 && g_cur_rt_h>0){
            size_t rt_bytes=(size_t)g_cur_rt_w*(size_t)g_cur_rt_h*4u;
            unsigned char *rt_px=(unsigned char *)malloc(rt_bytes);
            if(rt_px){
                glReadPixels(0,0,g_cur_rt_w,g_cur_rt_h,GL_RGBA,
                             GL_UNSIGNED_BYTE,rt_px);
                char path[1200];
                snprintf(path,sizeof path,
                         "%s/mem2gmem_%03u_swap%04ld_before.ppm",
                         dir,n,g_swap_count);
                glp_mem2gmem_dump_ppm(path,rt_px,g_cur_rt_w,g_cur_rt_h);
                free(rt_px);
            }
        }
        fprintf(stderr,
                "[MEM2GMEM] seq=%u swap=%ld tex=%d/%dx%d hash=%08x "
                "rt=%dx%d sc=%d,%d+%dx%d\n",
                n,g_swap_count,tex0,tw,th,tex_hash,g_cur_rt_w,g_cur_rt_h,
                sc[0],sc[1],sc[2],sc[3]);
        glActiveTexture((GLenum)old_active);
        pending=(int)n;
    }else if(pending){
        int n=pending;
        pending=0;
        if(g_cur_rt_w>0 && g_cur_rt_h>0){
            size_t rt_bytes=(size_t)g_cur_rt_w*(size_t)g_cur_rt_h*4u;
            unsigned char *rt_px=(unsigned char *)malloc(rt_bytes);
            if(rt_px){
                glReadPixels(0,0,g_cur_rt_w,g_cur_rt_h,GL_RGBA,
                             GL_UNSIGNED_BYTE,rt_px);
                char path[1200];
                snprintf(path,sizeof path,
                         "%s/mem2gmem_%03d_swap%04ld_after.ppm",
                         dir,n,g_swap_count);
                glp_mem2gmem_dump_ppm(path,rt_px,g_cur_rt_w,g_cur_rt_h);
                free(rt_px);
            }
        }
    }
    (void)glGetError();
}

void glp_program_probe(const char *phase, int mode, int count, int type,
                       uintptr_t indices){
    glp_mem2gmem_probe(phase);
    /*
     * One-frame, per-draw framebuffer oracle for the honest HMI path.
     *
     * GLP_HMI_DRAW_PROBE is the already-presented Screen frame number.  A draw
     * between swaps N and N+1 therefore belongs to probe value N.  Read only
     * the active scissor rectangle (normally one 256x96/256 tile), not the
     * whole 1024x480 attachment: this keeps the diagnostic bounded while still
     * exposing the exact draw that erases a good background or writes striped
     * geometry.  The probe is observational; it never changes GL state.
     */
    static struct {
        int active;
        unsigned seq;
        uint32_t prog, rtga, hash;
        size_t rgbnz, anz;
        uint64_t rgbsum;
        int x, y, w, h;
        GLint tex[4];
        GLint blend, depth, cull;
    } hp;
    const char *hmi_probe=getenv("GLP_HMI_DRAW_PROBE");
    if(hmi_probe && *hmi_probe){
        char *end=NULL;
        long target=strtol(hmi_probe,&end,10);
        int before=phase && phase[0]=='b';
        if(end!=hmi_probe && *end=='\0' && g_swap_count==target){
            if(before){
                GLint sc[4]={0,0,g_cur_rt_w,g_cur_rt_h};
                GLint act=GL_TEXTURE0;
                hp.active=0;
                if(glIsEnabled(GL_SCISSOR_TEST))
                    glGetIntegerv(GL_SCISSOR_BOX,sc);
                hp.x=sc[0] < 0 ? 0 : sc[0];
                hp.y=sc[1] < 0 ? 0 : sc[1];
                hp.w=sc[2];
                hp.h=sc[3];
                if(hp.x+hp.w>g_cur_rt_w) hp.w=g_cur_rt_w-hp.x;
                if(hp.y+hp.h>g_cur_rt_h) hp.h=g_cur_rt_h-hp.y;
                if(hp.w<0)hp.w=0;
                if(hp.h<0)hp.h=0;
                hp.prog=g_cur_prog;
                hp.rtga=(g_cur_rt>=0 && g_cur_rt<g_rt_n)
                    ? g_rt[g_cur_rt].ga : 0xffffffffu;
                hp.blend=glIsEnabled(GL_BLEND);
                hp.depth=glIsEnabled(GL_DEPTH_TEST);
                hp.cull=glIsEnabled(GL_CULL_FACE);
                glGetIntegerv(GL_ACTIVE_TEXTURE,&act);
                for(int unit=0;unit<4;unit++){
                    glActiveTexture((GLenum)(GL_TEXTURE0+unit));
                    glGetIntegerv(GL_TEXTURE_BINDING_2D,&hp.tex[unit]);
                }
                glActiveTexture((GLenum)act);
                hp.hash=2166136261u;
                hp.rgbnz=hp.anz=0;
                hp.rgbsum=0;
                size_t np=(size_t)hp.w*(size_t)hp.h;
                unsigned char *px=np
                    ? (unsigned char *)malloc(np*4u) : NULL;
                if(px){
                    glReadPixels(hp.x,hp.y,hp.w,hp.h,GL_RGBA,
                                 GL_UNSIGNED_BYTE,px);
                    for(size_t p=0;p<np;p++){
                        const unsigned char *q=px+p*4u;
                        if(q[0]||q[1]||q[2])hp.rgbnz++;
                        if(q[3])hp.anz++;
                        hp.rgbsum+=(uint64_t)q[0]+q[1]+q[2];
                        for(int c=0;c<4;c++){
                            hp.hash^=q[c];
                            hp.hash*=16777619u;
                        }
                    }
                    free(px);
                    hp.active=1;
                }
            }else if(hp.active){
                uint32_t hash=2166136261u;
                size_t rgbnz=0,anz=0;
                uint64_t rgbsum=0;
                size_t np=(size_t)hp.w*(size_t)hp.h;
                unsigned char *px=np
                    ? (unsigned char *)malloc(np*4u) : NULL;
                if(px){
                    glReadPixels(hp.x,hp.y,hp.w,hp.h,GL_RGBA,
                                 GL_UNSIGNED_BYTE,px);
                    for(size_t p=0;p<np;p++){
                        const unsigned char *q=px+p*4u;
                        if(q[0]||q[1]||q[2])rgbnz++;
                        if(q[3])anz++;
                        rgbsum+=(uint64_t)q[0]+q[1]+q[2];
                        for(int c=0;c<4;c++){
                            hash^=q[c];
                            hash*=16777619u;
                        }
                    }
                    free(px);
                    fprintf(stderr,
                        "[HMIDRAW] frame=%ld seq=%u prog=%u "
                        "mode=0x%x count=%d type=0x%x indices=0x%lx "
                        "rtga=0x%08x box=%d,%d+%dx%d tex=%d,%d,%d,%d "
                        "blend=%d depth=%d cull=%d "
                        "rgb=%lu->%lu alpha=%lu->%lu "
                        "sum=%llu->%llu hash=%08x->%08x changed=%d\n",
                        g_swap_count,++hp.seq,hp.prog,mode,count,type,
                        (unsigned long)indices,hp.rtga,
                        hp.x,hp.y,hp.w,hp.h,
                        hp.tex[0],hp.tex[1],hp.tex[2],hp.tex[3],
                        hp.blend,hp.depth,hp.cull,
                        (unsigned long)hp.rgbnz,(unsigned long)rgbnz,
                        (unsigned long)hp.anz,(unsigned long)anz,
                        (unsigned long long)hp.rgbsum,
                        (unsigned long long)rgbsum,
                        hp.hash,hash,
                        hash!=hp.hash || rgbnz!=hp.rgbnz ||
                            anz!=hp.anz || rgbsum!=hp.rgbsum);
                }
                hp.active=0;
            }
        }else if(before){
            hp.active=0;
        }
    }
    static int pairs;
    static uint32_t before_hash;
    static unsigned before_rgbnz, before_anz;
    glp_asset54_probe(phase,mode,count,type,indices);
    glp_layer18_probe(phase,mode,count,type,indices);
    glp_quad51_probe(phase,mode,count,type,indices);
    if (!getenv("GLP_RG_DUMP") || (g_cur_prog != 33 && g_cur_prog != 36) ||
        pairs >= 4 || g_cur_rt < 0 || g_cur_rt >= g_rt_n)
        return;

    int is_before = phase && phase[0] == 'b';
    GLint actual=0, fbo=0, vp[4]={0}, sc[4]={0}, act=GL_TEXTURE0;
    GLint ebo=0, abo=0;
    glGetIntegerv(GL_CURRENT_PROGRAM,&actual);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&act);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&ebo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&abo);
    fprintf(stderr,
            "[PROGDRAW] pair=%d phase=%s tracked=%u actual=%d "
            "mode=0x%x count=%d type=0x%x indices=0x%lx "
            "rtga=0x%08x wh=%dx%d fbo=%d "
            "vp=%d,%d+%dx%d sc=%d,%d+%dx%d "
            "scis=%d blend=%d depth=%d cull=%d ebo=%d abo=%d status=0x%x\n",
            pairs+1,phase,g_cur_prog,actual,mode,count,type,
            (unsigned long)indices,g_rt[g_cur_rt].ga,g_cur_rt_w,g_cur_rt_h,
            fbo,vp[0],vp[1],vp[2],vp[3],sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_SCISSOR_TEST),glIsEnabled(GL_BLEND),
            glIsEnabled(GL_DEPTH_TEST),glIsEnabled(GL_CULL_FACE),
            ebo,abo,glCheckFramebufferStatus(GL_FRAMEBUFFER));

    if (is_before && actual) {
        GLint na=0;
        glGetProgramiv((GLuint)actual,GL_ACTIVE_ATTRIBUTES,&na);
        for (GLint i=0; i<na; i++) {
            char name[96]; GLsizei len=0; GLint sz=0; GLenum aty=0;
            glGetActiveAttrib((GLuint)actual,(GLuint)i,sizeof name,
                              &len,&sz,&aty,name);
            GLint loc=glGetAttribLocation((GLuint)actual,name);
            GLint en=0,buf=0,as=0,stride=0,vt=0,norm=0;
            if (loc >= 0) {
                glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                                    &en);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buf);
                glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_SIZE,&as);
                glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                    &stride);
                glGetVertexAttribiv((GLuint)loc,GL_VERTEX_ATTRIB_ARRAY_TYPE,&vt);
                glGetVertexAttribiv((GLuint)loc,
                                    GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&norm);
            }
            fprintf(stderr,
                    "[PROGATTR] prog=%u name=%s loc=%d en=%d buf=%d "
                    "size=%d stride=%d type=0x%x norm=%d\n",
                    g_cur_prog,name,loc,en,buf,as,stride,vt,norm);
        }

        for (int unit=0; unit<3; unit++) {
            GLint tex=0,tw=0,th=0,ifmt=0;
            glActiveTexture((GLenum)(GL_TEXTURE0+unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);
            if (tex) {
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,
                                         GL_TEXTURE_INTERNAL_FORMAT,&ifmt);
            }
            size_t np=(tw>0 && th>0) ? (size_t)tw*(size_t)th : 0;
            unsigned char *tp=(np && np <= 16u*1024u*1024u)
                ? (unsigned char *)malloc(np*4u) : NULL;
            size_t rgbnz=0,anz=0;
            uint32_t hash=2166136261u;
            if (tp) {
                glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,tp);
                for (size_t p=0; p<np; p++) {
                    const unsigned char *q=tp+p*4u;
                    if (q[0] || q[1] || q[2]) rgbnz++;
                    if (q[3]) anz++;
                    for (int c=0;c<4;c++){hash^=q[c];hash*=16777619u;}
                }
            }
            fprintf(stderr,
                    "[PROGTEX] prog=%u unit=%d tex=%d wh=%dx%d ifmt=0x%x "
                    "rgbnz=%lu anz=%lu hash=%08x err=0x%x\n",
                    g_cur_prog,unit,tex,tw,th,ifmt,
                    (unsigned long)rgbnz,(unsigned long)anz,hash,glGetError());
            free(tp);
        }
        glActiveTexture((GLenum)act);
    }

    int w=g_cur_rt_w,h=g_cur_rt_h;
    size_t np=(w>0 && h>0) ? (size_t)w*(size_t)h : 0;
    unsigned char *px=np ? (unsigned char *)malloc(np*4u) : NULL;
    unsigned rgbnz=0,anz=0;
    uint32_t hash=2166136261u;
    if (px) {
        glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
        for (size_t p=0;p<np;p++) {
            const unsigned char *q=px+p*4u;
            if (q[0] || q[1] || q[2]) rgbnz++;
            if (q[3]) anz++;
            for(int c=0;c<4;c++){hash^=q[c];hash*=16777619u;}
        }
    }
    fprintf(stderr,
            "[PROGPIX] pair=%d phase=%s prog=%u rgbnz=%u anz=%u "
            "hash=%08x delta=%d err=0x%x\n",
            pairs+1,phase,g_cur_prog,rgbnz,anz,hash,
            is_before ? 0 :
                (hash!=before_hash || rgbnz!=before_rgbnz || anz!=before_anz),
            glGetError());
    free(px);
    if (is_before) {
        before_hash=hash;
        before_rgbnz=rgbnz;
        before_anz=anz;
    } else {
        pairs++;
    }
}

static GLuint g_coverage_query;

/* One occlusion query per exact PM4 program.  Pixel hashes cannot distinguish
 * zero raster coverage from a covered draw whose blend equation keeps the
 * attachment unchanged; GL_SAMPLES_PASSED can. */
int glp_coverage_probe_begin(int mode, int count){
    static unsigned char seen[MAXPROG];
    static unsigned quad51_seen;
    GLint sc[4]={0};
    if (!getenv("GLP_RG_DUMP") || g_cur_prog < 18 || g_cur_prog >= MAXPROG ||
        (g_cur_prog == 51 ? quad51_seen >= 35u : seen[g_cur_prog]))
        return 0;
    if (g_cur_prog == 51) quad51_seen++;
    else seen[g_cur_prog]=1;
    if (!g_coverage_query) glGenQueries(1,&g_coverage_query);
    if (!g_coverage_query) return 0;
    glGetIntegerv(GL_SCISSOR_BOX,sc);
    glBeginQuery(GL_SAMPLES_PASSED,g_coverage_query);
    fprintf(stderr,
            "[COVERAGE] phase=begin prog=%u mode=0x%x count=%d "
            "rtga=0x%08x wh=%dx%d sc=%d,%d+%dx%d cull=%d depth=%d\n",
            g_cur_prog,mode,count,
            (g_cur_rt>=0 && g_cur_rt<g_rt_n) ? g_rt[g_cur_rt].ga : 0,
            g_cur_rt_w,g_cur_rt_h,sc[0],sc[1],sc[2],sc[3],
            glIsEnabled(GL_CULL_FACE),glIsEnabled(GL_DEPTH_TEST));
    return (int)g_cur_prog;
}

void glp_coverage_probe_end(int token){
    if (!token) return;
    GLuint samples=0;
    GLint src_rgb=0,dst_rgb=0,src_a=0,dst_a=0,eq_rgb=0,eq_a=0;
    GLboolean mask[4]={0};
    glEndQuery(GL_SAMPLES_PASSED);
    glGetQueryObjectuiv(g_coverage_query,GL_QUERY_RESULT,&samples);
    glGetIntegerv(GL_BLEND_SRC_RGB,&src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB,&dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,&eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&eq_a);
    glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    fprintf(stderr,
            "[COVERAGE] phase=end prog=%d samples=%u blend=%d "
            "func=0x%x,0x%x/0x%x,0x%x eq=0x%x/0x%x mask=%d%d%d%d err=0x%x\n",
            token,samples,glIsEnabled(GL_BLEND),
            src_rgb,dst_rgb,src_a,dst_a,eq_rgb,eq_a,
            mask[0],mask[1],mask[2],mask[3],glGetError());
}

static void glp_bind_rendertarget(uint32_t ga, int w, int h){
    int requested_w = w > 0 ? w : 1;
    int requested_h = h > 0 ? h : 1;
    g_cur_rt_w = requested_w;
    g_cur_rt_h = requested_h;
    int old = glp_rt_index(ga);
    if (getenv("GLP_RG_DUMP") && w == 614 && h == 314)
        fprintf(stderr,"[RT614BIND] ga=0x%08x old=%d\n",ga,old);
    int idx = glp_rt_ensure(ga, requested_w, requested_h);
    if (idx < 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)glp_default_fbo);
        g_cur_rt = -1;
        fprintf(stderr, "[RT] table full, ga=0x%08x fell back to sink fbo=%d\n",
                ga, glp_default_fbo);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt[idx].fbo);
    g_cur_rt = idx;
    if (ga == GLP_DISPLAY_GA && g_rt[idx].w >= 1024 && g_rt[idx].h >= 480)
        g_present_rt = idx;
    if (old < 0) {
        static int rn=0; if(rn<40){rn++;
          fprintf(stderr,"[RT] surface FBO ga=0x%08x %dx%d fbo=%u tex=%u\n",
                  ga,g_rt[idx].w,g_rt[idx].h,g_rt[idx].fbo,g_rt[idx].tex); }
    }
    { static int bn=0; if(bn<80){bn++;
      fprintf(stderr,"[RTBIND] ga=0x%08x logical=%dx%d backing=%dx%d idx=%d fbo=%u\n",
              ga,g_cur_rt_w,g_cur_rt_h,g_rt[idx].w,g_rt[idx].h,
              idx,g_rt[idx].fbo); } }
}

/* Only the guest's real main display allocation is presentable.  Other
 * full-size surfaces are intermediate producers and must not steal the scanout
 * merely because they were the most recently bound/resolved RT. */
unsigned glp_present_fbo(void){
    return (g_present_rt >= 0 &&
            (g_rt[g_present_rt].ready || g_rt[g_present_rt].written))
         ? g_rt[g_present_rt].fbo : (unsigned)glp_default_fbo;
}

/* Sampling the same texture that is attached to the draw FBO is an undefined
 * GL feedback loop.  Adreno permits this pattern for its memory-backed display
 * surface, so snapshot the current pixels and sample the copy. */
static GLuint glp_rt_sample_tex(int idx, GLenum target){
    if (idx < 0 || idx >= g_rt_n) return 0;
    if (idx != g_cur_rt) return g_rt[idx].tex;
    if (!g_rt[idx].snapshot) {
        glGenTextures(1, &g_rt[idx].snapshot);
        glBindTexture(target, g_rt[idx].snapshot);
        glTexImage2D(target, 0, GL_RGBA, g_rt[idx].w, g_rt[idx].h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(target, g_rt[idx].snapshot);
    }
    glCopyTexSubImage2D(target, 0, 0, 0, 0, 0, g_rt[idx].w, g_rt[idx].h);
    return g_rt[idx].snapshot;
}

typedef struct {
    uint32_t hash;
    long nonblack;
    long alpha_nonzero, alpha_opaque;
    unsigned alpha_max;
    int x0, y0, x1, y1;
    unsigned char *rgba;
} glp_resolve_stats;

static void glp_resolve_read_stats(GLuint fbo, int w, int h,
                                   glp_resolve_stats *s, int keep_pixels){
    memset(s, 0, sizeof *s);
    s->x0 = w;
    s->y0 = h;
    s->x1 = -1;
    s->y1 = -1;
    if (w <= 0 || h <= 0) return;
    size_t bytes = (size_t)w * (size_t)h * 4u;
    unsigned char *px = (unsigned char *)malloc(bytes);
    if (!px) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t hash = 2166136261u;
    for (int y=0; y<h; y++) {
        for (int x=0; x<w; x++) {
            const unsigned char *p = px + ((size_t)y*w + x)*4u;
            int on = p[0] > 8 || p[1] > 8 || p[2] > 8;
            for (int c=0; c<4; c++) {
                hash ^= p[c];
                hash *= 16777619u;
            }
            if (p[3]) s->alpha_nonzero++;
            if (p[3] == 255) s->alpha_opaque++;
            if (p[3] > s->alpha_max) s->alpha_max = p[3];
            if (!on) continue;
            s->nonblack++;
            if (x < s->x0) s->x0 = x;
            if (y < s->y0) s->y0 = y;
            if (x > s->x1) s->x1 = x;
            if (y > s->y1) s->y1 = y;
        }
    }
    s->hash = hash;
    if (keep_pixels) s->rgba = px;
    else free(px);
}

static void glp_resolve_dump_ppm(const char *dir, const char *kind,
                                 unsigned seq, long swap, uint32_t ga,
                                 int w, int h, const glp_resolve_stats *s){
    if (!dir || !*dir || !s->rgba) return;
    glp_mkdirs(dir);
    char path[1200];
    snprintf(path, sizeof path,
             "%s/resolve_%04u_swap%04ld_%08x_%s_%08x.ppm",
             dir, seq, swap, ga, kind, s->hash);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    unsigned char *row = (unsigned char *)malloc((size_t)w*3u);
    if (!row) { fclose(f); return; }
    for (int y=h-1; y>=0; y--) {
        const unsigned char *src = s->rgba + (size_t)y*w*4u;
        for (int x=0; x<w; x++) {
            row[x*3+0]=src[x*4+0];
            row[x*3+1]=src[x*4+1];
            row[x*3+2]=src[x*4+2];
        }
        fwrite(row, 1, (size_t)w*3u, f);
    }
    free(row);
    fclose(f);
}

/* Materialize the a3xx RB_COPY resolve.  Keep the source FBO bound while the
 * destination texture is updated; later glBindTexGA will find it by gpuaddr. */
static void glp_resolve_to_ga(uint32_t ga, int w, int h, int from_gmem){
    if (!ga) return;
    if (getenv("GLP_RG_DUMP") && w == 614 && h == 314)
        fprintf(stderr,
                "[RT614RESOLVE] ga=0x%08x from_gmem=%d cur=%d curwh=%dx%d\n",
                ga,from_gmem,g_cur_rt,g_cur_rt_w,g_cur_rt_h);
    glp_probe_rt614("resolve",0);
    GLint prev_fbo=0, src_fbo=0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prev_fbo);
    if (from_gmem) {
        /* resolve_gmem_draw binds the exact RB_MRT_BUF_BASE GMEM offset
         * immediately before this op.  Prefer that selected color buffer;
         * offset zero is only the compatibility fallback for older streams. */
        int gm = (g_cur_rt >= 0 && g_cur_rt < g_rt_n)
               ? g_cur_rt : glp_rt_index(0);
        if (gm < 0) {
            fprintf(stderr, "[RESOLVE] missing GMEM backing for ga=0x%08x\n", ga);
            return;
        }
        src_fbo=(GLint)g_rt[gm].fbo;
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)src_fbo);
    } else {
        src_fbo=prev_fbo;
    }
    /* Explicit resolve sizes are logical.  An event-time size of zero means
     * "the complete allocation", so only that fallback uses backing extent. */
    int src_w = (g_cur_rt >= 0 && g_cur_rt < g_rt_n)
              ? g_rt[g_cur_rt].w : g_cur_rt_w;
    int src_h = (g_cur_rt >= 0 && g_cur_rt < g_rt_n)
              ? g_rt[g_cur_rt].h : g_cur_rt_h;
    /* The draw-time GMEM resolve carries its real COPY_DEST pitch/scissor.
     * Event-time presentation passes 0,0 and deliberately selects the complete
     * bound source allocation instead of the last (often 1024x96) scissor. */
    if (w<=0 || h<=0) {
        w=src_w;
        h=src_h;
    } else {
        if (w > src_w) w=src_w;
        if (h > src_h) h=src_h;
    }
    /* A3xx resolves one CP_SET_BIN tile at a time.  The collapsed host GMEM
     * attachment is full-size, but its current GL scissor remains the exact
     * global tile rectangle.  Copying the whole attachment here made every
     * later tile replace all earlier tiles; an empty final strip consequently
     * erased an otherwise valid display.  Preserve destination pixels outside
     * the current tile exactly as the hardware GMEM->memory resolve does. */
    int copy_x=0, copy_y=0, copy_w=w, copy_h=h;
    if (from_gmem && g_pm4_tile_render &&
        glIsEnabled(GL_SCISSOR_TEST)) {
        GLint sc[4]={0};
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        copy_x=sc[0]; copy_y=sc[1]; copy_w=sc[2]; copy_h=sc[3];
        if (copy_x < 0) { copy_w += copy_x; copy_x = 0; }
        if (copy_y < 0) { copy_h += copy_y; copy_y = 0; }
        if (copy_x + copy_w > w) copy_w = w - copy_x;
        if (copy_y + copy_h > h) copy_h = h - copy_y;
        if (copy_w < 0) copy_w = 0;
        if (copy_h < 0) copy_h = 0;
    }
    int idx=glp_rt_index(ga);
    /* -1 means two different things: "destination does not exist yet" for idx
     * and "the default/GMEM-collapse FBO is bound" for g_cur_rt.  Comparing
     * those sentinels directly discarded every first resolve from the default
     * FBO, leaving the later 1024x480 sampler backing all-zero. */
    if (!from_gmem && idx>=0 && idx==g_cur_rt) {
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)prev_fbo);
        return;                                  /* already texture-backed in place */
    }
    idx=glp_rt_ensure(ga,w,h);
    if (idx<0) {
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)prev_fbo);
        return;
    }
    /* glp_rt_ensure may have bound the destination while allocating/growing it. */
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)src_fbo);
    const char *trace_dir = getenv("GLP_RESOLVE_TRACE");
    long trace_start = 0;
    const char *trace_start_env = getenv("GLP_RESOLVE_TRACE_START");
    if (trace_start_env && *trace_start_env)
        trace_start = strtol(trace_start_env, NULL, 10);
    int trace = trace_dir && *trace_dir && ga == GLP_DISPLAY_GA &&
                from_gmem && g_swap_count >= trace_start;
    static unsigned trace_seq;
    static uint32_t last_src_hash, last_before_hash, last_after_hash;
    static int trace_kept;
    unsigned seq = trace ? ++trace_seq : 0;
    glp_resolve_stats src_stats, before_stats, after_stats;
    memset(&src_stats, 0, sizeof src_stats);
    memset(&before_stats, 0, sizeof before_stats);
    memset(&after_stats, 0, sizeof after_stats);
    if (trace) {
        glp_resolve_read_stats((GLuint)src_fbo, w, h, &src_stats, 1);
        glp_resolve_read_stats(g_rt[idx].fbo, w, h, &before_stats, 1);
        glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)src_fbo);
    }
    {
    const char *dump_env=getenv("PM4GL_DUMPLAYERS");
    int dump_min=dump_env ? atoi(dump_env) : 0;
    if (dump_env && glp_layer_dump_frame_ready() &&
        (dump_min <= 1 || g_rt_n >= dump_min) &&
        from_gmem && w==1024 && h==480) {
        unsigned char *px=(unsigned char*)malloc((size_t)w*h*3);
        if(px){
            /* Capture the exact collapsed-GMEM source before glCopyTexSubImage2D
             * so a later clear/swap cannot hide where pixels disappeared. */
            glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px);
            char nm[64]; snprintf(nm,sizeof nm,"resolve_src_%08x.ppm",ga);
            FILE *f=fopen(nm,"wb");
            if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
                for(int y=h-1;y>=0;y--) fwrite(px+(size_t)y*w*3,1,w*3,f);
                fclose(f); fprintf(stderr,"[RESOLVEDUMP] %s\n",nm); }
            free(px);
        }
    }
    }
    glBindTexture(GL_TEXTURE_2D,g_rt[idx].tex);
    if (copy_w > 0 && copy_h > 0)
        glCopyTexSubImage2D(GL_TEXTURE_2D,0,
                            copy_x,copy_y,copy_x,copy_y,copy_w,copy_h);
    g_rt[idx].written=1;
    g_rt[idx].ready=1;
    if (trace) {
        glp_resolve_read_stats(g_rt[idx].fbo, w, h, &after_stats, 1);
        GLint sc[4]={0}, vp[4]={0};
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        glGetIntegerv(GL_VIEWPORT, vp);
        fprintf(stderr,
                "[RESOLVETRACE] seq=%u swap=%ld ga=0x%08x src_fbo=%d "
                "wh=%dx%d copy=%d,%d+%dx%d sc=%d,%d+%dx%d "
                "vp=%d,%d+%dx%d "
                "src=%08x/rgb%ld/a%ld/o%ld/m%u/%d,%d-%d,%d "
                "before=%08x/rgb%ld/a%ld/o%ld/m%u/%d,%d-%d,%d "
                "after=%08x/rgb%ld/a%ld/o%ld/m%u/%d,%d-%d,%d\n",
                seq,g_swap_count,ga,src_fbo,w,h,
                copy_x,copy_y,copy_w,copy_h,
                sc[0],sc[1],sc[2],sc[3],vp[0],vp[1],vp[2],vp[3],
                src_stats.hash,src_stats.nonblack,
                src_stats.alpha_nonzero,src_stats.alpha_opaque,
                src_stats.alpha_max,
                src_stats.x0,src_stats.y0,src_stats.x1,src_stats.y1,
                before_stats.hash,before_stats.nonblack,
                before_stats.alpha_nonzero,before_stats.alpha_opaque,
                before_stats.alpha_max,
                before_stats.x0,before_stats.y0,before_stats.x1,before_stats.y1,
                after_stats.hash,after_stats.nonblack,
                after_stats.alpha_nonzero,after_stats.alpha_opaque,
                after_stats.alpha_max,
                after_stats.x0,after_stats.y0,after_stats.x1,after_stats.y1);
        if (trace_kept < 192 &&
            (src_stats.hash != last_src_hash ||
             before_stats.hash != last_before_hash ||
             after_stats.hash != last_after_hash)) {
            glp_resolve_dump_ppm(trace_dir,"src",seq,g_swap_count,ga,w,h,
                                 &src_stats);
            glp_resolve_dump_ppm(trace_dir,"before",seq,g_swap_count,ga,w,h,
                                 &before_stats);
            glp_resolve_dump_ppm(trace_dir,"after",seq,g_swap_count,ga,w,h,
                                 &after_stats);
            trace_kept++;
        }
        last_src_hash = src_stats.hash;
        last_before_hash = before_stats.hash;
        last_after_hash = after_stats.hash;
        free(src_stats.rgba);
        free(before_stats.rgba);
        free(after_stats.rgba);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)prev_fbo);
    if(ga==GLP_DISPLAY_GA && g_rt[idx].w>=1024 && g_rt[idx].h>=480)
        g_present_rt=idx;
    { static int n=0; if(n<40){n++;
      fprintf(stderr,"[RESOLVE] src_fbo=%d -> ga=0x%08x idx=%d tex=%u "
              "%dx%d copy=%d,%d+%dx%d err=0x%x\n",
              src_fbo,ga,idx,g_rt[idx].tex,w,h,
              copy_x,copy_y,copy_w,copy_h,glGetError()); } }
}

/* Diagnostic: read back each layer FBO's texture to layer_<ga>.ppm so we can
 * tell layer-render errors from composition errors. */
void glp_dump_layers(void){
    static int done=0;
    const char *dump_env=getenv("PM4GL_DUMPLAYERS");
    int min_rts=dump_env ? atoi(dump_env) : 0;
    int full_producers=0;
    for (int i=0;i<g_rt_n;i++)
        if (g_rt[i].ga != 0u && g_rt[i].ga != GLP_DISPLAY_GA &&
            g_rt[i].w==1024 && g_rt[i].h==480)
            full_producers++;
    /* The display RT appears before the late status/main producer surfaces.
     * Dumping at that first swap permanently missed the useful FBOs. */
    if(done || !glp_layer_dump_frame_ready() ||
       g_present_rt < 0 || full_producers < 2 ||
       (min_rts > 1 && g_rt_n < min_rts)) return;
    GLint prev_fbo=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prev_fbo);
    for (int i=0;i<g_rt_n && i<RT_MAX;i++){
        int w=g_rt[i].w,h=g_rt[i].h; if(w<=0||h<=0) continue;
        unsigned char *px=(unsigned char*)malloc((size_t)w*h*3); if(!px) continue;
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt[i].fbo);
        glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px);
        char nm[64]; snprintf(nm,sizeof nm,"layer_%08x.ppm",g_rt[i].ga);
        FILE *f=fopen(nm,"wb");
        if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
            for(int y=h-1;y>=0;y--) fwrite(px+(size_t)y*w*3,1,w*3,f); fclose(f);
            fprintf(stderr,"[LAYERDUMP] %s\n",nm); }
        free(px);
    }
    {
        int w=1024,h=480;
        unsigned char *px=(unsigned char*)malloc((size_t)w*h*3);
        if(px){
            int gm=glp_rt_index(0);
            glBindFramebuffer(GL_FRAMEBUFFER,
                              gm>=0 ? g_rt[gm].fbo : (GLuint)glp_default_fbo);
            glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px);
            FILE *f=fopen("layer_gmem.ppm","wb");
            if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
                for(int y=h-1;y>=0;y--) fwrite(px+(size_t)y*w*3,1,w*3,f);
                fclose(f); fprintf(stderr,"[LAYERDUMP] layer_gmem.ppm\n"); }
            free(px);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)prev_fbo);
    done=1;   /* one shot after layers exist */
}

int glp_gl2x(unsigned x, const int32_t *ai, int nargs, const void *data, unsigned dl)
{
    (void)nargs; (void)dl;
    switch (x) {
    case 1:  {
        /* a3xx is a TILED renderer: GRAS_CL_VPORT is set per binning/tile pass
         * (default 1.0 -> a 2x2 viewport, or one 256xN GMEM tile).  The guest
         * translator already feeds global surface geometry while collapsing
         * all GMEM tiles into one host FBO.  Keeping the final x=768,w=256 tile
         * viewport therefore left exactly the rightmost quarter of a 1024x480
         * resolve.  Expand EVERY viewport of the collapsed-GMEM target to its
         * full logical RT; memory-backed layer FBOs retain ordinary viewports. */
        int x=ai[0],y=ai[1],w=ai[2],h=ai[3];
        int tile_render = nargs >= 5 && ai[4] != 0;
        g_pm4_tile_render = tile_render;
        int collapsed_gmem = g_cur_rt >= 0 && g_rt[g_cur_rt].ga == 0;
        int backing_w = (g_cur_rt >= 0) ? g_rt[g_cur_rt].w : g_cur_rt_w;
        int backing_h = (g_cur_rt >= 0) ? g_rt[g_cur_rt].h : g_cur_rt_h;
        /* Tiny 1x2/16x8 helper metadata is not a useful surface extent; retain
         * the established backing-sized collapse for it.  Ordinary passes,
         * including non-display offscreen targets, use their real logical
         * dimensions regardless of how large the reusable GMEM texture grew. */
        int target_w = (g_cur_rt_w >= 64 && g_cur_rt_h >= 64)
                     ? g_cur_rt_w : backing_w;
        int target_h = (g_cur_rt_w >= 64 && g_cur_rt_h >= 64)
                     ? g_cur_rt_h : backing_h;
        if (g_cur_rt >= 0 && backing_w >= 1024 &&
            (x != 0 || y != 0 || w < target_w || h < target_h)) {
            static int fn;
            if (fn++ < 80) fprintf(stderr,
                "[VPFULL] ga=0x%08x logical=%dx%d backing=%dx%d "
                "in=%d,%d+%dx%d tile=%d\n",
                g_rt[g_cur_rt].ga,g_cur_rt_w,g_cur_rt_h,backing_w,backing_h,
                x,y,w,h,tile_render);
        }
        if (tile_render || collapsed_gmem || g_cur_rt < 0 ||
            w < 64 || h < 64) {
            x=0; y=0; w=target_w; h=target_h;
        }
        static int vn=0; if(vn<16){vn++; fprintf(stderr,
            "[pm4gl] glViewport in(%d %d %d %d) tile=%d -> %d %d %d %d\n",
            ai[0],ai[1],ai[2],ai[3],tile_render,x,y,w,h);}
        glViewport(x, y, w, h); return 1; }
    case 2:  glDepthRange(x_f(ai[0]), x_f(ai[1])); return 1;
    case 3:  { static int cn=0; if(cn<10){cn++; GLint fb=0,vp[4]={0}; glGetIntegerv(0x8CA6,&fb); glGetIntegerv(GL_VIEWPORT,vp); fprintf(stderr,"[pm4gl] glClear mask=0x%x boundFBO=%d vp=%d,%d,%d,%d\n",ai[0],fb,vp[0],vp[1],vp[2],vp[3]);}
             /*
              * A real GMEM sweep reaches us as several ISSUE submissions, one
              * per CP_SET_BIN tile.  Keep the programmed tile scissor enabled:
              * clearing the whole collapsed FBO here erased the four tiles
              * accumulated by preceding submissions and left only the final
              * 96-pixel strip.  Non-tile passes still use their normal scissor
              * state, while a tile clear now matches the hardware tile extent.
              */
             if(getenv("PM4GL_FORCE_CLEAR")){
                 glClearColor(1.f,0.f,0.f,1.f);
                 glClear((GLbitfield)ai[0]|GL_COLOR_BUFFER_BIT);
                 glp_note_rt_write();
                 return 1;
             }
             glClear((GLbitfield)ai[0]);
             if(ai[0]&GL_COLOR_BUFFER_BIT) glp_note_rt_write();
             glp_probe_rt614("clear",0);
             return 1; }
    case 4:  glClearColor(x_f(ai[0]), x_f(ai[1]), x_f(ai[2]), x_f(ai[3])); return 1;
    case 5:  /* Honor the real CP_SET_BIN scissor while accumulating a collapsed
              * GMEM tile sweep. Outside that explicitly tagged path the old
              * a3xx tiny/stale scissor is unsafe, so keep it disabled. */
             if (ai[0]==0x0C11 /*GL_SCISSOR_TEST*/) {
                 if (g_pm4_tile_render) glEnable((GLenum)ai[0]);
                 else glDisable((GLenum)ai[0]);
                 return 1;
             }
             if (ai[0]==0x0B44 /*GL_CULL_FACE*/ ||
                 ai[0]==0x0B71 /*GL_DEPTH_TEST*/) {
                 glDisable((GLenum)ai[0]);
                 return 1;
             }
             glEnable((GLenum)ai[0]); return 1;
    case 6:  glDisable((GLenum)ai[0]); return 1;
    case 7:  glCullFace((GLenum)ai[0]); return 1;
    case 8:  glFrontFace((GLenum)ai[0]); return 1;
    case 9:  glBlendEquationSeparate((GLenum)ai[0], (GLenum)ai[1]); return 1;
    case 10: glEnableVertexAttribArray((GLuint)ai[0]); return 1;
    case 11: { unsigned s = (unsigned)ai[0]; GLuint p = (s<256)? g_slot_prog[s] : 0;
               glUseProgram(p); g_cur_prog = p;
               /* DIAG: the real ir3_c uniform const-file (shader colors + MVP) is
                * not yet fed (Kanzi's Qualcomm const-upload isn't standard
                * CP_LOAD_STATE). Seed a visible default so geometry that reads
                * ir3_c isn't invisible-black — proves the render path + shows the
                * UI layout. c0 = mid-grey solid color; c1..c3 = identity-ish MVP
                * rows so a passthrough/MVP VS at least doesn't collapse to 0. */
               if (getenv("PM4GL_SEED_CONST") && p) {
                   extern int glp_ir3_c_loc(unsigned);
                   int loc = glp_ir3_c_loc(p);
                   if (loc >= 0) {
                       GLfloat seed[8*4] = {0};
                       seed[0]=0.55f; seed[1]=0.55f; seed[2]=0.60f; seed[3]=1.0f; /* c0 solid grey */
                       seed[4]=1.0f;  seed[9]=1.0f; seed[14]=1.0f; seed[19]=1.0f;  /* c1..c4 ~identity */
                       glUniform4fv(loc, 8, seed);
                   }
               }
               return 1; }
    case 12: glUniformMatrix4fv(ai[0], ai[1], (GLboolean)ai[2], (const GLfloat*)data); return 1;
    case 13: { /* glProgramGLSL: compile+link+cache once per slot */
        unsigned s = (unsigned)ai[0]; if (s >= 256) return 1;
        if (!g_slot_prog[s] && data && dl){
            const char *vs = (const char*)data;
            const char *fs = vs + strlen(vs) + 1;
            char *vs_r = pm4gl_reconcile_varyings(vs, fs);  /* inject missing FS varyings */
            GLuint v = pm4gl_compile(GL_VERTEX_SHADER, vs_r);
            GLuint f = pm4gl_compile(GL_FRAGMENT_SHADER, fs);
            GLuint p = glCreateProgram();
            /* Exact programs share the GLuint namespace with the old binary-
             * shader substitute path.  Zero-initialized substitute location
             * tables used to make glp_set_layermode() write location 0 before
             * every exact draw, clobbering glp_rectmode(1) back to zero. */
            if (p < MAXPROG) {
                g_prog_exact[p] = 1;
                g_subst_samp[p] = -1;
                g_subst_colorize[p] = -1;
                g_subst_layermode[p] = -1;
                g_subst_lr_loc[p] = -1;
                g_subst_lc_loc[p] = -1;
                g_prog_layermode[p] = 0;
            }
            glAttachShader(p, v); glAttachShader(p, f);
            pm4gl_bind_attrs(p, vs_r);
            glLinkProgram(p);
            GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok){ char log[1024]; glGetProgramInfoLog(p, sizeof log, 0, log);
                fprintf(stderr, "[pm4gl] program slot %u LINK FAIL: %s\n", s, log); }
            else {
                pm4gl_bind_ir3_sampler_units(p);
                fprintf(stderr, "[pm4gl] program slot %u linked OK (glprog %u)\n", s, p);
            }
            if (getenv("PM4GL_DUMPPROG"))
                fprintf(stderr,
                        "[progdump slot %u]\n"
                        "---VS-BEGIN---\n%s\n---VS-END---\n"
                        "---VS-RECONCILED-BEGIN---\n%s\n"
                        "---VS-RECONCILED-END---\n"
                        "---FS-BEGIN---\n%s\n---FS-END---\n",
                        s,vs,vs_r,fs);
            free(vs_r);
            g_slot_prog[s] = p;
        }
        return 1; }
    case 14:   /* glBindRenderTarget(gpuaddr, w, h): route to per-gpuaddr FBO */
        glp_bind_rendertarget((uint32_t)ai[0], ai[1], ai[2]);
        return 1;
    case 15: { /* glBindTexGA(target, name, gpuaddr): bind layer FBO tex if it's an RT */
        int idx = glp_rt_index((uint32_t)ai[2]);
        /* A later GL command can sample an RT after its first ordered write.
         * If it is also the current draw target, glp_rt_sample_tex() copies the
         * completed pixels into `snapshot` first, avoiding an attachment
         * feedback loop.  Before the first write, and only then, return stable
         * transparent black instead of undefined glTexImage2D storage. */
        int usable = idx >= 0 &&
                     (g_rt[idx].ready || g_rt[idx].written);
        GLuint rt = usable ? glp_rt_sample_tex(idx, (GLenum)ai[0]) : 0;
        GLuint bound;
        if (idx >= 0 && !usable) {
            bound = glp_transparent_texture((GLenum)ai[0]);
            static int un;
            if (un++ < 80)
                fprintf(stderr,
                        "[RTUNREADY] ga=0x%08x idx=%d cur=%d written=%u "
                        "ready=%u -> transparent tex=%u\n",
                        (uint32_t)ai[2], idx, g_cur_rt,
                        g_rt[idx].written, g_rt[idx].ready, bound);
        } else {
            bound = rt ? rt : (GLuint)ai[1];
        }
        glBindTexture((GLenum)ai[0], bound);
        /*
         * CPU-uploaded a3xx textures keep guest row 0 at texture T=0.  An
         * OpenGL render-target texture has the opposite framebuffer Y
         * convention.  The a3xx memory->GMEM preload uses the same PS_REPL
         * coordinates for either kind, so flip only unit-0 RT sampling in the
         * injected varying bridge.  Without this, every tile preloads the
         * previous display upside down and the new upright scene is then drawn
         * over it, producing the characteristic mirrored lower half.
         */
        {
            GLint active=GL_TEXTURE0;
            glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
            if(active==GL_TEXTURE0 && g_cur_prog){
                GLint flip=glGetUniformLocation(g_cur_prog,"glp_rtflip");
                if(flip>=0) glUniform1f(flip,usable ? 1.0f : 0.0f);
            }
        }
        { static int tn=0; if(tn<120){tn++; GLint fb=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fb);
          fprintf(stderr,"[TEXGA] ga=0x%08x name=%d idx=%d cur=%d -> tex=%u fbo=%d\n",
                  (uint32_t)ai[2],ai[1],idx,g_cur_rt,bound,fb); } }
        /* GLP_RG_DUMP is a forensic mode: retain one stable mapping for every
         * distinct guest texture/RT binding instead of losing late HMI assets
         * behind the bounded noisy [TEXGA] log above. */
        if (getenv("GLP_RG_DUMP")) {
            struct texga_seen { uint32_t ga; int name, idx, cur; GLuint tex; };
            static struct texga_seen seen[512];
            static unsigned nseen;
            unsigned i;
            for (i=0; i<nseen; i++)
                if (seen[i].ga == (uint32_t)ai[2] && seen[i].name == ai[1] &&
                    seen[i].idx == idx && seen[i].cur == g_cur_rt &&
                    seen[i].tex == bound)
                    break;
            if (i == nseen && nseen < sizeof(seen)/sizeof(seen[0])) {
                seen[nseen].ga = (uint32_t)ai[2];
                seen[nseen].name = ai[1];
                seen[nseen].idx = idx;
                seen[nseen].cur = g_cur_rt;
                seen[nseen].tex = bound;
                nseen++;
                fprintf(stderr,
                        "[TEXGAMAP] ga=0x%08x name=%d idx=%d cur=%d -> tex=%u\n",
                        (uint32_t)ai[2], ai[1], idx, g_cur_rt, bound);
            }
        }
        return 1; }
    case 16: { /* glRectMode: 0=off, 1=PS_REPL, 2=PS_REPL+position override */
        if (g_cur_prog) {
            GLint loc = glGetUniformLocation(g_cur_prog, "glp_rectmode");
            if (loc >= 0) glUniform1f(loc, (GLfloat)ai[0]);
            if (getenv("GLP_RG_DUMP") &&
                g_cur_rt_w == 512 && g_cur_rt_h == 128) {
                GLint actual=0; GLfloat value=-1.0f;
                glGetIntegerv(GL_CURRENT_PROGRAM,&actual);
                if (loc >= 0) glGetUniformfv(g_cur_prog,loc,&value);
                fprintf(stderr,
                        "[RECTMODE512] tracked=%u actual=%d set=%d loc=%d "
                        "read=%.3g err=0x%x\n",
                        g_cur_prog,actual,ai[0] ? 1 : 0,loc,value,glGetError());
            }
            static int rn;
            if (getenv("GLP_RG_DUMP") && rn++ < 120)
                fprintf(stderr,"[RECTMODE] prog=%u value=%d loc=%d\n",
                        g_cur_prog, ai[0] ? 1 : 0, loc);
        }
        return 1; }
    case 17: /* glResolveToGA(gpuaddr,w,h[,from_gmem]) */
        glp_resolve_to_ga((uint32_t)ai[0], ai[1], ai[2],
                          nargs >= 4 && ai[3] != 0);
        return 1;
    default: return 0;
    }
}
/* a3xx const-file wiring: the ir3 decompiler emits `uniform vec4 ir3_c[]`, so the
 * PM4->GL2 const upload (GL2_glUniformConsts) needs the base location of ir3_c[0]
 * on the in-use program; GLSL ES guarantees element i sits at base+i. Cache it
 * per program. -1 = program has no ir3_c (substitute/libeal) -> upload no-op. */
static int           g_ir3_c_loc[MAXPROG];
static unsigned char g_ir3_c_done[MAXPROG];
int glp_ir3_c_loc(unsigned prog){
    if(prog>=MAXPROG) return -1;
    if(!g_ir3_c_done[prog]){ g_ir3_c_loc[prog]=glGetUniformLocation(prog,"ir3_c"); g_ir3_c_done[prog]=1; }
    return g_ir3_c_loc[prog];
}

/* ---- structured render-graph dump (env GLP_RG_DUMP=/path) -------------------
 * OBSERVED ground truth of what the real Kanzi engine draws, per draw call:
 *   {program, fragment-shader id, bound FBO, bound textures units 0-3, blend
 *    state, every active float uniform (incl kzProjectionCameraWorldMatrix MVP),
 *    draw mode/count, monotonic order}.  One JSON object per line; a {"frame":N}
 *    marker opens each frame.  On swap the just-completed frame's file is renamed
 *    to <path>.latest, so a reader always gets exactly one complete, bounded
 *    frame.  This replaces blind KZB-format guessing: to know where a node draws,
 *    read its MVP here instead of re-deriving the layout math. */
static FILE *g_rg_fp = NULL; static long g_rg_order = 0, g_rg_frame = 0;
void glp_rg_frame(void){ const char*p=getenv("GLP_RG_DUMP"); if(!p||!*p) return;
    if(g_rg_fp){ fclose(g_rg_fp); g_rg_fp=NULL;
        /* A Screen post may occur between PM4 submissions.  Do not replace the
         * last useful graph with a one-line empty frame in that interval. */
        if(g_rg_order>0){ char out[600];
            if(getenv("GLP_RG_KEEP"))
                snprintf(out,sizeof out,"%s.frame_%04ld",p,g_rg_frame);
            else
                snprintf(out,sizeof out,"%s.latest",p);
            rename(p,out); } }
    g_rg_fp=fopen(p,"w"); g_rg_frame++; g_rg_order=0;
    if(g_rg_fp) fprintf(g_rg_fp,"{\"frame\":%ld}\n",g_rg_frame); }
void glp_rg_dump(unsigned mode,int count){
    if(!g_rg_fp) return;                     /* only inside a frame (after first swap) */
    unsigned prog=g_cur_prog; FILE*f=g_rg_fp;
    GLint fbo=0; glGetIntegerv(0x8CA6,&fbo);                 /* FRAMEBUFFER_BINDING */
    GLuint sh[8]; GLsizei sc=0; if(prog) glGetAttachedShaders(prog,8,&sc,sh);
    unsigned fs=(sc>1?sh[1]:(sc>0?sh[0]:0));
    GLint act=0x84C0; glGetIntegerv(0x84E0,&act);            /* ACTIVE_TEXTURE */
    int tex[4]; for(int u=0;u<4;u++){ glActiveTexture(0x84C0+u); GLint t=0;
        glGetIntegerv(0x8069,&t); tex[u]=t; }               /* TEXTURE_BINDING_2D */
    glActiveTexture(act);
    fprintf(f,"{\"o\":%ld,\"prog\":%u,\"fs\":%u,\"fbo\":%d,\"mode\":%u,\"count\":%d,\"tex\":[%d,%d,%d,%d]",
        g_rg_order++,prog,fs,fbo,mode,count,tex[0],tex[1],tex[2],tex[3]);
    if(prog<GBMAXPROG && g_prog_blend[prog].valid){ glp_blend_t*b=&g_prog_blend[prog];
        fprintf(f,",\"blend\":[%u,%u,%u,%u,%u,%u]",b->rgbEq,b->rgbSrc,b->rgbDst,b->aEq,b->aSrc,b->aDst); }
    if(prog){ GLint nu=0; glGetProgramiv(prog,GL_ACTIVE_UNIFORMS,&nu);
        fprintf(f,",\"u\":{"); int first=1;
        for(GLint i=0;i<nu;i++){ char nm[64]; GLint sz=0; GLenum ty=0; GLsizei ln=0;
            glGetActiveUniform(prog,i,sizeof nm,&ln,&sz,&ty,nm);
            int nc=(ty==0x8B5C)?16:(ty==0x8B5B)?9:(ty==0x8B52)?4:(ty==0x8B51)?3:(ty==0x8B50)?2:(ty==0x1406)?1:0;
            if(!nc && ty!=0x8B5E) continue;                  /* numeric values + sampler2D unit */
            char*br=strchr(nm,'['); if(br)*br=0;             /* array uniform -> base name */
            GLint loc=glGetUniformLocation(prog,nm); if(loc<0) continue;
            if(ty==0x8B5E /* SAMPLER_2D */){
                GLint unit=0; glGetUniformiv(prog,loc,&unit);
                fprintf(f,"%s\"%s\":{\"sampler2D\":%d}",first?"":",",nm,unit);
                first=0; continue;
            }
            float v[16]; glGetUniformfv(prog,loc,v);
            fprintf(f,"%s\"%s\":[",first?"":",",nm); first=0;
            for(int k=0;k<nc;k++) fprintf(f,"%s%.5g",k?",":"",v[k]); fprintf(f,"]"); }
        fprintf(f,"}"); }
    fprintf(f,"}\n"); }
/* set glpColorize on the current program (called from the sampler-mirror in
 * gl2_dispatch when the bound Texture is an uploaded atlas -> use the Kanzi
 * colorize-atlas font/icon path instead of plain passthrough). */
void glp_set_colorize(int on){
    if(g_cur_prog>=MAXPROG || g_prog_exact[g_cur_prog]) return;
    int l=g_subst_colorize[g_cur_prog];
    if(l>=0) glUniform1f(l, on?1.0f:0.0f); }
/* Kanzi bound the sampler at `loc` on the current substituted program. If it's the
 * LayerRender/LayerComposition sampler, this program is an engine layer-composition
 * built-in -> remember the mode so the substitute does the real composite. */
void glp_note_sampler(int loc){
    if(g_cur_prog>=MAXPROG || g_prog_exact[g_cur_prog] || loc<0) return;
    if(loc==g_subst_lr_loc[g_cur_prog])      g_prog_layermode[g_cur_prog]=1;
    else if(loc==g_subst_lc_loc[g_cur_prog]) g_prog_layermode[g_cur_prog]=2; }
/* push the detected layer mode into the substitute's glpLayerMode uniform (per draw) */
void glp_set_layermode(void){
    if(g_cur_prog>=MAXPROG || g_prog_exact[g_cur_prog]) return;
    int l=g_subst_layermode[g_cur_prog];
    if(l>=0) glUniform1f(l, (float)g_prog_layermode[g_cur_prog]); }
/* Kanzi ships shaders as Adreno GPU binaries (no GLSL source), so the attached
 * shaders reach us empty -> link fails. Substitute generic GLSL (using Kanzi's
 * real attribute names kzPosition/kzTextureCoordinate0/kzColor0 + the standard
 * kzProjectionCameraWorldMatrix MVP uniform) into any source-less shader so the
 * program links and geometry draws. Not pixel-accurate, but produces frames. */
/* Recovered-GLSL substitutes. Kanzi ships only Adreno binaries over the wire and
 * never falls back to glShaderSource, so we reconstruct the Kanzi "Materials_common"
 * shaders from the offline KZB GLSL extraction. The KEY fix vs the old generic
 * substitute: declare the REAL uniform/sampler names (Texture, LayerRenderTexture,
 * Color, BlendIntensity, LayerRenderOpacity) so Kanzi's own glGetUniformLocation +
 * glUniform* bindings land on them. The host then inits Color/BlendIntensity/
 * LayerRenderOpacity to 1 after link, so a material that doesn't use one is
 * unaffected. Texture and LayerRenderTexture both default to unit 0, so whichever
 * Kanzi binds, sampling Texture reads the right thing (icon atlas OR a layer FBO →
 * this is what finally composits the menu's offscreen layers). */
static const char *SUBST_VS_TEX =
    "#define lowp\n#define mediump\n#define highp\n"
    "uniform mat4 kzProjectionCameraWorldMatrix;\n"
    "uniform float glp_rtflip;\n"
    "attribute vec4 kzPosition;\n"
    "attribute vec2 kzTextureCoordinate0;\n"
    "attribute vec4 kzColor0;\n"
    "varying vec2 vTexCoord;\nvarying vec4 vColor0;\n"
    "void main(){ vTexCoord=vec2(kzTextureCoordinate0.x, "
    "mix(kzTextureCoordinate0.y,1.0-kzTextureCoordinate0.y,glp_rtflip)); "
    "vColor0=kzColor0;\n"
    "  gl_Position = kzProjectionCameraWorldMatrix * vec4(kzPosition.xyz,1.0); }\n";
/* Universal built-in substitute. CRITICAL FIX vs the old one: it declares AND
 * KEEPS LIVE the layer samplers (LayerRenderTexture/LayerCompositionTexture) +
 * LayerOpacity, so their glGetUniformLocation resolves to a real location ->
 * Kanzi actually binds the layer FBO to them (the dispatch mirrors that bind
 * onto our active 'Texture' sampler). The old shader declared LayerRenderTexture
 * but never sampled it, so GLSL optimized it out -> location -1 -> Kanzi skipped
 * the bind -> the layer-composition pass sampled garbage/unit-0 -> black menu.
 * glpKeep is a uniform fixed to 0 so the compiler can't fold the aux samples
 * away, but they contribute nothing to the visible output. */
static const char *SUBST_FS_TEX =
    "#define lowp\n#define mediump\n#define highp\n"
    "uniform sampler2D Texture;\n"
    "uniform sampler2D LayerRenderTexture;\n"
    "uniform sampler2D LayerCompositionTexture;\n"
    "uniform vec4 Color;\n"
    "uniform float BlendIntensity;\n"
    "uniform float LayerRenderOpacity;\n"
    "uniform float LayerOpacity;\n"
    /* ealFontColor: the truetype font material's color uniform. If the substitute
     * lacks it, glGetUniformLocation->-1 -> Kanzi's font material setup fails ->
     * Dummytype fallback -> null glyph image -> crash. Declaring+keeping it live
     * keeps the font material valid (real truetype path). */
    "uniform vec4 ealFontColor;\n"
    "uniform float glpKeep;\n"
    /* glpColorize: 0 = plain/composite (passthrough * Color * opacities), 1 = the
     * real Kanzi colorize-atlas path for fonts/icons whose atlas packs r=colorize
     * mask, g=alpha, b=grey base (extracted from the KZB GLSL). The generic
     * passthrough showed those 3 channels straight as R/G/B -> rainbow. Set per
     * draw from glp_set_colorize() when the bound Texture is an uploaded atlas. */
    "uniform float glpColorize;\n"
    /* glpLayerMode: 0 = atlas/material, 1 = layer-render composite (eal built-in
     * `vec4(rgb, a*LayerRenderOpacity)`), 2 = layer-composition composite
     * (`vec4(rgb, a*LayerOpacity)`). Set per draw from which sampler Kanzi bound:
     * the binary-only engine composition built-ins sample LayerRender/Composition
     * Texture, and the OLD substitute sampled `Texture` (wrong) and dropped the
     * layer (glpKeep=0) -> the drum drop-shadow/gloss layers composited as hard
     * opaque black. Now we do the real alpha-preserving composite for them. */
    "uniform float glpLayerMode;\n"
    "varying vec2 vTexCoord;\nvarying vec4 vColor0;\n"
    "void main(){\n"
    "  vec4 lr = texture2D(LayerRenderTexture, vTexCoord);\n"
    "  vec4 lc = texture2D(LayerCompositionTexture, vTexCoord);\n"
    "  vec4 t  = texture2D(Texture, vTexCoord);\n"
    "  vec4 comp = t * Color * BlendIntensity * LayerRenderOpacity * LayerOpacity;\n"
    "  vec4 col; col.rgb = vec3(t.r) * Color.rgb + t.b; col.a = t.g * BlendIntensity;\n"
    "  vec4 atlas = mix(comp, col, glpColorize) + glpKeep * ealFontColor;\n"
    "  vec4 lrc = vec4(lr.rgb, lr.a * LayerRenderOpacity);\n"
    "  vec4 lcc = vec4(lc.rgb, lc.a * LayerOpacity);\n"
    "  gl_FragColor = (glpLayerMode > 1.5) ? lcc : (glpLayerMode > 0.5) ? lrc : atlas;\n"
    "}\n";
static const char *SUBST_VS_SOLID =
    "#define lowp\n#define mediump\n#define highp\n"
    "uniform mat4 kzProjectionCameraWorldMatrix;\n"
    "attribute vec4 kzPosition;\n"
    "void main(){ gl_Position = kzProjectionCameraWorldMatrix * vec4(kzPosition.xyz,1.0); }\n";
static const char *SUBST_FS_SOLID =
    "#define lowp\n#define mediump\n#define highp\n"
    "uniform vec4 Color;\n"
    "uniform float BlendIntensity;\n"
    "void main(){ gl_FragColor = vec4(Color.rgb,1.0) * Color.a * BlendIntensity; }\n";
/* per-program: does it bind kzTextureCoordinate0? (textured vs solidColor family) */
static void inject_substitute(uint32_t prog){
    /* GLPASS_NO_SUBST: skip substitution so the binary-path (glShaderBinary
     * no-op -> empty shaders) link FAILS -> Kanzi sees LINK_STATUS=FALSE and
     * recompiles from the REAL GLSL via glShaderSource, which we then use. */
    if (getenv("GLPASS_NO_SUBST")) return;
    int textured = (prog<MAXPROG)? g_prog_tex[prog] : 1;
    GLuint sh[8]; GLsizei cnt=0; glGetAttachedShaders(prog, 8, &cnt, sh);
    for(GLsizei i=0;i<cnt;i++){
        /* real EAL source? then keep it. (Don't trust GL_SHADER_SOURCE_LENGTH:
         * strip_precision's preamble makes even empty shaders report >1.) */
        if(sh[i]<MAXSHADER && g_shader_has_src[sh[i]]) continue;
        GLint type=0; glGetShaderiv(sh[i], GL_SHADER_TYPE, &type);
        const char *src = (type==GL_VERTEX_SHADER)
            ? (textured? SUBST_VS_TEX : SUBST_VS_SOLID)
            : (textured? SUBST_FS_TEX : SUBST_FS_SOLID);
        glShaderSource(sh[i], 1, &src, 0); glCompileShader(sh[i]);
        GLint ok=0; glGetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if(!ok){ char log[512]; glGetShaderInfoLog(sh[i],sizeof log,0,log);
            fprintf(stderr,"[inject] shader %u(type=0x%x) COMPILE FAIL: %s\n",sh[i],type,log); }
    }
}
static void g_link(void *st,uint32_t p){ (void)st;
    if (p < MAXPROG) g_prog_exact[p] = 0;
    inject_substitute(p);
    /* propagate any per-shader blendoperation (from the fragment shader) to the
     * program, so g_draw can apply it */
    { GLuint sh[8]; GLsizei cnt=0; glGetAttachedShaders(p, 8, &cnt, sh);
      for(GLsizei i=0;i<cnt;i++) if(sh[i]<MAXSHADER && g_shader_blend[sh[i]].valid && p<GBMAXPROG)
          g_prog_blend[p] = g_shader_blend[sh[i]]; }
    glLinkProgram(p);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[512]; GLsizei n=0; glGetProgramInfoLog(p,sizeof log,&n,log);
        fprintf(stderr,"[g_link] program %u LINK FAILED: %.*s\n",p,(int)n,log); return; }
    int textured = (p<MAXPROG)? g_prog_tex[p] : 1;
    fprintf(stderr,"[g_link] program %u linked OK (%s)\n",p, textured?"textured":"solid");
    /* Init the material uniforms so a material that doesn't bind one is unaffected:
     * Color/BlendIntensity/LayerRenderOpacity default to 1, samplers to unit 0.
     * Kanzi's own glUniform* then overrides whatever the real material uses. */
    GLint cur=0; glGetIntegerv(GL_CURRENT_PROGRAM,&cur); glUseProgram(p);
    GLint l;
    if((l=glGetUniformLocation(p,"Color"))>=0)             glUniform4f(l,1,1,1,1);
    if((l=glGetUniformLocation(p,"BlendIntensity"))>=0)    glUniform1f(l,1.0f);
    if((l=glGetUniformLocation(p,"LayerRenderOpacity"))>=0)glUniform1f(l,1.0f);
    if((l=glGetUniformLocation(p,"LayerOpacity"))>=0)      glUniform1f(l,1.0f);
    if((l=glGetUniformLocation(p,"glpKeep"))>=0)           glUniform1f(l,0.0f);
    if((l=glGetUniformLocation(p,"ealFontColor"))>=0)      glUniform4f(l,1,1,1,1);
    if((l=glGetUniformLocation(p,"Texture"))>=0)           glUniform1i(l,0);
    if((l=glGetUniformLocation(p,"LayerRenderTexture"))>=0)glUniform1i(l,0);
    if((l=glGetUniformLocation(p,"LayerCompositionTexture"))>=0)glUniform1i(l,0);
    if((l=glGetUniformLocation(p,"glpColorize"))>=0)      glUniform1f(l,0.0f);
    if((l=glGetUniformLocation(p,"glpLayerMode"))>=0)     glUniform1f(l,0.0f);
    if((l=glGetUniformLocation(p,"glp_rtflip"))>=0)       glUniform1f(l,0.0f);
    glUseProgram(cur);
    if(p<MAXPROG){ g_subst_samp[p] = glGetUniformLocation(p,"Texture");
        g_subst_colorize[p] = glGetUniformLocation(p,"glpColorize");
        g_subst_layermode[p] = glGetUniformLocation(p,"glpLayerMode");
        g_subst_lr_loc[p] = glGetUniformLocation(p,"LayerRenderTexture");
        g_subst_lc_loc[p] = glGetUniformLocation(p,"LayerCompositionTexture");
        g_prog_layermode[p] = 0; } }
static int32_t g_getProgramiv(void *st,uint32_t p,uint32_t pn){ (void)st; GLint v=0; glGetProgramiv(p,pn,&v); return v; }
static void g_use(void *st,uint32_t p){ (void)st; glUseProgram(p); g_cur_prog=p; }
static void g_enableVAA(void *st,uint32_t i){ (void)st; glEnableVertexAttribArray(i); }
static void g_vap(void *st,uint32_t i,int sz,uint32_t ty,uint8_t nm,int str,const void*d,uint32_t nb){
    gl_state*g=st; if(i>=16) return;
    { static int w[16]={0}; if(!w[i]){w[i]=1; fprintf(stderr,"[vap-client] attr %u sz=%d ty=%u stride=%d nbytes=%u\n",i,sz,ty,str,nb);} }
    free(g->vbuf[i]); g->vbuf[i]=malloc(nb); memcpy(g->vbuf[i],d,nb);
    glVertexAttribPointer(i,sz,ty,nm,str,g->vbuf[i]);   /* per-index buffer: two arrays coexist */
}
extern void glp_guard_attribs(const char *who);
static void g_draw(void *st,uint32_t m,int f,int c){ (void)st; glp_guard_attribs("glDrawArrays"); glp_apply_blend(g_cur_prog);
    if(getenv("GLP_DRAW_LOG")){ extern int glp_unit_tex_fmt(int); extern unsigned glp_unit_tex_size(int);
        GLuint sh[8]; GLsizei sc=0; glGetAttachedShaders(g_cur_prog,8,&sc,sh);
        static long dn=0; if(dn<600){dn++; fprintf(stderr,"[draw] prog=%u fs=%u vtx=%d texfmt0=0x%x texsz0=0x%x\n",g_cur_prog,(sc>1?sh[1]:(sc>0?sh[0]:0)),c,glp_unit_tex_fmt(0),glp_unit_tex_size(0)); } } glp_set_layermode(); glp_rg_dump(m,c); glDrawArrays(m,f,c); glp_note_rt_write();
    static long n=0; ++n; if(n<=60||n%500==0) fprintf(stderr,"[g_draw] DA #%ld mode=0x%x count=%d prog=%u\n",n,m,c,g_cur_prog); }
static void g_enable(void *st,uint32_t c){ (void)st; glEnable(c); }
static void g_disable(void *st,uint32_t c){ (void)st; glDisable(c); }
static void g_cull(void *st,uint32_t m){ (void)st; glCullFace(m); }
static int32_t g_getUniformLoc(void *st,uint32_t p,const char*n){ (void)st; int32_t l=glGetUniformLocation(p,n);
    static int c=0; if(c<24){c++; fprintf(stderr,"[g_getUniformLoc] prog %u name '%s' -> %d\n",p,n,l);} return l; }
static void g_uniform1f(void *st,int l,float v){ (void)st; glUniform1f(l,v); }
static void g_uniform2fv(void *st,int l,int c,const float*v){ (void)st; glUniform2fv(l,c,v); }
static void g_uniformm4(void *st,int l,int c,const float*v){ (void)st; glUniformMatrix4fv(l,c,GL_FALSE,v); }

glp_backend *glp_backend_gl(void) {
    static glp_backend be;
    memset(&g_gl,0,sizeof g_gl);
    be.st=&g_gl;
    be.eglGetDisplay=g_getDisplay; be.eglInitialize=g_initialize; be.eglChooseConfig=g_chooseConfig;
    be.eglCreateContext=g_createCtx; be.eglCreateWindowSurface=g_createWin;
    be.eglMakeCurrent=g_makeCurrent; be.eglSwapBuffers=g_swap;
    be.viewport=g_viewport; be.clearColor=g_clearColor; be.clear=g_clear;
    be.createShader=g_createShader; be.shaderSource=g_shaderSource; be.compileShader=g_compile;
    be.getShaderiv=g_getShaderiv; be.createProgram=g_createProgram; be.attachShader=g_attach;
    be.bindAttribLocation=g_bindAttr; be.linkProgram=g_link; be.getProgramiv=g_getProgramiv;
    be.useProgram=g_use; be.enableVertexAttribArray=g_enableVAA;
    be.vertexAttribPointer=g_vap; be.drawArrays=g_draw;
    be.enable=g_enable; be.disable=g_disable; be.cullFace=g_cull;
    be.getUniformLocation=g_getUniformLoc; be.uniform1f=g_uniform1f;
    be.uniform2fv=g_uniform2fv; be.uniformMatrix4fv=g_uniformm4;
    return &be;
}
