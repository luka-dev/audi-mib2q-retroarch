package com.luka.retroarch.inject.audio;

import de.audi.atip.audio.HMIAudioService;
import de.audi.atip.audio.HMIAudioServiceListener;
import de.audi.atip.audio.IAudioFocusClient;
import de.audi.atip.audio.IAudioFocusManager;
import de.audi.atip.interapp.audio.ATIPAudioRoute;
import de.audi.atip.interapp.audio.ATIPMediaRouterService;
import de.audi.atip.interapp.audio.ATIPMediaRouterServiceListener;
import de.audi.atip.interapp.def.ATIPAudioRouteImpl;
import de.audi.atip.start.ILastmodeHandler;
import de.dreisoft.lsd.ServiceInfo;
import com.luka.retroarch.inject.Shell;
import java.io.File;
import java.io.FileWriter;
import java.util.Hashtable;

/**
 * Owns the RetroArch entertainment-audio session for the RaScreen lifetime.
 *
 * This deliberately uses the same three public services as the stock Media
 * application: IAudioFocusManager, HMIAudioService(CLIENT_MEDIA), and
 * ATIPMediaRouterService.  It must not register a raw DSIMediaRouter client or
 * select an SDIS context: both paths already have independent owners inside
 * the OEM audio bundle and using them here creates competing state machines.
 */
public final class AudioFocusBridge {
    private static final long MAX_DIAGNOSTIC_LOG_BYTES = 256L * 1024L;
    private static final int CONNECTION_MEDIA_MFP = 20;
    private static final int TERMINAL_FRONT = 0;
    private static final int AUDIO_APP_MEDIA = 2;
    private static final int ROUTING_INPUT_INTERNAL_MEDIA = 1;
    private static final int ROUTING_OUTPUT_MPL1 = 1;
    private static final int ROUTING_RESULT_OK = 0;

    private static final int CONN_FADED_IN = 0;
    private static final int CONN_STARTED = 2;
    private static final long SERVICE_RETRY_MS = 250L;
    private static final long SERVICE_TIMEOUT_MS = 15000L;
    private static final long CONNECTION_TIMEOUT_MS = 6000L;
    private static final long ROUTE_TIMEOUT_MS = 4000L;
    private static final long PCM_TIMEOUT_MS = 12000L;
    private static final long FADE_TIMEOUT_MS = 5000L;
    private static final int MAX_RECOVERY_CONNECTION_RETRIES = 3;
    private static final String PCM_READY_DEFAULT = "/tmp/retroarch.pcm.ready";

    private static volatile boolean requested;
    private static boolean workerRunning;
    private static boolean active;
    private static boolean nativeLaunchIssued;
    private static boolean connectionRequested;
    private static boolean connectionStarted;
    private static boolean fadedIn;
    private static boolean routeTakeoverIssued;
    private static boolean routeConfirmed;
    private static boolean routeCommandAccepted;
    private static boolean baselineCaptured;
    private static boolean previousRouteCaptureOpen;
    private static boolean releaseInProgress;
    private static boolean audioManagerAvailabilityKnown;
    private static boolean audioManagerAvailable;
    private static boolean focusLossSignalled;
    private static boolean focusRestoredAfterLoss;
    private static boolean recoveryStartIssued;
    private static boolean lossArmed;
    private static int recoveryConnectionRetries;
    private static int generation;
    private static int currentFocus = -1;
    private static int previousFocus = -1;
    private static int previousConnection = -1;
    private static int previousRoute = -1;
    private static int currentRoute = -1;
    private static int currentRouteStatus = -1;
    private static long lastConnectionRequestAt;
    private static long routeCommandAcceptedAt;
    private static ClassLoader classLoaderHint;
    private static Runnable launchCallback;
    private static Runnable failureCallback;

    private static HMIAudioService hmiAudio;
    private static IAudioFocusManager focusManager;
    private static ILastmodeHandler lastmodeHandler;
    private static ATIPMediaRouterService mediaRouterService;
    private static ServiceInfo audioListenerRegistration;
    private static ServiceInfo focusListenerRegistration;
    private static ServiceInfo routeListenerRegistration;

    private static final HMIAudioServiceListener AUDIO_LISTENER =
            new HMIAudioServiceListener() {
        public void updateAMAvailable(boolean available) {
            synchronized (AudioFocusBridge.class) {
                audioManagerAvailabilityKnown = true;
                audioManagerAvailable = available;
                if (!available) {
                    connectionStarted = false;
                    fadedIn = false;
                } else if (requested && nativeLaunchIssued
                        && focusLossSignalled
                        && currentFocus == AUDIO_APP_MEDIA) {
                    /* Audio Management restarts invalidate all connections but
                     * do not necessarily emit another focus callback. Stock
                     * clients restore their requested context here too. */
                    focusRestoredAfterLoss = true;
                    recoveryConnectionRetries = 0;
                }
            }
            log("audio manager available=" + yn(available));
            if (!available) signalFocusLost("audio-manager-unavailable");
        }

        public void startConnection(int connection, int terminal) {
            if (connection != CONNECTION_MEDIA_MFP
                    || terminal != TERMINAL_FRONT) return;
            synchronized (AudioFocusBridge.class) {
                connectionStarted = true;
                fadedIn = false;
                recoveryConnectionRetries = 0;
            }
            log("start conn=" + connection + " terminal=" + terminal);
        }

        public void fadedIn(int connection, int terminal) {
            if (connection != CONNECTION_MEDIA_MFP
                    || terminal != TERMINAL_FRONT) return;
            synchronized (AudioFocusBridge.class) {
                connectionStarted = true;
                fadedIn = true;
                active = requested && !focusLossSignalled
                        && currentFocus == AUDIO_APP_MEDIA
                        && (!audioManagerAvailabilityKnown
                                || audioManagerAvailable);
            }
            log("faded-in conn=" + connection + " terminal=" + terminal);
        }

        public void pauseConnection(int connection, int terminal) {
            connectionLost("pause", connection, terminal, 0);
        }

        public void stopConnection(int connection, int terminal) {
            connectionLost("stop", connection, terminal, 0);
        }

        public void errorConnection(int connection, int terminal, int error) {
            connectionLost("error", connection, terminal, error);
        }

        public void updateVolumeLock(int connection, int terminal,
                                     boolean locked) {
            /* Volume, mute and ducking remain owned by the OEM audio bundle. */
        }
    };

    private static final IAudioFocusClient FOCUS_LISTENER =
            new IAudioFocusClient() {
        public void updateAudioFocus(int terminal, int app) {
            if (terminal != TERMINAL_FRONT) return;
            synchronized (AudioFocusBridge.class) {
                currentFocus = app;
                if (app == AUDIO_APP_MEDIA && focusLossSignalled)
                {
                    focusRestoredAfterLoss = true;
                    recoveryConnectionRetries = 0;
                }
                if (app != AUDIO_APP_MEDIA) {
                    connectionStarted = false;
                    fadedIn = false;
                    focusRestoredAfterLoss = false;
                }
            }
            log("focus terminal=" + terminal + " app=" + app);
            if (app != AUDIO_APP_MEDIA) signalFocusLost("focus-app-" + app);
        }
    };

    private static final ATIPMediaRouterServiceListener ROUTE_LISTENER =
            new ATIPMediaRouterServiceListener() {
        public void updateActiveAudioRoutes(ATIPAudioRoute[] routes) {
            if (routes == null) return;
            for (int i = 0; i < routes.length; i++) {
                ATIPAudioRoute route = routes[i];
                if (route == null
                        || route.getRoutingOutput() != ROUTING_OUTPUT_MPL1)
                    continue;

                int input = route.getRoutingInput();
                int status = route.getRouteStatus();
                boolean routeLost;
                synchronized (AudioFocusBridge.class) {
                    currentRoute = input;
                    currentRouteStatus = status;
                    if (previousRouteCaptureOpen && previousRoute < 0)
                        previousRoute = input;
                    routeConfirmed = routeTakeoverIssued
                            && input == ROUTING_INPUT_INTERNAL_MEDIA
                            && status == ROUTING_RESULT_OK;
                    if (routeTakeoverIssued
                            && (input != ROUTING_INPUT_INTERNAL_MEDIA
                                    || status != ROUTING_RESULT_OK))
                        routeCommandAccepted = false;
                    routeLost = routeTakeoverIssued
                            && (input != ROUTING_INPUT_INTERNAL_MEDIA
                                    || status != ROUTING_RESULT_OK);
                }
                log("route output=" + ROUTING_OUTPUT_MPL1
                        + " input=" + input + " status=" + status);
                if (routeLost)
                    signalFocusLost("route-not-active input=" + input
                            + " status=" + status);
            }
        }
    };

    private AudioFocusBridge() {}

    /**
     * Starts asynchronous session acquisition.  launch is called exactly once
     * after focus, connection and MPL1 routing are confirmed, so native QSA is
     * never started against an unowned entertainment route.
     */
    public static synchronized void request(ClassLoader hint, Runnable launch,
                                            Runnable failure) {
        MediaSessionBridge.ensureRegistered(hint);
        boolean handoff = baselineCaptured && !releaseInProgress;
        classLoaderHint = hint;
        launchCallback = launch;
        failureCallback = failure;
        requested = true;
        active = false;
        nativeLaunchIssued = false;
        connectionStarted = false;
        fadedIn = false;
        routeTakeoverIssued = false;
        routeConfirmed = false;
        routeCommandAccepted = false;
        focusLossSignalled = false;
        focusRestoredAfterLoss = false;
        recoveryStartIssued = false;
        lossArmed = false;
        recoveryConnectionRetries = 0;
        if (!handoff) {
            connectionRequested = false;
            baselineCaptured = false;
            previousRouteCaptureOpen = true;
            previousFocus = -1;
            previousConnection = -1;
            previousRoute = -1;
        } else {
            /* A release was requested, but native QSA has not closed yet. Keep
             * the original OEM baseline: focus=2/connection=20/route=1 are our
             * ownership, not the state to restore after this new session. */
            previousRouteCaptureOpen = false;
        }
        currentRoute = -1;
        currentRouteStatus = -1;
        currentFocus = -1;
        generation++;
        if (handoff)
            log("transferring pending OEM audio baseline to new session");
        if (releaseInProgress || workerRunning) return;
        startWorker(generation);
    }

    public static void request(ClassLoader hint, Runnable launch) {
        request(hint, launch, null);
    }

    /** Compatibility signature; a missing launch callback fails closed. */
    public static void request(ClassLoader hint) {
        request(hint, null, null);
    }

    /**
     * Stops new callbacks/recovery immediately, but keeps the OEM connection
     * alive until native QSA has closed. Returns a token for finishRelease().
     */
    public static synchronized int beginRelease() {
        if (requested) generation++;
        requested = false;
        active = false;
        return generation;
    }

    /** Completes an idempotent release unless a newer session has begun. */
    public static void finishRelease(int token) {
        releaseOwnedState(token);
    }

    /** Compatibility for tools built against the previous bridge. */
    public static void release() {
        int token = beginRelease();
        finishRelease(token);
    }

    private static synchronized void startWorker(final int workerGeneration) {
        if (workerRunning || releaseInProgress) return;
        workerRunning = true;
        Thread worker = new Thread(new Runnable() {
            public void run() {
                try {
                    runSession(workerGeneration);
                } catch (Throwable t) {
                    log("session worker failed " + t);
                    boolean current;
                    boolean launched;
                    synchronized (AudioFocusBridge.class) {
                        current = isCurrentLocked(workerGeneration);
                        launched = nativeLaunchIssued;
                    }
                    if (current) {
                        if (launched) {
                            Shell.terminateRetroArch();
                            notifyFailure("audio worker exception " + t);
                        } else {
                            abortBeforeNative(workerGeneration,
                                    "audio worker exception " + t);
                        }
                    }
                } finally {
                    boolean restart;
                    int nextGeneration;
                    synchronized (AudioFocusBridge.class) {
                        workerRunning = false;
                        restart = requested && generation != workerGeneration;
                        nextGeneration = generation;
                    }
                    if (restart) startWorker(nextGeneration);
                }
            }
        });
        worker.setName("retroarch-audio-focus");
        worker.setDaemon(true);
        worker.start();
    }

    private static void runSession(int workerGeneration) {
        long deadline = now() + SERVICE_TIMEOUT_MS;
        while (isCurrent(workerGeneration) && now() < deadline) {
            if (resolveServices() && registerListeners()) break;
            sleep(SERVICE_RETRY_MS);
        }
        if (!isCurrent(workerGeneration)) return;
        if (!servicesReady() || !listenersReady()) {
            abortBeforeNative(workerGeneration,
                    "audio services unavailable after " + SERVICE_TIMEOUT_MS + "ms");
            return;
        }

        capturePreviousState(workerGeneration);
        if (!waitForAudioManager(workerGeneration, 3000L)) {
            if (isCurrent(workerGeneration))
                abortBeforeNative(workerGeneration, "audio manager unavailable");
            return;
        }

        if (!waitForMediaSession(workerGeneration, 2000L))
            log("stock Media terminal extension not ready; using HMIAudio fallback");

        /* Start QSA before asking HMIAudio to start connection 20.  On MU1316
         * a STARTED entertainment connection with no PCM producer is paused
         * again almost immediately.  The old ordering (connection -> route ->
         * native launch) therefore raced its own canLaunchNative() gate and
         * bounced RaScreen straight back to MainWizard.  Opening and pre-filling
         * mpl1_int_ent is inaudible until the OEM route/fade below, so waiting
         * for the ready marker here keeps routing fully owned by the stock
         * audio manager while removing that producer/connection deadlock. */
        if (!launchNativeOnce(workerGeneration)) {
            if (isCurrent(workerGeneration))
                abortBeforeNative(workerGeneration,
                        "native launch callback unavailable or failed");
            return;
        }

        if (!waitForInitialPcmReady(workerGeneration, PCM_TIMEOUT_MS)) {
            if (!isCurrent(workerGeneration)) return;
            log("QSA did not report PCM ready before audio routing");
            Shell.terminateRetroArch();
            notifyFailure("QSA PCM handshake timeout");
            return;
        }

        /* Tell the stock Media application that connection 20 is its active
         * playing context before changing focus.  Otherwise a late stock
         * focus callback may restore NO_PLAYABLE_FILES suppression (9) and
         * stop the direct connection 20 we just requested. */
        boolean stockMediaPrepared = MediaSessionBridge.preparePlayingAudio();
        if (stockMediaPrepared) {
            synchronized (AudioFocusBridge.class) {
                connectionRequested = true;
                lastConnectionRequestAt = now();
            }
        }

        try {
            focusManager.setActiveAudioApp(TERMINAL_FRONT, AUDIO_APP_MEDIA);
            log("requested front audio focus app=" + AUDIO_APP_MEDIA);
        } catch (Throwable t) {
            abortBeforeNative(workerGeneration, "setActiveAudioApp failed " + t);
            return;
        }

        if (!waitForFocus(workerGeneration, AUDIO_APP_MEDIA, 3000L)) {
            if (isCurrent(workerGeneration))
                waitMutedForRecovery(workerGeneration,
                        "front media focus was not confirmed");
            return;
        }

        synchronized (AudioFocusBridge.class) {
            /* The initial CarPlay=48 callback is now behind us.  Every later
             * focus/connection edge is a real interruption. */
            lossArmed = true;
        }
        /* The stock focus callback normally starts its newly armed context 20.
         * Give it a short window, then use the direct call as a fallback. */
        if (!stockMediaPrepared
                || !waitForConnectionStarted(workerGeneration, 750L))
            requestConnection();
        if (!waitForConnectionStarted(workerGeneration, CONNECTION_TIMEOUT_MS)) {
            if (isCurrent(workerGeneration))
                waitMutedForRecovery(workerGeneration,
                        "connection 20 did not reach STARTED");
            return;
        }

        /* Allow a cached route callback to arrive before overwriting MPL1. */
        sleep(200L);
        if (!isCurrent(workerGeneration)) return;
        setMediaRoute();
        if (!waitForRoute(workerGeneration, ROUTE_TIMEOUT_MS)) {
            if (isCurrent(workerGeneration))
                waitMutedForRecovery(workerGeneration,
                        "MPL1 route 1->1 was not confirmed");
            return;
        }

        if (!canLaunchNative(workerGeneration)) {
            if (isCurrent(workerGeneration))
                waitMutedForRecovery(workerGeneration,
                        "audio ownership changed before fade-in");
            return;
        }

        if (!activationInterrupted()) {
            fadeToConnection();
            if (!waitForFadedIn(workerGeneration, FADE_TIMEOUT_MS)) {
                if (!isCurrent(workerGeneration)) return;
                if (activationInterrupted()) {
                    log("audio ownership changed during fade; waiting for recovery");
                } else {
                    log("connection 20 did not reach FADED_IN");
                    waitMutedForRecovery(workerGeneration,
                            "connection 20 fade timeout");
                    return;
                }
            }
        }

        boolean becameActive;
        synchronized (AudioFocusBridge.class) {
            if (isCurrentLocked(workerGeneration) && fadedIn
                    && !focusLossSignalled
                    && currentFocus == AUDIO_APP_MEDIA
                    && (!audioManagerAvailabilityKnown || audioManagerAvailable)
                    && connectionStarted && routeUsableLocked()) {
                active = true;
                focusLossSignalled = false;
                focusRestoredAfterLoss = false;
                lossArmed = true;
            }
            becameActive = active;
        }
        if (becameActive) {
            log("ACTIVE focus=2 connection=20 route=1->1 PCM=ready");
            MediaSessionBridge.publishPlaying();
        }

        /* Stay alive to restore QSA after temporary phone/navigation focus. */
        monitorSession(workerGeneration);
    }

    private static void monitorSession(int workerGeneration) {
        while (isCurrent(workerGeneration)) {
            boolean needsRecovery;
            synchronized (AudioFocusBridge.class) {
                needsRecovery = focusLossSignalled
                        && currentFocus == AUDIO_APP_MEDIA
                        && (!audioManagerAvailabilityKnown
                                || audioManagerAvailable);
            }
            if (needsRecovery)
                recoverAfterInterruption(workerGeneration);
            sleep(100L);
        }
    }

    /** Keep the foreground RA state alive while audio ownership is elsewhere. */
    private static void waitMutedForRecovery(int workerGeneration,
                                             String reason) {
        boolean signal;
        synchronized (AudioFocusBridge.class) {
            if (!isCurrentLocked(workerGeneration)) return;
            signal = !focusLossSignalled || recoveryStartIssued;
            lossArmed = true;
            focusLossSignalled = true;
            focusRestoredAfterLoss = currentFocus == AUDIO_APP_MEDIA;
            recoveryStartIssued = false;
            recoveryConnectionRetries = 0;
            active = false;
            fadedIn = false;
            lastConnectionRequestAt = 0L;
        }
        if (signal) Shell.signalRetroArchAudioFocusLost();
        log("audio activation interrupted: " + reason
                + "; keeping RA screen and waiting muted for recovery");
        monitorSession(workerGeneration);
    }

    private static void recoverAfterInterruption(int workerGeneration) {
        boolean started;
        boolean mayRequest;
        int retries;
        long requestedAt;
        synchronized (AudioFocusBridge.class) {
            started = connectionStarted;
            mayRequest = focusRestoredAfterLoss;
            retries = recoveryConnectionRetries;
            requestedAt = lastConnectionRequestAt;
        }
        if (!started) {
            /* A pause callback with focus still on Media is a transient OEM
             * duck/interruption; its existing request remains pending and we
             * must not fight it. Re-request only after a real focus-away/back
             * edge has been observed. */
            if (mayRequest && retries < MAX_RECOVERY_CONNECTION_RETRIES
                    && now() - requestedAt >= 1000L) {
                synchronized (AudioFocusBridge.class) {
                    if (!recoveryContextReadyLocked(workerGeneration)
                            || recoveryConnectionRetries != retries)
                        return;
                    recoveryConnectionRetries = retries + 1;
                }
                requestConnection();
            }
            return;
        }

        if (!recoveryOwnershipReady(workerGeneration, false)) return;
        setMediaRoute();
        if (!waitForRoute(workerGeneration, ROUTE_TIMEOUT_MS)) return;
        if (!recoveryOwnershipReady(workerGeneration, true)) return;

        /* SIGRTMIN+1 restarts and pre-fills QSA, but deliberately leaves the
         * core paused. The player decides when gameplay resumes. */
        /* Shell signals are launched asynchronously. Wait until SIGRTMIN has
         * actually cleared the marker before sending SIGRTMIN+1, otherwise a
         * rapid phone-focus round trip can reorder stop/start at the process. */
        if (!waitForPcmStopped(workerGeneration, 1500L)) {
            if (!recoveryOwnershipReady(workerGeneration, true)) return;
            Shell.signalRetroArchAudioFocusLost();
            if (!waitForPcmStopped(workerGeneration, 1500L)) return;
        }
        if (!recoveryOwnershipReady(workerGeneration, true)) return;
        synchronized (AudioFocusBridge.class) {
            if (!recoveryOwnershipReadyLocked(workerGeneration, true)) return;
            recoveryStartIssued = true;
        }
        Shell.signalRetroArchAudioFocusGained();
        if (!waitForRecoveryPcmReady(workerGeneration, 3000L)) {
            cancelRecoveryStart("ownership changed or PCM restart timed out");
            return;
        }
        if (!recoveryOwnershipReady(workerGeneration, true)) {
            cancelRecoveryStart("ownership changed after PCM restart");
            return;
        }
        fadeToConnection();
        if (!waitForFadedIn(workerGeneration, FADE_TIMEOUT_MS)
                || !recoveryOwnershipReady(workerGeneration, true)) {
            cancelRecoveryStart("ownership changed during recovery fade");
            return;
        }

        synchronized (AudioFocusBridge.class) {
            if (!recoveryOwnershipReadyLocked(workerGeneration, true)
                    || !fadedIn) {
                recoveryStartIssued = false;
                Shell.signalRetroArchAudioFocusLost();
                return;
            }
            active = true;
            focusLossSignalled = false;
            focusRestoredAfterLoss = false;
            recoveryStartIssued = false;
        }
        log("audio recovered after interruption; gameplay remains paused");
        MediaSessionBridge.publishPlaying();
    }

    private static boolean resolveServices() {
        if (hmiAudio == null) {
            Object service = DsiReflection.getServiceByProperty(
                    "de.audi.atip.audio.HMIAudioService",
                    HMIAudioService.AUDIO_CLIENT_ID,
                    HMIAudioService.CLIENT_MEDIA, classLoaderHint);
            if (service instanceof HMIAudioService)
                hmiAudio = (HMIAudioService)service;
        }
        if (focusManager == null) {
            Object service = DsiReflection.getService(
                    "de.audi.atip.audio.IAudioFocusManager", classLoaderHint);
            if (service instanceof IAudioFocusManager) {
                focusManager = (IAudioFocusManager)service;
                if (service instanceof ILastmodeHandler)
                    lastmodeHandler = (ILastmodeHandler)service;
            }
        }
        if (mediaRouterService == null) {
            Object service = DsiReflection.getService(
                    "de.audi.atip.interapp.audio.ATIPMediaRouterService",
                    classLoaderHint);
            if (service instanceof ATIPMediaRouterService)
                mediaRouterService = (ATIPMediaRouterService)service;
        }
        return servicesReady();
    }

    private static boolean servicesReady() {
        return hmiAudio != null && focusManager != null
                && mediaRouterService != null;
    }

    private static boolean registerListeners() {
        try {
            if (audioListenerRegistration == null) {
                Hashtable properties = new Hashtable();
                properties.put(HMIAudioService.AUDIO_CLIENT_ID,
                        HMIAudioService.CLIENT_MEDIA);
                audioListenerRegistration = new ServiceInfo(null,
                        HMIAudioServiceListener.class.getName(),
                        AUDIO_LISTENER, properties);
            }
            if (focusListenerRegistration == null)
                focusListenerRegistration = new ServiceInfo(null,
                        IAudioFocusClient.class.getName(), FOCUS_LISTENER,
                        new Hashtable());
            if (routeListenerRegistration == null)
                routeListenerRegistration = new ServiceInfo(null,
                        ATIPMediaRouterServiceListener.class.getName(),
                        ROUTE_LISTENER, new Hashtable());
            return true;
        } catch (Throwable t) {
            log("listener registration failed " + t);
            return false;
        }
    }

    private static boolean listenersReady() {
        return audioListenerRegistration != null
                && focusListenerRegistration != null
                && routeListenerRegistration != null;
    }

    private static void capturePreviousState(int workerGeneration) {
        int capturedFocus = -1;
        int capturedConnection = -1;
        try {
            if (lastmodeHandler != null)
                capturedFocus = lastmodeHandler.getLastmodeAudio(TERMINAL_FRONT);
        } catch (Throwable t) {
            log("getLastmodeAudio failed " + t);
        }
        if (capturedFocus < 0) {
            synchronized (AudioFocusBridge.class) {
                capturedFocus = currentFocus;
            }
        }

        try {
            capturedConnection = hmiAudio.getActiveEntertainmentConnection(
                    TERMINAL_FRONT);
        } catch (Throwable t) {
            log("getActiveEntertainmentConnection failed " + t);
        }
        synchronized (AudioFocusBridge.class) {
            if (!isCurrentLocked(workerGeneration)) return;
            if (!baselineCaptured) {
                previousFocus = capturedFocus;
                previousConnection = capturedConnection;
                baselineCaptured = true;
                log("captured focus=" + previousFocus + " connection="
                        + previousConnection + " route=" + previousRoute);
            } else {
                log("reusing captured focus=" + previousFocus + " connection="
                        + previousConnection + " route=" + previousRoute);
            }
        }
    }

    private static boolean waitForAudioManager(int workerGeneration,
                                               long timeout) {
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            synchronized (AudioFocusBridge.class) {
                if (audioManagerAvailabilityKnown)
                    return audioManagerAvailable;
            }
            sleep(50L);
        }
        /* Older bundles may not publish the availability callback. Service
         * presence is the best safe fallback and requestConnection still has
         * its own confirmed STARTED gate. */
        return isCurrent(workerGeneration) && hmiAudio != null;
    }

    private static boolean waitForMediaSession(int workerGeneration,
                                               long timeout) {
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            if (MediaSessionBridge.isReady()) return true;
            sleep(50L);
        }
        return isCurrent(workerGeneration) && MediaSessionBridge.isReady();
    }

    private static boolean waitForFocus(int workerGeneration, int wanted,
                                        long timeout) {
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            int observed;
            synchronized (AudioFocusBridge.class) { observed = currentFocus; }
            if (observed == wanted) return true;
            if (lastmodeHandler != null) {
                try {
                    observed = lastmodeHandler.getLastmodeAudio(TERMINAL_FRONT);
                    synchronized (AudioFocusBridge.class) {
                        currentFocus = observed;
                    }
                    if (observed == wanted) return true;
                } catch (Throwable ignored) {}
            }
            sleep(50L);
        }
        return false;
    }

    private static void requestConnection() {
        /* Reassert the stock Media context before every direct retry. */
        MediaSessionBridge.preparePlayingAudio();
        try {
            hmiAudio.requestConnection(CONNECTION_MEDIA_MFP,
                    TERMINAL_FRONT, 0);
            synchronized (AudioFocusBridge.class) {
                connectionRequested = true;
                lastConnectionRequestAt = now();
            }
            log("requested connection=20 terminal=0 (no auto-fade)");
        } catch (Throwable t) {
            log("requestConnection failed " + t);
        }
    }

    private static boolean waitForConnectionStarted(int workerGeneration,
                                                    long timeout) {
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            synchronized (AudioFocusBridge.class) {
                if (connectionStarted || fadedIn) return true;
            }
            try {
                int status = hmiAudio.getStatus(CONNECTION_MEDIA_MFP,
                        TERMINAL_FRONT);
                if (status == CONN_STARTED || status == CONN_FADED_IN) {
                    synchronized (AudioFocusBridge.class) {
                        connectionStarted = true;
                        fadedIn = status == CONN_FADED_IN;
                    }
                    return true;
                }
            } catch (Throwable ignored) {}
            sleep(50L);
        }
        return false;
    }

    private static void setMediaRoute() {
        try {
            synchronized (AudioFocusBridge.class) {
                previousRouteCaptureOpen = false;
                routeTakeoverIssued = true;
                routeConfirmed = currentRoute == ROUTING_INPUT_INTERNAL_MEDIA
                        && currentRouteStatus == ROUTING_RESULT_OK;
                routeCommandAccepted = false;
            }
            mediaRouterService.setAudioRoutes(new ATIPAudioRoute[] {
                    new ATIPAudioRouteImpl(ROUTING_INPUT_INTERNAL_MEDIA,
                            ROUTING_OUTPUT_MPL1, ROUTING_RESULT_OK)
            });
            synchronized (AudioFocusBridge.class) {
                routeCommandAccepted = currentRoute < 0
                        || (currentRoute == ROUTING_INPUT_INTERNAL_MEDIA
                                && currentRouteStatus == ROUTING_RESULT_OK);
                routeCommandAcceptedAt = now();
            }
            log("requested ATIP route 1->1");
        } catch (Throwable t) {
            synchronized (AudioFocusBridge.class) {
                routeCommandAccepted = false;
            }
            log("ATIP setAudioRoutes failed " + t);
        }
    }

    private static boolean waitForRoute(int workerGeneration, long timeout) {
        long deadline = now() + timeout;
        long retryAt = now() + 1000L;
        while (isCurrent(workerGeneration) && now() < deadline) {
            boolean acceptedWithoutEcho;
            synchronized (AudioFocusBridge.class) {
                if (routeConfirmed) return true;
                /* Setting an already-active route may produce no DSI update.
                 * The stock ATIP service is a synchronous in-process adapter;
                 * after a short rejection window, a returned call is the only
                 * available confirmation for that idempotent case. */
                acceptedWithoutEcho = routeCommandAccepted
                        && now() - routeCommandAcceptedAt >= 300L;
            }
            if (acceptedWithoutEcho) {
                log("route command accepted (no DSI echo; likely unchanged)");
                return true;
            }
            if (now() >= retryAt) {
                setMediaRoute();
                retryAt = now() + 1000L;
            }
            sleep(50L);
        }
        return false;
    }

    private static boolean launchNativeOnce(int workerGeneration) {
        Runnable callback;
        synchronized (AudioFocusBridge.class) {
            if (!isCurrentLocked(workerGeneration) || nativeLaunchIssued)
                return false;
            callback = launchCallback;
            if (callback == null) return false;
            nativeLaunchIssued = true;
        }
        try {
            callback.run();
            return true;
        } catch (Throwable t) {
            synchronized (AudioFocusBridge.class) {
                if (isCurrentLocked(workerGeneration))
                    nativeLaunchIssued = false;
            }
            log("native launch callback failed " + t);
            return false;
        }
    }

    private static boolean waitForRecoveryPcmReady(int workerGeneration,
                                                   long timeout) {
        String path = System.getProperty("ra.qnx.audio.ready");
        if (path == null || path.length() == 0) path = PCM_READY_DEFAULT;
        File marker = new File(path);
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            if (marker.exists()) return true;
            if (!recoveryOwnershipReady(workerGeneration, true)) return false;
            sleep(50L);
        }
        return false;
    }

    private static boolean waitForInitialPcmReady(int workerGeneration,
                                                  long timeout) {
        String path = System.getProperty("ra.qnx.audio.ready");
        if (path == null || path.length() == 0) path = PCM_READY_DEFAULT;
        File marker = new File(path);
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            if (marker.exists()) return true;
            /* Initial PCM prefill deliberately happens before focus,
             * connection 20 and MPL1 routing.  activationInterrupted() tests
             * those later ownership conditions, so consulting it here makes
             * this wait fail immediately by construction.  Generation
             * invalidation remains the cancellation authority. */
            sleep(50L);
        }
        return false;
    }

    private static boolean waitForPcmStopped(int workerGeneration,
                                             long timeout) {
        String path = System.getProperty("ra.qnx.audio.ready");
        if (path == null || path.length() == 0) path = PCM_READY_DEFAULT;
        File marker = new File(path);
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            if (!marker.exists()) return true;
            sleep(50L);
        }
        return !marker.exists();
    }

    private static boolean canLaunchNative(int workerGeneration) {
        synchronized (AudioFocusBridge.class) {
            return isCurrentLocked(workerGeneration)
                    && currentFocus == AUDIO_APP_MEDIA
                    && (!audioManagerAvailabilityKnown || audioManagerAvailable)
                    && connectionStarted
                    && routeUsableLocked();
        }
    }

    private static boolean routeUsableLocked() {
        return routeConfirmed || (routeCommandAccepted
                && (currentRoute < 0
                        || (currentRoute == ROUTING_INPUT_INTERNAL_MEDIA
                                && currentRouteStatus == ROUTING_RESULT_OK)));
    }

    private static boolean recoveryContextReadyLocked(int workerGeneration) {
        return isCurrentLocked(workerGeneration)
                && focusLossSignalled
                && currentFocus == AUDIO_APP_MEDIA
                && (!audioManagerAvailabilityKnown || audioManagerAvailable);
    }

    private static boolean recoveryOwnershipReadyLocked(int workerGeneration,
                                                         boolean requireRoute) {
        return recoveryContextReadyLocked(workerGeneration)
                && connectionStarted
                && (!requireRoute || routeUsableLocked());
    }

    private static boolean recoveryOwnershipReady(int workerGeneration,
                                                   boolean requireRoute) {
        synchronized (AudioFocusBridge.class) {
            return recoveryOwnershipReadyLocked(workerGeneration, requireRoute);
        }
    }

    private static void cancelRecoveryStart(String reason) {
        boolean stop;
        synchronized (AudioFocusBridge.class) {
            stop = recoveryStartIssued;
            recoveryStartIssued = false;
        }
        if (stop) {
            Shell.signalRetroArchAudioFocusLost();
            log("cancelled QSA focus recovery: " + reason);
        }
    }

    private static boolean activationInterrupted() {
        synchronized (AudioFocusBridge.class) {
            return focusLossSignalled
                    || currentFocus != AUDIO_APP_MEDIA
                    || (audioManagerAvailabilityKnown && !audioManagerAvailable)
                    || !connectionStarted
                    || (currentRoute >= 0 && !routeUsableLocked());
        }
    }

    private static void fadeToConnection() {
        try {
            hmiAudio.fadeToConnection(CONNECTION_MEDIA_MFP, TERMINAL_FRONT);
            log("requested fadeTo connection=20 terminal=0");
        } catch (Throwable t) {
            log("fadeToConnection failed " + t);
        }
    }

    private static boolean waitForFadedIn(int workerGeneration, long timeout) {
        long deadline = now() + timeout;
        while (isCurrent(workerGeneration) && now() < deadline) {
            synchronized (AudioFocusBridge.class) {
                if (fadedIn) return true;
            }
            try {
                if (hmiAudio.getStatus(CONNECTION_MEDIA_MFP,
                        TERMINAL_FRONT) == CONN_FADED_IN) {
                    synchronized (AudioFocusBridge.class) { fadedIn = true; }
                    return true;
                }
            } catch (Throwable ignored) {}
            sleep(50L);
        }
        return false;
    }

    private static void releaseOwnedState(int token) {
        HMIAudioService audio;
        IAudioFocusManager focus;
        ATIPMediaRouterService router;
        int oldFocus;
        int oldConnection;
        int oldRoute;
        boolean ownedConnection;
        boolean stockContextRestored;

        synchronized (AudioFocusBridge.class) {
            if (token != generation || requested || releaseInProgress) {
                log("ignored stale release token=" + token
                        + " generation=" + generation
                        + " requested=" + yn(requested)
                        + " releasing=" + yn(releaseInProgress));
                return;
            }
            releaseInProgress = true;
            audio = hmiAudio;
            focus = focusManager;
            router = mediaRouterService;
            oldFocus = previousFocus;
            oldConnection = previousConnection;
            oldRoute = previousRoute;
            ownedConnection = connectionRequested;
            connectionRequested = false;
            connectionStarted = false;
            fadedIn = false;
            routeTakeoverIssued = false;
            routeConfirmed = false;
            routeCommandAccepted = false;
            baselineCaptured = false;
            previousRouteCaptureOpen = false;
            focusLossSignalled = false;
            focusRestoredAfterLoss = false;
            recoveryStartIssued = false;
            lossArmed = false;
            recoveryConnectionRetries = 0;
            nativeLaunchIssued = false;
        }

        /* If Media already owned connection 20 before RA, keep the shared OEM
         * connection alive. Otherwise release only the connection we acquired. */
        if (ownedConnection && !(oldFocus == AUDIO_APP_MEDIA
                && oldConnection == CONNECTION_MEDIA_MFP) && audio != null) {
            try {
                audio.releaseConnection(CONNECTION_MEDIA_MFP, TERMINAL_FRONT);
                log("released connection=20 terminal=0");
            } catch (Throwable t) {
                log("releaseConnection failed " + t);
            }
        }

        if (oldRoute >= 0 && oldRoute != ROUTING_INPUT_INTERNAL_MEDIA
                && router != null) {
            try {
                router.setAudioRoutes(new ATIPAudioRoute[] {
                        new ATIPAudioRouteImpl(oldRoute, ROUTING_OUTPUT_MPL1,
                                ROUTING_RESULT_OK)
                });
                log("restored MPL1 route input=" + oldRoute);
            } catch (Throwable t) {
                log("restore route failed " + t);
            }
        }

        /* Restore Media's own activeAudioContext before restoring focus.  Its
         * next focus callback must revive the previous connection, not RA 20. */
        stockContextRestored = MediaSessionBridge.restoreAudioContext(
                oldConnection);

        if (oldFocus > 0 && focus != null) {
            try {
                focus.setActiveAudioApp(TERMINAL_FRONT, oldFocus);
                log("restored front audio focus app=" + oldFocus);
            } catch (Throwable t) {
                log("restore focus failed " + t);
            }
        }

        /* If another Media connection was active, focus app=2 may not change
         * and therefore emits no restoration edge. Resume it explicitly. */
        if (oldFocus == AUDIO_APP_MEDIA && oldConnection > 0
                && oldConnection != CONNECTION_MEDIA_MFP && audio != null
                && !stockContextRestored) {
            try {
                audio.requestAndFadeToConnection(oldConnection, TERMINAL_FRONT);
                log("restored previous Media connection=" + oldConnection);
            } catch (Throwable t) {
                log("restore previous Media connection failed " + t);
            }
        }

        MediaSessionBridge.restoreNoPlayableIfSuppressed(oldConnection);

        unregisterListeners();
        boolean restart;
        int nextGeneration;
        synchronized (AudioFocusBridge.class) {
            audioManagerAvailabilityKnown = false;
            audioManagerAvailable = false;
            currentFocus = -1;
            previousFocus = -1;
            previousConnection = -1;
            previousRoute = -1;
            currentRoute = -1;
            currentRouteStatus = -1;
            if (!requested) {
                launchCallback = null;
                failureCallback = null;
            }
            releaseInProgress = false;
            restart = requested && !workerRunning;
            nextGeneration = generation;
        }
        log("audio session released after native PCM close");
        if (restart) startWorker(nextGeneration);
    }

    private static void unregisterListeners() {
        ServiceInfo audio;
        ServiceInfo focus;
        ServiceInfo route;
        synchronized (AudioFocusBridge.class) {
            audio = audioListenerRegistration;
            focus = focusListenerRegistration;
            route = routeListenerRegistration;
            audioListenerRegistration = null;
            focusListenerRegistration = null;
            routeListenerRegistration = null;
        }
        unregister(audio, "HMIAudio");
        unregister(focus, "audio-focus");
        unregister(route, "media-route");
    }

    private static void unregister(ServiceInfo registration, String name) {
        if (registration == null) return;
        try { registration.unregister(); }
        catch (Throwable t) { log(name + " listener unregister failed " + t); }
    }

    private static void connectionLost(String event, int connection,
                                       int terminal, int error) {
        if (connection != CONNECTION_MEDIA_MFP
                || terminal != TERMINAL_FRONT) return;
        synchronized (AudioFocusBridge.class) {
            connectionStarted = false;
            fadedIn = false;
            active = false;
            if ("error".equals(event) && currentFocus == AUDIO_APP_MEDIA)
                focusRestoredAfterLoss = true;
        }
        log(event + " conn=" + connection + " terminal=" + terminal
                + (error == 0 ? "" : " error=" + error));
        signalFocusLost(event);
    }

    private static void signalFocusLost(String reason) {
        boolean notify = false;
        synchronized (AudioFocusBridge.class) {
            /* Only a session that actually reached ACTIVE can lose anything.
             * QSA is started before focus is requested, so during acquisition
             * the first updateAudioFocus() merely reports whichever app owns
             * the front terminal right now (CarPlay = 48).  Treating that
             * baseline as a loss sent SIGRTMIN + desired=0 to the QSA we had
             * just launched; nothing in the acquisition path re-arms desired,
             * so connection 20 was stopped again for lack of a PCM producer
             * and activation aborted every time.  Acquisition failures are
             * already covered by activationInterrupted()/canLaunchNative(). */
            if (requested && lossArmed
                    && (!focusLossSignalled || recoveryStartIssued)) {
                focusLossSignalled = true;
                recoveryStartIssued = false;
                active = false;
                notify = true;
            }
        }
        if (notify) {
            Shell.signalRetroArchAudioFocusLost();
            log("focus lost (" + reason
                    + "); paused core and stopped QSA via SIGRTMIN");
        }
    }

    private static void abortBeforeNative(int workerGeneration, String reason) {
        Runnable failure;
        int token;
        synchronized (AudioFocusBridge.class) {
            if (!isCurrentLocked(workerGeneration)) return;
            requested = false;
            generation++;
            token = generation;
            failure = failureCallback;
        }
        log("activation aborted: " + reason);
        finishRelease(token);
        if (failure != null) {
            try { failure.run(); }
            catch (Throwable t) { log("failure callback failed " + t); }
        }
    }

    private static void notifyFailure(String reason) {
        Runnable failure;
        synchronized (AudioFocusBridge.class) { failure = failureCallback; }
        log("session failure: " + reason);
        if (failure != null) {
            try { failure.run(); }
            catch (Throwable t) { log("failure callback failed " + t); }
        }
    }

    private static boolean isCurrent(int workerGeneration) {
        synchronized (AudioFocusBridge.class) {
            return isCurrentLocked(workerGeneration);
        }
    }

    private static boolean isCurrentLocked(int workerGeneration) {
        return requested && generation == workerGeneration;
    }

    private static long now() {
        return System.currentTimeMillis();
    }

    private static void sleep(long millis) {
        try { Thread.sleep(millis); }
        catch (InterruptedException ignored) {}
    }

    private static String yn(boolean value) {
        return value ? "1" : "0";
    }

    static void log(String message) {
        String line = "[RA-AUDIO] " + message;
        try { System.out.println(line); }
        catch (Throwable ignored) {}

        String record = System.currentTimeMillis() + " " + message + "\n";
        if (!appendLog("/fs/sda0/retroarch/logs/ra_audio.log", record))
            appendLog("/tmp/ra_audio.log", record);
    }

    private static synchronized boolean appendLog(String path, String line) {
        FileWriter writer = null;
        try {
            File file = new File(path);
            if (file.isFile() && file.length() >= MAX_DIAGNOSTIC_LOG_BYTES) {
                File previous = new File(path + ".1");
                try { previous.delete(); }
                catch (Throwable ignored) {}
                if (!file.renameTo(previous)) {
                    FileWriter truncate = null;
                    try { truncate = new FileWriter(file, false); }
                    finally {
                        if (truncate != null) {
                            try { truncate.close(); }
                            catch (Throwable ignored) {}
                        }
                    }
                }
            }
            writer = new FileWriter(file, true);
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
}
