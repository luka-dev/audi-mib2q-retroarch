/*
 * dsi_audio_provider.cpp — native dsi.audio.DSIAudioManagement comm PROVIDER.
 *
 * WHY THIS EXISTS
 * ---------------
 * Phase 2b proved our CLIENT side is genuine: the proxy constructs, connect()
 * returns 0, and the framework's own agent logged OUR uuid
 *   service N/A: dropped proxy=['dsi.audio.DSIAudioManagement'=07302fa7-...]
 * "dropped" because the real provider (AudioProcess) lives on the RCC, which
 * does not exist offline. So offline the proxy can never go alive and ID 8
 * (getActiveEntertainmentConnection) can never be exercised.
 *
 * This binary supplies the missing half: it registers a DSIAudioManagement
 * provider on the LOCAL mesh so the client proxy has something to bind to.
 *
 * WHY THE PREVIOUS ATTEMPT (phase 2c) FAILED
 * ------------------------------------------
 * 2c built dio_manager's *fallback* registration record — the branch dio's own
 * source marks "// master proxy not required". registerService() returned 0 but
 * the service was never published. The correct shape is the GENERATED one, RE'd
 * from persistenceTNG sub_19B22C, and already proven to publish by
 *   Tools/qnx-carplay-emu/host/kp_provider.cpp  (dsi.keypanel.DSIKeyPanel).
 * build_audio_registration() below is that construction copied verbatim; the
 * ONLY deltas are the two UUIDs and the proxy .so name.
 *
 * UUID ROLES (why this pairing and not the reverse)
 * -------------------------------------------------
 * kp_provider registers ifUUID = BEFF9A63 (keypanel factory record's lead UUID)
 * and the HMI client looks that same UUID up as its proxy instance. Our client
 * probe passed 07302FA7 as IdentityArgs[0] (instance) and the agent echoed it
 * back in the drop log -> 07302FA7 is the INTERFACE uuid, 9DEC172B the TYPE.
 *
 * SUCCESS CRITERION, in order:
 *   1. registerService() -> 0                          (2c got this much)
 *   2. the client proxy's lifecycle flips pending(0) -> alive(1)   <- the real test
 *   3. client sends ID 8; kp_on_connect-style capture logs the stub
 *
 * Build: ./build_audio_provider.sh   (links against generated stub .so's — the
 * firmware libs have no .dynamic section and cannot be linked against directly).
 * Run:   LD_PRELOAD=/path/libdsiaudioprovider.so, grafted into `servicemgr`.
 *        NEVER the broker: the broker's CoreApi +28 is NULL (RE'd in kp).
 */
#include "comm_abi.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>

/* dword index helpers into the 108-byte ServiceRegistration impl */
static inline void set_w(void* p, int i, uint32_t v){ ((uint32_t*)p)[i] = v; }
static inline void set_p(void* p, int i, void* v)   { ((void**)p)[i]    = v; }

/* util::SharedPtrDefaultBase copy ctor — not in comm_abi.h, only reachable on the
 * providerImpl!=NULL path (which we don't take; kept so the shape stays verbatim). */
extern "C" void util_SharedPtr_copy(void* self, const void* src)
    asm("_ZN4util20SharedPtrDefaultBaseC1ERKS0_");

static void* reg_alloc(size_t n){ return calloc(1, n); }

/* ---- listener / provider active object -------------------------------------
 * clientChange dispatches a connect to listener->vtbl[+0x54](self,nClients,stub+8)
 * and a disconnect to +0x58. Everything else is a no-op returning 0. Our own C++
 * vtable would work too, but a hand-built one keeps the slot indices explicit. */
static int  au_listener_noop(void){ return 0; }
static void* au_listener_vtbl[48];
static void* au_listener_obj[12];
static volatile void* g_au_stub = 0;    /* live per-client stub, captured at connect */

static int au_on_connect(void* self, int nClients, void* proxyPtr){
    (void)self;
    g_au_stub = (void*)((char*)proxyPtr - 8);   /* 3rd arg is stub+8 */
    fprintf(stderr, "[au] CONNECT clients=%d stub=%p\n", nClients, (void*)g_au_stub);
    fflush(stderr);
    return 0;
}
static int au_on_disconnect(void* self, int nClients, void* proxyPtr){
    (void)self; (void)proxyPtr;
    fprintf(stderr, "[au] DISCONNECT clients=%d\n", nClients); fflush(stderr);
    g_au_stub = 0;
    return 0;
}
static void* au_make_listener(void){
    for (int i = 0; i < 48; i++) au_listener_vtbl[i] = (void*)au_listener_noop;
    au_listener_vtbl[0x54/4] = (void*)au_on_connect;
    au_listener_vtbl[0x58/4] = (void*)au_on_disconnect;
    au_listener_obj[0] = (void*)au_listener_vtbl;
    return (void*)au_listener_obj;
}

/* ---- the generated ServiceRegistration (persistenceTNG sub_19B22C) ----------
 * Copied verbatim from kp_provider.cpp::build_keypanel_registration. Do not
 * "clean this up": every offset here is an RE'd constant, and the two comments
 * below mark places where the obvious-looking alternative crashes or silently
 * fails to publish. */
static void* build_audio_registration(void* interfaceUUID, void* typeUUID,
                                      void* activeObjFactory,
                                      void* lifeListener, void* otherListener,
                                      const void* providerImplSharedPtr)
{
    uint8_t* impl = (uint8_t*)reg_alloc(SZ_SERVICEREG_IMPL);
    comm_LifecycleImpl_ctor(impl, /*State=*/0, lifeListener);
    /* KEEP impl vptr = LifecycleImpl (NOT ServiceRegistration): isAlive/isDead on
     * the handle calls impl->vtbl[+12]. With the ServiceRegistration vtable that
     * recurses into itself -> impl[1] NULL -> SIGSEGV. */
    ipl_UUID_copy(impl + 28, interfaceUUID);        /* [7..]  interface UUID copy */
    set_w(impl, 12, 0x00000000u);                   /* [12] instance id = 0. 0x80000000
                                                     * (auto-gen) yields a random id the
                                                     * client's lookup won't match. */
    set_p(impl, 14, activeObjFactory);              /* [14] */
    set_w(impl, 15, 0);
    set_w(impl, 16, 1);                             /* [16] refcount */
    set_w(impl, 17, 0);
    set_p(impl, 18, otherListener);                 /* [18] */
    if (providerImplSharedPtr)                      /* [19..21] SharedPtr providerImpl */
        util_SharedPtr_copy(impl + 76, providerImplSharedPtr);
    ipl_UUID_copy(impl + 88, typeUUID);             /* [22..] type UUID copy (ends at 108) */
    set_p(impl, 13, impl + 88);                     /* [13] -> &type UUID copy */

    void** handle = (void**)reg_alloc(SZ_SERVICEREG_HANDLE);
    handle[0] = COMM_VPTR(comm_vtable_ServiceRegistration);
    handle[1] = impl;
    handle[2] = COMM_VPTR(comm_vtable_TrackedReferenceBase);
    (*(uint32_t*)(impl + 64))++;                    /* handle holds a ref */
    return handle;
}


/* ---- phase 2g: give the stub something to dispatch INTO -------------------
 * The generated dispatcher (libdsiaudioproxy sub_5A68,
 * "DSIAudioManagementRPCStub::process") routes every method through
 *     (*(**(stub+4) + N))(*(stub+4), args..., stub+8)
 * i.e. stub[1] is the provider implementation -- and the stub ctor sub_43B4
 * sets it to 0. We registered with providerImpl=NULL (agent DefaultAOFactory),
 * so it stays 0 and any incoming call would deref NULL. Nothing dispatched so
 * far only because no call had a handler to reach.
 *
 * Rather than reconstruct the AOFactory plumbing that would fill stub[1], wrap
 * the factory record's stub creator: call the shipped one, then plant our own
 * implementation object. Same technique that worked for the client's pc/pd.
 *
 * Handler slot map, read straight out of the dispatcher's switch (this also
 * re-derives the phase-1 wire-ID map by an independent route):
 *    id 1 ->+48   id 2 ->+40   id 3 ->+44   id 5 ->+0    id 7 ->+8
 *    id 8 ->+12   id 9 ->+24   id 11->+4    id 12->+16   id 14->+36
 *    id 15->+28   id 16->+32   id 17->+20   id 24->+52
 * id 12 takes 3 ints (requestConnection) and id 17 two ints + a bool
 * (setVolumeLock), exactly as phase 1 concluded from the argument shapes.
 * Every handler's LAST argument is stub+8, the stub's own reply proxy.
 */
static void*  g_impl_vtbl[16];
static void*  g_impl_obj[4];
static void*  (*g_real_stub_create)(void*, void*, void*, uint32_t);

/* The shipped stub ctor sub_2B8C is trivial (new(0x30) + sub_43B4), so the hook
 * replicates it and calls sub_43B4 directly. */
typedef void* (*stub_ctor_t)(void*, void*, void*, void*, uint32_t);
static stub_ctor_t    g_real_stub_ctor;      /* libbase + 0x43B4 */
static unsigned char* g_libbase;

static volatile int g_impl_calls;

static int au_slot_log(int slot, void* self, int a, int b, int c)
{
    g_impl_calls++;
    fprintf(stderr, "[au] *** impl vslot[%d] (+0x%02x) self=%p a=%08x b=%08x c=%08x%s\n",
            slot, slot * 4, self, (unsigned)a, (unsigned)b, (unsigned)c,
            slot == 3 ? "   <<<<< ID 8 ARRIVED" : "");
    fflush(stderr);
    return 0;
}
static int au_slot_00(void* s,int a,int b,int c){ return au_slot_log(0,s,a,b,c); }
static int au_slot_01(void* s,int a,int b,int c){ return au_slot_log(1,s,a,b,c); }
static int au_slot_02(void* s,int a,int b,int c){ return au_slot_log(2,s,a,b,c); }
static int au_slot_03(void* s,int a,int b,int c){ return au_slot_log(3,s,a,b,c); }
static int au_slot_04(void* s,int a,int b,int c){ return au_slot_log(4,s,a,b,c); }
static int au_slot_05(void* s,int a,int b,int c){ return au_slot_log(5,s,a,b,c); }
static int au_slot_06(void* s,int a,int b,int c){ return au_slot_log(6,s,a,b,c); }
static int au_slot_07(void* s,int a,int b,int c){ return au_slot_log(7,s,a,b,c); }
static int au_slot_08(void* s,int a,int b,int c){ return au_slot_log(8,s,a,b,c); }
static int au_slot_09(void* s,int a,int b,int c){ return au_slot_log(9,s,a,b,c); }
static int au_slot_10(void* s,int a,int b,int c){ return au_slot_log(10,s,a,b,c); }
static int au_slot_11(void* s,int a,int b,int c){ return au_slot_log(11,s,a,b,c); }
static int au_slot_12(void* s,int a,int b,int c){ return au_slot_log(12,s,a,b,c); }
static int au_slot_13(void* s,int a,int b,int c){ return au_slot_log(13,s,a,b,c); }
static int au_slot_14(void* s,int a,int b,int c){ return au_slot_log(14,s,a,b,c); }
static int au_slot_15(void* s,int a,int b,int c){ return au_slot_log(15,s,a,b,c); }

/* Wrap the stub's own dispatcher so we can see every method that reaches it.
 * The client can now provably deliver (id 18 came through and was reported as
 * unknown), but id 8 produces no handler call and no crash, so we need to know
 * whether process() runs for it at all. vtable off_210A8 slot[1] = sub_5A68. */
static void*  g_stub_vtbl[8];
/* NB: process() returns `undefined1` -- a BYTE (Ghidra). Typing the pointer as
 * int-returning left the upper bits of r0 undefined, so the earlier "-> 0"
 * readings were not trustworthy evidence about which branch ran. */
static unsigned char (*g_real_process)(void*, int, void*);

/* Wrap the DESERIALIZER's "read int" (its vtable slot 11 / +0x2c, the one every
 * argument-taking case calls first). Both decompilers say case 7/8 must call it
 * before anything else, so if this never fires the switch never took those
 * cases -- which is the only hypothesis left standing. */
static void*  g_deser_vtbl[24];
static void (*g_real_read_int)(void*, int*);
static volatile int g_read_calls;

static void au_read_int_trace(void* self, int* out)
{
    g_read_calls++;
    g_real_read_int(self, out);
    fprintf(stderr, "[au]     read_int -> %d (err=%d)\n",
            out ? *out : -1, ((unsigned char*)self)[4]);
    fflush(stderr);
}

static void au_hook_deserializer(void* deser)
{
    int i;
    if (!deser) return;
    for (i = 0; i < 24; i++) g_deser_vtbl[i] = (*(void***)deser)[i];
    g_real_read_int = (void(*)(void*, int*))g_deser_vtbl[11];   /* +0x2c */
    g_deser_vtbl[11] = (void*)au_read_int_trace;
    *(void**)deser = (void*)g_deser_vtbl;
    fprintf(stderr, "[au] deserializer %p hooked (real read_int=%p)\n",
            deser, (void*)g_real_read_int);
    fflush(stderr);
}

static int au_process_trace(void* stub, int method_id, void* msg)
{
    unsigned char  rc;
    unsigned char* deser = (unsigned char*)((void**)stub)[9];   /* stub+0x24 */
    int before, err_before, err_after;

    if (deser && *(void**)deser != (void*)g_deser_vtbl) au_hook_deserializer(deser);
    err_before = deser ? deser[4] : -1;
    if (msg) {
        uint32_t* m = (uint32_t*)msg;
        fprintf(stderr, "[au]     msg[0..3]=%08x %08x %08x %08x\n", m[0], m[1], m[2], m[3]);
    }
    before     = g_impl_calls;
    /* Disassembly (0x1635c..0x16374) shows the implementation call is
     * unconditional:  r0=*(stub+4); r3=*r0; r3=*(r3+0xc); blx r3.
     * So resolve that exact chain here and compare it against our own symbols --
     * we have been printing the vtable read out of impl but never checking it
     * really is OUR array, nor that slot 3 really is our thunk. */
    {
        void*  impl  = ((void**)stub)[1];
        void** ivt   = impl ? *(void***)impl : NULL;
        void*  slot3 = ivt ? ivt[3] : NULL;
        fprintf(stderr,
            "[au] >>> process(mid=%d) deser=%p err=%d\n"
            "[au]     impl=%p (&g_impl_obj=%p %s)\n"
            "[au]     ivt =%p (&g_impl_vtbl=%p %s)\n"
            "[au]     ivt[3]=%p (&au_slot_03=%p %s)\n",
            method_id, deser, err_before,
            impl,  (void*)&g_impl_obj[0], impl  == (void*)&g_impl_obj[0] ? "MATCH" : "MISMATCH",
            (void*)ivt, (void*)g_impl_vtbl,  (void*)ivt == (void*)g_impl_vtbl ? "MATCH" : "MISMATCH",
            slot3, (void*)au_slot_03, slot3 == (void*)au_slot_03 ? "MATCH" : "MISMATCH");
    }
    fflush(stderr);

    /* THE BUG, finally visible: at stub-creation our object is installed and
     * matches, but by dispatch time stub[1] points somewhere else -- the
     * framework calls setImplementation (vtable slot[0], sub_344C) with the
     * object its AOFactory produced, overwriting ours. Every "no dispatch"
     * result so far was us watching an object nobody calls. Re-plant ours right
     * before the dispatcher runs. */
    if (((void**)stub)[1] != (void*)g_impl_obj) {
        fprintf(stderr, "[au]     re-planting impl %p -> %p\n",
                ((void**)stub)[1], (void*)g_impl_obj);
        ((void**)stub)[1] = (void*)g_impl_obj;
    }
    rc = g_real_process(stub, method_id, msg);

    err_after = deser ? deser[4] : -1;
    /* Both decompilers agree case 7/8 has exactly two outcomes: deserialize
     * error -> return 1, or the implementation call. err_after tells which. */
    fprintf(stderr, "[au] <<< process(mid=%d) -> %u   implCalls %d->%d   readCalls=%d   deserErr %d->%d\n",
            method_id, (unsigned)(rc & 0xFF), before, g_impl_calls, g_read_calls,
            err_before, err_after);
    fflush(stderr);
    return rc & 0xFF;
}

static void* au_stub_create_hook(void* a1, void* a2, void* a3, uint32_t a4)
{
    void* stub = calloc(1, 0x30);         /* sub_2B8C: operator new(0x30) */
    if (stub) {
        int i;
        g_real_stub_ctor(stub, a1, a2, a3, a4);
        ((void**)stub)[1] = (void*)g_impl_obj;   /* the implementation pointer */
        for (i = 0; i < 7; i++)
            g_stub_vtbl[i] = *(void**)(g_libbase + 0x210A8 + i * 4);
        g_real_process = (unsigned char(*)(void*,int,void*))g_stub_vtbl[1];
        g_stub_vtbl[1] = (void*)au_process_trace;
        *(void**)stub = (void*)g_stub_vtbl;
        fprintf(stderr, "[au] *** stub created %p -> impl=%p (&g_impl_obj=%p &g_impl_vtbl=%p slot3=%p) ***\n",
                stub, (void*)g_impl_obj, (void*)&g_impl_obj[0], (void*)g_impl_vtbl, g_impl_vtbl[3]);
    }
    fflush(stderr);
    return stub;
}

/* Patch an ARM function's entry with an absolute branch:
 *      LDR PC, [PC, #-4]   (0xE51FF004)
 *      .word target
 * Table patching did not take (see RE_DSI_AUDIO.md §11): the framework
 * snapshots the creator pointer into its own registry, so only patching the
 * CODE catches a cached pointer. */
static int install_trampoline(void* fn, void* target)
{
    long   ps   = sysconf(_SC_PAGESIZE);
    char  *page = (char*)((uintptr_t)fn & ~(uintptr_t)(ps - 1));
    size_t span = (size_t)(((char*)fn + 8) - page);
    uint32_t* p = (uint32_t*)fn;

    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[au] mprotect(%p) failed: %s\n", fn, strerror(errno));
        return -1;
    }
    p[0] = 0xE51FF004u;                      /* LDR PC, [PC, #-4] */
    p[1] = (uint32_t)(uintptr_t)target;
    /* Instruction cache must see the new bytes. QEMU/TCG notices self-modifying
     * code on its own, but real hardware does not. */
    msync(page, span, MS_INVALIDATE);
    fprintf(stderr, "[au] trampoline: %p -> %p\n", fn, target);
    fflush(stderr);
    return 0;
}

static void au_build_impl(void)
{
    int i;
    g_impl_vtbl[0]=(void*)au_slot_00; g_impl_vtbl[1]=(void*)au_slot_01; g_impl_vtbl[2]=(void*)au_slot_02; g_impl_vtbl[3]=(void*)au_slot_03; g_impl_vtbl[4]=(void*)au_slot_04; g_impl_vtbl[5]=(void*)au_slot_05; g_impl_vtbl[6]=(void*)au_slot_06; g_impl_vtbl[7]=(void*)au_slot_07; g_impl_vtbl[8]=(void*)au_slot_08; g_impl_vtbl[9]=(void*)au_slot_09; g_impl_vtbl[10]=(void*)au_slot_10; g_impl_vtbl[11]=(void*)au_slot_11; g_impl_vtbl[12]=(void*)au_slot_12; g_impl_vtbl[13]=(void*)au_slot_13; g_impl_vtbl[14]=(void*)au_slot_14; g_impl_vtbl[15]=(void*)au_slot_15;
    g_impl_obj[0] = (void*)g_impl_vtbl;
}

static void* g_reg = 0;

static int au_register(void)
{
    uint8_t ifUUID[SZ_IPL_UUID], tyUUID[SZ_IPL_UUID];
    /* 07302FA7-AE8E-5815-B570-33D89F0CFEAF = dsi.audio.DSIAudioManagement interface */
    ipl_UUID_ctor(ifUUID, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
                  0x33,0xD8,0x9F,0x0C,0xFE,0xAF);
    /* 9DEC172B-A004-5427-A4CC-39F18A58277D = its type UUID */
    ipl_UUID_ctor(tyUUID, 0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu,
                  0x39,0xF1,0x8A,0x58,0x27,0x7D);

    static uint8_t aof[SZ_DEFAULT_AOFACTORY];
    comm_DefaultAOFactory_ctor(aof);

    /* The proxy lib carries the STUB side (marshalling for ID 8 et al). Loading it
     * RTLD_GLOBAL is what makes the framework able to instantiate a stub for us. */
    /* One dlopen is enough: every name form resolves to the same table
     * (verified -- all four gave table=784a1a24, only the handles differed). */
    void* h = dlopen("/eso/lib/factories/lib_07302fa7-ae8e-5815-b570-33d89f0cfeaf.so",
                     RTLD_NOW | RTLD_GLOBAL);
    if (!h){ fprintf(stderr, "[au] dlopen: %s\n", dlerror()); return -2; }
    int n = 0;
    void* (*gf)(int*) = (void*(*)(int*))dlsym(h, "getProxyStubFactory");
    unsigned char* table = gf ? (unsigned char*)(intptr_t)gf(&n) : 0;
    unsigned char* libbase = (unsigned char*)gf - 0x2998;   /* phase 1 anchor */
    fprintf(stderr, "[au] factory table=%p recs=%d libbase=%p\n",
            (void*)table, n, (void*)libbase);

    au_build_impl();
    g_libbase = libbase;
    g_real_stub_ctor = (stub_ctor_t)(libbase + 0x43B4);     /* the real stub ctor */
    for (int i = 0; i < n && table; i++) {
        unsigned char* r  = table + i * 0xBC;
        const char*    nm = *(const char**)(r + 0x1C);
        void*          sc = *(void**)(r + 0x40);
        if (nm && !strcmp(nm, "dsi.audio.DSIAudioManagement") && sc) {
            fprintf(stderr, "[au] rec[%d] shipped stub_create=%p (libbase+0x2B8C=%p)\n",
                    i, sc, (void*)(libbase + 0x2B8C));
            /* mprotect on the .so's text is denied on QNX ("Permission denied"),
             * so the trampoline is out. Before trying anything heavier, dump the
             * WHOLE record: we have only ever patched words equal to +0x40's
             * value, so a creator stored under a different pointer would have
             * been invisible. Annotate every word that points into the lib. */
            for (int off = 0; off < 0xBC; off += 4) {
                uint32_t w = *(uint32_t*)(r + off);
                unsigned char* pw = (unsigned char*)(uintptr_t)w;
                if (pw > libbase && pw < libbase + 0x30000)
                    fprintf(stderr, "[au]   rec[%d]+0x%02x = %p  (libbase+0x%05x)\n",
                            i, off, (void*)pw, (unsigned)(pw - libbase));
            }
            /* keep the table patch too, harmless and already proven to install */
            for (int off = 0; off < 0xBC; off += 4)
                if (*(void**)(r + off) == sc) *(void**)(r + off) = (void*)au_stub_create_hook;
            fprintf(stderr, "[au] rec[%d] table entries repointed to hook %p\n",
                    i, (void*)au_stub_create_hook);
            break;
        }
    }

    /* providerImpl = NULL -> impl[19]=0 -> register uses the agent's DefaultAOFactory.
     * (Passing a bare dsi::ServiceProviderBase here gets mis-called as an AOFactory.) */
    void* L = au_make_listener();
    g_reg = build_audio_registration(ifUUID, tyUUID, aof, L, L, /*providerImpl=*/NULL);
    int rc = g_reg ? comm_ServiceRegistration_registerService(g_reg) : -1;
    fprintf(stderr, "[au] registerService -> %d (reg=%p)\n", rc, g_reg);
    fflush(stderr);
    return rc;
}

/* Poll isAlive + stub capture. rc==0 alone is NOT success — 2c got rc==0 without
 * ever publishing, so the loop reports isAlive and the connect callback instead. */
static void au_watch_loop(void)
{
    void* last = 0;
    int   lastAlive = -1;
    for (;;){
        int alive = g_reg ? comm_ServiceRegistration_isAlive(g_reg) : -1;
        if (alive != lastAlive){
            lastAlive = alive;
            fprintf(stderr, "[au] registration isAlive=%d\n", alive); fflush(stderr);
        }
        if ((void*)g_au_stub != last){
            last = (void*)g_au_stub;
            fprintf(stderr, "[au] stub-now=%p\n", last); fflush(stderr);
        }
        usleep(200000);
    }
}

static void* au_graft_thread(void*)
{
    fprintf(stderr, "[au] graft: waiting for host comm agent...\n"); fflush(stderr);
    for (int i = 0; i < 6000 && !comm_CoreApi_getInstance(); i++) usleep(100000);
    if (!comm_CoreApi_getInstance()){
        fprintf(stderr, "[au] graft: no CoreApi, giving up\n"); return NULL;
    }
    fprintf(stderr, "[au] graft: host agent UP -> registering DSIAudioManagement\n");
    au_register();
    au_watch_loop();
    return NULL;
}

extern "C" { extern char* __progname; }
#ifndef AU_HOST
#define AU_HOST "servicemgr"   /* normal agent (id 102): valid CoreApi +28.
                                * The BROKER's +28 is NULL -> never graft there. */
#endif
__attribute__((constructor)) static void au_graft_init(void)
{
    const char* pn = __progname ? __progname : "(null)";
    fprintf(stderr, "[au] ctor loaded in progname='%s'\n", pn); fflush(stderr);
    if (!strstr(pn, AU_HOST)) return;
    fprintf(stderr, "[au] graft ACTIVE in host '%s'\n", pn); fflush(stderr);
    pthread_t t; pthread_create(&t, NULL, au_graft_thread, NULL);
}
