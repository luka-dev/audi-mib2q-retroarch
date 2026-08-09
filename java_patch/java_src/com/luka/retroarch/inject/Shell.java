package com.luka.retroarch.inject;

/**
 * Fire-and-forget shell launcher on the HMI's OSGi/Equinox side.
 * The only thing the HMI does at runtime is spawn / signal the native process;
 * everything else (video, input, audio, lifecycle) is native. No IPC socket.
 */
public final class Shell {
    private static final String RETROARCH_LOCK = "/tmp/retroarch.lock";
    private static final String AUDIO_DESIRED =
            "/tmp/retroarch.audio.desired";

    private Shell() {
    }

    public static void shAsync(String cmd) {
        try {
            /* Runtime.exec() returns as soon as /bin/sh has been spawned.  Do
             * not use the OEM CommandLineExecuter here: it drains stdout until
             * EOF and therefore turns a background command which inherits that
             * pipe into a synchronous HMIEvent-thread wait. */
            Runtime.getRuntime().exec(new String[] { "/bin/sh", "-c", cmd });
        } catch (Throwable t) {
            // never let a shell failure escape into the HMI
        }
    }

    /** QNX SIGRTMIN (41): pause the core and stop QSA after focus loss. */
    public static synchronized void signalRetroArchAudioFocusLost() {
        writeAudioDesired(false);
        signalRetroArch(41);
    }

    /** QNX SIGRTMIN+1 (42): restart/pre-fill QSA; gameplay stays paused. */
    public static synchronized void signalRetroArchAudioFocusGained() {
        writeAudioDesired(true);
        signalRetroArch(42);
    }

    public static void terminateRetroArch() {
        signalRetroArch(15);
    }

    private static void signalRetroArch(int signal) {
        shAsync("pid=`cat " + RETROARCH_LOCK + " 2>/dev/null`; "
                + "case \"$pid\" in ''|*[!0-9]*) ;; "
                + "*) kill -" + signal + " \"$pid\" 2>/dev/null ;; esac");
    }

    /**
     * SIG41/SIG42 are different realtime signals and QNX may dequeue them by
     * signal priority rather than Java call order. The file carries the latest
     * desired state; either signal merely wakes the native lifecycle thread.
     */
    private static void writeAudioDesired(boolean enabled) {
        java.io.FileWriter writer = null;
        try {
            writer = new java.io.FileWriter(AUDIO_DESIRED, false);
            writer.write(enabled ? "1\n" : "0\n");
        } catch (Throwable ignored) {
            /* Native retains a signal-number fallback for read-only /tmp. */
        } finally {
            if (writer != null) {
                try { writer.close(); }
                catch (Throwable ignored) {}
            }
        }
    }
}
