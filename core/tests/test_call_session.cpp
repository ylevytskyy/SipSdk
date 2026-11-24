// Unit tests for sip_call_t (core/src/call_session.h): the call identity
// strings that the C ABI returns as raw pointers.

#include "call_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

namespace {

// Long enough that libc++ and libstdc++ keep them on the heap, so an
// in-place overwrite or a reallocation would change what the old pointer reads.
const char *kInitialUri = "sip:initial-remote-party-with-a-long-name@pbx.example.test:5060";
const char *kUpdatedUri = "sip:updated-remote-party@pbx.example.test:5060";
const char *kUpdatedId = "updated-call-id-0123456789abcdef0123456789";
const char *kUpdatedName = "Updated Display Name For The Remote Party";

// Given pointers returned by the identity getters,
// when the call identity is updated (as attach_raw_call does on the worker thread),
// then the earlier pointers still read the earlier strings,
// and the getters return the new strings.
void test_identity_pointers_stay_valid_after_update()
{
    sip_call_t *call = new sip_call_t(nullptr, nullptr, nullptr, kInitialUri, true, false);

    const char *id_before = call->get_id();
    const char *uri_before = call->get_remote_uri();
    const char *name_before = call->get_peer_display_name();
    const std::string id_copy = id_before;
    const std::string uri_copy = uri_before;
    const std::string name_copy = name_before;
    TEST_ASSERT(uri_copy == kInitialUri, "initial remote URI must come from the constructor");

    call->update_identity(kUpdatedId, kUpdatedName, kUpdatedUri);

    TEST_ASSERT(std::string(id_before) == id_copy, "earlier get_id() pointer changed after update");
    TEST_ASSERT(std::string(uri_before) == uri_copy, "earlier get_remote_uri() pointer changed after update");
    TEST_ASSERT(std::string(name_before) == name_copy,
                "earlier get_peer_display_name() pointer changed after update");

    TEST_ASSERT(std::string(call->get_id()) == kUpdatedId, "get_id() must return the updated id");
    TEST_ASSERT(std::string(call->get_remote_uri()) == kUpdatedUri, "get_remote_uri() must return the updated URI");
    TEST_ASSERT(std::string(call->get_peer_display_name()) == kUpdatedName,
                "get_peer_display_name() must return the updated name");

    call->release();
    std::printf("PASS: test_identity_pointers_stay_valid_after_update\n");
}

// Given a reader thread that keeps copying the remote URI,
// when another thread keeps updating the call identity,
// then every copy the reader makes is one of the published URIs.
// The race it guards against is narrow, so this test runs for a fixed time;
// test_identity_pointers_stay_valid_after_update is the deterministic guard.
void test_concurrent_reads_see_complete_published_values()
{
    const std::string uri_a = std::string(kInitialUri);
    const std::string uri_b = std::string(kUpdatedUri);
    sip_call_t *call = new sip_call_t(nullptr, nullptr, nullptr, uri_a.c_str(), true, false);

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> bad_reads{0};
    std::thread reader([&]() {
        while (!stop.load()) {
            std::string copy = call->get_remote_uri();
            if (copy != uri_a && copy != uri_b) bad_reads.fetch_add(1);
            reads.fetch_add(1);
        }
    });

    // Keep updating for a fixed time once the reader is running.
    while (reads.load() == 0) std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (int i = 0; std::chrono::steady_clock::now() < deadline; ++i) {
        call->update_identity(nullptr, nullptr, (i % 2 == 0) ? uri_b.c_str() : uri_a.c_str());
    }
    stop.store(true);
    reader.join();

    TEST_ASSERT(bad_reads.load() == 0, "a reader saw a URI that was never published");
    call->release();
    std::printf("PASS: test_concurrent_reads_see_complete_published_values\n");
}

} // namespace

int main()
{
    test_identity_pointers_stay_valid_after_update();
    test_concurrent_reads_see_complete_published_values();
    std::printf("ALL_CALL_SESSION_TESTS_PASSED\n");
    return 0;
}
