package de.luka.ra.inject;

import de.audi.atip.hmi.event.KeyEvent;
import de.esolutions.hmi.widgets.audi.evo.widgets.AbstractPlaceholderMenuController;
import de.esolutions.hmi.widgets.audi.evo.widgets.menu.MenuItemController;

/**
 * Small hook interface used by the RetroArch injected jar.
 * Keep this Java 1.2 compatible (no generics, no enums).
 */
public interface IMenuHook {
    void onWrapperConstructed(AbstractPlaceholderMenuController outer, MenuItemController menuItem);
    boolean onKeyPressed(AbstractPlaceholderMenuController outer, MenuItemController menuItem, KeyEvent e);
    void onItemFocused(AbstractPlaceholderMenuController outer, MenuItemController menuItem);
    String getLabelOverride(AbstractPlaceholderMenuController outer, MenuItemController menuItem);
}
