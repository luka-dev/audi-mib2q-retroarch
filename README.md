# retroarch-qnx — RetroArch for QNX 6.5 / MHI2Q (armle-v7)

RetroArch and all production core sources are vendored directly in this
repository; there are no required gitlinks or submodules. Exact upstream and
local snapshot commits are recorded in `VENDORED_SOURCES.md`.

Port RetroArch with gpSP, PCSX-ReARMed and Mupen64Plus-Next to the Audi MHI2Q
head unit (QNX 6.5.0, APQ8064 / Cortex-A15 + Adreno 320), built with our
[[gcc49-qnx-port]] toolchain
(`../qnx-gcc49/qnx49.sh`). Source in `src/` (libretro/RetroArch clone).

## The catch: "QNX" port == BlackBerry 10 port

RetroArch's existing QNX support (`frontend/drivers/platform_qnx.c`,
`gfx/drivers_context/qnx_ctx.c`, `input/drivers/qnx_input.c`, `-DHAVE_BB10`) was
written for the BlackBerry PlayBook/BB10 — QNX-based, but it leans on **BPS**
(BlackBerry Platform Services: `bps/bps.h`, `navigator`, `packageinfo`, events)
and **OpenAL**, none of which exist on MHI2Q. What MHI2Q *does* have: QNX
**Screen** (`screen/screen.h`, io-graphics — Kanzi HMI uses it), **EGL** +
**GLESv2** (Adreno driver), **io-audio/QSA**.

So the BB10 drivers are a *starting shape*, not drop-in. The work:

| layer | BB10 port uses | MHI2Q replacement |
|-------|----------------|-------------------|
| window/context (`qnx_ctx.c`) | bps navigator → EGL window | QNX **Screen** window → EGL directly (drop bps) |
| platform/lifecycle (`platform_qnx.c`) | bps app events | minimal QNX main loop (drop bps) |
| input (`qnx_input.c`) | bps touch/keys | QNX **io-hid/HIDDI** generic/BT pads + **io-usb XUSB/GIP** Xbox pads |
| audio | OpenAL | io-audio/QSA PCM + OEM DSI entertainment focus |

## Working reference: how gpSP / PCSX-ReARMed already did it on MHI2Q

Don't reinvent the MHI2Q integration — it's solved and running in
`../../Patches/gpSP/qnx/` and `../../Patches/pcsx/pcsx_rearmed/qnx/`. Reuse these
exact mechanisms instead of RetroArch's BB10 drivers:

| need | proven mechanism (files) | notes |
|------|--------------------------|-------|
| **video / display session** | `plat_qnx_egl.c` → `dlopen("/eso/lib/libdisplayinit.so")`, `dlsym("display_init"/"display_create_window")` | you **don't own the screen** (Kanzi does). libdisplayinit creates a **dedicated displayable** (id **200**, ctx **90**) — an overlay layer that does NOT touch `DISPLAYABLE_HMI (16)` / context 0. Returns an `EGLNativeWindowType` → standard EGL + GLES2, present frames as an RGB565 texture on a shader quad. Same `libdisplayinit.so` used by `../qnx-gl-passthrough`. **But gpSP's ad-hoc id-200 is a full overlay — for a *windowed, in-HMI* view, use the map-viewer model below instead.** |
| **audio** | `qnx_snd.c` → **QSA** (`<sys/asoundlib.h>`, `snd_pcm_*`, `SND_PCM_CHANNEL_PLAYBACK`, 48 kHz) | RetroArch has no QSA driver → write one (small) or start `null`. **PCM transport only** — see the routing analysis below. |
| **input** | HID gamepads via QNX `io-hid`/`hidd_*`; Xbox XUSB/GIP via `io-usb`/`usbd_*` | NOT DSI keypad, NOT Linux evdev (QNX has neither `/dev/input/js`; gpSP's probe of it was a blind guess). |
| **launch/lifecycle** | injected, fail-closed `RaScreen` SystemSMM state | screen connect launches RA; every disconnect restores display, releases audio and sends SIGTERM — no localhost protocol |
| **audio focus / routing** | stock LSD `HMIAudioService` + `DSIMediaRouter` from `RaScreen` | Java behaves like an OEM media client: Media/MFP connection 20, focus app 2 and ENT_INTMEDIA → MPL1; no new native DSI process or framework registry edit |

### No ad-hoc IP sockets (why the gpSP UDP bridge is gone)

gpSP shuttled data between its injected Java and the native binary over
**hand-rolled localhost UDP** — `qnx_input.c` bound `127.0.0.1:<port>` and parsed
`"KEY <code> <state>"` / `"ENC <delta>"` datagrams from the Java `KeyPanelBridge`,
and `qnx_snd.c` ran a "simple UDP protocol on localhost" for audio control/ack
(plus a diag TCP socket). That whole layer is **removed** in this design:

- **Input** is read in-process through **io-hid/HIDDI** or **io-usb/XUSB/GIP** → the keypad
  UDP listener + Java `KeyPanelBridge` disappear entirely. Zero IPC.
- **Audio focus** is requested directly by the injected HMI state through the
  stock LSD service registry. This is an in-process reflective call into the
  OEM service interfaces, not a Java/native UDP bridge. QSA remains the native
  PCM transport.
- The **only** sockets left are *inside* DSI's own transport (`libdsicommon` →
  `libcomm`/`libsocket`) — that's the platform's IPC bus that every MHI2Q service
  already uses, invisible to us; we call a C++ API, not a socket.

Net: RetroArch creates its native displayable, writes PCM through QSA and reads
the gamepad through HIDDI or the local io-usb client. The real `RaScreen` HMI state owns display context,
process lifetime and the OEM entertainment session. **No `AF_INET`, no loopback
ports, no text protocol.**

So the MHI2Q port of RetroArch is: **rip `-DHAVE_BB10`/bps, render via
libdisplayinit, read pads in-process through HIDDI/io-usb, write PCM through QSA and
bind the OEM route to the HMI state's lifecycle** — no ad-hoc sockets.

### Lifecycle / focus state machine (MENU-BACK, context switch, ignition)

Hard keys (MENU/BACK/rotary) belong to the **HMI**, not the emulator — the game
only ever sees the USB gamepad (HID). Lifecycle is a **one-way command channel:
the HMI decides, the emulator obeys** — there is nothing to negotiate (the emu
never vetoes a close), so it does **not** need a bidirectional DSI conversation and
does **not** need to be a DSI display-listener. The HMI already knows every trigger
(which key, that *it* switched context, ignition from its own subsystems) and the
child PID (it spawned it) → **plain POSIX signals, one direction, HMI → emu.** No
sockets, no DSI round-trip for lifecycle.

| trigger (HMI knows it) | signal → emu | what the emu does (in the *runloop*, not the handler) |
|---------|------|--------|
| **context switch away** (call/nav/media takes the display — not BACK) | **SIGUSR1** = pause | halt `retro_run` + buffer swap (**0 GPU**); **drain/discard** QSA, close/deconfigure the PCM subchannel, **release DSI audio focus**. **Resident.** |
| **context switch back** | **SIGUSR2** = resume | re-acquire DSI focus, reopen/reprepare QSA, clear stale resampler buffers, resume `retro_run` |
| **BACK** (explicit close) | **SIGTERM** | `CMD_EVENT_SAVE_FILES` (**SRAM flush**) + `content_save_state` + core unload → **clean exit** |
| **long out-of-context** (timer) / **ignition off** (`DSIPowerManagement` in the HMI, or system power-down) | **SIGTERM** | same clean-exit path (SRAM save + exit) |
| **HOLD BACK / hung / watchdog** | **SIGKILL** | force kill (no SRAM — see below) |

**Signal handling must be done the QNX way (Codex-reviewed):**
- **No real work in a handler.** Async handlers must only set a `volatile
  sig_atomic_t` (or post a semaphore / write a pipe byte). SRAM flush, EGL, QSA,
  DSI, mutexes, logging are **not** async-signal-safe. RetroArch already models
  this (handler sets a quit flag, the runloop acts) — pause/resume/save all run in
  the **main loop**, never the handler.
- **Dedicated lifecycle thread + `sigwaitinfo()`.** In a multithreaded process,
  a process-targeted signal lands on *whatever thread isn't blocking it* — could be
  the audio or a DSI-callback thread. So **block SIGUSR1/USR2/TERM in every thread
  before RetroArch spawns its threads**, then run one lifecycle thread doing
  `sigwaitinfo()` that flips the atomics. (Be explicit about inherited masks if
  launched via `posix_spawn`.)
- **Bounded break-out.** The runloop must check the lifecycle atomics **every outer
  iteration**, and every blocking QSA/EGL call must handle `EINTR` (retry) — a flag
  is useless if the main thread is wedged forever in `eglSwapBuffers()` or
  `snd_pcm_*write()`. That wedge is exactly what the SIGKILL escalation backstops.
- **Signals coalesce / are priority-ordered, not a sequenced command stream.** For
  rapid pause/resume churn, treat the signal as "reconcile to desired state" and
  keep the authoritative desired-state (paused/running) somewhere the runloop reads,
  rather than counting edges. (A QNX **pulse** channel is the heavier alternative if
  ordering ever matters — still not ad-hoc TCP.)

Why signals, not DSI, for lifecycle: it's one-directional, works even if the emu's
DSI/EGL is wedged, and **SIGKILL has no DSI equivalent** for a hung process. DSI is
reserved for what the emu *asks the services for on its own* (audio focus,
displayable positioning) — never for HMI→emu "pause/close" commands. Whether the
emu paused/saved is answered by the **lockfile** (below), not a DSI reply.

Why `SIGUSR1`-handler and not `SIGSTOP` for pause: `SIGSTOP` freezes the whole
process instantly (0 code, 0 CPU) but the emu then **can't release audio / stop PCM
cleanly** — it's frozen mid-buffer (QSA underrun). The handler lets it pause
cleanly. (`SIGSTOP` is fine only if the HMI reroutes audio on the context switch and
you don't care about a clean stop.)

**Everything funnels into one clean-exit path** (SRAM flush → core unload → exit),
reached by SIGTERM from any of BACK / timeout / ignition — written once.
Install it in `platform_qnx.c`'s `install_signal_handler` slot
(SIGTERM→shutdown, SIGUSR1→pause, SIGUSR2→resume).

**Relaunch serialization (BACK then immediately re-open from the menu).** BACK
triggers an async teardown (SRAM flush can take a beat). If the user re-opens the
game before the old process is gone, we must **fully close the old, then run the
new** — never two instances fighting over the displayable / audio focus / gamepad,
never a half-written SRAM. Mechanism = a **single-instance lockfile** (e.g.
`/fs/sda0/retroarch/ra.lock`, exclusive `flock` + the holder's PID inside), no
socket:

1. Every instance grabs the lock at startup and releases it only after the
   clean-exit path finishes (SRAM written, core unloaded).
2. A newly-spawned instance that finds the lock held reads the PID, makes sure a
   SIGTERM was delivered, and **blocks on the lock** until the old instance
   finishes tearing down and releases it — *then* proceeds to load the new content.

Implementation details that actually matter (Codex-reviewed):
- Open `O_RDWR|O_CREAT`; set **`FD_CLOEXEC`** (unless a supervisor deliberately
  locks then `exec`s RetroArch, in which case it must *not* be close-on-exec).
- **The lock is the authority, the PID inside is diagnostic only** — never decide
  "is it alive?" from the PID file; the kernel's lock ownership is the truth.
- Treat **`ENOSYS`/`EOPNOTSUPP` as fatal at startup**, loudly — QNX returns it when
  the filesystem doesn't support `flock`, and silently continuing means no
  serialization at all. (Verify on the real `/fs/sda0` QNX6 fs.)
- Handle **`EINTR`** on the blocking acquire (a blocked waiter can be woken by a
  signal) — retry the wait.
- Add a **wait timeout** so a new launch can't block forever if the old process is
  wedged and the HMI failed to escalate to SIGKILL; on timeout, escalate (SIGKILL
  the holder) rather than hanging the UI.
- QNX documents file locks as released when the holding process terminates, so
  SIGKILL does drop it — but confirm on device (see the orphan test below).

So even a hammered BACK→open→BACK→open sequence is serialized: old fully closed
(state saved) → new runs. The emulator self-enforces this via the lock, so it's
robust regardless of how the HMI hook fires; a tiny native launch supervisor doing
`kill(old,SIGTERM); waitpid(old); exec(new)` is an optional belt-and-suspenders.

### Hung emulator → HOLD-BACK force kill (SIGKILL escalation)

A frozen core / GL deadlock won't answer SIGTERM — the signal handler never runs
because the main loop (or the whole process) is stuck. **A hung process can't kill
itself**, so the kill must come from *outside* (the HMI hook / supervisor, which
holds the child PID from the spawn). Escalation ladder:

| gesture | signal | result |
|---------|--------|--------|
| **short BACK** | `SIGTERM` | graceful: SRAM flush + clean exit (the normal path). Arm a **~2–3 s watchdog timer**. |
| watchdog timer fires & still alive | `SIGKILL` | auto force-kill — SIGTERM was ignored ⇒ it's hung. |
| **HOLD BACK** (long-press) | `SIGKILL` **immediately** | user override: skip the wait, nuke a visibly-frozen game now. |

`SIGKILL` can't be caught or blocked — the kernel tears the process down
regardless of state. Trade-off: **no SRAM save** on a force-kill (the process was
hung, it couldn't save reliably anyway). That's the accepted cost of the escape
hatch; the short-BACK path is the one that saves.

Post-kill cleanup is automatic and needs no socket:
- **Lockfile**: `flock` is released by the kernel when the process dies (even on
  SIGKILL) — so serialization survives; the next launch acquires the lock cleanly.
  (This is exactly why the lock is an `flock`, not a bare PID file that would go
  stale on SIGKILL.)
- **Displayable + audio focus**: the kernel guarantees only that *process-owned*
  resources (fds, locks, memory) are reclaimed on death. **DSI/compositor state is
  NOT kernel-guaranteed** — it depends on the display/audio services implementing
  client-death cleanup correctly. **Treat this as a required hardware test, not an
  assumption** (Codex's point, and the right one). Run `kill -9` while the emu is:
  (a) actively `eglSwapBuffers`-ing, (b) holding DSI audio focus, (c) mid PCM
  playback, (d) during an HMI context switch — then check: does the displayable
  disappear, does opacity/position reset, is the audio route/focus released, and can
  a relaunch recreate the layer? **If anything leaks**, the HMI/supervisor must do
  explicit post-kill cleanup by the known displayable id (43) and audio connection
  identity — it's switching context to the menu on BACK anyway and can force-release
  the layer via its own DSIDisplayManagement.

Implementation: the HMI's Java launch hook already holds the child `Process` —
`Process.destroy()` = SIGTERM (short BACK), `Process.destroyForcibly()` = SIGKILL
(hold BACK / watchdog). A native supervisor is the alternative. **Optional proactive
watchdog:** the emulator bumps a heartbeat (touch a file / increment a shared
counter) each frame; the supervisor SIGKILLs if the heartbeat goes stale for N
seconds — catches a hang even without the user pressing anything.

> ⚠ **The shell-PID trap (Codex caught this — gpSP's launcher shape hits it).**
> gpSP launched via `CommandLineExecuter("/bin/sh", {"-c", cmd})`. With plain
> `sh -c "retroarch …"` the PID the HMI holds is **the shell**, not RetroArch —
> so `destroy()`/`destroyForcibly()` kills the shell and leaves the **emulator
> orphaned**, still holding the displayable, audio focus and the lockfile. Every
> signal in this design silently targets the wrong process.
> Fix, pick one: **`sh -c 'exec /path/retroarch …'`** (exec replaces the shell so
> the PID *is* RetroArch), a **direct native spawn** (`posix_spawn`, no shell), or
> a **small native supervisor** that owns the real child PID / process group (and
> can signal the whole group). Also be explicit about the **inherited signal mask**
> across `posix_spawn`/`spawn` — a mask inherited from the HMI could leave our
> lifecycle signals blocked in the child.

### Video — dedicated full-screen context owned by the RA state

The stock navigation is not a separate fullscreen app — the map is a
**compositor layer** the Kanzi HMI positions inside its own scene. There is a
fixed set of hardware/compositor **displayables** (`org/dsi/ifc/displaycontroller/
Constants.java`): `DISPLAYABLE_HMI=16`, `DISPLAYABLE_MAPVIEWER=19`, route-guidance
20, intersection views 21-24, `REAR_VIEW_CAM=17`, `DVD_VIDEO=25`, `FILE_VIDEO=37`,
`DIGITAL_VIDEOPLAYER_1/2=43/44`, `HMI2=36`, `3D=41`, …

Two layers of control, coarse + fine:

- **Native — `/eso/bin/apps/dmdt`** (Display Manager control tool; io-graphics is
  the compositor). Only two verbs matter:
  - `dc <cid> <did1> <did2> …` — **declare a context** `cid` as a set of displayables.
  - `sc <display> <cid>` — **switch a display** to show context `cid`.
  (`gd`/`gc`/`gs` list clients/contexts/system, `ts <display> <path>` screenshots.)
  This is context-granular only — **no position/opacity here**.
- **Java — `IDisplayManager`** does the *fine* placement of a displayable *within*
  the current context: `setPosition(disp, terminal, x, y)`, `setOpacity`,
  `fadeToOpacity`, `switchContext`. `DisplayControllerEvo.isBackGroundLayer()`
  says **everything except `HMI`(slot0) and `HMI2`(slot15) composites UNDER the
  HMI** — so any non-HMI displayable in the active context is automatically a
  background layer showing through the HMI's transparent regions.

The stock context 25 is `{HMI 16, DIGITAL_VIDEOPLAYER_1 43}`. It is suitable for
video behind HMI chrome, but it necessarily retains the lower status bar and an
empty custom screen can cover the video layer. RetroArch therefore keeps its
real SystemSMM state/lifecycle but uses a dedicated full-screen display context:
`dmdt dc 90 43` followed by `dmdt sc 0 90`. Because context 90 contains only
displayable 43, neither HMI displayable 16 nor its status bar is present.

**Emulator recipe (full-screen, state-machine owned):**
1. Native render is unchanged from gpSP: libdisplayinit → QNX **Screen** window
   (`screen_create_window_type` + `SCREEN_PROPERTY_FORMAT/SIZE/USAGE(GLES2)/
   ID_STRING/VISIBLE` + `screen_create_window_buffers`) → EGL surface. Pass the
   emulator's displayable id to `display_create_window(dpy,cfg,w,h,DISP_ID,&win,&kd)`.
2. After the window exists, declare context 90 with displayable 43 and switch
   display 0 to it. The custom Java state saves the previous OEM context before
   launch and restores it on every disconnect/BACK/MENU/forced transition.
3. **Pick a free displayable id.** `dmdt`'s enum lists ~40 displayables but this
   US nav variant only wires slots 0-18 (HMI…GOOGLE_EARTH). Unused here and safe
   to claim: `DISPLAYABLE_DIGITAL_VIDEOPLAYER_1/2` (**43/44 — best fit: a digital
   GLES video surface, our exact case**), plus the analog-tuner ones absent on
   US MHI2Q (`TV_TUNER`, `TV_AUX1/2`, `FBAS_1/2/3`, `DVD_VIDEO`, `FILE_VIDEO=37`).
   Avoid `MAPVIEWER(19)` (nav owns it) and the id-200 gpSP overlay (own-context).
   **43/44 verified chrome-free:** no Java widget/controller (only a DSI const +
   stock context 25 membership), and no node in any of the 63 `.kzb` scenes — vs
   `DVD_VIDEO`, which has player UI. Context 90 gives RA exclusive 1024x480 pixels.

Reference: `de/audi/atip/hmi/view/IDisplayManager.java`,
`de/esolutions/hmi/widgets/audi/evo/DisplayControllerEvo.java` (`isBackGroundLayer`,
`setPositionOnBackgroundLayers`), `.../evo/high/MapControllerEvoHigh.java`;
native: `/eso/bin/apps/dmdt`, `/eso/lib/libdisplayinit.so`, gpSP
`Patches/gpSP/qnx/plat_qnx_egl.c` (the `dmdt dc/sc` shell-out at lines 404/413).

Main-context IDs (from `MainContext`, arg to `switchContext`/`sc`): NONE=0,
NAVIGATION=16, NAVIGATION_MAP=20, AUDIO=32, AUDIO_TUNER=36, AUDIO_MEDIA=39,
PHONE=48, APPS=96, CONNECT=112, CAR=128. Launch-from-Apps → the emulator lives
under `APPS=96` (or reuse the current context to overlay wherever the user is).

#### How the stock map renderer creates its GL surface (confirms our path)

The native map engine's surface-creation is **identical** to gpSP / our plan —
the only special sauce is the Kanzi-based engine on top, which we don't need.
Stack, top → bottom:

```
Java nav-HMI
 → libealswig.so   SWIG-JNI, package de.esolutions.graphics.eal.*
                   (IInputListener, ealMouseState_t, CAbstractEventListener…)
 → libeal.so       C++ scene graph "eal::api" (INode2D/3D, ITextureShared,
                   GLSL-ES 1.00 shaders), built ON TOP of the Kanzi engine
                   (symbols kzuRenderer/kzuLayerRender/kzcMatrix4x4/PropertyTypeLibrary)
 → libGLESv2.so.1  GLES2 (Adreno)
 → display_create_window   ← libdisplayinit   (log: "eal> display_create_window(%i,%i)")
 → QNX Screen window → EGL surface (eglChooseConfig)
```

Takeaways:
- **Bottom two links = the same `display_create_window → Screen → EGL/GLES2`** that
  gpSP uses. The map does nothing special at the surface level; its "magic" is the
  Kanzi engine above. So RetroArch's own GLES2 blitter over libdisplayinit is a
  faithful substitute — we skip eal/Kanzi entirely.
- **The fade** `MapControllerEvoHigh.setOpacity/fadeToOpacity` drives is a shader
  uniform inside eal (`LayerRenderOpacity`, composites `LayerRenderTexture` →
  `LayerCompositionTexture`). Confirms the Java side moves *layer opacity*, not map
  pixels. For us the io-graphics/Screen compositor does the layer blend at the
  displayable level — simpler, no in-engine compositing needed.
- **`eal::api::ITextureShared::updateFromDisplayable(id[,x,y,w,h])`** (`[CTextureShared]
  Capturing displayable=%u`): the engine can capture *another* compositor displayable
  into a GL texture. Not needed by us, but proves a layer-capture path exists in the
  firmware if we ever want to sample the HMI into a scene.

Reference (native): `/eso/lib/libeal.so`, `/eso/lib/libealswig.so`,
`/eso/lib/libdisplayinit.so`, `/eso/bin/apps/dmdt`.

### Audio routing — do it better than gpSP (LSD `MU1316-lsd-full` analysis)

Two separate things: **(1) getting the PCM out** and **(2) getting audio focus.**
QSA (io-audio) is only (1) — the amp won't play it, or won't duck/interrupt
correctly, unless the head-unit **audio manager** grants (2).

The shipping session combines the OEM layers that must agree:

- **`IAudioFocusManager`** selects stock media app 2 for front HMI terminal 0.
- **Stock LSD `HMIAudioService` for `CLIENT_MEDIA` (id 1)** is the
  connection/fade arbiter. It already owns the firmware's DSI/RPC client, so
  the injected state calls `requestAndFadeToConnection(20,0)` and
  `releaseConnection(20,0)` without creating a second framework process. The
  service reports `startConnection`/`pauseConnection`/`stopConnection` through
  an ordinary `HMIAudioServiceListener`. The
  system knows about **entertainment ducking** (`Constants.LOWERINGPRESET_ENTERTAINMENT_*`
  applied when nav `NT_NAV` / park-assist `NT_APS` prompts play) and amp types
  (`DEVICETYPE_*BOSE/B&O/FENDER`). Connection 20 is Media/MFP.
- **`org.dsi.ifc.media.DSIMediaRouter`** (from the injected HMI state) registers that same connection as
  EXTERNAL/NONE, routes `VIRTUALCHANNEL_ENT_INTMEDIA` to `PHYSICALCHANNEL_MPL1`
  and starts streaming. Unlike the old gpSP path, this happens only after focus
  and the request-and-fade connection succeed, and all three are released
  together when `RaScreen` disconnects.

**The proven native PCM path (gpSP `qnx_snd.c`, plays PS1 audio today):**
open a **QSA** playback device (`snd_pcm_open`), then set the **entertainment
mixer switch `MS_ENT`** via `snd_ctl` — default route `mpl5_dio_ent` (mixer
input 5 → output 128 / MPL). `deva-ctrl-qc.so` (the io-audio driver) **internally
owns the Qualcomm CSD/amp session** for `mplN_*_ent`, so you do NOT touch
`/dev/audio_service` / `/dev/csdProxy` / CSD directly. Unit config lives in
`/etc/system/config/audio/preferences`; physical channels are `mpl1..mpl7`
(1=`mpl1_int_ent`, 5=`mpl5_dio_ent`/CarPlay).

> Ignore `armle/bin/audio_chime` + `audio_service`/`csd_ipc` — that's a separate
> low-level Qualcomm **chime** path (park-assist/warnings), not the continuous
> entertainment path, and may not even be usable for streamed game audio.

#### Which channel, and who sets `MS_ENT` — the actual analysis

Ground truth from `armle/lib/dll/deva-ctrl-qc.so` (the Qualcomm io-audio driver)
— the entertainment playback devices it creates, and how the LSD's virtual
channels map onto them:

| virtual channel (`org/dsi/ifc/audio/Constants.java`) | id | `/dev/snd` device | what it's for |
|---|---|---|---|
| `VIRTUALCHANNEL_ENT_INTMEDIA` | 1 | `mpl1_int_ent` | **internal media** (the HU's own USB/SD/BT-audio player) |
| `VIRTUALCHANNEL_ENT_ML` | 3 | — | MirrorLink |
| `VIRTUALCHANNEL_ENT_DIO` | 4 | `mpl5_dio_ent` | digital-in / **CarPlay** ← what gpSP grabbed |
| `VIRTUALCHANNEL_ENT_GAL` | 5 | `mpl6_gal_ent` | Google Android Link (Android Auto) |
| `VIRTUALCHANNEL_ENT_BCL` | 33 | `mpl7_bcl_ent` | BaiduCarLife (China market) |

**`MS_ENT` is not a "route my stream" knob — it's the hardware entertainment
*source selector*.** The driver builds it with `ado_mixer_switch_new` and drives
it through `deva_mixer_router_set_switch_routing`: it decides **which single
mplN is connected to the amp** at a time. That's the same switch the HMI flips
when you change source (radio → media → CarPlay).

So the real question isn't "which mplN" — it's **who sets `MS_ENT`**:

- **gpSP: sets it itself** (`snd_ctl` → input 5 → output 128). It had to, because
  it couldn't reach DSI from native code. Cost: it **fights the HMI** — the HMI
  re-asserts `MS_ENT` on any source change, the volume knob / mute / ducking logic
  never learns the game is playing, and on `mpl5_dio_ent` it **collides with real
  CarPlay**.
- **Us: never touch `MS_ENT`.** Native RetroArch alone requests/releases
  `CL_ENT_AMP_MEDIA_MFP` (20) and handles the manager's start/pause/stop replies.
  The HMI state only selects media focus/context and configures the MPL1 route,
  so there is no duplicate connection request with contradictory terminal/group
  values. Volume/mute/ducking remain in the system model.

**Recommendation: `VIRTUALCHANNEL_ENT_INTMEDIA` (1) → write PCM to
`/dev/snd/mpl1_int_ent`, and let `DSIAudioManagement` do the routing.** Reasons:
semantically the emulator *is* internal entertainment media; it's the channel the
audio manager already knows how to route/duck/volume; and it is **definitely
provisioned** — the HU's own player uses it every day. Contention with that player
is not a bug to dodge but exactly what the focus arbiter resolves: we take the
connection, the media player pauses, like any source switch.

> **Don't reuse the "pick an unused slot" trick from the display side.** It was
> right for displayable 43 (`DIGITAL_VIDEOPLAYER`, chrome-free), and `mpl7_bcl_ent`
> (BaiduCarLife) looks equally free on a US unit — but **audio isn't display**: an
> unused channel may have no **ACDB/CSD calibration**, so the route exists on paper
> and plays into nothing. Silence that's hard to debug. Use the provisioned channel.

The listener exposes `startConnection`, `pauseConnection`, `stopConnection`,
`fadedIn` and `errorConnection`. The bridge filters them by connection and sends
QNX `SIGRTMIN` (41) once when connection 20 is taken. RetroArch pauses on the
main thread and deliberately never auto-resumes gameplay.

**Implementation:** native QSA for PCM (`snd_pcm_open` on the granted device,
normally 48 kHz), plus `AudioFocusBridge` inside the injected HMI state. The
bridge finds the exact Media `HMIAudioService` by `AUDIO_CLIENT_ID=1`, registers
a listener through LSD, selects `IAudioFocusManager`/SDIS context and configures
`DSIMediaRouter`. `request()` runs from `RaScreen.connected()`; `release()` runs
from every `disconnecting()` path. Diagnostics go to `/tmp/ra_audio.log` and
RetroArch's normal log. There is no native DSI client, `framework.json` edit,
manual `MS_ENT` write or UDP bridge.

**No `MS_ENT` fallback — deliberately.** Falling back to gpSP's manual switch
grab is not a safety net, it's the design we just rejected: it fights the HMI over
the source selector, collides with CarPlay, and silently desyncs volume/mute/
ducking. Its failure modes are *worse and harder to diagnose* than having no audio
yet. If an OEM service is temporarily unavailable, the daemon worker retries
while the RA state remains connected and rolls back every partial activation.
During bring-up, opening the PCM device without a granted connection is useful
only as a diagnostic, never as the shipping path.

Refs: `qnx_snd.c`, `org/dsi/ifc/audio/{DSIAudioManagement,Constants}.java`,
`eso/lib/factories/libdsiaudioproxy.so`, `deva-ctrl-qc.so`.

## Filesystem layout & install paths (ro appimg vs writable state)

Ground truth from the firmware (`strings` over the appimg + QNX io-blk naming):

| mount | what it is | r/w | use |
|-------|-----------|-----|-----|
| `/mnt/app` | the signed **appimg** (this whole tree) | **read-only** at runtime | binary, Java JAR, cores, runtime libs and initial config only |
| `/fs/sda0` | **SD slot 1**, partition 0 (the tested 32 GB FAT32 card) | rw*, removable | primary RetroArch resources/state + games |
| `/fs/sdb0` | **SD slot 2**, partition 0 (nav is internal, not on either card) | rw*, *may be absent* | additional games |
| `/fs/usb0_0` | **USB** mass storage | rw*, *may be absent* | ROMs (alt) |
| `/mnt/persist` | tiny persistent KV store (`savedpreset.json`) | rw | too small — skip |
| `/tmp`, `/dev/shmem` | tmpfs (RAM) | rw, **volatile** | scratch only |

`rw*` = the SD/USB block devices are user-writable **but may be auto-mounted
read-only** — the launcher may need `mount -uw /fs/sda0` or `/fs/sdb0` before
writing saves/config there. That's fine: they're user media, not the signed app
image. (Contrast `/mnt/app`: **never** remount — signed, ro by design.)

The tested unit exposes the two physical controllers as `sda` and `sdb`, with
their first partitions mounted as `/fs/sda0` and `/fs/sdb0`. A path such as
`/fs/sda1` would mean partition 1 of slot 1, not the second slot. The package
keeps writable RetroArch state on `/fs/sda0`; content discovery reads games
from both cards.

**gpSP's mistake (the exact issue you flagged):** it defaulted rom/save/config to
`/mnt/app/root/gpsp/{roms,saves,gpsp_qnx.cfg}` — *inside the read-only appimg*.
Saves to `/mnt/app/...` fail unless someone `mount -uw`'s the app image. It had a
fallback probe (`/fs/usb0_0` → `/fs/sdb0` → `/fs/sda0`) but the defaults were wrong.

Keep `/mnt/app` read-only during normal operation. Remount it writable only for
an explicit, verified deployment. Static UI assets are safe in appimg; saves,
configuration, caches and every other runtime write belong on SD.

**Design — immutable factory app + replaceable SD user layer:**
```
# READ-ONLY at runtime — complete safe application/UI layer
/mnt/app/root/retroarch/
    retroarch                  # griffin frontend binary
    cores/*.so                 # gpSP, PCSX-ReARMed, Mupen64Plus-Next GLES2
    lib/*.so*                  # private runtime libraries
    assets/{ozone,audi,pkg}/   # Ozone UI, Audi fonts/wallpaper, fallbacks
    autoconfig/qnx/*.cfg       # controller mappings available without SD
    rumble/qnx/*.cfg           # validated HID output reports
    info/*.info                # seed metadata copied to a blank SD
    ra.sh retroarch.cfg content-rules.cfg  # launcher, factory config, rules
/mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar

# WRITABLE FAT32 SD — portable content and all user/runtime state
/fs/sda0/retroarch/
    config/retroarch.cfg       # writable working copy of factory defaults
    config/retroarch-core-options.cfg  # per-card core settings
    config/remaps/             # controller remaps
    info/*.info                # core metadata + writable core_info.cache
    database/rdb/*.rdb         # compiled game databases
    cheats/**/*.cht            # official cheat collection
    system/                    # BIOS and core system files
    saves/ states/             # SRAM + savestates (safe default)
    ps1/ gba/ n64/ roms/       # game content scanned recursively
    playlists/ thumbnails/ logs/ screenshots/
```
`build/mnt_app/` and `build/sd_card/` mirror these two filesystem roots and can
be copied directly to the unit/card. The app config is an immutable factory
template. On a fresh SD, `ra.sh` copies it to the card and starts RetroArch with
the SD file as its primary `--config`. This is deliberate: RetroArch saves the
primary config, not a minimal `--appendconfig`, so every explicit/exit save is
guaranteed to remain on removable media. Replacing the SD resets configuration,
saves and playlists to the known-good factory state on the next launch.

**Launcher must, at startup:**
1. Remount `/fs/sda0` writable if needed and create its complete RetroArch
   resource/runtime tree.
2. Seed a missing SD config and core-info set from the immutable app layer.
3. Export `RA_DATA_DIR=/mnt/app/root/retroarch` for static assets/rumble and
   `RA_USER_DIR=/fs/sda0/retroarch` for writable state.
4. Start with the SD config as primary. If no SD exists, use a volatile copy in
   `/tmp/retroarch`; never make `/mnt/app` a save target.

Caveats: `/fs/sda0`/`sdb0`/`usb0_0` are **FAT** (`fs-dos.so`) → case-insensitive, no
symlinks, 4 GB/file (all fine for retro), and may come up **ro** (see step 3).
Keep thumbnails/logs bounded so the removable card cannot fill unexpectedly.

### Automatic game playlists

RetroArch starts an internal background task when `RA_CONTENT_RULES` points to
`content-rules.cfg`. It scans only the declared media roots, merges matching
content into one playlist per rule and refreshes Ozone without blocking the UI.
It does not require database hashes and does not ask the user to select a core.
Notifications report scan start, updates, an unchanged library, or errors.

The PS1 rule scans these two directories recursively:

```
/fs/sda0/retroarch/ps1
/fs/sdb0/retroarch/ps1
```

It accepts `.cue`, `.chd` and `.pbp`, ignores track `.bin` files, assigns
PCSX-ReARMed, removes region/revision tags from labels, and retains multi-disc
suffixes. Adding another system/core is a new `ruleN_*` block; scanner code does
not change. Generated `.lpl` files live in the normal playlist directory on the
primary card, and are not rewritten when the detected content is unchanged.
After a successful directory scan, stale entries beneath that directory are
also removed from History and Favorites. An absent card or an unreadable
system directory is not treated as proof of deletion, so temporarily removing
an SD card does not permanently erase its Favorites.

The GBA and N64 rules use the same two-card scheme. Nintendo 64 cartridge dumps
belong in `retroarch/n64/` as `.z64`, `.n64` or `.v64`; they are assigned to the
GLES2 Mupen64Plus-Next core automatically. The optional 64DD BIOS belongs at
`retroarch/system/Mupen64plus/IPL.n64` and is not required for cartridge games.

## Strategy (why RetroArch fits MHI2Q — see gcc49 findings)

- **Cores as separate `.so`** (dlopen) + **griffin unity build** (1 TU → one small
  ELF) → dodges the **~15 MB exec ceiling** (each loadable ELF stays small;
  proven constraint, see the tailscale note in [[gcc49-qnx-port]]).
- Dynamic-link everything (libc, our libstdc++.so.6, cores).
- Big buffers via runtime `mmap` (contiguous mmap is unlimited — measured 240 MB),
  **not** static/BSS (which procnto commits at exec). The QNX Mupen build also
  removes upstream GLSM's unsafe 80 MB uniform-cache array and halves its ARM
  dynarec cache, reducing core BSS from about 145 MB to 44 MB.
- JIT cores (mgba/PPSSPP dynarec) work — **W^X is not enforced on QNX 6.5**
  (measured); remember `__builtin___clear_cache` after emitting.
- Compile with **`-O2 -fno-strict-aliasing -mfpu=neon`**; emulators type-pun
  guest memory (needs no-strict-aliasing) and want NEON. Unaligned access: see the
  SCTLR.A note — prefer target `SCTLR.A=0` over `-mno-unaligned-access`.

## Milestones

- [x] **0. Compiles at all** — **YES.** Current RetroArch HEAD (git `abb7222`) has
      **no GCC-4.9 language blocker.** Bulk test: **178/212 `libretro-common/*.c`
      compile clean** with the `qnx65-sdp-arm` toolchain (`-std=gnu99`). The 34
      "failures" are all config/feature-gated (`cdrom.c` wants `-DHAVE_CDROM` to get
      the vfs struct member, 7z/zstd/chd want external libs, glsym/gl want GL headers)
      — a real build only compiles the enabled set. **No need to pin an old tag.**
      One toolchain nit: a few headers (`array/rhmap.h`) use `ptrdiff_t`/`size_t`
      assuming a transitive `<stddef.h>` that QNX Dinkum headers don't pull → add
      **`-include stddef.h`** globally (or patch those headers). See Build below.
- [x] **1. Skeleton links** — **DONE.** `./build.sh` → `make -f Makefile.griffin
      platform=qnx` links `retroarch`: **1.9 MB stripped** (unstripped 2.2 MB) —
      **13 MB under the 15 MB exec ceiling**, huge margin even before cores go to
      separate `.so`. ELF32 ARM **Version5 EABI, VFPv3, wchar_t=4** = matches the
      firmware ABI; `NEEDED libEGL.so.1 libGLESv2.so.1 libsocket.so.3
      libstdc++.so.6 libm.so.2 libc.so.3` (all on-device); `dlopen` imported
      (dynamic cores). What it took: rewrote the BB10 `qnx` stanza (our GCC not
      `qcc`, drop `-lbps -lscreen -lOpenAL -DHAVE_BB10`), stubbed the 4 bps/screen
      drivers, and fixed 4 portability nits — `GLchar` missing from QNX's pre-2013
      `GLES2/gl2.h`, `screen.h` include in `input_autodetect_builtin.c`, `madvise`
      absent on QNX 6.5, and **dropped `-mfpu=neon`** (gcc-4.9 NEON autovec emits
      the `[rN:64]` align hint that binutils-2.19 gas rejects — a known ceiling).
- [x] **2. Video (code)** — **DONE (compiles; runtime needs HW).** `qnx_ctx.c`
      rewritten off bps/screen onto **libdisplayinit** (`dlopen /eso/lib/
      libdisplayinit.so` → `display_init` + `display_create_window(dpy,cfg,w,h,
      displayable,&win,&kd)` → RetroArch's generic EGL/GLES2 surface). Renders to
      **displayable 43** (`RA_QNX_DISPLAYABLE_ID` override) in private context 90
      (`RA_QNX_CONTEXT_ID`) and routes display 0 only after EGL/window creation.
      Frame-on-screen can only be verified on the HU or the GL-passthrough QEMU.
- [x] **3. Core pipeline** — **DONE.** gpSP, PCSX-ReARMed and the GLES2/ARM-
      dynarec Mupen64Plus-Next build as QNX armle-v7 **DYN, Version5 EABI**
      cores and are loaded through the full
      `retro_*` ABI. The original synthetic test core served its bring-up purpose
      and is intentionally absent from the production source/payload.
- [x] **3.5 Lifecycle / focus — IMPLEMENTED** (builds clean, +2.5 KB → 1.96 MB).
      `frontend/drivers/platform_qnx.c`: signals **blocked in every thread**
      (`pthread_sigmask` before RetroArch spawns its threads) + a dedicated
      **`sigwaitinfo()` lifecycle thread** that only flips
      `qnx_lifecycle_quit` / `qnx_lifecycle_paused` — **no work in an async
      handler**; wired into the 4 `frontend_ctx` sighandler slots. Single-instance
      **`flock`** on `/fs/sda0/retroarch/ra.lock` (`FD_CLOEXEC`, EINTR-retry,
      `ENOSYS` loud-fatal, 8 s wait then escalate, PID = diagnostic only).
      `gfx/drivers_context/qnx_ctx.c` reconciles once per frame on the main
      thread: `*quit = frontend_driver_get_signal_handler_state()` (→ RetroArch's
      SRAM-flush + core-unload exit) and `CMD_EVENT_PAUSE`/`UNPAUSE` on desired-
      state change; `swap_buffers` **skips `eglSwapBuffers` while paused → 0 GPU**.
      Env: `RA_LOCK_PATH`, `RA_QNX_DISPLAYABLE_ID`, `RA_DATA_DIR`, `RA_USER_DIR`.
      Verified statically (symbols `sigwaitinfo`/`flock`/`pthread_sigmask` imported,
      strings present). **Still needs on-HW checks:** flock support on the real
      `/fs/sda0`, and the SIGKILL-orphan matrix (displayable/audio reclaim).
- [x] **4. Input — HID + XUSB/GIP gamepads — IMPLEMENTED** (builds clean;
      stripped frontend 2.16 MB). `input/drivers_joypad/qnx_joypad.c` is a native
      QNX HID DDI backend built against the **real `<sys/hiddi.h>`** and linked to
      `libhiddi.so.1`. It recursively walks application collections, scans all 32
      (possibly sparse) input-report indices, attaches every unique report and
      keeps their state separate before aggregation. This fixes the real HU pad,
      whose only usable input is **report index 1**, and supports hot-plug plus up
      to `MAX_USERS` pads.

      The normal path uses public `hidd_get_all_buttons` / `hidd_get_buttons` /
      `hidd_get_usage_value`. QNX 6.5's preparser reports zero values for the HU's
      otherwise valid 9-byte gamepad report, so the fallback reads the original
      HID report descriptor and parses its standard Main/Global/Local items. It is
      still generic: Button-page variable/array fields (up to 256 buttons), X/Y/Z/
      Rx/Ry/Rz/Slider/Dial axes and four native hat switches are decoded by their
      descriptor bit offsets, report IDs and logical ranges. The private descriptor
      symbol is only used after an exact HIDDI v1.00 server check; the staged QNX
      6.5 library exports it. A narrow SDL-derived decoder handles the verified
      8BitDo 9-byte and enhanced reports while preserving the same physical DInput
      ordering. It also reads the 8BitDo feature report that enables enhanced mode.

      Vendor-specific Xbox devices use a second transport in
      `input/drivers_joypad/qnx_xusb.c`, linked to QNX `libusbdi.so.2`: wired Xbox
      360/XUSB (including 8BitDo/GameSir/etc. XInput modes), the four-port Xbox 360
      wireless receiver, and Xbox One/Series GIP. It matches the exact class/
      subclass/protocol plus SDL's maintained vendor allowlists, opens only the
      selected interrupt/bulk endpoints, decodes input, handles receiver presence,
      GIP announce/identify/ACK/startup, hot-plug and two-motor rumble. Unknown
      vendor-specific interfaces are detached immediately. A built-in name-matched
      XInput profile gives third-party VID/PIDs standard A/B/X/Y, hat, sticks and
      combined trigger axis without waiting for a new database entry.

      Physical HID controls are deliberately kept separate from RetroPad semantics.
      Mapping lives in external files under `autoconfig/qnx/`; 234 QNX
      Libretro DirectInput/HID profiles are mechanically retargeted to `qnx` and
      stay in the immutable app layer. User changes are stored as remaps under
      `/fs/sda0/retroarch/config/remaps`, so replacing the SD preserves the safe
      controller database while resetting user overrides.
      Unknown products use `QNX HID Gamepad.cfg`; exact product-name or VID/PID
      profiles take precedence. `RA_QNX_HID_DEBUG=1` logs
      topology and `RA_QNX_HID_DUMP=1` logs only the first eight packets per report.
      Timestamped logs are saved under `/fs/sda0/retroarch/logs/`. DSI keypad stays
      out.
      HID rumble uses output reports and external profiles under
      `rumble/qnx/<vid>_<pid>_<report-id>.cfg`. The driver validates report ID,
      actual descriptor length and every configured offset before attaching the
      output handle; unknown layouts are never guessed. The shipped profiles cover
      DS3, DS4 v1/v2, DualSense, DualSense Edge, SDL-supported 8BitDo products and
      Microsoft Xbox One/Elite/Series Bluetooth IDs. The Sony Bluetooth profiles
      include sequence tags and CRC32. XUSB/GIP rumble is part of its fixed protocol
      backend, not a guessed HID profile. More HID controllers can be added as data
      after their output reports are verified, without rebuilding RA.
      **Bluetooth comes free:** io-hid aggregates HID regardless of transport and
      the firmware's `eso/bin/apps/btstack` speaks **HIDP** (its profile list:
      a2dp, avrcp, HFP, **HIDP**, LE, MAP, OPP, PAN, PBAP) — so a BT pad arrives
      through the *same* callbacks, zero extra code. *(Open question for HW: whether
      the stock HMI pairing UI will pair a non-phone HID device, or whether pairing
      must be driven from `btstack` directly.)*
      **Debug on target:** `/usr/sbin/hidview` (ships in firmware) uses this exact
      API and dumps collections/usages/report data — use it to see what a given pad
      actually reports before fighting the driver.
      Verified statically: `libhiddi.so.1` and `libusbdi.so.2` in `NEEDED`; the
      expected `hidd_*` and `usbd_*` APIs are imported from the QNX 6.5 SDK.
      Both libraries are in the extracted MHI2Q firmware (`libusbdi.so.2` is in
      `ifs_coreservices3/lib`; the firmware's own `io-hid`/`hidview` use HIDDI).
      `libusbdi` is therefore not bundled; the package retains its known-good
      compatibility copy of `libhiddi.so.1`. The new raw Xbox path and every
      rumble family still require controller-by-controller validation on the HU.
- [x] **5a. Audio — QSA PCM driver IMPLEMENTED** (builds, +2.7 KB → 1.97 MB).
      `audio/drivers/qnx_qsa.c`, registered as audio driver **`qsa`** (extern in
      `audio_driver.h`, `&audio_qsa` in `audio_drivers[]` under `HAVE_QSA`, griffin
      include, `-lasound`). Default device `/dev/snd/mpl1_int_ent`
      (`RA_QNX_AUDIO_DEV` overrides). Carries over gpSP's hard-won QSA details:
      `snd_pcm_plugin_set_disable(PLUGIN_DISABLE_MMAP)` for consistent DMA with
      `snd_pcm_write`, `snd_pcm_channel_info` to take the device's **native voice
      count** (mpl1 is **6-channel**, mpl5 is 2 — so stereo is **upmixed** here,
      otherwise you get garbage/silence), format probe preferring S16_LE, and
      underrun recovery (`channel_status` → `channel_prepare` → silence prefill).
      **Does not touch `MS_ENT`** — by design. Verified: `libasound.so.2` in
      `NEEDED`, 11 `snd_pcm_*` symbols imported.
- [x] **5b. Audio — OEM entertainment focus/route IMPLEMENTED.** Java 1.4
      `AudioFocusBridge` uses the already-running stock LSD audio bundle: it
      selects the exact `HMIAudioService` with `AUDIO_CLIENT_ID=1`, registers a
      Media listener, calls `requestAndFadeToConnection(20,0)`, selects focus app
      2/media context 3 and configures EXTERNAL/NONE, virtual channel 1 → MPL1.
      Focus-loss callbacks send one QNX `SIGRTMIN` edge; the runloop pauses once
      with no auto-resume. Every disconnect releases connection 20, stops and
      unregisters the route, and clears focus/context before SIGTERM. Native QSA
      is PCM-only. No `framework.json` edit, native DSI client, UDP bridge or
      manual `MS_ENT` change.
      The JAR builds with class major 48 and contains no boot-critical SMM/factory
      shadows. **On-HU validation still required:** confirm `ACTIVE` in
      `/tmp/ra_audio.log`, audible PCM regardless of the previously selected HMI
      source, correct return to radio/media, and phone/nav/PDC interruption.
- [x] **5c. Full frontend data/features IMPLEMENTED.** The QNX build enables
      `HAVE_LIBRETRODB`, `HAVE_CORE_INFO_CACHE` and `HAVE_SCREENSHOTS`.
      7zip support is intentionally omitted to keep the HU build lean. External
      package data contains all 306 official core
      info files, all 145 compiled Libretro RDBs, and five GLES2 GLSL presets
      (nearest, bilinear, sharp-bilinear, scanline, CRT-PI). These resources and
      screenshots live under `/fs/sda0/retroarch`; appimg stays minimal.
- [x] **5d. Cheat database IMPLEMENTED.** `HAVE_CHEATS` is enabled and the
      package carries all 28,301 upstream `libretro-database/cht` files for 44
      systems (commit `6fd53f98`, CC BY-SA 4.0) directly in the SD payload at
      `/fs/sda0/retroarch/cheats`; there is no appimg duplicate or first-launch
      copy.

## Build

### Isolated macOS UI test

Run `./run-macos.sh` to build and open the Audi/Ozone frontend at 1024x480.
All generated binaries, cores, saves, playlists and logs stay under
`out/macos-test`; the test only reads games and BIOS from the canonical
`build/sd_card/retroarch` tree. The generated application bundle is
`out/macos-test/RetroArchTest.app`.

The test uses native arm64 gpSP and PCSX-ReARMed cores and the same external
UI assets as the QNX package. Closing its window does not deploy anything to,
or restart, the head unit.

Toolchain: **`../qnx-65-sdp-docker/host-scripts/qnx-run.sh`** (Docker image **`qnx65-sdp-arm`** — the
consolidated polyglot SDP image: GCC 4.9.4 built from source + Go + Rust, all merged
into the SDP host tree). It mounts `$PWD → /src`, so run it from this `src/` dir.
Compiler is `arm-unknown-nto-qnx6.5.0eabi-gcc` (GCC 4.9.4). (The old standalone
`qnx-gcc49`/`qnx65-gcc49` image is retired — same 4.9.4 port, now baked into the SDP
image.)

**Verified M0 smoke-compile** (single file, no link):
```sh
TC=../../qnx-65-sdp-docker/host-scripts/qnx-run.sh
$TC arm-unknown-nto-qnx6.5.0eabi-gcc -std=gnu99 -c -include stddef.h \
    -Ilibretro-common/include -I. -DHAVE_QNX -D__QNX__ \
    libretro-common/file/config_file.c -o /tmp/o.o        # → OK
```

Full-build plan (M1): `make -f Makefile.griffin platform=unix` won't know QNX —
easier to drive the main `Makefile.common` griffin path with an explicit cross
`CC`/`CXX` and a hand-written config (skip `./configure`, which host-probes and
can't cross). Add `-include stddef.h` to global `CFLAGS`. Start with
null/dummy video+audio+input to get a linking skeleton, then measure the ELF
against the 15 MB exec ceiling before wiring real drivers.
