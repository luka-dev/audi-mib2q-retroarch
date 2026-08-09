package de.luka.ra.inject.items;

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
import de.luka.ra.inject.IMenuHook;
import de.luka.ra.inject.Shell;
import de.luka.ra.inject.audio.AudioFocusBridge;
import de.luka.ra.inject.ids.CustomWidgetIds;
import de.luka.ra.inject.ids.IdAllocator;
import de.luka.ra.inject.ids.MainWizardWidgetIds;
import de.luka.ra.inject.sm.RuntimeSmmInjector;

/**
 * Main Wizard entry for the runtime-injected RetroArch SystemSMM state.
 *
 * The menu hook only creates the Games row and requests SM events.  Launch,
 * display ownership and cleanup belong to RaScreen.connected/disconnecting,
 * so every OEM transition away from the RA state performs the same cleanup.
 */
public final class RetroArchHook implements IMenuHook {
    private static final int GAMES_WIDGET_ID = CustomWidgetIds.GAMES;
    private static final int TERMINAL_CENTER = 0;
    private static final int CLEAR_TRANSPARENT = 0;
    private static final int CLEAR_OPAQUE = 2;

    /* Backward-compatible constants used by deployment notes/tools. */
    public static final int RA_SCREEN_ID = RuntimeSmmInjector.RA_SCREEN_ID;
    private static final int EV_ENTER = RuntimeSmmInjector.EV_ENTER;
    private static final int EV_EXIT = RuntimeSmmInjector.EV_EXIT;

    private static final String LOCK = "/tmp/retroarch.lock";
    private static final String EXIT_MARKER = "/tmp/retroarch.exited";
    private static final String LAUNCH_CMD =
            "(rm -f " + EXIT_MARKER
            + "; /bin/sh /mnt/app/root/retroarch/ra.sh"
            + "; : > " + EXIT_MARKER
            + ") </dev/null >/tmp/ra_run.log 2>&1 &";
    private static final String TERM_CMD =
            "kill -TERM `cat " + LOCK + " 2>/dev/null` 2>/dev/null";

    private static volatile boolean raRunning;
    private static volatile boolean exitRequested;
    private static volatile int savedContext = -1;
    private static volatile int sessionGeneration;

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
        exitRequested = false;
        savedContext = -1;
        watcherGeneration = ++sessionGeneration;

        /* Never let a marker from an earlier session win the race against the
         * launcher's own rm. This is local tmpfs I/O and does not touch HMI
         * state or block on the native process. */
        try { new File(EXIT_MARKER).delete(); }
        catch (Throwable ignored) {}

        try {
            IDisplayManager display = AbstractWidget.hmiService.getDisplayManager();
            savedContext = display.getCurrentContextID(TERMINAL_CENTER);
        } catch (Throwable t) {
            log("RA connect: could not read previous display context: " + t);
        }

        /* Start the PCM producer before routing the amplifier to MPL1. */
        Shell.shAsync(LAUNCH_CMD);
        startNativeExitWatcher(watcherGeneration);
        AudioFocusBridge.request(RetroArchHook.class.getClassLoader());
        try {
            setClearMethod(terminal, CLEAR_TRANSPARENT);
            /* The EGL driver routes private context 90 only after displayable
             * 43/window creation. IDisplayManager cannot switch 90 because the
             * OEM Java table only contains contexts 0..78. */
            log("RA state connected: launched process, saved context " + savedContext
                    + ", native route=90{43}, clear=transparent");
        } catch (Throwable t) {
            log("RA connect display setup failed: " + t);
        }
    }

    /** Called for BACK and for every forced OEM transition away from the RA state. */
    public static synchronized void onRaScreenDisconnecting(HMITerminal terminal) {
        if (!raRunning) return;
        raRunning = false;
        exitRequested = false;
        sessionGeneration++; /* retires the current native-exit watcher */

        AudioFocusBridge.release();

        try {
            setClearMethod(terminal, CLEAR_OPAQUE);
            if (savedContext >= 0) {
                AbstractWidget.hmiService.getDisplayManager()
                        .switchContext(savedContext, TERMINAL_CENTER, null);
            }
        } catch (Throwable t) {
            log("RA disconnect display restore failed: " + t);
        }

        Shell.shAsync(TERM_CMD);
        log("RA state disconnected: restored context " + savedContext + " and sent SIGTERM");
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
     * transparent RA state. The shell wrapper creates EXIT_MARKER whenever the
     * native process ends (clean quit or crash); this bounded Java watcher then
     * requests the same EV_EXIT transition as BACK.
     */
    private static void startNativeExitWatcher(final int generation) {
        Thread watcher = new Thread(new Runnable() {
            public void run() {
                File marker = new File(EXIT_MARKER);
                for (;;) {
                    synchronized (RetroArchHook.class) {
                        if (!raRunning || generation != sessionGeneration) return;
                    }

                    if (marker.exists()) {
                        nativeProcessExited(generation);
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

    private static synchronized void nativeProcessExited(int generation) {
        if (!raRunning || generation != sessionGeneration || exitRequested) return;
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
        java.io.FileWriter writer = null;
        try {
            writer = new java.io.FileWriter("/tmp/ra_hook.log", true);
            writer.write(message + "\n");
        } catch (Throwable ignored) {
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
