/* dsi_audio_client.h — audio focus on the MHI2Q head unit via the firmware's
 * native DSI service dsi.audio.DSIAudioManagement.
 *
 * Writing PCM to /dev/snd/mpl1_int_ent is only half of playing audio on this
 * unit: the entertainment source selector, the volume knob and the ducking
 * presets are all driven by the audio manager, and it only knows about streams
 * that asked it for a connection. We therefore request a connection instead of
 * grabbing the MS_ENT mixer switch behind the HMI's back (see README).
 *
 * Everything here is reconstructed from the firmware and decompiled factory
 * implementation; this header records the relevant wire ids and contracts.
 *
 * All calls are safe to make on a unit where DSI is unavailable: init() fails
 * cleanly and every other entry point becomes a no-op returning an error, so
 * the audio driver can fall back to opening the PCM device without focus for
 * bring-up diagnostics.
 */

#ifndef DSI_AUDIO_CLIENT_H
#define DSI_AUDIO_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Audio CONNECTION ids -- org.dsi.ifc.audio.Constants (lsd.jxe), the namespace
 * requestConnection() actually speaks.
 *
 * ⚠ These are NOT the mplN virtual channels. The PCM device is chosen by name
 * (/dev/snd/mpl1_int_ent); the numbers below are a different namespace, and
 * confusing the two is dangerous rather than merely wrong: audio connection 1
 * is CL_FCT_AMP_RELEASE_ALL, so requesting "channel 1" tells the amplifier to
 * drop everything. An emulator is the internal media player, hence MFP. */
#define DSI_CONN_AMP_RELEASE_ALL      1   /* CL_FCT_AMP_RELEASE_ALL  -- never ask for this */
#define DSI_CONN_ENT_MEDIA_MFP       20   /* CL_ENT_AMP_MEDIA_MFP    -- internal media, ours */
#define DSI_CONN_ENT_MEDIA_BTDEVICE  21   /* CL_ENT_AMP_MEDIA_BTDEVICE */
#define DSI_CONN_ENT_GAL_MEDIA      156   /* CL_ENT_AMP_GAL_MEDIA (Android Auto) */
#define DSI_CONN_ENT_DIO_MEDIA      162   /* CL_ENT_AMP_DIO_MEDIA (CarPlay)      */

/* Terminals -- same Constants class. */
#define DSI_TERMINAL_DEFAULT          0   /* TERMINAL_SINGLE_USER_DEFAULT_TERMINAL */
#define DSI_TERMINAL_FRONT_DRIVER     1
#define DSI_TERMINAL_FRONT_PASSENGER  2

/* Reply wire ids. The connection state is carried by WHICH method the manager
 * calls, not by a value inside it. Authoritative source: the framework's own
 * name/id table, eso/ems_tables/dsi.audio.ems_table.json, confirmed against the
 * ids extracted from the shipped reply senders. */
#define DSI_REPLY_ERROR_CONNECTION          4    /* errorConnection(i,i,i)    */
#define DSI_REPLY_FADED_IN                  6    /* fadedIn(i,i)              */
#define DSI_REPLY_PAUSE_CONNECTION         10    /* pauseConnection(i,i)      */
#define DSI_REPLY_RESPONSE_VOLUMELOCK      13    /* responseVolumelock(i,i,b) */
#define DSI_REPLY_START_CONNECTION         18    /* startConnection(i,i)      */
#define DSI_REPLY_STOP_CONNECTION          19    /* stopConnection(i,i)       */
#define DSI_REPLY_UPDATE_AM_AVAILABLE      20    /* updateAMAvailable(i,i,i)  */
#define DSI_REPLY_UPDATE_ACTIVE_CONN       21    /* updateActiveConnection    */
#define DSI_REPLY_UPDATE_ACTIVE_ENT_CONN   22    /* ...EntertainmentConnection*/

/* Every connection-scoped reply starts with the CONNECTION it concerns:
 *   startConnection(connection, terminal)   pauseConnection(connection, terminal)
 *   stopConnection (connection, terminal)   errorConnection(connection, terminal, errorCode)
 *   updateActiveEntertainmentConnection(connection, terminal, validFlag)
 * and requestConnection is (connection, terminal, group). The manager talks to
 * every registered client, so a reply must be matched against OUR connection or
 * we would pause the emulator when CarPlay's connection gets paused. */

/* Called from a framework thread when the audio manager replies. Keep it short
 * and async-signal-safe-ish: set a flag, do the work on the audio thread. */
typedef void (*dsi_audio_reply_cb)(int reply_code, int a, int b, void *user);

/* Bring up the comm agent, register the reply service, bind the proxy and wait
 * for it to go alive. agent_name must match an entry in /config/framework.json
 * (RetroArch ships as "RetroArch", id 520). Returns 0 on success. */
int  dsi_audio_init(const char *agent_name, dsi_audio_reply_cb cb, void *user);

/* Ask the audio manager for a connection on a virtual channel (wire id 12).
 * The answer arrives asynchronously via the callback as DSI_RP_*. */
int  dsi_audio_request_connection(int connection, int terminal, int group);

/* Give it back (wire id 11). */
int  dsi_audio_release_connection(int a, int b);

/* Read-only query (wire id 8) — handy as a liveness check. */
int  dsi_audio_get_active_entertainment(int arg);

/* True once the proxy has bound to a provider. */
int  dsi_audio_is_alive(void);

/* True while we hold the entertainment connection. PCM must not be written
 * when this is false: the manager has already routed the amp elsewhere. */
int  dsi_audio_has_focus(void);

/* Consumes the "focus was just taken from us" edge, returning 1 once per loss.
 * The caller pauses the emulator on that edge and does NOT resume by itself --
 * the user decides when to carry on, and audio returns on its own once whoever
 * took the focus gives it back. */
int  dsi_audio_take_focus_lost_event(void);

/* Call once per frame from the main thread. Re-acquires the connection, with
 * backoff, after somebody else took it. */
void dsi_audio_poll(void);

void dsi_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* DSI_AUDIO_CLIENT_H */
