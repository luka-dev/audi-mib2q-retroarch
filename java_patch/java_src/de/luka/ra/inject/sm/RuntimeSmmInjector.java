package de.luka.ra.inject.sm;

import de.audi.atip.hmi.HMITerminal;
import de.audi.atip.hmi.view.AbstractScreenFactory;
import de.audi.atip.hmi.view.Screen;
import de.audi.atip.hmi.view.ScreenCache;
import de.audi.atip.statemachine.SMModule;
import de.audi.atip.statemachine.State;
import de.audi.atip.statemachine.Transition;
import de.audi.tghu.smi.SMI;
import de.audi.tghu.smi.StateMachine;
import de.audi.tghu.smi.StateMachineTerminal;
import de.audi.tghu.system.hmi.evohigh.RaScreen;
import de.audi.tghu.system.sm.SystemSMM;
import de.esolutions.hmi.widgets.audi.base.AbstractWidget;
import de.esolutions.hmi.widgets.audi.evo.widgets.AbstractPlaceholderMenuController;
import de.luka.ra.inject.items.RetroArchHook;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import org.osgi.framework.BundleContext;
import org.osgi.framework.ServiceReference;

/**
 * Installs the RetroArch state into the already initialized, OEM SystemSMM.
 *
 * Nothing in the HMI boot path is shadowed: the stock SystemSMMInitStates,
 * SystemSMMInitTransitions and SystemScreenFactory finish first.  On the first
 * Games activation we locate the live SystemSMM OSGi service, validate the
 * exact MU1316 table fingerprint, build complete replacement arrays, and swap
 * their references before the first custom event is fired.
 *
 * Keep this source Java 1.4/J9 compatible: no generics, autoboxing or modern
 * reflection helpers.
 */
public final class RuntimeSmmInjector {
    public static final int RA_SCREEN_ID = 250;
    public static final int DRUM_STATE_INDEX = 89;
    public static final int EV_ENTER = 9990001;
    public static final int EV_EXIT = 9990002;

    private static final int EXPECTED_STATES = 631;
    private static final int EXPECTED_TRANSITIONS = 890;
    private static final int SYSTEM_MODULE_ID = 0;
    private static final int CENTER_TERMINAL_ID = 0;
    private static final int MAIN_SUBTERMINAL_ID = 0;
    private static final int ID_MULTIPLIER = 100000;

    private static boolean installed;
    private static boolean installing;
    private static int raStateID = -1;
    private static int enterTransitionID = -1;
    private static int exitTransitionID = -1;

    private RuntimeSmmInjector() {
    }

    /** Fail closed: false means the caller must not fire EV_ENTER. */
    public static synchronized boolean ensureInstalled(AbstractPlaceholderMenuController outer) {
        if (installed) return true;
        if (installing) return false;
        installing = true;

        ServiceLookup lookup = null;
        SmmPatch patch = null;
        try {
            if (outer == null) throw new IllegalStateException("MainWizard controller is null");
            lookup = findSystemSmm();
            patch = new SmmPatch(lookup.smm);
            patch.prepare();

            /* Register the screen first.  If this fails, no SMM table is touched. */
            registerRaScreen(outer);
            patch.apply();
            patch.verify();

            /* The SMI keeps live State objects in its active stack. Those
             * objects captured the old trigger arrays when HMI started, so
             * swapping only the SystemSMM tables is not sufficient: the first
             * EV_ENTER would otherwise be ignored until an unrelated state
             * transition happened to recreate state 89. Refresh the stack on
             * this event-dispatch thread before publishing installed=true. */
            refreshActiveStateStack(true, patch.newEnterTransitionID);

            raStateID = patch.newStateID;
            enterTransitionID = patch.newEnterTransitionID;
            exitTransitionID = patch.newExitTransitionID;
            installed = true;
            log("runtime SMM install PASS: state=" + raStateID
                    + " enterTrans=" + enterTransitionID
                    + " exitTrans=" + exitTransitionID
                    + " screen=" + RA_SCREEN_ID);
            return true;
        } catch (Throwable t) {
            if (patch != null) {
                patch.rollback();
                try { refreshActiveStateStack(false, -1); }
                catch (Throwable ignored) {}
            }
            log("runtime SMM install FAILED (no EV_ENTER): " + t);
            return false;
        } finally {
            if (lookup != null) lookup.release();
            installing = false;
        }
    }

    public static synchronized boolean isInstalled() {
        return installed;
    }

    public static synchronized int getRaStateID() {
        return raStateID;
    }

    private static ServiceLookup findSystemSmm() throws Exception {
        if (AbstractWidget.framework == null) {
            throw new IllegalStateException("AbstractWidget.framework is null");
        }
        BundleContext context = AbstractWidget.framework.getBundleCxt();
        if (context == null) throw new IllegalStateException("BundleContext is null");

        ServiceReference[] refs = context.getServiceReferences(SMModule.class.getName(), null);
        if (refs == null || refs.length == 0) {
            throw new IllegalStateException("no SMModule OSGi services");
        }

        for (int i = 0; i < refs.length; i++) {
            Object service = null;
            try {
                service = context.getService(refs[i]);
                if (service instanceof SystemSMM) {
                    SystemSMM smm = (SystemSMM) service;
                    if (smm.getModuleID() == SYSTEM_MODULE_ID
                            && smm.getTerminalID() == CENTER_TERMINAL_ID
                            && smm.getSubterminalID() == MAIN_SUBTERMINAL_ID) {
                        log("found live SystemSMM: terminal=" + smm.getTerminalID()
                                + " subterminal=" + smm.getSubterminalID()
                                + " name=" + smm.getSMMName());
                        return new ServiceLookup(context, refs[i], smm);
                    }
                }
            } finally {
                if (service != null
                        && !(service instanceof SystemSMM
                        && ((SystemSMM) service).getModuleID() == SYSTEM_MODULE_ID
                        && ((SystemSMM) service).getTerminalID() == CENTER_TERMINAL_ID
                        && ((SystemSMM) service).getSubterminalID() == MAIN_SUBTERMINAL_ID)) {
                    try { context.ungetService(refs[i]); }
                    catch (Throwable ignored) {}
                }
            }
        }
        throw new IllegalStateException("main terminal SystemSMM service not found");
    }

    private static void registerRaScreen(AbstractPlaceholderMenuController outer)
            throws Exception {
        HMITerminal terminal = outer.getTerminal();
        if (terminal == null) throw new IllegalStateException("MainWizard terminal is null");
        if (terminal.getTerminalID() != CENTER_TERMINAL_ID) {
            throw new IllegalStateException("wrong terminal " + terminal.getTerminalID());
        }
        ScreenCache cache = terminal.getScreenCache();
        if (cache == null) throw new IllegalStateException("ScreenCache is null");

        Screen existing = cache.getScreen(RA_SCREEN_ID);
        if (existing != null) {
            if (!(existing instanceof RaScreen)) {
                throw new IllegalStateException("screen ID 250 already belongs to "
                        + existing.getClass().getName());
            }
            log("RaScreen already present in ScreenCache");
            return;
        }

        AbstractScreenFactory factory = outer.getScreenFactory();
        if (factory == null) throw new IllegalStateException("SystemScreenFactory is null");
        RaScreen raScreen = RaScreen.build(factory, terminal);
        cache.putScreen(raScreen);
        if (cache.getScreen(RA_SCREEN_ID) != raScreen) {
            throw new IllegalStateException("ScreenCache rejected RaScreen");
        }
        log("preloaded RaScreen ID 250 into OEM ScreenCache");
    }

    private static Field findField(Class type, String name) throws Exception {
        Class c = type;
        while (c != null) {
            try {
                Field field = c.getDeclaredField(name);
                field.setAccessible(true);
                return field;
            } catch (NoSuchFieldException e) {
                c = c.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Method findMethod(Class type, String name) throws Exception {
        Class c = type;
        while (c != null) {
            try {
                Method method = c.getDeclaredMethod(name, new Class[0]);
                method.setAccessible(true);
                return method;
            } catch (NoSuchMethodException e) {
                c = c.getSuperclass();
            }
        }
        throw new NoSuchMethodException(name);
    }

    /** Recreate SMI's live State objects from the newly swapped SMM tables. */
    private static void refreshActiveStateStack(boolean expectCustom,
                                                int expectedEnterTransition) throws Exception {
        Object interpreter = AbstractWidget.framework.getSMInterpreter();
        if (!(interpreter instanceof SMI)) {
            throw new IllegalStateException("SMInterpreter is not MU1316 SMI");
        }
        StateMachineTerminal terminal = ((SMI) interpreter).getSMTerminal(CENTER_TERMINAL_ID);
        if (terminal == null) throw new IllegalStateException("SMI terminal 0 is null");
        StateMachine machine = terminal.getActiveSubterminalStateMachine();
        if (machine == null) throw new IllegalStateException("main StateMachine is null");

        Method refresh = findMethod(machine.getClass(), "reinitActiveStateStack");
        refresh.invoke(machine, new Object[0]);

        State[] active = machine.getActiveStateStack();
        int count = machine.getActiveStates();
        boolean drumActive = false;
        StringBuffer ids = new StringBuffer();
        for (int i = 0; i < count; i++) {
            State state = active[i];
            if (i != 0) ids.append(',');
            ids.append(state == null ? -1 : state.getStateID());
            if (state != null && state.getStateID() == DRUM_STATE_INDEX) {
                drumActive = true;
                if (expectCustom) {
                    require(state.getOutgoingTransition(EV_ENTER) == expectedEnterTransition,
                            "live state 89 still lacks EV_ENTER");
                }
            }
        }
        if (expectCustom) require(drumActive, "state 89 is not active at Games click");
        log("refreshed live SMI stack: [" + ids.toString() + "]");
    }

    private static int[] grow(int[] source, int extra) {
        int[] result = new int[source.length + extra];
        System.arraycopy(source, 0, result, 0, source.length);
        return result;
    }

    private static int[][] grow(int[][] source, int oldLength, int extra) {
        int[][] result = new int[oldLength + extra][];
        if (source != null) System.arraycopy(source, 0, result, 0, source.length);
        return result;
    }

    private static int[] append(int[] source, int value) {
        int oldLength = source == null ? 0 : source.length;
        int[] result = new int[oldLength + 1];
        if (source != null) System.arraycopy(source, 0, result, 0, oldLength);
        result[oldLength] = value;
        return result;
    }

    private static boolean arraysEqual(int[] actual, int[] expected) {
        if (actual == null || expected == null || actual.length != expected.length) return false;
        for (int i = 0; i < actual.length; i++) {
            if (actual[i] != expected[i]) return false;
        }
        return true;
    }

    private static boolean contains(int[] values, int value) {
        if (values == null) return false;
        for (int i = 0; i < values.length; i++) if (values[i] == value) return true;
        return false;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }

    private static void log(String message) {
        RetroArchHook.log("[SMM] " + message);
    }

    private static final class ServiceLookup {
        final BundleContext context;
        final ServiceReference reference;
        final SystemSMM smm;

        ServiceLookup(BundleContext context, ServiceReference reference, SystemSMM smm) {
            this.context = context;
            this.reference = reference;
            this.smm = smm;
        }

        void release() {
            try { context.ungetService(reference); }
            catch (Throwable ignored) {}
        }
    }

    private static final class SmmPatch {
        final SystemSMM smm;
        Field historyField;

        int[] oldStateFlags;
        int[] oldStateSuperstates;
        int[] oldStateDhs;
        int[] oldStateHistory;
        int[][] oldStateTriggers;
        int[][] oldStateOutTransitions;
        int[][] oldStateMediators;
        int[][] oldStateSyncTriggers;
        int[][] oldStateSyncExitTriggers;
        int[][] oldStateSyncTargets;
        int[] oldTransitionFlags;
        int[][] oldTransitionTargetFlags;
        int[][] oldTransitionTargetStates;

        int[] newStateFlags;
        int[] newStateSuperstates;
        int[] newStateDhs;
        int[] newStateHistory;
        int[][] newStateTriggers;
        int[][] newStateOutTransitions;
        int[][] newStateMediators;
        int[][] newStateSyncTriggers;
        int[][] newStateSyncExitTriggers;
        int[][] newStateSyncTargets;
        int[] newTransitionFlags;
        int[][] newTransitionTargetFlags;
        int[][] newTransitionTargetStates;

        int newStateID;
        int newEnterTransitionID;
        int newExitTransitionID;
        boolean mutationStarted;

        SmmPatch(SystemSMM smm) {
            this.smm = smm;
        }

        void prepare() throws Exception {
            Class type = smm.getClass();
            oldStateFlags = (int[]) findField(type, "stateFlagList").get(smm);
            oldStateSuperstates = (int[]) findField(type, "stateSuperstateList").get(smm);
            oldStateDhs = (int[]) findField(type, "stateDHSList").get(smm);
            historyField = findField(type, "stateHistoryList");
            oldStateHistory = (int[]) historyField.get(smm);
            oldStateTriggers = (int[][]) findField(type, "stateTriggerEventList").get(smm);
            oldStateOutTransitions = (int[][]) findField(type, "stateOutTransList").get(smm);
            oldStateMediators = (int[][]) findField(type, "stateMediatorList").get(smm);
            oldStateSyncTriggers = (int[][]) findField(type, "stateSyncTriggerList").get(smm);
            oldStateSyncExitTriggers = (int[][]) findField(type, "stateSyncExitTriggerList").get(smm);
            oldStateSyncTargets = (int[][]) findField(type, "stateSyncTargetList").get(smm);
            oldTransitionFlags = (int[]) findField(type, "transFlagList").get(smm);
            oldTransitionTargetFlags = (int[][]) findField(type, "transTrgtFlagList").get(smm);
            oldTransitionTargetStates = (int[][]) findField(type, "transTrgtStateList").get(smm);

            validateStockFingerprint();

            int stateCount = oldStateFlags.length;
            int transitionCount = oldTransitionFlags.length;
            newStateID = SYSTEM_MODULE_ID * ID_MULTIPLIER + stateCount;
            newEnterTransitionID = SYSTEM_MODULE_ID * ID_MULTIPLIER + transitionCount;
            newExitTransitionID = SYSTEM_MODULE_ID * ID_MULTIPLIER + transitionCount + 1;

            newStateFlags = grow(oldStateFlags, 1);
            newStateFlags[stateCount] = 0;
            newStateSuperstates = grow(oldStateSuperstates, 1);
            newStateSuperstates[stateCount] = oldStateSuperstates[DRUM_STATE_INDEX];
            newStateDhs = grow(oldStateDhs, 1);
            newStateDhs[stateCount] = RA_SCREEN_ID;
            newStateHistory = grow(oldStateHistory, 1);
            newStateHistory[stateCount] = -1;

            newStateTriggers = grow(oldStateTriggers, stateCount, 1);
            newStateOutTransitions = grow(oldStateOutTransitions, stateCount, 1);
            newStateMediators = grow(oldStateMediators, stateCount, 1);
            newStateSyncTriggers = grow(oldStateSyncTriggers, stateCount, 1);
            newStateSyncExitTriggers = grow(oldStateSyncExitTriggers, stateCount, 1);
            newStateSyncTargets = grow(oldStateSyncTargets, stateCount, 1);

            newStateTriggers[DRUM_STATE_INDEX] = append(
                    oldStateTriggers[DRUM_STATE_INDEX], EV_ENTER);
            newStateOutTransitions[DRUM_STATE_INDEX] = append(
                    oldStateOutTransitions[DRUM_STATE_INDEX], newEnterTransitionID);
            newStateTriggers[stateCount] = new int[] {EV_EXIT};
            newStateOutTransitions[stateCount] = new int[] {newExitTransitionID};

            newTransitionFlags = grow(oldTransitionFlags, 2);
            newTransitionTargetFlags = grow(oldTransitionTargetFlags, transitionCount, 2);
            newTransitionTargetStates = grow(oldTransitionTargetStates, transitionCount, 2);
            newTransitionTargetFlags[transitionCount] = new int[] {0};
            newTransitionTargetFlags[transitionCount + 1] = new int[] {0};
            newTransitionTargetStates[transitionCount] = new int[] {newStateID};
            newTransitionTargetStates[transitionCount + 1] = new int[] {DRUM_STATE_INDEX};

            log("prepared OEM table copies: states " + stateCount + "->" + (stateCount + 1)
                    + ", transitions " + transitionCount + "->" + (transitionCount + 2));
        }

        private void validateStockFingerprint() {
            require(smm.getModuleID() == SYSTEM_MODULE_ID, "SystemSMM module is not 0");
            require(oldStateFlags != null && oldStateFlags.length == EXPECTED_STATES,
                    "unexpected state count");
            require(oldStateSuperstates != null && oldStateSuperstates.length == EXPECTED_STATES,
                    "superstate table size mismatch");
            require(oldStateDhs != null && oldStateDhs.length == EXPECTED_STATES,
                    "DHS table size mismatch");
            require(oldStateHistory != null && oldStateHistory.length == EXPECTED_STATES,
                    "history table size mismatch");
            require(oldStateTriggers != null && oldStateTriggers.length == EXPECTED_STATES,
                    "trigger table size mismatch");
            require(oldStateOutTransitions != null
                    && oldStateOutTransitions.length == EXPECTED_STATES,
                    "out-transition table size mismatch");
            require(oldStateMediators != null && oldStateMediators.length == EXPECTED_STATES,
                    "mediator table size mismatch");
            require(oldStateSyncTriggers != null && oldStateSyncTriggers.length == EXPECTED_STATES,
                    "sync-trigger table size mismatch");
            require(oldStateSyncExitTriggers != null
                    && oldStateSyncExitTriggers.length == EXPECTED_STATES,
                    "sync-exit table size mismatch");
            require(oldStateSyncTargets != null && oldStateSyncTargets.length == EXPECTED_STATES,
                    "sync-target table size mismatch");
            require(oldTransitionFlags != null
                    && oldTransitionFlags.length == EXPECTED_TRANSITIONS,
                    "unexpected transition count");
            require(oldTransitionTargetFlags != null
                    && oldTransitionTargetFlags.length == EXPECTED_TRANSITIONS,
                    "transition target-flag table size mismatch");
            require(oldTransitionTargetStates != null
                    && oldTransitionTargetStates.length == EXPECTED_TRANSITIONS,
                    "transition target-state table size mismatch");

            /* Exact MU1316 MainWizard state fingerprint from shipping bytecode. */
            require(oldStateFlags[DRUM_STATE_INDEX] == 0, "state 89 flags changed");
            require(oldStateSuperstates[DRUM_STATE_INDEX] == 94,
                    "state 89 superstate changed");
            require(oldStateDhs[DRUM_STATE_INDEX] == 46, "state 89 DHS changed");
            require(arraysEqual(oldStateTriggers[DRUM_STATE_INDEX], new int[] {-13, 1539}),
                    "state 89 trigger fingerprint changed");
            require(arraysEqual(oldStateOutTransitions[DRUM_STATE_INDEX], new int[] {357, 757}),
                    "state 89 transition fingerprint changed");
            for (int i = 0; i < oldStateDhs.length; i++) {
                require(oldStateDhs[i] != RA_SCREEN_ID, "DHS screen ID 250 collision");
                require(!contains(oldStateTriggers[i], EV_ENTER),
                        "EV_ENTER collision at state " + i);
                require(!contains(oldStateTriggers[i], EV_EXIT),
                        "EV_EXIT collision at state " + i);
            }
        }

        void apply() throws Throwable {
            mutationStarted = true;
            try {
                /* New transitions are unreachable until state trigger tables are swapped. */
                smm.setTransFlagList(newTransitionFlags);
                smm.setTransTrgtFlagList(newTransitionTargetFlags);
                smm.setTransTrgtStateList(newTransitionTargetStates);

                /* Prepare every per-state table before publishing the longer flag table. */
                smm.setStateTriggerEventList(newStateTriggers);
                smm.setStateOutTransList(newStateOutTransitions);
                smm.setStateMediatorList(newStateMediators);
                smm.setStateSyncTriggerList(newStateSyncTriggers);
                smm.setStateSyncExitTriggerList(newStateSyncExitTriggers);
                smm.setStateSyncTargetList(newStateSyncTargets);
                smm.setStateSuperstateList(newStateSuperstates);
                smm.setStateDHSList(newStateDhs);
                historyField.set(smm, newStateHistory);
                smm.setStateFlagList(newStateFlags); /* publishes the new state last */
            } catch (Throwable t) {
                rollback();
                throw t;
            }
        }

        void verify() {
            State drum = null;
            State ra = null;
            Transition enter = null;
            Transition exit = null;
            try {
                drum = smm.getState(DRUM_STATE_INDEX);
                ra = smm.getState(newStateID);
                enter = smm.getTransition(newEnterTransitionID);
                exit = smm.getTransition(newExitTransitionID);
                require(drum != null, "drum state missing after install");
                require(ra != null, "RA state missing after install");
                require(ra.getScreenID() == RA_SCREEN_ID, "RA state has wrong screen");
                require(ra.getSuperstateID() == 94, "RA state has wrong superstate");
                require(ra.getOutgoingTransition(EV_EXIT) == newExitTransitionID,
                        "RA exit transition not registered");
                require(drum.getOutgoingTransition(EV_ENTER)
                        == newEnterTransitionID, "drum enter transition not registered");
                require(enter != null && enter.targetStates() == 1
                        && enter.getTargetState(0) == newStateID,
                        "enter transition target invalid");
                require(exit != null && exit.targetStates() == 1
                        && exit.getTargetState(0) == DRUM_STATE_INDEX,
                        "exit transition target invalid");
            } finally {
                if (drum != null) drum.dispose();
                if (ra != null) ra.dispose();
                if (enter != null) enter.dispose();
                if (exit != null) exit.dispose();
            }
        }

        void rollback() {
            if (!mutationStarted) return;
            /* Hide the appended state first, then restore all supporting tables. */
            try { smm.setStateFlagList(oldStateFlags); } catch (Throwable ignored) {}
            try { historyField.set(smm, oldStateHistory); } catch (Throwable ignored) {}
            try { smm.setStateDHSList(oldStateDhs); } catch (Throwable ignored) {}
            try { smm.setStateSuperstateList(oldStateSuperstates); } catch (Throwable ignored) {}
            try { smm.setStateSyncTargetList(oldStateSyncTargets); } catch (Throwable ignored) {}
            try { smm.setStateSyncExitTriggerList(oldStateSyncExitTriggers); }
            catch (Throwable ignored) {}
            try { smm.setStateSyncTriggerList(oldStateSyncTriggers); } catch (Throwable ignored) {}
            try { smm.setStateMediatorList(oldStateMediators); } catch (Throwable ignored) {}
            try { smm.setStateOutTransList(oldStateOutTransitions); } catch (Throwable ignored) {}
            try { smm.setStateTriggerEventList(oldStateTriggers); } catch (Throwable ignored) {}
            try { smm.setTransTrgtStateList(oldTransitionTargetStates); } catch (Throwable ignored) {}
            try { smm.setTransTrgtFlagList(oldTransitionTargetFlags); } catch (Throwable ignored) {}
            try { smm.setTransFlagList(oldTransitionFlags); } catch (Throwable ignored) {}
            mutationStarted = false;
            log("rolled back runtime SMM tables");
        }
    }
}
