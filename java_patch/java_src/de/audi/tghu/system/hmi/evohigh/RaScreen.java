package de.audi.tghu.system.hmi.evohigh;

import de.audi.atip.hmi.HMITerminal;
import de.audi.atip.hmi.event.KeyEvent;
import de.audi.atip.hmi.view.AbstractScreenFactory;
import de.esolutions.hmi.widgets.audi.base.InitializationContext;
import de.esolutions.hmi.widgets.audi.evo.ScreenWidgetEVO;
import de.esolutions.hmi.widgets.audi.evo.high.widgets.ScreenRendererHigh;
import de.esolutions.hmi.widgets.audi.evo.widgets.StatusBarStubController;
import de.luka.ra.inject.items.RetroArchHook;
import de.luka.ra.inject.sm.RuntimeSmmInjector;

/**
 * Empty, transparent SystemSMM screen for native RetroArch.
 *
 * The instance is preloaded into the OEM ScreenCache, so the stock
 * SystemScreenFactory is never replaced.  Screen connection/disconnection is
 * the RetroArch lifecycle: forced OEM transitions (PDC, camera, shutdown) get
 * the same cleanup as BACK.
 */
public class RaScreen extends ScreenWidgetEVO {
    public RaScreen(int id) {
        super(id);
    }

    public static RaScreen build(AbstractScreenFactory factory, HMITerminal terminal) {
        RaScreen screen = new RaScreen(RuntimeSmmInjector.RA_SCREEN_ID);
        screen.setScreenFactory(factory);
        screen.setTerminal(terminal);
        screen.setCacheBehaviour(0); /* always cached: factory has no case 250 */
        screen.setClearMethod(0);    /* transparent HMI plane */

        ScreenRendererHigh renderer = new ScreenRendererHigh(screen);
        screen.setRenderer(renderer);
        if (factory instanceof SystemScreenFactory) {
            try {
                renderer.setFonts(((SystemScreenFactory) factory)
                        .getFonts(0, terminal.getTerminalID()));
            } catch (Throwable ignored) {
                /* No text is rendered; fonts are optional for this empty screen. */
            }
        }

        /* Engineering (red-menu) screens explicitly install this stub with
         * render style 2 instead of inheriting the previous lower status bar.
         * Context 90 hides DISPLAYABLE_HMI entirely once routed; the stub also
         * prevents a stale bar during the short launch/route interval. */
        StatusBarStubController statusBar = new StatusBarStubController();
        statusBar.setModelID(138);
        statusBar.setBounds(0, 0, 100, 100);
        statusBar.setRenderStyle(2);
        screen.add(statusBar);
        return screen;
    }

    public void connected(InitializationContext context) {
        super.connected(context);
        RetroArchHook.onRaScreenConnected(this.getTerminal());
    }

    public void disconnecting() {
        RetroArchHook.onRaScreenDisconnecting(this.getTerminal());
        super.disconnecting();
    }

    public void keyPressed(KeyEvent event) {
        if (event == null) return;
        int keyCode = -1;
        try { keyCode = event.getKeyCode(); }
        catch (Throwable ignored) {}

        if (keyCode == 15 || keyCode == 30 || keyCode == 41) {
            RetroArchHook.requestRaExit();
        }
        /* RA owns input while this state is active. */
        try { event.consume(); }
        catch (Throwable ignored) {}
    }
}
