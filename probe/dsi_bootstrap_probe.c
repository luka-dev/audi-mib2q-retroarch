/* dsi_bootstrap_probe — phase 2a: can a plain process join the DSI mesh?
 *
 * WHY dlopen AND NOT LINKING
 * --------------------------
 * The firmware's framework libs (libosal/libutil/libiplcommon/libcomm) cannot be
 * link-time bound with the SDP's binutils 2.19: `readelf -h` on them reports
 *   "no .dynamic section in the dynamic segment"
 * and ld opens the file, sees the SONAME/DT_HASH, yet resolves nothing — the
 * requested and provided mangled names are byte-identical (verified:
 * `_ZN4osal4OsalC1Ebb` wanted, `_ZN4osal4OsalC1Ebb` present, FUNC GLOBAL).
 * So we bind at RUNTIME instead, exactly like the proven libdisplayinit path.
 *
 * This is NOT the "hand-copied ABI" mistake we rejected for hiddi. There the sin
 * was inventing *struct layouts*. Here every object we touch is either opaque or
 * a size recovered from disassembly, and we only call functions whose C++ mangled
 * names encode their full signature — the name itself is the contract:
 *
 *   _ZN4osal4OsalC1Ebb                 osal::Osal::Osal(bool,bool)          this: 1 byte
 *   _ZN4util4Util4initEPKcbbbb         util::Util::init(const char*,b,b,b,b)  static
 *   _ZN3ipl12basic_string...C1EPKcRKS4_  ipl::string::string(const char*, const alloc&)  this: 12 bytes
 *   _ZN4comm12AgentStarterC1ERKN3ipl...  comm::AgentStarter(const ipl::string&, const ipl::string&)  this: 4 bytes (PIMPL)
 *   _ZN4comm12AgentStarter5startEPNS_25AgentStarterEventReceiverEb
 *
 * Sizes are from the harman-sdk recon (Patches/dio_manager-mhi2q) and are the one
 * thing that could still be wrong — so each object gets a guard region that is
 * checked for overrun after the call. A too-small size shows up as a loud
 * "SMASHED" line instead of silent heap corruption.
 *
 * Phase 2a does the bootstrap ONLY (no proxy, no connect, no RPC). Each step
 * prints before it runs and flushes, so if the process dies the last line names
 * the exact call that broke.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

/* object sizes recovered from disassembly / the harman-sdk recon */
#define SZ_OSAL          1     /* ctor writes 1 byte to this; rest is static state */
#define SZ_IPL_STRING   12     /* {u32 size, u32 cap, char* data}                   */
#define SZ_AGENTSTARTER  4     /* PIMPL: sole member is a void* to a 0x7b8 impl     */

#define GUARD 64
#define GUARD_BYTE 0xA5

typedef struct { unsigned char *mem; size_t sz; const char *what; } obj_t;

static int g_fail;

/* allocate an object with a poisoned guard tail so an undersized model is loud */
static void obj_new(obj_t *o, size_t sz, const char *what)
{
   o->sz = sz; o->what = what;
   o->mem = (unsigned char*)calloc(1, sz + GUARD);
   memset(o->mem + sz, GUARD_BYTE, GUARD);
}
static void obj_check(obj_t *o)
{
   size_t i;
   for (i = 0; i < GUARD; i++)
      if (o->mem[o->sz + i] != GUARD_BYTE) {
         printf("   [SMASHED] %s wrote %u byte(s) past our %u-byte model "
                "-> the recovered size is WRONG, stop here\n",
                o->what, (unsigned)(i + 1), (unsigned)o->sz);
         fflush(stdout); g_fail++; return;
      }
   printf("   [ ok ] %s stayed inside its %u-byte model\n", o->what, (unsigned)o->sz);
   fflush(stdout);
}

static const char *LIBS[] = {
   "libiplcommon.so", "libosal.so", "libutil.so", "libcomm.so", "libdsicommon.so", NULL
};

static void *sym(const char *name)
{
   void *p = dlsym(RTLD_DEFAULT, name);
   printf("   %-70s %s\n", name, p ? "found" : "MISSING");
   if (!p) g_fail++;
   fflush(stdout);
   return p;
}

int main(int argc, char **argv)
{
   const char *cli_name = (argc > 1) ? argv[1] : "RAProbe";
   const char *cli_node = (argc > 2) ? argv[2] : "local";   /* dmdt uses "local" */
   const char **l;
   int i;

   /* ctor/function pointer types — signatures come from the mangled names */
   void  (*osal_ctor)(void*, int, int);
   int   (*util_init)(const char*, int, int, int, int);
   void  (*str_ctor)(void*, const char*, const void*);
   void  (*as_ctor)(void*, const void*, const void*);
   int   (*as_start)(void*, void*, int);

   obj_t osal, sname, snode, agent;
   unsigned char alloc_dummy[4];      /* ipl::allocator<char> is empty; pass a ref */

   printf("=== dsi_bootstrap_probe (phase 2a: join the DSI mesh) ===\n");
   printf("    name=\"%s\" node=\"%s\"\n\n", cli_name, cli_node);
   fflush(stdout);

   printf("1) dlopen the framework libs (GLOBAL so inter-lib refs resolve)\n");
   for (l = LIBS; *l; l++) {
      void *h = dlopen(*l, RTLD_NOW | RTLD_GLOBAL);
      printf("   %-18s %s\n", *l, h ? "loaded" : dlerror());
      if (!h) g_fail++;
      fflush(stdout);
   }
   if (g_fail) { printf("\nFATAL: framework libs not loadable -> stop.\n"); return 1; }

   printf("\n2) resolve the bootstrap entry points\n");
   osal_ctor = (void(*)(void*,int,int))                      sym("_ZN4osal4OsalC1Ebb");
   util_init = (int(*)(const char*,int,int,int,int))         sym("_ZN4util4Util4initEPKcbbbb");
   str_ctor  = (void(*)(void*,const char*,const void*))
      sym("_ZN3ipl12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKcRKS4_");
   as_ctor   = (void(*)(void*,const void*,const void*))
      sym("_ZN4comm12AgentStarterC1ERKN3ipl12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_");
   as_start  = (int(*)(void*,void*,int))
      sym("_ZN4comm12AgentStarter5startEPNS_25AgentStarterEventReceiverEb");
   if (g_fail) { printf("\nFATAL: missing entry points -> stop.\n"); return 1; }

   printf("\n3) osal::Osal(true, false)\n"); fflush(stdout);
   obj_new(&osal, SZ_OSAL, "osal::Osal");
   osal_ctor(osal.mem, 1, 0);
   obj_check(&osal);

   printf("\n4) util::Util::init(\"%s\", 1,1,1,1)\n", cli_name); fflush(stdout);
   i = util_init(cli_name, 1, 1, 1, 1);
   printf("   returned %d\n", i); fflush(stdout);

   printf("\n5) ipl::string x2\n"); fflush(stdout);
   memset(alloc_dummy, 0, sizeof(alloc_dummy));
   obj_new(&sname, SZ_IPL_STRING, "ipl::string(name)");
   obj_new(&snode, SZ_IPL_STRING, "ipl::string(node)");
   str_ctor(sname.mem, cli_name, alloc_dummy);
   str_ctor(snode.mem, cli_node, alloc_dummy);
   obj_check(&sname); obj_check(&snode);
   /* {size, cap, data} — print it back as a self-check of the layout */
   printf("   name: size=%u cap=%u data=\"%s\"\n",
          *(uint32_t*)sname.mem, *(uint32_t*)(sname.mem+4),
          *(char**)(sname.mem+8) ? *(char**)(sname.mem+8) : "(null)");
   printf("   node: size=%u cap=%u data=\"%s\"\n",
          *(uint32_t*)snode.mem, *(uint32_t*)(snode.mem+4),
          *(char**)(snode.mem+8) ? *(char**)(snode.mem+8) : "(null)");
   fflush(stdout);

   printf("\n6) comm::AgentStarter(name, node)   <- the real test\n"); fflush(stdout);
   obj_new(&agent, SZ_AGENTSTARTER, "comm::AgentStarter");
   as_ctor(agent.mem, sname.mem, snode.mem);
   obj_check(&agent);
   printf("   impl ptr = %p\n", *(void**)agent.mem); fflush(stdout);

   printf("\n7) AgentStarter::start(NULL, true)\n"); fflush(stdout);
   i = as_start(agent.mem, NULL, 1);
   printf("   returned %d\n", i); fflush(stdout);

   printf("\n8) settling 500 ms on the bus ...\n"); fflush(stdout);
   usleep(500 * 1000);

   printf("\n=== phase 2a %s ===\n",
          g_fail ? "FAILED - do not proceed" : "PASSED - process is on the DSI mesh");
   printf("Next: 2b = DSIAudioManagement client proxy, connect(), then the\n"
          "read-only getActiveEntertainmentConnection (wire ID 8).\n");
   fflush(NULL);

   /* dmdt _Exit()s rather than unwinding; framework teardown is its own ABI
    * risk and would only add noise to a successful bring-up. */
   _exit(g_fail ? 1 : 0);
}
