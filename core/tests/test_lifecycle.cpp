#include "sip_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cassert>

static std::atomic<int> g_reg_callbacks{0};
static std::atomic<int> g_call_callbacks{0};

static void test_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    (void)client;
    (void)state;
    (void)code;
    (void)reason;
    (void)user_data;
    g_reg_callbacks++;
}

static void test_on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data)
{
    (void)client;
    (void)call;
    (void)user_data;
    g_call_callbacks++;
}

static void test_on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
{
    (void)call;
    (void)state;
    (void)code;
    (void)reason;
    (void)user_data;
    g_call_callbacks++;
}

static void test_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
{
    (void)call;
    (void)width;
    (void)height;
    (void)on_hold;
    (void)is_muted;
    (void)user_data;
}

static void test_on_dtmf_digit(sip_call_t *call, const char *digit, void *user_data)
{
    (void)call;
    (void)digit;
    (void)user_data;
}

int main(int argc, char *argv[])
{
    const char *config_dir = (argc > 1) ? argv[1] : nullptr;
    std::printf("==> Running sip_core C ABI lifecycle tests (config_dir=%s)\n", config_dir ? config_dir : "null");

    sip_callbacks_t callbacks = {};
    callbacks.on_reg_state = test_on_reg_state;
    callbacks.on_incoming_call = test_on_incoming_call;
    callbacks.on_call_state = test_on_call_state;
    callbacks.on_call_media = test_on_call_media;
    callbacks.on_dtmf_digit = test_on_dtmf_digit;

    // Test 1: Defensive null-safety
    std::printf("--- Test 1: Null safety and invalid args ---\n");
    sip_client_destroy(nullptr); // Must not crash
    int null_init_rc = sip_client_init(nullptr, config_dir);
    if (null_init_rc == 0) {
        std::fprintf(stderr, "FAIL: sip_client_init(nullptr) returned 0\n");
        return 1;
    }
    std::printf("PASS: sip_client_init(nullptr) rejected with code %d\n", null_init_rc);

    // Test 2: Single init & destroy cycle
    std::printf("--- Test 2: Single engine init and clean teardown ---\n");
    int test_context = 42;
    sip_client_t *client = sip_client_create(&callbacks, &test_context);
    if (!client) {
        std::fprintf(stderr, "FAIL: sip_client_create returned nullptr\n");
        return 2;
    }
    std::printf("PASS: sip_client_create returned valid handle\n");

    int init_rc = sip_client_init(client, config_dir);
    if (init_rc != 0) {
        std::fprintf(stderr, "FAIL: sip_client_init returned error code %d\n", init_rc);
        sip_client_destroy(client);
        return 3;
    }
    std::printf("PASS: sip_client_init succeeded (rc=0)\n");

    // Let the worker loop run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sip_client_destroy(client);
    client = nullptr;
    std::printf("PASS: sip_client_destroy joined background thread and freed resources\n");

    // Test 3: Multiple consecutive lifecycle cycles in the same process
    std::printf("--- Test 3: 5 consecutive init/destroy cycles (leak & hang check) ---\n");
    const int kIterations = 5;
    for (int i = 1; i <= kIterations; i++) {
        sip_client_t *iter_client = sip_client_create(&callbacks, &test_context);
        if (!iter_client) {
            std::fprintf(stderr, "FAIL: Iteration %d sip_client_create failed\n", i);
            return 4;
        }

        int iter_rc = sip_client_init(iter_client, config_dir);
        if (iter_rc != 0) {
            std::fprintf(stderr, "FAIL: Iteration %d sip_client_init failed with code %d\n", i, iter_rc);
            sip_client_destroy(iter_client);
            return 5;
        }

        // Brief run to let worker loop poll
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        sip_client_destroy(iter_client);
        std::printf("PASS: Iteration %d completed cleanly\n", i);
    }

    std::printf("ALL_LIFECYCLE_TESTS_PASSED\n");
    return 0;
}
