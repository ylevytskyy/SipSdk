#include "sip_abi.h"

#include <atomic>
#include <cassert>
#include <cerrno>
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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

struct MessageStatusEvent {
    uint64_t id;
    sip_message_status_t status;
    int code;
    std::string reason;
    bool id_known_to_caller;  // the caller's message_id out-param already held this id when the callback ran
};

struct ReceivedMessage {
    std::string from;
    std::string content_type;
    std::string body;
};

static std::vector<MessageStatusEvent> g_msg_status;
static std::atomic<const uint64_t *> g_caller_ids{nullptr};
static std::atomic<size_t> g_caller_ids_count{0};
static std::vector<ReceivedMessage> g_msg_received;

static void test_on_message_status(sip_client_t *client, uint64_t message_id, sip_message_status_t status,
                                   int code, const char *reason, void *user_data)
{
    (void)client;
    (void)user_data;
    {
        std::lock_guard<std::mutex> lock(g_reg_mutex);
        const uint64_t *ids = g_caller_ids.load();
        bool known = true;
        if (ids) {
            known = false;
            size_t n = g_caller_ids_count.load();
            for (size_t i = 0; i < n && !known; i++) {
                known = reinterpret_cast<const volatile uint64_t *>(ids)[i] == message_id;
            }
        }
        g_msg_status.push_back({message_id, status, code, reason ? reason : "", known});
    }
    g_reg_cv.notify_all();
}

static void test_on_message_received(sip_client_t *client, const char *from_uri, const char *content_type,
                                     const uint8_t *body, size_t body_len, void *user_data)
{
    (void)client;
    (void)user_data;
    {
        std::lock_guard<std::mutex> lock(g_reg_mutex);
        g_msg_received.push_back({from_uri ? from_uri : "",
                                  content_type ? content_type : "",
                                  std::string(reinterpret_cast<const char *>(body), body_len)});
    }
    g_reg_cv.notify_all();
}

static bool wait_for_message_status(uint64_t id, int timeout_sec, MessageStatusEvent &matched)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_reg_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_msg_status) {
            if (ev.id == id) {
                matched = ev;
                return true;
            }
        }
        g_reg_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

static bool wait_for_received_body(const std::string &body, int timeout_sec, ReceivedMessage &matched)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_reg_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &m : g_msg_received) {
            if (m.body == body) {
                matched = m;
                return true;
            }
        }
        g_reg_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

// assert() is compiled out in Release builds; messaging checks must always run.
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL: check failed: %s (line %d)\n", #cond, __LINE__);       \
            std::exit(30);                                                            \
        }                                                                             \
    } while (0)

// Minimal SIP UDP peer: binds 127.0.0.1 on a free port.
static int open_raw_peer(int &port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        close(fd);
        return -1;
    }
    port_out = ntohs(addr.sin_port);
    return fd;
}

// Waits for one MESSAGE request, answers 200 OK and returns the raw request text.
static std::string raw_peer_answer_one(int fd, int timeout_ms)
{
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[8192];
    sockaddr_in src{};
    socklen_t src_len = sizeof(src);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&src), &src_len);
    if (n <= 0) return "";
    std::string request(buf, static_cast<size_t>(n));
    if (request.compare(0, 8, "MESSAGE ") != 0) return request;

    std::string reply = "SIP/2.0 200 OK\r\n";
    size_t pos = request.find("\r\n") + 2;
    while (pos < request.size()) {
        size_t end = request.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        std::string line = request.substr(pos, end - pos);
        if (line.compare(0, 4, "Via:") == 0 || line.compare(0, 5, "From:") == 0 ||
            line.compare(0, 8, "Call-ID:") == 0 || line.compare(0, 5, "CSeq:") == 0) {
            reply += line + "\r\n";
        } else if (line.compare(0, 3, "To:") == 0) {
            reply += line + ";tag=rawpeer\r\n";
        }
        pos = end + 2;
    }
    reply += "Content-Length: 0\r\n\r\n";
    sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&src), src_len);
    return request;
}

static uint64_t send_text(sip_client_t *client, const std::string &uri, const char *ctype, const std::string &body)
{
    uint64_t id = 0;
    int rc = sip_client_send_message(client, uri.c_str(), ctype,
                                     reinterpret_cast<const uint8_t *>(body.data()), body.size(), &id);
    if (rc != 0) {
        std::printf("FAIL: sip_client_send_message returned %d\n", rc);
        std::exit(20);
    }
    CHECK(id != 0);
    return id;
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

    // Messaging input validation and the no-account path (before registration)
    std::printf("--- Test 1b: Message input validation ---\n");
    sip_message_callbacks_t msg_callbacks = {};
    msg_callbacks.on_message_received = test_on_message_received;
    msg_callbacks.on_message_status = test_on_message_status;
    CHECK(sip_client_set_message_callbacks(nullptr, &msg_callbacks) == EINVAL);
    CHECK(sip_client_set_message_callbacks(client, &msg_callbacks) == 0);
    {
        const uint8_t hi[] = {'h', 'i'};
        uint64_t id = 0;
        std::string self_uri = std::string("sip:1001@") + asterisk_host + ":" + std::to_string(asterisk_port);
        CHECK(sip_client_send_message(nullptr, self_uri.c_str(), "text/plain", hi, sizeof(hi), &id) == EINVAL);
        CHECK(sip_client_send_message(client, nullptr, "text/plain", hi, sizeof(hi), &id) == EINVAL);
        CHECK(sip_client_send_message(client, "", "text/plain", hi, sizeof(hi), &id) == EINVAL);
        CHECK(sip_client_send_message(client, self_uri.c_str(), "text/plain", nullptr, 0, &id) == EINVAL);
        CHECK(sip_client_send_message(client, self_uri.c_str(), "text/plain\r\nX-Evil: 1", hi, sizeof(hi), &id) == EINVAL);
        CHECK(sip_client_send_message(client, "sip:1001@host\r\nX-Evil: 1", "text/plain", hi, sizeof(hi), &id) == EINVAL);
        CHECK(sip_client_send_message(client, self_uri.c_str(), "textplain", hi, sizeof(hi), &id) == EINVAL);
        std::vector<uint8_t> too_big(SIP_MESSAGE_MAX_BODY_SIZE + 1, 'x');
        CHECK(sip_client_send_message(client, self_uri.c_str(), "text/plain", too_big.data(), too_big.size(), &id) == EMSGSIZE);
        CHECK(id == 0);

        // No account allocated yet: the message is accepted for sending and then fails.
        uint64_t no_ua_id = send_text(client, self_uri, nullptr, "no account");
        MessageStatusEvent st{};
        bool got = wait_for_message_status(no_ua_id, 5, st);
        if (!got || st.status != SIP_MESSAGE_STATUS_FAILED) {
            std::printf("FAIL: expected FAILED for a message without an account\n");
            sip_client_destroy(client);
            return 21;
        }
    }
    // The dispatcher here is inline, so the status callback can run before
    // sip_client_send_message returns. The id the caller holds must already be set.
    // A burst keeps the engine thread awake, which makes the early callback likely.
    {
        std::string self_uri = std::string("sip:1001@") + asterisk_host + ":" + std::to_string(asterisk_port);
        const uint8_t b[] = {'x'};
        const size_t burst = 3000;
        std::vector<uint64_t> outs(burst, 0);
        g_caller_ids.store(outs.data());
        g_caller_ids_count.store(burst);
        // Busy threads make the sending thread lose the CPU between post and return.
        std::atomic<bool> spin{true};
        std::vector<std::thread> spinners;
        for (unsigned t = 0; t < std::thread::hardware_concurrency() * 2; t++) {
            spinners.emplace_back([&spin] { while (spin.load(std::memory_order_relaxed)) {} });
        }
        for (size_t i = 0; i < burst; i++) {
            CHECK(sip_client_send_message(client, self_uri.c_str(), "text/plain", b, sizeof(b), &outs[i]) == 0);
        }
        spin.store(false);
        for (auto &t : spinners) t.join();
        MessageStatusEvent last{};
        CHECK(wait_for_message_status(outs[burst - 1], 10, last));
        for (size_t i = 0; i < burst; i++) {
            MessageStatusEvent ev{};
            CHECK(wait_for_message_status(outs[i], 10, ev));
        }
        g_caller_ids.store(nullptr);
        size_t early = 0;
        {
            std::lock_guard<std::mutex> lock(g_reg_mutex);
            for (const auto &ev : g_msg_status) {
                if (!ev.id_known_to_caller) early++;
            }
        }
        if (early != 0) {
            std::printf("FAIL: %zu of %zu status callbacks fired before the caller had the message id\n", early, burst);
            sip_client_destroy(client);
            return 27;
        }
    }
    std::printf("PASS: Message input validation (EINVAL on bad input, FAILED without an account)\n");

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

    // Test 2b: SIP MESSAGE round trip through Asterisk to our own AOR
    std::printf("--- Test 2b: SIP MESSAGE round trip (1001 -> 1001 via Asterisk) ---\n");
    {
        std::string self_uri = std::string("sip:1001@") + asterisk_host + ":" + std::to_string(asterisk_port);
        const std::string text = "hello from sip_core \xC3\xA9";
        uint64_t id = send_text(client, self_uri, "text/plain", text);

        MessageStatusEvent st{};
        if (!wait_for_message_status(id, 10, st) || st.status != SIP_MESSAGE_STATUS_SENT ||
            st.code < 200 || st.code > 299) {
            std::printf("FAIL: expected SENT 2xx for message %llu (status=%d code=%d reason='%s')\n",
                         (unsigned long long)id, st.status, st.code, st.reason.c_str());
            sip_client_destroy(client);
            return 22;
        }
        ReceivedMessage rx{};
        if (!wait_for_received_body(text, 10, rx)) {
            std::printf("FAIL: message body not received back\n");
            sip_client_destroy(client);
            return 23;
        }
        if (rx.content_type != "text/plain" || rx.from.find("sip:1001@") != 0) {
            std::printf("FAIL: received from='%s' content_type='%s'\n", rx.from.c_str(), rx.content_type.c_str());
            sip_client_destroy(client);
            return 24;
        }
        std::printf("PASS: Message round trip (id=%llu status=SENT code=%d from=%s type=%s)\n",
                    (unsigned long long)id, st.code, rx.from.c_str(), rx.content_type.c_str());

        // Rich content: Asterisk only relays text/plain (it answers 415), so a raw UDP peer in
        // this process receives the MESSAGE, checks its Content-Type and answers 200 OK.
        const std::string json = "{\"kind\":\"rich\"}";
        int peer_port = 0;
        int peer_fd = open_raw_peer(peer_port);
        CHECK(peer_fd >= 0);
        std::string raw_request;
        std::thread peer_thread([peer_fd, &raw_request]() { raw_request = raw_peer_answer_one(peer_fd, 8000); });
        std::string peer_uri = "sip:peer@127.0.0.1:" + std::to_string(peer_port);
        uint64_t json_id = send_text(client, peer_uri, "application/json", json);
        MessageStatusEvent jst{};
        bool json_status = wait_for_message_status(json_id, 10, jst);
        peer_thread.join();
        close(peer_fd);
        if (!json_status || jst.status != SIP_MESSAGE_STATUS_SENT || jst.code != 200 ||
            raw_request.find("\r\nContent-Type: application/json\r\n") == std::string::npos ||
            raw_request.find("\r\n\r\n" + json) == std::string::npos) {
            std::printf("FAIL: expected SENT 200 for application/json (status=%d code=%d reason='%s') request:\n%s\n",
                         jst.status, jst.code, jst.reason.c_str(), raw_request.c_str());
            sip_client_destroy(client);
            return 25;
        }
        std::printf("PASS: Rich content type application/json sent with its Content-Type (status=SENT code=%d)\n",
                    jst.code);

        // Unknown peer: Asterisk has no route for 7777 in the messages context.
        std::string unknown_uri = std::string("sip:7777@") + asterisk_host + ":" + std::to_string(asterisk_port);
        uint64_t bad_id = send_text(client, unknown_uri, "text/plain", "nobody home");
        MessageStatusEvent bst{};
        if (!wait_for_message_status(bad_id, 10, bst) || bst.status != SIP_MESSAGE_STATUS_FAILED ||
            bst.code < 400) {
            std::printf("FAIL: expected FAILED 4xx for unknown peer (status=%d code=%d reason='%s')\n",
                         bst.status, bst.code, bst.reason.c_str());
            sip_client_destroy(client);
            return 26;
        }
        std::printf("PASS: Unknown peer message FAILED (code=%d reason='%s')\n", bst.code, bst.reason.c_str());
    }

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

    // Test 6: a MESSAGE still waiting for a response when the client is destroyed must be
    // reported FAILED, and its bookkeeping must be released.
    std::printf("--- Test 6: pending MESSAGE at client destroy ---\n");
    {
        int peer_port = 0;
        int peer_fd = open_raw_peer(peer_port);
        CHECK(peer_fd >= 0);  // a peer that never answers
        sip_client_t *c2 = sip_client_create(&callbacks, nullptr);
        CHECK(c2 != nullptr);
        {
            std::lock_guard<std::mutex> lock(g_reg_mutex);
            g_msg_status.clear();  // message ids restart at 1 for a new client
        }
        CHECK(sip_client_init(c2, config_dir) == 0);
        CHECK(sip_client_set_message_callbacks(c2, &msg_callbacks) == 0);
        CHECK(sip_client_register(c2, "1001", "127.0.0.1", "test1001", "udp", peer_port) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string uri = "sip:1002@127.0.0.1:" + std::to_string(peer_port);
        uint64_t pending_id = send_text(c2, uri, "text/plain", "never answered");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        {
            std::lock_guard<std::mutex> lock(g_reg_mutex);
            for (const auto &ev : g_msg_status) CHECK(ev.id != pending_id);  // still pending
        }
        sip_client_destroy(c2);
        MessageStatusEvent pst{};
        if (!wait_for_message_status(pending_id, 2, pst) || pst.status != SIP_MESSAGE_STATUS_FAILED) {
            std::printf("FAIL: pending message %llu got no FAILED status at destroy\n", (unsigned long long)pending_id);
            close(peer_fd);
            return 28;
        }
        close(peer_fd);
        std::printf("PASS: pending MESSAGE reported FAILED at destroy (reason='%s')\n", pst.reason.c_str());
    }

    std::printf("ALL_REGISTRATION_TESTS_PASSED\n");
    return 0;
}
