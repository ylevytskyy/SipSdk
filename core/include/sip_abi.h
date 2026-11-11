#ifndef SIP_ABI_H
#define SIP_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef SIP_CORE_BUILDING_DLL
    #define SIP_ABI_EXPORT __declspec(dllexport)
  #else
    #define SIP_ABI_EXPORT __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define SIP_ABI_EXPORT __attribute__((visibility("default")))
  #else
    #define SIP_ABI_EXPORT
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sip_client_t sip_client_t;
typedef struct sip_call_t sip_call_t;

typedef enum {
    SIP_REG_STATE_UNREGISTERED = 0,
    SIP_REG_STATE_REGISTERING,
    SIP_REG_STATE_REGISTERED,
    SIP_REG_STATE_FAILED
} sip_reg_state_t;

typedef enum {
    SIP_CALL_STATE_INCOMING = 0,
    SIP_CALL_STATE_RINGING,
    SIP_CALL_STATE_CONNECTING,
    SIP_CALL_STATE_ESTABLISHED,
    SIP_CALL_STATE_HOLD,
    SIP_CALL_STATE_CLOSED
} sip_call_state_t;

typedef struct sip_callbacks_t {
    /** Optional main-thread dispatcher hook for non-Apple platforms or custom event loops. */
    void (*dispatch_to_main)(void (*task)(void *arg), void *arg, void *user_data);

    void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);
    void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);
    void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);
    void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);
    void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);

    /**
     * Optional hook to prepare audio hardware session (e.g. AVAudioSession / AudioManager).
     * Called once when the first call needs audio, not once per call; matched by one
     * release_audio_session. Runs under an engine lock: must not call back into the SDK.
     */
    bool (*prepare_audio_session)(sip_client_t *client, void *user_data);

    /**
     * Optional hook to release audio hardware session when all calls finish.
     * Runs under an engine lock: must not call back into the SDK.
     */
    void (*release_audio_session)(sip_client_t *client, void *user_data);
} sip_callbacks_t;

/**
 * Creates an uninitialized SIP client instance with the given callbacks.
 * user_data is stored and passed back to all callback invocations.
 */
SIP_ABI_EXPORT sip_client_t *sip_client_create(const sip_callbacks_t *callbacks, void *user_data);

/**
 * Initializes the SIP core stack and spawns the background worker event loop.
 * config_path: Path to directory containing Baresip configuration (or nullptr for default).
 * Returns 0 on success, or a positive error code (errno/libre error) on failure.
 */
SIP_ABI_EXPORT int sip_client_init(sip_client_t *client, const char *config_path);

/**
 * Extended account configuration parameters.
 */
typedef struct sip_account_config_t {
    const char *user;
    const char *domain;
    const char *pwd;
    const char *transport;       /* "udp" or "tcp", defaults to "udp" if null */
    int port;                     /* e.g. 5060, defaults to 5060 if <= 0 */
    const char *display_name;    /* optional display name, e.g. "Alice" */
    const char *outbound_proxy;  /* optional outbound proxy URI, e.g. "sip:proxy.example.com:5060" */
    const char *stun_server;     /* optional STUN/TURN host:port, e.g. "stun.l.google.com:19302" */
    const char *stun_user;       /* optional STUN/TURN username */
    const char *stun_pass;       /* optional STUN/TURN password */
    const char *media_nat;       /* optional media NAT traversal mode: "ice", "stun", "turn", or nullptr */
    uint32_t reg_interval;       /* registration expiration interval in seconds (0 for default) */
} sip_account_config_t;

/**
 * Registers a SIP account with extended configuration (display name, outbound proxy, STUN/TURN/ICE, reg interval).
 * Dispatches asynchronously onto the core worker event loop.
 */
SIP_ABI_EXPORT int sip_client_register_account(sip_client_t *client, const sip_account_config_t *config);

/**
 * Registers a SIP account with the specified credentials and transport.
 * Dispatches asynchronously onto the core worker event loop.
 *
 * @param client Valid client instance returned from sip_client_create.
 * @param user SIP username / extension (e.g. "1001").
 * @param domain SIP domain or registrar host (e.g. "127.0.0.1").
 * @param pwd Account password for MD5 digest authentication.
 * @param transport Protocol transport: "udp" or "tcp" (case-insensitive).
 * @param port SIP port number (e.g. 5060).
 * @return 0 if registration command was queued, or EINVAL on invalid arguments.
 */
SIP_ABI_EXPORT int sip_client_register(sip_client_t *client,
                                      const char *user,
                                      const char *domain,
                                      const char *pwd,
                                      const char *transport,
                                      int port);

/**
 * Resets network transports for network handover (e.g. Wi-Fi <-> Cellular).
 * Closes stale transport sockets and initiates re-registration if re_register is true.
 */
SIP_ABI_EXPORT int sip_client_reset_transports(sip_client_t *client, bool re_register);

/**
 * Unregisters the currently registered account (Expires: 0).
 * Dispatches asynchronously onto the core worker event loop.
 */
SIP_ABI_EXPORT int sip_client_unregister(sip_client_t *client);

/**
 * Checks whether the client currently has a registered SIP user agent.
 * Safe to call from any thread: it reads the state from the last registration event.
 */
SIP_ABI_EXPORT bool sip_client_is_registered(const sip_client_t *client);

/**
 * Shuts down the background worker event loop, releases all VoIP and networking resources,
 * and frees the sip_client_t instance. Safe to pass nullptr.
 */
SIP_ABI_EXPORT void sip_client_destroy(sip_client_t *client);

/**
 * Initiates an outgoing call to the specified target SIP URI.
 *
 * @param client Valid client instance returned from sip_client_create.
 * @param target_uri Destination SIP URI (e.g. "sip:1002@127.0.0.1:5060").
 * @param with_video True to offer video (VIDMODE_ON); false for audio-only (VIDMODE_OFF).
 * @return Opaque handle to the created sip_call_t session, or nullptr on failure.
 */
SIP_ABI_EXPORT sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);

/**
 * Answers an incoming call.
 *
 * @param call Valid call session handle.
 * @param with_video True to answer with video; false for audio-only.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_answer(sip_call_t *call, bool with_video);

/**
 * Hangs up, cancels, or rejects an active or incoming call.
 *
 * @param call Valid call session handle.
 * @param code SIP status code to send (e.g. 0 for default BYE/CANCEL, 486 for Busy Here, 603 for Decline).
 * @param reason Reason phrase (or nullptr for default).
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_hangup(sip_call_t *call, int code, const char *reason);

/**
 * Places the call on hold or resumes it.
 *
 * @param call Valid call session handle.
 * @param hold True to hold; false to resume.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_set_hold(sip_call_t *call, bool hold);

/**
 * Mutes or unmutes the local microphone for this call.
 *
 * @param call Valid call session handle.
 * @param muted True to mute; false to unmute.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_set_muted(sip_call_t *call, bool muted);

/**
 * Sends DTMF digits across the active call via RFC 4733.
 *
 * @param call Valid call session handle.
 * @param digits String of DTMF digits (e.g. "123#*").
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_send_dtmf(sip_call_t *call, const char *digits);

/**
 * Sets client-wide microphone mute state.
 */
SIP_ABI_EXPORT int sip_client_set_mic_muted(sip_client_t *client, bool muted);

/**
 * Checks client-wide microphone mute state.
 */
SIP_ABI_EXPORT bool sip_client_is_mic_muted(const sip_client_t *client);

/**
 * Returns the unique SIP Call-ID string. Pointer valid until call session destroyed.
 */
SIP_ABI_EXPORT const char *sip_call_get_id(const sip_call_t *call);

/**
 * Returns the remote peer SIP URI string. Pointer valid until call session destroyed.
 */
SIP_ABI_EXPORT const char *sip_call_get_remote_uri(const sip_call_t *call);

/**
 * Returns the remote peer display name string (if present in From/To header).
 * Pointer valid until call session destroyed.
 */
SIP_ABI_EXPORT const char *sip_call_get_peer_display_name(const sip_call_t *call);

/**
 * Returns whether the call has negotiated video capability.
 */
SIP_ABI_EXPORT bool sip_call_has_video(const sip_call_t *call);

/**
 * Returns whether this call was initiated locally (outgoing).
 */
SIP_ABI_EXPORT bool sip_call_is_outgoing(const sip_call_t *call);

/**
 * Returns whether the call is currently on hold.
 */
SIP_ABI_EXPORT bool sip_call_is_on_hold(const sip_call_t *call);

/**
 * Returns whether the call microphone is currently muted.
 */
SIP_ABI_EXPORT bool sip_call_is_muted(const sip_call_t *call);

/**
 * Returns whether the call has reached the CLOSED state.
 */
SIP_ABI_EXPORT bool sip_call_is_closed(const sip_call_t *call);

/**
 * Returns the active call duration in seconds.
 */
SIP_ABI_EXPORT uint32_t sip_call_get_duration(const sip_call_t *call);

/**
 * Gets user data associated with this call handle.
 */
SIP_ABI_EXPORT void *sip_call_get_user_data(const sip_call_t *call);

/**
 * Sets user data associated with this call handle.
 */
SIP_ABI_EXPORT void sip_call_set_user_data(sip_call_t *call, void *user_data);

/**
 * Releases memory for a closed call session handle.
 */
SIP_ABI_EXPORT void sip_call_destroy(sip_call_t *call);

/* ============================================================================
 * Media Driver Abstractions & Extensibility
 * ============================================================================ */

typedef struct sip_video_frame_t {
    int width;
    int height;
    const uint8_t *y;
    const uint8_t *u;
    const uint8_t *v;
    int y_stride;
    int u_stride;
    int v_stride;
    uint64_t timestamp_ns;
} sip_video_frame_t;

typedef struct sip_video_capture_driver_t {
    void *user_data;
    int (*start)(void *user_data, int width, int height, int fps);
    void (*stop)(void *user_data);
    void (*set_frame_handler)(void *user_data, void (*handler)(const sip_video_frame_t *frame, void *arg), void *arg);
    void (*switch_camera)(void *user_data);
    void (*set_camera)(void *user_data, int position);
    void (*set_enabled)(void *user_data, bool enabled);
    bool (*is_enabled)(void *user_data);
} sip_video_capture_driver_t;

typedef struct sip_video_renderer_driver_t {
    void *user_data;
    void (*render_frame)(void *user_data, const sip_video_frame_t *frame);
    void (*on_stream_resized)(void *user_data, int width, int height);
    void (*clear)(void *user_data);
} sip_video_renderer_driver_t;

typedef struct sip_video_codec_driver_t {
    void *user_data;
    int (*register_codec)(void *user_data);
    void (*unregister_codec)(void *user_data);
} sip_video_codec_driver_t;

/**
 * Configures the active audio driver and devices (e.g. "pulse", "alsa", "audiounit").
 * A null argument keeps the current value; an empty module name keeps the current module.
 * While the client runs, the change is applied on the SIP thread, after work already queued there.
 * Do not call it while sip_client_init or sip_client_destroy runs on another thread.
 */
SIP_ABI_EXPORT int sip_client_set_audio_driver(sip_client_t *client, const char *src_mod, const char *src_dev, const char *play_mod, const char *play_dev);

/**
 * Retrieves the audio driver names: the ones configured at init, or the last ones set
 * with sip_client_set_audio_driver, even if the SIP thread has not applied them yet.
 */
SIP_ABI_EXPORT int sip_client_get_audio_driver(const sip_client_t *client, char *src_mod, size_t src_mod_sz, char *play_mod, size_t play_mod_sz);

/**
 * Registers video capture driver for camera hardware integration.
 */
SIP_ABI_EXPORT int sip_client_set_video_capture_driver(sip_client_t *client, const sip_video_capture_driver_t *driver);

/**
 * Registers remote video renderer driver for decoded incoming video.
 */
SIP_ABI_EXPORT int sip_client_set_remote_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer);

/**
 * Registers local video renderer driver for camera preview.
 */
SIP_ABI_EXPORT int sip_client_set_local_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer);

/**
 * Registers hardware video codec driver (e.g. VideoToolbox H.264 or MediaCodec).
 */
SIP_ABI_EXPORT int sip_client_set_video_codec_driver(sip_client_t *client, const sip_video_codec_driver_t *driver);

/**
 * Requests an immediate RTCP PLI/FIR keyframe from the remote peer.
 */
SIP_ABI_EXPORT int sip_call_request_keyframe(sip_call_t *call);

/**
 * Camera and local preview controls.
 */
SIP_ABI_EXPORT int sip_client_switch_camera(sip_client_t *client);
SIP_ABI_EXPORT int sip_client_set_camera(sip_client_t *client, int position);
SIP_ABI_EXPORT int sip_client_set_local_video_enabled(sip_client_t *client, bool enabled);
SIP_ABI_EXPORT bool sip_client_is_local_video_enabled(const sip_client_t *client);
SIP_ABI_EXPORT int sip_client_start_local_preview(sip_client_t *client);
SIP_ABI_EXPORT int sip_client_stop_local_preview(sip_client_t *client);
SIP_ABI_EXPORT bool sip_client_is_local_preview_active(const sip_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* SIP_ABI_H */
