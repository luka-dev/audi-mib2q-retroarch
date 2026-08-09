/* dsi_state_probe — phase 2b-ii: inspect a CONNECTED proxy's state.
 *
 * Phase 2b-i proved we can construct a comm::Proxy for DSIAudioManagement and
 * that connect() returns 0. The remaining unknown for an actual RPC is where the
 * *transport* comes from: the generated proxy does
 *
 *     holder    = *(void**)(this + 8);
 *     transport = (*(*(void**)holder + 24))(holder);   // holder vslot[6]
 *     send      = *(fn**)(*transport + 8);             // transport vslot[2]
 *
 * and in the generated class `this+8` lives inside a SharedPtr handed in by the
 * framework. Since connect() makes the *core* do the binding, whatever we need may
 * now be reachable from our own proxy — so this probe LOOKS, and only looks.
 *
 * DELIBERATELY READ-ONLY: it dereferences and prints, it never calls an unknown
 * vtable slot. Calling a wrong slot on a live framework object could send traffic
 * or corrupt state; printing addresses lets the slots be identified offline in
 * IDA against libcomm instead. Every dereference is bounds-checked against a
 * plausible-pointer test first, so a bad guess prints "(unreadable)" instead of
 * faulting.
 *
 * Output to feed back into IDA: the vtable addresses. Subtract libcomm's load base
 * (printed here via a known libcomm symbol) to get file offsets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>

static int g_fail;

static void *sym(const char *n)
{
   void *p = dlsym(RTLD_DEFAULT, n);
   if (!p) { printf("   MISSING %s\n", n); g_fail++; }
   return p;
}

/* A pointer from a reconstructed layout may be nonsense, and this process's
 * mappings are scattered (stack ~0x000f_xxxx, heap ~0x0010_xxxx, libs ~0x780x_xxxx),
 * so an address-range guess is useless — the first version of this probe computed
 * an inverted window and rejected everything. Instead: actually try the read, with
 * SIGSEGV/SIGBUS trapped and longjmp'd out of. Any pointer can then be probed
 * safely and a bad one prints "(faulted)" instead of killing the process. */
static sigjmp_buf g_jmp;
static volatile int g_trapping;

static void fault_handler(int sig)
{
   (void)sig;
   if (g_trapping)
      siglongjmp(g_jmp, 1);
   _exit(2);
}

/* try to read one word at p; returns 0 on fault */
static int try_read(const void *p, uintptr_t *out)
{
   if (!p || ((uintptr_t)p & 3))
      return 0;
   g_trapping = 1;
   if (sigsetjmp(g_jmp, 1) == 0) {
      *out = *(const volatile uintptr_t*)p;
      g_trapping = 0;
      return 1;
   }
   g_trapping = 0;
   return 0;
}

static int readable(const void *p)
{
   uintptr_t tmp;
   return try_read(p, &tmp);
}

static void dump_words(const char *label, const void *base, int n)
{
   int i;
   printf("   %s @ %p:\n", label, base);
   if (!readable(base)) { printf("      (unreadable)\n"); return; }
   for (i = 0; i < n; i++) {
      uintptr_t w;
      if (!try_read((const uintptr_t*)base + i, &w)) { printf("      +0x%02x = (faulted)\n", i*4); break; }
      printf("      +0x%02x = %08lx%s\n", i * 4, (unsigned long)w,
             readable((void*)w) ? "  <- readable pointer" : "");
   }
   fflush(stdout);
}

/* print a vtable's slots as raw addresses for offline identification */
static void dump_vtable(const char *label, const void *obj, int slots)
{
   int i;
   const uintptr_t *vt;
   if (!readable(obj)) { printf("   %s: object unreadable\n", label); return; }
   if (!try_read(obj, (uintptr_t*)&vt)) { printf("   %s: vptr unreadable\n", label); return; }
   printf("   %s vtable @ %p:\n", label, (void*)vt);
   if (!readable(vt)) { printf("      (unreadable)\n"); return; }
   for (i = 0; i < slots; i++) {
      uintptr_t sl;
      if (!try_read(vt + i, &sl)) { printf("      slot[%2d] (faulted)\n", i); break; }
      printf("      slot[%2d] (+0x%02x) = %08lx\n", i, i * 4, (unsigned long)sl);
   }
   fflush(stdout);
}

static void (*osal_ctor)(void*, int, int);
static int  (*util_init)(const char*, int, int, int, int);
static void (*str_ctor)(void*, const char*, const void*);
static void (*as_ctor)(void*, const void*, const void*);
static int  (*as_start)(void*, void*, int);
static void (*uuid_ctor)(void*, uint32_t, uint32_t, uint32_t, uint16_t,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
static void (*proxy_ctor)(void*, const void*, int, void*);
static int  (*proxy_connect)(void*);

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
   void *storage;
   static const uint8_t A_tail[6] = {0x33,0xD8,0x9F,0x0C,0xFE,0xAF};
   static const uint8_t B_tail[6] = {0x39,0xF1,0x8A,0x58,0x27,0x7D};

   printf("=== dsi_state_probe (phase 2b-ii: inspect a connected proxy) ===\n");
   printf("    READ-ONLY: dereferences and prints, never calls an unknown slot\n\n");
   fflush(stdout);

   for (l = LIBS; *l; l++)
      if (!dlopen(*l, RTLD_NOW | RTLD_GLOBAL)) { printf("FATAL dlopen %s\n", *l); return 1; }

   /* The framework auto-loads a service's proxy/stub factory by UUID-derived
    * filename, but only where the factory directory is configured — servicemgr
    * has that config, a bare probe does not. Without it the STUB_CREATED reply
    * lands in Core::completeProxy() with no generated RPCProxy to instantiate:
    *    Assert failed ... Core.cxx:1005, completeProxy()
    * which is exactly how the previous run died. Load it explicitly. */
   {
      const char *fac = "/eso/lib/factories/lib_07302fa7-ae8e-5815-b570-33d89f0cfeaf.so";
      void *fh = dlopen(fac, RTLD_NOW | RTLD_GLOBAL);
      printf("   factory %s -> %s\n", fac, fh ? "loaded" : dlerror());
      fflush(stdout);
   }

   osal_ctor = (void(*)(void*,int,int))              sym("_ZN4osal4OsalC1Ebb");
   util_init = (int(*)(const char*,int,int,int,int)) sym("_ZN4util4Util4initEPKcbbbb");
   str_ctor  = (void(*)(void*,const char*,const void*))
      sym("_ZN3ipl12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcRKS4_");
   as_ctor   = (void(*)(void*,const void*,const void*))
      sym("_ZN4comm12AgentStarterC1ERKN3ipl12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_");
   as_start  = (int(*)(void*,void*,int))
      sym("_ZN4comm12AgentStarter5startEPNS_25AgentStarterEventReceiverEb");
   uuid_ctor = (void(*)(void*,uint32_t,uint32_t,uint32_t,uint16_t,
                        uint8_t,uint8_t,uint8_t,uint8_t,uint8_t,uint8_t))
      sym("_ZN3ipl4UUIDC1Ejjjthhhhhh");
   proxy_ctor = (void(*)(void*,const void*,int,void*))
      sym("_ZN4comm5ProxyC1ERKNS_11TrackedBase12IdentityArgsENS_14InterfaceStyleEPNS_17LifecycleListenerE");
   proxy_connect = (int(*)(void*)) sym("_ZN4comm5Proxy7connectEv");
   if (g_fail) return 1;

   /* Derive a plausible address window from things we know are mapped: a libcomm
    * function and our own stack. Everything we print is checked against it. */
   signal(SIGSEGV, fault_handler);
   signal(SIGBUS,  fault_handler);
   printf("   libcomm anchor (Proxy::connect) = %p  (fault-trapped reads enabled)\n\n",
          (void*)proxy_connect);
   fflush(stdout);

   printf("1) bootstrap\n"); fflush(stdout);
   memset(osal_mem,0,sizeof osal_mem); osal_ctor(osal_mem, 1, 0);
   util_init(cli_name, 1,1,1,1);
   memset(sname,0,sizeof sname); memset(snode,0,sizeof snode);
   str_ctor(sname, cli_name, osal_mem); str_ctor(snode, cli_node, osal_mem);
   memset(agent,0,sizeof agent); as_ctor(agent, sname, snode);
   printf("   AgentStarter::start -> %d\n", as_start(agent, NULL, 1));
   usleep(300 * 1000);

   printf("\n2) build + connect the DSIAudioManagement proxy\n"); fflush(stdout);
   iface_names[0] = "dsi.audio.DSIAudioManagement";
   iface_names[1] = "dsi.audio.DSISound";
   memset(inst,0,sizeof inst); memset(stat,0,sizeof stat); memset(proxy,0,sizeof proxy);
   uuid_ctor(inst, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
             A_tail[0],A_tail[1],A_tail[2],A_tail[3],A_tail[4],A_tail[5]);
   *(int*)(inst + 20) = 0;
   uuid_ctor(stat, 0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu,
             B_tail[0],B_tail[1],B_tail[2],B_tail[3],B_tail[4],B_tail[5]);
   identity[0] = inst; identity[1] = stat; identity[2] = iface_names;
   proxy_ctor(proxy, identity, 0, NULL);
   printf("   connect() -> %d\n", proxy_connect(proxy)); fflush(stdout);

   /* connect() is ASYNC: it only kicks off the broker lookup + CREATE_STUB
    * round trip. The first run of this probe dumped m_storage immediately and
    * read state 0 (pending) at 00:01:07.77 — while the broker's CREATE_STUB
    * only landed at 00:01:08.13. So poll the lifecycle word instead of
    * snapshotting it. m_storage+0x04: 0=pending, 1=alive, 2=dead. */
   {
      volatile int *state = (volatile int*)((char*)*(void**)(proxy + 4) + 4);
      int last = -1, i;
      printf("\n2b) polling lifecycle at m_storage+0x04 (0=pending 1=alive 2=dead)\n");
      for (i = 0; i < 200; i++) {          /* up to ~10s */
         int s = *state;
         if (s != last) {
            printf("   t=%4dms  state=%d%s\n", i * 50, s,
                   s == 1 ? "   <<< ALIVE — provider bound" : "");
            fflush(stdout);
            last = s;
            if (s == 1) break;
         }
         usleep(50 * 1000);
      }
      if (last != 1)
         printf("   never went alive within 10s (last=%d)\n", last);
      fflush(stdout);
   }

   printf("\n3) the proxy itself (12-byte model)\n");
   dump_words("proxy", proxy, 4);
   dump_vtable("proxy primary", proxy, 10);

   storage = *(void**)(proxy + 4);
   printf("\n4) m_storage — the state object connect() operates on\n");
   dump_words("m_storage", storage, 16);
   dump_vtable("m_storage", storage, 12);

   /* Follow any pointer-looking word out of m_storage one level. The transport
    * (or the object owning it) should be among these. */
   printf("\n5) one level down from m_storage (pointer-looking words only)\n");
   if (readable(storage)) {
      int i;
      for (i = 0; i < 12; i++) {
         void *w = ((void**)storage)[i];
         if (readable(w)) {
            char lbl[48];
            snprintf(lbl, sizeof lbl, "m_storage+0x%02x ->", i * 4);
            dump_words(lbl, w, 8);
         }
      }
   }

   printf("\n=== phase 2b-ii done (inspection only) ===\n");
   printf("Feed the vtable addresses back into IDA against libcomm to name the\n"
          "slots; the transport getter is the one the generated proxy calls as\n"
          "holder vslot[6] (+0x18).\n");
   fflush(NULL);
   _exit(0);
}
