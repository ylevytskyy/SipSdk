#ifndef SIP_CORE_ENGINE_H
#define SIP_CORE_ENGINE_H

#include "sip_abi.h"
#include "media_driver.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mqueue;
struct ua;
struct bevent;
struct call;
struct sip_lsnr;
struct sip_msg;
struct tls;

namespace sip_core {

class MediaDriverManager;

struct AccountConfig {
    std::string user;
    std::string domain;
    std::string pwd;
    std::string transport{"udp"};
    int port{5060};
    std::string display_name;
    std::string outbound_proxy;
    std::string stun_server;
    std::string stun_user;
    std::string stun_pass;
    std::string media_nat;
    uint32_t reg_interval{0};
    std::string tls_ca_file;
    bool verify_tls_cert{true};
    std::string media_enc;
    int max_calls{4};
};

class Engine {
public:
    Engine(sip_client_t *owner, const sip_callbacks_t *callbacks, void *user_data);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    int init(const char *config_path);
    void destroy();

    int register_account(const AccountConfig &config);
    int register_account(const char *user, const char *domain, const char *pwd, const char *transport, int port);
    int unregister_account();
    bool is_registered() const;
    int reset_transports(bool re_register);

    sip_call_t *make_call(const char *target_uri, bool with_video);
    int dial_call(const char *target_uri, bool with_video, sip_call_t **out_call);
    int answer_call(sip_call_t *call, bool with_video);
    int hangup_call(sip_call_t *call, int code, const char *reason);
    int hold_call(sip_call_t *call, bool hold);
    int mute_call(sip_call_t *call, bool muted);
    int send_dtmf(sip_call_t *call, const char *digits);
    int set_client_mic_muted(bool muted);
    bool is_client_mic_muted() const;
    int set_audio_driver(const char *src_mod, const char *src_dev, const char *play_mod, const char *play_dev);
    int get_audio_driver(char *src_mod, size_t src_mod_sz, char *play_mod, size_t play_mod_sz) const;
    void destroy_call(sip_call_t *call);
    int request_keyframe(sip_call_t *call);
    int transfer_call(sip_call_t *call, const char *target_uri);
    int attended_transfer_call(sip_call_t *call, sip_call_t *consult_call);
    int accept_transfer(sip_call_t *call, bool accept);

    int switch_camera();
    int set_camera(int position);
    int set_local_video_enabled(bool enabled);
    bool is_local_video_enabled() const;
    int start_local_preview();
    int stop_local_preview();
    bool is_local_preview_active() const;

    int set_message_callbacks(const sip_message_callbacks_t *callbacks);
    int send_message(const char *peer_uri, const char *content_type, const uint8_t *body, size_t body_len,
                     uint64_t *message_id);
    void fail_pending_messages(const char *reason);
    void dispatch_message_status(uint64_t message_id, sip_message_status_t status, int code, const char *reason);
    void dispatch_message_received(std::string from_uri, std::string content_type, std::string body);

    int set_video_capture_driver(std::shared_ptr<IVideoCaptureDriver> driver);
    int set_remote_renderer(std::shared_ptr<IVideoRendererDriver> renderer);
    int set_local_renderer(std::shared_ptr<IVideoRendererDriver> renderer);
    int set_video_codec_driver(std::shared_ptr<IVideoCodecDriver> driver);
    MediaDriverManager &media_driver_manager();
    void notify_media_resize(int width, int height);
    void preserve_video_rx_and_display(struct call *bcall);

    bool is_started() const { return m_started.load(); }
    sip_client_t *owner() const { return m_owner; }
    void *user_data() const { return m_user_data; }
    const sip_callbacks_t &callbacks() const { return m_callbacks; }

    /**
     * Dispatches a callable task onto the dedicated Baresip re_main worker loop.
     * Safe to invoke from any caller thread.
     */
    bool post_task(std::function<void()> task);

    void handle_ua_event(int ev, struct bevent *event);
    void dispatch_reg_state(sip_reg_state_t state, int code, const char *reason);
    void dispatch_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason);
    void dispatch_incoming_call(sip_call_t *call);
    void dispatch_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted);
    void dispatch_dtmf_digit(sip_call_t *call, const char *digit);
    void dispatch_call_stats(sip_call_t *call, const sip_call_stats_t *stats);
    void dispatch_transfer_status(sip_call_t *call, int code, const char *reason);
    void dispatch_transfer_request(sip_call_t *call, const char *refer_to_uri, bool has_replaces);
    void dispatch_call_rejected_busy(const char *caller_uri);

    bool prepare_audio_session();
    void release_audio_session_if_idle();

    sip_call_t *find_or_create_call_session(struct call *c, const char *remote_uri, bool is_outgoing, bool with_video);
    sip_call_t *find_call_session(struct call *c);
    void remove_call_session(struct call *c);
    void evaluate_pending_hold_on_loop(sip_call_t *call);
    void trigger_hold_fallback_on_loop(sip_call_t *call, int code);

private:
    int init_on_loop_thread(const char *config_path);
    void run_loop(std::string config_path);
    void cleanup_on_loop_thread();
    int configure_wss_tls_on_loop(bool verify_tls_cert, const std::string &ca_file);
    int add_wss_transport_on_loop();
    void auto_hold_other_calls(sip_call_t *except_call);
    void hold_call_on_loop(sip_call_t *call, bool hold);

    static void mqueue_callback(int id, void *data, void *arg);
    static bool sip_request_handler(const struct sip_msg *msg, void *arg);
    bool handle_incoming_message(const struct sip_msg *msg);
    friend struct EngineTestAccess;  // lets core tests feed a decoded MESSAGE

    sip_client_t *m_owner{nullptr};
    sip_callbacks_t m_callbacks{};
    void *m_user_data{nullptr};

    std::thread m_loop_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_start_cv;

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_mic_muted{false};
    std::atomic<bool> m_audio_session_active{false};
    std::atomic<bool> m_is_registered{false};
    std::atomic<int> m_max_calls{4};

    struct mqueue *m_mq{nullptr};
    struct ua *m_ua{nullptr};
    struct tls *m_wss_tls{nullptr};
    std::string m_active_transport;
    bool m_events_registered{false};
    int m_start_result{0};
    bool m_start_signaled{false};

    mutable std::mutex m_calls_mutex;
    std::vector<sip_call_t *> m_all_calls;

    mutable std::mutex m_message_mutex;
    sip_message_callbacks_t m_message_callbacks{};
    std::atomic<uint64_t> m_next_message_id{1};
    struct sip_lsnr *m_message_lsnr{nullptr};

    std::unique_ptr<MediaDriverManager> m_media_manager;
};

} // namespace sip_core

#endif // SIP_CORE_ENGINE_H
