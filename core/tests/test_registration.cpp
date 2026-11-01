#include "sip_abi.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

struct RegEvent {
    sip_reg_state_t state;
    int code;
    std::string reason;
    bool was_main_thread;
};

static std::mutex g_reg_mutex;
static std::condition_variable g_reg_cv;
static std::vector<RegEvent> g_reg_events;
static std::atomic<int> g_main_dispatch_count{0};

static bool is_current_thread_main()
{
#if defined(__APPLE__)
    return pthread_main_np() != 0;
#else
    return true;
#endif
}

static void test_dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
{
    (void)user_data;
    g_main_dispatch_count++;
    // Execute task
    if (task) {
        task(arg);
    }
}

static void test_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    (void)client;
    (void)user_data;
    RegEvent ev;
    ev.state = state;
    ev.code = code;
    ev.reason = reason ? reason : "";
    ev.was_main_thread = is_current_thread_main();

    {
        std::lock_guard<std::mutex> lock(g_reg_mutex);
        g_reg_events.push_back(ev);
    }
    g_reg_cv.notify_all();
}

static bool wait_for_state(sip_reg_state_t target_state, int timeout_sec, RegEvent &matched_event)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_reg_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_reg_events) {
            if (ev.state == target_state) {
                matched_event = ev;
                return true;
            }
        }
        g_reg_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

static void clear_events()
{
    std::lock_guard<std::mutex> lock(g_reg_mutex);
    g_reg_events.clear();
}

int main(int argc, char *argv[])
{
    const char *config_dir = (argc > 1) ? argv[1] : nullptr;
    const char *asterisk_host = (argc > 2) ? argv[2] : "127.0.0.1";
    int asterisk_port = (argc > 3) ? std::atoi(argv[3]) : 5060;

    std::printf("==> Running sip_core C ABI registration tests (host=%s:%d, config_dir=%s)\n",
                asterisk_host, asterisk_port, config_dir ? config_dir : "null");

    sip_callbacks_t callbacks = {};
    callbacks.dispatch_to_main = test_dispatch_to_main;
    callbacks.on_reg_state = test_on_reg_state;

    // Test 1: Null safety and parameter validation
    std::printf("--- Test 1: Null safety and argument validation ---\n");
    assert(sip_client_is_registered(nullptr) == false);
    assert(sip_client_unregister(nullptr) != 0);
    assert(sip_client_register(nullptr, "1001", asterisk_host, "test1001", "udp", asterisk_port) != 0);

    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    assert(client != nullptr);
    assert(sip_client_register(client, nullptr, asterisk_host, "test1001", "udp", asterisk_port) != 0);
    assert(sip_client_register(client, "1001", nullptr, "test1001", "udp", asterisk_port) != 0);
    assert(sip_client_register(client, "1001", asterisk_host, "test1001", "udp", -1) != 0);
    assert(sip_client_is_registered(client) == false);
    std::printf("PASS: Parameter validation successfully rejected invalid inputs\n");

    int init_rc = sip_client_init(client, config_dir);
    if (init_rc != 0) {
        std::fprintf(stderr, "FAIL: sip_client_init failed with code %d\n", init_rc);
        sip_client_destroy(client);
        return 1;
    }
    std::printf("PASS: sip_client_init succeeded\n");

    // Test 2: Successful UDP registration with MD5 digest challenge against Asterisk PBX (extension 1001)
    std::printf("--- Test 2: Successful UDP registration & MD5 auth against Asterisk (1001) ---\n");
    clear_events();
    int reg_rc = sip_client_register(client, "1001", asterisk_host, "test1001", "udp", asterisk_port);
    if (reg_rc != 0) {
        std::fprintf(stderr, "FAIL: sip_client_register returned code %d\n", reg_rc);
        sip_client_destroy(client);
        return 2;
    }

    RegEvent ok_event{};
    bool registered = wait_for_state(SIP_REG_STATE_REGISTERED, 10, ok_event);
    if (!registered) {
        std::fprintf(stderr, "FAIL: Did not reach SIP_REG_STATE_REGISTERED within 10s\n");
        {
            std::lock_guard<std::mutex> lock(g_reg_mutex);
            for (size_t i = 0; i < g_reg_events.size(); i++) {
                std::fprintf(stderr, "      event[%zu]: state=%d code=%d reason='%s'\n",
                             i, g_reg_events[i].state, g_reg_events[i].code, g_reg_events[i].reason.c_str());
            }
        }
        sip_client_destroy(client);
        return 3;
    }

    assert(ok_event.code == 200);
    assert(sip_client_is_registered(client) == true);
    std::printf("PASS: Registered with Asterisk PBX (code=%d, reason='%s')\n",
                ok_event.code, ok_event.reason.c_str());

    // Test 3: Clean unregistration
    std::printf("--- Test 3: Clean unregistration (Expires: 0) ---\n");
    clear_events();
    int unreg_rc = sip_client_unregister(client);
    if (unreg_rc != 0) {
        std::fprintf(stderr, "FAIL: sip_client_unregister returned code %d\n", unreg_rc);
        sip_client_destroy(client);
        return 4;
    }

    RegEvent unreg_event{};
    bool unregistered = wait_for_state(SIP_REG_STATE_UNREGISTERED, 5, unreg_event);
    if (!unregistered) {
        std::fprintf(stderr, "FAIL: Did not reach SIP_REG_STATE_UNREGISTERED within 5s\n");
        sip_client_destroy(client);
        return 5;
    }
    assert(sip_client_is_registered(client) == false);
    std::printf("PASS: Unregistered cleanly from Asterisk PBX\n");

    // Test 4: Negative authentication test (wrong password -> 401 Unauthorized / Failed)
    std::printf("--- Test 4: Negative auth test with incorrect password ---\n");
    clear_events();
    int fail_reg_rc = sip_client_register(client, "1001", asterisk_host, "wrong_password_xyz", "udp", asterisk_port);
    if (fail_reg_rc != 0) {
        std::fprintf(stderr, "FAIL: sip_client_register returned code %d\n", fail_reg_rc);
        sip_client_destroy(client);
        return 6;
    }

    RegEvent fail_event{};
    bool failed = wait_for_state(SIP_REG_STATE_FAILED, 8, fail_event);
    if (!failed) {
        std::fprintf(stderr, "FAIL: Did not reach SIP_REG_STATE_FAILED for invalid password\n");
        sip_client_destroy(client);
        return 7;
    }
    assert(fail_event.code == 401 || fail_event.code == 403);
    assert(sip_client_is_registered(client) == false);
    std::printf("PASS: Rejected invalid credentials with code %d ('%s')\n",
                fail_event.code, fail_event.reason.c_str());

    // Test 5: Main thread dispatch check
    std::printf("--- Test 5: Verify UI thread dispatch ---\n");
    assert(g_main_dispatch_count.load() > 0);
    std::printf("PASS: Main thread dispatcher invoked %d times\n", g_main_dispatch_count.load());

    // Teardown
    sip_client_destroy(client);
    std::printf("ALL_REGISTRATION_TESTS_PASSED\n");
    return 0;
}
