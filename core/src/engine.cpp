#include "engine.h"
#include "account_session.h"
#include "call_session.h"
#include "media_driver.h"

#include <algorithm>

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
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
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *c : m_all_calls) {
            if (c) {
                c->release();
            }
        }
        m_all_calls.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_accounts_mutex);
        for (auto *a : m_accounts) {
            if (a) {
                a->release();
            }
        }
        m_accounts.clear();
        m_default_account = nullptr;
    }
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
        if (cfg->audio.src_mod[0] == '\0' || strcmp(cfg->audio.src_mod, "coreaudio") == 0) {
            snprintf(cfg->audio.src_mod, sizeof(cfg->audio.src_mod), "%s", "audiounit");
        }
        if (cfg->audio.play_mod[0] == '\0' || strcmp(cfg->audio.play_mod, "coreaudio") == 0) {
            snprintf(cfg->audio.play_mod, sizeof(cfg->audio.play_mod), "%s", "audiounit");
        }
#if TARGET_OS_OSX || (defined(TARGET_OS_MAC) && !TARGET_OS_IPHONE)
        // Ensure audio buffer is at least 80ms min to prevent CoreAudio HAL (512 frames = 64ms at 8kHz) underrun chopping
        if (cfg->audio.buffer.min < 80) {
            cfg->audio.buffer.min = 80;
        }
        if (cfg->audio.buffer.max < 240) {
            cfg->audio.buffer.max = 240;
        }
#endif
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
        cfg->avt.rxmode = RECEIVE_MODE_MAIN;
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

    if (cfg != nullptr) {
        cfg->sip.transports =
            (1u << SIP_TRANSP_UDP) |
            (1u << SIP_TRANSP_TCP) |
            (1u << SIP_TRANSP_TLS) |
            (1u << SIP_TRANSP_WS);
        // SIP_TRANSP_WSS is added and managed by Engine via m_wss_tls so
        // per-account verify_tls_cert and tls_ca_file apply cleanly.
    }

    rc = ua_init("SipSdk", true, true, true);
    if (rc != 0) {
        baresip_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    (void)configure_wss_tls_on_loop(true, "");

    rc = bevent_register(ua_event_handler, this);
    if (rc == 0) {
        m_events_registered = true;
    }

    // Incoming SIP MESSAGE requests for our account (any content type).
    if (uag_sip() != nullptr) {
        (void)sip_listen(&m_message_lsnr, uag_sip(), true, sip_request_handler, this);
    }

#if defined(__APPLE__)
    (void)module_preload("audiounit");
#elif defined(ANDROID) || defined(__ANDROID__)
    (void)module_preload("opensles");
#endif
    (void)module_preload("g711");
    (void)module_preload("account");
    (void)module_preload("stun");
    (void)module_preload("turn");
    (void)module_preload("ice");
    (void)module_preload("srtp");
    (void)module_preload("dtls_srtp");
    (void)module_preload("opus");

    rc = conf_modules();
    if (rc != 0) {
        m_message_lsnr = (struct sip_lsnr *)mem_deref(m_message_lsnr);
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
        m_message_lsnr = (struct sip_lsnr *)mem_deref(m_message_lsnr);
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
    fail_pending_messages("Client stopped");
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

    m_message_lsnr = (struct sip_lsnr *)mem_deref(m_message_lsnr);

    if (m_events_registered) {
        bevent_unregister(ua_event_handler);
        m_events_registered = false;
    }

    {
        std::vector<sip_account_t *> to_destroy;
        {
            std::lock_guard<std::mutex> lock(m_accounts_mutex);
            to_destroy.swap(m_accounts);
            m_default_account = nullptr;
        }
        for (auto *acc : to_destroy) {
            if (acc) {
                acc->mark_removed();
                acc->set_reg_state(SIP_REG_STATE_UNREGISTERED);
                struct ua *u = acc->detach_raw_ua();
                if (u != nullptr) {
                    ua_destroy(u);
                }
                acc->release();
            }
        }
    }

    if (m_media_manager) {
        m_media_manager->unregister_baresip_plugins();
    }

    ua_close();
    if (m_wss_tls != nullptr) {
        m_wss_tls = (struct tls *)mem_deref(m_wss_tls);
    }
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    m_started = false;
}

struct WssLaddrCtx {
    struct sip *sip;
    struct tls *tls;
    int err;
};

static bool add_wss_laddr_cb(const char *ifname, const struct sa *sa, void *arg)
{
    (void)ifname;
    auto *ctx = static_cast<WssLaddrCtx *>(arg);
    if (!ctx || !ctx->sip || !ctx->tls || !sa) {
        return false;
    }
    struct sa local;
    sa_cpy(&local, sa);
    sa_set_port(&local, 0);
    int rc = sip_transp_add_websock(ctx->sip, SIP_TRANSP_WSS, &local, false, nullptr, ctx->tls);
    if (rc != 0 && ctx->err == 0) {
        ctx->err = rc;
    }
    return false;
}

int Engine::add_wss_transport_on_loop()
{
    struct sip *sip = uag_sip();
    struct network *net = baresip_network();
    if (!sip || !net || !m_wss_tls) {
        return EINVAL;
    }
    WssLaddrCtx ctx{sip, m_wss_tls, 0};
    net_laddr_apply(net, add_wss_laddr_cb, &ctx);
    return ctx.err;
}

int Engine::configure_wss_tls_on_loop(bool verify_tls_cert, const std::string &ca_file)
{
    struct tls *new_tls = nullptr;
    int rc = tls_alloc(&new_tls, TLS_METHOD_TLS, nullptr, nullptr);
    if (rc != 0 || !new_tls) {
        return rc != 0 ? rc : ENOMEM;
    }

    (void)tls_set_verify_purpose(new_tls, "sslserver");

    if (!verify_tls_cert) {
        tls_disable_verify_server(new_tls);
    } else if (!ca_file.empty()) {
        int ca_rc = tls_add_ca(new_tls, ca_file.c_str());
        if (ca_rc != 0) {
            mem_deref(new_tls);
            return ca_rc;
        }
    } else {
        struct config *bcfg = conf_config();
        if (bcfg != nullptr && (bcfg->sip.cafile[0] != '\0' || bcfg->sip.capath[0] != '\0')) {
            const char *cf = bcfg->sip.cafile[0] != '\0' ? bcfg->sip.cafile : nullptr;
            const char *cp = bcfg->sip.capath[0] != '\0' ? bcfg->sip.capath : nullptr;
            (void)tls_add_cafile_path(new_tls, cf, cp);
        }
    }

    if (m_wss_tls != nullptr) {
        m_wss_tls = (struct tls *)mem_deref(m_wss_tls);
    }
    m_wss_tls = new_tls;

    // Flush and re-add SIP transports so WSS binds to the updated TLS context.
    (void)uag_reset_transp(false, false);
    return add_wss_transport_on_loop();
}

static std::string build_canonical_aor(const AccountConfig &cfg,
                                       std::string &lower_transport,
                                       std::string &transport_param,
                                       int &effective_port)
{
    lower_transport = cfg.transport;
    for (auto &c : lower_transport) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    if (lower_transport.empty()) {
        lower_transport = "udp";
    }
    transport_param.clear();
    if (lower_transport == "tcp") {
        transport_param = ";transport=tcp";
    } else if (lower_transport == "tls") {
        transport_param = ";transport=tls";
    } else if (lower_transport == "ws") {
        transport_param = ";transport=ws";
    } else if (lower_transport == "wss") {
        transport_param = ";transport=wss";
    }

    effective_port = cfg.port;
    if (effective_port <= 0) {
        if (lower_transport == "tls") {
            effective_port = 5061;
        } else if (lower_transport == "ws") {
            effective_port = 80;
        } else if (lower_transport == "wss") {
            effective_port = 443;
        } else {
            effective_port = 5060;
        }
    }

    return "sip:" + cfg.user + "@" + cfg.domain + ":" + std::to_string(effective_port) + transport_param;
}

sip_account_t *Engine::find_account_by_ua(struct ua *u) const
{
    if (!u) return nullptr;
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    for (auto *acc : m_accounts) {
        if (acc && acc->raw_ua() == u) {
            return acc;
        }
    }
    return nullptr;
}

sip_account_t *Engine::find_account_by_call(struct call *c) const
{
    if (!c) return nullptr;
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    for (auto *acc : m_accounts) {
        if (!acc) continue;
        struct ua *u = acc->raw_ua();
        if (!u) continue;
        struct list *calls = ua_calls(u);
        for (struct le *le = list_head(calls); le != nullptr; le = le->next) {
            if (le->data == c) {
                return acc;
            }
        }
    }
    return nullptr;
}

struct ua *Engine::resolve_call_ua(const sip_call_t *call) const
{
    if (call) {
        sip_account_t *acc = call->account();
        if (acc) {
            struct ua *u = acc->raw_ua();
            if (u) return u;
        }
    }
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    if (m_default_account) {
        return m_default_account->raw_ua();
    }
    for (auto *acc : m_accounts) {
        if (acc && acc->raw_ua() != nullptr) {
            return acc->raw_ua();
        }
    }
    return nullptr;
}

bool Engine::is_account_active(const sip_account_t *account) const
{
    if (!account || account->is_removed()) return false;
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    return std::find(m_accounts.begin(), m_accounts.end(), account) != m_accounts.end();
}

int Engine::start_account_ua_on_loop(sip_account_t *account, const AccountConfig &cfg, int effective_max_calls)
{
    if (!account || account->is_removed()) {
        return EINVAL;
    }

    struct config *bcfg = conf_config();
    if (bcfg != nullptr) {
        /* Keep Baresip's internal cap one above m_max_calls so the (cap+1)-th INVITE
           reaches BEVENT_SIPSESS_CONN where Engine replies 486 and fires on_call_rejected_busy. */
        bcfg->call.max_calls = static_cast<uint32_t>(effective_max_calls + 1);
    }

    struct ua *old_ua = account->detach_raw_ua();
    if (old_ua != nullptr) {
        ua_destroy(old_ua);
    }

    std::string lower_transport;
    std::string transport_param;
    int port = 0;
    std::string aor = build_canonical_aor(cfg, lower_transport, transport_param, port);

    auto notify_fail = [this, account](int code, const char *reason) {
        account->set_reg_state(SIP_REG_STATE_FAILED);
        dispatch_account_reg_state(account, SIP_REG_STATE_FAILED, code, reason);
        if (get_default_account() == account) {
            dispatch_reg_state(SIP_REG_STATE_FAILED, code, reason);
        }
    };

    if (lower_transport == "tls") {
        struct tls *t = uag_tls();
        if (t != nullptr) {
            if (!cfg.verify_tls_cert) {
                tls_disable_verify_server(t);
            }
            if (!cfg.tls_ca_file.empty()) {
                int ca_rc = tls_add_ca(t, cfg.tls_ca_file.c_str());
                if (ca_rc != 0 && cfg.verify_tls_cert) {
                    notify_fail(ca_rc, "Failed to load TLS CA certificate");
                    return ca_rc;
                }
            }
        }
    } else if (lower_transport == "wss") {
        int wss_rc = configure_wss_tls_on_loop(cfg.verify_tls_cert, cfg.tls_ca_file);
        if (wss_rc != 0 && cfg.verify_tls_cert) {
            notify_fail(wss_rc, "Failed to configure WSS TLS context");
            return wss_rc;
        }
    }

    struct ua *new_ua = nullptr;
    int rc = ua_alloc(&new_ua, aor.c_str());
    if (rc != 0 || new_ua == nullptr) {
        notify_fail(rc != 0 ? rc : EINVAL, "Failed to allocate UA");
        return rc != 0 ? rc : EINVAL;
    }

    account->attach_raw_ua(new_ua);

    struct account *acc = ua_account(new_ua);
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
        } else if (lower_transport == "tls" || lower_transport == "tcp" ||
                   lower_transport == "ws" || lower_transport == "wss") {
            std::string outbound = "sip:" + cfg.domain + ":" + std::to_string(port) + transport_param;
            (void)account_set_outbound(acc, outbound.c_str(), 0);
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

        std::string media_enc = cfg.media_enc;
        if (media_enc.empty() && (lower_transport == "tls" || lower_transport == "wss")) {
            media_enc = "srtp-mand";
        }
        if (!media_enc.empty() && media_enc != "none") {
            (void)account_set_mediaenc(acc, media_enc.c_str());
        }

        (void)account_set_video_codecs(acc, "H264/packetization-mode=1");
        if (aucodec_find(baresip_aucodecl(), "opus", 48000, 2) != nullptr) {
            (void)account_set_audio_codecs(acc, "opus/48000/2,G722/16000/1,PCMU/8000/1,PCMA/8000/1");
        } else {
            (void)account_set_audio_codecs(acc, "G722/16000/1,PCMU/8000/1,PCMA/8000/1");
        }
    }

    rc = ua_register(new_ua);
    if (rc != 0) {
        notify_fail(rc, "Registration submission failed");
    }
    return rc;
}

int Engine::set_account_callbacks(const sip_account_callbacks_t *callbacks)
{
    std::lock_guard<std::mutex> lock(m_account_cb_mutex);
    if (callbacks) {
        m_account_callbacks = *callbacks;
    } else {
        m_account_callbacks = sip_account_callbacks_t{};
    }
    return 0;
}

int Engine::add_account(const AccountConfig &config, sip_account_t **out_account)
{
    if (out_account) *out_account = nullptr;
    if (!out_account || config.user.empty() || config.domain.empty() || config.port < 0 || config.port > 65535) {
        return EINVAL;
    }
    if (!m_started.load()) {
        return EIO;
    }

    std::string lower_transport;
    std::string transport_param;
    int port = 0;
    std::string aor = build_canonical_aor(config, lower_transport, transport_param, port);

    int effective_max_calls = config.max_calls == 0 ? m_max_calls.load() : config.max_calls;
    if (effective_max_calls < 1) effective_max_calls = 1;
    if (effective_max_calls > 8) effective_max_calls = 8;
    m_max_calls.store(effective_max_calls);

    sip_account_t *account = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_accounts_mutex);
        if (m_accounts.size() >= SIP_CLIENT_MAX_ACCOUNTS) {
            return EBUSY;
        }
        for (auto *existing : m_accounts) {
            if (existing && !existing->is_removed() && existing->aor_str() == aor) {
                return EEXIST;
            }
        }
        uint64_t id = m_next_account_id.fetch_add(1);
        account = new sip_account_t(m_owner, this, id, aor, config.user, config.domain, lower_transport);
        // Engine retains 1 ref; caller receives 1 ref (initial refcount is 1, so retain once for engine).
        account->retain();
        m_accounts.push_back(account);
        if (!m_default_account) {
            m_default_account = account;
        }
    }

    account->retain();
    bool posted = post_task([this, account, cfg = config, effective_max_calls]() {
        (void)start_account_ua_on_loop(account, cfg, effective_max_calls);
        account->release();
    });
    if (!posted) {
        {
            std::lock_guard<std::mutex> lock(m_accounts_mutex);
            auto it = std::find(m_accounts.begin(), m_accounts.end(), account);
            if (it != m_accounts.end()) {
                m_accounts.erase(it);
                if (m_default_account == account) {
                    m_default_account = m_accounts.empty() ? nullptr : m_accounts.front();
                }
                account->release(); // engine ref
            }
        }
        account->release(); // task ref
        account->release(); // caller ref
        return EIO;
    }

    *out_account = account;
    return 0;
}

int Engine::remove_account(sip_account_t *account)
{
    if (!account) return EINVAL;
    {
        std::lock_guard<std::mutex> lock(m_accounts_mutex);
        auto it = std::find(m_accounts.begin(), m_accounts.end(), account);
        if (it == m_accounts.end()) {
            return EINVAL;
        }
        m_accounts.erase(it);
        account->mark_removed();
        if (m_default_account == account) {
            m_default_account = m_accounts.empty() ? nullptr : m_accounts.front();
        }
    }

    // Transfer engine's reference to the cleanup task on the loop thread.
    bool posted = post_task([this, account]() {
        struct ua *u = account->detach_raw_ua();
        if (u != nullptr) {
            std::vector<sip_call_t *> acc_calls;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                for (auto *sess : m_all_calls) {
                    if (sess && !sess->is_closed() && sess->account() == account) {
                        sess->retain();
                        acc_calls.push_back(sess);
                    }
                }
            }
            for (auto *sess : acc_calls) {
                struct call *c = sess->raw_call();
                if (c != nullptr) {
                    sess->latch_media_stats();
                    ua_hangup(u, c, 0, "Account Removed");
                }
                sess->release();
            }
            ua_unregister(u);
            ua_destroy(u);
        }
        account->set_reg_state(SIP_REG_STATE_UNREGISTERED);
        dispatch_account_reg_state(account, SIP_REG_STATE_UNREGISTERED, 0, "Account Removed");
        account->release(); // engine ref
    });
    if (!posted) {
        account->set_reg_state(SIP_REG_STATE_UNREGISTERED);
        account->release(); // engine ref
        return EIO;
    }
    return 0;
}

int Engine::set_default_account(sip_account_t *account)
{
    if (!account) return EINVAL;
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    auto it = std::find(m_accounts.begin(), m_accounts.end(), account);
    if (it == m_accounts.end() || account->is_removed()) {
        return EINVAL;
    }
    m_default_account = account;
    return 0;
}

sip_account_t *Engine::get_default_account() const
{
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    return m_default_account;
}

size_t Engine::get_account_count() const
{
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    return m_accounts.size();
}

size_t Engine::get_accounts(sip_account_t **out_accounts, size_t max_count) const
{
    std::lock_guard<std::mutex> lock(m_accounts_mutex);
    if (!out_accounts) {
        return m_accounts.size();
    }
    size_t count = std::min(max_count, m_accounts.size());
    for (size_t i = 0; i < count; ++i) {
        out_accounts[i] = m_accounts[i];
    }
    return count;
}

int Engine::reregister_account(sip_account_t *account)
{
    if (!is_account_active(account)) return EINVAL;
    account->retain();
    bool posted = post_task([this, account]() {
        struct ua *u = account->raw_ua();
        if (u != nullptr && !account->is_removed()) {
            int rc = ua_register(u);
            if (rc != 0) {
                account->set_reg_state(SIP_REG_STATE_FAILED);
                dispatch_account_reg_state(account, SIP_REG_STATE_FAILED, rc, "Registration submission failed");
                if (get_default_account() == account) {
                    dispatch_reg_state(SIP_REG_STATE_FAILED, rc, "Registration submission failed");
                }
            }
        }
        account->release();
    });
    if (!posted) {
        account->release();
        return EIO;
    }
    return 0;
}

int Engine::unregister_account(sip_account_t *account)
{
    if (!is_account_active(account)) return EINVAL;
    account->set_reg_state(SIP_REG_STATE_UNREGISTERED);
    account->retain();
    bool posted = post_task([this, account]() {
        struct ua *u = account->raw_ua();
        if (u != nullptr) {
            ua_unregister(u);
        } else {
            dispatch_account_reg_state(account, SIP_REG_STATE_UNREGISTERED, 0, "Unregistered");
            if (get_default_account() == account) {
                dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, 0, "Unregistered");
            }
        }
        account->release();
    });
    if (!posted) {
        account->release();
        return EIO;
    }
    return 0;
}

int Engine::register_account(const AccountConfig &config)
{
    if (config.user.empty() || config.domain.empty() || config.port < 0 || config.port > 65535) {
        return EINVAL;
    }

    int effective_max_calls = config.max_calls == 0 ? 4 : config.max_calls;
    if (effective_max_calls < 1) effective_max_calls = 1;
    if (effective_max_calls > 8) effective_max_calls = 8;
    m_max_calls.store(effective_max_calls);

    std::string lower_transport;
    std::string transport_param;
    int port = 0;
    std::string aor = build_canonical_aor(config, lower_transport, transport_param, port);

    sip_account_t *old_default = nullptr;
    sip_account_t *new_default = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_accounts_mutex);
        old_default = m_default_account;
        if (old_default) {
            auto it = std::find(m_accounts.begin(), m_accounts.end(), old_default);
            if (it != m_accounts.end()) {
                m_accounts.erase(it);
            }
            old_default->mark_removed();
        }
        uint64_t id = m_next_account_id.fetch_add(1);
        new_default = new sip_account_t(m_owner, this, id, aor, config.user, config.domain, lower_transport);
        // Engine owns the sole reference for legacy register_account calls.
        m_accounts.insert(m_accounts.begin(), new_default);
        m_default_account = new_default;
    }

    new_default->retain();
    bool posted = post_task([this, old_default, new_default, cfg = config, effective_max_calls]() {
        if (old_default) {
            struct ua *old_ua = old_default->detach_raw_ua();
            if (old_ua != nullptr) {
                ua_destroy(old_ua);
            }
            old_default->release();
        }
        (void)start_account_ua_on_loop(new_default, cfg, effective_max_calls);
        new_default->release();
    });

    if (!posted) {
        if (old_default) {
            old_default->release();
        }
        new_default->release();
        return EIO;
    }
    return 0;
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
    bool posted = post_task([this, re_register]() {
        bool has_wss = false;
        std::vector<sip_account_t *> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_accounts_mutex);
            for (auto *acc : m_accounts) {
                if (acc && !acc->is_removed()) {
                    if (acc->transport_str() == "wss") has_wss = true;
                    acc->retain();
                    snapshot.push_back(acc);
                }
            }
        }

        if (has_wss) {
            (void)uag_reset_transp(false, false);
            (void)add_wss_transport_on_loop();
            if (re_register) {
                for (auto *acc : snapshot) {
                    struct ua *u = acc->raw_ua();
                    if (u != nullptr) {
                        (void)ua_register(u);
                    }
                }
            }
        } else {
            (void)uag_reset_transp(re_register, true);
            (void)add_wss_transport_on_loop();
        }

        for (auto *acc : snapshot) {
            acc->release();
        }
    });
    return posted ? 0 : EIO;
}

int Engine::unregister_account()
{
    sip_account_t *def = get_default_account();
    if (def) {
        return unregister_account(def);
    }
    bool posted = post_task([this]() {
        dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, 0, "Unregistered");
    });
    return posted ? 0 : EIO;
}

bool Engine::is_registered() const
{
    sip_account_t *def = get_default_account();
    return def != nullptr && def->is_registered();
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
            struct ua *bua = bevent_get_ua(event);
            sip_account_t *acc = find_account_by_ua(bua);
            std::string reason = extract_reason("Registering");
            if (acc) {
                acc->set_reg_state(SIP_REG_STATE_REGISTERING);
                dispatch_account_reg_state(acc, SIP_REG_STATE_REGISTERING, 0, reason.c_str());
            }
            if (!acc || acc == get_default_account()) {
                dispatch_reg_state(SIP_REG_STATE_REGISTERING, 0, reason.c_str());
            }
            break;
        }
        case BEVENT_REGISTER_OK: {
            struct ua *bua = bevent_get_ua(event);
            sip_account_t *acc = find_account_by_ua(bua);
            int scode = msg ? static_cast<int>(msg->scode) : 200;
            std::string reason = extract_reason("OK");
            if (acc) {
                acc->set_reg_state(SIP_REG_STATE_REGISTERED);
                dispatch_account_reg_state(acc, SIP_REG_STATE_REGISTERED, scode, reason.c_str());
            }
            if (!acc || acc == get_default_account()) {
                dispatch_reg_state(SIP_REG_STATE_REGISTERED, scode, reason.c_str());
            }
            break;
        }
        case BEVENT_REGISTER_FAIL: {
            struct ua *bua = bevent_get_ua(event);
            sip_account_t *acc = find_account_by_ua(bua);
            int scode = msg ? static_cast<int>(msg->scode) : 401;
            std::string reason = extract_reason("Registration Failed");
            if (acc) {
                acc->set_reg_state(SIP_REG_STATE_FAILED);
                dispatch_account_reg_state(acc, SIP_REG_STATE_FAILED, scode, reason.c_str());
            }
            if (!acc || acc == get_default_account()) {
                dispatch_reg_state(SIP_REG_STATE_FAILED, scode, reason.c_str());
            }
            break;
        }
        case BEVENT_UNREGISTERING: {
            struct ua *bua = bevent_get_ua(event);
            sip_account_t *acc = find_account_by_ua(bua);
            int scode = msg ? static_cast<int>(msg->scode) : 0;
            std::string reason = extract_reason("Unregistered");
            if (acc) {
                acc->set_reg_state(SIP_REG_STATE_UNREGISTERED);
                dispatch_account_reg_state(acc, SIP_REG_STATE_UNREGISTERED, scode, reason.c_str());
            }
            if (!acc || acc == get_default_account()) {
                dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, scode, reason.c_str());
            }
            break;
        }
        case BEVENT_CALL_INCOMING: {
            if (bcall) {
                sip_account_t *acc = find_account_by_call(bcall);
                if (!acc) {
                    acc = find_account_by_ua(bevent_get_ua(event));
                }
                if (!acc && msg) {
                    acc = find_account_by_ua(uag_find_msg(msg));
                }
                if (!acc) {
                    acc = get_default_account();
                }
                const char *peer_uri = call_peeruri(bcall);
                bool with_video = call_has_video(bcall);
                sip_call_t *sess = find_or_create_call_session(bcall, peer_uri, false, with_video);
                sess->attach_raw_call(bcall);
                if (acc) {
                    sess->set_account(acc);
                }
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
                sess->start_stats_timer();
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
                    sess->set_hold_confirmed(true);
                    sess->stop_hold_check_timer();
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
                    sess->set_hold_confirmed(false);
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
                    sess->stop_stats_timer();
                    uint32_t dur = call_duration(bcall);
                    sess->latch_media_stats();
                    sess->set_closed(dur);
                    int scode = (msg && msg->scode > 0) ? static_cast<int>(msg->scode) : static_cast<int>(call_scode(bcall));
                    if (text && (strstr(text, "transfer") || strstr(text, "transferred") || strstr(text, "transfered"))) {
                        dispatch_transfer_status(sess, 200, "Call transferred");
                        sip_call_t *consult = sess->take_consult_call();
                        if (consult) {
                            if (!consult->is_closed()) {
                                consult->stop_stats_timer();
                                struct call *cc = consult->raw_call();
                                struct ua *cua = resolve_call_ua(consult);
                                if (cua != nullptr && cc != nullptr) {
                                    ua_hangup(cua, cc, 0, "Transfer completed");
                                } else {
                                    consult->set_closed(0);
                                    dispatch_call_state(consult, SIP_CALL_STATE_CLOSED, 200, "Transfer completed");
                                }
                            }
                            consult->release();
                        }
                    }
                    if (sess->is_recording()) {
                        stop_recording(sess);
                    }
                    if (sess->is_in_conference()) {
                        remove_from_conference_on_loop(sess, false);
                    }
                    remove_call_session(bcall);
                    dispatch_call_state(sess, SIP_CALL_STATE_CLOSED, scode, extract_reason("Call Closed").c_str());
                }
            }
            release_audio_session_if_idle();
            break;
        }
        case BEVENT_CALL_TRANSFER: {
            if (bcall && text) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    sess->set_pending_transfer_uri(text);
                    bool has_replaces = (strstr(text, "Replaces=") != nullptr || strstr(text, "replaces=") != nullptr);
                    dispatch_transfer_request(sess, text, has_replaces);
                }
            }
            break;
        }
        case BEVENT_CALL_TRANSFER_FAILED: {
            if (bcall) {
                sip_call_t *sess = find_call_session(bcall);
                if (sess) {
                    int code = 500;
                    if (text) {
                        int parsed = 0;
                        if (sscanf(text, "%d", &parsed) == 1 && parsed >= 300 && parsed < 700) {
                            code = parsed;
                        }
                    }
                    dispatch_transfer_status(sess, code, text ? text : "Transfer Failed");
                }
            }
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
                if (sess) {
                    sess->stop_hold_check_timer();
                    if (sess->has_video()) {
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
            if (!bua && msg) {
                bua = uag_find_msg(msg);
            }
            if (!bua) {
                bua = resolve_call_ua(nullptr);
            }
            if (bua && msg) {
                int open_calls = 0;
                {
                    std::lock_guard<std::mutex> lock(m_calls_mutex);
                    for (auto *sess : m_all_calls) {
                        if (sess && !sess->is_closed()) {
                            open_calls++;
                        }
                    }
                }
                if (open_calls >= m_max_calls.load()) {
                    std::string caller_uri;
                    if (msg->from.auri.p && msg->from.auri.l > 0) {
                        caller_uri.assign(msg->from.auri.p, msg->from.auri.l);
                    }
                    (void)sip_treply(nullptr, uag_sip(), msg, 486, "Busy Here");
                    dispatch_call_rejected_busy(caller_uri.c_str());
                    break;
                }
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

struct AccountRegStateTaskArgs {
    sip_client_t *client{nullptr};
    sip_account_t *account{nullptr};
    sip_reg_state_t state{SIP_REG_STATE_UNREGISTERED};
    int code{0};
    std::string reason;
    void (*on_account_reg_state)(sip_client_t *, sip_account_t *, sip_reg_state_t, int, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_account_reg_state_task(void *arg)
{
    auto *args = static_cast<AccountRegStateTaskArgs *>(arg);
    if (args) {
        if (args->on_account_reg_state) {
            args->on_account_reg_state(args->client, args->account, args->state, args->code,
                                       args->reason.c_str(), args->user_data);
        }
        if (args->account) {
            args->account->release();
        }
        delete args;
    }
}

void Engine::dispatch_account_reg_state(sip_account_t *account, sip_reg_state_t state, int code, const char *reason)
{
    if (!account) return;
    sip_account_callbacks_t cbs;
    {
        std::lock_guard<std::mutex> lock(m_account_cb_mutex);
        cbs = m_account_callbacks;
    }
    if (!cbs.on_account_reg_state) return;

    account->retain();
    auto *args = new AccountRegStateTaskArgs{
        m_owner,
        account,
        state,
        code,
        reason ? reason : "",
        cbs.on_account_reg_state,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_account_reg_state_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_account_reg_state_task);
#else
    run_account_reg_state_task(args);
#endif
}

sip_call_t *Engine::make_call(const char *target_uri, bool with_video)
{
    sip_call_t *call = nullptr;
    int rc = dial_call(target_uri, with_video, &call);
    if (rc != 0) {
        errno = rc;
        return nullptr;
    }
    return call;
}

int Engine::dial_call(const char *target_uri, bool with_video, sip_call_t **out_call)
{
    return dial_call_on_account(get_default_account(), target_uri, with_video, out_call);
}

int Engine::dial_call_on_account(sip_account_t *account, const char *target_uri, bool with_video, sip_call_t **out_call)
{
    if (out_call) {
        *out_call = nullptr;
    }
    if (!target_uri || std::strlen(target_uri) == 0 || !out_call) {
        return EINVAL;
    }
    if (!m_started.load()) {
        return EIO;
    }
    if (account != nullptr && !is_account_active(account)) {
        return EINVAL;
    }

    std::string uri_str = target_uri;
    sip_call_t *call_session = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        int open_calls = 0;
        for (auto *sess : m_all_calls) {
            if (sess && !sess->is_closed()) {
                open_calls++;
            }
        }
        if (open_calls >= m_max_calls.load()) {
            return EBUSY;
        }
        call_session = new sip_call_t(m_owner, this, nullptr, uri_str.c_str(), true, with_video);
        if (account) {
            call_session->set_account(account);
        }
        m_all_calls.push_back(call_session);
    }

    prepare_audio_session();

    call_session->retain();
    if (account) account->retain();
    bool posted = post_task([this, account, call_session, uri_str, with_video]() {
        struct ua *bua = account ? account->raw_ua() : resolve_call_ua(nullptr);
        if (!bua) {
            call_session->set_closed(0);
            dispatch_call_state(call_session, SIP_CALL_STATE_CLOSED, EINVAL, "No UA available");
            release_audio_session_if_idle();
            if (account) account->release();
            call_session->release();
            return;
        }

        auto_hold_other_calls(call_session);

        struct call *baresip_call = nullptr;
        enum vidmode vmode = with_video ? VIDMODE_ON : VIDMODE_OFF;
        int rc = ua_connect(bua, &baresip_call, nullptr, uri_str.c_str(), vmode);
        if (rc != 0 || baresip_call == nullptr) {
            call_session->set_closed(0);
            dispatch_call_state(call_session, SIP_CALL_STATE_CLOSED, rc != 0 ? rc : EIO, "Connect failed");
            release_audio_session_if_idle();
            if (account) account->release();
            call_session->release();
            return;
        }

        call_session->attach_raw_call(baresip_call);
        dispatch_call_state(call_session, SIP_CALL_STATE_CONNECTING, 100, "Connecting");
        if (account) account->release();
        call_session->release();
    });

    if (!posted) {
        if (account) account->release();
        call_session->set_closed(0);
        destroy_call(call_session);
        call_session->release();
        release_audio_session_if_idle();
        return EIO;
    }

    *out_call = call_session;
    return 0;
}

int Engine::answer_call(sip_call_t *call, bool with_video)
{
    if (!call) return EINVAL;

    prepare_audio_session();

    call->retain();
    bool posted = post_task([this, call, with_video]() {
        struct call *c = call->raw_call();
        struct ua *bua = resolve_call_ua(call);
        if (bua != nullptr && c != nullptr) {
            auto_hold_other_calls(call);
            enum vidmode vmode = with_video ? VIDMODE_ON : VIDMODE_OFF;
            (void)ua_answer(bua, c, vmode);
        } else {
            release_audio_session_if_idle();
        }
        call->release();
    });

    if (!posted) {
        release_audio_session_if_idle();
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::hangup_call(sip_call_t *call, int code, const char *reason)
{
    if (!call) return EINVAL;

    std::string reason_str = reason ? reason : "";
    uint16_t scode = code > 0 ? static_cast<uint16_t>(code) : 0;

    call->retain();
    bool posted = post_task([this, call, scode, reason_str]() {
        call->stop_stats_timer();
        if (call->is_recording()) {
            stop_recording(call);
        }
        struct call *c = call->raw_call();
        struct ua *bua = resolve_call_ua(call);
        if (bua != nullptr && c != nullptr) {
            call->latch_media_stats();
            ua_hangup(bua, c, scode, reason_str.empty() ? nullptr : reason_str.c_str());
        } else {
            call->set_closed(0);
            dispatch_call_state(call, SIP_CALL_STATE_CLOSED, scode, reason_str.c_str());
            release_audio_session_if_idle();
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

void Engine::auto_hold_calls_outside_set(const std::vector<sip_call_t *> &exempt_calls)
{
    std::vector<sip_call_t *> to_hold;
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_all_calls) {
            if (!sess || sess->is_closed() || sess->is_on_hold()) {
                continue;
            }
            if (std::find(exempt_calls.begin(), exempt_calls.end(), sess) != exempt_calls.end()) {
                continue;
            }
            struct call *c = sess->raw_call();
            if (c != nullptr) {
                enum call_state st = call_state(c);
                if (st == CALL_STATE_ESTABLISHED || st == CALL_STATE_TRANSFER) {
                    sess->retain();
                    to_hold.push_back(sess);
                }
            }
        }
    }
    for (auto *sess : to_hold) {
        hold_single_call_on_loop(sess, true);
        sess->release();
    }
    sync_conference_mixer_on_loop();
}

void Engine::auto_hold_other_calls(sip_call_t *except_call)
{
    std::vector<sip_call_t *> exempt;
    if (except_call) {
        exempt.push_back(except_call);
    }
    auto_hold_calls_outside_set(exempt);
}

void Engine::trigger_hold_fallback_on_loop(sip_call_t *call, int code)
{
    if (!call || call->is_closed()) {
        return;
    }
    struct call *c = call->raw_call();
    if (c != nullptr) {
        struct audio *au = call_audio(c);
        if (au != nullptr) {
            audio_mute(au, true);
            audio_stop(au);
        }
    }
    if (!call->is_muted()) {
        call->set_muted(true);
        call->set_hold_fallback_muted(true);
    }
    call->set_on_hold(true);
    dispatch_call_state(call, SIP_CALL_STATE_HOLD, code, "Hold Fallback");
    dispatch_call_media(call, 0, 0, true, true);
}

void Engine::evaluate_pending_hold_on_loop(sip_call_t *call)
{
    if (!call || call->is_closed() || !call->is_hold_reinvite_pending()) {
        return;
    }
    if (call->is_hold_confirmed()) {
        call->stop_hold_check_timer();
        return;
    }
    struct call *c = call->raw_call();
    if (!c) {
        call->stop_hold_check_timer();
        return;
    }
    int ticks = call->increment_hold_check_ticks();
    enum sdp_neg_state neg = call_sdp_neg_state(c);
    if (neg != SDP_NEG_LOCAL_OFFER) {
        call->stop_hold_check_timer();
        trigger_hold_fallback_on_loop(call, 488);
    } else if (ticks >= 50) {
        call->stop_hold_check_timer();
        trigger_hold_fallback_on_loop(call, 408);
    }
}

void Engine::hold_single_call_on_loop(sip_call_t *call, bool hold)
{
    if (!call || call->is_closed()) return;
    struct call *c = call->raw_call();
    if (c == nullptr) return;

    if (!hold) {
        call->stop_hold_check_timer();
        call->set_hold_confirmed(false);
        if (call->consume_hold_fallback_muted()) {
            call->set_muted(false);
        }
        int rc = call_hold(c, false);
        (void)rc;
        struct audio *au = call_audio(c);
        if (au != nullptr) {
            bool effective_au_mute = call->is_in_conference() ? false : call->is_muted();
            audio_mute(au, effective_au_mute);
            if (!audio_started(au)) {
                (void)audio_update(au);
            }
        }
        call->set_on_hold(false);
        dispatch_call_state(call, SIP_CALL_STATE_ESTABLISHED, 200, "Resumed");
        dispatch_call_media(call, 0, 0, false, call->is_muted());
        return;
    }

    call->set_hold_confirmed(false);
    int rc = call_hold(c, true);
    if (rc != 0) {
        trigger_hold_fallback_on_loop(call, rc);
        return;
    }

    call->set_on_hold(true);
    if (!call->is_hold_confirmed()) {
        call->start_hold_check_timer();
    }
    dispatch_call_state(call, SIP_CALL_STATE_HOLD, 200, "Hold");
    dispatch_call_media(call, 0, 0, true, call->is_muted());
}

void Engine::hold_call_on_loop(sip_call_t *call, bool hold)
{
    if (!call || call->is_closed()) return;

    if (call->is_in_conference()) {
        std::vector<sip_call_t *> group;
        {
            std::lock_guard<std::mutex> lock(m_calls_mutex);
            for (auto *sess : m_conference_calls) {
                if (sess && !sess->is_closed()) {
                    sess->retain();
                    group.push_back(sess);
                }
            }
        }
        if (!hold) {
            auto_hold_calls_outside_set(group);
            for (auto *sess : group) {
                hold_single_call_on_loop(sess, false);
                sess->release();
            }
        } else {
            for (auto *sess : group) {
                hold_single_call_on_loop(sess, true);
                sess->release();
            }
        }
        sync_conference_mixer_on_loop();
        return;
    }

    if (!hold) {
        auto_hold_other_calls(call);
    }
    hold_single_call_on_loop(call, hold);
}

int Engine::hold_call(sip_call_t *call, bool hold)
{
    if (!call) return EINVAL;

    call->retain();
    bool posted = post_task([this, call, hold]() {
        hold_call_on_loop(call, hold);
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::mute_call(sip_call_t *call, bool muted)
{
    if (!call) return EINVAL;

    call->retain();
    bool posted = post_task([this, call, muted]() {
        if (call->is_in_conference()) {
            if (m_media_manager) {
                m_media_manager->conference_mixer().set_local_muted(muted);
            }
            std::vector<sip_call_t *> group;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                for (auto *sess : m_conference_calls) {
                    if (sess && !sess->is_closed()) {
                        sess->retain();
                        group.push_back(sess);
                    }
                }
            }
            for (auto *sess : group) {
                struct call *c = sess->raw_call();
                if (c != nullptr) {
                    struct audio *au = call_audio(c);
                    if (au != nullptr) {
                        audio_mute(au, false);
                        if (m_media_manager && sess->is_recording()) {
                            m_media_manager->call_recorder().set_local_muted(
                                reinterpret_cast<uint64_t>(au), muted);
                        }
                    }
                }
                sess->set_hold_fallback_muted(false);
                sess->set_muted(muted);
                dispatch_call_media(sess, 0, 0, sess->is_on_hold(), muted);
                sess->release();
            }
            call->release();
            return;
        }
        struct call *c = call->raw_call();
        if (c != nullptr) {
            struct audio *au = call_audio(c);
            if (au != nullptr) {
                audio_mute(au, muted);
                if (m_media_manager && call->is_recording()) {
                    m_media_manager->call_recorder().set_local_muted(
                        reinterpret_cast<uint64_t>(au), muted);
                }
            }
            call->set_hold_fallback_muted(false);
            call->set_muted(muted);
            dispatch_call_media(call, 0, 0, call->is_on_hold(), muted);
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::send_dtmf(sip_call_t *call, const char *digits)
{
    if (!call || !digits || std::strlen(digits) == 0) return EINVAL;

    std::string digits_str = digits;

    call->retain();
    bool posted = post_task([call, digits_str]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            for (char ch : digits_str) {
                (void)call_send_digit(c, ch);
            }
            (void)call_send_digit(c, KEYCODE_REL);
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::transfer_call(sip_call_t *call, const char *target_uri)
{
    if (!call || !target_uri || target_uri[0] == '\0') return EINVAL;

    std::string uri_str = target_uri;
    call->retain();
    bool posted = post_task([this, call, uri_str]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            int err = call_transfer(c, uri_str.c_str());
            if (err != 0) {
                dispatch_transfer_status(call, 500, "call_transfer failed");
            }
        } else {
            dispatch_transfer_status(call, 500, "Call object invalid");
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::attended_transfer_call(sip_call_t *call, sip_call_t *consult_call)
{
    if (!call || !consult_call) return EINVAL;

    call->retain();
    consult_call->retain();
    bool posted = post_task([this, call, consult_call]() {
        struct call *c = call->raw_call();
        struct call *sc = consult_call->raw_call();
        if (c != nullptr && sc != nullptr) {
            call->set_consult_call(consult_call);
            int err = call_replace_transfer(c, sc);
            if (err != 0) {
                call->set_consult_call(nullptr);
                dispatch_transfer_status(call, 500, "call_replace_transfer failed");
            }
        } else {
            dispatch_transfer_status(call, 500, "Call objects invalid");
        }
        consult_call->release();
        call->release();
    });

    if (!posted) {
        consult_call->release();
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::accept_transfer(sip_call_t *call, bool accept)
{
    if (!call) return EINVAL;

    call->retain();
    bool posted = post_task([this, call, accept]() {
        struct call *c = call->raw_call();
        if (c != nullptr) {
            if (!accept) {
                (void)call_notify_sipfrag(c, 603, "Decline");
                call->set_pending_transfer_uri("");
            } else {
                std::string target = call->pending_transfer_uri();
                if (!target.empty()) {
                    std::string clean_uri = target;
                    if (!clean_uri.empty() && clean_uri.front() == '<' && clean_uri.back() == '>') {
                        clean_uri = clean_uri.substr(1, clean_uri.size() - 2);
                    }
                    struct call *call2 = nullptr;
                    struct ua *bua = resolve_call_ua(call);
                    enum vidmode vmode = call_has_video(c) ? VIDMODE_ON : VIDMODE_OFF;
                    int err = bua ? ua_call_alloc(&call2, bua, vmode, nullptr, c, call_localuri(c), true) : EINVAL;
                    if (err == 0 && call2 != nullptr) {
                        struct pl pl;
                        pl_set_str(&pl, clean_uri.c_str());
                        err = call_connect(call2, &pl);
                        if (err == 0) {
                            sip_call_t *sess2 = find_or_create_call_session(call2, clean_uri.c_str(), true, call_has_video(call2));
                            if (sess2 && call->account()) {
                                sess2->set_account(call->account());
                            }
                        } else {
                            (void)call_notify_sipfrag(c, 500, "Connect Error");
                            mem_deref(call2);
                        }
                    } else {
                        (void)call_notify_sipfrag(c, 500, "Alloc Error");
                    }
                } else {
                    (void)call_notify_sipfrag(c, 500, "No Pending URI");
                }
            }
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }

    return 0;
}

int Engine::set_client_mic_muted(bool muted)
{
    m_mic_muted = muted;
    post_task([this, muted]() {
        if (m_media_manager) {
            m_media_manager->conference_mixer().set_local_muted(muted);
        }
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_all_calls) {
            if (sess && sess->raw_call() != nullptr) {
                struct audio *au = call_audio(sess->raw_call());
                if (au != nullptr) {
                    bool effective_au_mute = sess->is_in_conference() ? false : muted;
                    audio_mute(au, effective_au_mute);
                    if (m_media_manager && sess->is_recording()) {
                        m_media_manager->call_recorder().set_local_muted(
                            reinterpret_cast<uint64_t>(au), muted);
                    }
                }
                sess->set_muted(muted);
                dispatch_call_media(sess, 0, 0, sess->is_on_hold(), muted);
            }
        }
    });
    return 0;
}

void Engine::sync_conference_mixer_on_loop()
{
    if (!m_media_manager) return;
    auto &mixer = m_media_manager->conference_mixer();

    std::vector<sip_call_t *> active_legs;
    bool any_muted = m_mic_muted.load();
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_conference_calls) {
            if (sess && !sess->is_closed() && !sess->is_on_hold() && sess->raw_call() != nullptr) {
                active_legs.push_back(sess);
                if (sess->is_muted()) {
                    any_muted = true;
                }
            }
        }
    }

    if (active_legs.size() < 2) {
        mixer.clear();
        return;
    }

    mixer.set_local_muted(any_muted);
    for (auto *sess : active_legs) {
        struct call *c = sess->raw_call();
        if (!c) continue;
        struct audio *au = call_audio(c);
        if (au) {
            audio_set_conference(au, true);
            audio_mute(au, false);
            mixer.add_leg(reinterpret_cast<uint64_t>(au));
        }
        struct video *vid = call_video(c);
        if (vid) {
            video_stop(vid);
        }
    }
}

void Engine::remove_from_conference_on_loop(sip_call_t *call, bool hold_removed)
{
    if (!call) return;

    std::vector<sip_call_t *> snapshot;
    sip_call_t *dissolved_rem = nullptr;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        auto it = std::find(m_conference_calls.begin(), m_conference_calls.end(), call);
        if (it != m_conference_calls.end()) {
            m_conference_calls.erase(it);
            call->set_in_conference(false);
            changed = true;
        }
        if (m_conference_calls.size() == 1) {
            dissolved_rem = m_conference_calls.front();
            m_conference_calls.clear();
            if (dissolved_rem) {
                dissolved_rem->set_in_conference(false);
                dissolved_rem->retain();
            }
            changed = true;
        }
        snapshot = m_conference_calls;
    }

    if (!changed) return;

    if (m_media_manager) {
        struct call *c = call->raw_call();
        if (c) {
            struct audio *au = call_audio(c);
            if (au) {
                m_media_manager->conference_mixer().remove_leg(reinterpret_cast<uint64_t>(au));
                audio_set_conference(au, false);
                audio_mute(au, call->is_muted());
            }
        }
    }

    if (dissolved_rem) {
        if (m_media_manager) {
            m_media_manager->conference_mixer().clear();
        }
        struct call *rc = dissolved_rem->raw_call();
        if (rc) {
            struct audio *au = call_audio(rc);
            if (au) {
                audio_set_conference(au, false);
                audio_mute(au, dissolved_rem->is_muted());
            }
        }
        dissolved_rem->release();
    } else {
        sync_conference_mixer_on_loop();
    }

    if (hold_removed && !call->is_closed()) {
        hold_single_call_on_loop(call, true);
    }

    dispatch_conference_changed(snapshot);
}

int Engine::merge_calls(sip_call_t *call_a, sip_call_t *call_b)
{
    if (!call_a || !call_b || call_a == call_b) return EINVAL;
    if (call_a->is_closed() || call_b->is_closed()) return EINVAL;
    if (call_a->engine() != this || call_b->engine() != this) return EINVAL;

    call_a->retain();
    call_b->retain();
    bool posted = post_task([this, call_a, call_b]() {
        if (!call_a->is_closed() && !call_b->is_closed() &&
            call_a->raw_call() != nullptr && call_b->raw_call() != nullptr) {
            std::vector<sip_call_t *> group;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                auto add_unique = [&](sip_call_t *c) {
                    if (std::find(m_conference_calls.begin(), m_conference_calls.end(), c) == m_conference_calls.end()) {
                        m_conference_calls.push_back(c);
                    }
                    c->set_in_conference(true);
                };
                add_unique(call_a);
                add_unique(call_b);
                for (auto *sess : m_conference_calls) {
                    if (sess && !sess->is_closed()) {
                        sess->retain();
                        group.push_back(sess);
                    }
                }
            }

            auto_hold_calls_outside_set(group);
            for (auto *sess : group) {
                if (sess->is_on_hold()) {
                    hold_single_call_on_loop(sess, false);
                }
            }
            sync_conference_mixer_on_loop();

            std::vector<sip_call_t *> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                snapshot = m_conference_calls;
            }
            dispatch_conference_changed(snapshot);

            for (auto *sess : group) {
                sess->release();
            }
        }
        call_a->release();
        call_b->release();
    });

    if (!posted) {
        call_a->release();
        call_b->release();
        return EIO;
    }
    return 0;
}

int Engine::add_to_conference(sip_call_t *call)
{
    if (!call || call->is_closed() || call->engine() != this) return EINVAL;
    {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        if (m_conference_calls.size() < 2) {
            return EINVAL;
        }
    }

    call->retain();
    bool posted = post_task([this, call]() {
        if (!call->is_closed() && call->raw_call() != nullptr) {
            std::vector<sip_call_t *> group;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                if (m_conference_calls.size() < 2) {
                    call->release();
                    return;
                }
                if (std::find(m_conference_calls.begin(), m_conference_calls.end(), call) == m_conference_calls.end()) {
                    m_conference_calls.push_back(call);
                }
                call->set_in_conference(true);
                for (auto *sess : m_conference_calls) {
                    if (sess && !sess->is_closed()) {
                        sess->retain();
                        group.push_back(sess);
                    }
                }
            }

            auto_hold_calls_outside_set(group);
            for (auto *sess : group) {
                if (sess->is_on_hold()) {
                    hold_single_call_on_loop(sess, false);
                }
            }
            sync_conference_mixer_on_loop();

            std::vector<sip_call_t *> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_calls_mutex);
                snapshot = m_conference_calls;
            }
            dispatch_conference_changed(snapshot);

            for (auto *sess : group) {
                sess->release();
            }
        }
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }
    return 0;
}

int Engine::split_call(sip_call_t *call)
{
    if (!call || call->engine() != this || !call->is_in_conference()) return EINVAL;

    call->retain();
    bool posted = post_task([this, call]() {
        remove_from_conference_on_loop(call, true);
        call->release();
    });

    if (!posted) {
        call->release();
        return EIO;
    }
    return 0;
}

int Engine::leave_conference()
{
    bool posted = post_task([this]() {
        std::vector<sip_call_t *> group;
        {
            std::lock_guard<std::mutex> lock(m_calls_mutex);
            for (auto *sess : m_conference_calls) {
                if (sess) {
                    sess->set_in_conference(false);
                    sess->retain();
                    group.push_back(sess);
                }
            }
            m_conference_calls.clear();
        }
        if (m_media_manager) {
            m_media_manager->conference_mixer().clear();
        }
        for (auto *sess : group) {
            struct call *c = sess->raw_call();
            if (c) {
                struct audio *au = call_audio(c);
                if (au) {
                    audio_set_conference(au, false);
                }
                struct ua *bua = resolve_call_ua(sess);
                if (bua && !sess->is_closed()) {
                    sess->latch_media_stats();
                    ua_hangup(bua, c, 0, "Conference Ended");
                }
            }
            sess->release();
        }
        dispatch_conference_changed({});
    });
    return posted ? 0 : EIO;
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

    std::string s_mod = src_mod ? src_mod : (cfg ? cfg->audio.src_mod : "");
    std::string s_dev = src_dev ? src_dev : (cfg ? cfg->audio.src_dev : "");
    std::string p_mod = play_mod ? play_mod : (cfg ? cfg->audio.play_mod : "");
    std::string p_dev = play_dev ? play_dev : (cfg ? cfg->audio.play_dev : "");
    bool has_src = (src_mod != nullptr || src_dev != nullptr);
    bool has_play = (play_mod != nullptr || play_dev != nullptr);

    post_task([this, s_mod, s_dev, p_mod, p_dev, has_src, has_play]() {
        std::lock_guard<std::mutex> lock(m_calls_mutex);
        for (auto *sess : m_all_calls) {
            if (!sess || sess->raw_call() == nullptr || sess->is_closed()) continue;
            struct audio *au = call_audio(sess->raw_call());
            if (!au) continue;
            if (has_src) {
                audio_set_source(au, s_mod.empty() ? nullptr : s_mod.c_str(),
                                 s_dev.c_str());
            }
            if (has_play) {
                audio_set_player(au, p_mod.empty() ? nullptr : p_mod.c_str(),
                                 p_dev.c_str());
            }
        }
    });

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
                if (sess && !sess->is_closed() && sess->raw_call() == nullptr && sess->is_outgoing() &&
                    remote_uri && std::strcmp(sess->get_remote_uri(), remote_uri) == 0) {
                    sess->attach_raw_call(c);
                    return sess;
                }
            }
            for (auto *sess : m_all_calls) {
                if (sess && !sess->is_closed() && sess->raw_call() == nullptr && sess->is_outgoing()) {
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

struct CallRejectedBusyTaskArgs {
    sip_client_t *client{nullptr};
    std::string caller_uri;
    void (*on_call_rejected_busy)(sip_client_t *, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_call_rejected_busy_task(void *arg)
{
    auto *args = static_cast<CallRejectedBusyTaskArgs *>(arg);
    if (args) {
        if (args->on_call_rejected_busy) {
            args->on_call_rejected_busy(args->client, args->caller_uri.c_str(), args->user_data);
        }
        delete args;
    }
}

void Engine::dispatch_call_rejected_busy(const char *caller_uri)
{
    if (!m_callbacks.on_call_rejected_busy) return;

    auto *args = new CallRejectedBusyTaskArgs{
        m_owner,
        caller_uri ? caller_uri : "",
        m_callbacks.on_call_rejected_busy,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_call_rejected_busy_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_call_rejected_busy_task);
#else
    run_call_rejected_busy_task(args);
#endif
}

struct ConferenceChangedTaskArgs {
    sip_client_t *client{nullptr};
    std::vector<sip_call_t *> calls;
    void (*cb)(sip_client_t *, sip_call_t *const *, size_t, void *){nullptr};
    void *user_data{nullptr};
};

static void run_conference_changed_task(void *arg)
{
    auto *args = static_cast<ConferenceChangedTaskArgs *>(arg);
    if (args->cb) {
        args->cb(args->client,
                 args->calls.empty() ? nullptr : args->calls.data(),
                 args->calls.size(),
                 args->user_data);
    }
    for (auto *c : args->calls) {
        if (c) c->release();
    }
    delete args;
}

void Engine::dispatch_conference_changed(const std::vector<sip_call_t *> &calls)
{
    if (!m_callbacks.on_conference_changed) return;

    auto *args = new ConferenceChangedTaskArgs{};
    args->client = m_owner;
    args->cb = m_callbacks.on_conference_changed;
    args->user_data = m_user_data;
    args->calls.reserve(calls.size());
    for (auto *c : calls) {
        if (c) {
            c->retain();
            args->calls.push_back(c);
        }
    }

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_conference_changed_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_conference_changed_task);
#else
    run_conference_changed_task(args);
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

struct CallStatsTaskArgs {
    sip_call_t *call{nullptr};
    sip_call_stats_t stats{};
    void (*callback)(sip_call_t *call, const sip_call_stats_t *stats, void *user_data){nullptr};
    void *user_data{nullptr};
};

static void run_call_stats_task(void *arg)
{
    auto *args = static_cast<CallStatsTaskArgs *>(arg);
    if (args && args->callback) {
        args->callback(args->call, &args->stats, args->user_data);
    }
    if (args && args->call) {
        args->call->release();
    }
    delete args;
}

void Engine::dispatch_call_stats(sip_call_t *call, const sip_call_stats_t *stats)
{
    if (!m_callbacks.on_call_stats || !stats) return;

    if (call) {
        call->retain();
    }
    auto *args = new CallStatsTaskArgs{
        call,
        *stats,
        m_callbacks.on_call_stats,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_call_stats_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_call_stats_task);
#else
    run_call_stats_task(args);
#endif
}

struct TransferStatusTaskArgs {
    sip_call_t *call{nullptr};
    int code{0};
    std::string reason;
    void (*callback)(sip_call_t *call, int code, const char *reason, void *user_data){nullptr};
    void *user_data{nullptr};
};

static void run_transfer_status_task(void *arg)
{
    auto *args = static_cast<TransferStatusTaskArgs *>(arg);
    if (args && args->callback) {
        args->callback(args->call, args->code, args->reason.c_str(), args->user_data);
    }
    if (args && args->call) {
        args->call->release();
    }
    delete args;
}

void Engine::dispatch_transfer_status(sip_call_t *call, int code, const char *reason)
{
    if (!m_callbacks.on_transfer_status) return;

    if (call) {
        call->retain();
    }
    auto *args = new TransferStatusTaskArgs{
        call,
        code,
        reason ? reason : "",
        m_callbacks.on_transfer_status,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_transfer_status_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_transfer_status_task);
#else
    run_transfer_status_task(args);
#endif
}

struct TransferRequestTaskArgs {
    sip_call_t *call{nullptr};
    std::string refer_to_uri;
    bool has_replaces{false};
    void (*callback)(sip_call_t *call, const char *refer_to_uri, bool has_replaces, void *user_data){nullptr};
    void *user_data{nullptr};
};

static void run_transfer_request_task(void *arg)
{
    auto *args = static_cast<TransferRequestTaskArgs *>(arg);
    if (args && args->callback) {
        args->callback(args->call, args->refer_to_uri.c_str(), args->has_replaces, args->user_data);
    }
    if (args && args->call) {
        args->call->release();
    }
    delete args;
}

void Engine::dispatch_transfer_request(sip_call_t *call, const char *refer_to_uri, bool has_replaces)
{
    if (!m_callbacks.on_transfer_request || !refer_to_uri) return;

    if (call) {
        call->retain();
    }
    auto *args = new TransferRequestTaskArgs{
        call,
        refer_to_uri,
        has_replaces,
        m_callbacks.on_transfer_request,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_transfer_request_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_transfer_request_task);
#else
    run_transfer_request_task(args);
#endif
}

// ============================================================================
// Messaging (SIP MESSAGE)
// ============================================================================

static bool has_control_chars(const char *s)
{
    for (; *s; ++s) {
        unsigned char ch = static_cast<unsigned char>(*s);
        if (ch < 0x20 || ch == 0x7f) {
            return true;
        }
    }
    return false;
}

static bool is_valid_content_type(const char *ctype)
{
    if (has_control_chars(ctype)) return false;
    const char *slash = std::strchr(ctype, '/');
    return slash != nullptr && slash != ctype && slash[1] != '\0' && slash[1] != ';';
}

// Pending MESSAGE transactions. libre is handed only an opaque token, never a pointer,
// so a late or missing callback cannot touch freed memory: whoever removes the token
// from this registry (the response handler or engine teardown) reports the one final
// status. libre v4.12.0 does not call the handler when the engine stops: ua_close() ->
// sip_close(sip, false) only sets sip->closing and returns (re src/sip/sip.c:183-195),
// while the handler runs on teardown only through sip_request_close()
// (re src/sip/request.c:991 -> close_handler -> terminate -> resph, ECONNABORTED).
struct PendingMessage {
    Engine *engine{nullptr};
    uint64_t id{0};
};

static std::mutex g_pending_messages_mutex;
static std::map<uintptr_t, PendingMessage> g_pending_messages;
static uintptr_t g_next_pending_token = 1;

static uintptr_t register_pending_message(Engine *engine, uint64_t id)
{
    std::lock_guard<std::mutex> lock(g_pending_messages_mutex);
    uintptr_t token = g_next_pending_token++;
    g_pending_messages[token] = PendingMessage{engine, id};
    return token;
}

// Returns false when the token is already gone (the final status was reported).
static bool take_pending_message(uintptr_t token, PendingMessage &out)
{
    std::lock_guard<std::mutex> lock(g_pending_messages_mutex);
    auto it = g_pending_messages.find(token);
    if (it == g_pending_messages.end()) return false;
    out = it->second;
    g_pending_messages.erase(it);
    return true;
}

void Engine::fail_pending_messages(const char *reason)
{
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(g_pending_messages_mutex);
        for (auto it = g_pending_messages.begin(); it != g_pending_messages.end();) {
            if (it->second.engine == this) {
                ids.push_back(it->second.id);
                it = g_pending_messages.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (uint64_t id : ids) {
        dispatch_message_status(id, SIP_MESSAGE_STATUS_FAILED, 0, reason);
    }
}

static void message_response_handler(int err, const struct sip_msg *msg, void *arg)
{
    uintptr_t token = reinterpret_cast<uintptr_t>(arg);
    if (!err && msg && msg->scode < 200) {
        return; // provisional response; wait for the final one
    }

    PendingMessage pending;
    if (!take_pending_message(token, pending)) return;

    if (err) {
        pending.engine->dispatch_message_status(pending.id, SIP_MESSAGE_STATUS_FAILED, 0, std::strerror(err));
    } else if (msg) {
        std::string reason = (msg->reason.p && msg->reason.l > 0) ? std::string(msg->reason.p, msg->reason.l) : "";
        sip_message_status_t status = (msg->scode >= 200 && msg->scode < 300)
            ? SIP_MESSAGE_STATUS_SENT : SIP_MESSAGE_STATUS_FAILED;
        pending.engine->dispatch_message_status(pending.id, status, static_cast<int>(msg->scode), reason.c_str());
    } else {
        pending.engine->dispatch_message_status(pending.id, SIP_MESSAGE_STATUS_FAILED, 0, "No response");
    }
}

int Engine::set_message_callbacks(const sip_message_callbacks_t *callbacks)
{
    std::lock_guard<std::mutex> lock(m_message_mutex);
    if (callbacks) {
        m_message_callbacks = *callbacks;
    } else {
        m_message_callbacks = sip_message_callbacks_t{};
    }
    return 0;
}

int Engine::set_recording_callbacks(const sip_recording_callbacks_t *callbacks)
{
    std::lock_guard<std::mutex> lock(m_recording_mutex);
    if (callbacks) {
        m_recording_callbacks = *callbacks;
    } else {
        m_recording_callbacks = sip_recording_callbacks_t{};
    }
    return 0;
}

int Engine::set_media_recorder_driver(std::shared_ptr<IMediaRecorderDriver> driver)
{
    if (!m_media_manager) return EINVAL;
    m_media_manager->set_media_recorder_driver(std::move(driver));
    return 0;
}

void Engine::dispatch_recording_state(sip_call_t *call, sip_recording_state_t state,
                                      const char *path, int error_code)
{
    sip_recording_callbacks_t cbs{};
    {
        std::lock_guard<std::mutex> lock(m_recording_mutex);
        cbs = m_recording_callbacks;
    }
    if (!cbs.on_recording_state) return;

    std::string path_copy = path ? path : "";
    if (call) call->retain();

    auto *ctx = new std::function<void()>([this, cbs, call, state, path_copy, error_code]() {
        cbs.on_recording_state(m_owner, call, state, path_copy.c_str(), error_code, m_user_data);
        if (call) call->release();
    });

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(
            [](void *arg) {
                auto *fn = static_cast<std::function<void()> *>(arg);
                (*fn)();
                delete fn;
            },
            ctx,
            m_user_data);
    } else {
        (*ctx)();
        delete ctx;
    }
}

int Engine::start_recording(sip_call_t *call, const char *path, sip_recording_mode_t mode)
{
    if (!call || !path || path[0] == '\0') return EINVAL;
    if (call->is_closed()) return EINVAL;
    if (call->is_recording()) return EALREADY;
    if (!m_started.load() || !m_media_manager) return EIO;

    std::string path_str = path;
    auto run_on_loop = [this, call, path_str, mode]() -> int {
        if (call->is_closed()) return EINVAL;
        if (call->is_recording()) return EALREADY;
        struct call *c = call->raw_call();
        if (!c) return EINVAL;
        struct audio *au = call_audio(c);
        if (!au) return EIO;
        uint64_t leg_id = reinterpret_cast<uint64_t>(au);
        RecordingMode rec_mode = static_cast<RecordingMode>(static_cast<int>(mode));
        int rc = m_media_manager->call_recorder().start_recording(leg_id, path_str, rec_mode);
        if (rc != 0) {
            dispatch_recording_state(call, SIP_RECORDING_STATE_FAILED, path_str.c_str(), rc);
            return rc;
        }
        m_media_manager->call_recorder().set_local_muted(leg_id, call->is_muted() || m_mic_muted.load());
        call->set_recording_state(true, leg_id, path_str);
        dispatch_recording_state(call, SIP_RECORDING_STATE_RECORDING, path_str.c_str(), 0);
        return 0;
    };

    if (std::this_thread::get_id() == m_loop_thread.get_id()) {
        return run_on_loop();
    }

    std::mutex wait_mu;
    std::condition_variable wait_cv;
    bool done = false;
    int result = EIO;

    call->retain();
    bool posted = post_task([&]() {
        result = run_on_loop();
        call->release();
        {
            std::lock_guard<std::mutex> lock(wait_mu);
            done = true;
        }
        wait_cv.notify_one();
    });
    if (!posted) {
        call->release();
        return EIO;
    }

    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return done; });
    return result;
}

int Engine::stop_recording(sip_call_t *call)
{
    if (!call) return EINVAL;
    if (!m_media_manager) return EIO;

    auto run_on_loop = [this, call]() -> int {
        uint64_t leg_id = call->recording_leg_id();
        if (leg_id == 0 || !call->is_recording()) {
            return ENOENT;
        }
        std::string stopped_path;
        int rc = m_media_manager->call_recorder().stop_recording(leg_id, &stopped_path);
        call->set_recording_state(false, 0, "");
        if (rc == 0) {
            dispatch_recording_state(call, SIP_RECORDING_STATE_IDLE, stopped_path.c_str(), 0);
        }
        return rc;
    };

    if (!m_started.load() || std::this_thread::get_id() == m_loop_thread.get_id()) {
        return run_on_loop();
    }

    std::mutex wait_mu;
    std::condition_variable wait_cv;
    bool done = false;
    int result = ENOENT;

    call->retain();
    bool posted = post_task([&]() {
        result = run_on_loop();
        call->release();
        {
            std::lock_guard<std::mutex> lock(wait_mu);
            done = true;
        }
        wait_cv.notify_one();
    });
    if (!posted) {
        call->release();
        return run_on_loop();
    }

    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return done; });
    return result;
}

int Engine::send_message(const char *peer_uri, const char *content_type, const uint8_t *body, size_t body_len,
                         uint64_t *message_id)
{
    return send_message_on_account(get_default_account(), peer_uri, content_type, body, body_len, message_id);
}

int Engine::send_message_on_account(sip_account_t *account, const char *peer_uri, const char *content_type,
                                    const uint8_t *body, size_t body_len, uint64_t *message_id)
{
    if (!peer_uri || peer_uri[0] == '\0' || has_control_chars(peer_uri)) return EINVAL;
    const char *ctype = content_type ? content_type : SIP_MESSAGE_CONTENT_TYPE_TEXT;
    if (!is_valid_content_type(ctype)) return EINVAL;
    if (!body || body_len == 0) return EINVAL;
    if (body_len > SIP_MESSAGE_MAX_BODY_SIZE) return EMSGSIZE;
    if (!m_started.load()) return EIO;
    if (account != nullptr && !is_account_active(account)) return EINVAL;

    uint64_t id = m_next_message_id.fetch_add(1);
    std::string uri_str = peer_uri;
    std::string ctype_str = ctype;
    std::string payload(reinterpret_cast<const char *>(body), body_len);

    // Hand the id to the caller before the task is posted: with an inline dispatcher the
    // status callback can run before this function returns.
    if (message_id) *message_id = id;

    if (account) account->retain();
    bool posted = post_task([this, account, id, uri_str, ctype_str, payload]() {
        struct ua *bua = account ? account->raw_ua() : resolve_call_ua(nullptr);
        if (bua == nullptr) {
            dispatch_message_status(id, SIP_MESSAGE_STATUS_FAILED, 0, "No account registered");
            if (account) account->release();
            return;
        }
        uintptr_t token = register_pending_message(this, id);
        int rc = sip_req_send(bua, "MESSAGE", uri_str.c_str(), message_response_handler,
                              reinterpret_cast<void *>(token),
                              "Accept: text/plain\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "\r\n%b",
                              ctype_str.c_str(), payload.size(), payload.data(), payload.size());
        if (rc != 0) {
            PendingMessage unused;
            if (take_pending_message(token, unused)) {
                dispatch_message_status(id, SIP_MESSAGE_STATUS_FAILED, 0, std::strerror(rc));
            }
        }
        if (account) account->release();
    });
    if (!posted) {
        if (account) account->release();
        if (message_id) *message_id = 0;
        return EIO;
    }
    return 0;
}

bool Engine::sip_request_handler(const struct sip_msg *msg, void *arg)
{
    auto *engine = static_cast<Engine *>(arg);
    return engine ? engine->handle_incoming_message(msg) : false;
}

bool Engine::handle_incoming_message(const struct sip_msg *msg)
{
    if (!msg || pl_strcmp(&msg->met, "MESSAGE") != 0) {
        return false;
    }
    struct ua *bua = uag_find_msg(msg);
    sip_account_t *acc = find_account_by_ua(bua);
    if (acc == nullptr) {
        return false;
    }

    if (msg->mb && mbuf_get_left(msg->mb) > SIP_MESSAGE_MAX_BODY_SIZE) {
        (void)sip_treply(nullptr, uag_sip(), msg, 413, "Request Entity Too Large");
        return true;
    }

    std::string from(msg->from.auri.p ? msg->from.auri.p : "", msg->from.auri.l);
    std::string ctype = SIP_MESSAGE_CONTENT_TYPE_TEXT;
    if (msg->ctyp.type.l > 0 && msg->ctyp.subtype.l > 0) {
        ctype = std::string(msg->ctyp.type.p, msg->ctyp.type.l) + "/" +
                std::string(msg->ctyp.subtype.p, msg->ctyp.subtype.l);
    }
    std::string body;
    if (msg->mb) {
        body.assign(reinterpret_cast<const char *>(mbuf_buf(msg->mb)), mbuf_get_left(msg->mb));
    }

    // Stateful reply: the server transaction absorbs UDP retransmissions of this
    // request, so a lost 200 OK does not deliver the message twice.
    (void)sip_treply(nullptr, uag_sip(), msg, 200, "OK");
    dispatch_account_message_received(acc, from, ctype, body);
    dispatch_message_received(std::move(from), std::move(ctype), std::move(body));
    return true;
}

struct MessageStatusTaskArgs {
    sip_client_t *client{nullptr};
    uint64_t id{0};
    sip_message_status_t status{SIP_MESSAGE_STATUS_FAILED};
    int code{0};
    std::string reason;
    void (*on_message_status)(sip_client_t *, uint64_t, sip_message_status_t, int, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_message_status_task(void *arg)
{
    auto *args = static_cast<MessageStatusTaskArgs *>(arg);
    if (args) {
        if (args->on_message_status) {
            args->on_message_status(args->client, args->id, args->status, args->code, args->reason.c_str(),
                                    args->user_data);
        }
        delete args;
    }
}

void Engine::dispatch_message_status(uint64_t message_id, sip_message_status_t status, int code, const char *reason)
{
    sip_message_callbacks_t cbs;
    {
        std::lock_guard<std::mutex> lock(m_message_mutex);
        cbs = m_message_callbacks;
    }
    if (!cbs.on_message_status) return;

    auto *args = new MessageStatusTaskArgs{
        m_owner,
        message_id,
        status,
        code,
        reason ? reason : "",
        cbs.on_message_status,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_message_status_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_message_status_task);
#else
    run_message_status_task(args);
#endif
}

struct MessageReceivedTaskArgs {
    sip_client_t *client{nullptr};
    std::string from;
    std::string content_type;
    std::string body;
    void (*on_message_received)(sip_client_t *, const char *, const char *, const uint8_t *, size_t, void *){nullptr};
    void *user_data{nullptr};
};

static void run_message_received_task(void *arg)
{
    auto *args = static_cast<MessageReceivedTaskArgs *>(arg);
    if (args) {
        if (args->on_message_received) {
            args->on_message_received(args->client, args->from.c_str(), args->content_type.c_str(),
                                      reinterpret_cast<const uint8_t *>(args->body.data()), args->body.size(),
                                      args->user_data);
        }
        delete args;
    }
}

void Engine::dispatch_message_received(std::string from_uri, std::string content_type, std::string body)
{
    sip_message_callbacks_t cbs;
    {
        std::lock_guard<std::mutex> lock(m_message_mutex);
        cbs = m_message_callbacks;
    }
    if (!cbs.on_message_received) return;

    auto *args = new MessageReceivedTaskArgs{
        m_owner,
        std::move(from_uri),
        std::move(content_type),
        std::move(body),
        cbs.on_message_received,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_message_received_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_message_received_task);
#else
    run_message_received_task(args);
#endif
}

struct AccountMessageReceivedTaskArgs {
    sip_client_t *client{nullptr};
    sip_account_t *account{nullptr};
    std::string from;
    std::string content_type;
    std::string body;
    void (*on_account_message_received)(sip_client_t *, sip_account_t *, const char *, const char *,
                                        const uint8_t *, size_t, void *){nullptr};
    void *user_data{nullptr};
};

static void run_account_message_received_task(void *arg)
{
    auto *args = static_cast<AccountMessageReceivedTaskArgs *>(arg);
    if (args) {
        if (args->on_account_message_received) {
            args->on_account_message_received(args->client, args->account, args->from.c_str(),
                                              args->content_type.c_str(),
                                              reinterpret_cast<const uint8_t *>(args->body.data()),
                                              args->body.size(), args->user_data);
        }
        if (args->account) {
            args->account->release();
        }
        delete args;
    }
}

void Engine::dispatch_account_message_received(sip_account_t *account, std::string from_uri,
                                               std::string content_type, std::string body)
{
    if (!account) return;
    sip_account_callbacks_t cbs;
    {
        std::lock_guard<std::mutex> lock(m_account_cb_mutex);
        cbs = m_account_callbacks;
    }
    if (!cbs.on_account_message_received) return;

    account->retain();
    auto *args = new AccountMessageReceivedTaskArgs{
        m_owner,
        account,
        std::move(from_uri),
        std::move(content_type),
        std::move(body),
        cbs.on_account_message_received,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_account_message_received_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_account_message_received_task);
#else
    run_account_message_received_task(args);
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
