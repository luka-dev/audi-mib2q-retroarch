/* dsi_alive_probe — phase 2e: get the client proxy to ALIVE.
 *
 * WHY THE PREVIOUS RUN ASSERTED
 * -----------------------------
 * Phase 2d put a real provider on the mesh (service AVAILABLE, RPCStub created),
 * but our client died completing the proxy:
 *     Assert failed, src/comm/core/Core.cxx:1005 + :1006, completeProxy()
 * Decompiling libcomm's completeProxy (sub_218D4) names those two asserts:
 *
 *     if (proxy.m_trackedState->m_interfaceStyle) { pc = rec+116; pd = rec+120; }
 *     else                                        { pc = rec+56;  pd = rec+60;  }
 *     if (!pc) doAssert("pc", Core.cxx, 1005);
 *     if (!pd) doAssert("pd", Core.cxx, 1006);
 *     ...
 *     impl = pc(&sid, transport, svcName, trackedState[14]);
 *     ProxyTracked::update(trackedState, 1 = ALIVE, impl, pd);
 *
 * pc/pd are the CLIENT proxy's create/destroy, read out of the factory record at
 * +0x38/+0x3C (we pass style 0). The phase-1 dump shows why they are NULL:
 *
 *   record 0  uuid 07302fa7-...  creators at +0x40/+0x44  (stub pair)     <- our service
 *   record 1  uuid 55183b6f-...  creators at +0x38/+0x3C  (proxy pair)
 *
 * So the shipped lib has a *stub* creator for DSIAudioManagement and no client
 * proxy creator — exactly phase 1's conclusion ("the client proxy is generated
 * into the consumer binary, not shipped"), now confirmed a second way.
 * (Aside: 55183b6f is the same UUID our provider's stub looks up and never
 * finds — it is the reply-direction service, not the client proxy for 07302fa7.)
 *
 * WHAT THIS PROBE DOES
 * --------------------
 * Supplies the missing pair: patch record 0's +0x38/+0x3C to point at our own
 * create/destroy, so completeProxy has something to call. Our create just
 * records its arguments and returns a heap object — enough for
 * ProxyTracked::update() to flip the lifecycle to ALIVE (1).
 *
 * That is the point: it proves the client half end-to-end and CAPTURES THE
 * TRANSPORT (create's 2nd argument), which is the object the RE'd send path
 * needs — `holder = this+8; transport = holder->vslot[6](); send =
 * (*transport)[2]; send(transport, dest, payload, u16@this+16, methodId)`.
 * Sending ID 8 (getActiveEntertainmentConnection) is the next step and is
 * deliberately NOT done here: this run establishes ALIVE first.
 *
 * Patching a firmware table in our own address space is a probe technique, not
 * a shipping one. The real client must carry a generated proxy; this tells us
 * what that proxy has to provide before we write one.
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

static void *proxy_create(void *sid_ptr, void *transport, void *svc_name, uint32_t a4)
{
   void *impl = calloc(1, 256);         /* opaque to the framework; it only stores it */
   g_create_calls++;
   g_transport = transport;
   printf("\n   >>> proxy_create CALLED  sid@%p(=%u) transport=%p svcname=%p a4=0x%08x\n",
          sid_ptr, sid_ptr ? *(unsigned short *)sid_ptr : 0, transport, svc_name,
          (unsigned)a4);
   printf("   >>> returning impl=%p\n", impl);
   fflush(stdout);

   /* READ-ONLY reconnaissance for the ID 8 send. The RE'd send path is
    *   holder = *(void**)(this+8);              // inside the SharedPtr
    *   transport = holder->vslot[6](holder);
    *   send = (*transport)[2];
    *   send(transport, dest, payload, u16@this+16, methodId);
    * `transport` here is completeProxy's a4, the same value it stores with
    * SharedPtrDefaultBase::operator=(trackedState+96, a4) — i.e. a SharedPtr,
    * not the holder itself. Print the words and the vtable so the holder offset
    * and vslot[6] can be identified offline against libcomm, instead of
    * guessing a slot and calling it (a wrong slot on a live transport could
    * emit traffic or corrupt state). */
   dump("transport SharedPtr", transport, 6);
   {
      int k;
      for (k = 0; k < 6; k++) {
         void *w = ((void **)transport)[k];
         if (plausible(w)) {
            char lbl[64];
            snprintf(lbl, sizeof lbl, "  transport+0x%02x ->", k * 4);
            dump(lbl, w, 8);
            dump_vtable(lbl, w, 10);
         }
      }
   }
   fflush(stdout);
   return impl;
}

static void proxy_destroy(void *impl)
{
   printf("   >>> proxy_destroy(%p)\n", impl);
   fflush(stdout);
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
         rec = r;
   }
   if (!rec) { printf("   FATAL: no record with a missing proxy creator\n"); return 1; }
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
   if (g_fail) return 1;

   printf("\n2) bootstrap\n"); fflush(stdout);
   memset(osal_mem, 0, sizeof osal_mem); osal_ctor(osal_mem, 1, 0);
   util_init(cli_name, 1, 1, 1, 1);
   memset(sname, 0, sizeof sname); memset(snode, 0, sizeof snode);
   str_ctor(sname, cli_name, osal_mem); str_ctor(snode, cli_node, osal_mem);
   memset(agent, 0, sizeof agent); as_ctor(agent, sname, snode);
   printf("   AgentStarter::start -> %d\n", as_start(agent, NULL, 1));
   usleep(300 * 1000);

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
   proxy_ctor(proxy, identity, 0, NULL);
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
      if (last == 1)
         printf("PHASE 2e PASSED - client proxy is ALIVE; transport captured for the ID 8 send.\n");
      else
         printf("phase 2e incomplete (state=%d)\n", last);
   }
   fflush(NULL);
   _exit(0);
}
