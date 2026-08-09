/* dsi_audio_probe — phase 1: validate the DSIAudioManagement factory ABI on target.
 *
 * WHY THIS EXISTS, AND WHY IT IS SEPARATE FROM RETROARCH
 * -----------------------------------------------------
 * Everything we know about `libdsiaudioproxy.so` came from disassembly
 * (see ../RE_DSI_AUDIO.md). Reconstructed ABI that is wrong does not fail
 * cleanly on this target — it corrupts memory. So before any of it is wired
 * into the emulator we prove the foundational layer here, in a throwaway
 * process, where a wrong guess can only kill the probe.
 *
 * PHASE 1 (this file) IS DELIBERATELY ZERO-RISK:
 *   it only dlopen()s the stock library, calls the single exported entry point
 *   `getProxyStubFactory`, and READS the factory table. No comm:: objects are
 *   constructed, no virtual calls are made, nothing is written. If our record
 *   layout is wrong the worst case is that the validation below prints a
 *   mismatch — which is exactly the information we want.
 *
 * It answers, with evidence:
 *   - does the library load standalone (deps resolvable outside the HMI)?
 *   - does `getProxyStubFactory` have the shape we think (count-out + table)?
 *   - is the record stride really 0xBC and the count really 4?
 *   - do the interface names / UUIDs / creator pointers match the static analysis?
 *
 * Only once this passes does phase 2 (AgentStarter + proxy + connect +
 * getActiveEntertainmentConnection, wire ID 8) make sense.
 *
 * Build:  see build_probe.sh          Run on target:  ./dsi_audio_probe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

/* ---- what we believe the ABI is (from RE_DSI_AUDIO.md) ------------------ */

#define REC_STRIDE      0xBC        /* 188 bytes per record   */
#define EXPECT_COUNT    4           /* 2 interfaces x 2 roles */

/* ipl::UUID ctor is (u32,u32,u32,u16,u8 x6) = 20 bytes. We deliberately treat
 * it as opaque bytes: we only compare/print it, never interpret the fields. */
typedef struct { unsigned char raw[20]; } ipl_uuid_t;

/* Only the fields we PROVED are named. The creator pair is deliberately NOT a
 * fixed field here: run 1 showed its offset differs between record kinds
 * (stub records carry it at +0x40/+0x44, proxy-reply records at +0x38/+0x3C),
 * so we *discover* it at runtime instead of asserting a layout. */
typedef struct {
   ipl_uuid_t   uuid1;          /* +0x00  proven */
   const char  *version1;       /* +0x14  "2.11.45" */
   const char  *version2;       /* +0x18  "2.11.45" */
   const char  *name;           /* +0x1C  interface name */
   ipl_uuid_t   uuid2;          /* +0x20  proven */
   unsigned char rest[REC_STRIDE - 0x34];   /* +0x34.. discovered below */
} dsi_factory_record_t;

#define KIND1_OFF  0x48         /* proven: role tag 1 */
#define KIND2_OFF  0x5C         /* proven: role tag 2 */
#define SCAN_LO    0x30         /* window we scan for the creator pair */
#define SCAN_HI    0x60

typedef void *(*get_factory_fn)(uint32_t *out_count);

static const char *g_lib_paths[] = {
   "/eso/lib/factories/libdsiaudioproxy.so",
   "/mnt/app/eso/lib/factories/libdsiaudioproxy.so",
   "libdsiaudioproxy.so",
   NULL
};

static int g_fail;
#define CHECK(cond, fmt, ...)                                      \
   do {                                                            \
      if (cond) { printf("   [ ok ] " fmt "\n", ##__VA_ARGS__); }   \
      else      { printf("   [FAIL] " fmt "\n", ##__VA_ARGS__); g_fail++; } \
   } while (0)

/* Anchor for "is this a pointer into libdsiaudioproxy's text?". We take the
 * address of its one exported function and accept anything within ±1 MB — the
 * module is ~140 KB, so this is generous but still rejects NULL and garbage.
 * Doing it by measurement (not by a hardcoded base) keeps it ASLR-agnostic. */
static uintptr_t g_mod_anchor;

static int is_code_ptr(uintptr_t v)
{
   uintptr_t d;
   if (!v || !g_mod_anchor)
      return 0;
   d = (v > g_mod_anchor) ? (v - g_mod_anchor) : (g_mod_anchor - v);
   return d < 0x100000;
}

static void hexuuid(const ipl_uuid_t *u, char *out /* >=48 */)
{
   int i;
   for (i = 0; i < 20; i++)
      sprintf(out + i * 2, "%02x", u->raw[i]);
   out[40] = 0;
}

int main(void)
{
   void            *h = NULL;
   get_factory_fn   getfac;
   uint32_t         count = 0xDEADBEEF;
   void            *table = NULL;
   const char     **p;
   unsigned         i;
   int              audio_records = 0;

   printf("=== dsi_audio_probe (phase 1: factory table validation) ===\n\n");

   /* 1. load ------------------------------------------------------------ */
   printf("1) dlopen libdsiaudioproxy.so\n");
   for (p = g_lib_paths; *p && !h; p++) {
      h = dlopen(*p, RTLD_NOW);
      printf("   %-46s : %s\n", *p, h ? "loaded" : dlerror());
   }
   if (!h) {
      printf("\n   FATAL: cannot load. If this fails standalone, the library's\n"
             "   dependencies (libdsicommon/libcomm/libosal/libutil/libipl) are\n"
             "   not resolvable outside the HMI process - that is itself the\n"
             "   finding, and phase 2 would be blocked on it.\n");
      return 1;
   }

   /* 2. the one exported entry point ------------------------------------ */
   printf("\n2) dlsym getProxyStubFactory\n");
   getfac = (get_factory_fn)dlsym(h, "getProxyStubFactory");
   CHECK(getfac != NULL, "symbol resolved (%p)", (void*)getfac);
   if (!getfac) return 1;
   g_mod_anchor = (uintptr_t)getfac;    /* anchor for is_code_ptr() */

   /* Documented-by-disassembly behaviour: NULL arg -> NULL return. Probing
    * this first is free and tells us the calling convention matches. */
   printf("\n3) call with NULL (expect NULL return, per sub_2998)\n");
   CHECK(getfac(NULL) == NULL, "getProxyStubFactory(NULL) == NULL");

   /* 3. the table ------------------------------------------------------- */
   printf("\n4) call with &count\n");
   table = getfac(&count);
   CHECK(table != NULL, "table pointer = %p", table);
   CHECK(count == EXPECT_COUNT, "count = %u (expected %u)", count, EXPECT_COUNT);
   if (!table || count == 0 || count > 64) {
      printf("   FATAL: implausible table/count - ABI assumption is wrong.\n");
      return 1;
   }

   /* 4. walk the records ------------------------------------------------ */
   printf("\n5) records (assuming stride 0x%X)\n", REC_STRIDE);
   for (i = 0; i < count; i++) {
      const dsi_factory_record_t *r =
         (const dsi_factory_record_t *)((const unsigned char *)table + i * REC_STRIDE);
      char u1[48], u2[48];
      const char *name = r->name;

      hexuuid(&r->uuid1, u1);
      hexuuid(&r->uuid2, u2);

      printf("\n   --- record %u @ %p ---\n", i, (const void*)r);
      printf("   uuid1    : %s\n", u1);
      printf("   uuid2    : %s\n", u2);
      printf("   version  : %s / %s\n",
             r->version1 ? r->version1 : "(null)",
             r->version2 ? r->version2 : "(null)");
      printf("   name     : %s\n", name ? name : "(null)");
      /* --- discover the creator pair instead of assuming its offset --- */
      {
         const unsigned char *b = (const unsigned char *)r;
         unsigned off;
         int found = 0;
         printf("   code ptrs found in +0x%02X..+0x%02X:\n", SCAN_LO, SCAN_HI);
         for (off = SCAN_LO; off + 4 <= SCAN_HI; off += 4) {
            uintptr_t v;
            memcpy(&v, b + off, sizeof(v));
            if (is_code_ptr(v)) {
               printf("      +0x%02X = %p%s\n", off, (void*)v,
                      (off == KIND1_OFF || off == KIND2_OFF) ? "  <- (tag slot?)" : "");
               found++;
            }
         }
         CHECK(found >= 2, "at least one create/destroy pair present (found %d ptr(s))", found);

         printf("   kind tags: +0x%02X = %d   +0x%02X = %d\n",
                KIND1_OFF, *(const int*)(b + KIND1_OFF),
                KIND2_OFF, *(const int*)(b + KIND2_OFF));
         CHECK(*(const int*)(b + KIND1_OFF) == 1 && *(const int*)(b + KIND2_OFF) == 2,
               "role tags are 1 and 2");

         /* raw evidence, so the real layout can be read off the log */
         printf("   raw +0x30..+0x60:");
         for (off = 0x30; off < 0x60; off += 4) {
            uintptr_t v; memcpy(&v, b + off, sizeof(v));
            if (!((off - 0x30) % 16)) printf("\n      +0x%02X:", off);
            printf(" %08lx", (unsigned long)v);
         }
         printf("\n");
      }

      /* The single strongest check: if the stride/offsets are right, the name
       * pointer lands on one of exactly two known strings. If it doesn't, our
       * layout is wrong and NOTHING further should be trusted. */
      if (name && (!strcmp(name, "dsi.audio.DSIAudioManagement") ||
                   !strcmp(name, "dsi.audio.DSISound"))) {
         printf("   [ ok ] name matches a known interface\n");
         if (!strcmp(name, "dsi.audio.DSIAudioManagement"))
            audio_records++;
      } else {
         printf("   [FAIL] name is not a known interface -> layout/stride wrong\n");
         g_fail++;
      }

   }

   printf("\n6) summary\n");
   CHECK(audio_records == 2, "found %d DSIAudioManagement records (expected 2: stub + proxy-reply)",
         audio_records);

   printf("\n=== %s (%d check(s) failed) ===\n",
          g_fail ? "ABI MISMATCH - do NOT proceed to phase 2" :
                   "phase 1 PASSED - factory ABI confirmed on target",
          g_fail);
   printf("\nNext, only if this passed: phase 2 = osal::Osal + util::Util::init +\n"
          "comm::AgentStarter(name,\"local\") + start(), build the client proxy,\n"
          "connect(), then call getActiveEntertainmentConnection (wire ID 8,\n"
          "read-only) BEFORE ever sending requestConnection (ID 12).\n");

   /* deliberately not dlclose()d: the static ctors registered atexit handlers */
   return g_fail ? 1 : 0;
}
