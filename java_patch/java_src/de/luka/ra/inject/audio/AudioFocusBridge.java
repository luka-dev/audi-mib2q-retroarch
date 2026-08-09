package de.luka.ra.inject.audio;

import de.audi.atip.audio.HMIAudioService;
import de.audi.atip.audio.HMIAudioServiceListener;
import de.audi.atip.audio.IAudioFocusManager;
import de.audi.audio.sdis.ifc.SdisAudioService;
import de.dreisoft.lsd.ServiceInfo;
import de.luka.ra.inject.Shell;
import java.util.Hashtable;
import org.dsi.ifc.media.AudioRoute;
import org.dsi.ifc.media.DSIMediaRouter;

/**
 * A complete MU1316 entertainment-audio session owned by the injected
 * RaScreen lifecycle.  The stock LSD audio bundle remains the sole DSI/RPC
 * owner: this class consumes its public HMI services exactly like an OEM media
 * application.  Nothing is added to framework.json and no OEM audio class is
 * shadowed.
 */
public final class AudioFocusBridge {
    private static final int CONNECTION_MEDIA_MFP = 20;
    private static final int HMI_TERMINAL_FRONT = 0;
    private static final int FOCUS_TERMINAL_FRONT = 0;
    private static final int AUDIO_CONTEXT_MEDIA = 3;
    private static final int AUDIO_CONTEXT_NONE = 0;
    private static final int AUDIO_APP_MEDIA = 2;

    private static final int AUDIO_SOURCE_EXTERNAL = 2;
    private static final int VIDEO_SOURCE_NONE = 0;
    private static final int SINK_HEADUNIT = 1;
    private static final int VIRTUAL_CHANNEL_INTERNAL_MEDIA = 1;
    private static final int PHYSICAL_CHANNEL_MPL1 = 1;
    private static final int ROUTING_RESULT_OK = 0;
    private static final int RETRY_DELAY_MS = 1000;

    private static volatile boolean requested;
    private static boolean active;
    private static boolean workerRunning;
    private static boolean connectionRequested;
    private static boolean focusLossSignalled;
    private static boolean audioManagerAvailabilityKnown;
    private static boolean audioManagerAvailable;
    private static ClassLoader classLoaderHint;

    private static HMIAudioService hmiAudio;
    private static DSIMediaRouter mediaRouter;
    private static IAudioFocusManager focusManager;
    private static SdisAudioService sdisAudio;
    private static ServiceInfo listenerRegistration;

    private static final HMIAudioServiceListener AUDIO_LISTENER =
            new HMIAudioServiceListener() {
        public void updateAMAvailable(boolean available) {
            synchronized (AudioFocusBridge.class) {
                audioManagerAvailabilityKnown = true;
                audioManagerAvailable = available;
            }
            log("audio manager available=" + yn(available));
            if (!available) onFocusLost("audio-manager-unavailable");
        }

        public void startConnection(int connection, int terminal) {
            onFocusGranted("start", connection, terminal);
        }

        public void fadedIn(int connection, int terminal) {
            onFocusGranted("faded-in", connection, terminal);
        }

        public void pauseConnection(int connection, int terminal) {
            onConnectionLost("pause", connection, terminal, 0);
        }

        public void stopConnection(int connection, int terminal) {
            onConnectionLost("stop", connection, terminal, 0);
        }

        public void errorConnection(int connection, int terminal, int error) {
            onConnectionLost("error", connection, terminal, error);
        }

        public void updateVolumeLock(int connection, int terminal,
                                     boolean locked) {
            /* Stock volume/mute handling remains inside the audio bundle. */
        }
    };

    private AudioFocusBridge() {}

    /** Non-blocking; service discovery and routing happen on a daemon thread. */
    public static synchronized void request(ClassLoader hint) {
        classLoaderHint = hint;
        requested = true;
        focusLossSignalled = false;
        if (active || workerRunning) return;
        workerRunning = true;

        Thread worker = new Thread(new Runnable() {
            public void run() {
                try {
                    int attempts = 0;
                    while (requested) {
                        if (activateOnce()) return;
                        attempts++;
                        if ((attempts % 5) == 0)
                            log("waiting for LSD audio services, attempt=" + attempts);
                        try { Thread.sleep(RETRY_DELAY_MS); }
                        catch (InterruptedException e) { return; }
                    }
                } finally {
                    boolean restart;
                    synchronized (AudioFocusBridge.class) {
                        workerRunning = false;
                        restart = requested && !active;
                    }
                    /* Covers release->immediate relaunch while this worker was
                     * leaving its loop with the old requested=false value. */
                    if (restart) request(classLoaderHint);
                }
            }
        });
        worker.setName("retroarch-audio-focus");
        worker.setDaemon(true);
        worker.start();
    }

    /** Idempotent release used by every RaScreen disconnection path. */
    public static synchronized void release() {
        boolean hadSession = active || connectionRequested;
        requested = false;
        active = false; /* callbacks caused by our release must not pause RA */

        if (mediaRouter != null) {
            try { mediaRouter.stopStreaming(CONNECTION_MEDIA_MFP); }
            catch (Throwable t) { log("stopStreaming failed " + t); }
            try { mediaRouter.unregisterClient(CONNECTION_MEDIA_MFP); }
            catch (Throwable t) { log("unregisterClient failed " + t); }
        }
        if (connectionRequested && hmiAudio != null) {
            try {
                hmiAudio.releaseConnection(CONNECTION_MEDIA_MFP,
                        HMI_TERMINAL_FRONT);
            } catch (Throwable t) {
                log("releaseConnection failed " + t);
            }
        }
        connectionRequested = false;

        if (focusManager != null) {
            try { focusManager.setActiveAudioApp(FOCUS_TERMINAL_FRONT, 0); }
            catch (Throwable t) { log("clear audio focus failed " + t); }
        }
        if (sdisAudio != null) {
            try { sdisAudio.setAudioContext(AUDIO_CONTEXT_NONE); }
            catch (Throwable t) { log("clear SDIS context failed " + t); }
        }
        unregisterAudioListener();
        audioManagerAvailabilityKnown = false;
        audioManagerAvailable = false;
        focusLossSignalled = false;
        log(hadSession ? "released connection 20, MPL1 route and HMI focus"
                : "release: no active audio session");
    }

    private static synchronized boolean activateOnce() {
        if (!requested) return false;
        if (active) return true;
        if (!resolveServices()) return false;
        if (!registerAudioListener()) return false;
        if (audioManagerAvailabilityKnown && !audioManagerAvailable) {
            log("audio manager not available yet");
            return false;
        }

        boolean focusOk = callSetFocus(AUDIO_APP_MEDIA);
        if (sdisAudio != null) {
            try { sdisAudio.setAudioContext(AUDIO_CONTEXT_MEDIA); }
            catch (Throwable t) { log("set SDIS context failed " + t); }
        }

        boolean mediaOk = configureMediaRoute();
        boolean connectionOk = false;
        if (mediaOk) {
            try {
                /* HMI terminal 0 maps to audio terminal 1 on MU1316. */
                hmiAudio.requestAndFadeToConnection(CONNECTION_MEDIA_MFP,
                        HMI_TERMINAL_FRONT);
                connectionRequested = true;
                connectionOk = true;
            } catch (Throwable t) {
                log("requestAndFadeToConnection failed " + t);
            }
        }

        if (requested && focusOk && mediaOk && connectionOk) {
            active = true;
            log("ACTIVE stock HMIAudio conn=20 HT=0, focusApp=2, "
                    + "route ENT_INTMEDIA->MPL1");
            return true;
        }

        rollbackActivation();
        log("activation incomplete focus=" + yn(focusOk)
                + " media=" + yn(mediaOk)
                + " connection=" + yn(connectionOk));
        return false;
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
        if (mediaRouter == null) {
            Object service = DsiReflection.getService(
                    "org.dsi.ifc.media.DSIMediaRouter", classLoaderHint);
            if (service instanceof DSIMediaRouter)
                mediaRouter = (DSIMediaRouter)service;
        }
        if (focusManager == null) {
            Object service = DsiReflection.getService(
                    "de.audi.atip.audio.IAudioFocusManager", classLoaderHint);
            if (service instanceof IAudioFocusManager)
                focusManager = (IAudioFocusManager)service;
        }
        if (sdisAudio == null) {
            Object service = DsiReflection.getService(
                    "de.audi.audio.sdis.ifc.SdisAudioService", classLoaderHint);
            if (service instanceof SdisAudioService)
                sdisAudio = (SdisAudioService)service;
        }
        return hmiAudio != null && mediaRouter != null && focusManager != null;
    }

    /**
     * ServiceInfo is the exact registration path used by BundleInfo.  Supplying
     * no owner bundle is supported by its constructor; unregister() still
     * produces the normal LSD removal event.
     */
    private static boolean registerAudioListener() {
        if (listenerRegistration != null) return true;
        try {
            Hashtable properties = new Hashtable();
            properties.put(HMIAudioService.AUDIO_CLIENT_ID,
                    HMIAudioService.CLIENT_MEDIA);
            listenerRegistration = new ServiceInfo(null,
                    HMIAudioServiceListener.class.getName(),
                    AUDIO_LISTENER, properties);
            log("registered stock HMIAudioServiceListener for CLIENT_MEDIA");
            return true;
        } catch (Throwable t) {
            log("listener registration failed " + t);
            return false;
        }
    }

    private static void unregisterAudioListener() {
        if (listenerRegistration == null) return;
        try { listenerRegistration.unregister(); }
        catch (Throwable t) { log("listener unregister failed " + t); }
        listenerRegistration = null;
    }

    private static boolean callSetFocus(int app) {
        try {
            focusManager.setActiveAudioApp(FOCUS_TERMINAL_FRONT, app);
            return true;
        } catch (Throwable t) {
            log("setActiveAudioApp failed " + t);
            return false;
        }
    }

    private static boolean configureMediaRoute() {
        boolean registered = false;
        try {
            mediaRouter.registerClient(CONNECTION_MEDIA_MFP,
                    "RetroArch", "RetroArch");
            registered = true;
            mediaRouter.requestConfiguration(CONNECTION_MEDIA_MFP,
                    AUDIO_SOURCE_EXTERNAL, VIDEO_SOURCE_NONE, SINK_HEADUNIT);
            if (!setAudioRoute()) throw new Exception("setAudioRoutes failed");
            mediaRouter.startStreaming(CONNECTION_MEDIA_MFP);
            return true;
        } catch (Throwable t) {
            log("media route setup failed " + t);
            if (registered) {
                try { mediaRouter.stopStreaming(CONNECTION_MEDIA_MFP); }
                catch (Throwable ignored) {}
                try { mediaRouter.unregisterClient(CONNECTION_MEDIA_MFP); }
                catch (Throwable ignored) {}
            }
            return false;
        }
    }

    private static boolean setAudioRoute() {
        try {
            mediaRouter.setAudioRoutes(new AudioRoute[]{new AudioRoute(
                    VIRTUAL_CHANNEL_INTERNAL_MEDIA,
                    PHYSICAL_CHANNEL_MPL1,
                    ROUTING_RESULT_OK)});
            return true;
        } catch (Throwable t) {
            log("setAudioRoutes failed " + t);
            return false;
        }
    }

    private static void rollbackActivation() {
        active = false;
        if (connectionRequested && hmiAudio != null) {
            try { hmiAudio.releaseConnection(CONNECTION_MEDIA_MFP,
                    HMI_TERMINAL_FRONT); }
            catch (Throwable ignored) {}
        }
        connectionRequested = false;
        if (mediaRouter != null) {
            try { mediaRouter.stopStreaming(CONNECTION_MEDIA_MFP); }
            catch (Throwable ignored) {}
            try { mediaRouter.unregisterClient(CONNECTION_MEDIA_MFP); }
            catch (Throwable ignored) {}
        }
        callSetFocus(0);
        if (sdisAudio != null) {
            try { sdisAudio.setAudioContext(AUDIO_CONTEXT_NONE); }
            catch (Throwable ignored) {}
        }
    }

    private static synchronized void onFocusGranted(String event,
                                                     int connection,
                                                     int terminal) {
        if (connection != CONNECTION_MEDIA_MFP) return;
        focusLossSignalled = false;
        log(event + " conn=" + connection + " terminal=" + terminal);
    }

    private static synchronized void onConnectionLost(String event,
                                                       int connection,
                                                       int terminal,
                                                       int error) {
        if (connection != CONNECTION_MEDIA_MFP) return;
        log(event + " conn=" + connection + " terminal=" + terminal
                + (error == 0 ? "" : " error=" + error));
        onFocusLost(event);
    }

    private static synchronized void onFocusLost(String reason) {
        if (!requested || !active || focusLossSignalled) return;
        focusLossSignalled = true;
        Shell.signalRetroArchAudioFocusLost();
        log("focus lost (" + reason + "); sent SIGRTMIN, no auto-resume");
    }

    private static String yn(boolean value) {
        return value ? "1" : "0";
    }

    static void log(String message) {
        String line = "[RA-AUDIO] " + message;
        try { System.out.println(line); }
        catch (Throwable ignored) {}

        java.io.FileWriter writer = null;
        try {
            writer = new java.io.FileWriter("/tmp/ra_audio.log", true);
            writer.write(message + "\n");
        } catch (Throwable ignored) {
        } finally {
            if (writer != null) {
                try { writer.close(); }
                catch (Throwable ignored) {}
            }
        }
    }
}
