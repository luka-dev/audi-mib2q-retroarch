package de.luka.ra.inject;

/**
 * Fire-and-forget shell launcher on the HMI's OSGi/Equinox side.
 * The only thing the HMI does at runtime is spawn / signal the native process;
 * everything else (video, input, audio, lifecycle) is native. No IPC socket.
 */
public final class Shell {
    private static final String RETROARCH_LOCK = "/tmp/retroarch.lock";

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

    /** QNX SIGRTMIN (41): pause once when the stock audio manager takes focus. */
    public static void signalRetroArchAudioFocusLost() {
        shAsync("pid=`cat " + RETROARCH_LOCK + " 2>/dev/null`; "
                + "case \"$pid\" in ''|*[!0-9]*) ;; "
                + "*) kill -41 \"$pid\" 2>/dev/null ;; esac");
    }
}
