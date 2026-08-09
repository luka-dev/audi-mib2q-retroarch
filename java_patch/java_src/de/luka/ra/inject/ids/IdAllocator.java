package de.luka.ra.inject.ids;

import de.esolutions.hmi.widgets.audi.evo.widgets.AbstractPlaceholderMenuController;
import de.esolutions.hmi.widgets.audi.evo.widgets.menu.MenuItemController;

/**
 * Helpers for allocating IDs without colliding with existing menu items.
 *
 * Java 1.2 compatible (no generics, no enhanced-for).
 */
public final class IdAllocator {
    private IdAllocator() {
    }

    public static int nextFreeInternalId(AbstractPlaceholderMenuController outer, int startInclusive) {
        int id = startInclusive;
        // Simple linear scan; menu sizes are tiny.
        while (isInternalIdUsed(outer, id)) {
            id++;
        }
        return id;
    }

    private static boolean isInternalIdUsed(AbstractPlaceholderMenuController outer, int internalId) {
        if (outer == null) return false;
        try {
            java.util.List children = outer.getChildren();
            if (children == null) return false;
            for (int i = 0; i < children.size(); i++) {
                Object o = children.get(i);
                if (o instanceof MenuItemController) {
                    if (((MenuItemController)o).getInternalID() == internalId) {
                        return true;
                    }
                }
            }
        } catch (Throwable t) {
            // ignore
        }
        return false;
    }
}

