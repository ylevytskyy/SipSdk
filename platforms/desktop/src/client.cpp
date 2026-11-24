#include <sip/client.hpp>
#include <sip/audio_driver.hpp>
#include <sip_abi.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

namespace sip {

static std::string ensure_default_config_dir()
{
    const char *home = std::getenv("HOME");
    std::string base_dir = home ? std::string(home) + "/.sipsdk" : "/tmp/.sipsdk";
    mkdir(base_dir.c_str(), 0755);

    std::string config_file = base_dir + "/config";
    struct stat st;
    if (stat(config_file.c_str(), &st) != 0) {
        std::ofstream ofs(config_file);
        if (ofs.is_open()) {
            ofs << "# Minimal static Baresip configuration for SipSdk Desktop\n";
            ofs << "poll_method select\n";
            ofs << "sip_trans_bsize 128\n";
            ofs << "rtp_tos 184\n";
            ofs << "rtcp_enable yes\n";
            ofs << "rtcp_mux no\n";
            ofs << "jitter_buffer_delay 5-10\n\n";

#if defined(__APPLE__)
            ofs << "audio_player audiounit,nil\n";
            ofs << "audio_source audiounit,nil\n";
            ofs << "module audiounit.so\n";
#else
            ofs << "audio_player pulse,nil\n";
            ofs << "audio_source pulse,nil\n";
#endif
            ofs << "video_source avcapture,nil\n";
            ofs << "video_display iosview,nil\n";
            ofs << "video_size 640x480\n";
            ofs << "video_fps 30.0\n\n";

            ofs << "module g711.so\n";
            ofs << "module account.so\n";
            ofs << "module stun.so\n";
            ofs << "module turn.so\n";
            ofs << "module ice.so\n";
            ofs << "module srtp.so\n";
        }
    }
    return base_dir;
}

static sip_video_capture_driver_t make_c_capture_driver(sip_core::IVideoCaptureDriver *driver)
{
    sip_video_capture_driver_t c_drv{};
    c_drv.user_data = driver;
    c_drv.start = [](void *ud, int w, int h, int fps) -> int {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        return d ? d->start(w, h, fps) : -1;
    };
    c_drv.stop = [](void *ud) {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        if (d) d->stop();
    };
    c_drv.set_frame_handler = [](void *ud, void (*handler)(const sip_video_frame_t *, void *), void *arg) {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        if (!d) return;
        if (!handler) {
            d->set_frame_handler(nullptr);
            return;
        }
        d->set_frame_handler([handler, arg](const sip_core::VideoFrame &vf) {
            sip_video_frame_t cf{};
            cf.width = vf.width;
            cf.height = vf.height;
            cf.y = vf.y;
            cf.u = vf.u;
            cf.v = vf.v;
            cf.y_stride = vf.y_stride;
            cf.u_stride = vf.u_stride;
            cf.v_stride = vf.v_stride;
            cf.timestamp_ns = vf.timestamp_ns;
            handler(&cf, arg);
        });
    };
    c_drv.switch_camera = [](void *ud) {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        if (d) d->switch_camera();
    };
    c_drv.set_camera = [](void *ud, int pos) {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        if (d) d->set_camera(pos);
    };
    c_drv.set_enabled = [](void *ud, bool en) {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        if (d) d->set_enabled(en);
    };
    c_drv.is_enabled = [](void *ud) -> bool {
        auto *d = static_cast<sip_core::IVideoCaptureDriver *>(ud);
        return d ? d->is_enabled() : false;
    };
    return c_drv;
}

std::string Client::get_version()
{
    return "1.0.0-desktop";
}

struct Client::Callbacks {
    static void on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self) return;

        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener) {
            RegistrationState st = static_cast<RegistrationState>(state);
            listener->on_registration_state(st, code, reason ? reason : "");
        }
    }

    static void on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            listener->on_incoming_call(call_ptr);
        }
    }

    static void on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            CallState st = static_cast<CallState>(state);
            listener->on_call_state(call_ptr, st, code, reason ? reason : "");
        }

        if (state == SIP_CALL_STATE_CLOSED) {
            self->remove_call(call);
        }
    }

    static void on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            CallMediaInfo media;
            media.width = width;
            media.height = height;
            media.on_hold = on_hold;
            media.is_muted = is_muted;
            listener->on_call_media(call_ptr, media);
        }
    }

    static void on_dtmf_digit(sip_call_t *call, const char *digit, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            listener->on_dtmf_digit(call_ptr, digit ? digit : "");
        }
    }

    static void on_call_stats(sip_call_t *call, const sip_call_stats_t *stats, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call || !stats) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            CallStats s;
            s.audio_loss_rate = stats->audio_loss_rate;
            s.audio_jitter_ms = stats->audio_jitter_ms;
            s.audio_rtt_ms = stats->audio_rtt_ms;
            s.audio_bitrate_kbps = stats->audio_bitrate_kbps;
            s.audio_codec = stats->audio_codec;
            s.video_loss_rate = stats->video_loss_rate;
            s.video_jitter_ms = stats->video_jitter_ms;
            s.video_bitrate_kbps = stats->video_bitrate_kbps;
            s.video_fps = stats->video_fps;
            s.video_codec = stats->video_codec;
            listener->on_call_stats(call_ptr, s);
        }
    }

    static void on_transfer_status(sip_call_t *call, int code, const char *reason, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            listener->on_transfer_status(call_ptr, code, reason ? reason : "");
        }
    }

    static void on_transfer_request(sip_call_t *call, const char *refer_to_uri, bool has_replaces, void *user_data)
    {
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_ptr = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener && call_ptr) {
            listener->on_transfer_request(call_ptr, refer_to_uri ? refer_to_uri : "", has_replaces);
        }
    }

    static void on_call_rejected_busy(sip_client_t *client, const char *caller_uri, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self) return;

        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }
        if (listener) {
            listener->on_call_rejected_busy(caller_uri ? caller_uri : "");
        }
    }

    static void on_message_received(sip_client_t *client, const char *from_uri, const char *content_type,
                                    const uint8_t *body, size_t body_len, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self) return;

        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener) {
            IncomingMessage message;
            message.from = from_uri ? from_uri : "";
            message.content_type = content_type ? content_type : "text/plain";
            if (body && body_len > 0) {
                message.body.assign(reinterpret_cast<const char *>(body), body_len);
            }
            listener->on_message_received(message);
        }
    }

    static void on_message_status(sip_client_t *client, uint64_t message_id, sip_message_status_t status,
                                  int code, const char *reason, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self) return;

        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }

        if (listener) {
            listener->on_message_status(message_id, static_cast<MessageStatus>(status), code, reason ? reason : "");
        }
    }

    static void on_recording_state(sip_client_t *client, sip_call_t *call, sip_recording_state_t state,
                                   const char *path, int error_code, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self || !call) return;

        auto call_obj = self->get_or_create_call(call);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }
        if (listener) {
            listener->on_recording_state(
                call_obj,
                static_cast<RecordingState>(state),
                path ? path : "",
                error_code);
        }
    }

    static void dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
    {
        (void)user_data;
        if (task) {
            task(arg);
        }
    }

    static void on_conference_changed(sip_client_t *client, sip_call_t *const *calls, size_t count, void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self) return;

        std::vector<std::shared_ptr<Call>> members;
        members.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (calls[i]) {
                auto c = self->get_or_create_call(calls[i]);
                if (c) members.push_back(std::move(c));
            }
        }
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }
        if (listener) {
            listener->on_conference_changed(members);
        }
    }

    static void on_account_reg_state(sip_client_t *client, sip_account_t *account,
                                     sip_reg_state_t state, int code, const char *reason,
                                     void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self || !account) return;

        auto acc_ptr = self->find_account(account);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }
        if (listener && acc_ptr) {
            listener->on_account_registration_state(acc_ptr, static_cast<RegistrationState>(state),
                                                    code, reason ? reason : "");
        }
    }

    static void on_account_message_received(sip_client_t *client, sip_account_t *account,
                                            const char *from_uri, const char *content_type,
                                            const uint8_t *body, size_t body_len,
                                            void *user_data)
    {
        (void)client;
        auto *self = static_cast<Client *>(user_data);
        if (!self || !account) return;

        auto acc_ptr = self->find_account(account);
        std::shared_ptr<IClientListener> listener;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            listener = self->m_listener;
        }
        if (listener && acc_ptr) {
            IncomingMessage message;
            message.from = from_uri ? from_uri : "";
            message.content_type = content_type ? content_type : "text/plain";
            if (body && body_len > 0) {
                message.body.assign(reinterpret_cast<const char *>(body), body_len);
            }
            listener->on_account_message_received(acc_ptr, message);
        }
    }
};

Client::Client(std::string username, std::string domain)
    : m_username(std::move(username))
    , m_domain(std::move(domain))
{
}

Client::~Client()
{
    stop();
}

std::string Client::aor() const
{
    std::ostringstream ss;
    ss << "sip:" << m_username << "@" << m_domain << ":" << m_port;
    return ss.str();
}

void Client::set_listener(std::shared_ptr<IClientListener> listener)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = std::move(listener);
}

int Client::start(const std::string &config_path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client) {
        return 0; // Already started
    }

    sip_callbacks_t cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.dispatch_to_main = Callbacks::dispatch_to_main;
    cb.on_reg_state = Callbacks::on_reg_state;
    cb.on_incoming_call = Callbacks::on_incoming_call;
    cb.on_call_state = Callbacks::on_call_state;
    cb.on_call_media = Callbacks::on_call_media;
    cb.on_dtmf_digit = Callbacks::on_dtmf_digit;
    cb.on_call_stats = Callbacks::on_call_stats;
    cb.on_transfer_status = Callbacks::on_transfer_status;
    cb.on_conference_changed = Callbacks::on_conference_changed;
    cb.on_transfer_request = Callbacks::on_transfer_request;
    cb.on_call_rejected_busy = Callbacks::on_call_rejected_busy;

    m_client = sip_client_create(&cb, this);
    if (!m_client) {
        return -1;
    }

    if (m_capture_driver) {
        sip_video_capture_driver_t c_drv = make_c_capture_driver(m_capture_driver.get());
        sip_client_set_video_capture_driver(m_client, &c_drv);
    }

    sip_message_callbacks_t msg_cb;
    std::memset(&msg_cb, 0, sizeof(msg_cb));
    msg_cb.on_message_received = Callbacks::on_message_received;
    msg_cb.on_message_status = Callbacks::on_message_status;
    sip_client_set_message_callbacks(m_client, &msg_cb);

    sip_recording_callbacks_t rec_cb;
    std::memset(&rec_cb, 0, sizeof(rec_cb));
    rec_cb.on_recording_state = Callbacks::on_recording_state;
    sip_client_set_recording_callbacks(m_client, &rec_cb);

    sip_account_callbacks_t acc_cb;
    std::memset(&acc_cb, 0, sizeof(acc_cb));
    acc_cb.on_account_reg_state = Callbacks::on_account_reg_state;
    acc_cb.on_account_message_received = Callbacks::on_account_message_received;
    sip_client_set_account_callbacks(m_client, &acc_cb);

    std::string resolved_path = config_path.empty() ? ensure_default_config_dir() : config_path;
    int rc = sip_client_init(m_client, resolved_path.c_str());
    if (rc != 0) {
        sip_client_destroy(m_client);
        m_client = nullptr;
        return rc;
    }

    if (m_audio_backend != AudioBackend::Auto) {
        std::string mod = AudioDriverManager::get_backend_name(m_audio_backend);
        if (mod == "dummy") mod = "nil";
        sip_client_set_audio_driver(m_client, mod.c_str(), m_audio_device.c_str(), mod.c_str(), m_audio_device.c_str());
    } else {
#if !defined(__APPLE__)
        sip_client_set_audio_driver(m_client, "nil", "", "nil", "");
#endif
    }

    return 0;
}

void Client::stop()
{
    sip_client_t *c = nullptr;
    std::unordered_map<sip_call_t *, std::shared_ptr<Call>> calls_to_invalidate;
    std::unordered_map<sip_account_t *, std::shared_ptr<Account>> accounts_to_invalidate;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        c = m_client;
        m_client = nullptr;
        calls_to_invalidate = std::move(m_active_calls);
        m_active_calls.clear();
        accounts_to_invalidate = std::move(m_accounts);
        m_accounts.clear();
    }
    for (auto &pair : calls_to_invalidate) {
        if (pair.second) {
            pair.second->invalidate();
        }
        if (pair.first) {
            sip_call_destroy(pair.first);
        }
    }
    for (auto &pair : accounts_to_invalidate) {
        if (pair.second) {
            pair.second->invalidate();
        }
    }
    if (c) {
        sip_client_destroy(c);
    }
}

int Client::register_account()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return -1;
    sip_account_config_t cfg{};
    cfg.user = m_username.c_str();
    cfg.domain = m_domain.c_str();
    cfg.pwd = m_password.empty() ? nullptr : m_password.c_str();
    cfg.transport = m_transport.c_str();
    cfg.port = m_port;
    cfg.tls_ca_file = m_tls_ca_file.empty() ? nullptr : m_tls_ca_file.c_str();
    cfg.verify_tls_cert = m_verify_tls_cert;
    cfg.media_enc = m_media_enc.empty() ? nullptr : m_media_enc.c_str();
    cfg.max_calls = m_max_calls;
    return sip_client_register_account(m_client, &cfg);
}

int Client::unregister_account()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return -1;
    return sip_client_unregister(m_client);
}

bool Client::is_registered() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return false;
    return sip_client_is_registered(m_client);
}

int Client::add_account(const AccountConfig &config, std::shared_ptr<Account> &out_account)
{
    out_account.reset();
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return EIO;

    sip_account_config_t cfg{};
    cfg.user = config.username.c_str();
    cfg.domain = config.domain.c_str();
    cfg.pwd = config.password.empty() ? nullptr : config.password.c_str();
    cfg.transport = config.transport.empty() ? "udp" : config.transport.c_str();
    cfg.port = config.port;
    cfg.display_name = config.display_name.empty() ? nullptr : config.display_name.c_str();
    cfg.outbound_proxy = config.outbound_proxy.empty() ? nullptr : config.outbound_proxy.c_str();
    cfg.stun_server = config.stun_server.empty() ? nullptr : config.stun_server.c_str();
    cfg.stun_user = config.stun_user.empty() ? nullptr : config.stun_user.c_str();
    cfg.stun_pass = config.stun_pass.empty() ? nullptr : config.stun_pass.c_str();
    cfg.media_nat = config.media_nat.empty() ? nullptr : config.media_nat.c_str();
    cfg.reg_interval = config.reg_interval;
    cfg.tls_ca_file = config.tls_ca_file.empty() ? nullptr : config.tls_ca_file.c_str();
    cfg.verify_tls_cert = config.verify_tls_cert;
    cfg.media_enc = config.media_enc.empty() ? nullptr : config.media_enc.c_str();
    cfg.max_calls = config.max_calls;

    sip_account_t *handle = nullptr;
    int rc = sip_client_add_account(cli, &cfg, &handle);
    if (rc != 0 || !handle) return rc != 0 ? rc : EIO;

    auto acc = std::make_shared<Account>(handle);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accounts[handle] = acc;
    }
    out_account = std::move(acc);
    return 0;
}

int Client::remove_account(const std::shared_ptr<Account> &account)
{
    if (!account || !account->raw_handle()) return EINVAL;
    sip_client_t *cli = nullptr;
    sip_account_t *handle = account->raw_handle();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return EIO;

    int rc = sip_client_remove_account(cli, handle);
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accounts.erase(handle);
    }
    return rc;
}

int Client::set_default_account(const std::shared_ptr<Account> &account)
{
    if (!account || !account->raw_handle()) return EINVAL;
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return EIO;
    return sip_client_set_default_account(cli, account->raw_handle());
}

std::shared_ptr<Account> Client::default_account() const
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return nullptr;
    return find_account(sip_client_get_default_account(cli));
}

std::vector<std::shared_ptr<Account>> Client::accounts() const
{
    std::vector<std::shared_ptr<Account>> result;
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return result;

    sip_account_t *handles[SIP_CLIENT_MAX_ACCOUNTS]{};
    size_t count = sip_client_get_accounts(cli, handles, SIP_CLIENT_MAX_ACCOUNTS);
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto acc = find_account(handles[i]);
        if (acc) result.push_back(std::move(acc));
    }
    return result;
}

std::shared_ptr<Call> Client::make_call(const std::string &target_uri, bool with_video)
{
    std::shared_ptr<Call> out_call;
    (void)dial(target_uri, with_video, out_call);
    return out_call;
}

int Client::dial(const std::string &target_uri, bool with_video, std::shared_ptr<Call> &out_call)
{
    out_call.reset();
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return EINVAL;

    sip_call_t *handle = nullptr;
    int rc = sip_call_dial(cli, target_uri.c_str(), with_video, &handle);
    if (rc != 0 || !handle) return rc != 0 ? rc : EIO;

    out_call = get_or_create_call(handle);
    return 0;
}

int Client::dial(const std::shared_ptr<Account> &account, const std::string &target_uri, bool with_video, std::shared_ptr<Call> &out_call)
{
    out_call.reset();
    if (!account || !account->raw_handle()) return EINVAL;

    sip_call_t *handle = nullptr;
    int rc = sip_account_dial(account->raw_handle(), target_uri.c_str(), with_video, &handle);
    if (rc != 0 || !handle) return rc != 0 ? rc : EIO;

    out_call = get_or_create_call(handle);
    return 0;
}

int Client::merge_calls(const std::shared_ptr<Call> &call_a, const std::shared_ptr<Call> &call_b)
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli || !call_a || !call_b || !call_a->raw_handle() || !call_b->raw_handle()) return EINVAL;
    return sip_client_merge_calls(cli, call_a->raw_handle(), call_b->raw_handle());
}

int Client::add_to_conference(const std::shared_ptr<Call> &call)
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli || !call || !call->raw_handle()) return EINVAL;
    return sip_client_add_to_conference(cli, call->raw_handle());
}

int Client::split_call(const std::shared_ptr<Call> &call)
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli || !call || !call->raw_handle()) return EINVAL;
    return sip_client_split_call(cli, call->raw_handle());
}

int Client::leave_conference()
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return EINVAL;
    return sip_client_leave_conference(cli);
}

int Client::send_message(const std::string &target_uri,
                         const std::string &body,
                         const std::string &content_type,
                         uint64_t *message_id)
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return -1;

    return sip_client_send_message(cli,
                                   target_uri.c_str(),
                                   content_type.c_str(),
                                   reinterpret_cast<const uint8_t *>(body.data()),
                                   body.size(),
                                   message_id);
}

int Client::send_message(const std::shared_ptr<Account> &account,
                         const std::string &target_uri,
                         const std::string &body,
                         const std::string &content_type,
                         uint64_t *message_id)
{
    if (!account || !account->raw_handle()) return EINVAL;
    return sip_account_send_message(account->raw_handle(),
                                    target_uri.c_str(),
                                    content_type.c_str(),
                                    reinterpret_cast<const uint8_t *>(body.data()),
                                    body.size(),
                                    message_id);
}

void Client::set_mic_muted(bool muted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client) {
        sip_client_set_mic_muted(m_client, muted);
    }
}

bool Client::is_mic_muted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client) {
        return sip_client_is_mic_muted(m_client);
    }
    return false;
}

int Client::set_audio_driver(AudioBackend backend, const std::string &device)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_audio_backend = backend;
    m_audio_device = device;
    if (m_client) {
        std::string mod = AudioDriverManager::get_backend_name(backend);
        if (mod == "auto") {
#if defined(__APPLE__)
            mod = "audiounit";
#else
            mod = "pulse";
#endif
        } else if (mod == "dummy") {
            mod = "nil";
        }
        sip_client_set_audio_driver(m_client, mod.c_str(), device.c_str(), mod.c_str(), device.c_str());
    }
    return 0;
}

std::vector<AudioBackend> Client::get_available_audio_backends() const
{
    std::vector<AudioBackend> backends;
    backends.push_back(AudioBackend::Auto);
#if defined(__APPLE__)
    backends.push_back(AudioBackend::CoreAudio);
#else
    backends.push_back(AudioBackend::PulseAudio);
    backends.push_back(AudioBackend::Alsa);
#endif
    backends.push_back(AudioBackend::Dummy);
    return backends;
}

void Client::set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver)
{
    // Keep the old driver alive until the engine has dropped its adapter,
    // which still calls the old driver while it is destroyed. Declared before
    // the lock so the old driver is destroyed after m_mutex is released.
    std::shared_ptr<sip_core::IVideoCaptureDriver> previous;
    std::lock_guard<std::mutex> lock(m_mutex);
    previous = std::exchange(m_capture_driver, std::move(driver));
    if (m_client) {
        if (m_capture_driver) {
            sip_video_capture_driver_t c_drv = make_c_capture_driver(m_capture_driver.get());
            sip_client_set_video_capture_driver(m_client, &c_drv);
        } else {
            sip_client_set_video_capture_driver(m_client, nullptr);
        }
    }
}

void Client::use_synthetic_video_capture()
{
    set_video_capture_driver(std::make_shared<sip_core::SyntheticVideoCaptureDriver>());
}

int Client::start_local_preview()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return -1;
    return sip_client_start_local_preview(m_client);
}

int Client::stop_local_preview()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return -1;
    return sip_client_stop_local_preview(m_client);
}

bool Client::is_local_preview_active() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_client) return false;
    return sip_client_is_local_preview_active(m_client);
}

std::shared_ptr<Call> Client::get_or_create_call(sip_call_t *handle)
{
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_calls.find(handle);
    if (it != m_active_calls.end()) {
        if (it->second && !it->second->m_account) {
            sip_account_t *acc_h = sip_call_get_account(handle);
            auto acc_it = m_accounts.find(acc_h);
            if (acc_it != m_accounts.end()) {
                it->second->m_account = acc_it->second;
            }
        }
        return it->second;
    }
    auto call_ptr = std::make_shared<Call>(handle);
    sip_account_t *acc_h = sip_call_get_account(handle);
    auto acc_it = m_accounts.find(acc_h);
    if (acc_it != m_accounts.end()) {
        call_ptr->m_account = acc_it->second;
    }
    m_active_calls[handle] = call_ptr;
    return call_ptr;
}

std::shared_ptr<Account> Client::find_account(sip_account_t *handle) const
{
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_accounts.find(handle);
    return it != m_accounts.end() ? it->second : nullptr;
}

void Client::remove_call(sip_call_t *handle)
{
    if (!handle) return;
    std::shared_ptr<Call> removed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_active_calls.find(handle);
        if (it != m_active_calls.end()) {
            removed = std::move(it->second);
            m_active_calls.erase(it);
        }
    }
    if (removed) {
        removed->invalidate();
    }
    sip_call_destroy(handle);
}

} // namespace sip
