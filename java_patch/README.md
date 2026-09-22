# RetroArch HMI hook (Java 1.4, MU1316)

Sources for `lsd_patch/ra_mhi2q.jar`: a **Games** MainWizard entry, a runtime-injected
SystemSMM state (631 / screen 250) whose screen owns the native RetroArch lifecycle, and
the OEM entertainment-audio session (focus app 2, connection 20, MPL1 route).

Build: `./lsd_patch/build.sh` (or the compatibility wrapper `java_patch/build_java.sh`).

Documentation lives in the Obsidian vault `docs/retroarch-qnx/`:

- `hmi/games-menu-injection.md` — the single OEM shadow, table fingerprint, RaScreen
- `hmi/session-lifecycle.md` — connect/disconnect, `/tmp` markers, watchers, re-entry
- `hmi/audio-session.md` — AudioFocusBridge / MediaSessionBridge sequence, recovery, release
- `hmi/display-context-90.md` — context `{HMI 16, video 43}` and why the display manager is not patched
- `build/java-jar-build.md` — build inputs and the fail-closed bytecode gates

Device contract: `/mnt/app/root/retroarch/ra.sh` must export `RA_LOCK_PATH=/tmp/retroarch.lock`
(the hook reads the native PID from it), see `deploy/launcher-ra-sh.md`.
