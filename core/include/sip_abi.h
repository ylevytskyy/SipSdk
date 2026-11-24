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
typedef struct sip_account_t sip_account_t;

#define SIP_CLIENT_MAX_ACCOUNTS 8

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

/**
 * Real-time call quality telemetry metrics.
 */
typedef struct sip_call_stats_t {
    char audio_codec[32];
    uint32_t audio_bitrate_kbps;
    float audio_loss_rate;
    uint32_t audio_jitter_ms;
    uint32_t audio_rtt_ms;
    char video_codec[32];
    uint32_t video_bitrate_kbps;
    float video_loss_rate;
    uint32_t video_jitter_ms;
    uint32_t video_fps;
} sip_call_stats_t;

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

    /** Real-time call quality telemetry metrics (dispatched periodically, e.g. every 2 seconds). */
    void (*on_call_stats)(sip_call_t *call, const sip_call_stats_t *stats, void *user_data);

    /** Transfer progression status update (e.g. code 200 for success, 4xx/5xx/6xx for failure). */
    void (*on_transfer_status)(sip_call_t *call, int code, const char *reason, void *user_data);

    /** Incoming transfer request (REFER) from remote peer requiring accept/reject decision. */
    void (*on_transfer_request)(sip_call_t *call, const char *refer_to_uri, bool has_replaces, void *user_data);

    /** Incoming INVITE rejected with 486 Busy Here because max_calls cap was reached. */
    void (*on_call_rejected_busy)(sip_client_t *client, const char *caller_uri, void *user_data);

    /** Local audio conference membership changed (calls array is valid only for the callback duration). */
    void (*on_conference_changed)(sip_client_t *client, sip_call_t *const *calls, size_t count, void *user_data);
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
    const char *transport;       /* "udp", "tcp", "tls", "ws", or "wss"; defaults to "udp" if null */
    int port;                     /* e.g. 5060 (UDP/TCP), 5061 (TLS), 8088/80 (WS), 8089/443 (WSS) */
    const char *display_name;    /* optional display name, e.g. "Alice" */
    const char *outbound_proxy;  /* optional outbound proxy URI, e.g. "sip:proxy.example.com:5060" */
    const char *stun_server;     /* optional STUN/TURN host:port, e.g. "stun.l.google.com:19302" */
    const char *stun_user;       /* optional STUN/TURN username */
    const char *stun_pass;       /* optional STUN/TURN password */
    const char *media_nat;       /* optional media NAT traversal mode: "ice", "stun", "turn", or nullptr */
    uint32_t reg_interval;       /* registration expiration interval in seconds (0 for default) */
    const char *tls_ca_file;     /* optional custom CA PEM certificate file path */
    bool verify_tls_cert;        /* verify TLS certificate against CA, defaults to true */
    const char *media_enc;       /* optional media encryption: "srtp-mand", "dtls_srtp", "none" */
    int max_calls;               /* max concurrent open calls (1..8, 0 defaults to 4) */
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
 * @param transport Protocol transport: "udp", "tcp", "tls", "ws", or "wss" (case-insensitive).
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
 * @return Opaque handle to the created sip_call_t session, or nullptr on failure (sets errno, e.g. EBUSY at max_calls).
 */
SIP_ABI_EXPORT sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);

/**
 * Initiates an outgoing call to the specified target SIP URI with explicit error code.
 *
 * @param client Valid client instance returned from sip_client_create.
 * @param target_uri Destination SIP URI (e.g. "sip:1002@127.0.0.1:5060").
 * @param with_video True to offer video (VIDMODE_ON); false for audio-only (VIDMODE_OFF).
 * @param out_call Output pointer populated with created call handle on success (nullptr on failure).
 * @return 0 on success, EBUSY when max_calls cap is reached, EINVAL on bad args, EIO if not started.
 */
SIP_ABI_EXPORT int sip_call_dial(sip_client_t *client, const char *target_uri, bool with_video, sip_call_t **out_call);

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
 * Initiates a blind call transfer (RFC 3515 REFER) to the specified destination URI.
 *
 * @param call Active call session handle to be transferred.
 * @param target_uri Destination SIP URI (e.g. "sip:1004@127.0.0.1:5060").
 * @return 0 if transfer request was queued, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_transfer(sip_call_t *call, const char *target_uri);

/**
 * Initiates an attended call transfer (RFC 3515 REFER with Replaces header).
 * Transfers `call` (typically on hold) to the peer of `consult_call`.
 *
 * @param call Primary call session to be transferred.
 * @param consult_call Active consultation call session whose peer replaces the primary call.
 * @return 0 if attended transfer request was queued, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_attended_transfer(sip_call_t *call, sip_call_t *consult_call);

/**
 * Accepts or rejects an incoming transfer request (REFER).
 *
 * @param call Active call session that received on_transfer_request.
 * @param accept True to accept and dial the transfer target; false to decline (603 Decline).
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_accept_transfer(sip_call_t *call, bool accept);

/**
 * Sets client-wide microphone mute state.
 */
SIP_ABI_EXPORT int sip_client_set_mic_muted(sip_client_t *client, bool muted);

/**
 * Checks client-wide microphone mute state.
 */
SIP_ABI_EXPORT bool sip_client_is_mic_muted(const sip_client_t *client);

/**
 * Merges two open calls into the client's local audio conference (or adds the non-conference
 * call to an already active conference). Un-holds participating legs and starts mix-minus audio.
 */
SIP_ABI_EXPORT int sip_client_merge_calls(sip_client_t *client, sip_call_t *call_a, sip_call_t *call_b);

/**
 * Adds an open call to an already active local audio conference (returns EINVAL if none active).
 */
SIP_ABI_EXPORT int sip_client_add_to_conference(sip_client_t *client, sip_call_t *call);

/**
 * Removes a call from the local audio conference and places it on hold.
 * If only one leg remains in the conference, dissolves the conference and leaves that leg active.
 */
SIP_ABI_EXPORT int sip_client_split_call(sip_client_t *client, sip_call_t *call);

/**
 * Ends all calls currently in the local audio conference and clears conference state.
 */
SIP_ABI_EXPORT int sip_client_leave_conference(sip_client_t *client);

/**
 * Returns whether the call is currently participating in the local audio conference.
 */
SIP_ABI_EXPORT bool sip_call_is_in_conference(const sip_call_t *call);

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
 * Returns whether the active call media is encrypted (SRTP).
 */
SIP_ABI_EXPORT bool sip_call_is_secure(const sip_call_t *call);

/**
 * Retrieves real-time quality and diagnostic statistics for an active call.
 * Safe to invoke from any thread.
 *
 * @param call Valid call session handle.
 * @param stats Destination pointer for telemetry snapshot.
 * @return 0 on success, or EINVAL if arguments are invalid.
 */
SIP_ABI_EXPORT int sip_call_get_stats(const sip_call_t *call, sip_call_stats_t *stats);

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

/* ============================================================================
 * Messaging (SIP MESSAGE, RFC 3428)
 *
 * Pager-mode messages sent and received outside of calls. Every message carries
 * a MIME content type, so later extensions reuse the same path:
 *   - delivery / read receipts: IMDN bodies ("message/imdn+xml", RFC 5438) report
 *     SIP_MESSAGE_STATUS_DELIVERED / SIP_MESSAGE_STATUS_DISPLAYED for a message id;
 *   - typing indicators: "application/im-iscomposing+xml" bodies (RFC 3994);
 *   - rich content: any "type/subtype" body (images, files, JSON);
 *   - group chat: peer_uri may name a conference or group URI; MSRP sessions
 *     (RFC 4975) would get their own handle type beside sip_call_t.
 * See docs/messaging.md.
 * ============================================================================ */

#define SIP_MESSAGE_CONTENT_TYPE_TEXT "text/plain"

/**
 * Largest message body, in bytes. sip_client_send_message rejects a larger body with
 * EMSGSIZE; an incoming MESSAGE with a larger body is answered 413 Request Entity Too
 * Large and on_message_received is not called.
 */
#define SIP_MESSAGE_MAX_BODY_SIZE 65536

typedef enum {
    SIP_MESSAGE_STATUS_SENT = 0,     /* a 2xx final response came back (server accepted it) */
    SIP_MESSAGE_STATUS_FAILED,       /* non-2xx final response, transport error, or no account */
    SIP_MESSAGE_STATUS_DELIVERED,    /* reserved: IMDN delivery notification from the peer */
    SIP_MESSAGE_STATUS_DISPLAYED     /* reserved: IMDN display (read) notification from the peer */
} sip_message_status_t;

typedef struct sip_message_callbacks_t {
    /**
     * A message arrived for the registered account. from_uri is the sender address
     * (e.g. "sip:1002@pbx"), content_type its MIME type (e.g. "text/plain"). body is not
     * NUL-terminated and is valid only during the call.
     */
    void (*on_message_received)(sip_client_t *client, const char *from_uri, const char *content_type,
                                const uint8_t *body, size_t body_len, void *user_data);

    /**
     * Status update for a message sent with sip_client_send_message. code is the SIP
     * status code (0 when no response was received), reason its phrase or error text.
     */
    void (*on_message_status)(sip_client_t *client, uint64_t message_id, sip_message_status_t status,
                              int code, const char *reason, void *user_data);
} sip_message_callbacks_t;

/**
 * Sets the messaging callbacks. Callbacks run through dispatch_to_main like the other
 * client callbacks and receive the user_data given to sip_client_create.
 * Pass nullptr to remove them. Returns 0, or EINVAL for a null client.
 */
SIP_ABI_EXPORT int sip_client_set_message_callbacks(sip_client_t *client, const sip_message_callbacks_t *callbacks);

/**
 * Sends a SIP MESSAGE from the registered account.
 *
 * @param client Started client instance.
 * @param peer_uri Destination SIP URI (e.g. "sip:1002@127.0.0.1:5060").
 * @param content_type MIME type "type/subtype"; nullptr means "text/plain".
 * @param body Message body bytes (UTF-8 text for text/plain); 1..SIP_MESSAGE_MAX_BODY_SIZE bytes.
 * @param body_len Body length in bytes.
 * @param message_id Optional out-param: local id later reported by on_message_status. It is
 *        written before the call returns and before the message is queued, so it already
 *        holds the id if on_message_status fires first. The status callback may run before
 *        this call returns (an inline dispatch_to_main); do not wait for the return value
 *        to start tracking the id. Set to 0 when the call fails with EIO.
 * @return 0 when the message was queued; EINVAL on invalid arguments (null or empty values,
 *         control characters, a content type without '/'); EMSGSIZE for a body that is too
 *         large; EIO when the engine is not running.
 */
SIP_ABI_EXPORT int sip_client_send_message(sip_client_t *client,
                                           const char *peer_uri,
                                           const char *content_type,
                                           const uint8_t *body,
                                           size_t body_len,
                                           uint64_t *message_id);

/* ============================================================================
 * Local Call Recording (Audio WAV + Video MP4/MKV)
 * ============================================================================ */

typedef enum {
    SIP_RECORDING_AUDIO_STEREO = 0, /* 48 kHz S16LE 2ch WAV: left = local mic, right = remote peer */
    SIP_RECORDING_AUDIO_MONO   = 1, /* 48 kHz S16LE 1ch WAV: local + remote mixed */
    SIP_RECORDING_AUDIO_VIDEO  = 2  /* Audio + remote video (native driver MP4 or portable MKV fallback) */
} sip_recording_mode_t;

typedef enum {
    SIP_RECORDING_STATE_IDLE      = 0,
    SIP_RECORDING_STATE_RECORDING = 1,
    SIP_RECORDING_STATE_FAILED    = 2
} sip_recording_state_t;

typedef struct sip_recording_callbacks_t {
    void (*on_recording_state)(sip_client_t *client,
                               sip_call_t *call,
                               sip_recording_state_t state,
                               const char *path,
                               int error_code,
                               void *user_data);
} sip_recording_callbacks_t;

/**
 * Pluggable platform media recorder driver for SIP_RECORDING_AUDIO_VIDEO
 * (e.g. AVAssetWriter on Apple, MediaCodec+MediaMuxer on Android, IMFSinkWriter on Windows).
 * When no driver is registered, sip_core falls back to its built-in portable MKV writer.
 */
typedef struct sip_media_recorder_driver_t {
    void *user_data;
    int (*start)(void *user_data, const char *path, int sample_rate, int channels,
                 int video_width, int video_height);
    void (*write_audio)(void *user_data, const int16_t *pcm, size_t sample_count, uint64_t timestamp_ns);
    void (*write_video)(void *user_data, const sip_video_frame_t *frame);
    int (*stop)(void *user_data);
} sip_media_recorder_driver_t;

SIP_ABI_EXPORT int sip_client_set_recording_callbacks(sip_client_t *client,
                                                      const sip_recording_callbacks_t *callbacks);

SIP_ABI_EXPORT int sip_client_set_media_recorder_driver(sip_client_t *client,
                                                        const sip_media_recorder_driver_t *driver);

SIP_ABI_EXPORT int sip_call_start_recording(sip_call_t *call,
                                            const char *path,
                                            sip_recording_mode_t mode);

SIP_ABI_EXPORT int sip_call_stop_recording(sip_call_t *call);

SIP_ABI_EXPORT bool sip_call_is_recording(const sip_call_t *call);

SIP_ABI_EXPORT const char *sip_call_get_recording_path(const sip_call_t *call);

/* ============================================================================
 * Multi-Account Support (1..SIP_CLIENT_MAX_ACCOUNTS per sip_client_t)
 * ============================================================================ */

typedef struct sip_account_callbacks_t {
    /** Registration state changed for a specific account. */
    void (*on_account_reg_state)(sip_client_t *client, sip_account_t *account,
                                 sip_reg_state_t state, int code, const char *reason,
                                 void *user_data);

    /** A SIP MESSAGE arrived for a specific account. */
    void (*on_account_message_received)(sip_client_t *client, sip_account_t *account,
                                        const char *from_uri, const char *content_type,
                                        const uint8_t *body, size_t body_len,
                                        void *user_data);
} sip_account_callbacks_t;

/**
 * Sets per-account callbacks without altering sip_callbacks_t or sip_message_callbacks_t.
 * Pass nullptr to clear them.
 */
SIP_ABI_EXPORT int sip_client_set_account_callbacks(sip_client_t *client,
                                                    const sip_account_callbacks_t *callbacks);

/**
 * Adds and registers a new SIP account on the client (up to SIP_CLIENT_MAX_ACCOUNTS).
 * The first account added becomes the default account.
 * Returns 0 on success, EINVAL on invalid args, EEXIST for duplicate active AOR,
 * EBUSY when SIP_CLIENT_MAX_ACCOUNTS is reached, or EIO if not started.
 */
SIP_ABI_EXPORT int sip_client_add_account(sip_client_t *client,
                                          const sip_account_config_t *config,
                                          sip_account_t **out_account);

/**
 * Unregisters and removes an account from the client, hanging up any active calls on it.
 * Caller still releases the handle via sip_account_destroy(account).
 */
SIP_ABI_EXPORT int sip_client_remove_account(sip_client_t *client, sip_account_t *account);

/**
 * Selects the default account used by single-account APIs (sip_call_dial, sip_client_send_message, etc.).
 */
SIP_ABI_EXPORT int sip_client_set_default_account(sip_client_t *client, sip_account_t *account);

/**
 * Returns the current default account handle, or nullptr if none is active.
 */
SIP_ABI_EXPORT sip_account_t *sip_client_get_default_account(const sip_client_t *client);

/**
 * Returns the number of active accounts registered on this client.
 */
SIP_ABI_EXPORT size_t sip_client_get_account_count(const sip_client_t *client);

/**
 * Copies up to max_count active account handles into out_accounts.
 * Returns the number of handles written (or total count if out_accounts is nullptr).
 */
SIP_ABI_EXPORT size_t sip_client_get_accounts(const sip_client_t *client,
                                              sip_account_t **out_accounts,
                                              size_t max_count);

/**
 * Initiates or refreshes registration for a specific account.
 */
SIP_ABI_EXPORT int sip_account_reregister(sip_account_t *account);

/**
 * Unregisters a specific account without removing it from the client.
 */
SIP_ABI_EXPORT int sip_account_unregister(sip_account_t *account);

/**
 * Initiates an outgoing call using a specific account.
 */
SIP_ABI_EXPORT int sip_account_dial(sip_account_t *account,
                                    const char *target_uri,
                                    bool with_video,
                                    sip_call_t **out_call);

/**
 * Sends a SIP MESSAGE from a specific account.
 */
SIP_ABI_EXPORT int sip_account_send_message(sip_account_t *account,
                                            const char *peer_uri,
                                            const char *content_type,
                                            const uint8_t *body,
                                            size_t body_len,
                                            uint64_t *message_id);

/**
 * Returns the account handle that owns this call (both outgoing and incoming calls), or nullptr.
 */
SIP_ABI_EXPORT sip_account_t *sip_call_get_account(const sip_call_t *call);

/**
 * Account metadata and state accessors.
 */
SIP_ABI_EXPORT uint64_t sip_account_get_id(const sip_account_t *account);
SIP_ABI_EXPORT const char *sip_account_get_aor(const sip_account_t *account);
SIP_ABI_EXPORT const char *sip_account_get_user(const sip_account_t *account);
SIP_ABI_EXPORT const char *sip_account_get_domain(const sip_account_t *account);
SIP_ABI_EXPORT bool sip_account_is_registered(const sip_account_t *account);
SIP_ABI_EXPORT sip_reg_state_t sip_account_get_reg_state(const sip_account_t *account);
SIP_ABI_EXPORT void *sip_account_get_user_data(const sip_account_t *account);
SIP_ABI_EXPORT void sip_account_set_user_data(sip_account_t *account, void *user_data);

/**
 * Releases the caller's reference to an account handle. Safe to pass nullptr.
 */
SIP_ABI_EXPORT void sip_account_destroy(sip_account_t *account);

#ifdef __cplusplus
}
#endif

#endif /* SIP_ABI_H */
