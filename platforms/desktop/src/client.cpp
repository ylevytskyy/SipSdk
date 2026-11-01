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
            ofs << "module pulse.so\n";
            ofs << "module alsa.so\n";
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

    static void dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
    {
        (void)user_data;
        if (task) {
            task(arg);
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

    m_client = sip_client_create(&cb, this);
    if (!m_client) {
        return -1;
    }

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
    }

    return 0;
}

void Client::stop()
{
    sip_client_t *c = nullptr;
    std::unordered_map<sip_call_t *, std::shared_ptr<Call>> calls_to_invalidate;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        c = m_client;
        m_client = nullptr;
        calls_to_invalidate = std::move(m_active_calls);
        m_active_calls.clear();
    }
    for (auto &pair : calls_to_invalidate) {
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
    return sip_client_register(m_client,
                               m_username.c_str(),
                               m_domain.c_str(),
                               m_password.c_str(),
                               m_transport.c_str(),
                               m_port);
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

std::shared_ptr<Call> Client::make_call(const std::string &target_uri, bool with_video)
{
    sip_client_t *cli = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cli = m_client;
    }
    if (!cli) return nullptr;

    sip_call_t *handle = sip_call_make(cli, target_uri.c_str(), with_video);
    if (!handle) return nullptr;

    return get_or_create_call(handle);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capture_driver = std::move(driver);
}

void Client::use_synthetic_video_capture()
{
    set_video_capture_driver(std::make_shared<sip_core::SyntheticVideoCaptureDriver>());
}

std::shared_ptr<Call> Client::get_or_create_call(sip_call_t *handle)
{
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_active_calls.find(handle);
    if (it != m_active_calls.end()) {
        return it->second;
    }
    auto call_ptr = std::make_shared<Call>(handle);
    m_active_calls[handle] = call_ptr;
    return call_ptr;
}

void Client::remove_call(sip_call_t *handle)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_active_calls.erase(handle);
}

} // namespace sip
