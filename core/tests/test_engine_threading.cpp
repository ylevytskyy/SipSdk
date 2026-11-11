// Thread-safety tests for the engine's registration state and audio driver
// config (RES-10). Run under ThreadSanitizer to check the data-race tests;
// a normal build checks the behavior only.

#include "sip_abi.h"

#include <re.h>
#include <baresip.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            std::fflush(nullptr);                                                 \
            /* _Exit: a held SIP thread must not see static destructors run. */  \
            std::_Exit(1);                                                        \
        }                                                                         \
    } while (0)

// MARK: - Fake registrar

static std::string sip_header(const std::string &msg, const std::string &name)
{
    std::string prefix = name + ":";
    size_t pos = 0;
    while (pos < msg.size()) {
        size_t end = msg.find("\r\n", pos);
        if (end == std::string::npos) end = msg.size();
        std::string line = msg.substr(pos, end - pos);
        if (line.size() >= prefix.size()) {
            bool match = true;
            for (size_t i = 0; i < prefix.size(); i++) {
                if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)prefix[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t value = prefix.size();
                while (value < line.size() && line[value] == ' ') value++;
                return line.substr(value);
            }
        }
        pos = end + 2;
    }
    return "";
}

/// Answers REGISTER with 200 OK on a free UDP port of 127.0.0.1 while `answering` is true.
class FakeRegistrar {
public:
    std::atomic<bool> answering{true};

    ~FakeRegistrar() { stop(); }

    bool start()
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(m_sock, (sockaddr *)&addr, sizeof(addr)) != 0) return false;
        socklen_t len = sizeof(addr);
        if (getsockname(m_sock, (sockaddr *)&addr, &len) != 0) return false;
        m_port = ntohs(addr.sin_port);
        timeval timeout{0, 100000};
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        m_running = true;
        m_thread = std::thread([this]() { serve(); });
        return true;
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_sock >= 0) {
            close(m_sock);
            m_sock = -1;
        }
    }

    int port() const { return m_port; }

private:
    void serve()
    {
        char buf[8192];
        while (m_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            ssize_t n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, (sockaddr *)&peer, &peer_len);
            if (n <= 0) continue;
            std::string msg(buf, (size_t)n);
            if (msg.rfind("REGISTER", 0) != 0 || !answering) continue;

            std::string to = sip_header(msg, "To");
            if (to.find(";tag=") == std::string::npos) to += ";tag=registrar";
            std::string resp = "SIP/2.0 200 OK\r\n"
                               "Via: " + sip_header(msg, "Via") + "\r\n"
                               "From: " + sip_header(msg, "From") + "\r\n"
                               "To: " + to + "\r\n"
                               "Call-ID: " + sip_header(msg, "Call-ID") + "\r\n"
                               "CSeq: " + sip_header(msg, "CSeq") + "\r\n"
                               "Contact: " + sip_header(msg, "Contact") + "\r\n"
                               "Expires: 3600\r\n"
                               "Content-Length: 0\r\n\r\n";
            sendto(m_sock, resp.c_str(), resp.size(), 0, (sockaddr *)&peer, peer_len);
        }
    }

    int m_sock{-1};
    int m_port{0};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

// MARK: - Engine callbacks

static std::mutex g_mutex;
static std::condition_variable g_cv;
static std::vector<sip_reg_state_t> g_reg_states;  // guarded by g_mutex
static bool g_hold_sip_thread = false;             // guarded by g_mutex
static bool g_sip_thread_held = false;             // guarded by g_mutex

/// The engine calls this on its SIP thread. While a test asks, it holds the
/// SIP thread here, so the test can act while that thread is busy.
static void dispatch_on_sip_thread(void (*task)(void *arg), void *arg, void *user_data)
{
    (void)user_data;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_hold_sip_thread) {
            g_sip_thread_held = true;
            g_cv.notify_all();
            g_cv.wait(lock, []() { return !g_hold_sip_thread; });
            g_sip_thread_held = false;
        }
    }
    if (task) task(arg);
}

static void on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    (void)client;
    (void)code;
    (void)reason;
    (void)user_data;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_reg_states.push_back(state);
    g_cv.notify_all();
}

static size_t reg_state_count()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_reg_states.size();
}

/// Waits until a registration event at index `from` or later has `state`.
static bool wait_for_reg_state(sip_reg_state_t state, size_t from, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_cv.wait_for(lock, timeout, [&]() {
        for (size_t i = from; i < g_reg_states.size(); i++) {
            if (g_reg_states[i] == state) return true;
        }
        return false;
    });
}

static void hold_sip_thread_on_next_event()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_sip_thread = true;
}

static bool wait_until_sip_thread_held(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_cv.wait_for(lock, timeout, []() { return g_sip_thread_held; });
}

static void release_sip_thread()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hold_sip_thread = false;
    g_cv.notify_all();
}

// MARK: - Helpers

static std::string g_config_dir;

static sip_client_t *start_client()
{
    sip_callbacks_t callbacks = {};
    callbacks.dispatch_to_main = dispatch_on_sip_thread;
    callbacks.on_reg_state = on_reg_state;
    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    CHECK(client != nullptr, "sip_client_create returned null");
    CHECK(sip_client_init(client, g_config_dir.c_str()) == 0, "sip_client_init failed");
    return client;
}

static int register_with(sip_client_t *client, int port)
{
    sip_account_config_t account = {};
    account.user = "1001";
    account.domain = "127.0.0.1";
    account.pwd = "";
    account.transport = "udp";
    account.port = port;
    return sip_client_register_account(client, &account);
}

static bool wait_until_registered_is(sip_client_t *client, bool expected, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sip_client_is_registered(client) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return sip_client_is_registered(client) == expected;
}

// MARK: - Tests

// Given an account registered with a registrar,
// when the app registers again and the registrar stops answering,
// then is_registered reports false until a new registration succeeds.
static void test_reregister_reports_not_registered_until_it_succeeds()
{
    std::printf("--- Test 1: a re-register reports not registered until it succeeds ---\n");
    FakeRegistrar registrar;
    CHECK(registrar.start(), "fake registrar did not start");
    sip_client_t *client = start_client();

    size_t from = reg_state_count();
    CHECK(register_with(client, registrar.port()) == 0, "first register failed");
    CHECK(wait_for_reg_state(SIP_REG_STATE_REGISTERED, from, std::chrono::seconds(5)), "first registration never succeeded");
    CHECK(wait_until_registered_is(client, true, std::chrono::seconds(2)), "is_registered is false after a successful registration");

    registrar.answering = false;
    CHECK(register_with(client, registrar.port()) == 0, "second register failed");
    CHECK(wait_until_registered_is(client, false, std::chrono::seconds(3)), "is_registered stayed true while the new registration was unanswered");

    sip_client_destroy(client);
    std::printf("PASS\n");
}

// Given a running client,
// when one thread re-registers the account many times while another thread reads is_registered,
// then the reads do not race with the SIP thread replacing the account (ThreadSanitizer stays quiet).
static void test_is_registered_does_not_race_with_reregister()
{
    std::printf("--- Test 2: is_registered while the account is replaced ---\n");
    FakeRegistrar registrar;
    CHECK(registrar.start(), "fake registrar did not start");
    sip_client_t *client = start_client();

    std::atomic<bool> reading{true};
    std::thread reader([&]() {
        while (reading) {
            (void)sip_client_is_registered(client);
        }
    });
    for (int i = 0; i < 30; i++) {
        CHECK(register_with(client, registrar.port()) == 0, "register failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    reading = false;
    reader.join();

    sip_client_destroy(client);
    std::printf("PASS\n");
}

/// Starts a registration the registrar never answers, so the only events come
/// from tasks on the SIP thread, and holds the SIP thread on the first one.
static void hold_sip_thread(sip_client_t *client, FakeRegistrar &registrar)
{
    registrar.answering = false;
    hold_sip_thread_on_next_event();
    CHECK(register_with(client, registrar.port()) == 0, "register failed");
    CHECK(wait_until_sip_thread_held(std::chrono::seconds(3)), "the SIP thread was never held");
}

// Given a running client whose SIP thread is busy,
// when the app sets the audio driver,
// then baresip's audio config changes only on the SIP thread, after it is free.
static void test_set_audio_driver_changes_config_on_sip_thread()
{
    std::printf("--- Test 3: set_audio_driver changes the config on the SIP thread ---\n");
    FakeRegistrar registrar;
    CHECK(registrar.start(), "fake registrar did not start");
    sip_client_t *client = start_client();
    hold_sip_thread(client, registrar);

    // The SIP thread is held, so reading its config here does not race.
    std::string src_before = conf_config()->audio.src_mod;
    std::string play_before = conf_config()->audio.play_mod;
    CHECK(sip_client_set_audio_driver(client, "test-src", "", "test-play", "") == 0, "set_audio_driver failed");
    CHECK(src_before == conf_config()->audio.src_mod, "audio source config changed on the caller thread");
    CHECK(play_before == conf_config()->audio.play_mod, "audio player config changed on the caller thread");

    // Destroying the client runs the queued change and joins the SIP thread.
    release_sip_thread();
    sip_client_destroy(client);
    CHECK(std::strcmp(conf_config()->audio.src_mod, "test-src") == 0, "audio source change was never applied");
    CHECK(std::strcmp(conf_config()->audio.play_mod, "test-play") == 0, "audio player change was never applied");
    std::printf("PASS\n");
}

// Given a running client whose SIP thread is busy,
// when the app sets the audio driver and reads it back,
// then it reads the driver it just set.
static void test_get_audio_driver_returns_driver_just_set()
{
    std::printf("--- Test 4: get_audio_driver returns the driver just set ---\n");
    FakeRegistrar registrar;
    CHECK(registrar.start(), "fake registrar did not start");
    sip_client_t *client = start_client();
    hold_sip_thread(client, registrar);

    CHECK(sip_client_set_audio_driver(client, "next-src", "", "next-play", "") == 0, "set_audio_driver failed");
    char src[64] = {};
    char play[64] = {};
    CHECK(sip_client_get_audio_driver(client, src, sizeof(src), play, sizeof(play)) == 0, "get_audio_driver failed");
    CHECK(std::strcmp(src, "next-src") == 0, "get_audio_driver did not return the source just set");
    CHECK(std::strcmp(play, "next-play") == 0, "get_audio_driver did not return the player just set");

    release_sip_thread();
    sip_client_destroy(client);
    std::printf("PASS\n");
}

// Given a client that has not started,
// when the app sets the audio driver,
// then baresip's audio config holds that driver.
static void test_set_audio_driver_before_start_writes_config()
{
    std::printf("--- Test 5: set_audio_driver before start writes the config ---\n");
    sip_callbacks_t callbacks = {};
    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    CHECK(client != nullptr, "sip_client_create returned null");

    CHECK(sip_client_set_audio_driver(client, "early-src", "", "early-play", "") == 0, "set_audio_driver failed");
    CHECK(std::strcmp(conf_config()->audio.src_mod, "early-src") == 0, "audio source config was not written");
    CHECK(std::strcmp(conf_config()->audio.play_mod, "early-play") == 0, "audio player config was not written");

    sip_client_destroy(client);
    std::printf("PASS\n");
}

// Given a client that has just started, with no driver set by the app,
// when the app reads the audio driver,
// then it reads the drivers baresip was configured with at start.
static void test_get_audio_driver_returns_configured_drivers_after_start()
{
    std::printf("--- Test 6: get_audio_driver returns the configured drivers after start ---\n");
    FakeRegistrar registrar;
    CHECK(registrar.start(), "fake registrar did not start");
    sip_client_t *client = start_client();
    hold_sip_thread(client, registrar);

    // The SIP thread is held, so reading its config here does not race.
    std::string configured_src = conf_config()->audio.src_mod;
    std::string configured_play = conf_config()->audio.play_mod;
#if defined(__APPLE__)
    // Engine start fills in audiounit when the config names no module.
    CHECK(!configured_src.empty() && !configured_play.empty(), "no audio driver configured at start");
#endif
    char src[64] = {};
    char play[64] = {};
    CHECK(sip_client_get_audio_driver(client, src, sizeof(src), play, sizeof(play)) == 0, "get_audio_driver failed");
    CHECK(configured_src == src, "get_audio_driver did not return the configured source");
    CHECK(configured_play == play, "get_audio_driver did not return the configured player");

    release_sip_thread();
    sip_client_destroy(client);
    std::printf("PASS\n");
}

/// With no arguments, runs every test; otherwise only the test numbers given.
static bool should_run(int argc, char *argv[], int number)
{
    if (argc < 2) return true;
    for (int i = 1; i < argc; i++) {
        if (std::atoi(argv[i]) == number) return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    char dir_template[] = "/tmp/sipsdk-engine-threading-XXXXXX";
    CHECK(mkdtemp(dir_template) != nullptr, "mkdtemp failed");
    g_config_dir = dir_template;
    std::printf("==> Engine threading tests (config_dir=%s)\n", g_config_dir.c_str());

    if (should_run(argc, argv, 1)) test_reregister_reports_not_registered_until_it_succeeds();
    if (should_run(argc, argv, 2)) test_is_registered_does_not_race_with_reregister();
    if (should_run(argc, argv, 3)) test_set_audio_driver_changes_config_on_sip_thread();
    if (should_run(argc, argv, 4)) test_get_audio_driver_returns_driver_just_set();
    if (should_run(argc, argv, 5)) test_set_audio_driver_before_start_writes_config();
    if (should_run(argc, argv, 6)) test_get_audio_driver_returns_configured_drivers_after_start();

    std::printf("ALL ENGINE THREADING TESTS PASSED\n");
    return 0;
}
