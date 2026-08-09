package de.luka.ra.inject;

import de.audi.atip.hmi.event.KeyEvent;
import de.esolutions.hmi.widgets.audi.evo.widgets.AbstractPlaceholderMenuController;
import de.esolutions.hmi.widgets.audi.evo.widgets.menu.MenuItemController;
import de.luka.ra.inject.items.RetroArchHook;

/** Central dispatcher for the injected menu hooks. */
public final class HookManager {
    private static final IMenuHook[] HOOKS = new IMenuHook[] {
            new RetroArchHook()
    };

    private HookManager() {
    }

    public static void onWrapperConstructed(AbstractPlaceholderMenuController outer, MenuItemController menuItem) {
        for (int i = 0; i < HOOKS.length; i++) {
            try { HOOKS[i].onWrapperConstructed(outer, menuItem); }
            catch (Throwable t) { /* never crash HMI construction */ }
        }
    }

    public static boolean onKeyPressed(AbstractPlaceholderMenuController outer, MenuItemController menuItem, KeyEvent e) {
        for (int i = 0; i < HOOKS.length; i++) {
            try { if (HOOKS[i].onKeyPressed(outer, menuItem, e)) return true; }
            catch (Throwable t) { /* ignore */ }
        }
        return false;
    }

    public static void onItemFocused(AbstractPlaceholderMenuController outer, MenuItemController menuItem) {
        for (int i = 0; i < HOOKS.length; i++) {
            try { HOOKS[i].onItemFocused(outer, menuItem); }
            catch (Throwable t) { /* never crash HMI focus propagation */ }
        }
    }

    public static String getLabelOverride(AbstractPlaceholderMenuController outer, MenuItemController menuItem) {
        for (int i = 0; i < HOOKS.length; i++) {
            try { String s = HOOKS[i].getLabelOverride(outer, menuItem); if (s != null) return s; }
            catch (Throwable t) { /* ignore */ }
        }
        return null;
    }
}
