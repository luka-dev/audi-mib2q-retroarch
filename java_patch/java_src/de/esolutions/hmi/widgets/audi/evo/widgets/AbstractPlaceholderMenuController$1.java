package de.esolutions.hmi.widgets.audi.evo.widgets;

import de.audi.atip.hmi.event.JoystickEvent;
import de.audi.atip.hmi.event.KeyEvent;
import de.audi.atip.hmi.event.WheelButtonEvent;
import de.audi.atip.hmi.view.AbstractScreenFactory;
import de.audi.atip.util.Util;
import de.esolutions.hmi.widgets.audi.base.AbstractWidget;
import de.esolutions.hmi.widgets.audi.base.InitializationContext;
import de.esolutions.hmi.widgets.audi.base.HMITerminalImpl;
import de.esolutions.hmi.widgets.audi.base.widgets.AbstractWidgetController;
import de.esolutions.hmi.widgets.audi.evo.widgets.menu.MenuItemController;
import com.luka.retroarch.inject.HookManager;

/**
 * Replacement for LSD's anonymous PlaceholderMenuItem wrapper.
 *
 * RetroArch injection: adds a "Games" MainWizard item + SystemSMM event.
 * - Label override: widgetID==10 => "Games" (MainWizard only)
 * - Action override: widgetID==10 => install/enter the runtime RA state
 */
class AbstractPlaceholderMenuController$1 implements PlaceholderMenuItem {
    private static boolean RA_SHADOW_LOGGED = false;
    private final MenuItemController val$menuItem;
    private final AbstractWidget val$widget;
    private final AbstractPlaceholderMenuController this$0;

    AbstractPlaceholderMenuController$1(AbstractPlaceholderMenuController outer,
                                       MenuItemController menuItemController,
                                       AbstractWidget widget) {
        this.this$0 = outer;
        this.val$menuItem = menuItemController;
        this.val$widget = widget;
        // Reduce blast radius: only run our hook system on MainWizard.
        if (!RA_SHADOW_LOGGED) { RA_SHADOW_LOGGED = true; System.out.println("[RA-HOOK] shadow class AbstractPlaceholderMenuController$1 LOADED (replaced stock)"); }
        if (this.this$0 instanceof MainWizardController) {
            HookManager.onWrapperConstructed(this.this$0, this.val$menuItem);
        }
    }

    public void keyTurned(WheelButtonEvent e) {
        this.val$menuItem.keyTurned(e);
    }

    public void keyReleased(KeyEvent e) {
        this.val$menuItem.keyReleased(e);
    }

    public void keyPressed(KeyEvent e) {
        // Reduce blast radius: only hook key handling on MainWizard.
        if (this.this$0 instanceof MainWizardController) {
            if (HookManager.onKeyPressed(this.this$0, this.val$menuItem, e)) {
                return;
            }
        }
        this.val$menuItem.keyPressed(e);
    }

    public void keyMoved(JoystickEvent e) {
        this.val$menuItem.keyMoved(e);
    }

    public void itemFocused() {
        if (this.this$0 instanceof MainWizardController) {
            HookManager.onItemFocused(this.this$0, this.val$menuItem);
        }
    }

    public boolean isVisible() {
        return this.val$menuItem.isVisible();
    }

    public boolean isSubSelection() {
        return false;
    }

    public boolean isSubItem() {
        return false;
    }

    public boolean isSelected() {
        return this.val$menuItem.isSelected();
    }

    public boolean isMultiItem() {
        return false;
    }

    public int getSubItemCount() {
        return 0;
    }

    public String getLabelText() {
        // Reduce blast radius: only label overrides on MainWizard.
        if (this.this$0 instanceof MainWizardController) {
            String ovr = HookManager.getLabelOverride(this.this$0, this.val$menuItem);
            if (ovr != null) return ovr;
        }

        InitializationContext ctx = this.this$0.getInitContext();
        if (ctx == null) return null;
        AbstractScreenFactory sf = ctx.getScreenFactory();
        if (sf == null) return null;
        HMITerminalImpl term = this.this$0.getTerminalImpl();
        if (term == null) return null;
        return sf.getText(this.val$menuItem.getLabelId(), term.getViewSizeManager().getCurrentViewSize());
    }

    public Object getIconData(int n) {
        if (this.this$0 instanceof MainWizardController) {
            int wid = this.val$menuItem.getWidgetID();
            if (wid != -1) {
                return Util.createInteger(wid);
            }
        }
        int[] idx = this.val$menuItem.getBitmapIndices();
        if (idx == null || idx.length <= n) {
            return null;
        }
        return Util.createInteger(idx[n]);
    }

    public int[] getColorIndices() {
        return this.val$widget.getColorIndices();
    }

    public boolean isEnabled() {
        return this.val$widget.isEnabled();
    }

    public AbstractWidgetController getWidget() {
        return this.val$menuItem;
    }

    public int getSdsItemSelectedAction() {
        return this.val$menuItem.getSdsItemSelectedAction();
    }

    public int getSdsCommand() {
        return this.val$menuItem.getSdsCommand();
    }

    public int getInternalID() {
        return this.val$menuItem.getInternalID();
    }

    public int getWidgetID() {
        return this.val$menuItem.getWidgetID();
    }

    public boolean isLockable() {
        return this.val$menuItem.isLockable();
    }
}
