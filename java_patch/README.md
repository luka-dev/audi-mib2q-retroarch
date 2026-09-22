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
context 90 as `{HMI 16, video 43}`. The transparent HMI plane keeps stock global
partial popups (including volume) above the game, while the screen's style-2
status-bar stub suppresses the lower bar. Disconnecting the screen restores the
previous context and sends `SIGTERM` to the PID in `/tmp/retroarch.lock`. Thus
BACK and any OEM-forced transition away from the state use the same cleanup path.

The same lifecycle owns the HMI half of the OEM entertainment audio session. It
registers an `IMediaTerminalExtension` and receives the live front
`IMediaTerminal`, then records connection 20 as the stock Media application's
own active audio context before requesting focus app 2. This prevents a late
Media focus callback from restoring the old no-playable/suppression context 9
and stopping RetroArch connection 20 when entering from CarPlay focus 48. A
daemon worker selects the stock Media `HMIAudioService` by `AUDIO_CLIENT_ID=1`,
registers normal Media, focus and ATIP-route listeners, starts RetroArch/QSA and
waits for its successful silent PCM prefill, selects focus app 2, confirms
Media/MFP connection 20, and routes internal media to MPL1 through the stock
`ATIPMediaRouterService`. Starting the
PCM producer first is required on MU1316: an entertainment connection without
a producer is paused again before routing can complete. The HMI fades in only
after STARTED + route confirmation. On disconnection SIGTERM is
sent first, then the captured focus/connection and any route actually observed
by the ATIP listener are restored after native PCM closes. The ATIP service has
no route getter, so an unobserved old route is left for its OEM owner when focus
returns. Once active, the Media BAP connector receives `RetroArch / Playing`
and `InfoState=0`, so the VC no longer inherits `NO_PLAYABLE_FILES`. Focus or
connection loss pauses the core and stops QSA but never exits the foreground RA
state; focus recovery restarts sound but never auto-resumes gameplay. The
stop/start signals carry a shared
desired-state file, and native-exit markers are session-specific. Rapid re-entry
waits for the old QSA close; a process that misses the bounded exit timeout
blocks relaunch rather than allowing two PCM writers. The launcher remains the
foreground parent of RetroArch and explicitly waits for it, while the JVM reaps
its shell; a threadless QNX `/proc/PID` zombie is not treated as a live PCM
owner. No SDIS context, raw router owner, `framework.json` edit or native DSI
client is required. Diagnostics persist under `/fs/sda0/retroarch/logs` (with
`/tmp` fallback). Hook/audio files rotate at 256 KiB, the run log at 512 KiB,
and only eight timestamped frontend sessions are retained.

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

The injector writes concise records to stdout and
`/fs/sda0/retroarch/logs/ra_hook.log` (`/tmp` without an SD). Expected
first-entry milestones are:

```text
[SMM] found live SystemSMM
[SMM] preloaded RaScreen ID 250 into OEM ScreenCache
[SMM] refreshed live SMI stack
[SMM] runtime SMM install PASS: state=631 enterTrans=890 exitTrans=891 screen=250
fired main SystemSMM EV_ENTER=9990001
RA state connected
```

RetroArch stdout/stderr goes to `/fs/sda0/retroarch/logs/ra_run.log` (`/tmp`
without an SD).

## Device contract

`/mnt/app/root/retroarch/ra.sh` must exist and export the same lock path used by
the hook:

```sh
export RA_LOCK_PATH=/tmp/retroarch.lock
```

The native QNX platform code writes RetroArch's own PID there, so cleanup never
accidentally targets an intermediate shell process.
