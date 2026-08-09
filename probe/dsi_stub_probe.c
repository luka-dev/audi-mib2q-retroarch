/* dsi_stub_probe — phase 2c: register our OWN dsi.audio.DSIAudioManagement
 * service, then point a client proxy at it and watch the proxy go ALIVE.
 *
 * WHY
 * ---
 * The real provider (`AudioProcess`) lives on the RCC — a different processor with
 * a different framework (Colibry/MoCCA, servicebroker-DSI) talking to the audio
 * DSP — so it cannot be staged into this MMX image. In our QEMU every DSI proxy
 * therefore reports "service N/A", and our (verified-correct) client proxy can
 * never reach state 1. Standing up a *stub* provider closes that gap without the
 * RCC: it lets us prove the client goes alive and, next, that a request actually
 * arrives — i.e. it validates the send path and the wire-ID map.
 *
 * RECIPE — reused from Patches/dio_manager-mhi2q (they already reversed and BUILT
 * DSI service registration for dio's CarPlay service; see
 * from-source/dio-src/CDSICarplayImpl.cpp attachBroker/reconBuildRecord). We only
 * swap in the DSIAudioManagement UUID. All entry points below are REAL exported
 * symbols, verified present:
 *
 *   _ZN3dsi19ServiceProviderBaseC1EPKcj                  (libdsicommon)
 *   _ZN4comm13LifecycleImplC1ENS_9Lifecycle5StateEPNS_17LifecycleListenerE
 *   _ZTVN4comm11TrackedBaseE                             (vtable object)
 *   _ZN4util20SharedPtrDefaultBaseC2ERKS0_
 *   _ZN4comm19ServiceRegistration15registerServiceEv     <- the actual register
 *
 * The 0x6C "registration record" built here is the same structure we deliberately
 * avoided on the client side (the 3-arg Proxy ctor let us skip it) — on the
 * provider side there is no way around it, which is exactly why reusing dio's
 * reconstruction matters.
 *
 * Guard tails are kept on every object we size ourselves, so a wrong size prints
 * [SMASHED] rather than corrupting the heap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

#define GUARD 64
#define GUARD_BYTE 0xA5
#define SZ_SPB     36      /* dsi::ServiceProviderBase, recon: vptr + 32          */
#define SZ_HOLDER  64      /* comm::ServiceRegistration: vptr@0, m_ref@+4 (roomy) */
#define SZ_REC   0x6C      /* the registration record                             */
#define SZ_PROXY   12

static int g_fail;

static void *sym(const char *n)
{
   void *p = dlsym(RTLD_DEFAULT, n);
   printf("   %-72s %s\n", n, p ? "found" : "MISSING");
   if (!p) g_fail++;
   fflush(stdout);
   return p;
}

static unsigned char *obj_new(size_t sz)
{
   unsigned char *m = (unsigned char*)calloc(1, sz + GUARD);
   memset(m + sz, GUARD_BYTE, GUARD);
   return m;
}
static void obj_check(const char *what, const unsigned char *m, size_t sz)
{
   size_t i;
   for (i = 0; i < GUARD; i++)
      if (m[sz + i] != GUARD_BYTE) {
         printf("   [SMASHED] %s exceeded its %u-byte model by %u\n",
                what, (unsigned)sz, (unsigned)(i + 1));
         g_fail++; fflush(stdout); return;
      }
   printf("   [ ok ] %s within its %u-byte model\n", what, (unsigned)sz);
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
   int i, rc;

   void (*osal_ctor)(void*,int,int);
   int  (*util_init)(const char*,int,int,int,int);
   void (*str_ctor)(void*,const char*,const void*);
   void (*as_ctor)(void*,const void*,const void*);
   int  (*as_start)(void*,void*,int);
   void (*uuid_ctor)(void*,uint32_t,uint32_t,uint32_t,uint16_t,
                     uint8_t,uint8_t,uint8_t,uint8_t,uint8_t,uint8_t);
   void (*spb_ctor)(void*, const char*, unsigned);
   void (*life_ctor)(void*, int, void*);
   void (*spdb_copy)(void*, const void*);
   int  (*svc_register)(void*);
   void (*proxy_ctor)(void*, const void*, int, void*);
   int  (*proxy_connect)(void*);
   void *tb_vtable;

   unsigned char osal_mem[8], sname[16], snode[16], agent[8];
   unsigned char *spb, *holder, *rec, *proxy;
   unsigned char svcUuid[20+GUARD], statUuid[20+GUARD], spEmpty[12];
   unsigned char instId[24+GUARD];
   const char *iface_names[2];
   const void *identity[3];

   printf("=== dsi_stub_probe (phase 2c: our own DSIAudioManagement provider) ===\n\n");
   fflush(stdout);

   for (l = LIBS; *l; l++)
      if (!dlopen(*l, RTLD_NOW | RTLD_GLOBAL)) { printf("FATAL dlopen %s\n", *l); return 1; }

   printf("1) resolve provider-side entry points (all real exports)\n");
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
   spb_ctor  = (void(*)(void*,const char*,unsigned)) sym("_ZN3dsi19ServiceProviderBaseC1EPKcj");
   life_ctor = (void(*)(void*,int,void*))
      sym("_ZN4comm13LifecycleImplC1ENS_9Lifecycle5StateEPNS_17LifecycleListenerE");
   spdb_copy = (void(*)(void*,const void*)) sym("_ZN4util20SharedPtrDefaultBaseC2ERKS0_");
   svc_register = (int(*)(void*)) sym("_ZN4comm19ServiceRegistration15registerServiceEv");
   tb_vtable = sym("_ZTVN4comm11TrackedBaseE");
   proxy_ctor = (void(*)(void*,const void*,int,void*))
      sym("_ZN4comm5ProxyC1ERKNS_11TrackedBase12IdentityArgsENS_14InterfaceStyleEPNS_17LifecycleListenerE");
   proxy_connect = (int(*)(void*)) sym("_ZN4comm5Proxy7connectEv");
   if (g_fail) { printf("\nFATAL: missing entry points\n"); return 1; }

   printf("\n2) bootstrap (phase 2a)\n"); fflush(stdout);
   memset(osal_mem,0,sizeof osal_mem); osal_ctor(osal_mem, 1, 0);
   util_init(cli_name, 1,1,1,1);
   memset(sname,0,sizeof sname); memset(snode,0,sizeof snode);
   str_ctor(sname, cli_name, osal_mem); str_ctor(snode, cli_node, osal_mem);
   memset(agent,0,sizeof agent); as_ctor(agent, sname, snode);
   printf("   AgentStarter::start -> %d\n", as_start(agent, NULL, 1)); fflush(stdout);
   usleep(300*1000);

   printf("\n3) dsi::ServiceProviderBase(\"RA_AudioMgmtStub\", 0)\n"); fflush(stdout);
   spb = obj_new(SZ_SPB);
   spb_ctor(spb, "RA_AudioMgmtStub", 0);
   obj_check("ServiceProviderBase", spb, SZ_SPB);

   printf("\n4) build the 0x6C registration record (dio's recipe, our UUID)\n");
   fflush(stdout);
   memset(svcUuid, 0, sizeof svcUuid); memset(svcUuid+20, GUARD_BYTE, GUARD);
   /* DSIAudioManagement service UUID (factory table record 0, phase 1) */
   uuid_ctor(svcUuid, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
             0x33,0xD8,0x9F,0x0C,0xFE,0xAF);
   obj_check("service UUID", svcUuid, 20);

   rec = obj_new(SZ_REC);
   life_ctor(rec, 0, NULL);                       /* comm::LifecycleImpl(state,listener) */
   memset(rec + 0x24, 0, SZ_REC - 0x24);
   *(const void**)rec = (const char*)tb_vtable + 8;   /* vtable = TrackedBase + 8 */
   memcpy(rec + 0x1c, svcUuid, 20);
   memcpy(rec + 0x58, svcUuid, 20);
   *(void**)(rec + 0x34) = rec + 0x58;
   memset(spEmpty, 0, sizeof spEmpty);
   spdb_copy(rec + 0x48, spEmpty);                /* empty SharedPtrDefaultBase */
   (*(volatile unsigned*)(rec + 0x40))++;         /* build refcount */
   obj_check("0x6C record", rec, SZ_REC);
   printf("   record @ %p, vptr=%p\n", (void*)rec, *(void**)rec); fflush(stdout);

   printf("\n5) holder = comm::ServiceRegistration, install record, register\n");
   fflush(stdout);
   holder = obj_new(SZ_HOLDER);
   *(void**)holder = (char*)sym("_ZTVN4comm19ServiceRegistrationE") + 8;
   *(void**)(holder + 4) = rec;                   /* tracked ref */
   rc = svc_register(holder);
   printf("   ServiceRegistration::registerService() -> %d %s\n", rc,
          rc == 0 ? "<- 0 = registered" : "<- non-zero = refused");
   obj_check("ServiceRegistration holder", holder, SZ_HOLDER);
   if (rc != 0) g_fail++;

   printf("\n6) now point a CLIENT proxy at the same UUID and watch it go alive\n");
   fflush(stdout);
   iface_names[0] = "dsi.audio.DSIAudioManagement";
   iface_names[1] = "dsi.audio.DSISound";
   memset(instId,0,sizeof instId); memset(instId+24, GUARD_BYTE, GUARD);
   uuid_ctor(instId, 0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u,
             0x33,0xD8,0x9F,0x0C,0xFE,0xAF);
   *(int*)(instId + 20) = 0;
   memset(statUuid,0,sizeof statUuid);
   uuid_ctor(statUuid, 0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu,
             0x39,0xF1,0x8A,0x58,0x27,0x7D);
   identity[0] = instId; identity[1] = statUuid; identity[2] = iface_names;
   proxy = obj_new(SZ_PROXY);
   proxy_ctor(proxy, identity, 0, NULL);
   printf("   connect() -> %d\n", proxy_connect(proxy)); fflush(stdout);

   /* lifecycle state lives at m_storage+4: 0=pending 1=alive 2=dead */
   {
      void *storage = *(void**)(proxy + 4);
      printf("   polling lifecycle state at m_storage+4 (0=pending 1=alive 2=dead)\n");
      for (i = 0; i < 20; i++) {
         int st = storage ? *(volatile int*)((char*)storage + 4) : -1;
         printf("      t=%2ds state=%d%s\n", i, st, st == 1 ? "  <- ALIVE" : "");
         fflush(stdout);
         if (st == 1) break;
         sleep(1);
      }
   }

   printf("\n=== phase 2c %s ===\n", g_fail ? "INCOMPLETE" : "provider registered");
   fflush(NULL);
   _exit(g_fail ? 1 : 0);
}
