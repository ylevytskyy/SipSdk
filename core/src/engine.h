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

namespace sip_core {

class MediaDriverManager;

class Engine {
public:
    Engine(sip_client_t *owner, const sip_callbacks_t *callbacks, void *user_data);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    int init(const char *config_path);
    void destroy();

    int register_account(const char *user, const char *domain, const char *pwd, const char *transport, int port);
    int unregister_account();
    bool is_registered() const;

    sip_call_t *make_call(const char *target_uri, bool with_video);
    int answer_call(sip_call_t *call, bool with_video);
    int hangup_call(sip_call_t *call, int code, const char *reason);
    int hold_call(sip_call_t *call, bool hold);
    int mute_call(sip_call_t *call, bool muted);
    int send_dtmf(sip_call_t *call, const char *digits);
    int set_client_mic_muted(bool muted);
    bool is_client_mic_muted() const;
    void destroy_call(sip_call_t *call);
    int request_keyframe(sip_call_t *call);

    int switch_camera();
    int set_camera(int position);
    int set_local_video_enabled(bool enabled);
    bool is_local_video_enabled() const;
    int start_local_preview();
    int stop_local_preview();
    bool is_local_preview_active() const;

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

    bool prepare_audio_session();
    void release_audio_session_if_idle();

    sip_call_t *find_or_create_call_session(struct call *c, const char *remote_uri, bool is_outgoing, bool with_video);
    sip_call_t *find_call_session(struct call *c);
    void remove_call_session(struct call *c);

private:
    int init_on_loop_thread(const char *config_path);
    void run_loop(std::string config_path);
    void cleanup_on_loop_thread();

    static void mqueue_callback(int id, void *data, void *arg);

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

    struct mqueue *m_mq{nullptr};
    struct ua *m_ua{nullptr};
    bool m_events_registered{false};
    int m_start_result{0};
    bool m_start_signaled{false};

    mutable std::mutex m_calls_mutex;
    std::vector<sip_call_t *> m_all_calls;

    std::unique_ptr<MediaDriverManager> m_media_manager;
};

} // namespace sip_core

#endif // SIP_CORE_ENGINE_H
