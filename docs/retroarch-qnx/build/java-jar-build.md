---
title: lsd_patch/build.sh - the fail-closed jar build
tags: [build, java, hmi]
status: verified-source
sources:
  - lsd_patch/build.sh
  - java_patch/build_java.sh
  - java_patch/README.md "Build"
reconciles:
  - java_patch/README.md
---

# lsd_patch/build.sh - the fail-closed jar build

Output: `lsd_patch/ra_mhi2q.jar` (deployed to `/mnt/app/eso/hmi/lsd/jars/`). `java_patch/build_java.sh`
is a compatibility wrapper producing `java_patch/ra_games_hook.jar` from the same script.

## Inputs (overridable by env)

| Env | Default | What |
|---|---|---|
| `JAVA_HOME` | `Tools/jxe2jar/jvms/zulu8.78...` | JDK 8 (`javac -source 1.4 -target 1.4`) |
| `LSD_JAR` | `Tools/jxe2jar/out/MU1316-combined-final.jar` | decompiled/recovered stock HMI classes (compile classpath) |
| `JCL_JAR` | `Tools/jxe2jar/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar` | the **device** J9 class library, used as `-bootclasspath` |
| `OSGI_JAR` | `Firmwares/HU/MU1367.../osgi.jar` | OSGi API (optional) |
| `OUT_JAR` | `lsd_patch/ra_mhi2q.jar` | output |

Compiling against the device JCL is what makes `Integer.valueOf(int)` and other post-1.4 calls a
build error instead of a runtime `NoSuchMethodError` on the unit.

## Gates (any failure aborts)

1. **Forbidden shadows absent** - the jar must not contain `SystemSMMInitStates`,
   `SystemSMMInitTransitions`, `SystemScreenFactory`, `AudioActivator`, `BaseAudioService`,
   `DSIAudioListenerImpl`, `HMIAudioService`, `DSIMediaRouter`. Shadowing any of them would replace a
   boot-critical or audio-owner OEM class ([[games-menu-injection]], [[audio-session]]).
2. **Required classes present** - `AbstractPlaceholderMenuController$1`, `RaScreen`,
   `AudioFocusBridge`, `DsiReflection`, `MediaSessionBridge`, `RuntimeSmmInjector`.
3. **Class major = 48** (Java 1.4) for every injected class.
4. **Bytecode contracts** (via `javap -c`):
   - `MediaSessionBridge` calls `IAudioManager.requestAudio`, `IAudioManager.requestEntSuppression`,
     `CombiBAPServiceMedia.updateCurrentStation`, `CombiBAPServiceMedia.updateActiveInfoState`;
   - `RuntimeSmmInjector` calls `reinitActiveStateStack` (the live SMI refresh);
   - `AudioFocusBridge` calls `HMIAudioService.requestConnection/fadeToConnection/releaseConnection`,
     `ATIPMediaRouterService.setAudioRoutes`, `ILastmodeHandler.getLastmodeAudio`,
     `IAudioFocusManager.setActiveAudioApp`;
   - `AudioFocusBridge` must **not** call `DSIMediaRouter.registerClient/requestConfiguration/
     startStreaming` or `SdisAudioService.setAudioContext` (competing owners);
   - no `Integer.valueOf(I)` anywhere in the injector/hook;
   - no `retroarch-agent.fragment` string (an obsolete native-DSI fragment design).

The script ends with `PASS` and prints the runtime constants:
`RA_SCREEN=250 DRUM_STATE=89 EV_ENTER=9990001 EV_EXIT=9990002`.

## Coexistence with the CarPlay jar

Both `ra_mhi2q.jar` and `carplay_hook.jar` live in the same jars directory and must never carry the
same stock class. That is why context 90 is declared natively with `dmdt` and not by patching
`DisplayManagerMIB2High` here: [[display-context-90]].
