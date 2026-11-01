#ifndef SIP_DESKTOP_CLIENT_HPP
#define SIP_DESKTOP_CLIENT_HPP

#include "types.hpp"
#include "listener.hpp"
#include "call.hpp"

#include <media_driver.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration of C ABI handle
struct sip_client_t;

namespace sip {

class Client : public std::enable_shared_from_this<Client> {
public:
    Client(std::string username, std::string domain);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    const std::string &username() const { return m_username; }
    const std::string &domain() const { return m_domain; }
    std::string aor() const;

    void set_password(std::string password) { m_password = std::move(password); }
    void set_port(int port) { m_port = port; }
    void set_transport(std::string transport) { m_transport = std::move(transport); }
    void set_listener(std::shared_ptr<IClientListener> listener);

    int start(const std::string &config_path = "");
    void stop();
    bool is_started() const { return m_client != nullptr; }

    int register_account();
    int unregister_account();
    bool is_registered() const;

    std::shared_ptr<Call> make_call(const std::string &target_uri, bool with_video = false);

    void set_mic_muted(bool muted);
    bool is_mic_muted() const;

    int set_audio_driver(AudioBackend backend, const std::string &device = "");
    AudioBackend get_audio_driver() const { return m_audio_backend; }
    std::vector<AudioBackend> get_available_audio_backends() const;

    void set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver);
    void use_synthetic_video_capture();

    static std::string get_version();

private:
    struct Callbacks;
    std::shared_ptr<Call> get_or_create_call(sip_call_t *handle);
    void remove_call(sip_call_t *handle);

    std::string m_username;
    std::string m_domain;
    std::string m_password;
    int m_port{5060};
    std::string m_transport{"udp"};

    AudioBackend m_audio_backend{AudioBackend::Auto};
    std::string m_audio_device;

    sip_client_t *m_client{nullptr};
    std::shared_ptr<IClientListener> m_listener;
    mutable std::mutex m_mutex;

    std::unordered_map<sip_call_t *, std::shared_ptr<Call>> m_active_calls;
    std::shared_ptr<sip_core::IVideoCaptureDriver> m_capture_driver;
};

} // namespace sip

#endif // SIP_DESKTOP_CLIENT_HPP
