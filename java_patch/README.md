# RetroArch full SystemSMM state (MHI2Q / MU1316)

This patch adds **Games** to the main HMI menu and, on first activation,
reflectively appends a real state and two transitions to the already initialized
OEM `SystemSMM`:

```text
MainWizard state 89 --EV_ENTER 9990001--> RetroArch state 631 / screen 250
RetroArch state 631 --EV_EXIT  9990002--> MainWizard state 89
```

The native process lifecycle belongs to the custom state: connecting screen 250
launches RetroArch; the native EGL driver declares and routes private display
context 90 containing only video displayable 43. Unlike stock context 25
(`{HMI 16, video 43}`), this excludes the lower HMI status bar. Disconnecting
the screen restores the previous context and sends `SIGTERM` to the PID in
`/tmp/retroarch.lock`. Thus BACK and any OEM-forced transition away from the
state use the same cleanup path.

The same lifecycle owns the HMI half of the OEM entertainment audio session. On
connection a daemon worker selects the stock Media `HMIAudioService` by
`AUDIO_CLIENT_ID=1`, registers a normal listener, requests and fades to Media/MFP
connection 20, selects focus app 2/media context 3 and registers the external
MPL1 route. On every disconnection it releases connection 20, stops/unregisters
the route and clears focus/context. QSA only carries PCM samples. Listener
pause/stop/error events for connection 20 send `SIGRTMIN` to RetroArch for a
one-shot pause with no auto-resume. No `framework.json` edit or native DSI client
is required. Diagnostics are written to `/tmp/ra_audio.log`.

## Why this does not break HMI startup

The JAR deliberately does **not** shadow these boot-critical OEM classes:

- `de.audi.tghu.system.sm.SystemSMMInitStates`
- `de.audi.tghu.system.sm.SystemSMMInitTransitions`
- `de.audi.tghu.system.hmi.evohigh.SystemScreenFactory`

The stock tables and factory initialize normally. Injection is deferred until
the user first selects Games. Before changing anything, the injector validates
the exact MU1316 fingerprint: 631 states, 890 transitions, and the known state
89 superstate/screen/trigger/transition values. Any mismatch fails closed: it
does not fire `EV_ENTER`, and partial changes are rolled back.

After swapping the SMM tables, the injector also refreshes the live SMI
`activeStateStack`. This is required because the interpreter's active `State`
objects retain the trigger arrays captured during HMI startup; without the
refresh, a newly appended `EV_ENTER` exists in `SystemSMM` but the first Games
click is silently ignored by the already-active state 89.

The sole OEM shadow is the small, ABI-compatible anonymous
`AbstractPlaceholderMenuController$1` wrapper used to add and intercept the
Games row. Screen 250 is a new class and is preloaded into the OEM `ScreenCache`;
the OEM screen factory and all OEM audio/DSI classes stay untouched.

## Build

From the repository root:

```sh
./lsd_patch/build.sh
```

Canonical output: `lsd_patch/ra_mhi2q.jar`.

`java_patch/build_java.sh` is retained as a compatibility entry point and emits
`java_patch/ra_games_hook.jar` through the same safe build.

The build uses Java 1.4 source/target and the exact MU1316 device `jcl.jar`. It
fails if a core shadow appears, a required runtime class is missing, the class
major is not 48, or the unsupported device call `Integer.valueOf(int)` is found.

Optional overrides: `JAVA_HOME=`, `LSD_JAR=`, `JCL_JAR=`, `OSGI_JAR=`,
`OUT_JAR=`.

## Runtime diagnostics

The injector writes concise records to stdout and `/tmp/ra_hook.log`. Expected
first-entry milestones are:

```text
[SMM] found live SystemSMM
[SMM] preloaded RaScreen ID 250 into OEM ScreenCache
[SMM] refreshed live SMI stack
[SMM] runtime SMM install PASS: state=631 enterTrans=890 exitTrans=891 screen=250
fired main SystemSMM EV_ENTER=9990001
RA state connected
```

RetroArch stdout/stderr goes to `/tmp/ra_run.log`.

## Device contract

`/mnt/app/root/retroarch/ra.sh` must exist and export the same lock path used by
the hook:

```sh
export RA_LOCK_PATH=/tmp/retroarch.lock
```

The native QNX platform code writes RetroArch's own PID there, so cleanup never
accidentally targets an intermediate shell process.
