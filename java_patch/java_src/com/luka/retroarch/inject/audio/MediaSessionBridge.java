package com.luka.retroarch.inject.audio;

import de.audi.app.media.IMediaTerminal;
import de.audi.app.media.audio.IAudioManager;
import de.audi.app.media.extension.IContentProvider;
import de.audi.app.media.extension.IMediaTerminalExtension;
import de.audi.atip.interapp.combi.bap.audio.CombiBAPServiceMedia;
import de.audi.atip.interapp.combi.bap.audio.data.CombiBAPCurrentStationInfo;
import de.esolutions.hmi.widgets.audi.base.AbstractWidget;
import java.util.Hashtable;
import org.osgi.framework.BundleContext;
import org.osgi.framework.ServiceRegistration;

/**
 * Joins RetroArch to the already-running stock Media application without
 * shadowing any Media class.  MediaTerminalExtensionTracker supplies the live
 * terminal, allowing us to change Media's own active AudioContext instead of
 * racing its connection-9 (entertainment suppression) restore with a direct
 * HMIAudio connection-20 request.
 */
public final class MediaSessionBridge implements IMediaTerminalExtension {
    private static final int TERMINAL_FRONT = 0;
    private static final int CONNECTION_MEDIA_MFP = 20;
    private static final int CONNECTION_ENTERTAINMENT_SUPPRESSION = 9;
    private static final int INFO_STATE_NO_ERROR = 0;
    private static final int INFO_STATE_NO_PLAYABLE_FILES = 34;
    private static final int INFO_TYPE_TITLE = 72;
    private static final int INFO_TYPE_ARTIST = 73;
    private static final int INFO_TYPE_ALBUM = 74;

    private static MediaSessionBridge instance;
    private static ServiceRegistration registration;
    private static IMediaTerminal terminal;
    private static ClassLoader classLoaderHint;

    private MediaSessionBridge() {
    }

    /** Register once; an already-open Media tracker calls initExtension(). */
    public static synchronized boolean ensureRegistered(ClassLoader hint) {
        if (hint != null) classLoaderHint = hint;
        if (registration != null) return true;
        try {
            BundleContext context = AbstractWidget.framework.getBundleCxt();
            if (context == null) {
                AudioFocusBridge.log("Media session extension: BundleContext unavailable");
                return false;
            }
            instance = new MediaSessionBridge();
            Hashtable properties = new Hashtable(1);
            properties.put(IMediaTerminalExtension.SERVICE_PROPERTY_TERMINALID,
                    new Integer(TERMINAL_FRONT));
            registration = context.registerService(
                    IMediaTerminalExtension.class.getName(), instance,
                    properties);
            AudioFocusBridge.log("registered stock Media terminal extension");
            return registration != null;
        } catch (Throwable t) {
            AudioFocusBridge.log("Media terminal extension registration failed " + t);
            registration = null;
            instance = null;
            return false;
        }
    }

    public void initExtension(IMediaTerminal mediaTerminal) {
        if (mediaTerminal == null
                || mediaTerminal.getTerminalID() != TERMINAL_FRONT) return;
        synchronized (MediaSessionBridge.class) {
            terminal = mediaTerminal;
        }
        AudioFocusBridge.log("stock Media terminal 0 attached");
    }

    public void deinitExtension() {
        synchronized (MediaSessionBridge.class) {
            terminal = null;
        }
        AudioFocusBridge.log("stock Media terminal 0 detached");
    }

    /** This extension contributes ownership only, never an OEM content type. */
    public IContentProvider getContentProvider() {
        return null;
    }

    public static synchronized boolean isReady() {
        return terminal != null && terminal.getAudioManager() != null;
    }

    /**
     * Set connection 20 as Media's own active context before focus=2.  With no
     * Media focus requestAudio() only records the context; the stock focus
     * callback then requests it.  If focus is already Media it is idempotent.
     */
    public static boolean preparePlayingAudio() {
        IAudioManager manager = getAudioManager();
        if (manager == null) return false;
        try {
            manager.requestAudio(CONNECTION_MEDIA_MFP, true, false);
            AudioFocusBridge.log(
                    "stock Media active audio context=20 (RetroArch playing)");
            return true;
        } catch (Throwable t) {
            AudioFocusBridge.log("stock Media requestAudio(20) failed " + t);
            return false;
        }
    }

    /** Restore Media's own context so a later focus callback cannot revive RA. */
    public static boolean restoreAudioContext(int connection) {
        IAudioManager manager = getAudioManager();
        if (manager == null || connection <= 0) return false;
        try {
            if (connection == CONNECTION_ENTERTAINMENT_SUPPRESSION)
                manager.requestEntSuppression();
            else
                manager.requestAudio(connection, true, false);
            AudioFocusBridge.log("restored stock Media audio context="
                    + connection);
            return true;
        } catch (Throwable t) {
            AudioFocusBridge.log("restore stock Media context failed " + t);
            return false;
        }
    }

    /** Publish a playable Now Playing entry to the Media-focused BAP connector. */
    public static void publishPlaying() {
        CombiBAPServiceMedia service = getCombiService();
        if (service == null) {
            AudioFocusBridge.log("CombiBAPServiceMedia unavailable");
            return;
        }
        try {
            CombiBAPCurrentStationInfo info =
                    new CombiBAPCurrentStationInfo();
            info.setPrimaryInformation("RetroArch", INFO_TYPE_TITLE, 0);
            info.setSecondaryInformation("Playing", INFO_TYPE_ARTIST);
            info.setTertiaryInformation("Game", INFO_TYPE_ALBUM);
            service.updateCurrentStation(info);
            service.updateActiveInfoState(INFO_STATE_NO_ERROR);
            AudioFocusBridge.log(
                    "published Media BAP: RetroArch / Playing / NO_ERROR");
        } catch (Throwable t) {
            AudioFocusBridge.log("publish Media BAP failed " + t);
        }
    }

    /** Restore the known stock no-media state when that was the old context. */
    public static void restoreNoPlayableIfSuppressed(int oldConnection) {
        if (oldConnection != CONNECTION_ENTERTAINMENT_SUPPRESSION) return;
        CombiBAPServiceMedia service = getCombiService();
        if (service == null) return;
        try {
            service.updateCurrentStation(new CombiBAPCurrentStationInfo());
            service.updateActiveInfoState(INFO_STATE_NO_PLAYABLE_FILES);
            AudioFocusBridge.log("restored Media BAP NO_PLAYABLE_FILES");
        } catch (Throwable t) {
            AudioFocusBridge.log("restore Media BAP failed " + t);
        }
    }

    private static synchronized IAudioManager getAudioManager() {
        return terminal == null ? null : terminal.getAudioManager();
    }

    private static CombiBAPServiceMedia getCombiService() {
        Object service = DsiReflection.getService(
                "de.audi.atip.interapp.combi.bap.audio.CombiBAPServiceMedia",
                classLoaderHint);
        return service instanceof CombiBAPServiceMedia
                ? (CombiBAPServiceMedia)service : null;
    }
}
