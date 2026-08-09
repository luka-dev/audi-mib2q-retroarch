/* dsi_proxy_probe — phase 2b-i: construct a comm::Proxy and connect() it.
 *
 * Builds on phase 2a (dsi_bootstrap_probe), which proved a plain process can join
 * the DSI mesh. This is the next smallest testable step and deliberately stops
 * short of sending anything.
 *
 * WHY connect() ALONE IS WORTH A PROBE
 * ------------------------------------
 * comm::Proxy::connect() (libcomm @0x9e4f4) decompiles to:
 *
 *   state = this->m_storage;              // +0x04
 *   if (!state)          -> "Missing state object for proxy. Default constructed?"
 *   if (vslot[2](state))  return 0;       // already connected
 *   if (vslot[3](state)) -> error 1872
 *   core = comm::CoreApi::getInstance();  // singleton planted by AgentStarter
 *   return (*(*(void**)core + 20))(core, &proxy_copy);   // CoreApi vslot[5]
 *
 * i.e. the CORE does the service resolution and transport wiring — we only have to
 * hand it a correctly-constructed Proxy. So connect()'s return value tells us
 * whether our IdentityArgs/ctor are right, WITHOUT us having to solve the
 * generated-proxy send path first.
 *
 * WHAT IS BEING GUESSED (and how it fails loudly)
 * -----------------------------------------------
 *  - Proxy's size: recon says 12 (m_vptr, m_storage@+4, secondary vptr@+8).
 *  - IdentityArgs: 3 pointers {&{uuid,int}, &uuid_static, iface_name_table}
 *    — the shape is from sub_43B4 and was independently confirmed in dmdt.
 *  - The trailing int after the instance UUID (sub_43B4 took it from a ctor arg;
 *    we try 0).
 * Every object gets a poisoned guard tail that is verified after each call, so an
 * undersized model prints [SMASHED] instead of corrupting the heap.
 *
 * The factory table pairs the UUIDs crosswise (the STUB's identity carries record
 * 1's UUIDs), so which pair a *client* wants is not obvious — we try BOTH and
 * report each outcome instead of guessing.
 *
 * ORDERING: a call only works once the target service is registered (learned the
 * hard way: `dmdt gc` run before the HMI blocked forever). connect() itself should
 * be safe earlier, but run this after the audio service is up when possible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

#define SZ_PROXY   12          /* recon: vptr, m_storage@+4, secondary vptr@+8 */
#define SZ_UUID    20          /* ipl::UUID(u32,u32,u32,u16,u8 x6)             */
#define GUARD      64
#define GUARD_BYTE 0xA5

static int g_fail;

typedef struct { unsigned char *mem; size_t sz; const char *what; } obj_t;

static void obj_new(obj_t *o, size_t sz, const char *what)
{
   o->sz = sz; o->what = what;
   o->mem = (unsigned char*)calloc(1, sz + GUARD);
   memset(o->mem + sz, GUARD_BYTE, GUARD);
}
static int obj_check(obj_t *o)
{
   size_t i;
   for (i = 0; i < GUARD; i++)
      if (o->mem[o->sz + i] != GUARD_BYTE) {
         printf("      [SMASHED] %s wrote %u byte(s) past its %u-byte model\n",
                o->what, (unsigned)(i + 1), (unsigned)o->sz);
         fflush(stdout); g_fail++; return 0;
      }
   printf("      [ ok ] %s stayed inside its %u-byte model\n",
          o->what, (unsigned)o->sz); fflush(stdout);
   return 1;
}

static const char *LIBS[] = {
   "libiplcommon.so", "libosal.so", "libutil.so", "libcomm.so", "libdsicommon.so", NULL
};

static void *sym(const char *n)
{
   void *p = dlsym(RTLD_DEFAULT, n);
   printf("   %-96s %s\n", n, p ? "found" : "MISSING");
   if (!p) g_fail++;
   fflush(stdout);
   return p;
}

/* ---- bootstrap (proven in phase 2a) ---- */
static void (*osal_ctor)(void*, int, int);
static int  (*util_init)(const char*, int, int, int, int);
static void (*str_ctor)(void*, const char*, const void*);
static void (*as_ctor)(void*, const void*, const void*);
static int  (*as_start)(void*, void*, int);
/* ---- phase 2b-i ---- */
static void (*uuid_ctor)(void*, uint32_t, uint32_t, uint32_t, uint16_t,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t);
static void (*proxy_ctor)(void*, const void*, int, void*);
static int  (*proxy_connect)(void*);

/* one attempt: build IdentityArgs from a UUID pair, construct Proxy, connect */
static void try_pair(const char *label,
                     uint32_t a1,uint32_t a2,uint32_t a3,uint16_t a4,
                     const uint8_t *ab,
                     uint32_t b1,uint32_t b2,uint32_t b3,uint16_t b4,
                     const uint8_t *bb,
                     const char **iface_names)
{
   obj_t inst, stat, proxy;
   const void *identity[3];
   int rc, before = g_fail;

   printf("\n   --- attempt: %s ---\n", label); fflush(stdout);

   /* IdentityArgs[0] -> {ipl::UUID (20B), int}  (sub_43B4 appends an int) */
   obj_new(&inst, SZ_UUID + 4, "IdentityArgs[0] {uuid,int}");
   uuid_ctor(inst.mem, a1, a2, a3, a4, ab[0],ab[1],ab[2],ab[3],ab[4],ab[5]);
   if (!obj_check(&inst)) return;
   *(int*)(inst.mem + SZ_UUID) = 0;          /* the trailing int; try 0 */

   /* IdentityArgs[1] -> static ipl::UUID */
   obj_new(&stat, SZ_UUID, "IdentityArgs[1] uuid_static");
   uuid_ctor(stat.mem, b1, b2, b3, b4, bb[0],bb[1],bb[2],bb[3],bb[4],bb[5]);
   if (!obj_check(&stat)) return;

   identity[0] = inst.mem;
   identity[1] = stat.mem;
   identity[2] = iface_names;                /* interface-name table (off_219B8) */

   printf("      constructing comm::Proxy(IdentityArgs, style=0, listener=NULL)\n");
   fflush(stdout);
   obj_new(&proxy, SZ_PROXY, "comm::Proxy");
   proxy_ctor(proxy.mem, identity, 0, NULL);
   if (!obj_check(&proxy)) return;
   printf("      proxy words: [0]=%p [1]=%p(m_storage) [2]=%p\n",
          *(void**)(proxy.mem+0), *(void**)(proxy.mem+4), *(void**)(proxy.mem+8));
   fflush(stdout);

   if (!*(void**)(proxy.mem + 4)) {
      printf("      [FAIL] m_storage is NULL -> connect() would report\n"
             "             \"Missing state object for proxy\" (ctor did not take)\n");
      g_fail++; return;
   }

   printf("      calling connect() ...\n"); fflush(stdout);
   rc = proxy_connect(proxy.mem);
   printf("      connect() returned %d  %s\n", rc,
          rc == 0 ? "<- 0 = success/already-connected" : "<- non-zero = refused");
   fflush(stdout);
   if (rc != 0) g_fail++;
   if (g_fail == before) printf("      [ ok ] attempt \"%s\" succeeded\n", label);
   fflush(stdout);
}

int main(int argc, char **argv)
{
   const char *cli_name = (argc > 1) ? argv[1] : "RetroArch";
   const char *cli_node = (argc > 2) ? argv[2] : "local";
   const char **l;
   unsigned char osal_mem[8], sname[16], snode[16], agent[8];
   const char *iface_names[2];

   /* the two DSIAudioManagement UUID pairs from the factory table (phase 1) */
   static const uint8_t A_tail[6] = {0x33,0xD8,0x9F,0x0C,0xFE,0xAF}; /* 07302FA7... */
   static const uint8_t B_tail[6] = {0x39,0xF1,0x8A,0x58,0x27,0x7D}; /* 9DEC172B... */
   static const uint8_t C_tail[6] = {0xED,0x5A,0xE2,0x4F,0x6C,0x1C}; /* 55183B6F... */
   static const uint8_t D_tail[6] = {0x36,0x6B,0xA3,0x20,0x75,0xFE}; /* DB006BF0... */

   printf("=== dsi_proxy_probe (phase 2b-i: comm::Proxy + connect) ===\n");
   printf("    identity \"%s\" on node \"%s\"\n\n", cli_name, cli_node);
   fflush(stdout);

   printf("1) dlopen framework libs\n");
   for (l = LIBS; *l; l++) {
      void *h = dlopen(*l, RTLD_NOW | RTLD_GLOBAL);
      printf("   %-18s %s\n", *l, h ? "loaded" : dlerror());
      if (!h) g_fail++;
      fflush(stdout);
   }
   if (g_fail) { printf("FATAL: libs not loadable\n"); return 1; }

   printf("\n2) resolve symbols\n");
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
   if (g_fail) { printf("FATAL: missing symbols\n"); return 1; }

   printf("\n3) bootstrap (as proven in phase 2a)\n"); fflush(stdout);
   memset(osal_mem,0,sizeof osal_mem); osal_ctor(osal_mem, 1, 0);
   util_init(cli_name, 1,1,1,1);
   memset(sname,0,sizeof sname); memset(snode,0,sizeof snode);
   str_ctor(sname, cli_name, osal_mem);
   str_ctor(snode, cli_node, osal_mem);
   memset(agent,0,sizeof agent);
   as_ctor(agent, sname, snode);
   printf("   AgentStarter::start -> %d\n", as_start(agent, NULL, 1)); fflush(stdout);
   usleep(300 * 1000);       /* let registration settle before connecting */

   iface_names[0] = "dsi.audio.DSIAudioManagement";
   iface_names[1] = "dsi.audio.DSISound";

   printf("\n4) Proxy + connect, both UUID pairings (the table pairs them crosswise)\n");
   fflush(stdout);
   try_pair("pair A: inst=07302FA7-AE8E-5815-B570 static=9DEC172B-A004-5427-A4CC",
            0x07302FA7u, 0xAE8Eu, 0x5815u, 0xB570u, A_tail,
            0x9DEC172Bu, 0xA004u, 0x5427u, 0xA4CCu, B_tail, iface_names);
   try_pair("pair B: inst=55183B6F-9E82-5A18-BFDE static=DB006BF0-ACD2-5747-8415",
            0x55183B6Fu, 0x9E82u, 0x5A18u, 0xBFDEu, C_tail,
            0xDB006BF0u, 0xACD2u, 0x5747u, 0x8415u, D_tail, iface_names);

   printf("\n=== phase 2b-i %s ===\n",
          g_fail ? "INCOMPLETE - see the failing attempt above"
                 : "PASSED - a client Proxy connected");
   fflush(NULL);
   _exit(g_fail ? 1 : 0);
}
