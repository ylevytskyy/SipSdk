#include "engine.h"
#include "call_session.h"
#include "media_driver.h"

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace sip_core {

static void ua_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
    auto *engine = static_cast<Engine *>(arg);
    if (engine) {
        engine->handle_ua_event(static_cast<int>(ev), event);
    }
}

Engine::Engine(sip_client_t *owner, const sip_callbacks_t *callbacks, void *user_data)
    : m_owner(owner)
    , m_user_data(user_data)
    , m_media_manager(std::make_unique<MediaDriverManager>(this))
{
    if (callbacks) {
        m_callbacks = *callbacks;
    }
}

Engine::~Engine()
{
    destroy();
    std::lock_guard<std::mutex> lock(m_calls_mutex);
    for (auto *c : m_all_calls) {
        if (c) {
            c->release();
        }
    }
    m_all_calls.clear();
}

int Engine::init(const char *config_path)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_started.load()) {
        return 0; // Already running
    }
    if (m_loop_thread.joinable()) {
        lock.unlock();
        destroy();
        lock.lock();
    }

    m_stopping = false;
    m_start_signaled = false;
    m_start_result = 0;

    std::string path_str = config_path ? config_path : "";

    m_loop_thread = std::thread([this, path = std::move(path_str)]() mutable {
        this->run_loop(std::move(path));
    });

    m_start_cv.wait(lock, [this]() {
        return m_start_signaled;
    });

    if (m_start_result != 0) {
        lock.unlock();
        if (m_loop_thread.joinable()) {
            m_loop_thread.join();
        }
        return m_start_result;
    }

    return 0;
}

void Engine::destroy()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stopping.exchange(true)) {
        return;
    }
    if (!m_loop_thread.joinable()) {
        m_started = false;
        m_stopping = false;
        return;
    }

    if (m_mq != nullptr) {
        (void)mqueue_push(m_mq, 1, nullptr);
    } else {
        re_cancel();
    }
    lock.unlock();

    if (m_loop_thread.joinable()) {
        m_loop_thread.join();
    }

    lock.lock();
    m_started = false;
    m_stopping = false;
}

bool Engine::post_task(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started.load() || m_stopping.load() || !m_mq) {
        return false;
    }

    auto *task_copy = new std::function<void()>(std::move(task));
    int rc = mqueue_push(m_mq, 2, task_copy);
    if (rc != 0) {
        delete task_copy;
        return false;
    }
    return true;
}

int Engine::init_on_loop_thread(const char *config_path)
{
    int rc = libre_init();
    if (rc != 0) {
        return rc;
    }

    if (config_path && std::strlen(config_path) > 0) {
        rc = conf_path_set(config_path);
        if (rc != 0) {
            libre_close();
            return rc;
        }
    }

    rc = conf_configure();
    if (rc != 0) {
        libre_close();
        return rc;
    }

    re_thread_async_init(4);

    struct config *cfg = conf_config();
    if (cfg != nullptr) {
#if defined(ANDROID) || defined(__ANDROID__)
        if (cfg->audio.src_mod[0] == '\0' || strcmp(cfg->audio.src_mod, "aaudio") == 0) {
            snprintf(cfg->audio.src_mod, sizeof(cfg->audio.src_mod), "%s", "opensles");
            snprintf(cfg->audio.src_dev, sizeof(cfg->audio.src_dev), "%s", "nil");
        }
        if (cfg->audio.play_mod[0] == '\0' || strcmp(cfg->audio.play_mod, "aaudio") == 0) {
            snprintf(cfg->audio.play_mod, sizeof(cfg->audio.play_mod), "%s", "opensles");
            snprintf(cfg->audio.play_dev, sizeof(cfg->audio.play_dev), "%s", "nil");
        }
#elif defined(__APPLE__)
        if (cfg->audio.src_mod[0] == '\0') {
            snprintf(cfg->audio.src_mod, sizeof(cfg->audio.src_mod), "%s", "audiounit");
        }
        if (cfg->audio.play_mod[0] == '\0') {
            snprintf(cfg->audio.play_mod, sizeof(cfg->audio.play_mod), "%s", "audiounit");
        }
#endif
        snprintf(cfg->video.src_mod, sizeof(cfg->video.src_mod), "%s", "avcapture");
        snprintf(cfg->video.src_dev, sizeof(cfg->video.src_dev), "%s", "front");
        snprintf(cfg->video.disp_mod, sizeof(cfg->video.disp_mod), "%s", "iosview");
        if (cfg->video.width == 0 || cfg->video.height == 0) {
            cfg->video.width = 640;
            cfg->video.height = 480;
        }
        if (cfg->video.fps <= 0) {
            cfg->video.fps = 30.0;
        }
        if (cfg->video.bitrate == 0) {
            cfg->video.bitrate = 512000;
        }
        cfg->video.enc_fmt = VID_FMT_YUV420P;
    }

    rc = baresip_init(conf_config());
    if (rc != 0) {
        re_thread_async_close();
        libre_close();
        return rc;
    }

    if (baresip_network() != nullptr) {
        struct sa lo_addr;
        if (sa_set_str(&lo_addr, "127.0.0.1", 0) == 0 && !net_is_laddr(baresip_network(), &lo_addr)) {
            (void)net_add_address_ifname(baresip_network(), &lo_addr, "lo0");
        }
        struct sa dns_servers[2];
        if (sa_set_str(&dns_servers[0], "8.8.8.8", 53) == 0 &&
            sa_set_str(&dns_servers[1], "1.1.1.1", 53) == 0) {
            (void)net_use_nameserver(baresip_network(), dns_servers, 2);
        }
    }

    if (m_media_manager) {
        m_media_manager->register_baresip_plugins();
    }

    rc = ua_init("SipSdk", true, true, false);
    if (rc != 0) {
        baresip_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    rc = bevent_register(ua_event_handler, this);
    if (rc == 0) {
        m_events_registered = true;
    }

#if defined(ANDROID) || defined(__ANDROID__)
    (void)module_preload("opensles");
    (void)module_preload("g711");
#endif

    rc = conf_modules();
    if (rc != 0) {
        if (m_events_registered) {
            bevent_unregister(ua_event_handler);
            m_events_registered = false;
        }
        ua_close();
        baresip_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    rc = mqueue_alloc(&m_mq, mqueue_callback, this);
    if (rc != 0) {
        if (m_events_registered) {
            bevent_unregister(ua_event_handler);
            m_events_registered = false;
        }
        module_app_unload();
        conf_close();
        ua_close();
        baresip_close();
        mod_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    m_started = true;
    return 0;
}

void Engine::run_loop(std::string config_path)
{
    int rc = init_on_loop_thread(config_path.empty() ? nullptr : config_path.c_str());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_start_result = rc;
        m_start_signaled = true;
    }
    m_start_cv.notify_all();

    if (rc == 0) {
        (void)re_main(nullptr);
        cleanup_on_loop_thread();
    }
}

void Engine::cleanup_on_loop_thread()
{
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_all_calls) {
            if (sess) {
                sess->set_closed(0);
                sess->detach_raw_call();
            }
        }
    }
    release_audio_session_if_idle();

    if (m_mq != nullptr) {
        m_mq = (struct mqueue *)mem_deref(m_mq);
    }

    if (m_events_registered) {
        bevent_unregister(ua_event_handler);
        m_events_registered = false;
    }

    if (m_ua != nullptr) {
        ua_destroy(m_ua);
        m_ua = nullptr;
    }
    m_is_registered.store(false);

    if (m_media_manager) {
        m_media_manager->unregister_baresip_plugins();
    }

    ua_close();
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    m_started = false;
}

int Engine::register_account(const AccountConfig &config)
{
    if (config.user.empty() || config.domain.empty() || config.port <= 0 || config.port > 65535) {
        return EINVAL;
    }

    bool posted = post_task([this, cfg = config]() {
        if (m_ua != nullptr) {
            ua_destroy(m_ua);
            m_ua = nullptr;
        }

        std::string transport_param = "";
        std::string lower_transport = cfg.transport;
        for (auto &c : lower_transport) {
            c = (char)::tolower(c);
        }
        if (lower_transport == "tcp") {
            transport_param = ";transport=tcp";
        }

        std::string aor = "sip:" + cfg.user + "@" + cfg.domain + ":" + std::to_string(cfg.port) + transport_param;

        int rc = ua_alloc(&m_ua, aor.c_str());
        if (rc != 0 || m_ua == nullptr) {
            dispatch_reg_state(SIP_REG_STATE_FAILED, rc != 0 ? rc : EINVAL, "Failed to allocate UA");
            return;
        }

        struct account *acc = ua_account(m_ua);
        if (acc != nullptr) {
            if (!cfg.display_name.empty()) {
                (void)account_set_display_name(acc, cfg.display_name.c_str());
            }
            (void)account_set_auth_user(acc, cfg.user.c_str());
            if (!cfg.pwd.empty()) {
                (void)account_set_auth_pass(acc, cfg.pwd.c_str());
            }
            if (!cfg.outbound_proxy.empty()) {
                (void)account_set_outbound(acc, cfg.outbound_proxy.c_str(), 0);
            }
            if (!cfg.stun_server.empty()) {
                (void)account_set_stun_uri(acc, cfg.stun_server.c_str());
            }
            if (!cfg.stun_user.empty()) {
                (void)account_set_stun_user(acc, cfg.stun_user.c_str());
            }
            if (!cfg.stun_pass.empty()) {
                (void)account_set_stun_pass(acc, cfg.stun_pass.c_str());
            }
            if (!cfg.media_nat.empty()) {
                (void)account_set_medianat(acc, cfg.media_nat.c_str());
            }
            if (cfg.reg_interval > 0) {
                (void)account_set_regint(acc, cfg.reg_interval);
            }
            (void)account_set_video_codecs(acc, "H264");
        }

        rc = ua_register(m_ua);
        if (rc != 0) {
            dispatch_reg_state(SIP_REG_STATE_FAILED, rc, "Registration submission failed");
        }
    });

    return posted ? 0 : EIO;
}

int Engine::register_account(const char *user, const char *domain, const char *pwd, const char *transport, int port)
{
    if (!user || std::strlen(user) == 0 ||
        !domain || std::strlen(domain) == 0 ||
        port <= 0 || port > 65535) {
        return EINVAL;
    }

    AccountConfig cfg;
    cfg.user = user;
    cfg.domain = domain;
    cfg.pwd = pwd ? pwd : "";
    cfg.transport = transport ? transport : "udp";
    cfg.port = port;
    return register_account(cfg);
}

int Engine::reset_transports(bool re_register)
{
    bool posted = post_task([re_register]() {
        (void)uag_reset_transp(re_register, true);
    });
    return posted ? 0 : EIO;
}

int Engine::unregister_account()
{
    m_is_registered.store(false);
    bool posted = post_task([this]() {
        if (m_ua != nullptr) {
            ua_unregister(m_ua);
        } else {
            dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, 0, "Unregistered");
        }
    });
    return posted ? 0 : EIO;
}

bool Engine::is_registered() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_is_registered.load() && m_ua != nullptr && ua_isregistered(m_ua);
}

struct sdp_media_internal {
    struct le le;
    struct list lfmtl;
    struct list rfmtl;
    struct list lattrl;
    struct list rattrl;
    struct sa laddr;
    struct sa raddr;
    struct sa laddr_rtcp;
    struct sa raddr_rtcp;
    int32_t lbwv[SDP_BANDWIDTH_MAX];
    int32_t rbwv[SDP_BANDWIDTH_MAX];
    char *name;
    char *proto;
    char *protov[8];
    char *uproto;
    sdp_media_enc_h *ench;
    void *arg;
    enum sdp_dir ldir;
    enum sdp_dir rdir;
    bool fmt_ignore;
    bool disabled;
    int dynpt;
};

void Engine::handle_ua_event(int ev, struct bevent *event)
{
    if (!event) {
        return;
    }

    const struct sip_msg *msg = bevent_get_msg(event);
    const char *text = bevent_get_text(event);
    struct call *bcall = bevent_get_call(event);

    auto extract_reason = [msg, text](const char *default_reason) -> std::string {
        if (msg && msg->reason.p && msg->reason.l > 0) {
            return std::string(msg->reason.p, msg->reason.l);
        }
        if (text && std::strlen(text) > 0) {
            return std::string(text);
        }
        return default_reason;
    };

    switch (ev) {
        case BEVENT_REGISTERING: {
            dispatch_reg_state(SIP_REG_STATE_REGISTERING, 0, extract_reason("Registering").c_str());
            break;
        }
        case BEVENT_REGISTER_OK: {
            m_is_registered.store(true);
            int scode = msg ? static_cast<int>(msg->scode) : 200;
            dispatch_reg_state(SIP_REG_STATE_REGISTERED, scode, extract_reason("OK").c_str());
            break;
        }
        case BEVENT_REGISTER_FAIL: {
            m_is_registered.store(false);
            int scode = msg ? static_cast<int>(msg->scode) : 401;
            dispatch_reg_state(SIP_REG_STATE_FAILED, scode, extract_reason("Registration Failed").c_str());
            break;
        }
        case BEVENT_UNREGISTERING: {
            m_is_registered.store(false);
            int scode = msg ? static_cast<int>(msg->scode) : 0;
            dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, scode, extract_reason("Unregistered").c_str());
            break;
        }
        case BEVENT_CALL_INCOMING: {
            if (bcall) {
                const char *peer_uri = call_peeruri(bcall);
                bool with_video = call_has_video(bcall);
                sip_call_t *sess = find_or_create_call_session(bcall, peer_uri, false, with_video);
                sess->attach_raw_call(bcall);
                dispatch_incoming_call(sess);
                dispatch_call_state(sess, SIP_CALL_STATE_INCOMING, 180, extract_reason("Incoming Call").c_str());
            }
            break;
        }
        case BEVENT_CALL_OUTGOING: {
            if (bcall) {
                sip_call_t *sess = find_or_create_call_session(bcall, call_peeruri(bcall), true, call_has_video(bcall));
                sess->attach_raw_call(bcall);
                dispatch_call_state(sess, SIP_CALL_STATE_CONNECTING, 100, extract_reason("Connecting").c_str());
            }
            break;
        }
        case BEVENT_CALL_RINGING: {
            if (bcall) {
                sip_call_t *sess = find_or_create_call_session(bcall, call_peeruri(bcall), true, call_has_video(bcall));
                dispatch_call_state(sess, SIP_CALL_STATE_RINGING, 180, extract_reason("Ringing").c_str());
            }
            break;
        }
        case BEVENT_CALL_PROGRESS: {
            if (bcall) {
                sip_call_t *sess = find_or_create_call_session(bcall, call_peeruri(bcall), true, call_has_video(bcall));
                dispatch_call_state(sess, SIP_CALL_STATE_CONNECTING, 183, extract_reason("Session Progress").c_str());
            }
            break;
        }
        case BEVENT_CALL_ANSWERED:
        case BEVENT_CALL_ESTABLISHED: {
            if (bcall) {
                sip_call_t *sess = find_or_create_call_session(bcall, call_peeruri(bcall), false, call_has_video(bcall));
                sess->set_established();
                dispatch_call_state(sess, SIP_CALL_STATE_ESTABLISHED, 200, extract_reason("Established").c_str());
                if (sess->has_video()) {
                    preserve_video_rx_and_display(bcall);
                    int width = 640;
                    int height = 480;
                    struct config *cfg = conf_config();
                    if (cfg && cfg->video.width > 0 && cfg->video.height > 0) {
                        width = cfg->video.width;
                        height = cfg->video.height;
                    }
                    dispatch_call_media(sess, width, height, sess->is_on_hold(), sess->is_muted());
                }
            }
            break;
        }
        case BEVENT_CALL_HOLD: {
            if (bcall) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    sess->set_on_hold(true);
                    dispatch_call_state(sess, SIP_CALL_STATE_HOLD, 200, "Hold");
                    dispatch_call_media(sess, 0, 0, true, sess->is_muted());
                }
            }
            break;
        }
        case BEVENT_CALL_RESUME: {
            if (bcall) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    sess->set_on_hold(false);
                    dispatch_call_state(sess, SIP_CALL_STATE_ESTABLISHED, 200, "Resumed");
                    dispatch_call_media(sess, 0, 0, false, sess->is_muted());
                }
            }
            break;
        }
        case BEVENT_CALL_CLOSED: {
            if (bcall) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    uint32_t dur = call_duration(bcall);
                    sess->set_closed(dur);
                    int scode = (msg && msg->scode > 0) ? static_cast<int>(msg->scode) : static_cast<int>(call_scode(bcall));
                    dispatch_call_state(sess, SIP_CALL_STATE_CLOSED, scode, extract_reason("Call Closed").c_str());
                    remove_call_session(bcall);
                }
            }
            release_audio_session_if_idle();
            break;
        }
        case BEVENT_CALL_DTMF_START: {
            if (bcall && text && std::strlen(text) > 0) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    dispatch_dtmf_digit(sess, text);
                }
            }
            break;
        }
        case BEVENT_CALL_REMOTE_SDP: {
            if (bcall != nullptr && text && std::strcmp(text, "answer") == 0) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess && sess->has_video()) {
                    struct video *vid = call_video(bcall);
                    struct stream *strm = vid ? video_strm(vid) : nullptr;
                    struct sdp_media *m = strm ? stream_sdpmedia(strm) : nullptr;
                    if (m && !sdp_media_disabled(m)) {
                        auto *im = reinterpret_cast<struct sdp_media_internal *>(m);
                        if (im->ldir == sdp_media_ldir(m) && im->rdir == sdp_media_rdir(m)) {
                            im->rdir = SDP_SENDRECV;
                        }
                    }
                }
            }
            break;
        }
        case BEVENT_CALL_LOCAL_SDP: {
            if (bcall != nullptr) {
                preserve_video_rx_and_display(bcall);
            }
            break;
        }
        case BEVENT_SIPSESS_CONN: {
            struct ua *bua = bevent_get_ua(event);
            if (!bua) {
                bua = m_ua;
            }
            if (bua && msg) {
                (void)ua_accept(bua, msg);
            }
            break;
        }
        default:
            break;
    }
}

struct RegStateTaskArgs {
    sip_client_t *client{nullptr};
    sip_reg_state_t state{SIP_REG_STATE_UNREGISTERED};
    int code{0};
    std::string reason;
    void (*on_reg_state)(sip_client_t *, sip_reg_state_t, int, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_reg_state_task(void *arg)
{
    auto *args = static_cast<RegStateTaskArgs *>(arg);
    if (args) {
        if (args->on_reg_state) {
            args->on_reg_state(args->client, args->state, args->code, args->reason.c_str(), args->user_data);
        }
        delete args;
    }
}

void Engine::dispatch_reg_state(sip_reg_state_t state, int code, const char *reason)
{
    if (!m_callbacks.on_reg_state) {
        return;
    }

    auto *args = new RegStateTaskArgs{
        m_owner,
        state,
        code,
        reason ? reason : "",
        m_callbacks.on_reg_state,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_reg_state_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_reg_state_task);
#else
    run_reg_state_task(args);
#endif
}

sip_call_t *Engine::make_call(const char *target_uri, bool with_video)
{
    if (!target_uri || std::strlen(target_uri) == 0 || !m_started.load()) {
        return nullptr;
    }

    std::string uri_str = target_uri;
    auto *call_session = find_or_create_call_session(nullptr, uri_str.c_str(), true, with_video);

    prepare_audio_session();

    call_session->retain();
    post_task([this, call_session, uri_str, with_video]() {
        if (!m_ua) {
            call_session->set_closed(0);
            dispatch_call_state(call_session, SIP_CALL_STATE_CLOSED, EINVAL, "No UA available");
            release_audio_session_if_idle();
            call_session->release();
            return;
        }

        struct call *baresip_call = nullptr;
        enum vidmode vmode = with_video ? VIDMODE_ON : VIDMODE_OFF;
        int rc = ua_connect(m_ua, &baresip_call, nullptr, uri_str.c_str(), vmode);
        if (rc != 0 || baresip_call == nullptr) {
            call_session->set_closed(0);
            dispatch_call_state(call_session, SIP_CALL_STATE_CLOSED, rc != 0 ? rc : EIO, "Connect failed");
            release_audio_session_if_idle();
            call_session->release();
            return;
        }

        call_session->attach_raw_call(baresip_call);
        dispatch_call_state(call_session, SIP_CALL_STATE_CONNECTING, 100, "Connecting");
        call_session->release();
    });

    return call_session;
}

int Engine::answer_call(sip_call_t *call, bool with_video)
{
    if (!call) return EINVAL;

    prepare_audio_session();

    call->retain();
    post_task([this, call, with_video]() {
        struct call *c = call->raw_call();
        if (m_ua != nullptr && c != nullptr) {
            enum vidmode vmode = with_video ? VIDMODE_ON : VIDMODE_OFF;
            (void)ua_answer(m_ua, c, vmode);
        } else {
            release_audio_session_if_idle();
        }
        call->release();
    });

    return 0;
}

int Engine::hangup_call(sip_call_t *call, int code, const char *reason)
{
    if (!call) return EINVAL;

    std::string reason_str = reason ? reason : "";
    uint16_t scode = code > 0 ? static_cast<uint16_t>(code) : 0;

    call->retain();
    post_task([this, call, scode, reason_str]() {
        struct call *c = call->raw_call();
        if (m_ua != nullptr && c != nullptr) {
            ua_hangup(m_ua, c, scode, reason_str.empty() ? nullptr : reason_str.c_str());
        } else {
            call->set_closed(0);
            dispatch_call_state(call, SIP_CALL_STATE_CLOSED, scode, reason_str.c_str());
            release_audio_session_if_idle();
        }
        call->release();
    });

    return 0;
}

int Engine::hold_call(sip_call_t *call, bool hold)
{
    if (!call) return EINVAL;

    call->retain();
    post_task([this, call, hold]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            (void)call_hold(c, hold);
            call->set_on_hold(hold);
            dispatch_call_state(call, hold ? SIP_CALL_STATE_HOLD : SIP_CALL_STATE_ESTABLISHED,
                                200, hold ? "Hold" : "Resumed");
            dispatch_call_media(call, 0, 0, hold, call->is_muted());
        }
        call->release();
    });

    return 0;
}

int Engine::mute_call(sip_call_t *call, bool muted)
{
    if (!call) return EINVAL;

    call->retain();
    post_task([this, call, muted]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            struct audio *au = call_audio(c);
            if (au != nullptr) {
                audio_mute(au, muted);
            }
            call->set_muted(muted);
            dispatch_call_media(call, 0, 0, call->is_on_hold(), muted);
        }
        call->release();
    });

    return 0;
}

int Engine::send_dtmf(sip_call_t *call, const char *digits)
{
    if (!call || !digits || std::strlen(digits) == 0) return EINVAL;

    std::string digits_str = digits;

    call->retain();
    post_task([call, digits_str]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            for (char ch : digits_str) {
                (void)call_send_digit(c, ch);
            }
            (void)call_send_digit(c, KEYCODE_REL);
        }
        call->release();
    });

    return 0;
}

int Engine::set_client_mic_muted(bool muted)
{
    m_mic_muted = muted;
    post_task([this, muted]() {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_all_calls) {
            if (sess && sess->raw_call() != nullptr) {
                struct audio *au = call_audio(sess->raw_call());
                if (au != nullptr) {
                    audio_mute(au, muted);
                }
                sess->set_muted(muted);
                dispatch_call_media(sess, 0, 0, sess->is_on_hold(), muted);
            }
        }
    });
    return 0;
}

bool Engine::is_client_mic_muted() const
{
    return m_mic_muted.load();
}

int Engine::set_audio_driver(const char *src_mod, const char *src_dev, const char *play_mod, const char *play_dev)
{
    struct config *cfg = conf_config();
    if (!cfg) return -1;
    if (src_mod && std::strlen(src_mod) > 0) {
        snprintf(cfg->audio.src_mod, sizeof(cfg->audio.src_mod), "%s", src_mod);
    }
    if (src_dev) {
        snprintf(cfg->audio.src_dev, sizeof(cfg->audio.src_dev), "%s", src_dev);
    }
    if (play_mod && std::strlen(play_mod) > 0) {
        snprintf(cfg->audio.play_mod, sizeof(cfg->audio.play_mod), "%s", play_mod);
    }
    if (play_dev) {
        snprintf(cfg->audio.play_dev, sizeof(cfg->audio.play_dev), "%s", play_dev);
    }
    return 0;
}

int Engine::get_audio_driver(char *src_mod, size_t src_mod_sz, char *play_mod, size_t play_mod_sz) const
{
    struct config *cfg = conf_config();
    if (!cfg) return -1;
    if (src_mod && src_mod_sz > 0) {
        snprintf(src_mod, src_mod_sz, "%s", cfg->audio.src_mod);
    }
    if (play_mod && play_mod_sz > 0) {
        snprintf(play_mod, play_mod_sz, "%s", cfg->audio.play_mod);
    }
    return 0;
}

void Engine::destroy_call(sip_call_t *call)
{
    if (!call) return;
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto it = m_all_calls.begin(); it != m_all_calls.end(); ++it) {
            if (*it == call) {
                m_all_calls.erase(it);
                break;
            }
        }
    }
    call->release();
}

bool Engine::prepare_audio_session()
{
    if (m_callbacks.prepare_audio_session) {
        bool ok = m_callbacks.prepare_audio_session(m_owner, m_user_data);
        if (ok) {
            m_audio_session_active = true;
        }
        return ok;
    }
#if defined(__APPLE__)
    Class cls = objc_getClass("AVAudioSession");
    if (cls) {
        SEL sharedSel = sel_registerName("sharedInstance");
        auto sharedFn = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend);
        id session = sharedFn(cls, sharedSel);
        if (session) {
            Class nsStringCls = objc_getClass("NSString");
            SEL strWithUtf8Sel = sel_registerName("stringWithUTF8String:");
            auto strFn = reinterpret_cast<id (*)(Class, SEL, const char *)>(objc_msgSend);
            id cat = strFn(nsStringCls, strWithUtf8Sel, "AVAudioSessionCategoryPlayAndRecord");
            id mode = strFn(nsStringCls, strWithUtf8Sel, "AVAudioSessionModeVoiceChat");

            SEL setCatModeOptSel = sel_registerName("setCategory:mode:options:error:");
            auto setCatFn = reinterpret_cast<bool (*)(id, SEL, id, id, unsigned long, id *)>(objc_msgSend);
            setCatFn(session, setCatModeOptSel, cat, mode, 1 /* DefaultToSpeaker */, nullptr);

            SEL setActiveSel = sel_registerName("setActive:error:");
            auto setActiveFn = reinterpret_cast<bool (*)(id, SEL, bool, id *)>(objc_msgSend);
            setActiveFn(session, setActiveSel, true, nullptr);
        }
    }
#endif
    m_audio_session_active = true;
    return true;
}

void Engine::release_audio_session_if_idle()
{
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *c : m_all_calls) {
            if (c && !c->is_closed()) {
                return;
            }
        }
    }
    if (m_audio_session_active.exchange(false)) {
        if (m_callbacks.release_audio_session) {
            m_callbacks.release_audio_session(m_owner, m_user_data);
        }
#if defined(__APPLE__)
        Class cls = objc_getClass("AVAudioSession");
        if (cls) {
            SEL sharedSel = sel_registerName("sharedInstance");
            auto sharedFn = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend);
            id session = sharedFn(cls, sharedSel);
            if (session) {
                SEL setActiveSel = sel_registerName("setActive:error:");
                auto setActiveFn = reinterpret_cast<bool (*)(id, SEL, bool, id *)>(objc_msgSend);
                setActiveFn(session, setActiveSel, false, nullptr);
            }
        }
#endif
    }
}

sip_call_t *Engine::find_or_create_call_session(struct call *c, const char *remote_uri, bool is_outgoing, bool with_video)
{
    std::lock_guard<std::mutex> lock(m_calls_mutex);
    if (c != nullptr) {
        for (auto *sess : m_all_calls) {
            if (sess && sess->raw_call() == c) {
                return sess;
            }
        }
        if (is_outgoing) {
            for (auto *sess : m_all_calls) {
                if (sess && sess->raw_call() == nullptr && sess->is_outgoing()) {
                    sess->attach_raw_call(c);
                    return sess;
                }
            }
        }
    }
    auto *new_sess = new sip_call_t(m_owner, this, c, remote_uri, is_outgoing, with_video);
    m_all_calls.push_back(new_sess);
    return new_sess;
}

sip_call_t *Engine::find_call_session(struct call *c)
{
    std::lock_guard<std::mutex> lock(m_calls_mutex);
    for (auto *sess : m_all_calls) {
        if (sess && sess->raw_call() == c) {
            return sess;
        }
    }
    return nullptr;
}

void Engine::remove_call_session(struct call *c)
{
    std::lock_guard<std::mutex> lock(m_calls_mutex);
    for (auto *sess : m_all_calls) {
        if (sess && sess->raw_call() == c) {
            sess->detach_raw_call();
        }
    }
}

struct CallStateTaskArgs {
    sip_call_t *call{nullptr};
    sip_call_state_t state{SIP_CALL_STATE_CLOSED};
    int code{0};
    std::string reason;
    void (*on_call_state)(sip_call_t *, sip_call_state_t, int, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_call_state_task(void *arg)
{
    auto *args = static_cast<CallStateTaskArgs *>(arg);
    if (args) {
        if (args->on_call_state) {
            args->on_call_state(args->call, args->state, args->code, args->reason.c_str(), args->user_data);
        }
        if (args->call) {
            args->call->release();
        }
        delete args;
    }
}

void Engine::dispatch_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason)
{
    if (!m_callbacks.on_call_state) return;

    if (call) {
        call->retain();
    }
    auto *args = new CallStateTaskArgs{
        call,
        state,
        code,
        reason ? reason : "",
        m_callbacks.on_call_state,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_call_state_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_call_state_task);
#else
    run_call_state_task(args);
#endif
}

struct IncomingCallTaskArgs {
    sip_client_t *client{nullptr};
    sip_call_t *call{nullptr};
    void (*on_incoming_call)(sip_client_t *, sip_call_t *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_incoming_call_task(void *arg)
{
    auto *args = static_cast<IncomingCallTaskArgs *>(arg);
    if (args) {
        if (args->on_incoming_call) {
            args->on_incoming_call(args->client, args->call, args->user_data);
        }
        if (args->call) {
            args->call->release();
        }
        delete args;
    }
}

void Engine::dispatch_incoming_call(sip_call_t *call)
{
    if (!m_callbacks.on_incoming_call) return;

    if (call) {
        call->retain();
    }
    auto *args = new IncomingCallTaskArgs{
        m_owner,
        call,
        m_callbacks.on_incoming_call,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_incoming_call_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_incoming_call_task);
#else
    run_incoming_call_task(args);
#endif
}

struct CallMediaTaskArgs {
    sip_call_t *call{nullptr};
    int width{0};
    int height{0};
    bool on_hold{false};
    bool is_muted{false};
    void (*on_call_media)(sip_call_t *, int, int, bool, bool, void *){nullptr};
    void *user_data{nullptr};
};

static void run_call_media_task(void *arg)
{
    auto *args = static_cast<CallMediaTaskArgs *>(arg);
    if (args) {
        if (args->on_call_media) {
            args->on_call_media(args->call, args->width, args->height, args->on_hold, args->is_muted, args->user_data);
        }
        if (args->call) {
            args->call->release();
        }
        delete args;
    }
}

void Engine::dispatch_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted)
{
    if (!m_callbacks.on_call_media) return;

    if (call) {
        call->retain();
    }
    auto *args = new CallMediaTaskArgs{
        call,
        width,
        height,
        on_hold,
        is_muted,
        m_callbacks.on_call_media,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_call_media_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_call_media_task);
#else
    run_call_media_task(args);
#endif
}

struct DtmfDigitTaskArgs {
    sip_call_t *call{nullptr};
    std::string digit;
    void (*on_dtmf_digit)(sip_call_t *, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_dtmf_digit_task(void *arg)
{
    auto *args = static_cast<DtmfDigitTaskArgs *>(arg);
    if (args) {
        if (args->on_dtmf_digit) {
            args->on_dtmf_digit(args->call, args->digit.c_str(), args->user_data);
        }
        if (args->call) {
            args->call->release();
        }
        delete args;
    }
}

void Engine::dispatch_dtmf_digit(sip_call_t *call, const char *digit)
{
    if (!m_callbacks.on_dtmf_digit) return;

    if (call) {
        call->retain();
    }
    auto *args = new DtmfDigitTaskArgs{
        call,
        digit ? digit : "",
        m_callbacks.on_dtmf_digit,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_dtmf_digit_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_dtmf_digit_task);
#else
    run_dtmf_digit_task(args);
#endif
}

int Engine::set_video_capture_driver(std::shared_ptr<IVideoCaptureDriver> driver)
{
    if (m_started.load()) {
        post_task([this, driver]() {
            if (m_media_manager) {
                m_media_manager->set_capture_driver(driver);
            }
        });
    } else {
        if (m_media_manager) {
            m_media_manager->set_capture_driver(driver);
        }
    }
    return 0;
}

int Engine::set_remote_renderer(std::shared_ptr<IVideoRendererDriver> renderer)
{
    if (m_started.load()) {
        post_task([this, renderer]() {
            if (m_media_manager) {
                m_media_manager->set_remote_renderer(renderer);
            }
        });
    } else {
        if (m_media_manager) {
            m_media_manager->set_remote_renderer(renderer);
        }
    }
    return 0;
}

int Engine::set_local_renderer(std::shared_ptr<IVideoRendererDriver> renderer)
{
    if (m_started.load()) {
        post_task([this, renderer]() {
            if (m_media_manager) {
                m_media_manager->set_local_renderer(renderer);
            }
        });
    } else {
        if (m_media_manager) {
            m_media_manager->set_local_renderer(renderer);
        }
    }
    return 0;
}

int Engine::set_video_codec_driver(std::shared_ptr<IVideoCodecDriver> driver)
{
    if (m_started.load()) {
        post_task([this, driver]() {
            if (m_media_manager) {
                m_media_manager->set_codec_driver(driver);
            }
        });
    } else {
        if (m_media_manager) {
            m_media_manager->set_codec_driver(driver);
        }
    }
    return 0;
}

MediaDriverManager &Engine::media_driver_manager()
{
    return *m_media_manager;
}

void Engine::notify_media_resize(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_calls_mutex);
    for (auto *sess : m_all_calls) {
        if (sess && sess->has_video() && !sess->is_closed()) {
            dispatch_call_media(sess, width, height, sess->is_on_hold(), sess->is_muted());
        }
    }
}

void Engine::preserve_video_rx_and_display(struct call *bcall)
{
    if (!bcall) return;
    sip_call_t *sess = find_call_session(bcall);
    if (!sess || !sess->has_video()) return;
    struct video *vid = call_video(bcall);
    if (!vid) return;
    struct stream *strm = video_strm(vid);
    if (!strm) return;
    struct sdp_media *m = stream_sdpmedia(strm);
    if (!m || sdp_media_disabled(m)) return;

    auto *im = reinterpret_cast<struct sdp_media_internal *>(m);
    if (im->ldir == sdp_media_ldir(m) && im->rdir == sdp_media_rdir(m)) {
        im->rdir = SDP_SENDRECV;
    }

    (void)stream_enable_rx(strm, true);
    (void)video_start_display(vid, call_peeruri(bcall));
    video_req_keyframe(vid);
}

int Engine::request_keyframe(sip_call_t *call_sess)
{
    if (!call_sess) return EINVAL;
    call_sess->retain();
    bool posted = post_task([call_sess]() {
        struct call *c = call_sess->raw_call();
        if (c != nullptr) {
            struct video *vid = call_video(c);
            if (vid != nullptr) {
                video_req_keyframe(vid);
            }
        }
        call_sess->release();
    });
    if (!posted) {
        call_sess->release();
        return EIO;
    }
    return 0;
}

int Engine::switch_camera()
{
    if (!m_media_manager) return EINVAL;
    m_media_manager->switch_camera();
    return 0;
}

int Engine::set_camera(int position)
{
    if (!m_media_manager) return EINVAL;
    m_media_manager->set_camera(position);
    const char *dev = (position == 1) ? "back" : "front";
    post_task([this, dev]() {
        if (conf_config() != nullptr) {
            snprintf(conf_config()->video.src_dev, sizeof(conf_config()->video.src_dev), "%s", dev);
        }
    });
    return 0;
}

int Engine::set_local_video_enabled(bool enabled)
{
    if (!m_media_manager) return EINVAL;
    return m_media_manager->set_local_video_enabled(enabled);
}

bool Engine::is_local_video_enabled() const
{
    if (!m_media_manager) return false;
    return m_media_manager->is_local_video_enabled();
}

int Engine::start_local_preview()
{
    if (!m_media_manager) return EINVAL;
    return m_media_manager->start_local_preview();
}

int Engine::stop_local_preview()
{
    if (!m_media_manager) return EINVAL;
    return m_media_manager->stop_local_preview();
}

bool Engine::is_local_preview_active() const
{
    if (!m_media_manager) return false;
    return m_media_manager->is_local_preview_active();
}

void Engine::mqueue_callback(int id, void *data, void *arg)
{
    auto *engine = static_cast<Engine *>(arg);
    if (!engine) {
        return;
    }

    if (id == 1) {
        re_cancel();
    } else if (id == 2 && data != nullptr) {
        auto *task = static_cast<std::function<void()> *>(data);
        if (task && *task) {
            (*task)();
        }
        delete task;
    }
}

} // namespace sip_core
