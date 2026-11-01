#include <sip/desktop.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

class TestListener : public sip::IClientListener {
public:
    struct RegEvent {
        sip::RegistrationState state;
        int code;
        std::string reason;
    };

    struct CallStateEvent {
        std::shared_ptr<sip::Call> call;
        sip::CallState state;
        int code;
        std::string reason;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<RegEvent> reg_events;
    std::vector<CallStateEvent> call_events;
    std::vector<std::shared_ptr<sip::Call>> incoming_calls;

    void on_registration_state(sip::RegistrationState state, int code, const std::string &reason) override {
        std::lock_guard<std::mutex> lock(mutex);
        reg_events.push_back({state, code, reason});
        cv.notify_all();
    }

    void on_incoming_call(std::shared_ptr<sip::Call> call) override {
        std::lock_guard<std::mutex> lock(mutex);
        incoming_calls.push_back(call);
        cv.notify_all();
    }

    void on_call_state(std::shared_ptr<sip::Call> call, sip::CallState state, int code, const std::string &reason) override {
        std::lock_guard<std::mutex> lock(mutex);
        call_events.push_back({call, state, code, reason});
        cv.notify_all();
    }

    bool wait_for_reg_state(sip::RegistrationState expected_state, int timeout_seconds = 5) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto &ev : reg_events) {
                if (ev.state == expected_state) {
                    return true;
                }
            }
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }
};

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const char *config_dir = (argc > 1) ? argv[1] : nullptr;
    const char *asterisk_host = std::getenv("ASTERISK_HOST") ? std::getenv("ASTERISK_HOST") : "127.0.0.1";
    int asterisk_port = std::getenv("ASTERISK_PORT") ? std::atoi(std::getenv("ASTERISK_PORT")) : 5060;

    std::printf("=================================================================\n");
    std::printf("Running Desktop C++ SDK Lifecycle & Registration Tests\n");
    std::printf("Asterisk target: %s:%d\n", asterisk_host, asterisk_port);
    std::printf("=================================================================\n");

    {
        // Test 1: Version check
        std::printf("--- Test 1: SDK Version ---\n");
        std::string ver = sip::Client::get_version();
        TEST_ASSERT(!ver.empty(), "Version string must not be empty");
        std::printf("PASS: SDK Version = %s\n", ver.c_str());

        // Test 2: Unstarted Client State
        std::printf("--- Test 2: Unstarted Client State ---\n");
        auto client = std::make_shared<sip::Client>("1001", asterisk_host);
        TEST_ASSERT(!client->is_started(), "Client should not report started before start()");
        TEST_ASSERT(!client->is_registered(), "Client should not report registered before start()");
        TEST_ASSERT(!client->is_mic_muted(), "Client should default to unmuted");
        TEST_ASSERT(client->aor() == std::string("sip:1001@") + asterisk_host + ":5060", "AOR mismatch");
        std::printf("PASS: Unstarted state verified\n");

        // Test 3: Client Start and Teardown
        std::printf("--- Test 3: Start and Stop Lifecycle ---\n");
        int rc = client->start(config_dir ? config_dir : "");
        TEST_ASSERT(rc == 0, "client->start() failed");
        TEST_ASSERT(client->is_started(), "client->is_started() should be true");
        client->stop();
        TEST_ASSERT(!client->is_started(), "client->is_started() should be false after stop");
        std::printf("PASS: Start and stop lifecycle verified\n");

        // Test 4: Registration against Asterisk PBX
        std::printf("--- Test 4: Registration against Asterisk PBX ---\n");
        auto listener = std::make_shared<TestListener>();
        client = std::make_shared<sip::Client>("1001", asterisk_host);
        client->set_password("test1001");
        client->set_port(asterisk_port);
        client->set_transport("udp");
        client->set_listener(listener);

        rc = client->start(config_dir ? config_dir : "");
        TEST_ASSERT(rc == 0, "client->start() failed");

        rc = client->register_account();
        TEST_ASSERT(rc == 0, "client->register_account() failed");

        bool reg_ok = listener->wait_for_reg_state(sip::RegistrationState::Registered, 5);
        TEST_ASSERT(reg_ok, "Did not receive RegistrationState::Registered within timeout");
        TEST_ASSERT(client->is_registered(), "client->is_registered() should return true");
        std::printf("PASS: SIP registration successful for 1001@%s\n", asterisk_host);

        // Test 5: Microphone Mute Toggle
        std::printf("--- Test 5: Microphone Mute Toggle ---\n");
        client->set_mic_muted(true);
        TEST_ASSERT(client->is_mic_muted(), "Mic should be muted");
        client->set_mic_muted(false);
        TEST_ASSERT(!client->is_mic_muted(), "Mic should be unmuted");
        std::printf("PASS: Microphone mute toggle verified\n");

        // Test 6: Call Creation & Object Model
        std::printf("--- Test 6: Make Call Handle ---\n");
        std::string echo_uri = std::string("sip:9001@") + asterisk_host + ":" + std::to_string(asterisk_port);
        auto call = client->make_call(echo_uri, false);
        TEST_ASSERT(call != nullptr, "make_call returned null");
        TEST_ASSERT(call->is_outgoing(), "Call should be outgoing");
        TEST_ASSERT(call->get_remote_uri() == echo_uri, "Call remote URI mismatch");
        TEST_ASSERT(!call->get_id().empty(), "Call ID must not be empty");

        // Hangup the call
        call->hangup();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf("PASS: Outgoing call handle verified and hung up\n");

        // Test 7: Unregister and Teardown
        std::printf("--- Test 7: Unregister and Clean Stop ---\n");
        rc = client->unregister_account();
        TEST_ASSERT(rc == 0, "client->unregister_account() failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        client->stop();
        TEST_ASSERT(!client->is_started(), "Client should be stopped");
        std::printf("PASS: Clean unregister and stop verified\n");
    }

    std::printf("=================================================================\n");
    std::printf("ALL DESKTOP LIFECYCLE TESTS PASSED\n");
    std::printf("=================================================================\n");
    return 0;
}
