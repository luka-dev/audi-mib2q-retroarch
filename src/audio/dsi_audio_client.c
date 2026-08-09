/* dsi_audio_client.c — see dsi_audio_client.h.
 *
 * Implements the reverse-engineered MHI2Q DSI audio-focus client. Two things
 * about the shape of this file:
 *
 * 1. Everything binds at RUNTIME via dlopen/dlsym. The firmware's C++ libs have
 *    no .dynamic section, so ld cannot link against them at all; a mangled name
 *    encodes the full signature, so the name IS the contract.
 *
 * 2. The factory library ships only the SERVER halves of this interface -- a
 *    stub for the forward direction and a proxy for the reply direction. The
 *    client halves are code-generated into each consumer by the IDL compiler,
 *    which we do not have, so we supply them: a create/destroy pair for the
 *    client proxy on factory record 0, and one for the reply stub on record 1.
 *    Both are installed into the factory table the framework itself consults.
 *    That is not a hack around the framework; it is the same thing a generated
 *    client registers, just written by hand.
 */

#include "dsi_audio_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>

#define FACTORY_PATH  "/eso/lib/factories/lib_07302fa7-ae8e-5815-b570-33d89f0cfeaf.so"
#define IFACE_NAME    "dsi.audio.DSIAudioManagement"

/* Factory record layout recovered from the stock DSI implementation. */
#define REC_STRIDE    0xBC
#define OFF_NAME      0x1C
#define OFF_PROXY_C   0x38   /* client proxy create  -- empty in the shipped lib */
#define OFF_PROXY_D   0x3C
#define OFF_STUB_C    0x40   /* stub create/destroy  -- present on record 0 */
#define OFF_STUB_D    0x44

/* Offsets into libdsiaudioproxy, anchored on getProxyStubFactory @ +0x2998. */
#define LIB_ANCHOR    0x2998
#define OFF_STUB_CTOR 0x43B4    /* the real stub ctor, one call below sub_2B8C */
#define OFF_STUB_VTBL 0x210A8   /* forward stub vtable: 7 live slots           */
#define OFF_MSG_VTBL  0x211E8   /* a generated per-method message vtable       */
#define STUB_SIZE     0x30
#define STUB_VTBL_N   7

/* Proxy impl layout built by the shipped ctor (§10). */
#define IMPL_HOLDER   0x08
#define IMPL_SID      0x10
#define IMPL_MISC     0x14
#define IMPL_STREAM   0x20

#define LOG(...)      do { fprintf(stderr, "[dsi-audio] " __VA_ARGS__); \
                           fputc('\n', stderr); fflush(stderr); } while (0)

/* ------------------------------------------------------------------ *
 *  framework entry points, all resolved by mangled name
 * ------------------------------------------------------------------ */
static void  (*fw_osal_ctor)(void *, int, int);
static int   (*fw_util_init)(const char *, int, int, int, int);
static void  (*fw_str_ctor)(void *, const char *, const void *);
static void  (*fw_agent_ctor)(void *, const void *, const void *);
static int   (*fw_agent_start)(void *, void *, int);
static void  (*fw_uuid_ctor)(void *, uint32_t, uint32_t, uint32_t, uint16_t,
                             uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
static void  (*fw_uuid_copy)(void *, const void *);
static void  (*fw_proxy_ctor4)(void *, const void *, const void *, int, void *);
static int   (*fw_proxy_connect)(void *);
static void  (*fw_lifecycle_ctor)(void *, int, void *);
static void  (*fw_aof_ctor)(void *);
static int   (*fw_register_service)(void *);
static void  (*fw_sp_ctor_raw)(void *, void *, void *, void *);
static void  *fw_vt_servicereg;
static void  *fw_vt_trackedref;

static int    g_missing;

static void *sym(const char *name)
{
   void *p = dlsym(RTLD_DEFAULT, name);
   if (!p) { LOG("missing symbol %s", name); g_missing++; }
   return p;
}

static const char *FRAMEWORK_LIBS[] = {
   "libiplcommon.so", "libosal.so", "libutil.so",
   "libcomm.so", "libdsicommon.so", NULL
};

/* ------------------------------------------------------------------ *
 *  module state
 * ------------------------------------------------------------------ */
static volatile int        g_ready;
static unsigned char      *g_libbase;
static unsigned char      *g_rec_forward;   /* record 0: 07302fa7 */
static unsigned char      *g_rec_reply;     /* record 1: 55183b6f */

static void               *g_proxy_impl;    /* our client proxy impl  */
static unsigned char       g_proxy[32];     /* the comm::Proxy itself */
static void               *g_reply_reg;     /* reply ServiceRegistration handle */

/* Written by the framework reply thread and read by the main/audio threads.
 * Aligned 32-bit loads/stores are atomic on this target; volatile prevents the
 * compiler from caching them across those independent execution contexts. */
static volatile int        g_focus;          /* do we hold the connection?   */
static volatile int        g_focus_lost_edge; /* consumed by the frontend     */
static volatile int        g_want_connection = -1;
static volatile int        g_want_terminal;
static volatile int        g_want_group;
static volatile unsigned   g_retry_countdown;
static dsi_audio_reply_cb  g_cb;
static void               *g_cb_user;

/* the shipped creators we delegate to */
static void *(*g_shipped_proxy_create)(void *, void *, void *, uint32_t);
static void *(*g_shipped_stub_ctor)(void *, void *, void *, void *, uint32_t);

/* ------------------------------------------------------------------ *
 *  writable-data helper
 * ------------------------------------------------------------------ */
static int make_writable(void *addr, size_t len)
{
   long   ps   = sysconf(_SC_PAGESIZE);
   char  *base = (char *)((uintptr_t)addr & ~(uintptr_t)(ps - 1));
   size_t span = (size_t)(((char *)addr + len) - base);
   /* The factory table lives in the library's .data and is already writable;
    * doing this explicitly makes a read-only mapping fail here, loudly, rather
    * than as a SIGSEGV in the middle of the patch. */
   return mprotect(base, span, PROT_READ | PROT_WRITE);
}

/* ================================================================== *
 *  the reply stub  (our half of the reply direction)
 * ================================================================== */
static void *g_reply_stub_vtbl[STUB_VTBL_N + 1];
static void *g_reply_impl_vtbl[16];
static void *g_reply_impl_obj[4];

/* Our dispatcher. The framework hands us (stub, wire method id, message); the
 * arguments are read out of the deserializer at stub+0x24 exactly as the shipped
 * dispatcher does -- its vtable slot 11 (+0x2c) reads one int. */
static int reply_process(void *stub, int method_id, void *msg)
{
   unsigned char *deser = (unsigned char *)((void **)stub)[9];   /* stub+0x24 */
   int a = 0, b = 0, c = 0;

   (void)msg;
   if (deser) {
      void **dvt = *(void ***)deser;
      ((void (*)(void *, int *))dvt[11])(deser, &a);
      if (!deser[4])
         ((void (*)(void *, int *))dvt[11])(deser, &b);
      if (!deser[4] && (method_id == DSI_REPLY_ERROR_CONNECTION
                       || method_id == DSI_REPLY_RESPONSE_VOLUMELOCK
                       || method_id == DSI_REPLY_UPDATE_AM_AVAILABLE
                       || method_id == DSI_REPLY_UPDATE_ACTIVE_CONN
                       || method_id == DSI_REPLY_UPDATE_ACTIVE_ENT_CONN))
         ((void (*)(void *, int *))dvt[11])(deser, &c);
   }
   /* The connection state is the METHOD, not a value inside it. An earlier
    * version guessed the opposite -- scanning the arguments for 2000..2004 --
    * because those RP_* numbers appear on the Java side; they are reply-type
    * constants there, never payload. The real map came out of the framework's
    * own table (ems_tables/dsi.audio.ems_table.json) and agrees exactly with
    * the ids extracted from the shipped reply senders. See §16. */
   /* a = connection, b = terminal for every connection-scoped reply. The
    * manager serves every client, so anything about a connection that is not
    * ours must be ignored -- otherwise CarPlay being paused would pause us. */
   if (g_want_connection >= 0 && a != g_want_connection
       && method_id != DSI_REPLY_UPDATE_ACTIVE_ENT_CONN)
   {
      LOG("reply id=%d for connection %d (not ours, %d) - ignored",
          method_id, a, g_want_connection);
      return 0;
   }

   switch (method_id)
   {
      case DSI_REPLY_START_CONNECTION:      /* we may play          */
      case DSI_REPLY_FADED_IN:              /* fade-in completed    */
         if (!g_focus)
            LOG("focus granted (%s)",
                method_id == DSI_REPLY_START_CONNECTION ? "startConnection"
                                                        : "fadedIn");
         g_focus           = 1;
         g_retry_countdown = 0;
         break;

      case DSI_REPLY_PAUSE_CONNECTION:      /* someone needs the amp */
      case DSI_REPLY_STOP_CONNECTION:       /* connection torn down  */
      case DSI_REPLY_ERROR_CONNECTION:
         if (g_focus)
         {
            LOG("focus taken by another source (%s)",
                method_id == DSI_REPLY_PAUSE_CONNECTION ? "pauseConnection" :
                method_id == DSI_REPLY_STOP_CONNECTION  ? "stopConnection"
                                                        : "errorConnection");
            g_focus_lost_edge = 1;          /* the frontend pauses on this */
         }
         g_focus = 0;
         /* Try to get it back, but slowly: whoever took it is entitled to keep
          * it, and hammering the arbiter helps nobody. */
         g_retry_countdown = 300;           /* ~5 s at 60 Hz */
         break;

      case DSI_REPLY_UPDATE_ACTIVE_ENT_CONN:
         /* Broadcast: who currently owns the entertainment connection. This is
          * how we learn the other source let go, without polling for it. */
         if (c && a == g_want_connection)
         {
            if (!g_focus)
               LOG("entertainment connection is ours again");
            g_focus           = 1;
            g_retry_countdown = 0;
         }
         else if (g_focus)
         {
            LOG("entertainment connection changed to %d (valid=%d)", a, c);
            g_focus_lost_edge = 1;
            g_focus           = 0;
            g_retry_countdown = 300;
         }
         break;

      default:
         break;
   }

   LOG("reply id=%d a=%d b=%d c=%d", method_id, a, b, c);
   if (g_cb)
      g_cb(method_id, a, b, g_cb_user);
   return 0;
}

/* setImplementation. The framework calls this (stub vtable slot 0) with the
 * object its ActiveObjectFactory produced, which would otherwise replace ours
 * and silently swallow every dispatch -- the single most expensive thing to
 * discover in this whole reconstruction. Ignore what it
 * offers and keep ours. */
static void *reply_set_implementation(void *stub, void *framework_impl)
{
   (void)framework_impl;
   ((void **)stub)[1] = (void *)g_reply_impl_obj;
   return stub;
}

static int reply_impl_noop(void *self, int a, int b, int c)
{
   (void)self; (void)a; (void)b; (void)c;
   return 0;
}

static void *reply_stub_create(void *a1, void *a2, void *a3, uint32_t a4)
{
   void *stub;
   int   i;

   stub = calloc(1, STUB_SIZE);
   if (!stub)
      return NULL;

   /* Let the shipped ctor lay the object out: it builds the SharedPtr to the
    * transport, the sid, the deserializer -- and an embedded comm::Proxy. */
   g_shipped_stub_ctor(stub, a1, a2, a3, a4);

   for (i = 0; i < STUB_VTBL_N; i++)
      g_reply_stub_vtbl[i] = *(void **)(g_libbase + OFF_STUB_VTBL + i * 4);
   g_reply_stub_vtbl[0] = (void *)reply_set_implementation;
   g_reply_stub_vtbl[1] = (void *)reply_process;
   *(void **)stub = (void *)g_reply_stub_vtbl;

   /* A reply stub carries no proxy back: Core::preCreateStub asserts
    * "!stub->getReplyProxy().valid()". The shipped ctor built one (correct for
    * a forward stub), so invalidate it -- comm::Proxy is {vptr, m_storage,
    * vptr2} and validity is m_storage. */
   *(void **)((char *)stub + 12) = NULL;

   ((void **)stub)[1] = (void *)g_reply_impl_obj;
   LOG("reply stub %p ready", stub);
   return stub;
}

static void reply_stub_destroy(void *stub) { free(stub); }

static void build_reply_impl(void)
{
   int i;
   for (i = 0; i < 16; i++)
      g_reply_impl_vtbl[i] = (void *)reply_impl_noop;
   g_reply_impl_obj[0] = (void *)g_reply_impl_vtbl;
}

/* ================================================================== *
 *  the client proxy  (our half of the forward direction)
 * ================================================================== */

/* Core::completeProxy calls this as pc(&sid, transport, svcName, state[14]) and
 * stores what we return as the proxy implementation. Delegate to the shipped
 * ctor reached through record 1's creator: it assembles the holder, the sid and
 * -- the part we cannot reasonably rebuild -- the serialization stream bound to
 * this connection. */
static void *proxy_create(void *sid_ptr, void *transport, void *svc_name, uint32_t a4)
{
   g_proxy_impl = g_shipped_proxy_create(sid_ptr, transport, svc_name, a4);
   LOG("proxy impl %p (sid=%u)", g_proxy_impl,
       g_proxy_impl ? *(unsigned short *)((char *)g_proxy_impl + IMPL_SID) : 0u);
   return g_proxy_impl;
}

static void proxy_destroy(void *impl) { (void)impl; g_proxy_impl = NULL; }

/* ------------------------------------------------------------------ *
 *  sending
 * ------------------------------------------------------------------ */
/* The framework serializes the payload by calling msg->vtable[2] when the
 * connection flushes -- the sizing pass below only measures. Two things the
 * generated serializers encode that we must match exactly:
 *
 *  - the argument COUNT is fixed per method, so borrowing another method's
 *    vtable emits the wrong number of ints and the message is dropped silently;
 *  - the message LAYOUT shifts with that count. The stream pointer sits right
 *    after the arguments: 2-arg senders keep it at +0x0C, 3-arg senders at
 *    +0x10. Everything after it follows along.
 *
 *      msg[0]            serializer vtable
 *      msg[1 .. argc]    the arguments
 *      +4+argc*4         stream
 *      +8+argc*4         u16 methodId
 *      +12+argc*4        int   (copied from the proxy impl)
 *      +16+argc*4        u16 agentId, u16 entityId
 */
#define MSG_STREAM_OFF(argc)  (4 + (argc) * 4)

static void *g_msg_vtbl[8];
static int   g_msg_argc;

static int msg_serialize(void *msg, void *writeable)
{
   unsigned char *stream = *(unsigned char **)((char *)msg + MSG_STREAM_OFF(g_msg_argc));
   void **svt = *(void ***)stream;
   int k;

   (void)writeable;
   ((void (*)(void *))svt[3])(stream);                  /* begin */
   if (stream[4])
      return 1;
   for (k = 0; k < g_msg_argc; k++) {
      ((void (*)(void *, uint32_t))svt[12])(stream, ((uint32_t *)msg)[1 + k]);
      if (stream[4])
         return 1;
   }
   ((void (*)(void *))svt[4])(stream);                  /* end */
   return stream[4] != 0;
}

static int dsi_send(int method_id, int argc, uint32_t a1, uint32_t a2, uint32_t a3)
{
   unsigned char *impl = (unsigned char *)g_proxy_impl;
   void     *stream, *holder, *transport;
   void    **svt, **hvt, **tvt;
   uint32_t  msg[10];
   uint8_t   sp[12];
   uint16_t  sid;
   int       size, i;

   if (!g_ready || !impl) {
      LOG("send(%d): not connected", method_id);
      return -1;
   }
   stream = *(void **)(impl + IMPL_STREAM);
   holder = *(void **)(impl + IMPL_HOLDER);
   sid    = *(uint16_t *)(impl + IMPL_SID);
   if (!stream || !holder)
      return -1;

   svt = *(void ***)stream;
   hvt = *(void ***)holder;

   memset(msg, 0, sizeof msg);
   for (i = 0; i < 8; i++)
      g_msg_vtbl[i] = *(void **)(g_libbase + OFF_MSG_VTBL + i * 4);
   g_msg_vtbl[2] = (void *)msg_serialize;
   g_msg_argc    = argc;

   {
      char *m  = (char *)msg;
      int   so = MSG_STREAM_OFF(argc);
      msg[0] = (uint32_t)(uintptr_t)g_msg_vtbl;
      msg[1] = a1;
      msg[2] = a2;
      msg[3] = a3;
      *(void **)(m + so)            = stream;
      *(uint16_t *)(m + so + 4)     = (uint16_t)method_id;
      *(uint32_t *)(m + so + 8)     = *(uint32_t *)(impl + IMPL_MISC);
      *(uint16_t *)(m + so + 12)    = (uint16_t)((int (*)(void *))hvt[4])(holder);
      *(uint16_t *)(m + so + 14)    = 0xFFFF;           /* UNDEFINED_ENTITY_ID */
   }

   /* Sizing pass, mirroring the generated sender: prepare, write each argument,
    * ask for the byte count. */
   ((void (*)(void *))svt[5])(stream);
   if (((unsigned char *)stream)[4])
      return -1;
   for (i = 0; i < argc; i++) {
      uint32_t v = (i == 0) ? a1 : (i == 1) ? a2 : a3;
      ((void (*)(void *, uint32_t))svt[12])(stream, v);
      if (((unsigned char *)stream)[4])
         return -1;
   }
   size = ((int (*)(void *))svt[6])(stream);
   if (size < 0)
      return -1;

   transport = ((void *(*)(void *))hvt[6])(holder);     /* == holder + 8 */
   tvt = *(void ***)transport;
   fw_sp_ctor_raw(sp, msg, (void *)proxy_destroy, NULL);

   return ((int (*)(void *, int, void *, uint16_t, uint16_t))tvt[2])(
             transport, size, sp, sid, (uint16_t)method_id);
}

/* ================================================================== *
 *  registration
 * ================================================================== */
static void *build_registration(void *if_uuid, void *type_uuid,
                                void *aof, void *listener)
{
   uint8_t *impl = (uint8_t *)calloc(1, 0x6C);
   void   **handle;

   if (!impl)
      return NULL;
   /* Keep the LifecycleImpl vptr: isAlive/isDead on the handle dispatch through
    * it, and the ServiceRegistration vtable would recurse into itself. */
   fw_lifecycle_ctor(impl, 0, listener);
   fw_uuid_copy(impl + 28, if_uuid);
   ((uint32_t *)impl)[12] = 0;          /* instance id 0: what clients look up */
   ((void **)impl)[14]    = aof;
   ((uint32_t *)impl)[16] = 1;          /* refcount */
   ((void **)impl)[18]    = listener;
   fw_uuid_copy(impl + 88, type_uuid);
   ((void **)impl)[13]    = impl + 88;

   handle = (void **)calloc(1, 12);
   if (!handle) { free(impl); return NULL; }
   handle[0] = (char *)fw_vt_servicereg + 8;
   handle[1] = impl;
   handle[2] = (char *)fw_vt_trackedref + 8;
   (*(uint32_t *)(impl + 64))++;
   return handle;
}

static int   listener_noop(void) { return 0; }
static void *g_listener_vtbl[48];
static void *g_listener_obj[12];

/* ================================================================== *
 *  bring-up
 * ================================================================== */
static int locate_factory_records(void)
{
   void *h = dlopen(FACTORY_PATH, RTLD_NOW | RTLD_GLOBAL);
   int (*getfac)(int *);
   unsigned char *table;
   int n = 0, i;

   if (!h) { LOG("dlopen %s: %s", FACTORY_PATH, dlerror()); return -1; }
   getfac = (int (*)(int *))dlsym(h, "getProxyStubFactory");
   if (!getfac) { LOG("no getProxyStubFactory"); return -1; }

   table    = (unsigned char *)(intptr_t)getfac(&n);
   g_libbase = (unsigned char *)getfac - LIB_ANCHOR;
   if (!table || n <= 0) { LOG("empty factory table"); return -1; }

   for (i = 0; i < n; i++) {
      unsigned char *r  = table + i * REC_STRIDE;
      const char    *nm = *(const char **)(r + OFF_NAME);
      void          *pc = *(void **)(r + OFF_PROXY_C);
      void          *sc = *(void **)(r + OFF_STUB_C);

      if (!nm || strcmp(nm, IFACE_NAME))
         continue;
      /* The forward record ships a stub and no client proxy; the reply record
       * is its mirror. Matching on that shape rather than an index keeps this
       * honest if the table ever changes. */
      if (sc && !pc) g_rec_forward = r;
      if (pc && !sc) g_rec_reply   = r;
   }
   if (!g_rec_forward || !g_rec_reply) {
      LOG("factory records not found (forward=%p reply=%p)",
          (void *)g_rec_forward, (void *)g_rec_reply);
      return -1;
   }

   g_shipped_proxy_create = (void *(*)(void *, void *, void *, uint32_t))
                            *(void **)(g_rec_reply + OFF_PROXY_C);
   g_shipped_stub_ctor    = (void *(*)(void *, void *, void *, void *, uint32_t))
                            (g_libbase + OFF_STUB_CTOR);

   if (make_writable(g_rec_forward + OFF_PROXY_C, 8) != 0 ||
       make_writable(g_rec_reply   + OFF_STUB_C,  8) != 0) {
      LOG("factory table not writable: %s", strerror(errno));
      return -1;
   }
   *(void **)(g_rec_forward + OFF_PROXY_C) = (void *)proxy_create;
   *(void **)(g_rec_forward + OFF_PROXY_D) = (void *)proxy_destroy;
   *(void **)(g_rec_reply   + OFF_STUB_C)  = (void *)reply_stub_create;
   *(void **)(g_rec_reply   + OFF_STUB_D)  = (void *)reply_stub_destroy;
   LOG("factory halves installed (libbase=%p)", (void *)g_libbase);
   return 0;
}

static int resolve_symbols(void)
{
   const char **l;
   for (l = FRAMEWORK_LIBS; *l; l++)
      if (!dlopen(*l, RTLD_NOW | RTLD_GLOBAL)) {
         LOG("dlopen %s: %s", *l, dlerror());
         return -1;
      }

   fw_osal_ctor  = (void (*)(void *, int, int))              sym("_ZN4osal4OsalC1Ebb");
   fw_util_init  = (int (*)(const char *, int, int, int, int))sym("_ZN4util4Util4initEPKcbbbb");
   fw_str_ctor   = (void (*)(void *, const char *, const void *))
      sym("_ZN3ipl12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcRKS4_");
   fw_agent_ctor = (void (*)(void *, const void *, const void *))
      sym("_ZN4comm12AgentStarterC1ERKN3ipl12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_");
   fw_agent_start = (int (*)(void *, void *, int))
      sym("_ZN4comm12AgentStarter5startEPNS_25AgentStarterEventReceiverEb");
   fw_uuid_ctor  = (void (*)(void *, uint32_t, uint32_t, uint32_t, uint16_t,
                             uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))
      sym("_ZN3ipl4UUIDC1Ejjjthhhhhh");
   fw_uuid_copy  = (void (*)(void *, const void *)) sym("_ZN3ipl4UUIDC1ERKS0_");
   fw_proxy_ctor4 = (void (*)(void *, const void *, const void *, int, void *))
      sym("_ZN4comm5ProxyC1ERKNS_11TrackedBase12IdentityArgsERKNS_19ServiceRegistrationENS_14InterfaceStyleEPNS_17LifecycleListenerE");
   fw_proxy_connect  = (int (*)(void *)) sym("_ZN4comm5Proxy7connectEv");
   fw_lifecycle_ctor = (void (*)(void *, int, void *))
      sym("_ZN4comm13LifecycleImplC1ENS_9Lifecycle5StateEPNS_17LifecycleListenerE");
   fw_aof_ctor        = (void (*)(void *)) sym("_ZN4comm26DefaultActiveObjectFactoryC1Ev");
   fw_register_service = (int (*)(void *)) sym("_ZN4comm19ServiceRegistration15registerServiceEv");
   fw_sp_ctor_raw     = (void (*)(void *, void *, void *, void *))
      sym("_ZN4util20SharedPtrDefaultBaseC2EPvPFvS1_S1_ES1_");
   fw_vt_servicereg   = sym("_ZTVN4comm19ServiceRegistrationE");
   fw_vt_trackedref   = sym("_ZTVN4comm20TrackedReferenceBaseE");

   return g_missing ? -1 : 0;
}

/* Util::init aborts the process when the agent name is absent from the active
 * framework registry. Check first so a package installed without the matching
 * config fragment degrades to diagnostic PCM instead of killing RetroArch. */
static int framework_has_agent(const char *path, const char *agent_name)
{
   FILE *fp;
   char line[1024];
   char quoted[128];

   if (!path || !*path || !agent_name || !*agent_name)
      return 0;
   fp = fopen(path, "r");
   if (!fp)
      return 0;
   snprintf(quoted, sizeof(quoted), "\"%s\"", agent_name);
   while (fgets(line, sizeof(line), fp))
   {
      if (strstr(line, "\"name\"") && strstr(line, quoted))
      {
         fclose(fp);
         return 1;
      }
   }
   fclose(fp);
   return 0;
}

static int framework_agent_registered(const char *agent_name)
{
   const char *config_dir = getenv("IPL_CONFIG_DIR");
   char path[512];

   if (config_dir && *config_dir)
   {
      snprintf(path, sizeof(path), "%s/framework.json", config_dir);
      if (framework_has_agent(path, agent_name))
         return 1;
   }
   return framework_has_agent("/config/framework.json", agent_name)
       || framework_has_agent("/etc/eso/production/framework.json", agent_name);
}

int dsi_audio_init(const char *agent_name, dsi_audio_reply_cb cb, void *user)
{
   unsigned char osal_mem[8], sname[16], snode[16], agent[8];
   unsigned char if_u[32], ty_u[32];
   static unsigned char aof[32];
   const char   *iface_names[2];
   const void   *identity[3];
   volatile int *state;
   int i;

   if (g_ready)
      return 0;
   if (!framework_agent_registered(agent_name))
   {
      LOG("agent '%s' is not registered in framework.json; DSI disabled safely",
          agent_name);
      return -1;
   }
   g_cb      = cb;
   g_cb_user = user;

   if (resolve_symbols() != 0)
      return -1;

   LOG("bootstrap as '%s'", agent_name);
   memset(osal_mem, 0, sizeof osal_mem);
   fw_osal_ctor(osal_mem, 1, 0);
   fw_util_init(agent_name, 1, 1, 1, 1);
   memset(sname, 0, sizeof sname);
   memset(snode, 0, sizeof snode);
   fw_str_ctor(sname, agent_name, osal_mem);
   fw_str_ctor(snode, "local", osal_mem);
   memset(agent, 0, sizeof agent);
   fw_agent_ctor(agent, sname, snode);
   if (!fw_agent_start(agent, NULL, 1)) {
      LOG("AgentStarter::start failed -- is '%s' in /config/framework.json?",
          agent_name);
      return -1;
   }
   usleep(300 * 1000);

   if (locate_factory_records() != 0)
      return -1;
   build_reply_impl();

   /* Register the reply service. The audio manager's stub opens a proxy to it,
    * and without it the connection carries reply-sid 65535 and no replies can
    * ever come back. */
   for (i = 0; i < 48; i++)
      g_listener_vtbl[i] = (void *)listener_noop;
   g_listener_obj[0] = (void *)g_listener_vtbl;

   memset(if_u, 0, sizeof if_u);
   memset(ty_u, 0, sizeof ty_u);
   memset(aof,  0, sizeof aof);
   fw_uuid_ctor(if_u, 0x55183B6Fu, 0x9E82u, 0x5A18u, 0xBFDEu,
                0xED, 0x5A, 0xE2, 0x4F, 0x6C, 0x1C);
   *(int *)(if_u + 20) = 0;
   fw_uuid_ctor(ty_u, 0xDB006BF0u, 0xACD2u, 0x5747u, 0x8415u,
                0x36, 0x6B, 0xA3, 0x20, 0x75, 0xFE);
   fw_aof_ctor(aof);
   g_reply_reg = build_registration(if_u, ty_u, aof, (void *)g_listener_obj);
   if (!g_reply_reg || fw_register_service(g_reply_reg) != 0) {
      LOG("reply service registration failed");
      return -1;
   }
   usleep(300 * 1000);

   /* Now the forward proxy. The FOUR-argument ctor is required: it is what
    * records the reply service on the proxy, and the three-argument one binds
    * but can never receive a reply. */
   iface_names[0] = IFACE_NAME;
   iface_names[1] = "dsi.audio.DSISound";
   memset(if_u, 0, sizeof if_u);
   memset(ty_u, 0, sizeof ty_u);
   fw_uuid_ctor(if_u, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
                0x33, 0xD8, 0x9F, 0x0C, 0xFE, 0xAF);
   *(int *)(if_u + 20) = 0;
   fw_uuid_ctor(ty_u, 0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu,
                0x39, 0xF1, 0x8A, 0x58, 0x27, 0x7D);
   identity[0] = if_u;
   identity[1] = ty_u;
   identity[2] = iface_names;

   memset(g_proxy, 0, sizeof g_proxy);
   fw_proxy_ctor4(g_proxy, identity, g_reply_reg, 0, NULL);
   if (fw_proxy_connect(g_proxy) != 0) {
      LOG("connect failed");
      return -1;
   }

   /* connect() is asynchronous: it starts a broker lookup and a stub-creation
    * round trip. 0 = pending, 1 = alive, 2 = dead. */
   state = (volatile int *)((char *)*(void **)(g_proxy + 4) + 4);
   for (i = 0; i < 200 && *state == 0; i++)
      usleep(50 * 1000);
   if (*state != 1) {
      LOG("proxy did not go alive (state=%d) -- no audio provider on this unit?",
          *state);
      return -1;
   }

   g_ready = 1;
   LOG("connected");
   return 0;
}

int dsi_audio_request_connection(int connection, int terminal, int group)
{
   int rc;

   /* Publish the desired connection before sending. A fast provider may reply
    * on the framework thread as soon as the transport accepts the message. */
   g_want_connection = connection;
   g_want_terminal   = terminal;
   g_want_group      = group;
   rc = dsi_send(12, 3, (uint32_t)connection, (uint32_t)terminal,
                 (uint32_t)group);
   if (rc == 0)
   {
      /* Optimistic after the request was accepted by the transport. The amp is
       * still arbitrated by the manager; an error/pause/stop reply closes this
       * gate before subsequent PCM writes. This also remains compatible with
       * provider versions that route successfully but omit startConnection. */
      g_focus = 1;
      g_retry_countdown = 0;
   }
   else
   {
      g_focus = 0;
      g_retry_countdown = 300;
   }
   return rc;
}

int dsi_audio_has_focus(void) { return g_ready && g_focus; }

int dsi_audio_take_focus_lost_event(void)
{
   return __sync_lock_test_and_set(&g_focus_lost_edge, 0);
}

void dsi_audio_poll(void)
{
   if (!g_ready || g_focus || g_want_connection < 0)
      return;
   if (g_retry_countdown && --g_retry_countdown)
      return;
   LOG("re-requesting the connection");
   if (dsi_audio_request_connection(g_want_connection, g_want_terminal,
                                    g_want_group) != 0)
      g_retry_countdown = 300;
}

int dsi_audio_release_connection(int a, int b)
{
   int rc = dsi_send(11, 2, (uint32_t)a, (uint32_t)b, 0);
   g_focus           = 0;
   g_want_connection = -1;
   g_retry_countdown = 0;
   return rc;
}

int dsi_audio_get_active_entertainment(int arg)
{
   return dsi_send(8, 1, (uint32_t)arg, 0, 0);
}

int dsi_audio_is_alive(void) { return g_ready; }

void dsi_audio_shutdown(void)
{
   /* Leave the agent up: RetroArch exits right after, and tearing the mesh down
    * by hand is more ways to crash than it is worth. */
   g_ready = 0;
   g_focus = 0;
   g_focus_lost_edge = 0;
   g_want_connection = -1;
   g_retry_countdown = 0;
   g_cb    = NULL;
}
