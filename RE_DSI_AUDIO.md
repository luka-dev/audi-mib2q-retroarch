# RE: calling `dsi::audio::DSIAudioManagement` from a native process

> **Shipping decision (2026-08-07):** this document preserves the native DSI
> reverse engineering and the working prototype, but RetroArch no longer links
> that client. Exact MU1316 Java sources proved that the already-running LSD
> audio bundle exposes a Media `HMIAudioService` and listener registration path.
> The shipping JAR uses those stock services; therefore RetroArch needs no entry
> in `/config/framework.json`. QSA remains PCM-only. See §17 for current status.

Goal: acquire the entertainment audio connection (focus) from RetroArch without a
Java bridge and without touching `MS_ENT`. Target lib:
`/eso/lib/factories/libdsiaudioproxy.so` (MU1316, 139 KB, ARMv7, `-fno-rtti`).

Everything below is **disassembled fact**, not assumption. Addresses are file/VA
offsets in that .so unless stated otherwise.

## 1. The only real export: `getProxyStubFactory`

```c
// 0x2998
void *getProxyStubFactory(unsigned *out_count) {
    if (out_count) { *out_count = 4; return &table /*0x21A24*/; }
    return NULL;
}
```
Everything else exported is weak inline/template emission (`comm::Proxy::isAlive`
/`isDead`/`waitUntil*` + their **non-virtual thunks at +8**, `comm::StubBase::*`,
`util::transport::IMsgWriter`, vtables for `comm::StubBase` / `IMsgWriter`).
⇒ The generated classes are **not linkable**; they are reachable only through
this factory table.

## 2. The factory table — 4 records × **0xBC (188) bytes**, filled by the static
ctor `sub_2C74` (`.init_array`). Table base `0x21A24` (in **.bss**, so it is all
zeros in the file — read it only after the ctor runs).

| # | base | interface | record UUID | 2nd UUID | create | destroy | built by | size | vtable |
|---|------|-----------|-------------|----------|--------|---------|----------|------|--------|
| 0 | 0x21A24 | `dsi.audio.DSIAudioManagement` | `07302FA7-AE8E-5815-B570-33D89F0CFEAF` | `9DEC172B-A004-5427-A4CC-39F18A58277D` | `sub_2B8C` | `sub_29C4` | `sub_43B4` → logs **"Created RPCStub"** | 0x30 | `off_210A8` |
| 1 | 0x21AE0 | `dsi.audio.DSIAudioManagement` | `55183B6F-9E82-5A18-BFDE-ED5AE24F6C1C` | `DB006BF0-ACD2-5747-8415-366BA32075FE` | `sub_2B30` | `sub_29E0` | `sub_73F4` → logs **"Created RPCProxyReply"** | 0x2C | `off_21200` |
| 2 | 0x21B9C | `dsi.audio.DSISound` | `52B6F4B5-97C0-5B3F-9A7B-31A32ACA2906` | `1F545ED6-0481-5C05-B5FB-CCC67E20F61C` | `sub_2AD4` | `sub_29FC` | — | — | — |
| 3 | 0x21C58 | `dsi.audio.DSISound` | `781E4762-EF2B-5A94-972A-3C284BDB0FD4` | `E1E9715F-7C10-5394-B9B3-C506ACBE5BA0` | `sub_2A78` | `sub_2A18` | — | — | — |

Record field offsets (from the ctor's stores, entry 0 as reference):
```
+0x00  ipl::UUID   record uuid            (0x14 bytes incl. padding)
+0x14  const char* "2.11.45"              (version)
+0x18  const char* "2.11.45"              (version #2 - min/max?)
+0x1C  const char* "dsi.audio.DSIAudioManagement"
+0x20  ipl::UUID   second uuid            (0x14)
+0x40  fnptr       create   (= +0x54 duplicate)
+0x44  fnptr       destroy  (= +0x58 duplicate)
+0x48  int 1                              (role/kind tag)
+0x5C  int 2                              (role/kind tag)
+0x84  int 1 ; +0x98 int 2                (second sub-record's tags)
```
⇒ each record carries **two {create,destroy,kind}** sub-records (kind 1 / kind 2).

**Consequence:** the table supplies **STUB** (server-side, receives requests) and
**PROXY_REPLY** (client-side, receives replies). The *outgoing* client Proxy that
would carry `requestConnection` is **not** in this table — see §5.

## 3. Creator / destroyer shape (verified)

```c
// sub_2B8C  (create, entry 0)
void *create(a1,a2,a3,a4) { void *p = operator new(0x30, nothrow); sub_43B4(p,a1,a2,a3,a4); return p; }
// sub_29C4  (destroy)
void destroy(void *o) { if (o) (*(void(**)(void*))(*(void**)o + 20))(o); }   // vtable index 5 = deleting dtor
```
So: **create takes 4 args after `this`; destroy is virtual slot [5] (offset +20).**

## 4. Object layout + base ctor ABI (from `sub_43B4`, the stub)

```
obj+0x00  vptr  (off_210A8)              <- primary base
obj+0x04  0
obj+0x08  comm::Proxy subobject          <- comm::Proxy::Proxy(obj+8, IdentityArgs&, 0, 0)
obj+0x08  vptr overwritten with off_21038
obj+0x10  vptr off_2105C                 <- SECOND base (matches the +8 thunks)
obj+0x14  util::SharedPtrDefaultBase
obj+0x20  ptr from vslot[5] of a2->[1]
obj+0x24  result of vslot[4] on that
obj+0x28  *(a4+20)
obj+0x2C  a5
```
**`comm::TrackedBase::IdentityArgs` is 3 pointers**, built on the stack as
`{ &uuid_instance, &uuid_static, off_219B8 }`, then:
```c
comm::Proxy::Proxy(this+8, IdentityArgs const&, InterfaceStyle /*=0*/, LifecycleListener* /*=0*/);
```
(matches the imported mangled signature exactly).

## 5. ⚠ The wire method IDs are NOT the Java constants

`sub_3704` = `DSIAudioManagementRPCStub::calls` dispatcher. Its switch enumerates
the **on-wire method IDs**:

```
1, 2, 3, 5, 7, 8, 9, 11, 12, 14, 15, 16, 17, 24
```

The Java interface (`org/dsi/ifc/audio/DSIAudioManagement.java`) uses a *different*
numbering — `RT_FADETOCONNECTION=1000 … RT_REQUESTCONNECTION=1004 … RP_*=2000..`.
**Do not marshal 1004.** The Java `RT_/RP_` values are a higher-level (Java-side)
enumeration; the transport carries the small IDs above. Each ID is turned into a
`comm::CallEvent::CallID` via
`CallID::assign(&callid, ptr, const uint16_t& method_id)` and compared with `==`.

### ✅ RESOLVED — the full DSIAudioManagement wire-ID map

Decoded from `sub_5A68` (`DSIAudioManagementRPCStub::process`) by cross-checking
**(a)** how many values each case deserializes, **(b)** their types, and **(c)** the
handler vtable slot it dispatches to:

| wire ID | deserializes | handler slot | ⇒ method |
|---------|--------------|--------------|----------|
| **5**  | 2 × int          | +0  | `fadeToConnection(int,int)` |
| **11** | 2 × int          | +4  | `releaseConnection(int,int)` |
| **7**  | 1 × int          | +8  | `getActiveConnection(int)` |
| **8**  | 1 × int          | +12 | `getActiveEntertainmentConnection(int)` |
| **12** | **3 × int**      | +16 | **`requestConnection(int,int,int)`** |
| **17** | 2 × int + **bool** | +20 | `setVolumeLock(int,int,bool)` |
| **9**  | 2 × int          | +24 | `getVolumeLock(int,int)` |
| 1  | – | +48 | (registerClient / lifecycle) |
| 2  | 1 × `CIArray<int>` | +40 | (array setter) |
| 3  | 1 × int | +44 | |
| 14 | – | +36 | |
| 15 | 1 × `CIArray<int>` | +28 | |
| 16 | 1 × int | +32 | |
| 24 | 2 × arrays | +52 | |

**Three independent signals agree**, which is what makes this trustworthy:
1. **Arg count** — `requestConnection` is the *only* 3-int method ⇒ ID 12 is proven
   outright.
2. **Arg types** — ID 17 deserializes two ints *plus a bool* (read through a
   different deserializer vslot, `+24` vs `+44`), which uniquely fits
   `setVolumeLock(int,int,bool)`.
3. **Slot order** — the handler slots `+0,+4,+8,+12,+16,+20,+24` reproduce the Java
   interface's declaration order exactly (`RT_FADETOCONNECTION=1000`,
   `RELEASECONNECTION=1001`, `GETACTIVECONNECTION=1002`,
   `GETACTIVEENTERTAINMENTCONNECTION=1003`, `REQUESTCONNECTION=1004`,
   `SETVOLUMELOCK=1005`, `GETVOLUMELOCK=1006`).

This **confirms Codex's independent hypothesis** (fade=5, release=11, request=12).

> **Safe first call for the probe:** `getActiveEntertainmentConnection` = **ID 8**,
> one int arg, read-only — exactly the harmless call to try before ever sending
> `requestConnection`.

## 5b. The generated call/send shape (from `sub_9740`, verified)

A generated method body looks like this (decompiled, names ours):

```c
lock(this+36);                                  // per-object mutex
payload[0] = off_211E8;                         // vtable of the serializable payload
payload[1] = arg1; payload[2] = arg2;           // the call's arguments, inline
writer     = *(void**)(this+32);
entity     = *(uint16*)(this+16);               // object/entity id, stored at ctor time
method_id  = 18;                                // <-- IMMEDIATE, per method
...
SharedPtrDefaultBase(sp, payload, nullsub, 0);  // wrap payload
transport  = vslot[6] on *(this+8);             // get transport
send       = vslot[2] of transport;             // (*(t+8))
ret = send(transport, dest, sp, entity, method_id);
```

**Consequence:** the method ID is a literal immediate inside each generated method,
so the full ID map can be recovered mechanically by decompiling each method body
and reading the constant — no guessing. `sub_9740` uses **18**.

Codex's independent (Java-derived) hypothesis for the mapping is
`fadeToConnection→5`, `releaseConnection→11`, `requestConnection→12`. All three
fall inside the stub's switch set from §5, which is consistent — **but it is still
a hypothesis until read out of the native method bodies.**

## 5c. Architecture conclusion: the factory ships the SERVER halves

- entry 0 (0x30, `sub_43B4`) → "Created **RPCStub**" = receives requests (server).
- entry 1 (0x2C, `sub_73F4`) → "Created **RPCProxyReply**" = sends replies
  (server→client); `sub_9740` is one of its methods and matches its layout
  (`+8` transport holder, `+16` u16 entity, `+32` writer, `+36` mutex).

⇒ **Both factory-provided halves are server-side.** The *client* proxy that sends
`requestConnection` is generated into the **consumer's own binary** from the IDL —
it is not shipped in `libdsiaudioproxy.so`. So option (a) "just instantiate the
proxy from the factory" cannot, by itself, give us an outgoing call.

Remaining routes, in order of preference:
1. **Find a native consumer binary that already contains the client proxy** and use
   it as the reference implementation (best: real code, real ABI).
2. **Write the client proxy ourselves** against `comm::Proxy` + the transport send
   path — we now have the exact send shape above as a template, plus
   `comm::Proxy::Proxy(IdentityArgs const&, InterfaceStyle, LifecycleListener*)`.
3. Java bridge for focus only (rejected earlier; last resort).

## 5d. ✅ REFERENCE IMPLEMENTATION FOUND: `eso/bin/apps/dmdt` (34 KB)

The appimg has **846 binaries importing the client `comm::Proxy` ctor** — plenty of
native DSI clients. The best template is **`dmdt`** (the display-manager CLI we
already understand): tiny, standalone, and it does exactly "join the mesh → make a
proxy → call a method → exit".

### The native DSI client bootstrap (from `dmdt` `main` @ `0x106050`)

```c
ipl::ErrorStorage err(&ipl::ErrorStorage::action_dump_to_stdout);
osal::Osal        osal(/*a*/true, /*b*/false);
util::Util::init("DMRCClient", true, true, true, true);        // client name

comm::AgentStarter agent(ipl::string("DMRCClient"),            // <-- joins the DSI mesh
                         ipl::string("local"));                //     "local" = transport
comm::AgentStarter::start(&agent, /*AgentStarterEventReceiver*/nullptr, /*bool*/true);

make_proxy(obj);            // 0x103730
connect_proxy(obj);         // 0x10324C
do_command(cmd, obj);       // 0x105F48  -> the actual DSI calls

nanosleep(50 ms);           // calls are ASYNC — give the message time to leave
_Exit(0);
```
That `AgentStarter(name, "local")` + `start()` pair is **the missing "how does a
plain process get onto the DSI bus"** answer.

### Client proxy construction (`0x103730` → `0x103278`)

```c
obj+0x00 = vtable;  obj+0x04 = vtable2;
proxy_ctor(obj+8, obj, obj+4);        // comm::Proxy subobject at +8  (same as §4)
obj+0x14 mutex (MutexOps::init)
obj+0x1C condition (ConditionOps::init)
obj+0x28 SharedPtrDefaultBase
```
and inside `0x103278`:
```c
IdentityArgs ia = { &uuid_iface, &uuid_static, off_108620 };   // 3 ptrs — CONFIRMS §4
// build a comm::ServiceRegistration on the heap: operator new(0x6C)
//   base comm::LifecycleImpl, holds 2 UUIDs, refcounted at +0x40
comm::Proxy::Proxy(this, IdentityArgs const&, ServiceRegistration const&,
                   InterfaceStyle /*0*/, LifecycleListener*);
```
⚠ Note this is the **4-arg overload** (with `ServiceRegistration`), *different* from
the 3-arg overload the audio factory's server-side objects use. The 0x6C size
matches dio's note "createProxy builds the 0x6c registration RECORD" — independent
confirmation.

dmdt's UUIDs (for orientation): iface `C5E7062B-FF99-4456-805E-EDD3AAE4F0D8`,
static `685D22F6-1C44-5647-823C-F9530DB91C23`, registration
`F8F1844E-A4B7-410E-A4A6-59D39758B01D` / `FE953C10-6368-5E2F-A3B9-14744A155E77`.

### Bonus (display side): `dmdt`'s full command set, from its argv parser `0x105B98`
`gd`=1, `dc`=2, `sc`=3, `dm on`=4, `dm off`=5, `ts`=6, `sb`=7, `gc`=8, `gs`=9,
**`gld on`=10, `gld off`=11** (the last two are undocumented in its own help text).

### Ruled out: PPS
`/pps/services/audio/mixer` is touched **only by `deva-ctrl-qc.so`** and handles
only **BAL / FADE / BASS / TREB** tone controls (`echo "balance::100" >>`). It is
**not** a routing or focus API — dead end, checked and discarded. (It did confirm
`io-audio -d qc default_device=six_channel_speakerp` → the 6-channel default.)

### Ruled out: UUID-named factory libs
`lib_07302fa7-….so`, `lib_55183b6f-….so`, `lib_52b6f4b5-….so`, `lib_781e4762-….so`
are **byte-identical copies** of `libdsiaudioproxy.so` (same md5/size) — the registry
is just content-addressed by UUID. No extra code there.

### The complete verified client recipe (all four steps now known)

```c
/* 1. bootstrap — join the DSI mesh */
ipl::ErrorStorage err(&ipl::ErrorStorage::action_dump_to_stdout);
osal::Osal        osal(true, false);
util::Util::init("MyClient", true, true, true, true);
comm::AgentStarter agent(ipl::string("MyClient"), ipl::string("local"));
comm::AgentStarter::start(&agent, nullptr, true);

/* 2. build the client proxy  (dmdt 0x103730 / 0x103278) */
IdentityArgs ia = { &uuid_iface, &uuid_static, vtable_ptr };   // 3 pointers
ServiceRegistration *reg = new(0x6C) ...;                      // LifecycleImpl base, 2 UUIDs, refcount@+0x40
comm::Proxy::Proxy(obj+8, ia, *reg, /*InterfaceStyle*/0, /*listener*/nullptr);  // 4-ARG overload

/* 3. connect  (dmdt 0x10324C) */
comm::Proxy::connect(obj+8);                       // no args
(*(vslot[5] on *(obj+12)))(*(obj+12));

/* 4. call a method  (dmdt 0x102DA4 = "sc") */
sub = *(void**)( *(void**)(obj+12) + 4*( *(int*)(*(void**)(obj+12)+4) + 18 ) );
(*(fn**)(*(void**)sub + 20))(sub, arg1, arg2, /*method id*/ 123);   // vslot[5], id LAST
```

**Two independent confirmations of the call shape:** dmdt's `switchContext` passes
its method id (`123`) as the **last argument to virtual slot [5] (+20)**, and the
audio lib's `sub_9740` likewise ends in `send(transport, dest, payload, entity,
/*method id*/ 18)`. So: *generated call = virtual slot [5], method id is a literal
last argument.* That is the pattern to reproduce.

**Also note dmdt's method id is `123`, a three-digit value** — i.e. per-interface id
spaces differ; do not carry numbers across interfaces. For DSIAudioManagement the
candidate ids remain the stub-switch set of §5.

## 6. Open questions / next RE targets (priority order)

1. **`sub_5A68`** — map small method ID → method name via arg shape. (blocking)
2. Where does the **outgoing client Proxy** come from? Candidates: it is generated
   into the *client* binary from IDL (so no .so provides it) — in which case we
   must either reconstruct it, or drive `comm::Proxy` + the serializer directly.
   Check whether any *consumer* binary (e.g. the HMI/`audio` app) contains a
   `DSIAudioManagementProxy`, which would give a reference implementation.
3. `comm::traceCOMMProxy(IStreamSerializer&, u16, u16, u16, u32, transport::Writeable const&, Channel*)`
   — the `(u16,u16,u16,u32)` shape is a strong hint at the on-wire framing
   (service/object/method + call id?). Confirm against a real send path.
4. `libdsicommon.so`: `comm::Proxy::connect`@0x9e4f4, `comm::CoreApi::getInstance`
   @0x798e8 (AgentStarter singleton) — per `Patches/dio_manager-mhi2q` notes.

## 6a. ✅✅ PHASE 1 PASSED ON A LIVE STACK (QEMU, full comm mesh up)

Run in `Tools/qnx-gl-passthrough` QEMU against the real firmware libs, with
`broker` + `servicemgr` + persistence running. **0 checks failed.**

Confirmed at runtime (load base `0x78350000`, so runtime = static + base):

| item | static analysis | runtime | |
|---|---|---|---|
| `getProxyStubFactory` | `0x2998` | `78352998` | ✅ |
| factory table | `0x21A24` | `78371a24` | ✅ |
| `getProxyStubFactory(NULL)` | returns NULL | NULL | ✅ |
| `count` | 4 | 4 | ✅ |
| record stride | `0xBC` | all 4 names resolve | ✅ |
| creators rec0 | `sub_2B8C`/`sub_29C4` | `78352b8c`/`783529c4` | ✅ |
| `+0x14/+0x18` version | "2.11.45" | "2.11.45" | ✅ |
| `+0x1C` name, `+0x00/+0x20` UUIDs | — | byte-exact match | ✅ |

**The one thing the probe caught — and then solved.** Run 1 flagged NULL creators
on records 1/3 because the first version assumed a *fixed* creator offset taken
from record 0. Re-running with a **layout-discovering** probe (scan the record for
pointers into the module + hexdump) produced the ground truth:

```
record 0/2 (STUB)          record 1/3 (PROXY_REPLY)
+0x30: xxxxxxxx 0 0 0      +0x30: xxxxxxxx 0 create destroy
+0x40: create destroy 1 0  +0x40: 0 0 1 create
+0x50: 0 create destroy 2  +0x50: destroy 0 0 2
```

⇒ each record holds **two sub-records of 0x14 bytes**, at **+0x38** and **+0x4C**:

```
sub-record (0x14 bytes):
  +0x00  create   (PROXY_REPLY variant fills this pair)
  +0x04  destroy
  +0x08  create   (STUB variant fills this pair instead)
  +0x0C  destroy
  +0x10  role tag (1 for the first sub-record, 2 for the second)
```

Both pairs exist in every record; **which pair is populated tells you the role**.
Tags always sit at `+0x48` and `+0x5C` regardless of variant. That is why a
fixed-offset read worked for stubs and silently produced NULL for proxy-replies —
exactly the class of bug that would have been an indirect call through garbage in
production code. This is what the probe was for.

## 6b. The phase-1 probe

`probe/dsi_audio_probe.c` + `probe/build_probe.sh` → **10 445 B, `NEEDED` = only
`libc.so.3`**. Fully standalone: copy to the unit and run.

**It is deliberately zero-risk.** It only `dlopen()`s the stock library, calls the
single export `getProxyStubFactory`, and *reads* the table. It constructs no
`comm::` object and makes no virtual call, so a wrong layout guess can at worst
print a mismatch — it cannot corrupt anything. That is precisely why it comes
before phase 2.

What it proves (or refutes), in order:
1. the library **loads standalone** — i.e. its deps (`libdsicommon`/`libcomm`/
   `libosal`/`libutil`/`libipl`) resolve outside the HMI process. *If this fails,
   that alone blocks phase 2 and is the finding.*
2. `getProxyStubFactory(NULL) == NULL` — cheap calling-convention check.
3. `count == 4`, table pointer sane.
4. walking at **stride 0xBC**, each record's `name` lands exactly on
   `"dsi.audio.DSIAudioManagement"` or `"dsi.audio.DSISound"` — **the single
   strongest signal**: if the stride/offsets were wrong the pointer would not land
   on a known string. Also checks role tags are `1`/`2`, creator pointers non-NULL,
   and that both sub-records carry the same creator pair.
5. exactly **2** DSIAudioManagement records (stub + proxy-reply).

Exit code is non-zero on any mismatch, and it prints
`ABI MISMATCH - do NOT proceed to phase 2`.

**Phase 2 (only after phase 1 passes):** `osal::Osal(true,false)` →
`util::Util::init(name,1,1,1,1)` → `comm::AgentStarter(name,"local")` + `start()`
→ build the client proxy (4-arg `Proxy::Proxy`) → `connect()` → call
**`getActiveEntertainmentConnection` (wire ID 8, one int, read-only)** and only
then `requestConnection` (ID 12). Phase 2 *does* need the reconstructed `comm::`
headers from `Patches/dio_manager-mhi2q/from-source/harman-sdk/` and must link the
firmware's real `libdsicommon`/`libcomm`/`libosal`/`libutil` — which is exactly
the risky part phase 1 de-risks.

## 6c. ✅✅ PHASE 2a PASSED — a plain process CAN join the DSI mesh

`probe/dsi_bootstrap_probe.c`, run in the same QEMU (full comm stack up):

```
3) osal::Osal(true,false)              [ok] stayed inside its 1-byte model
4) util::Util::init("DMRCClient",1,1,1,1)  returned 0
5) ipl::string x2                      [ok] 12-byte model
   name: size=10 cap=10 data="DMRCClient"     <- {u32 size, u32 cap, char* data} PROVEN
   node: size=5  cap=5  data="local"
6) comm::AgentStarter(name,node)       [ok] stayed inside its 4-byte PIMPL model
   impl ptr = 114cf0
7) AgentStarter::start(NULL,true)      returned 1
=== phase 2a PASSED - process is on the DSI mesh ===   (exit status 0)
```

### Two decisions this validated

**1. Bind at runtime (dlopen+dlsym), not at link time.** The firmware framework
libs **cannot be link-bound** with the SDP's binutils 2.19: `readelf -h` on them
says *"no .dynamic section in the dynamic segment"*, and ld opens the file, sees
its SONAME + DT_HASH, and still resolves nothing — even though the wanted and
provided mangled names are byte-identical (`_ZN4osal4OsalC1Ebb`, FUNC GLOBAL, both
sides). So the probe `dlopen`s the five libs `RTLD_GLOBAL` and `dlsym`s the
mangled names. That is *not* the hand-copied-ABI mistake we rejected for hiddi:
a C++ mangled name encodes the whole signature, so **the symbol name is the
contract**; the only guessed values are three object sizes, and each object is
allocated with a poisoned guard tail that is checked after the call — an
undersized model prints `[SMASHED]` instead of corrupting the heap silently.
All three sizes held (Osal 1, ipl::string 12, AgentStarter 4).

**2. ⚠ The process must be REGISTERED in `framework.json` to join the bus.**
First run (as `"RAProbe"`) aborted — and importantly **not** with an ABI crash but
with the framework correctly refusing us:
```
AgentBase: could not map my own process name "RAProbe" to an id via configuration
The process "RAProbe" was not found in the configuration.   (DefaultConfigProvider.cxx:60)
```
`/config/framework.json` carries a per-process registry:
```json
{"name":"DMRCClient","exec":null,"node":"mmx","id":504,
 "transport":{"comm":{"mmx":{"resman":{"path":"dmrcclient"}},
                      "*":{"tcpip":{"port":21504}}}}}
```
Re-running as **`DMRCClient`** (id 504 — the stock `dmdt` debug identity) passed,
which proved the gate was purely the name lookup.

**We then gave RetroArch its own identity** (editing `framework.json` is fine per
the project owner), which is cleaner than squatting on a debug tool's slot:

```json
{"name": "RetroArch","exec": null,"node": "mmx", "id" : 520,
 "transport": {"comm":{ "mmx":{"resman":{"path":"retroarch"}},
                        "*":{ "tcpip":{"port":21520}} }}}
```
Chosen from the registry's own conventions: 124 entries, ids 1–516 in use, ports
`21000 + id`. **520 / 21520 were free.** Re-ran and it passed under `"RetroArch"`
(`ipl::string` reported `size=9 cap=9 data="RetroArch"`, AgentStarter built,
`start()` → 1, exit 0).

⚠ `framework.json` is **not strict JSON** — it carries `#` comments (the framework's
parser accepts them). Don't round-trip it through a strict JSON writer; edit it
textually and validate by stripping `^\s*#` lines first, or you will silently
reformat/destroy the file that every service on the unit reads at boot.

Cosmetic gap: `dmdt` constructs an `ipl::ErrorStorage` first; we skip it, so the
framework prints "YOU MUST DEFINE AN ERRORSTORAGE" and leaks 2328 B per thread.
Harmless for a probe, but add it for the real client.

## 6d. Phase 2b design — the two blockers are gone

**Blocker 1 (ServiceRegistration) — avoidable.** dmdt builds a 0x6C
`comm::ServiceRegistration` inline, with vtables that live in *dmdt's own binary*
(a generated subclass), and `libcomm` exports **nothing** for that class — so
replicating it would mean reconstructing those vtable slots too. But `libcomm`
exports **three** substantive `Proxy` ctor overloads:

```
comm::Proxy::Proxy(IdentityArgs const&, InterfaceStyle, LifecycleListener*)              <- 3-arg, NO registration
comm::Proxy::Proxy(IdentityArgs const&, util::SharedPtr<IActiveObjectFactory> const&, InterfaceStyle, LifecycleListener*)
comm::Proxy::Proxy(IdentityArgs const&, ServiceRegistration const&, InterfaceStyle, LifecycleListener*)   <- dmdt's
```
The **3-arg overload needs no ServiceRegistration at all** — and it is the one the
audio factory's own objects use (`sub_43B4`). That sidesteps the whole 0x6C
reconstruction.

**Blocker 2 (IdentityArgs' third field) — identified.** In `sub_43B4` the args are
built as `{ &uuid_instance, &uuid_static, off_219B8 }`. `off_219B8` turned out to
be a **table of interface-name pointers**:
`0x1FCF4 → "dsi.audio.DSIAudioManagement"`, `0x1FD14 → "dsi.audio.DSISound"`. So:

```c
struct IdentityArgs {          /* 3 pointers, confirmed twice (audio lib + dmdt) */
    const void  *uuid_instance;   /* ipl::UUID, 20 bytes */
    const void  *uuid_static;     /* ipl::UUID, 20 bytes */
    const char **iface_names;     /* interface-name string table */
};
```

**What is still genuinely missing for a *calling* client — now pinned down.**
The outgoing call in `sub_9740` resolves its transport like this:

```c
holder   = *(void**)(this + 8);                 /* NOT comm::Proxy - see below   */
entity   = vslot[4](holder);                    /* (*(*(this+8) + 16))(...)      */
transport= vslot[6](holder);                    /* (*(*(this+8) + 24))(...)      */
send     = *(fn**)(*transport + 8);             /* transport vslot[2]            */
send(transport, dest, payload, *(u16*)(this+16), /*method id*/ 18);
```

`this+8` is **inside the `util::SharedPtrDefaultBase` that the generated ctor
builds at `this+4`** (`sub_73F4`: `SharedPtrDefaultBase(this+4, a3)` — copied from
an argument). So the object that owns the transport is **handed to the generated
proxy by whoever creates it**, not produced by `comm::Proxy`. (Note the recon
models `util::SharedPtr` as a single 4-byte control pointer; here the
`SharedPtrDefaultBase` at `+4` clearly spans `+4..+0x0C`, so that model needs
widening for this variant.)

### `comm::Proxy::connect()` decoded (libcomm @0x9e4f4) — the binding is the *core's* job

```c
int Proxy::connect(Proxy *this) {
    state = this->m_storage;                 /* +0x04; if NULL ->
                                                "Missing state object for proxy.
                                                 Default constructed?"            */
    if (vslot[2](state)) return 0;           /* already connected                 */
    if (vslot[3](state)) -> error 1872
    core = comm::CoreApi::getInstance();     /* singleton, planted by AgentStarter */
    fn   = *(*(void**)core + 20);            /* CoreApi vslot[5]                   */
    Proxy copy(this);                        /* passes a COPY of the proxy         */
    return fn(core, &copy);
}
```

Two things follow, and they simplify 2b a lot:

1. **`connect()` needs nothing but a correctly-constructed `Proxy`.** Service
   resolution and transport wiring happen inside `CoreApi::vslot[5]` — the core
   does it, we don't. (Matches dio's note about `AgentStarter->vtbl[+0x14]`:
   0x14 = 20 = slot 5.)
2. **`CoreApi::getInstance()` is valid for us already** — its singleton is planted
   by `AgentStarter`, and phase 2a proved our AgentStarter construction + `start()`
   succeed. It asserts *"no AgentStarter instance?"* when unset, so a live
   instance is exactly what our bootstrap produces.

### ✅✅ PHASE 2b-i PASSED — a client Proxy constructs and connects

`probe/dsi_proxy_probe.c`, run on the live stack (output archived at
`probe/results/phase2b_proxy_connect.txt`):

```
comm::Proxy(IdentityArgs, style=0, listener=NULL)
   [ ok ] comm::Proxy stayed inside its 12-byte model
   proxy words: [0]=780d7310  [1]=109480 (m_storage)  [2]=780d7334
   connect() returned 0        <- success
```

…and identically for **both** UUID pairings. What this establishes:

- The **3-arg `Proxy` ctor works** with our reconstructed `IdentityArgs`
  (`{&{uuid,int}, &uuid_static, iface_name_table}`) — no `ServiceRegistration`
  needed, confirming the shortcut over dmdt's 4-arg path.
- `m_storage` (+0x04) came back **non-NULL**, so the ctor really took (a NULL there
  is precisely what makes `connect()` report *"Missing state object for proxy"*).
- The recon's **12-byte Proxy size holds** (guard tail intact), and the secondary
  vptr at +0x08 is populated as the recon predicted.
- **`connect()` returned 0.**

Honest caveat at the time: `connect()` returns 0 both for a fresh bind *and* via the
`if (vslot[2](state)) return 0;` short-circuit — **now resolved, see 2b-ii.**

### ✅ PHASE 2b-ii — the connected proxy dissected (read-only)

`probe/dsi_state_probe.c` dumps the live objects. It reads through a
**SIGSEGV/SIGBUS + `siglongjmp` trap** rather than guessing an address window (the
first version computed an inverted range from `min(stack, libcomm) - 32 MB` and
rejected everything — this process's mappings are scattered: stack ~`0x000f_xxxx`,
heap ~`0x0010_xxxx`, libs ~`0x780x_xxxx`). Runtime→file offset: **libcomm loads at
`0x78010000`** (`Proxy::connect` 780ae4f4 − file 0x9e4f4).

```
proxy @ ffda8:  +0x00=780d7310 (vptr)  +0x04=00109480 (m_storage)
                +0x08=780d7334 (secondary vptr)   +0x0c=0   <- 12-byte size confirmed
proxy vtable: slot[0..6] real, slot[7]=fffffff8  <- -8 offset-to-top => the
              SECONDARY vtable starts at slot[7]; the primary has 7 entries.

m_storage @ 109480:  +0x00=780d72f0 (vptr)   +0x04=00000000  <- lifecycle state
   +0x20..0x2c = 07302fa7 5815ae8e d833b570 affe0c9f   <- OUR UUID, stored ✓
   +0x34/+0x38 = 000ffd70 / 000ffd3c                   <- our IdentityArgs
```

**`m_storage` is a `comm::ProxyTracked`** (vtable resolved against libcomm):
`[0]/[1] ~ProxyTracked D1/D0`, **`[2] LifecycleImpl::isAlive`**,
**`[3] LifecycleImpl::isDead`**, `[4]/[5]` waitUntilDead/waitUntilAlive.

**This settles the 2b-i caveat.** The two guards are trivial:

```c
LifecycleImpl::isAlive() { return *(int*)(this+4) == 1; }
LifecycleImpl::isDead()  { return *(int*)(this+4) == 2; }
```
and the dump shows **state = 0**, so *neither* fired: `connect()` did **not**
short-circuit — it went down the `CoreApi::getInstance()->vslot[5](proxy)` path and
performed a real bind, returning 0 from it.

**And the binding is asynchronous.** State is still 0 *after* `connect()`, i.e. the
proxy is not "alive" yet — which is exactly why `waitUntilAlive()` exists.
Lifecycle encoding: **0 = pending, 1 = alive, 2 = dead**.

### ✅✅✅ 2b-iii — the FRAMEWORK ITSELF confirms our proxy is real

No extra probe was needed: the boot log already answers it, and the answer comes
from the framework's own agent, not from our code:

```
commAgent    comm.agent.Agent      warn  service N/A: dropped proxy=
                                         ['dsi.audio.DSIAudioManagement'=07302fa7-ae8e-5815-b570-…
commNotify   dsi.adapter.Provider  warn  Proxy error, try (fast) reconnection number 1:
                                         proxy instanceId=['dsi.audio.DSIAudi…
```

**`07302fa7-ae8e-5815-b570` is exactly the UUID our probe passed (pair A).** So:

1. **Independent, third-party confirmation that the client proxy is genuine.** The
   comm agent has our proxy in its registry, knows it by interface name *and* by
   our UUID, and is actively managing it — retrying reconnection on a ~5-minute
   cadence (00:06:18, 00:11:23, 00:16:28, …). Nothing about that could happen if
   our `IdentityArgs` or the ctor/`connect()` sequence were wrong. This is much
   stronger evidence than our own probe's "connect() returned 0".
2. **The provider is simply absent here:** *"service N/A"*. The
   DSIAudioManagement **service** is not running in this QEMU image (the
   `AudioService` hits in the log are the Java HMI's `NullHMIAudioService`
   complaints — *"No OSGi service 'HMIAudioService' registered!"* — not a DSI
   provider). Hence the lifecycle state stays **0 = pending** forever; it can never
   go alive in this boot, and polling `isAlive` would only have confirmed that.

⇒ **The client side of 2b is DONE and verified.** What remains before an actual
`getActiveEntertainmentConnection` (ID 8) is not more reconstruction but a **live
provider**: either stage the real audio service into the QEMU image (io-audio +
whatever hosts `dsi.audio.DSIAudioManagement`), or run the probe on the head unit,
where the service is running by definition.

**Ordering constraint discovered by the dmdt test:** a native client can join the
bus early, but a *call* only works once the target service is registered.
`dmdt gc` run before the HMI came up **blocked forever** (only
`DisplayManagerSupervision` was registered at that point) and stalled the boot
script. So any 2b probe must run **after** the audio service is up — and should
never be placed inline in the boot script without a timeout.

## 6e. Standing up our own stub provider in QEMU — dio already did the hard part

The real provider is **not stageable**: `dsi.audio.DSIAudioManagement` is served by
**`AudioProcess`**, which lives on the **RCC**, not the MMX. Its `NEEDED` list is a
different world — `libsys_colibry*`, `libsys_dsi_colibry`,
`libsys_dsi_servicebroker`, `libSysMoCCAFrameworkSharedSo`, `libdspipc` /
`liba2itodspipc` (audio DSP IPC), `libwdgadapter`/`libheartbeat`. So it is a
**different framework on a different processor talking to real audio hardware** —
which is also why DSI is configured with tcpip transport and per-node ids
(`"node":"mmx"` + ports in `framework.json`). Dropping it into our MMX image is not
an option. In our QEMU only **persistence** (`AttributesService`) actually
registers; everything else logs `service N/A` (1403 times).

**But a stub provider is now realistic**, because
`Patches/dio_manager-mhi2q/from-source/dio-src/CDSICarplayImpl.cpp` is a
*working reconstruction of a DSI service provider*:

```cpp
class CDSICarplayImpl : public dsi::ServiceProviderBase {          // base ctor is EXPORTED
    CDSICarplayImpl(CDIOManager* owner, unsigned serviceId)
      : dsi::ServiceProviderBase("DIO_DSICarPlay", serviceId) ...
    int registerService(void* agentCtx);   // sentinel-guarded, calls attachBroker
    int attachBroker(void* agentCtx);      // @0x1852a4 — the DSI-broker attach
};
```
and `attachBroker` is *implemented*, not just annotated:
- heap-allocate a **`comm::ServiceRegistration()`** (default ctor) and keep it alive
  for the object's lifetime (`m_serviceReg`, stock keeps it at `+0xd4`);
- build the service **UUID** with the real 10-arg `ipl::UUID` ctor;
- **`reconBuildRecord(svcUuid, devInfoSp)`** — their reconstruction of the **0x6C
  registration record** builder (@0x180858), installed as the holder's tracked ref.

That 0x6C record is exactly the thing we sidestepped on the client side by using the
3-arg `Proxy` ctor — and dio has already reversed and built it. Reuse it rather than
redo it.

**Plan for the QEMU loopback test:**
1. `dsi::ServiceProviderBase("RA_AudioMgmtStub", serviceId)` — exported ctor.
2. Register with `ServiceRegistration` + `reconBuildRecord`, using the
   **DSIAudioManagement UUID** (`07302FA7-…` / `9DEC172B-…` from §2) instead of
   CarPlay's.
3. Instantiate the **STUB** from the factory table (record 0's `create` —
   already mapped in phase 1) so requests get dispatched.
4. Run our existing client probe against it: the proxy should flip to
   **alive (state 1)**, then send `getActiveEntertainmentConnection` (**ID 8**) and
   watch it arrive in the stub — closing the loop on the send path *and* the wire-ID
   map, with no RCC and no audio hardware.

What this still cannot prove: actual audio routing/ducking — that needs the head
unit regardless.

### ⚠ 2c RESULT — the stub registers but does NOT publish (a known, documented wall)

`probe/dsi_stub_probe.c` implements the plan above. Everything *we* control worked:

```
dsi::ServiceProviderBase("RA_AudioMgmtStub", 0)   [ok] within its 36-byte model
0x6C registration record built                    [ok] vptr = TrackedBase+8
comm::ServiceRegistration::registerService()   -> 0   (accepted)
client connect()                                -> 0
lifecycle state at m_storage+4: 0,0,0 … for 20 s  <- NEVER reached 1 (alive)
```

And the framework keeps logging `service N/A` for DSIAudioManagement *after* our
registration — so nothing was published to the broker.

**Why, and why this is not a defect in our reconstruction.** dio's `attachBroker`
takes the **master-proxy path only when `agentCtx[0] && agentCtx[1]`**; their
committed code takes the *fallback* branch with `(void)agentCtx; // master proxy
not required`. That fallback builds a valid record — which is exactly what our
`registerService() -> 0` reflects — but it cannot publish. The dio project hit and
documented this same wall independently:

> *"the real eso providers **CANNOT run standalone offline** — `esoposprovider`
> execs then immediately dies with `servmngt::init() … the bundle loader
> commandline is invalid` / "Can only be started by the service manager": they are
> servicemgr-**SPAWNED BUNDLES** needing the bundle-loader + FrameworkProvider +
> instance-ID ecosystem the minimal mesh doesn't supply (running them =
> reproducing servicemgr's whole app-spawn subsystem = staging the entire live
> mesh)."*

That also explains the one service that *does* register in our image —
`persistence`/`AttributesService` — it is servicemgr-spawned.

⇒ **A standalone stub provider cannot make the client go alive in this offline
mesh.** The remaining option inside QEMU is to be **spawned by servicemgr as a
managed bundle**: our staged `servicemgr.json` has an empty `"startup":[]`, so
adding an entry there is the sanctioned mechanism — but it drags in the
bundle-loader/FrameworkProvider/instance-ID ecosystem, i.e. exactly the "stage the
whole live mesh" cost dio measured across many cycles.

### ✅ THE OFFLINE ANSWER — reuse `Tools/qnx-carplay-emu/host/kp_provider.cpp`

Codex review + a search of this repo turned up a **working precedent I had missed**:
`kp_provider.cpp` is a *native DSI provider* for `dsi.keypanel.DSIKeyPanel`, built
with exactly our technique (exported mangled symbols via `comm_abi.h` thunks) — and
it publishes. It encodes the details that make the difference:

```c
#define KP_HOST "servicemgr"  /* normal agent (id 102): valid CoreApi register(+28);
                                 broker's +28 is NULL  <- do NOT graft into the broker */
for (i=0;i<6000 && !comm_CoreApi_getInstance();i++) usleep(100000);   /* wait for CoreApi */
/* providerImpl=NULL -> impl[19]=0 -> register uses the agent's DefaultAOFactory */
rc = comm_ServiceRegistration_registerService(reg);
```
> *"replicate the generated ServiceRegistration construction (**RE'd from
> persistenceTNG sub_19B22C**)"* — i.e. the shape of a service that actually works.

**That is precisely why our 2c attempt registered but never published:** we built
dio's *fallback* record (the branch their own code marks `// master proxy not
required`), not the generated persistenceTNG shape — a 108-byte impl with specific
dword slots (`[13]`, `[19]` providerImpl SharedPtr, `[22]` type-UUID copy, SharedPtr
at +76). Same call, different record ⇒ accepted locally, never advertised.

**Concrete plan (highest value / effort ratio):**
1. Copy `kp_provider.cpp`'s registration construction verbatim; swap the keypanel
   UUIDs for **DSIAudioManagement** (`07302FA7-…` / `9DEC172B-…`).
2. Run it **inside `servicemgr`** via `LD_PRELOAD` (never the broker — its CoreApi
   `+28` is NULL), waiting for `CoreApi::getInstance()` before registering.
3. Attach the **STUB** from `libdsiaudioproxy.so`'s factory (record 0's `create`,
   mapped in phase 1) so requests dispatch.
4. Success criterion in order: our client proxy flips **pending → alive (state 1)**,
   then send **ID 8** and log its arrival at the stub — closing the send path and
   the wire-ID map offline.

Codex's verdicts on the alternatives, which match the evidence here: adding a
`servicemgr.json` `"startup"` entry is **not sufficient** (the bundle loader also
wants a service-manager-built command line, FrameworkProvider and instance-ID
context) and is a **dead end for audio** specifically; a second QEMU for the RCC is
a **rabbit hole** (platform bring-up, not client validation); a fake TCP peer is
**mostly a dead end** because same-node transport goes through resmgr and, with
`service N/A`, nothing is emitted anyway; `DSIAdmin` is likely introspection only.
And do **not** fabricate `agentCtx` — run inside a process that already owns it.

## 7. Rules for this reconstruction

- **Never hand-fake a layout that can be measured.** Every struct offset above came
  from a store in the disassembly; anything not yet measured stays unknown.
- Validate with `Patches/dio_manager-mhi2q/tools/struct-validate` before use.
- Wrong vtable index / layout on this target = silent memory corruption. Add
  `RECON_STATIC_ASSERT`-style size/offset asserts and a loud runtime sanity check
  (e.g. verify the record's name string equals `dsi.audio.DSIAudioManagement`
  before trusting the record).
- Reuse `Patches/dio_manager-mhi2q/from-source/harman-sdk/` (`comm/framework.h`,
  `ipl/`, `osal/`, `util/`) rather than starting a parallel recon.

---

# 8. Phase 2d — the native provider WORKS (2026-07-23)

`probe/dsi_audio_provider.cpp` + `probe/build_audio_provider.sh`.
LD_PRELOAD graft into `servicemgrmibhigh`, grafted alongside the existing
kp/ctul grafts at `merged_probe.build:60`.

## 8.1 Result: registered and published

Phase 2c registered but never published, because it used dio_manager's
*fallback* record (the branch dio's own source marks `// master proxy not
required`). 2d copies `qnx-carplay-emu/host/kp_provider.cpp`'s
persistenceTNG-shaped registration verbatim and swaps only the two UUIDs.
That was the whole difference. Boot log:

```
servicemgr  FW_COMM_Factories  loading factory lib /eso/lib/factories/lib_07302fa7-...so
servicemgr  FW_COMM_Services   on 102 using DefaultAOFactory for svc 07302fa7-...:0x00000000
servicemgr  FW_COMM_Services   on 102 registerSvc (local, instID=07302fa7-...:0x00000000,
                                                    key=9dec172b-a004-5427-...)
broker      FW_COMM_Services   on 100 agent 102 registers Service iid=07302fa7-..., key=9dec172b-...
broker      FW_COMM_Services   on 100 lookup for svc 07302fa7-... from 520, service is AVAILABLE
servicemgr  FW_COMM_Protocol   on 102 processing CREATE_STUB from 520, pid=102, iid=07302fa7-...
servicemgr  STUB_dsi_audio_DSIAudi..  Created RPCStub
servicemgr  FW_COMM_Services   on 102 adding stub with sid=5006 to service 07302fa7-...
```

`AVAILABLE` (2c got `UNAVAILABLE`) and a real `STUB_dsi_audio_DSIAudioManagement`
RPCStub = the service is genuinely on the mesh. Both UUIDs land where intended:
**07302FA7 = interface (instID), 9DEC172B = type (key)**, instance id 0.

The framework auto-loaded the real factory by UUID-derived filename, so the
provider's own `dlopen("libdsiaudioproxy.so")` is redundant on the server side.

## 8.2 Two build traps (both cost a boot)

1. **`libstdc++.so.6` is not in the image.** Linking the .cpp with `g++` puts it
   in `NEEDED`; the loader then silently skips the whole preload —
   `ldd:FATAL: Could not load library libstdc++.so.6`, zero `[au]` output, no
   other diagnostic. This is also why the neighbouring `[kp]`/`[ctul]` grafts
   emit nothing in this image. Fix: compile with `g++ -c -fno-exceptions
   -fno-rtti`, **link with `gcc -shared`**. We need no C++ runtime (calloc, no
   exceptions, hand-built vtables).
2. **Firmware libs have no `.dynamic`** and cannot be linked against. Generate
   stub `.so`s exporting the same mangled symbols under the same SONAMEs, link
   against those, let the runtime loader bind the real ones
   (`_build_in_container.sh`, lifted from `build_kp_provider.sh`).

## 8.3 Remaining gap: the CLIENT side, not the provider

The client (`dsi_state_probe`, agent 520 `RetroArch`) never reaches alive:

```
servicemgr  on 102 sending STUB_CREATED message to agent 520 with pid=102, sid=5006
dsi_state_probe:  Assert failed, src/comm/core/Core.cxx:1005, completeProxy()
                  Assert failed, src/comm/core/Core.cxx:1006, completeProxy()
Process 122898 (dsi_state_probe) exited status=2.
```

So the round trip gets all the way to `STUB_CREATED` and dies on OUR side while
completing the proxy. Two things were ruled out this run:

- **Not a timing artefact.** The first 2d run read `m_storage+0x04 == 0` at
  00:01:07.77 while `CREATE_STUB` only landed at 00:01:08.13 — `connect()` is
  async. The probe now polls the lifecycle word for 10s instead of snapshotting;
  it still never leaves 0, because the process asserts first.
- **Not the missing factory lib.** `dlopen("/eso/lib/factories/lib_07302fa7-...so")`
  before `connect()` reports `loaded` and changes nothing — same two asserts.

**Diagnosis:** `comm::Proxy` is a *base*. A real client instantiates the
**generated** proxy class produced by the factory, and that is what
`Core::completeProxy()` expects to find for `(pid, sid)`. We hand-built only the
base, so the completion has no generated RPCProxy to instantiate.

**Next step (phase 2e):** build the client proxy through the factory's `create`
entry (record 0 of `getProxyStubFactory`, whose layout phase 1 already mapped)
instead of calling `comm::Proxy`'s ctor directly, then re-run the alive poll and
send ID 8.

Note the provider's stub itself queues a lookup for
`55183b6f-9e82-5a18-bfde-ed5ae24f6c1c:0x00000000`, which stays unregistered —
that is the generated stub's own downstream dependency (the RCC-side peer) and
is expected to be absent offline. It does not block `CREATE_STUB`.

---

# 9. Phase 2e — the client proxy is ALIVE (2026-07-23)

`probe/dsi_alive_probe.c`. Result: **state 0 → 1**, `proxy_create` called once,
transport captured.

## 9.1 Why 2d asserted — read out of libcomm, not guessed

`Core::completeProxy` = libcomm `sub_218D4`. The two asserts that killed us:

```c
if (proxy.m_trackedState->m_interfaceStyle) { pc = rec+116; pd = rec+120; }
else                                        { pc = rec+56;  pd = rec+60;  }
if (!pc) doAssert("pc", Core.cxx, 1005);
if (!pd) doAssert("pd", Core.cxx, 1006);
...
impl = pc(&sid, transport, svcName, trackedState[14]);
ProxyTracked::update(trackedState, 1 /* ALIVE */, impl, pd);
```

(Line numbers decode from the `elf_hash_bucket[...]` args IDA mistypes:
`&elf_hash_bucket[211]+3` = 1003 `proxy.valid()`, 1004 `svc.valid()`,
**1005 `pc`**, **1006 `pd`**.)

`pc`/`pd` are the **client proxy's create/destroy**, read from the factory
record at **+0x38/+0x3C** for `interfaceStyle == 0` (what we pass). The live
table shows why they are NULL — and reveals the framework's convention:

```
rec[0] dsi.audio.DSIAudioManagement  proxy_create=0         stub_create=78352b8c
rec[1] dsi.audio.DSIAudioManagement  proxy_create=78352b30  stub_create=0
rec[2] dsi.audio.DSISound            proxy_create=0         stub_create=78352ad4
rec[3] dsi.audio.DSISound            proxy_create=78352a78  stub_create=0
```

**Per interface there are two records: one carries the stub creator (forward
direction, uuid 07302fa7), the other the proxy creator (reply direction, uuid
55183b6f).** The client proxy for the forward direction is generated into the
consumer binary and is genuinely not shipped — phase 1's conclusion, now
confirmed a second, independent way. (55183b6f is also the UUID our provider's
stub looks up and never finds: it is the reply service, not our missing proxy.)

## 9.2 Supplying pc/pd → ALIVE

The probe patches record 0's +0x38/+0x3C to its own create/destroy (patching the
UUID-named copy the framework itself loads, so both see one mapping), then
connects:

```
-> patching record @ 78371a24 ; pc/pd installed
>>> proxy_create CALLED  sid@fff3d4(=5007) transport=133f2c svcname=109524 a4=0x000ffd3c
t=   50ms state=1   <<<<< ALIVE
```

Our create only records its arguments and returns a heap block; that is enough
for `ProxyTracked::update` to flip the lifecycle. **This is a probe technique,
not a shipping one** — the real client must carry a generated proxy. What it
buys is the exact contract that proxy has to satisfy, plus a live transport.

## 9.3 The send path, now confirmed by name

Read-only dump inside `proxy_create` (fault-trapped, nothing called):

```
transport SharedPtr @ 133f2c:  +0x00=163f18  +0x04=1154b0  +0x08=1329f8(refcnt 5,2,1,1)
  +0x04 -> 1154b0  vtable 780d7388, and +0x10..+0x18 mirrors {163f18,1154b0,1329f8}
```

So **holder = SharedPtr+0x04**, matching the RE'd `holder = *(void**)(this+8)`
(SharedPtrDefaultBase starts at this+4). Following the chain through libcomm:

| step | value | libcomm | meaning |
|---|---|---|---|
| `holder->vslot[6]` | 780ae840 | `sub_9E840: return this+8` | transport = holder+8 |
| transport vptr | 780d7404 | — | (holder+0x08 in the dump) |
| `transport->vslot[2]` | 780af36c | `sub_9F36C -> sub_9F374(this-8)` | the send |

`sub_9F374` is `comm::RemoteConnection::send` — it carries the framework's own
log string, which names every parameter for us:

```
"on %d sending CALL_METHOD sid=%d, mid=%d to agent %d"
send(transport, payloadOffset, payloadSharedPtr, sid /*u16*/, methodId /*u16*/)
```

Exactly the shape RE'd from the generated proxy's `sub_9740`, now confirmed
against the transport implementation. Live values in hand: `transport` object
and `sid = 5007`.

**Next (phase 2f):** build the payload for **ID 8**
(`getActiveEntertainmentConnection`, 1 int, read-only) — the only remaining
unknown is the marshalling format, recoverable from the stub's decoder
(`sub_5A68`) or the generated sender `sub_9740` — then call `send` and watch it
arrive at the provider's RPCStub.

---

# 10. Phase 2f — CALL_METHOD id 8 goes out over the live transport (2026-07-23)

`probe/dsi_send_probe.c`. Result: `RemoteConnection::send -> 0`, and the
provider's stub binds our client.

## 10.1 Where the stream comes from (the piece 2e was missing)

2e reached ALIVE with a blank heap block as the proxy impl — enough for the
lifecycle, useless for sending, because the generated sender serializes into a
stream held *inside* that impl. The shipped ctor `sub_73F4` (reached via record
1's `proxy_create` = `sub_2B30`) builds a 0x2C-byte impl:

```
[0x00] vptr                    [0x04..0x0C] SharedPtr(transport)  (holder at 0x08)
[0x10] u16 sid                 [0x14] int    [0x18] a5
[0x1C] = holder->vslot[5](holder)
[0x20] = [0x1C]->vslot[2]()    <- the stream sub_9740 writes into
[0x24] mutex
```

That machinery is direction-agnostic — it wraps whatever transport and sid
`completeProxy` hands it. So the probe calls the **shipped** creator with the
exact arguments it received and returns its object: the framework gets a real
impl (still ALIVE) and we get a real stream. Live result:

```
>>> shipped create -> impl=10baf8
impl +0x00=78371200(=off_21200 ✓) +0x08=1164b0(holder) +0x20=105bc0(stream)
t=50ms state=1  <<<<< ALIVE
```

## 10.2 The wire format

From the generated sender `sub_9740` + its writer `sub_6C38`:

```
msg = { vtable@0x211E8, arg1, arg2, stream@0x0C, u16 methodId@0x10,
        int@0x14, u16 agentId@0x18, u16 entityId@0x1A }
stream->vslot[5]()                   begin
stream->vslot[12](stream, arg)       +48 = write one int32, once per argument
stream[4]                            error flag (byte)
size = stream->vslot[6]()            finish -> byte count
send(transport, size, msgSharedPtr, sid, methodId)
```

So `RemoteConnection::send`'s 2nd argument is the **payload size**, not an
offset — the bytes themselves were already written into the connection's buffer
by the stream, which is why the stream must come from the connection.

⚠ **Trap that cost one boot:** `0x211E8` **is** the vtable (its words are code
offsets 0x6d20/0x6f18/0x75bc), not a pointer to one. Dereferencing it hands the
send a garbage vptr and the process dies with no diagnostic. Take its address.

## 10.3 Result

```
6) send: mid=8 arg=0 sid=5008 agent=102 stream=105bc0 holder=1164b0
   marshalled size=4
   transport=1164b8 send=780af36c
   RemoteConnection::send -> 0
```

`size=4` for one int32; `transport = holder+8` and `send = sub_9F36C` exactly as
predicted in §9. Provider side, same boot:

```
servicemgr  on 102 received PROXY_ALIVE from 520
servicemgr  on 102 resolving PROXY_ALIVE for sid 5008
servicemgr  on 102 processing PROXY_ALIVE for stub with sid=5008, svc is 07302fa7-...
servicemgr  on 102 enq'ing ClientChange(connect) for Stub with sid=5008, svc is 07302fa7-...
```

i.e. our client is fully bound to the provider's real RPCStub. The log shows one
connect/disconnect cycle per probe (sid 5006, 5007, 5008).

## 10.4 What is NOT yet proven

**Arrival of the method at a handler.** Two reasons it is unobservable in this
run, neither of which indicates failure:

1. `"on %d sending CALL_METHOD sid=%d, mid=%d to agent %d"` is emitted at trace
   level 0, which is off in this image — its absence says nothing.
2. The provider registered with `providerImpl = NULL` (DefaultAOFactory), so the
   stub has no implementation object to dispatch into. Our own listener's
   connect hook does not fire either, for the same reason: the framework uses
   the shipped RPCStub, not our listener.

**Phase 2g:** give the provider a real implementation whose ID 8 handler logs,
then re-run this probe — that closes the loop end to end and validates the
wire-ID map against a live decoder. Everything needed for it is in place: the
provider publishes, the client binds, and the send is accepted.

---

# 11. Phase 2g — the dispatcher, decoded; runtime hook not yet landed

## 11.1 The handler map, read straight out of the dispatcher (this part is solid)

`libdsiaudioproxy sub_5A68` = `DSIAudioManagementRPCStub::process`. Every case
routes through **`(*(**(stub+4) + N))(*(stub+4), args..., stub+8)`** — so
**`stub[1]` is the provider implementation pointer**, and the stub ctor
`sub_43B4` initialises it to `0`. We registered with `providerImpl = NULL`
(agent DefaultAOFactory), so it stays 0; an incoming call would deref NULL.
Nothing had dispatched so far only because no call had a handler to reach.

| method id | impl vslot | arguments |
|---|---|---|
| 1  | +48 | none |
| 2  | +40 | array (deserialize) |
| 3  | +44 | 1 int |
| 5  | +0  | 2 int |
| 7  | +8  | 1 int |
| **8** | **+12** | **1 int** — getActiveEntertainmentConnection |
| 9  | +24 | 2 int |
| 11 | +4  | 2 int |
| **12** | **+16** | **3 int** — requestConnection |
| 14 | +36 | none |
| 15 | +28 | array |
| 16 | +32 | 1 int |
| **17** | **+20** | **2 int + bool** — setVolumeLock |
| 24 | +52 | 2 arrays |

Unknown ids fall through to
`"DSIAudioManagementRPCStub::process unknown method ID=%d"`. Every handler's
last argument is `stub+8`, the stub's own reply proxy. **This independently
re-derives the phase-1 wire-ID map** — 12 takes three ints, 17 takes two ints
and a bool, 8 takes one int — from the decoder rather than from argument-shape
inference. Also note the deserializer reads ints via `stream->vslot[11]` (+44)
and bytes via `+24`, the mirror of the writer's `+48` found in §10.

## 11.2 What did not work, and what it rules out

The provider now builds an implementation object (14-slot vtable, slot 3 logs
ID 8) and tries to plant it by wrapping the factory record's stub creator —
the same table-patch technique that worked for the client's `pc`/`pd`. Across
four boots:

1. Patch `rec[0]+0x40` → hook installed, never called; shipped ctor still ran.
2. Patch every copy in `0x38..0x58` → found 2 (at +0x40 and +0x54), same result.
3. Scan the **whole 0xBC record** → still exactly 2 copies, same result.
4. Patch under **all four name forms** the lib can be opened by → all resolve to
   **the same table** (`table=784a1a24` for every name; only the dlopen handles
   differ). Shipped ctor still ran.

Step 4 kills the obvious explanation: it is **not** two independent mappings.
The ordering is right too — our hook logs before the framework's
`loading factory lib /eso/lib/factories/lib_07302fa7-….so`. So the framework
must snapshot the creator into its own factory registry rather than reading the
record at CREATE_STUB time.

**Resolution of the hook (boot 8):** the wrapper *does* run once the hook calls
`sub_43B4` directly (`libbase + 0x43B4`) instead of the table's old pointer:

```
[au] rec[0] shipped stub_create=78482b8c (libbase+0x2B8C)
[au] *** stub created 1fcd98 -> impl=7800d98c installed ***
```

Full record dump, for the record — the creator appears exactly twice and there
is no third copy hiding outside the window phase 1 scanned:

```
rec[0]+0x14 = libbase+0x1fcec   (version string)
rec[0]+0x18 = libbase+0x1fcec
rec[0]+0x1c = libbase+0x1fcf4   (interface name)
rec[0]+0x40 = libbase+0x02b8c   (stub create)
rec[0]+0x44 = libbase+0x029c4   (stub destroy)
rec[0]+0x54 = libbase+0x02b8c   (same create, second sub-record)
```

Also ruled out along the way: `mprotect` on the library's text is **denied on
QNX** (`mprotect(78482b8c) failed: Permission denied`), so an entry-point
trampoline is not available; and `sub_43B4` has exactly one caller, `sub_2B8C`,
so there is no second creator path.

## 11.3 Where it actually stands

The implementation is installed in the stub, but **the call still does not
arrive**. Provider-side timeline for our client's sid:

```
08.899  adding stub with sid=5008 to service 07302fa7-...
08.899  created stub for pid=102, sid=5008, no reply proxy
08.900  processing PROXY_ALIVE for sid=5008 ; enq'ing ClientChange(connect)
10.951  stub orphaned by lost connection with sid: 5008      <- probe exited
```

Nothing between 08.900 and 10.951: no CALL_METHOD, and no
`"unknown method ID"` either — so `process` never ran. `send` returned 0, which
only means the connection's error byte was clear after the write.

Two candidate explanations, both cheap to test next:

1. **`no reply proxy`.** The stub is created without one because 55183b6f (the
   reply-direction service) is unregistered offline. If the receive path
   requires it before dispatching, the call is dropped before `process`. Test:
   register a second provider for 55183b6f the same way as the first.
2. **Message header fields.** Our hand-built msg copies `int@0x14` from the
   borrowed impl and sets agentId/entityId; these feed `traceCOMMStub` and the
   routing. A wrong value here routes the message nowhere. Test: dump the same
   fields from a *working* generated call (the reply direction, id 18) and diff.

**Bottom line:** §11.1's handler map is read from the decoder and stands
regardless. What remains open is one hop — the receive side — not the whole
path: the client marshals, sends, and the transport accepts (§10); the provider
publishes, binds the client, creates a stub and now holds an implementation.

## 11.4 Reply service registered — necessary, but not what gates dispatch

Candidate (1) from §11.3 tested. The client now registers
`55183b6f-9e82-5a18-bfde-ed5ae24f6c1c` (type `db006bf0-acd2-5747-8415-366ba32075fe`,
both read out of the stub ctor `sub_43B4`) using the same persistenceTNG
registration shape as the provider, and plants a stub creator on rec[1] — the
mirror of the pc/pd trick, since rec[1] ships `proxy_create` but no
`stub_create`.

It works as a registration:

```
client:  reply: rec[1] stub creator planted
client:  reply service registerService -> 0
broker:  00:01:08.896 on 100 agent 520 registers Service iid=55183b6f-...
broker:  00:01:09.400 lookup for svc 55183b6f-...
```

**This is required work regardless** — replies to `requestConnection`
(RP_STARTCONNECTION / RP_PAUSECONNECTION / RP_STOPCONNECTION) come back over
this service, so a real client has to register it.

But it did **not** change the dispatch:

```
servicemgr: 00:01:09.400 created stub for pid=102, sid=5008, no reply proxy
client:     RemoteConnection::send -> 0
provider:   (no ID 8, no unmapped-handler, no "unknown method ID")
```

and our `reply_stub_create` was never called.

**What that rules in.** "Reply proxy" is not established by the reply service
merely existing. The earlier CREATE_STUB trace shows the field it comes from:
`sending CREATE_STUB with pid 101, reply-sid 65535 to agent 100` — 65535 = none,
and that value originates on the **client** side, which is also what
`completeProxy` reports as `proxies hasReplySvc()=%d`.

**Next lead (concrete):** `comm::InterfaceStyle`. We construct the proxy with
style **0**, and `completeProxy` branches on exactly that field:

```c
if (proxy.m_trackedState->m_interfaceStyle) { pc = rec+116; pd = rec+120; }
else                                        { pc = rec+56;  pd = rec+60;  }
```

A non-zero style selects a *different* creator pair — plausibly the
reply-capable variant, which would also explain why the record reserves a
second pair far outside the window phase 1 scanned. Test: enumerate the
`InterfaceStyle` values from libcomm, construct the proxy with the reply-capable
one, and see whether CREATE_STUB then carries a real reply-sid.

## 11.5 The reply direction needs the 4-arg Proxy ctor — found, and it works

`comm::Proxy::hasReplyService()` (libcomm 0x1e888) answers §11.4's question
outright:

```c
state = this[1];
if (state && *(state+112) && UUID@(state+108) != nil && (v5 = *(state+112)))
    return *(v5 + 72) != 0;
return 0;
```

The proxy must carry a **reply-service UUID at `state+108`** and a pointer to
its **ServiceRegistration at `state+112`**. Only the **4-arg ctor**
`Proxy(IdentityArgs const&, ServiceRegistration const&, InterfaceStyle,
LifecycleListener*)` fills them. Phase 2b deliberately took the 3-arg ctor
("ServiceRegistration is AVOIDABLE") — correct for *binding*, which is all we
needed then, but it is exactly why every CREATE_STUB carried `reply-sid 65535`
and every provider stub was built `no reply proxy`.

Switching the client to the 4-arg ctor, passing the reply registration from
§11.4, establishes the reply direction for real:

```
client: reply service registerService -> 0
client: using the 4-arg ctor with our reply registration 1569b8
client: state+108 uuid word=780d5438  state+112=10a480      <- both non-null now
client: connect() -> 0
client: >>> REPLY stub_create called -> 1039f0               <- framework built our reply stub
client: >>> REPLY stub slot called (a=1064032 b=1084072 c=1051580)
client: >>> REPLY stub slot called (a=1063408 b=0 c=1051580)
```

The framework now calls into our reply stub. A vtable of no-ops returning 0 is
not enough, though — the process still dies after those two calls, so at least
one of those slots must return a real object rather than 0.

## 11.6 Structural conclusion

The shipped factory contains **only the server-side halves** of each interface:

| record | uuid | what it ships | what it omits |
|---|---|---|---|
| rec[0] | 07302fa7 (forward) | stub create/destroy | the client **proxy** |
| rec[1] | 55183b6f (reply)   | proxy create/destroy | the client **stub** |

Both omissions are the same thing seen twice: **the client halves are
code-generated into each consumer binary**. A complete offline loopback
therefore needs both generated — the forward proxy and the reply stub — and the
probe's shortcuts (borrowing the shipped reply-proxy impl, planting no-op
creators) get the framework to *accept* a client but cannot survive being
called back into.

That is also precisely what a real RetroArch client needs on the head unit, and
we now have every contract it must satisfy:

- **registration** — persistenceTNG shape (§8), for the reply service too (§11.4)
- **proxy construction** — 4-arg ctor with the reply registration (§11.5)
- **client proxy create/destroy** — factory record +0x38/+0x3C, called by
  `completeProxy` as `pc(&sid, transport, svcName, trackedState[14])` (§9)
- **impl layout** — holder at +0x08, sid at +0x10, stream at +0x20 (§10)
- **marshalling** — `begin` / `write int32` per arg / `finish`→size, then
  `send(transport, size, msgSharedPtr, sid, methodId)` (§10)
- **dispatch contract** the reply stub must honour — the handler-slot map in
  §11.1, with `stub[1]` = implementation and `stub+8` = reply proxy

The one thing still unverified end-to-end offline is a method arriving at a
handler, and it is blocked on writing those two generated halves rather than on
any remaining unknown in the protocol.

## 11.7 Reply-stub call sequence, and the role question settled for good

With 32 distinct logging thunks in the reply stub's vtable, the framework's
first two calls are:

```
>>> REPLY stub_create called -> 104190 (vtable 104110)
>>> REPLY stub vslot[0] (+0x00)  self=104190 a=00104400 b=00109aa8 c=0010133c
>>> REPLY stub vslot[6] (+0x18)  self=fbd3a0 a=00104190 b=00000000 c=0010121c
```

Note the second: `self` is a stack address and the FIRST ARGUMENT is our stub
(`104190`) — so vslot[6] is invoked on a different object with our stub passed
in. Returning 0 from these is not survivable; the process still dies, so at
least one of them must hand back a real object.

**The role question is now settled three independent ways.** `libdsiaudioproxy`
carries all four generated channel-name strings —
`PROXY_`, `PROXY_REPLY_`, `STUB_` for both DSIAudioManagement and DSISound —
which made it look as though the client proxy might be in there after all. It is
not: `sub_73F4`'s object (rec[1], the one we borrowed in §10) has a 13-method
vtable at `off_21200`, and its **slot[0] (`sub_82AC`) sends method id 4 with
three arguments over the `PROXY_REPLY_dsi_audio_DSIAudioManagement` channel**,
exactly as slot[5] (`sub_9740`) sends id 18. So that vtable is the *server's
proxy back to the client*, and the four strings are just the generator emitting
names for all four roles while the library implements only two.

Nothing anywhere else in the image references `dsi.audio.DSIAudioManagement` or
`PROXY_dsi_audio_DSIAudioManagement` — the client halves exist only in whatever
consumer was compiled against the IDL, which on this platform is the RCC.

Useful by-product: the reply interface's own wire IDs are readable from that
same vtable (slot 0 → id 4, slot 5 → id 18, …), and those are how
`RP_STARTCONNECTION` / `RP_PAUSECONNECTION` / `RP_STOPCONNECTION` will arrive.
That set is the spec for the reply stub RetroArch has to implement.

---

# 12. Phase 2h — both halves written; delivery proven; dispatch has one guard left

## 12.1 The two generated halves now exist and work

**Client proxy half.** `pc`/`pd` planted on rec[0]+0x38/+0x3C; `pc` calls the
shipped ctor `sub_73F4` (via rec[1]'s creator) so the impl gets a real
holder/sid/stream, then we drive the send ourselves.

**Reply stub half.** rec[1]+0x40/+0x44 planted; the creator calls the shipped
stub ctor `sub_43B4` (libbase+0x43B4) and swaps only the vptr for a copy of
`off_210A8` with slot[1] replaced by our dispatcher. Two crashes were resolved
by reading the shipped vtable rather than guessing:

- `slot[0] sub_344C` is `setImplementation` — `stub[1] = arg`. The framework
  hands the implementation in; we had been planting it by hand.
- `slot[6] sub_3A1C` is `getReplyProxy`, **returning by value** — hence `self`
  looked like a stack address with our stub as the first argument. It
  copy-constructs from `stub+8`; a calloc'd object has garbage there.
- Then `Core::preCreateStub` asserted `"!stub->getReplyProxy().valid()"`
  (Core.cxx:1939): a **reply** stub must NOT carry a reply proxy, so after the
  shipped ctor we clear the embedded Proxy's `m_storage` at `stub+12`.

With both halves in place the provider finally reports what we were after:

```
servicemgr: created stub for pid=102, sid=5008, has reply proxy
```

(every earlier boot said `no reply proxy`).

## 12.2 Delivery is proven, and the message format was wrong for a reason

A control run using the shipped sender (`sub_9740`, id 18) produced

```
STUB_dsi_audio_DSIAudi.. error  DSIAudioManagementRPCStub::process unknown method ID=18
```

so the whole path works: client → transport → agent → stub → `process`.

That isolated our own message as the fault, and the cause was in the msg vtable.
`off_211E8` slot[2] (`sub_75BC`) is the **real** serializer, called by the
framework when the connection flushes — not our sizing pass:

```c
stream->vslot[3]()                      begin
stream->vslot[12](stream, msg[+0x04])   arg1
stream->vslot[12](stream, msg[+0x08])   arg2     <- fixed, per generated method
stream->vslot[4]()                      end
```

Borrowing method 18's vtable meant the flush emitted two ints while we had
declared a 4-byte payload. Replacing slot[2] with a serializer that writes
exactly `argc` arguments fixed it — our own hand-built send then reached the
dispatcher too (`unknown method ID=18` from *our* path, control removed).

## 12.3 What is left: one guard inside the dispatcher

Tracing the provider's `process` (wrap `off_210A8` slot[1]) and giving the
implementation 16 individually-logging slots plus a call counter:

```
>>> process(stub=1fce08, methodId=18, msg=ca65bc) impl=7800e04c vtbl=7800df8c
<<< process(methodId=18) -> 0   implCalls 0->0
>>> process(stub=1fce08, methodId=7,  ...)  <<< -> 0   implCalls 0->0
>>> process(stub=1fce08, methodId=8,  ...)  <<< -> 0   implCalls 0->0
```

All three arrive; the implementation pointer and its vtable are ours; nothing
dispatches; everything returns 0.

That contradicts the pseudocode, which allows only two outcomes for case 7/8:
a deserialize error (`goto LABEL_10` → **return 1**) or the implementation call.
Returning 0 with neither means Hex-Rays folded a guard that the real code has.

## 12.4 Ghidra cross-check: no hidden guard — the instrumentation was lying

Imported into Ghidra (`ARM:LE:32:v8`, image_base **0x10000**, so `sub_5A68` is at
**0x15a68**; Ghidra's auto-analysis creates no function there because the only
reference is a vtable entry, so `create_function` first).

**Ghidra agrees with Hex-Rays.** Case 8 is:

```c
case 8:
  (**(code **)(*piVar8 + 0x2c))(piVar8,&uStack_9c);          // read one int
  if (*(char *)(*(int *)(param_1 + 0x24) + 4) == '\0') {     // no deserialize error
      ... two probe blocks ...
      (**(code **)(**(int **)(param_1 + 4) + 0xc))           // impl->vtable[3]
                (*(int **)(param_1 + 4), uStack_9c, param_1 + 8);
      uVar5 = *(undefined1 *)(*(int *)(param_1 + 0x24) + 4);
      goto LAB_00015c64;
  }
  goto code_r0x00015ca4;                                     // uVar5 = 1
```

The implementation call is unconditional once the read succeeds. No folded guard.

Three things the cleaner rendering did give us:

1. **`case 0x12: break;`** — id 18 falls through to `default`, which is why the
   control run logged `unknown method ID=18`. Confirmed, not inferred.
2. The default path has a **silent branch**:
   `if (channel->level < 5) { log("unknown method ID=%d"); ... } else uVar5 = 0;`
   — a route that returns 0 leaving no trace at all.
3. **`process` returns `undefined1`, a BYTE.** Our tracer declared it
   int-returning, so the upper bits of r0 were undefined and every earlier
   "`-> 0`" was not evidence of anything. Fixed to `unsigned char` + mask.

## 12.5 Where it actually stops (measured, not inferred)

With the byte return fixed, the deserializer's error flag logged either side of
the call, and the deserializer's own "read int" (its vtable slot 11, +0x2c)
wrapped:

```
>>> process(mid=18) impl=7800e2cc vtbl=7800e20c deser=1fcd28 err=0
<<< process(mid=18) -> 0  implCalls 0->0  readCalls=0  deserErr 0->0
>>> process(mid=7)  ...
    read_int -> 0 (err=0)
<<< process(mid=7)  -> 0  implCalls 0->0  readCalls=1  deserErr 0->0
>>> process(mid=8)  ...
    read_int -> 0 (err=0)
<<< process(mid=8)  -> 0  implCalls 0->0  readCalls=2  deserErr 0->0
```

So, precisely:

- id 18 performs **no read** — it took `default`, exactly as the source says.
- ids 7 and 8 **do take their cases**: each performs one read, and the value
  read is **0** — the argument we sent — with the error flag clear.
- The only missing step is the final `impl->vtable[N]` call.

That is a far tighter statement than §12.3's. The unexplained region has shrunk
from "the whole dispatcher" to the handful of instructions between the argument
read and the implementation call — the two probe blocks
(`sub_3A54` / `sub_3D4C` on a static string, each guarded by a NULL check).

## 12.6 The last hop, and the actual bug

Disassembling the tail of case 8 (`0x1635c..0x16374`, found by searching the
program for `ldr r3,[r3,#0xc]` after forcing the case blocks to disassemble)
settles the code question for good — the call is unconditional:

```asm
0001635c  ldr r3, [r5,#0x4]      ; r3 = stub[1] = implementation
00016360  add r2, r5, #0x8       ; arg3 = stub+8 (the reply proxy)
00016364  ldr r1, [sp,#0x1dc]    ; arg2 = the int just read
00016368  cpy r0, r3             ; this = implementation
0001636c  ldr r3, [r3,#0x0]      ; its vtable
00016370  ldr r3, [r3,#0xc]      ; slot 3
00016374  blx r3
```

So the call *was* executing. Resolving that exact chain at dispatch time and
comparing it against our own symbols — which we had never actually done, having
only ever printed the vtable read out of the implementation — showed why nothing
appeared to happen:

```
[au] *** stub created ... impl=7800e334 (&g_impl_obj=7800e334 MATCH)     <- at creation
[au] >>> process(mid=8)
[au]     impl  =7800e488  (&g_impl_obj =7800e334  MISMATCH)              <- at dispatch
[au]     ivt   =7800e3c8  (&g_impl_vtbl=7800e344  MISMATCH)
[au]     ivt[3]=7800bbac  (&au_slot_03 =7800bc64  MISMATCH)
```

**`stub[1]` is overwritten between creation and dispatch.** The framework calls
`setImplementation` (stub vtable slot[0], `sub_344C`) with the object its
ActiveObjectFactory produced — we registered with `providerImpl = NULL`, so the
agent's DefaultAOFactory supplies one. Every "no dispatch" result in §11–§12 was
us watching an object nobody calls.

## 12.7 Round trip closed

Re-planting our implementation immediately before the dispatcher runs:

```
[au]     re-planting impl 7800e4dc -> 7800e388
[au] *** impl vslot[2] (+0x08) self=7800e388 a=00000000 b=001fce10          <- id 7
[au] *** impl vslot[3] (+0x0c) self=7800e388 a=00000000 b=001fce10   <<<<< ID 8 ARRIVED
[au] <<< process(mid=18) implCalls 0->0     (default, no handler — correct)
[au] <<< process(mid=7)  implCalls 0->1
[au] <<< process(mid=8)  implCalls 1->2
```

`getActiveEntertainmentConnection` reaches a handler at slot **+0x0c**, exactly
where §11.1's map (read from the decoder) said it would; the argument is the `0`
we sent and the last parameter is `stub+8`, the reply proxy. id 7 lands in its
own slot +0x08 and id 18 falls to `default`. **The handler map is now verified
live, not merely read out of the code.**

The full chain works offline, with no RCC and no head unit: register the service
and its reply counterpart → client binds with the 4-arg ctor → marshal → send →
transport → agent → stub → dispatcher → handler.

**For the shipping client, do not re-plant.** The correct fix is to supply a real
`providerImpl` in the registration record (the SharedPtr at impl[19..21], §8) so
the AOFactory installs *our* object in the first place. The re-plant is a probe
shortcut that documents precisely what that object must be.

---

# 13. Historical native-client recipe (not shipped)

Everything below is verified on a live stack in QEMU (no RCC, no head unit).
Section references point at the evidence. It remains valuable as RE/probe
documentation, but the current build deliberately avoids this process-level
framework registration path.

## 13.1 One-time setup

1. **Historical native requirement: `/config/framework.json`.** A standalone
   native DSI client needs its own registry entry, or the
   framework refuses the process: `{"name":"RetroArch","exec":null,"node":"mmx",
   "id":520,"transport":{"comm":{"mmx":{"resman":{"path":"retroarch"}},
   "*":{"tcpip":{"port":21520}}}}}`. ⚠ the file has `#` comments — edit textually,
   never round-trip through a strict JSON writer.
2. **Bring-up:** `osal::Osal(true,false)` → `util::Util::init(name,1,1,1,1)` →
   `comm::AgentStarter(ipl::string(name), ipl::string("local"))` → `start()`.
   Bind everything with dlopen/dlsym: the firmware libs have no `.dynamic` and
   cannot be link-bound (§2a). Build an `ipl::ErrorStorage` too, or every thread
   leaks 2328 B and logs a warning.

## 13.2 Registration (both directions)

Use the persistenceTNG shape (§8) — 108-byte impl, LifecycleImpl vptr kept,
interface UUID at +28, instance id 0 at [12], AOFactory at [14], refcount at
[16], listener at [18], **providerImpl SharedPtr at [19..21]**, type UUID at +88,
[13] → it; wrapped in the 3-word handle.

- **Reply service** `55183b6f-9e82-5a18-bfde-ed5ae24f6c1c` /
  `db006bf0-acd2-5747-8415-366ba32075fe` — register it, and supply a stub creator
  on factory record 1 (the shipped lib has none). Replies
  (RP_STARTCONNECTION/PAUSE/STOP) arrive here (§11.4).
- ⚠ **Fill `providerImpl`**, do not leave it NULL: otherwise the AOFactory
  installs its own object over `stub[1]` and none of your handlers ever run
  (§12.6 — this cost the most time of anything here).

## 13.3 Client proxy

- Plant create/destroy on factory record 0 at **+0x38/+0x3C**; `completeProxy`
  calls `pc(&sid, transport, svcName, trackedState[14])` (§9).
- Build the impl like the shipped ctor `sub_73F4`: SharedPtr(transport) at +0x04
  (holder at +0x08), u16 sid at +0x10, `holder->vslot[5]()` at +0x1C, and
  **stream = that->vslot[2]() at +0x20** (§10).
- Construct the proxy with the **4-arg ctor** `Proxy(IdentityArgs const&,
  ServiceRegistration const&, InterfaceStyle, LifecycleListener*)`, passing the
  reply registration — the 3-arg one binds but leaves `reply-sid 65535` and no
  reply proxy (§11.5).

## 13.4 Sending

```
msg = { serializerVtable, arg1, arg2, stream@0x0C,
        u16 methodId@0x10, int@0x14, u16 agentId@0x18, u16 entityId@0x1A }
sizing:   stream->vslot[5]() ; stream->vslot[12](stream,arg) per arg ; size = stream->vslot[6]()
flush:    the framework calls msg->vtable[2] — YOUR serializer, which must write
          exactly this method's argument count:
              stream->vslot[3]() ; stream->vslot[12](...) per arg ; stream->vslot[4]()
send:     transport = holder->vslot[6](holder)          // == holder+8
          transport->vptr[2](transport, size, msgSharedPtr, sid, methodId)
```

⚠ The serializer is per-method: borrowing another method's vtable emits the
wrong argument count and the message is silently dropped (§12.2).

## 13.5 Wire IDs (verified live)

Forward interface, `methodId` → implementation vtable slot:

| id | slot | args | method |
|---|---|---|---|
| 7  | +0x08 | 1 int | getActiveConnection |
| **8** | **+0x0c** | **1 int** | **getActiveEntertainmentConnection** |
| 9  | +0x18 | 2 int | getVolumeLock |
| 11 | +0x04 | 2 int | releaseConnection |
| **12** | **+0x10** | **3 int** | **requestConnection** |
| 16 | +0x20 | 1 int | — |
| 17 | +0x14 | 2 int + bool | setVolumeLock |
| 5  | +0x00 | 2 int | fadeToConnection |
| 1 / 14 | +0x30 / +0x24 | none | — |
| 2 / 15 | +0x28 / +0x1c | array | — |
| 3  | +0x2c | 1 int | — |
| 24 | +0x34 | 2 arrays | — |

Every handler's last argument is `stub+8`, the reply proxy. Unknown ids log
`DSIAudioManagementRPCStub::process unknown method ID=%d`. The reply interface's
own ids are readable the same way from the shipped PROXY_REPLY vtable
`off_21200` (slot 0 → id 4, slot 5 → id 18, 13 slots) (§11.7).

## 13.6 What RetroArch actually calls

`requestConnection` (id 12, three ints) for `CL_ENT_AMP_MEDIA_MFP` (20), then
write PCM to `/dev/snd/mpl1_int_ent` via the existing QSA driver, and handle the
replies as lifecycle events (RP_STARTCONNECTION = may play, RP_PAUSECONNECTION =
pause, RP_STOPCONNECTION = stop). Never touch `MS_ENT`.

---

# 14. Historical native prototype (2026-07-23)

`src/audio/dsi_audio_client.{c,h}` — the reconstruction of §13 written as a real
prototype module. It used no re-planting or per-call patching, but is no longer
included from `griffin/griffin.c` or driven by the shipping QSA audio driver.

## 14.1 What changed from the probes

- **`setImplementation` instead of re-planting.** The stub vtable copy's slot 0
  is ours and installs our implementation, ignoring the object the AOFactory
  offers. One place, once per stub, no per-dispatch work (§12.6 was the bug).
- **Message layout follows the argument count.** The generated senders put the
  stream pointer immediately after the arguments — 2-arg methods at +0x0C,
  3-arg at +0x10 — and the method id, agent id and entity id follow it. The
  probe hard-coded the 2-argument case, which would have silently broken
  `requestConnection`. Now derived: `MSG_STREAM_OFF(argc) = 4 + argc*4`.
- **Fails soft.** If the audio manager is unreachable the driver logs and keeps
  playing without focus, so bench bring-up still produces sound.

## 14.2 Verified on the live stack

`probe/dsi_client_selftest.c` links the shipping module and runs the same
sequence the driver does:

```
[dsi-audio] factory halves installed (libbase=783f0000)
[dsi-audio] reply stub 10bba0 ready
[dsi-audio] proxy impl 10bc48 (sid=5008)
[dsi-audio] connected
dsi_audio_init -> 0 (alive=1)
getActiveEntertainmentConnection(id 8) -> 0
requestConnection(id 12, ENT_INTMEDIA)  -> 0
releaseConnection(id 11) -> 0
```

Provider side, same boot:

```
>>> process(mid=8)   read_int -> 0
*** impl vslot[3] (+0x0c)  a=00000000                      <- getActiveEntertainmentConnection
>>> process(mid=12)  read_int -> 1, 0, 0
*** impl vslot[4] (+0x10)  a=00000001 b=00000000 c=00000000 <- requestConnection(ENT_INTMEDIA)
```

`a = 1` is `VIRTUALCHANNEL_ENT_INTMEDIA`: the three-argument path arrives intact
at the slot §11.1 predicted. Build: `retroarch` 1 975 880 B stripped (+9 KB),
well under the 15 MB ceiling; `dlopen`/`dlsym`/`mprotect`/`sysconf` imported and
13 mangled framework symbols present in the binary.

## 14.3 Left open, deliberately

The reply interface has 13 wire ids (§11.7) but the mapping from those ids to
the RP_* meanings (RP_STARTCONNECTION / RP_PAUSECONNECTION / RP_STOPCONNECTION)
is **not** confirmed — the offline loopback has no real audio manager to answer,
and our own provider only echoes. So `qnx_qsa.c` logs replies and does not gate
playback on them. Once a unit with the live manager confirms the mapping,
RP_PAUSECONNECTION should stop the PCM writes on the same path the SIGUSR1 pause
already uses. That is the one piece of the audio story that genuinely needs
hardware.

---

# 15. Focus-loss policy (2026-07-23, owner's decision)

## 15.1 The rule

**Losing audio focus pauses the game once, and never resumes it by itself.**

- The RetroArch runloop stops producing PCM when it processes the pause edge;
  the short QSA hardware queue drains normally after the manager routes away.
- The emulator pauses, **edge-triggered, exactly once**.
- **No self-unpause.** The user unpauses and carries on (silently), or leaves.
- Audio routing returns according to the stock audio manager's connection state;
  no native re-request loop is involved.

A first draft of this had focus loss drive the pause state continuously, which
was wrong in a way worth recording: BT music starting takes the focus while our
screen never changes, so no context-switch event ever arrives — the user would
have been left staring at a permanently paused game with no way back short of
relaunching. Edge-triggered plus manual unpause removes that dead end while
still telling the player, unmistakably, that something took the sound.

## 15.2 Why the pause is visible

`command_event(CMD_EVENT_PAUSE, NULL)` → `runloop_pause_checks()`, which stops
`retro_run` (so no audio is produced), re-renders the cached frame (the screen
keeps its picture instead of going black), silences MIDI, and limits paused
frames to the refresh rate.

Without `HAVE_GFX_WIDGETS` the pause only pushes `MSG_PAUSED` through the OSD
queue with `duration = 1` — a single-frame toast, useless as an explanation for
sudden silence. **So widgets are now enabled for this platform** (+48 KB:
1 975 880 → 2 023 876 B). The indicator falls back to a text box when
`menu_pause.png` is missing (`gfx_widgets_draw_indicator`: `if (icon) … else`
draws the label), so shipping the asset directory stays optional.

## 15.3 Where it lives

- `AudioFocusBridge.java` — obtains stock Media `HMIAudioService`, owns
  connection 20 and registers `HMIAudioServiceListener` through LSD.
- `Shell.java` — turns a filtered pause/stop/error callback into QNX
  `SIGRTMIN` (41) for the PID stored in `/tmp/retroarch.lock`.
- `platform_qnx.c` — consumes that signal with `sigwaitinfo()` and records one
  atomic edge; no work runs in an async signal handler.
- `qnx_ctx.c` — consumes the edge on the main thread and issues one
  `CMD_EVENT_PAUSE`. It is deliberately separate from the level-based display
  lifecycle state.
- `qnx_qsa.c` — PCM transport only; it has no DSI/framework dependency.

## 15.4 No reply-ID guess in the shipping path

MU1316's `DSIAudioListenerImpl` already decodes the DSI methods and distributes
typed `startConnection`, `pauseConnection`, `stopConnection`, `fadedIn` and
`errorConnection` calls. The injected listener only filters `connection == 20`;
it never guesses a native wire id or result value.

---

# 16. The wire map, settled — from the framework's own table (2026-07-23)

§14.3 and §15.4 left one guess: which reply carries the connection state. It is
now answered exactly, and the guess turned out to be wrong in an instructive way.

## 16.1 Two independent sources, in agreement

**Source A — the shipped reply senders.** The PROXY_REPLY vtable at `off_21200`
has 13 senders; each moves its method id into the message trailer. Scanning for
`MOV Rd,#imm8` in each gives: slot0→4, slot1→6, slot2→10, slot3→21, slot4→22,
slot5→18, slot6→19, slot7→20, slot8→13, slot10→23.

**Source B — `eso/ems_tables/dsi.audio.ems_table.json`.** The framework ships
its own name↔id table, and the second signature of each entry carries the
**parameter names**. It matches source A exactly, id for id.

The Java side corroborates the names: `org.dsi.ifc.audio.DSIAudioManagementListener`,
found as `DSIAudioManagementListenerProxy` in `eso/hmi/lsd/DSITracer.jar`, has
the same nine listener methods with the same arities.

## 16.2 dsi.audio.DSIAudioManagement, complete

| id | method | parameters | direction |
|---|---|---|---|
| 0 | asyncException | errorCode:i, errorMsg:s, requestType:i | reply |
| 1/2/3 | clearNotification | –, attributes:#i, attribute:i | request |
| **4** | errorConnection | connection:i, terminal:i, errorCode:i | reply |
| 5 | fadeToConnection | connection:i, terminal:i | request |
| **6** | fadedIn | connection:i, terminal:i | reply |
| 7 | getActiveConnection | terminal:i | request |
| **8** | getActiveEntertainmentConnection | terminal:i | request |
| 9 | getVolumelock | connection:i, terminal:i | request |
| **10** | pauseConnection | connection:i, terminal:i | reply |
| 11 | releaseConnection | connection:i, terminal:i | request |
| **12** | requestConnection | connection:i, terminal:i, group:i | request |
| 13 | responseVolumelock | connection:i, terminal:i, active:b | reply |
| 14/15/16 | setNotification | –, attributes:#i, attribute:i | request |
| 17 | setVolumelock | connection:i, terminal:i, active:b | request |
| **18** | startConnection | connection:i, terminal:i | reply |
| **19** | stopConnection | connection:i, terminal:i | reply |
| 20 | updateAMAvailable | aMAvailable:i, sink:i, validFlag:i | reply |
| 21 | updateActiveConnection | connection:i, terminal:i, validFlag:i | reply |
| **22** | updateActiveEntertainmentConnection | connection:i, terminal:i, validFlag:i | reply |
| 23 | yyIndication / 24 yySet | key:s, value:s | reply / request |

Every id §11.1 derived from the dispatcher's switch is confirmed by name:
8 → getActiveEntertainmentConnection, 12 → requestConnection (three ints),
17 → setVolumelock (two ints and a bool), 11 → releaseConnection, 24 → yySet.

## 16.3 Two corrections this forced

**The state is the method, not a value.** §15.4 classified on the argument
value, looking for the Java RP_* numbers 2000..2004 in the payload. Those are
reply-type constants on the Java side and never travel on the wire. The manager
says what happened by *which method it calls*: startConnection / fadedIn mean we
may play; pauseConnection / stopConnection / errorConnection mean we may not.

**Replies must be filtered by connection.** Every connection-scoped reply begins
with `connection:i`. The manager talks to all its clients, so an unfiltered
client pauses itself when *CarPlay's* connection is paused. The client now
ignores any reply whose connection is not the one it requested.

`updateActiveEntertainmentConnection(connection, terminal, validFlag)` also turns
out to be the clean way to learn that the other source let go — it broadcasts who
currently owns the entertainment connection, so re-acquisition no longer depends
on the retry timer (which stays as a fallback).

## 16.4 Still unknown

`requestConnection`'s `terminal` and `group`. No caller in the image passes
literals we can read, so both are 0 until a real unit says otherwise. The
`connection` argument is the virtual channel (ENT_INTMEDIA = 1), which is
consistent with `getActiveEntertainmentConnection(terminal)` returning one.

## 16.5 A dangerous conflation, caught by cross-review

Codex, reviewing the map independently, flagged that
`requestConnection`'s first argument is **not** the mplN virtual channel. Checked
against `org/dsi/ifc/audio/Constants` (from `eso/hmi/lsd/lsd.jxe`, via the
converted `MU1316-lsd-full.jar`) — it is right, and the mistake was worse than
cosmetic:

```
CL_FCT_AMP_RELEASE_ALL      = 1     <- what we were passing
CL_ENT_AMP_MEDIA_MFP        = 20    <- internal media player: us
CL_ENT_AMP_MEDIA_BTDEVICE   = 21
CL_ENT_AMP_GAL_MEDIA        = 156   (Android Auto)
CL_ENT_AMP_DIO_MEDIA        = 162   (CarPlay)
TERMINAL_SINGLE_USER_DEFAULT_TERMINAL = 0, FRONT_DRIVER = 1, FRONT_PASSENGER = 2
```

There are **two namespaces** and the code had merged them:

- the **PCM device** is selected by name (`/dev/snd/mpl1_int_ent`); the "virtual
  channel" numbering that goes with it is a driver/LSD naming matter and never
  appears on the DSI wire;
- `requestConnection(connection, …)` takes an `org.dsi.ifc.audio` **CL_\*** id.

Passing 1 there does not mean "internal media" — it means
**CL_FCT_AMP_RELEASE_ALL**, i.e. telling the amplifier to drop everything. On a
bench unit that would have looked like the emulator killing all audio in the
car, and it would have been a thoroughly confusing thing to debug from the
symptom. The client now uses `CL_ENT_AMP_MEDIA_MFP` (20) with terminal 0.

Codex also confirmed, from the generated Java in the LSD image, what §16.3
concluded from the table: `DSIAudioManagementProxy.requestConnection` serializes
three ints under method id 12; `DSIAudioManagementReplyService` decodes id 10 as
two int32 into `pauseConnection` and id 18 into `startConnection`; and
`DSIAudioManagementDispatcher` calls **every** registered listener with no
connection filter of its own — which is exactly why the filtering §16.3 added is
required rather than merely tidy.

`group` remains 0: every media caller passes 0, only the tuner passes a packed
AudioGroup id (e.g. 0x71010200 for FM).

---

# 17. Audio: final shipping status (2026-08-07)

The native client above remains a verified RE prototype, but it is not linked
into RetroArch. Shipping code follows the exact MU1316 Java architecture:

- `AudioFocusBridge.java` selects `HMIAudioService` with
  `AUDIO_CLIENT_ID=CLIENT_MEDIA (1)`, registers a stock listener, calls
  `requestAndFadeToConnection(20,0)`/`releaseConnection(20,0)`, selects media
  focus app 2/context 3, and routes `ENT_INTMEDIA` to MPL1.
- `qnx_qsa.c` negotiates S16/S32 and the device's native voice count, upmixes
  stereo and handles short writes/underruns; it is PCM-only.
- `platform_qnx.c` + `qnx_ctx.c` turn listener `SIGRTMIN` into one main-thread
  pause with no auto-resume.
- `framework.json` remains untouched. The package contains no agent fragment,
  native DSI registration or manual `MS_ENT` fallback.
- Current stripped ARMv7 EABI5 frontend: 2,255,652 bytes.

Still required on the head unit: prove audible PCM from an arbitrary previous
HMI source, restoration of radio/media on exit, and phone/nav/PDC interruption
callbacks while saving `/tmp/ra_audio.log`, `/tmp/ra_hook.log` and
`/tmp/ra_run.log`.
