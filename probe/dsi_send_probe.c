/* dsi_send_probe - phase 2f: send CALL_METHOD id 8 over the live DSI transport.
 *
 * Builds on phase 2e (dsi_alive_probe), which got the client proxy to ALIVE by
 * patching the factory record's missing client pc/pd with its own stubs. That
 * proved the lifecycle but left the proxy impl an empty heap block -- useless
 * for sending, because the generated sender reads its serialization stream out
 * of that impl.
 *
 * THE SEND PATH, fully named (libcomm + libdsiaudioproxy, IDA):
 *
 *   generated sender  sub_9740(impl, arg1, arg2):
 *       msg = { vptr, arg1, arg2, stream=impl+0x20, (u16)methodId@0x10,
 *               int@0x14 = impl+0x14, (u16)agentId@0x18, (u16)entityId@0x1A }
 *       stream->vslot[5]()                  // begin
 *       sub_6C38(msg):  stream->vslot[12](stream, arg) per argument   // +48 = write int32
 *                       stream[4] is the error flag
 *       size = stream->vslot[6]()           // finish -> byte count
 *       holder    = *(void**)(impl+8)
 *       transport = holder->vslot[6](holder)          // sub_9E840: return this+8
 *       send      = transport->vptr[2]                // sub_9F36C -> comm::RemoteConnection::send
 *       send(transport, size, msgSharedPtr, sid, methodId)
 *
 *   RemoteConnection::send logs its own parameter names:
 *       "on %d sending CALL_METHOD sid=%d, mid=%d to agent %d"
 *
 * WHERE THE STREAM COMES FROM -- and why we no longer hand-build one.
 * The shipped ctor sub_73F4 (reached via record 1's proxy_create, sub_2B30)
 * builds a 0x2C-byte impl:
 *       [0]=vptr  [4..C]=SharedPtr(transport)  (u16)[0x10]=sid  [0x14]=..  [0x18]=a5
 *       [0x1C] = holder->vslot[5](holder)
 *       [0x20] = [0x1C]->vslot[2]()            <- the stream sub_9740 uses
 *       [0x24] = mutex
 * That machinery is direction-agnostic: it wraps whatever transport and sid
 * completeProxy hands it. So instead of reconstructing a stream by hand, this
 * probe calls the SHIPPED creator with the exact arguments completeProxy gave
 * us and returns its object. The framework gets a real impl (so we still reach
 * ALIVE) and we get a real stream.
 *
 * We then send method id 8 (getActiveEntertainmentConnection, one int, the
 * read-only method chosen in phase 1) on it.
 *
 * HONEST LIMITATION: record 1 is the reply-direction proxy, so its vtable is
 * for the wrong direction. We do not call any of its methods -- we only reuse
 * the transport/stream/sid it assembled, and drive the send ourselves with the
 * method id we want. Still a probe technique; the shipping client needs a
 * generated proxy. What this closes is the last unknown, the wire format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#define FACTORY_PATH "/eso/lib/factories/lib_07302fa7-ae8e-5815-b570-33d89f0cfeaf.so"
#define IFACE_NAME   "dsi.audio.DSIAudioManagement"
#define REC_STRIDE   0xBC
#define OFF_NAME     0x1C
#define OFF_PROXY_C  0x38      /* what completeProxy reads for style 0 */
#define OFF_PROXY_D  0x3C
#define OFF_STUB_C   0x40      /* filled in the shipped lib -> identifies record 0 */

static int g_fail;

static void *sym(const char *n)
{
   void *p = dlsym(RTLD_DEFAULT, n);
   if (!p) { printf("   MISSING %s\n", n); g_fail++; }
   return p;
}

/* ---- fault-trapped reads (same technique as dsi_state_probe: this process's
 * mappings are scattered — stack ~0x000f_xxxx, heap ~0x0013_xxxx, libs
 * ~0x780x_xxxx — so an address-window test is useless; actually attempt the
 * read with SIGSEGV/SIGBUS trapped). ------------------------------------- */
static sigjmp_buf g_jmp;
static volatile int g_trapping;

static void fault_handler(int sig)
{
   (void)sig;
   if (g_trapping) siglongjmp(g_jmp, 1);
   _exit(2);
}

static int try_read(const void *p, uintptr_t *out)
{
   if (!p || ((uintptr_t)p & 3)) return 0;
   g_trapping = 1;
   if (sigsetjmp(g_jmp, 1) == 0) {
      *out = *(const volatile uintptr_t *)p;
      g_trapping = 0;
      return 1;
   }
   g_trapping = 0;
   return 0;
}

static int plausible(const void *p) { uintptr_t t; return try_read(p, &t); }

static void dump(const char *label, const void *base, int n)
{
   int i;
   printf("   %s @ %p:\n", label, base);
   if (!plausible(base)) { printf("      (unreadable)\n"); return; }
   for (i = 0; i < n; i++) {
      uintptr_t w;
      if (!try_read((const uintptr_t *)base + i, &w)) { printf("      +0x%02x (faulted)\n", i*4); break; }
      printf("      +0x%02x = %08lx%s\n", i * 4, (unsigned long)w,
             plausible((void *)w) ? "  <- readable" : "");
   }
   fflush(stdout);
}

static void dump_vtable(const char *label, const void *obj, int slots)
{
   int i;
   const uintptr_t *vt;
   if (!try_read(obj, (uintptr_t *)&vt)) return;
   if (!plausible(vt)) return;
   printf("   %s vtable @ %p:\n", label, (void *)vt);
   for (i = 0; i < slots; i++) {
      uintptr_t sl;
      if (!try_read(vt + i, &sl)) break;
      printf("      slot[%2d] (+0x%02x) = %08lx\n", i, i * 4, (unsigned long)sl);
   }
   fflush(stdout);
}

/* ---- what completeProxy will call ------------------------------------- */
static volatile void *g_transport;      /* captured: the send path needs this */
static volatile int   g_create_calls;
static void          *g_impl;           /* the SHIPPED impl, with a real stream */
static unsigned char *g_libbase;        /* libdsiaudioproxy load base */

/* record 1's proxy_create (sub_2B30): (sid*, transportSharedPtr, svcObj, a5) */
typedef void *(*shipped_create_t)(void *, void *, void *, uint32_t);
static shipped_create_t g_shipped_create;

static void *proxy_create(void *sid_ptr, void *transport, void *svc_name, uint32_t a4)
{
   void *impl;
   g_create_calls++;
   g_transport = transport;
   printf("\n   >>> proxy_create CALLED  sid@%p(=%u) transport=%p svcname=%p a4=0x%08x\n",
          sid_ptr, sid_ptr ? *(unsigned short *)sid_ptr : 0, transport, svc_name,
          (unsigned)a4);

   /* Hand the SAME arguments to the shipped creator so it assembles a real impl
    * (holder + stream + sid). Phase 2e returned a blank block here, which was
    * enough for ALIVE but has no stream to serialize into. */
   impl = g_shipped_create(sid_ptr, transport, svc_name, a4);
   g_impl = impl;
   printf("   >>> shipped create -> impl=%p\n", impl);
   dump("impl", impl, 11);
   fflush(stdout);
   return impl;
}

static void proxy_destroy(void *impl)
{
   printf("   >>> proxy_destroy(%p)\n", impl);
   fflush(stdout);
}

/* util::SharedPtrDefaultBase(void* ptr, void(*deleter)(void*,void*), void* arg) */
static void (*sp_ctor_raw)(void *, void *, void *, void *);
static void noop_deleter(void *a, void *b) { (void)a; (void)b; }

/* Our replacement for the generated per-method serializer (the contract is
 * sub_75BC, decompiled): write exactly the arguments this method has. */
static void* g_msg_vtbl[8];
static int   g_msg_argc;

static int msg_serialize(void* msg, void* writeable)
{
   unsigned char* stream = *(unsigned char**)((char*)msg + 12);
   void** svt = *(void***)stream;
   int k;
   (void)writeable;
   ((void (*)(void*))svt[3])(stream);                    /* begin */
   if (stream[4]) return 1;
   for (k = 0; k < g_msg_argc; k++) {
      ((void (*)(void*, uint32_t))svt[12])(stream, ((uint32_t*)msg)[1 + k]);
      if (stream[4]) return 1;
   }
   ((void (*)(void*))svt[4])(stream);                    /* end */
   return stream[4] != 0;
}

/* Replicate the tail of the generated sender sub_9740 for an arbitrary method
 * id and a single int argument. Every slot index below is from the decompile
 * quoted at the top of this file -- nothing here is a guessed offset. */
static int do_send(int method_id, int argc, uint32_t arg1, uint32_t arg2)
{
   unsigned char *impl = (unsigned char *)g_impl;
   void     *stream, *holder, *transport;
   void    **svt, **hvt, **tvt;
   uint32_t  msg[8];
   uint8_t   sp[12];
   uint16_t  sid, agent_id;
   int       size, rc, i;

   if (!impl) { printf("   no impl, cannot send\n"); return -1; }
   stream = *(void **)(impl + 0x20);
   holder = *(void **)(impl + 0x08);
   sid    = *(uint16_t *)(impl + 0x10);
   if (!stream || !holder) { printf("   impl has no stream/holder\n"); return -1; }

   svt = *(void ***)stream;
   hvt = *(void ***)holder;
   agent_id = (uint16_t)((int (*)(void *))hvt[4])(holder);

   printf("\n6) send: mid=%d argc=%d args=%u,%u sid=%u agent=%u stream=%p holder=%p\n",
          method_id, argc, (unsigned)arg1, (unsigned)arg2, sid, agent_id, stream, holder);
   fflush(stdout);

   memset(msg, 0, sizeof msg);
   /* The msg vtable is where the REAL serialization happens. slot[2]
    * (off_211E8 -> sub_75BC) is called by the framework when the connection
    * flushes, and it writes a FIXED number of arguments for the method it was
    * generated for:
    *      stream->vslot[3]()                     begin
    *      stream->vslot[12](stream, msg[+0x04])  arg1
    *      stream->vslot[12](stream, msg[+0x08])  arg2   <- always two
    *      stream->vslot[4]()                     end
    * Our manual writes are only the sizing pass; borrowing method 18's vtable
    * meant the flush emitted TWO ints while we had declared a 4-byte payload.
    * Framing mismatch, message silently dropped -- which is exactly what we saw.
    * So use our own vtable: a copy of theirs with slot[2] replaced by a
    * serializer that writes exactly g_msg_argc arguments. */
   g_msg_argc = argc;
   for (i = 0; i < 8; i++)
      g_msg_vtbl[i] = *(void **)(g_libbase + 0x211E8 + i * 4);
   g_msg_vtbl[2] = (void *)msg_serialize;
   msg[0] = (uint32_t)(uintptr_t)g_msg_vtbl;
   msg[1] = arg1;
   msg[2] = arg2;
   msg[3] = (uint32_t)(uintptr_t)stream;
   *(uint16_t *)((char *)msg + 0x10) = (uint16_t)method_id;
   msg[5] = *(uint32_t *)(impl + 0x14);
   *(uint16_t *)((char *)msg + 0x18) = agent_id;
   *(uint16_t *)((char *)msg + 0x1A) = 0xFFFF;    /* UNDEFINED_ENTITY_ID */

   ((void (*)(void *))svt[5])(stream);                    /* sizing pass: prepare */
   if (((unsigned char *)stream)[4]) { printf("   stream error after begin\n"); return -1; }
   for (i = 0; i < argc; i++) {                           /* +48: write int32 each */
      ((void (*)(void *, uint32_t))svt[12])(stream, i ? arg2 : arg1);
      if (((unsigned char *)stream)[4]) { printf("   stream error after arg %d\n", i); return -1; }
   }
   size = ((int (*)(void *))svt[6])(stream);              /* finish -> size */
   printf("   marshalled size=%d\n", size);
   if (size < 0) { printf("   bad size\n"); return -1; }
   fflush(stdout);

   transport = ((void *(*)(void *))hvt[6])(holder);       /* sub_9E840: this+8 */
   tvt = *(void ***)transport;
   printf("   transport=%p send=%p\n", transport, tvt[2]);
   fflush(stdout);

   sp_ctor_raw(sp, msg, (void *)noop_deleter, NULL);
   rc = ((int (*)(void *, int, void *, uint16_t, uint16_t))tvt[2])(
           transport, size, sp, sid, (uint16_t)method_id);
   printf("   RemoteConnection::send -> %d\n", rc);
   fflush(stdout);
   return rc;
}

/* The table lives in the .so's .data and should already be writable; make that
 * explicit rather than relying on it, so a read-only mapping fails loudly here
 * instead of as a SIGSEGV inside the patch. */
static int make_writable(void *addr, size_t len)
{
   long   ps   = sysconf(_SC_PAGESIZE);
   char  *base = (char *)((uintptr_t)addr & ~(uintptr_t)(ps - 1));
   size_t span = (size_t)(((char *)addr + len) - base);
   return mprotect(base, span, PROT_READ | PROT_WRITE);
}


static void (*osal_ctor)(void *, int, int);
static int  (*util_init)(const char *, int, int, int, int);
static void (*str_ctor)(void *, const char *, const void *);
static void (*as_ctor)(void *, const void *, const void *);
static int  (*as_start)(void *, void *, int);
static void (*uuid_ctor)(void *, uint32_t, uint32_t, uint32_t, uint16_t,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
static void (*proxy_ctor)(void *, const void *, int, void *);
static int  (*proxy_connect)(void *);

/* ---- reply service (phase 2g-2) -------------------------------------------
 * The provider's stub logs "created stub for pid=102, sid=5008, NO REPLY PROXY"
 * and no CALL_METHOD ever reaches its dispatcher. The stub ctor sub_43B4 builds
 * a comm::Proxy to service 55183b6f-9e82-5a18-bfde-ed5ae24f6c1c (static uuid
 * db006bf0-acd2-5747-8415-366ba32075fe) -- the reply direction. Nobody
 * registers it offline, so the stub has no way back to us.
 *
 * This is not merely diagnostic: replies to requestConnection
 * (RP_STARTCONNECTION / RP_PAUSECONNECTION / ...) arrive over exactly this
 * service, so a real RetroArch client has to register it regardless.
 *
 * Registration shape = the persistenceTNG one proven by the provider
 * (108-byte impl, LifecycleImpl vptr kept, interface UUID at +28, instance id 0
 * at [12], AOFactory at [14], refcount at [16], type UUID at +88, [13] -> it).
 * And rec[1]'s stub creator is 0 in the shipped lib (the client's stub is
 * generated into the consumer), so we plant one the same way we planted pc/pd.
 */
static void  (*lifecycle_ctor)(void*, int, void*);
static void  (*aof_ctor)(void*);
static int   (*reg_service)(void*);
static void  (*uuid_copy)(void*, const void*);
static void  *vt_servicereg, *vt_trackedref;

static void* g_reply_stub;
static void* g_reply_reg;      /* the reply ServiceRegistration handle */
/* comm::Proxy(IdentityArgs const&, ServiceRegistration const&, InterfaceStyle, LifecycleListener*) */
static void (*proxy_ctor4)(void*, const void*, const void*, int, void*);

/* ---- the reply stub, built the way the framework expects -------------------
 * Two of the shipped forward-stub's 7 vtable slots explain every crash so far
 * (vtable off_210A8, slots decompiled):
 *      slot[0] sub_344C:  stub[1] = arg      -- setImplementation
 *      slot[1] sub_5A68:                     -- process (the dispatcher)
 *      slot[6] sub_3A1C:  comm::Proxy(out, *(comm::Proxy*)(stub+8))
 *                                            -- getReplyProxy, BY VALUE
 * The framework called our stub's slot[0] (handing us the implementation we had
 * been planting by hand) and then slot[6]. slot[6] returns by value, so on ARM
 * r0 is the caller's output buffer -- which is why `self` looked like a stack
 * address with our stub as the first argument. It copy-constructs a Proxy out
 * of stub+8; our calloc'd object had garbage there, hence the SIGSEGV.
 *
 * So do not hand-build the object at all: call the SHIPPED stub ctor sub_43B4
 * (libbase + 0x43B4, the one behind rec[0]'s creator) which lays out the whole
 * 0x30 stub including the embedded comm::Proxy at +8, then swap only the vptr
 * for a copy of off_210A8 whose slot[1] is our own dispatcher. Everything
 * generic keeps the shipped implementation; only method dispatch is ours. */
typedef void* (*shipped_stub_ctor_t)(void*, void*, void*, void*, uint32_t);
static shipped_stub_ctor_t g_shipped_stub_ctor;      /* libbase + 0x43B4 */
static void*               reply_stub_vtbl[8];

/* Our dispatcher for the REPLY interface. The reply direction's own wire ids are
 * the ones the shipped PROXY_REPLY vtable sends (slot0 -> id 4, slot5 -> id 18,
 * 13 slots in all); this logs whatever turns up so the map can be filled in from
 * live traffic. */
static int reply_process(void* stub, int method_id, void* msg)
{
   printf("   >>> REPLY process: methodId=%d stub=%p msg=%p\n", method_id, stub, msg);
   fflush(stdout);
   return 0;
}

static void reply_stub_destroy(void* p){ printf("   >>> REPLY stub_destroy(%p)\n", p); fflush(stdout); }

static void* reply_stub_create(void* a1, void* a2, void* a3, uint32_t a4)
{
   void* stub;
   int i;
   if (!g_shipped_stub_ctor) { printf("   REPLY: no shipped ctor\n"); return NULL; }
   stub = calloc(1, 0x30);                       /* sub_2B8C: operator new(0x30) */
   g_shipped_stub_ctor(stub, a1, a2, a3, a4);    /* builds fields + Proxy at +8 */
   for (i = 0; i < 7; i++)                       /* off_210A8 has 7 live slots */
      reply_stub_vtbl[i] = *(void**)(g_libbase + 0x210A8 + i * 4);
   reply_stub_vtbl[1] = (void*)reply_process;    /* ours: the dispatcher */
   *(void**)stub = (void*)reply_stub_vtbl;
   /* sub_43B4 builds a comm::Proxy to 55183b6f at stub+8 -- correct for a
    * FORWARD stub, fatal for a reply one: Core::preCreateStub asserts
    *     "!stub->getReplyProxy().valid()"   (Core.cxx:1939)
    * because the reply direction is the end of the chain and carries no proxy
    * back. comm::Proxy is {vptr, m_storage, vptr2} and validity is m_storage,
    * so clear the word at stub+8+4 to make getReplyProxy() invalid. */
   *(void**)((char*)stub + 12) = NULL;
   g_reply_stub = stub;
   printf("   >>> REPLY stub_create -> %p (shipped ctor + our vtable %p)\n",
          stub, (void*)reply_stub_vtbl);
   fflush(stdout);
   return stub;
}

static int  reply_listener_noop(void){ return 0; }
static void* reply_listener_vtbl[48];
static void* reply_listener_obj[12];

static void* build_registration(void* ifUUID, void* tyUUID, void* aof, void* listener)
{
   uint8_t* impl = (uint8_t*)calloc(1, 0x6C);
   void**   handle;
   lifecycle_ctor(impl, 0, listener);          /* keeps the LifecycleImpl vptr */
   uuid_copy(impl + 28, ifUUID);
   ((uint32_t*)impl)[12] = 0;                  /* instance id 0 */
   ((void**)impl)[14]    = aof;
   ((uint32_t*)impl)[16] = 1;
   ((void**)impl)[18]    = listener;
   uuid_copy(impl + 88, tyUUID);
   ((void**)impl)[13]    = impl + 88;
   handle = (void**)calloc(1, 12);
   handle[0] = (char*)vt_servicereg + 8;
   handle[1] = impl;
   handle[2] = (char*)vt_trackedref + 8;
   (*(uint32_t*)(impl + 64))++;
   return handle;
}

static void register_reply_service(unsigned char* table, int n)
{
   uint8_t ifU[32], tyU[32];
   static uint8_t aof[32];
   void* reg;
   int i;

   lifecycle_ctor = (void(*)(void*,int,void*))
      sym("_ZN4comm13LifecycleImplC1ENS_9Lifecycle5StateEPNS_17LifecycleListenerE");
   aof_ctor    = (void(*)(void*)) sym("_ZN4comm26DefaultActiveObjectFactoryC1Ev");
   reg_service = (int(*)(void*))  sym("_ZN4comm19ServiceRegistration15registerServiceEv");
   uuid_copy   = (void(*)(void*,const void*)) sym("_ZN3ipl4UUIDC1ERKS0_");
   vt_servicereg = sym("_ZTVN4comm19ServiceRegistrationE");
   vt_trackedref = sym("_ZTVN4comm20TrackedReferenceBaseE");
   if (g_fail) { printf("   reply service: missing symbols, skipping\n"); return; }

   /* plant a stub creator on rec[1] (the 55183b6f record: proxy_create set,
    * stub_create 0 -- the mirror image of rec[0]) */
   for (i = 0; i < n; i++) {
      unsigned char* r  = table + i * REC_STRIDE;
      const char*    nm = *(const char**)(r + OFF_NAME);
      if (nm && !strcmp(nm, IFACE_NAME) && *(void**)(r + OFF_PROXY_C) && !*(void**)(r + OFF_STUB_C)) {
         if (make_writable(r + OFF_STUB_C, 8) != 0) { printf("   reply: mprotect failed\n"); return; }
         *(void**)(r + OFF_STUB_C)     = (void*)reply_stub_create;
         *(void**)(r + OFF_STUB_C + 4) = (void*)reply_stub_destroy;
         printf("   reply: rec[%d] stub creator planted\n", i);
         break;
      }
   }

   for (i = 0; i < 48; i++) reply_listener_vtbl[i] = (void*)reply_listener_noop;
   reply_listener_obj[0] = (void*)reply_listener_vtbl;

   memset(ifU, 0, sizeof ifU); memset(tyU, 0, sizeof tyU); memset(aof, 0, sizeof aof);
   /* 55183b6f-9e82-5a18-bfde-ed5ae24f6c1c / db006bf0-acd2-5747-8415-366ba32075fe
    * -- both read out of the stub ctor sub_43B4. */
   uuid_ctor(ifU, 0x55183B6Fu, 0x9E82u, 0x5A18u, 0xBFDEu, 0xED,0x5A,0xE2,0x4F,0x6C,0x1C);
   *(int*)(ifU + 20) = 0;
   uuid_ctor(tyU, 0xDB006BF0u, 0xACD2u, 0x5747u, 0x8415u, 0x36,0x6B,0xA3,0x20,0x75,0xFE);
   aof_ctor(aof);
   reg = build_registration(ifU, tyU, aof, (void*)reply_listener_obj);
   g_reply_reg = reg;
   printf("   reply service registerService -> %d\n", reg ? reg_service(reg) : -1);
   fflush(stdout);
}


static const char *LIBS[] = {
   "libiplcommon.so", "libosal.so", "libutil.so", "libcomm.so", "libdsicommon.so", NULL
};

int main(int argc, char **argv)
{
   const char *cli_name = (argc > 1) ? argv[1] : "RetroArch";
   const char *cli_node = (argc > 2) ? argv[2] : "local";
   const char **l;
   unsigned char osal_mem[8], sname[16], snode[16], agent[8];
   unsigned char inst[32], stat[24], proxy[32];
   const char *iface_names[2];
   const void *identity[3];
   void *fh, *table, *rec = NULL;
   int (*getfac)(int *);
   int n = 0, i;
   static const uint8_t A_tail[6] = {0x33,0xD8,0x9F,0x0C,0xFE,0xAF};
   static const uint8_t B_tail[6] = {0x39,0xF1,0x8A,0x58,0x27,0x7D};

   signal(SIGSEGV, fault_handler);
   signal(SIGBUS,  fault_handler);

   printf("=== dsi_alive_probe (phase 2e: supply pc/pd, reach ALIVE) ===\n\n");
   fflush(stdout);

   for (l = LIBS; *l; l++)
      if (!dlopen(*l, RTLD_NOW | RTLD_GLOBAL)) { printf("FATAL dlopen %s\n", *l); return 1; }

   /* Load the SAME file the framework auto-loads (the UUID-named copy). A
    * second dlopen of an already-loaded object returns the same handle, so we
    * patch the table the framework itself will consult — patching the
    * libdsiaudioproxy.so copy instead would patch a different mapping. */
   printf("1) factory\n");
   fh = dlopen(FACTORY_PATH, RTLD_NOW | RTLD_GLOBAL);
   if (!fh) { printf("   FATAL dlopen %s: %s\n", FACTORY_PATH, dlerror()); return 1; }
   getfac = (int (*)(int *))dlsym(fh, "getProxyStubFactory");
   if (!getfac) { printf("   FATAL no getProxyStubFactory\n"); return 1; }
   table = (void *)(intptr_t)getfac(&n);
   printf("   table=%p count=%d\n", table, n);

   for (i = 0; i < n; i++) {
      unsigned char *r    = (unsigned char *)table + i * REC_STRIDE;
      const char    *name = *(const char **)(r + OFF_NAME);
      void          *pc   = *(void **)(r + OFF_PROXY_C);
      void          *sc   = *(void **)(r + OFF_STUB_C);
      printf("   rec[%d] name=%-30s proxy_create=%p stub_create=%p\n",
             i, name ? name : "(null)", pc, sc);
      /* record 0 = the one for OUR service: right interface, stub creator
       * present, proxy creator missing. Matching on that shape rather than on a
       * hard-coded index keeps this honest if the table ever differs. */
      if (name && !strcmp(name, IFACE_NAME) && !pc && sc)
         rec = r;                                  /* stub record: needs our pc */
      if (name && !strcmp(name, IFACE_NAME) && pc && !sc)
         g_shipped_create = (shipped_create_t)pc;  /* reply record: has a real one */
   }
   if (!rec) { printf("   FATAL: no record with a missing proxy creator\n"); return 1; }
   if (!g_shipped_create) { printf("   FATAL: no shipped proxy creator to borrow\n"); return 1; }
   g_libbase = (unsigned char *)getfac - 0x2998;   /* phase 1: getProxyStubFactory @ +0x2998 */
   g_shipped_stub_ctor = (shipped_stub_ctor_t)(g_libbase + 0x43B4);
   printf("   libbase=%p shipped_create=%p\n", g_libbase, (void *)g_shipped_create);
   printf("   -> patching record @ %p\n", rec);

   if (make_writable((char *)rec + OFF_PROXY_C, 8) != 0) {
      printf("   FATAL mprotect: %s\n", strerror(errno));
      return 1;
   }
   *(void **)((char *)rec + OFF_PROXY_C) = (void *)proxy_create;
   *(void **)((char *)rec + OFF_PROXY_D) = (void *)proxy_destroy;
   printf("   pc=%p pd=%p installed\n", (void *)proxy_create, (void *)proxy_destroy);
   fflush(stdout);

   osal_ctor = (void (*)(void *, int, int))              sym("_ZN4osal4OsalC1Ebb");
   util_init = (int (*)(const char *, int, int, int, int))sym("_ZN4util4Util4initEPKcbbbb");
   str_ctor  = (void (*)(void *, const char *, const void *))
      sym("_ZN3ipl12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcRKS4_");
   as_ctor   = (void (*)(void *, const void *, const void *))
      sym("_ZN4comm12AgentStarterC1ERKN3ipl12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_");
   as_start  = (int (*)(void *, void *, int))
      sym("_ZN4comm12AgentStarter5startEPNS_25AgentStarterEventReceiverEb");
   uuid_ctor = (void (*)(void *, uint32_t, uint32_t, uint32_t, uint16_t,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))
      sym("_ZN3ipl4UUIDC1Ejjjthhhhhh");
   proxy_ctor = (void (*)(void *, const void *, int, void *))
      sym("_ZN4comm5ProxyC1ERKNS_11TrackedBase12IdentityArgsENS_14InterfaceStyleEPNS_17LifecycleListenerE");
   proxy_connect = (int (*)(void *)) sym("_ZN4comm5Proxy7connectEv");
   sp_ctor_raw = (void (*)(void *, void *, void *, void *))
      sym("_ZN4util20SharedPtrDefaultBaseC2EPvPFvS1_S1_ES1_");
   /* The 4-arg ctor is what fills the reply-service fields. hasReplyService()
    * (libcomm 0x1e888) reads them: `state && *(state+112) && UUID@(state+108)
    * != nil && *(*(state+112) + 72) != 0`. The 3-arg ctor we used through
    * phase 2f leaves both zero, which is why every CREATE_STUB carried
    * reply-sid 65535 and the provider stub was built "no reply proxy". */
   proxy_ctor4 = (void (*)(void *, const void *, const void *, int, void *))
      sym("_ZN4comm5ProxyC1ERKNS_11TrackedBase12IdentityArgsERKNS_19ServiceRegistrationENS_14InterfaceStyleEPNS_17LifecycleListenerE");
   if (g_fail) return 1;

   printf("\n2) bootstrap\n"); fflush(stdout);
   memset(osal_mem, 0, sizeof osal_mem); osal_ctor(osal_mem, 1, 0);
   util_init(cli_name, 1, 1, 1, 1);
   memset(sname, 0, sizeof sname); memset(snode, 0, sizeof snode);
   str_ctor(sname, cli_name, osal_mem); str_ctor(snode, cli_node, osal_mem);
   memset(agent, 0, sizeof agent); as_ctor(agent, sname, snode);
   printf("   AgentStarter::start -> %d\n", as_start(agent, NULL, 1));
   usleep(300 * 1000);

   printf("\n2c) register the reply service so the provider's stub can call back\n");
   fflush(stdout);
   register_reply_service((unsigned char *)table, n);
   usleep(500 * 1000);

   printf("\n3) proxy + connect\n"); fflush(stdout);
   iface_names[0] = IFACE_NAME;
   iface_names[1] = "dsi.audio.DSISound";
   memset(inst, 0, sizeof inst); memset(stat, 0, sizeof stat); memset(proxy, 0, sizeof proxy);
   uuid_ctor(inst, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
             A_tail[0], A_tail[1], A_tail[2], A_tail[3], A_tail[4], A_tail[5]);
   *(int *)(inst + 20) = 0;
   uuid_ctor(stat, 0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu,
             B_tail[0], B_tail[1], B_tail[2], B_tail[3], B_tail[4], B_tail[5]);
   identity[0] = inst; identity[1] = stat; identity[2] = iface_names;
   if (g_reply_reg && proxy_ctor4) {
      printf("   using the 4-arg ctor with our reply registration %p\n", g_reply_reg);
      proxy_ctor4(proxy, identity, g_reply_reg, 0, NULL);
   } else {
      printf("   falling back to the 3-arg ctor (no reply service)\n");
      proxy_ctor(proxy, identity, 0, NULL);
   }
   {  /* what hasReplyService() will look at */
      unsigned char *st = (unsigned char *)*(void **)(proxy + 4);
      printf("   state+108 uuid word=%08lx  state+112=%p\n",
             st ? (unsigned long)*(uint32_t *)(st + 108) : 0,
             st ? *(void **)(st + 112) : NULL);
   }
   printf("   connect() -> %d\n", proxy_connect(proxy)); fflush(stdout);

   printf("\n4) lifecycle poll (0=pending 1=alive 2=dead)\n");
   {
      volatile int *state = (volatile int *)((char *)*(void **)(proxy + 4) + 4);
      int last = -1;
      for (i = 0; i < 300; i++) {           /* up to 15s */
         int s = *state;
         if (s != last) {
            printf("   t=%5dms state=%d%s\n", i * 50, s,
                   s == 1 ? "   <<<<< ALIVE" : (s == 2 ? "   <<<<< DEAD" : ""));
            fflush(stdout);
            last = s;
            if (s == 1 || s == 2) break;
         }
         usleep(50 * 1000);
      }
      printf("\n=== RESULT: state=%d  proxy_create calls=%d  transport=%p ===\n",
             last, g_create_calls, (void *)g_transport);
      if (last != 1) {
         printf("not alive (state=%d) - not sending\n", last);
      } else {
         printf("proxy ALIVE; sending getActiveEntertainmentConnection (id 8)\n");
         /* A/B on our own machinery. id 18 is known to reach the provider's
          * dispatcher (the shipped sender produced "unknown method ID=18"), so
          * sending 18 through OUR path isolates the two possibilities: our
          * machinery vs something specific to id 8. Two ints, like the shipped
          * sender uses. */
         printf("\n-- A: our path, id 18 (known-good target, 2 ints) --\n");
         do_send(18, 2, 0, 0);
         usleep(500 * 1000);
         printf("\n-- B: our path, id 7 (1 int, impl slot 2) --\n");
         do_send(7, 1, 0, 0);
         usleep(500 * 1000);
         printf("\n-- C: our path, id 8 (the real one, 1 int, impl slot 3) --\n");
         do_send(8, 1, 0, 0);
         usleep(2000 * 1000);   /* let the provider side log its arrival */
      }
   }
   fflush(NULL);
   _exit(0);
}
