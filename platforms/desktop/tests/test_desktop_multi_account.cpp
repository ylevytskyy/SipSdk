#include <sip/desktop.hpp>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stderr);                                                            \
            std::abort();                                                                   \
        }                                                                                   \
    } while (0)

namespace {

class MultiAccountListener : public sip::IClientListener {
public:
    void on_account_registration_state(std::shared_ptr<sip::Account> account,
                                       sip::RegistrationState state,
                                       int code,
                                       const std::string &reason) override
    {
        (void)code;
        (void)reason;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_reg_events.push_back({account ? account->id() : 0, state});
        m_cv.notify_all();
    }

    void on_account_message_received(std::shared_ptr<sip::Account> account,
                                     const sip::IncomingMessage &message) override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_messages.push_back({account ? account->id() : 0, message.body});
        m_cv.notify_all();
    }

    void on_call_state(std::shared_ptr<sip::Call> call,
                       sip::CallState state,
                       int code,
                       const std::string &reason) override
    {
        (void)code;
        (void)reason;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_call_events.push_back({call, state});
        m_cv.notify_all();
    }

    bool wait_reg(uint64_t acc_id, sip::RegistrationState target, int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_cv.wait_until(lk, deadline, [&]() {
            for (const auto &e : m_reg_events) {
                if (e.first == acc_id && e.second == target) return true;
            }
            return false;
        });
    }

    bool wait_message(uint64_t acc_id, const std::string &body, int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_cv.wait_until(lk, deadline, [&]() {
            for (const auto &m : m_messages) {
                if (m.first == acc_id && m.second == body) return true;
            }
            return false;
        });
    }

    bool wait_call_state(const std::shared_ptr<sip::Call> &call, sip::CallState target, int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_cv.wait_until(lk, deadline, [&]() {
            for (const auto &e : m_call_events) {
                if (e.first == call && e.second == target) return true;
            }
            return false;
        });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::pair<uint64_t, sip::RegistrationState>> m_reg_events;
    std::vector<std::pair<uint64_t, std::string>> m_messages;
    std::vector<std::pair<std::shared_ptr<sip::Call>, sip::CallState>> m_call_events;
};

} // namespace

int main()
{
    const char *host_env = std::getenv("ASTERISK_HOST");
    std::string host = (host_env && host_env[0] != '\0') ? host_env : "127.0.0.1";
    const char *port_env = std::getenv("ASTERISK_PORT");
    int port = (port_env && port_env[0] != '\0') ? std::atoi(port_env) : 5060;

    auto client = std::make_shared<sip::Client>("1001", host);
    auto listener = std::make_shared<MultiAccountListener>();
    client->set_listener(listener);
    client->set_audio_driver(sip::AudioBackend::Dummy);
    CHECK(client->start() == 0);

    sip::AccountConfig cfg1;
    cfg1.username = "1001";
    cfg1.domain = host;
    cfg1.password = "test1001";
    cfg1.transport = "udp";
    cfg1.port = port;

    sip::AccountConfig cfg2;
    cfg2.username = "1003";
    cfg2.domain = host;
    cfg2.password = "test1003";
    cfg2.transport = "udp";
    cfg2.port = port;

    std::shared_ptr<sip::Account> acc1;
    std::shared_ptr<sip::Account> acc2;
    CHECK(client->add_account(cfg1, acc1) == 0 && acc1 != nullptr);
    CHECK(client->add_account(cfg2, acc2) == 0 && acc2 != nullptr);
    CHECK(client->accounts().size() == 2);
    CHECK(client->default_account() == acc1);

    CHECK(listener->wait_reg(acc1->id(), sip::RegistrationState::Registered, 10));
    CHECK(listener->wait_reg(acc2->id(), sip::RegistrationState::Registered, 10));
    CHECK(acc1->is_registered());
    CHECK(acc2->is_registered());

    std::string uri_1003 = "sip:1003@" + host + ":" + std::to_string(port);
    uint64_t msg_id = 0;
    CHECK(client->send_message(acc1, uri_1003, "desktop-multi-hello", "text/plain", &msg_id) == 0);
    CHECK(msg_id > 0);
    CHECK(listener->wait_message(acc2->id(), "desktop-multi-hello", 10));

    std::string uri_9001 = "sip:9001@" + host + ":" + std::to_string(port);
    std::shared_ptr<sip::Call> call1;
    CHECK(client->dial(acc2, uri_9001, false, call1) == 0 && call1 != nullptr);
    CHECK(call1->account() == acc2);
    CHECK(listener->wait_call_state(call1, sip::CallState::Established, 10));
    CHECK(call1->hangup() == 0);
    CHECK(listener->wait_call_state(call1, sip::CallState::Closed, 10));

    CHECK(client->set_default_account(acc2) == 0);
    CHECK(client->default_account() == acc2);
    CHECK(client->remove_account(acc2) == 0);
    CHECK(client->default_account() == acc1);
    CHECK(client->accounts().size() == 1);

    client->stop();
    std::printf("PASS: Desktop C++ multi-account test passed\n");
    return 0;
}
