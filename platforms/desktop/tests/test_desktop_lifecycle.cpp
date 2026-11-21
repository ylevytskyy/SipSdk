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

    std::vector<sip::IncomingMessage> messages;
    std::vector<std::pair<uint64_t, sip::MessageStatus>> message_statuses;
    std::vector<std::string> busy_rejected_uris;

    void on_message_received(const sip::IncomingMessage &message) override {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(message);
        cv.notify_all();
    }

    void on_message_status(uint64_t message_id, sip::MessageStatus status, int code, const std::string &reason) override {
        (void)code; (void)reason;
        std::lock_guard<std::mutex> lock(mutex);
        message_statuses.emplace_back(message_id, status);
        cv.notify_all();
    }

    void on_call_rejected_busy(const std::string &caller_uri) override {
        std::lock_guard<std::mutex> lock(mutex);
        busy_rejected_uris.push_back(caller_uri);
        cv.notify_all();
    }

    bool wait_for_busy_rejected(std::string &uri_out, int timeout_seconds = 10) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!busy_rejected_uris.empty()) {
                uri_out = busy_rejected_uris.back();
                return true;
            }
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    bool wait_for_call_state(const std::shared_ptr<sip::Call> &call, sip::CallState expected_state, int timeout_seconds = 5) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto &ev : call_events) {
                if ((call == nullptr || ev.call == call) && ev.state == expected_state) {
                    return true;
                }
            }
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    bool wait_for_message(uint64_t id, const std::string &body, sip::MessageStatus &status_out,
                          sip::IncomingMessage &message_out, int timeout_seconds = 10) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            bool have_status = false;
            for (const auto &st : message_statuses) {
                if (st.first == id) { status_out = st.second; have_status = true; }
            }
            for (const auto &m : messages) {
                if (have_status && m.body == body) { message_out = m; return true; }
            }
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
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

        // Test 6b: Text message round trip through Asterisk (1001 -> 1001)
        std::printf("--- Test 6b: Text Message Round Trip ---\n");
        TEST_ASSERT(client->send_message("", "x") != 0, "send_message must reject an empty target");
        TEST_ASSERT(client->send_message(client->aor(), "") != 0, "send_message must reject an empty body");
        uint64_t message_id = 0;
        const std::string text = "hello from desktop";
        rc = client->send_message(client->aor(), text, "text/plain", &message_id);
        TEST_ASSERT(rc == 0, "send_message failed");
        TEST_ASSERT(message_id != 0, "send_message must return a message id");
        sip::MessageStatus msg_status = sip::MessageStatus::Failed;
        sip::IncomingMessage incoming;
        TEST_ASSERT(listener->wait_for_message(message_id, text, msg_status, incoming), "Message status or echo not received");
        TEST_ASSERT(msg_status == sip::MessageStatus::Sent, "Message status must be Sent");
        TEST_ASSERT(incoming.content_type == "text/plain", "Received content type mismatch");
        TEST_ASSERT(incoming.from.rfind("sip:1001@", 0) == 0, "Received sender mismatch");
        std::printf("PASS: Text message round trip (id=%llu from=%s)\n",
                    (unsigned long long)message_id, incoming.from.c_str());

        // Test 7: Unregister and Teardown
        std::printf("--- Test 7: Unregister and Clean Stop ---\n");
        rc = client->unregister_account();
        TEST_ASSERT(rc == 0, "client->unregister_account() failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        client->stop();
        TEST_ASSERT(!client->is_started(), "Client should be stopped");
        std::printf("PASS: Clean unregister and stop verified\n");

        // Test 8: TLS + SRTP & CallStats via C++ Desktop SDK
        std::printf("--- Test 8: TLS + SRTP & CallStats ---\n");
        auto tls_listener = std::make_shared<TestListener>();
        auto tls_client = std::make_shared<sip::Client>("1004", asterisk_host);
        tls_client->set_password("test1004");
        tls_client->set_port(5061);
        tls_client->set_transport("tls");
        const char *ca_path = "docker/asterisk/keys/asterisk.crt";
        for (const char *cand : {"docker/asterisk/keys/asterisk.crt",
                                 "../../docker/asterisk/keys/asterisk.crt",
                                 "/workspace/docker/asterisk/keys/asterisk.crt"}) {
            if (FILE *f = std::fopen(cand, "r")) {
                std::fclose(f);
                ca_path = cand;
                break;
            }
        }
        tls_client->set_tls_ca_file(ca_path);
        tls_client->set_verify_tls_cert(true);
        tls_client->set_media_enc("srtp-mand");
        tls_client->set_listener(tls_listener);
        TEST_ASSERT(tls_client->start(config_dir ? config_dir : "") == 0, "tls_client->start() failed");
        TEST_ASSERT(tls_client->register_account() == 0, "tls_client->register_account() failed");
        TEST_ASSERT(tls_listener->wait_for_reg_state(sip::RegistrationState::Registered, 5),
                    "TLS registration did not succeed");
        std::string tls_echo = std::string("sip:9001@") + asterisk_host + ":5061;transport=tls";
        auto tls_call = tls_client->make_call(tls_echo, false);
        TEST_ASSERT(tls_call != nullptr, "tls_client->make_call returned null");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        TEST_ASSERT(tls_call->is_secure(), "TLS/SRTP call must report is_secure() == true");
        sip::CallStats stats = tls_call->get_stats();
        TEST_ASSERT(stats.audio_codec == "opus", "Expected opus audio codec in CallStats");
        TEST_ASSERT(tls_call->transfer("") != 0, "transfer must reject empty URI");
        tls_call->hangup();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        tls_client->unregister_account();
        tls_client->stop();
        std::printf("PASS: TLS + SRTP & CallStats verified via Desktop C++ SDK\n");

        // Test 9: Multi-call auto-hold, swap, max_calls cap (EBUSY + 486 on_call_rejected_busy)
        std::printf("--- Test 9: Multi-call cap & swap via Desktop C++ SDK ---\n");
        auto mc_listener = std::make_shared<TestListener>();
        auto mc_client = std::make_shared<sip::Client>("1001", asterisk_host);
        mc_client->set_password("test1001");
        mc_client->set_port(asterisk_port);
        mc_client->set_transport("udp");
        TEST_ASSERT(mc_client->max_calls() == 4, "Default max_calls should be 4");
        mc_client->set_max_calls(4);
        mc_client->set_listener(mc_listener);
        TEST_ASSERT(mc_client->start(config_dir ? config_dir : "") == 0, "mc_client->start() failed");
        TEST_ASSERT(mc_client->register_account() == 0, "mc_client->register_account() failed");
        TEST_ASSERT(mc_listener->wait_for_reg_state(sip::RegistrationState::Registered, 5), "mc_client reg failed");

        std::shared_ptr<sip::Call> mc1, mc2;
        TEST_ASSERT(mc_client->dial(echo_uri, false, mc1) == 0 && mc1 != nullptr, "dial mc1 failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(mc1, sip::CallState::Established, 8), "mc1 establish failed");
        TEST_ASSERT(mc_client->dial(echo_uri, false, mc2) == 0 && mc2 != nullptr, "dial mc2 failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(mc1, sip::CallState::Hold, 5), "mc1 auto-hold failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(mc2, sip::CallState::Established, 8), "mc2 establish failed");
        TEST_ASSERT(mc1->set_hold(false) == 0, "swap mc1 failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(mc2, sip::CallState::Hold, 5), "mc2 hold on swap failed");

        // Merge mc1 + mc2 into local conference, then split mc2 back to hold
        TEST_ASSERT(mc_client->merge_calls(mc1, mc2) == 0, "merge_calls failed");
        for (int i = 0; i < 50 && !(mc1->is_in_conference() && mc2->is_in_conference()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        TEST_ASSERT(mc1->is_in_conference() && mc2->is_in_conference(), "both calls must be in conference");
        TEST_ASSERT(mc_client->split_call(mc2) == 0, "split_call failed");
        for (int i = 0; i < 50 && (mc1->is_in_conference() || mc2->is_in_conference()); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        TEST_ASSERT(!mc1->is_in_conference() && !mc2->is_in_conference(), "conference must dissolve on split");

        mc1->hangup();
        mc2->hangup();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        mc_client->set_max_calls(1);
        TEST_ASSERT(mc_client->register_account() == 0, "re-register max_calls=1 failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::shared_ptr<sip::Call> cap_warm;
        TEST_ASSERT(mc_client->dial(echo_uri, false, cap_warm) == 0 && cap_warm != nullptr, "warm cap dial failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(cap_warm, sip::CallState::Established, 8), "warm cap establish failed");
        std::shared_ptr<sip::Call> over_cap;
        TEST_ASSERT(mc_client->dial(echo_uri, false, over_cap) == EBUSY && over_cap == nullptr,
                    "dial over max_calls=1 must return EBUSY");
        TEST_ASSERT(mc_client->make_call(echo_uri, false) == nullptr, "make_call over cap must return null");
        cap_warm->hangup();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::string cb_uri = std::string("sip:9003@") + asterisk_host + ":" + std::to_string(asterisk_port);
        std::shared_ptr<sip::Call> cb_trig;
        TEST_ASSERT(mc_client->dial(cb_uri, false, cb_trig) == 0 && cb_trig != nullptr, "9003 dial failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(cb_trig, sip::CallState::Established, 8), "9003 establish failed");
        cb_trig->hangup();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::shared_ptr<sip::Call> hold_cap;
        TEST_ASSERT(mc_client->dial(echo_uri, false, hold_cap) == 0 && hold_cap != nullptr, "hold_cap dial failed");
        TEST_ASSERT(mc_listener->wait_for_call_state(hold_cap, sip::CallState::Established, 8), "hold_cap establish failed");
        std::string busy_caller;
        TEST_ASSERT(mc_listener->wait_for_busy_rejected(busy_caller, 10) && !busy_caller.empty(),
                    "Expected on_call_rejected_busy on 9003 callback at max_calls=1");
        hold_cap->hangup();
        mc_client->unregister_account();
        mc_client->stop();
        std::printf("PASS: Multi-call cap & swap verified via Desktop C++ SDK\n");
    }

    std::printf("=================================================================\n");
    std::printf("ALL DESKTOP LIFECYCLE TESTS PASSED\n");
    std::printf("=================================================================\n");
    return 0;
}
