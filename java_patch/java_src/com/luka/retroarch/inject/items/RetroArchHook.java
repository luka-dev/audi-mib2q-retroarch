package com.luka.retroarch.inject.items;

import java.io.File;

import de.audi.atip.hmi.HMITerminal;
import de.audi.atip.hmi.event.KeyEvent;
import de.audi.atip.hmi.view.IDisplayManager;
import de.esolutions.hmi.widgets.audi.base.AbstractWidget;
import de.esolutions.hmi.widgets.audi.base.eal.EALManager;
import de.esolutions.hmi.widgets.audi.base.eal.HMITerminalEAL;
import de.esolutions.hmi.widgets.audi.evo.widgets.AbstractPlaceholderMenuController;
import de.esolutions.hmi.widgets.audi.evo.widgets.MainWizardController;
import de.esolutions.hmi.widgets.audi.evo.widgets.menu.MenuItemController;
import com.luka.retroarch.inject.IMenuHook;
import com.luka.retroarch.inject.Shell;
import com.luka.retroarch.inject.audio.AudioFocusBridge;
import com.luka.retroarch.inject.ids.CustomWidgetIds;
import com.luka.retroarch.inject.ids.IdAllocator;
import com.luka.retroarch.inject.ids.MainWizardWidgetIds;
import com.luka.retroarch.inject.sm.RuntimeSmmInjector;

/**
 * Main Wizard entry for the runtime-injected RetroArch SystemSMM state.
 *
 * The menu hook only creates the Games row and requests SM events.  Launch,
 * display ownership and cleanup belong to RaScreen.connected/disconnecting,
 * so every OEM transition away from the RA state performs the same cleanup.
 */
public final class RetroArchHook implements IMenuHook {
    private static final long MAX_DIAGNOSTIC_LOG_BYTES = 256L * 1024L;
    private static final int GAMES_WIDGET_ID = CustomWidgetIds.GAMES;
    private static final int TERMINAL_CENTER = 0;
    private static final int CLEAR_TRANSPARENT = 0;
    private static final int CLEAR_OPAQUE = 2;

    /* Backward-compatible constants used by deployment notes/tools. */
    public static final int RA_SCREEN_ID = RuntimeSmmInjector.RA_SCREEN_ID;
    private static final int EV_ENTER = RuntimeSmmInjector.EV_ENTER;
    private static final int EV_EXIT = RuntimeSmmInjector.EV_EXIT;

    private static final String EXIT_MARKER_PREFIX = "/tmp/retroarch.exited.";
    private static final String RETROARCH_LOCK = "/tmp/retroarch.lock";
    private static final String PCM_READY_MARKER = "/tmp/retroarch.pcm.ready";
    private static final String AUDIO_DESIRED_MARKER =
            "/tmp/retroarch.audio.desired";
    private static volatile boolean raRunning;
    private static volatile boolean nativeStarted;
    private static volatile boolean audioAcquisitionStarted;
    private static volatile boolean exitRequested;
    private static volatile int savedContext = -1;
    private static volatile int sessionGeneration;
    private static volatile int pendingShutdownGeneration = -1;
    private static volatile int stuckShutdownGeneration = -1;

    public void onWrapperConstructed(AbstractPlaceholderMenuController outer,
                                     MenuItemController menuItem) {
        if (!(outer instanceof MainWizardController) || menuItem == null) return;
        if (menuItem.getWidgetID() != MainWizardWidgetIds.SETTINGS) return;
        if (hasGamesItemAlready(outer)) return;

        try {
            MenuItemController games = new MenuItemController();
            games.setEvent(EV_ENTER);
            games.setLabelId(0); /* wrapper supplies the literal Games label */
            games.setColorIndices(menuItem.getColorIndices());
            games.setGlassplateInsetsBottom(6);
            games.setGlassplateInsetsTop(7);
            games.setInternalID(IdAllocator.nextFreeInternalId(outer, 2000));
            games.setKeyHandler(3);
            games.setSdsCommand(0);
            games.setSdsItemSelectedAction(2);
            games.setType(2);
            games.setWidgetID(GAMES_WIDGET_ID);
            outer.add(games);
            log("injected Games MainWizard item (widgetID=" + GAMES_WIDGET_ID
                    + ", event=" + EV_ENTER + ")");
        } catch (Throwable t) {
            log("Games menu injection failed: " + t);
        }
    }

    public boolean onKeyPressed(AbstractPlaceholderMenuController outer,
                                MenuItemController menuItem, KeyEvent event) {
        if (!(outer instanceof MainWizardController) || menuItem == null || event == null) {
            return false;
        }
        if (menuItem.getWidgetID() != GAMES_WIDGET_ID || !isMenuEnter(event)) return false;

        try { event.consume(); }
        catch (Throwable ignored) {}

        if (raRunning) {
            log("ignored duplicate Games Enter while RA state is active");
            return true;
        }

        if (!RuntimeSmmInjector.ensureInstalled(outer)) {
            log("Games Enter aborted: runtime SystemSMM injection unavailable");
            return true;
        }

        try {
            AbstractWidget.hmiService.fireSMEvent(TERMINAL_CENTER, EV_ENTER);
            log("fired main SystemSMM EV_ENTER=" + EV_ENTER);
        } catch (Throwable t) {
            log("EV_ENTER failed: " + t);
        }
        return true;
    }

    /** Called only when the runtime RA state becomes the connected main screen. */
    public static synchronized void onRaScreenConnected(HMITerminal terminal) {
        final int watcherGeneration;
        if (raRunning) return;
        raRunning = true;
        nativeStarted = false;
        audioAcquisitionStarted = false;
        exitRequested = false;
        savedContext = -1;
        watcherGeneration = ++sessionGeneration;

        try {
            IDisplayManager display = AbstractWidget.hmiService.getDisplayManager();
            savedContext = display.getCurrentContextID(TERMINAL_CENTER);
        } catch (Throwable t) {
            log("RA connect: could not read previous display context: " + t);
        }

        startAudioAcquisitionWhenReady(watcherGeneration);
        try {
            setClearMethod(terminal, CLEAR_TRANSPARENT);
            /* The EGL driver routes private context 90 only after displayable
             * 43/window creation. It contains HMI 16 above video 43 so stock
             * partial popups remain visible. IDisplayManager cannot switch 90
             * because the OEM Java table only contains contexts 0..78. */
            log("RA state connected: acquiring OEM audio, saved context "
                    + savedContext + ", native route=90{16,43}, clear=transparent");
        } catch (Throwable t) {
            log("RA connect display setup failed: " + t);
        }
    }

    /** Called for BACK and for every forced OEM transition away from the RA state. */
    public static synchronized void onRaScreenDisconnecting(HMITerminal terminal) {
        if (!raRunning) return;
        boolean hadNative = nativeStarted;
        boolean hadAudioAcquisition = audioAcquisitionStarted;
        int nativeGeneration = sessionGeneration;
        int audioReleaseToken = hadAudioAcquisition
                ? AudioFocusBridge.beginRelease() : -1;
        raRunning = false;
        nativeStarted = false;
        audioAcquisitionStarted = false;
        exitRequested = false;
        sessionGeneration++; /* retires the current native-exit watcher */

        try {
            setClearMethod(terminal, CLEAR_OPAQUE);
            if (savedContext >= 0) {
                AbstractWidget.hmiService.getDisplayManager()
                        .switchContext(savedContext, TERMINAL_CENTER, null);
            }
        } catch (Throwable t) {
            log("RA disconnect display restore failed: " + t);
        }

        if (hadNative) {
            pendingShutdownGeneration = nativeGeneration;
            Shell.terminateRetroArch();
            startAudioReleaseWatcher(audioReleaseToken, nativeGeneration);
        } else if (hadAudioAcquisition) {
            AudioFocusBridge.finishRelease(audioReleaseToken);
        }
        log("RA state disconnected: restored context " + savedContext
                + (hadNative ? ", sent SIGTERM; audio release waits for QSA close"
                        : hadAudioAcquisition
                                ? ", released pre-native audio acquisition"
                                : ", prior native shutdown still owns audio release"));
        savedContext = -1;
    }

    /** BACK/MENU asks the main SystemSMM to take the RA -> MainWizard transition. */
    public static synchronized void requestRaExit() {
        if (exitRequested) return;
        exitRequested = true;
        try {
            AbstractWidget.hmiService.fireSMEvent(TERMINAL_CENTER, EV_EXIT);
            log("fired main SystemSMM EV_EXIT=" + EV_EXIT);
        } catch (Throwable t) {
            exitRequested = false; /* permit another BACK attempt */
            log("EV_EXIT failed: " + t);
        }
    }

    /** Compatibility with the earlier RaScreen implementation. */
    public static void raExit() {
        requestRaExit();
    }

    /**
     * A RetroArch menu Quit does not generate an HMI key event, so without a
     * native-exit bridge the process disappears while SystemSMM remains in the
     * transparent RA state. The shell wrapper creates a generation-specific
     * exit marker whenever the native process ends (clean quit or crash); this
     * bounded Java watcher then requests the same EV_EXIT transition as BACK.
     */
    private static void startAudioAcquisitionWhenReady(final int generation) {
        synchronized (RetroArchHook.class) {
            if (!raRunning || generation != sessionGeneration) return;
            if (pendingShutdownGeneration < 0) {
                startAudioAcquisition(generation);
                return;
            }
        }

        Thread waiter = new Thread(new Runnable() {
            public void run() {
                for (;;) {
                    synchronized (RetroArchHook.class) {
                        if (!raRunning || generation != sessionGeneration) return;
                        if (pendingShutdownGeneration < 0) break;
                    }
                    try { Thread.sleep(50L); }
                    catch (InterruptedException ignored) { return; }
                }
                startAudioAcquisition(generation);
            }
        });
        waiter.setName("retroarch-prior-exit-waiter");
        waiter.setDaemon(true);
        waiter.start();
        log("RA re-entry waits for prior native PCM close");
    }

    private static synchronized void startAudioAcquisition(final int generation) {
        if (!raRunning || generation != sessionGeneration
                || audioAcquisitionStarted || pendingShutdownGeneration >= 0)
            return;
        if (stuckShutdownGeneration >= 0) {
            File oldMarker = new File(exitMarker(stuckShutdownGeneration));
            if (oldMarker.exists()) {
                try { oldMarker.delete(); }
                catch (Throwable ignored) {}
                stuckShutdownGeneration = -1;
            } else if (!isRecordedNativeProcessAlive()) {
                log("clearing stale native-exit latch: no live PID in "
                        + RETROARCH_LOCK);
                stuckShutdownGeneration = -1;
            } else {
                log("refusing relaunch: prior native process missed exit timeout");
                Shell.terminateRetroArch();
                requestRaExit();
                return;
            }
        }
        audioAcquisitionStarted = true;
        try {
            new File(exitMarker(generation)).delete();
            new File(PCM_READY_MARKER).delete();
            new File(AUDIO_DESIRED_MARKER).delete();
        } catch (Throwable ignored) {}

        /* The stock sequence is focus -> connection STARTED -> route -> PCM
         * prefill -> fade. AudioFocusBridge invokes launch only at route. */
        AudioFocusBridge.request(RetroArchHook.class.getClassLoader(),
                new Runnable() {
                    public void run() { launchNative(generation); }
                },
                new Runnable() {
                    public void run() {
                        log("audio activation failed: returning to MainWizard");
                        requestRaExit();
                    }
                });
    }

    private static String exitMarker(int generation) {
        return EXIT_MARKER_PREFIX + generation;
    }

    private static String buildLaunchCommand(int generation) {
        String exitMarker = exitMarker(generation);
        return "rm -f " + exitMarker + " " + PCM_READY_MARKER + " "
                + AUDIO_DESIRED_MARKER
                + "; trap ': > " + exitMarker + "' 0"
                /* ra.sh remounts/probes the SD before rebinding stdout to its
                 * persistent log.  Its bootstrap redirection must therefore
                 * never target the possibly read-only SD itself. */
                + "; /bin/sh /mnt/app/root/retroarch/ra.sh"
                + " </dev/null >/tmp/ra_bootstrap.log 2>&1"
                + "; : > " + exitMarker;
    }

    /** QNX retains /proc/PID for an unreaped process after its final thread is
     * gone. Presence of that directory alone therefore does not mean the
     * native process can still own EGL/QSA/HID resources. */
    private static boolean hasLiveThreads(int pid) {
        Process process = null;
        java.io.BufferedReader reader = null;
        boolean found = false;
        try {
            process = Runtime.getRuntime().exec(new String[] {
                    "/bin/pidin", "-p" + pid, "threads"
            });
            reader = new java.io.BufferedReader(new java.io.InputStreamReader(
                    process.getInputStream()));
            String prefix = Integer.toString(pid);
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith(prefix)
                        && line.length() > prefix.length()
                        && Character.isWhitespace(line.charAt(prefix.length()))) {
                    found = true;
                }
            }
            process.waitFor();
            return found;
        } catch (Throwable t) {
            /* A diagnostic failure is unknown, so retain the safe fail-closed
             * behaviour and do not risk launching a second PCM writer. */
            return true;
        } finally {
            if (reader != null) {
                try { reader.close(); }
                catch (Throwable ignored) {}
            }
            if (process != null) {
                try { process.getErrorStream().close(); }
                catch (Throwable ignored) {}
                try { process.getOutputStream().close(); }
                catch (Throwable ignored) {}
            }
        }
    }

    /** A missing marker is only dangerous while its recorded PID has a live
     * thread. A threadless QNX zombie is safe to clear and must not latch Games
     * off forever. */
    private static boolean isRecordedNativeProcessAlive() {
        File lock = new File(RETROARCH_LOCK);
        if (!lock.isFile()) return false;

        java.io.BufferedReader reader = null;
        try {
            reader = new java.io.BufferedReader(new java.io.FileReader(lock));
            String line = reader.readLine();
            if (line == null) return false;
            line = line.trim();
            if (line.length() == 0) return false;
            int pid = Integer.parseInt(line);
            return pid > 0 && new File("/proc/" + pid).exists()
                    && hasLiveThreads(pid);
        } catch (Throwable t) {
            /* Malformed/unreadable non-empty lock is unknown, so fail closed. */
            return lock.length() > 0;
        } finally {
            if (reader != null) {
                try { reader.close(); }
                catch (Throwable ignored) {}
            }
        }
    }

    private static void startNativeExitWatcher(final int generation) {
        Thread watcher = new Thread(new Runnable() {
            public void run() {
                File marker = new File(exitMarker(generation));
                for (;;) {
                    synchronized (RetroArchHook.class) {
                        if (!raRunning || generation != sessionGeneration) return;
                    }

                    if (marker.exists()) {
                        nativeProcessExited(generation);
                        try { marker.delete(); }
                        catch (Throwable ignored) {}
                        return;
                    }

                    try { Thread.sleep(100L); }
                    catch (InterruptedException ignored) { return; }
                }
            }
        });
        watcher.setName("retroarch-exit-watcher");
        watcher.setDaemon(true);
        watcher.start();
    }

    private static synchronized void launchNative(int generation) {
        if (!raRunning || generation != sessionGeneration || nativeStarted)
            return;
        nativeStarted = true;
        Shell.shAsync(buildLaunchCommand(generation));
        startNativeExitWatcher(generation);
        log("OEM route ready: launched native RetroArch");
    }

    private static void startAudioReleaseWatcher(final int releaseToken,
                                                 final int nativeGeneration) {
        Thread watcher = new Thread(new Runnable() {
            public void run() {
                File marker = new File(exitMarker(nativeGeneration));
                long deadline = System.currentTimeMillis() + 5000L;
                while (!marker.exists()
                        && System.currentTimeMillis() < deadline) {
                    try { Thread.sleep(50L); }
                    catch (InterruptedException ignored) { break; }
                }
                boolean exited = marker.exists();
                if (!exited && !isRecordedNativeProcessAlive()) {
                    exited = true;
                    log("native exit marker missing, but recorded PID is not live");
                }
                if (!exited)
                    log("native exit marker timeout; forcing bounded audio release");
                AudioFocusBridge.finishRelease(releaseToken);
                if (exited) {
                    try { marker.delete(); }
                    catch (Throwable ignored) {}
                }
                synchronized (RetroArchHook.class) {
                    if (pendingShutdownGeneration == nativeGeneration) {
                        pendingShutdownGeneration = -1;
                        if (!exited)
                            stuckShutdownGeneration = nativeGeneration;
                    }
                }
            }
        });
        watcher.setName("retroarch-audio-release-watcher");
        watcher.setDaemon(true);
        watcher.start();
    }

    private static synchronized void nativeProcessExited(int generation) {
        if (!raRunning || generation != sessionGeneration || exitRequested) return;
        nativeStarted = false;
        audioAcquisitionStarted = false;
        int audioReleaseToken = AudioFocusBridge.beginRelease();
        AudioFocusBridge.finishRelease(audioReleaseToken);
        log("native RetroArch exited: requesting SystemSMM EV_EXIT=" + EV_EXIT);
        requestRaExit();
    }

    public void onItemFocused(AbstractPlaceholderMenuController outer,
                              MenuItemController menuItem) {
        /* State lifecycle, not menu focus, owns RetroArch cleanup. */
    }

    public String getLabelOverride(AbstractPlaceholderMenuController outer,
                                   MenuItemController menuItem) {
        if (outer instanceof MainWizardController && menuItem != null
                && menuItem.getWidgetID() == GAMES_WIDGET_ID) {
            return "Games";
        }
        return null;
    }

    public static void log(String message) {
        System.out.println("[RA-HOOK] " + message);
        String line = System.currentTimeMillis() + " " + message + "\n";
        if (!appendLog("/fs/sda0/retroarch/logs/ra_hook.log", line))
            appendLog("/tmp/ra_hook.log", line);
    }

    private static synchronized boolean appendLog(String path, String line) {
        java.io.FileWriter writer = null;
        try {
            File file = new File(path);
            if (file.isFile() && file.length() >= MAX_DIAGNOSTIC_LOG_BYTES) {
                File previous = new File(path + ".1");
                try { previous.delete(); }
                catch (Throwable ignored) {}
                if (!file.renameTo(previous)) {
                    java.io.FileWriter truncate = null;
                    try { truncate = new java.io.FileWriter(file, false); }
                    finally {
                        if (truncate != null) {
                            try { truncate.close(); }
                            catch (Throwable ignored) {}
                        }
                    }
                }
            }
            writer = new java.io.FileWriter(file, true);
            writer.write(line);
            return true;
        } catch (Throwable ignored) {
            return false;
        } finally {
            if (writer != null) {
                try { writer.close(); }
                catch (Throwable ignored) {}
            }
        }
    }

    private static void setClearMethod(HMITerminal terminal, int method) {
        if (terminal instanceof HMITerminalEAL) {
            EALManager eal = ((HMITerminalEAL) terminal).getEALManager();
            if (eal != null) eal.setClearMethod(method);
        } else {
            log("terminal is not HMITerminalEAL: " + terminal);
        }
    }

    private boolean hasGamesItemAlready(AbstractPlaceholderMenuController outer) {
        try {
            java.util.List children = outer.getChildren();
            if (children == null) return false;
            for (int i = 0; i < children.size(); i++) {
                Object child = children.get(i);
                if (child instanceof MenuItemController
                        && ((MenuItemController) child).getWidgetID() == GAMES_WIDGET_ID) {
                    return true;
                }
            }
        } catch (Throwable ignored) {}
        return false;
    }

    private static boolean isMenuEnter(KeyEvent event) {
        int keyCode = event.getKeyCode();
        return keyCode == KeyEvent.INC_MENU_ENTER || keyCode == KeyEvent.RC_INC_MENU_ENTER;
    }
}
