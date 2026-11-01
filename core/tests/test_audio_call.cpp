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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

struct CallEvent {
    sip_call_t *call;
    sip_call_state_t state;
    int code;
    std::string reason;
    bool was_main_thread;
};

struct MediaEvent {
    sip_call_t *call;
    int width;
    int height;
    bool on_hold;
    bool is_muted;
};

struct IncomingCallEvent {
    sip_client_t *client{nullptr};
    sip_call_t *call{nullptr};
    std::string remote_uri;
    std::string display_name;
    bool has_video{false};
    bool is_outgoing{false};
    bool was_main_thread{false};
};

static std::mutex g_call_mutex;
static std::condition_variable g_call_cv;
static std::vector<CallEvent> g_call_events;
static std::vector<MediaEvent> g_media_events;
static std::atomic<int> g_main_dispatch_count{0};

static std::mutex g_incoming_mutex;
static std::condition_variable g_incoming_cv;
static std::vector<IncomingCallEvent> g_incoming_events;

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
    if (task) {
        task(arg);
    }
}

static void test_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    (void)client;
    (void)state;
    (void)code;
    (void)reason;
    (void)user_data;
}

static void test_on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
{
    (void)user_data;
    CallEvent ev;
    ev.call = call;
    ev.state = state;
    ev.code = code;
    ev.reason = reason ? reason : "";
    ev.was_main_thread = is_current_thread_main();

    {
        std::lock_guard<std::mutex> lock(g_call_mutex);
        g_call_events.push_back(ev);
    }
    g_call_cv.notify_all();
}

static void test_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
{
    (void)user_data;
    MediaEvent ev{call, width, height, on_hold, is_muted};
    std::lock_guard<std::mutex> lock(g_call_mutex);
    g_media_events.push_back(ev);
}

static bool wait_for_call_state(sip_call_t *call, sip_call_state_t target_state, int timeout_sec, CallEvent &matched_event)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_call_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_call_events) {
            if ((call == nullptr || ev.call == call) && ev.state == target_state) {
                matched_event = ev;
                return true;
            }
        }
        g_call_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

static void clear_call_events()
{
    std::lock_guard<std::mutex> lock(g_call_mutex);
    g_call_events.clear();
}

static void test_on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data)
{
    (void)user_data;
    IncomingCallEvent ev;
    ev.client = client;
    ev.call = call;
    ev.remote_uri = sip_call_get_remote_uri(call);
    ev.display_name = sip_call_get_peer_display_name(call);
    ev.has_video = sip_call_has_video(call);
    ev.is_outgoing = sip_call_is_outgoing(call);
    ev.was_main_thread = is_current_thread_main();

    {
        std::lock_guard<std::mutex> lock(g_incoming_mutex);
        g_incoming_events.push_back(ev);
    }
    g_incoming_cv.notify_all();
}

static bool wait_for_incoming_call(int timeout_sec, IncomingCallEvent &matched_event)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_incoming_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!g_incoming_events.empty()) {
            matched_event = g_incoming_events.back();
            return true;
        }
        g_incoming_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

static void clear_incoming_events()
{
    std::lock_guard<std::mutex> lock(g_incoming_mutex);
    g_incoming_events.clear();
}

static std::string extract_header(const std::string &msg, const std::string &name)
{
    std::string prefix = name + ":";
    size_t pos = 0;
    while (pos < msg.length()) {
        size_t next_crlf = msg.find("\r\n", pos);
        if (next_crlf == std::string::npos) {
            next_crlf = msg.length();
        }
        std::string line = msg.substr(pos, next_crlf - pos);
        if (line.length() >= prefix.length()) {
            std::string line_pre = line.substr(0, prefix.length());
            bool match = true;
            for (size_t i = 0; i < prefix.length(); ++i) {
                if (::tolower(static_cast<unsigned char>(line_pre[i])) != ::tolower(static_cast<unsigned char>(prefix[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string val = line.substr(prefix.length());
                size_t first = val.find_first_not_of(" \t");
                size_t last = val.find_last_not_of(" \t");
                if (first != std::string::npos && last != std::string::npos) {
                    return val.substr(first, last - first + 1);
                }
                return "";
            }
        }
        pos = next_crlf + 2;
    }
    return "";
}

class LoopbackCaller {
public:
    explicit LoopbackCaller(uint16_t port)
        : m_port(port)
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, 0);
        assert(m_sock >= 0);
        int reuse = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        int rc = bind(m_sock, (struct sockaddr *)&addr, sizeof(addr));
        assert(rc == 0);
    }

    ~LoopbackCaller()
    {
        stop();
    }

    void start()
    {
        m_running = true;
        m_thread = std::thread(&LoopbackCaller::serve, this);
    }

    void stop()
    {
        if (m_running.exchange(false)) {
            if (m_sock >= 0) {
                shutdown(m_sock, SHUT_RDWR);
                close(m_sock);
                m_sock = -1;
            }
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }
    }

    bool wait_for_registration(int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_registered) return true;
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    void send_invite(const std::string &call_id, uint32_t cseq, const std::string &display_name, bool with_video)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_status_code = 0;
        m_last_response_body.clear();
        m_bye_received = false;
        m_last_call_id = call_id;
        m_last_cseq = cseq;

        std::string sdp = "v=0\r\n"
                          "o=bob 1 1 IN IP4 127.0.0.1\r\n"
                          "s=Incoming Audio Call\r\n"
                          "c=IN IP4 127.0.0.1\r\n"
                          "t=0 0\r\n"
                          "m=audio 16000 RTP/AVP 0 8\r\n"
                          "a=rtpmap:0 PCMU/8000\r\n"
                          "a=rtpmap:8 PCMA/8000\r\n"
                          "a=sendrecv\r\n";
        if (with_video) {
            sdp += "m=video 16002 RTP/AVP 96\r\n"
                   "a=rtpmap:96 H264/90000\r\n"
                   "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
                   "a=sendrecv\r\n";
        }

        std::string ruri = m_client_contact_uri.empty() ?
            ("sip:1001@127.0.0.1:" + std::to_string(ntohs(m_client_addr.sin_port))) :
            m_client_contact_uri;

        char buf[2048];
        int len = std::snprintf(buf, sizeof(buf),
            "INVITE %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-inv-%u;rport\r\n"
            "Max-Forwards: 70\r\n"
            "From: \"%s\" <sip:bob@127.0.0.1:%u>;tag=caller-%u\r\n"
            "To: <sip:1001@127.0.0.1:%u>\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %u INVITE\r\n"
            "Contact: <sip:bob@127.0.0.1:%u>\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            ruri.c_str(),
            m_port, cseq,
            display_name.c_str(), m_port, cseq,
            m_port,
            call_id.c_str(),
            cseq,
            m_port,
            sdp.length(), sdp.c_str());

        sendto(m_sock, buf, len, 0, (struct sockaddr *)&m_client_addr, sizeof(m_client_addr));
    }

    int wait_for_response(int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_last_status_code > 0) {
                return m_last_status_code;
            }
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return m_last_status_code;
    }

    bool wait_for_bye(int timeout_sec)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_bye_received) return true;
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    std::string last_response_body()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_last_response_body;
    }

private:
    void serve()
    {
        char buf[8192];
        while (m_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            ssize_t n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
            if (n <= 0) {
                continue;
            }
            buf[n] = '\0';
            std::string msg(buf, n);

            // 1. REGISTER
            if (msg.rfind("REGISTER", 0) == 0) {
                std::string contact = extract_header(msg, "Contact");
                std::string via = extract_header(msg, "Via");
                std::string from = extract_header(msg, "From");
                std::string to = extract_header(msg, "To");
                if (to.find(";tag=") == std::string::npos) {
                    to += ";tag=srv94";
                }
                std::string call_id = extract_header(msg, "Call-ID");
                std::string cseq = extract_header(msg, "CSeq");

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_client_addr = peer;
                    auto start = contact.find('<');
                    auto end = contact.find('>');
                    if (start != std::string::npos && end != std::string::npos && end > start) {
                        m_client_contact_uri = contact.substr(start + 1, end - start - 1);
                    } else if (!contact.empty()) {
                        m_client_contact_uri = contact;
                    }
                    m_registered = true;
                }
                m_cv.notify_all();

                std::string resp = "SIP/2.0 200 OK\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Contact: " + contact + "\r\n"
                                   "Content-Length: 0\r\n\r\n";
                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);
                continue;
            }

            // 2. SIP responses
            if (msg.rfind("SIP/2.0 ", 0) == 0) {
                int scode = std::atoi(msg.c_str() + 8);
                if (scode == 180 || scode == 100) {
                    continue;
                }

                std::string resp_call_id = extract_header(msg, "Call-ID");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_last_call_id.empty() && resp_call_id != m_last_call_id) {
                        continue;
                    }
                    m_last_status_code = scode;
                    auto body_pos = msg.find("\r\n\r\n");
                    if (body_pos != std::string::npos) {
                        m_last_response_body = msg.substr(body_pos + 4);
                    }
                }
                m_cv.notify_all();

                if (scode == 200) {
                    std::string cseq = extract_header(msg, "CSeq");
                    if (cseq.find("INVITE") != std::string::npos) {
                        std::string to = extract_header(msg, "To");
                        std::string from = extract_header(msg, "From");
                        std::string call_id = extract_header(msg, "Call-ID");
                        std::string ruri = m_client_contact_uri.empty() ?
                            ("sip:1001@127.0.0.1:" + std::to_string(ntohs(peer.sin_port))) :
                            m_client_contact_uri;

                        char ack_buf[1024];
                        int ack_len = std::snprintf(ack_buf, sizeof(ack_buf),
                            "ACK %s SIP/2.0\r\n"
                            "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-ack-%u;rport\r\n"
                            "Max-Forwards: 70\r\n"
                            "From: %s\r\n"
                            "To: %s\r\n"
                            "Call-ID: %s\r\n"
                            "CSeq: %u ACK\r\n"
                            "Content-Length: 0\r\n\r\n",
                            ruri.c_str(),
                            m_port, m_last_cseq,
                            from.c_str(),
                            to.c_str(),
                            call_id.c_str(),
                            m_last_cseq);
                        sendto(m_sock, ack_buf, ack_len, 0, (struct sockaddr *)&peer, peer_len);
                    }
                }
                continue;
            }

            // 3. BYE
            if (msg.rfind("BYE", 0) == 0) {
                std::string call_id = extract_header(msg, "Call-ID");
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_last_call_id.empty() && call_id != m_last_call_id) {
                        continue;
                    }
                }
                std::string via = extract_header(msg, "Via");
                std::string from = extract_header(msg, "From");
                std::string to = extract_header(msg, "To");
                std::string cseq = extract_header(msg, "CSeq");

                std::string resp = "SIP/2.0 200 OK\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Content-Length: 0\r\n\r\n";
                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_bye_received = true;
                }
                m_cv.notify_all();
                continue;
            }
        }
    }

    int m_sock{-1};
    uint16_t m_port{0};
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    sockaddr_in m_client_addr{};
    std::string m_client_contact_uri;
    std::atomic<bool> m_registered{false};
    int m_last_status_code{0};
    std::string m_last_response_body;
    std::string m_last_call_id;
    uint32_t m_last_cseq{0};
    std::atomic<bool> m_bye_received{false};
};

int main(int argc, char *argv[])
{
    const char *config_dir = (argc > 1) ? argv[1] : nullptr;
    const char *asterisk_host = (argc > 2) ? argv[2] : "127.0.0.1";
    int asterisk_port = (argc > 3) ? std::atoi(argv[3]) : 5060;

    std::printf("==> Running sip_core C ABI audio calling tests (host=%s:%d, config_dir=%s)\n",
                asterisk_host, asterisk_port, config_dir ? config_dir : "null");

    sip_callbacks_t callbacks = {};
    callbacks.dispatch_to_main = test_dispatch_to_main;
    callbacks.on_reg_state = test_on_reg_state;
    callbacks.on_incoming_call = test_on_incoming_call;
    callbacks.on_call_state = test_on_call_state;
    callbacks.on_call_media = test_on_call_media;

    // --- Test 1: Client initialization & account registration ---
    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    assert(client != nullptr);

    int rc = sip_client_init(client, config_dir);
    assert(rc == 0);
    std::printf("PASS: sip_client_init succeeded\n");

    // Register extension 1001 with Asterisk
    rc = sip_client_register(client, "1001", asterisk_host, "test1001", "udp", asterisk_port);
    assert(rc == 0);

    // Wait for registration
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // --- Test 2: Outgoing audio call to Asterisk Echo (9001) ---
    clear_call_events();
    std::string echo_uri = "sip:9001@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
    sip_call_t *call = sip_call_make(client, echo_uri.c_str(), false);
    assert(call != nullptr);
    assert(sip_call_is_outgoing(call) == true);
    assert(sip_call_has_video(call) == false);
    assert(sip_call_is_closed(call) == false);
    std::printf("PASS: sip_call_make created outgoing audio call (id=%s, remote=%s)\n",
                sip_call_get_id(call), sip_call_get_remote_uri(call));

    CallEvent estab_ev;
    bool established = wait_for_call_state(call, SIP_CALL_STATE_ESTABLISHED, 8, estab_ev);
    if (!established) {
        std::printf("FAIL: Outgoing audio call to 9001 did not establish within 8 seconds\n");
        return 1;
    }
    std::printf("PASS: Audio call established (code=%d, reason='%s')\n",
                estab_ev.code, estab_ev.reason.c_str());

    // Let call advance for duration tracking
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    uint32_t dur = sip_call_get_duration(call);
    std::printf("PASS: Active call duration timer: %u sec\n", dur);
    assert(dur >= 1);

    // --- Test 2b: Telephony controls (Mute, Hold, DTMF) ---
    std::printf("--- Test 2b: Telephony controls (Mute, Hold, DTMF) ---\n");

    // 1. Microphone mute & unmute
    rc = sip_call_set_muted(call, true);
    assert(rc == 0);
    assert(sip_call_is_muted(call) == true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("PASS: Call microphone muted\n");

    rc = sip_call_set_muted(call, false);
    assert(rc == 0);
    assert(sip_call_is_muted(call) == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("PASS: Call microphone unmuted\n");

    // 2. Client-wide microphone mute
    rc = sip_client_set_mic_muted(client, true);
    assert(rc == 0);
    assert(sip_client_is_mic_muted(client) == true);
    rc = sip_client_set_mic_muted(client, false);
    assert(rc == 0);
    assert(sip_client_is_mic_muted(client) == false);
    std::printf("PASS: Client-wide microphone mute toggle\n");

    // 3. Hold and resume
    rc = sip_call_set_hold(call, true);
    assert(rc == 0);
    CallEvent hold_ev;
    bool held = wait_for_call_state(call, SIP_CALL_STATE_HOLD, 5, hold_ev);
    assert(held);
    assert(sip_call_is_on_hold(call) == true);
    std::printf("PASS: Call placed on hold\n");

    rc = sip_call_set_hold(call, false);
    assert(rc == 0);
    CallEvent resume_ev;
    bool resumed = wait_for_call_state(call, SIP_CALL_STATE_ESTABLISHED, 5, resume_ev);
    assert(resumed);
    assert(sip_call_is_on_hold(call) == false);
    std::printf("PASS: Call resumed from hold\n");

    // 4. DTMF transmission
    rc = sip_call_send_dtmf(call, "123#*");
    assert(rc == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("PASS: DTMF digits transmitted (123#*)\n");

    // 5. Multi-call session isolation
    std::string music_uri = "sip:9002@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
    sip_call_t *call2 = sip_call_make(client, music_uri.c_str(), false);
    assert(call2 != nullptr);
    assert(call2 != call);
    assert(sip_call_is_on_hold(call2) == false);
    assert(sip_call_is_muted(call2) == false);
    std::printf("PASS: Second call session isolated from first call\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sip_call_hangup(call2, 0, "Hangup call2");
    CallEvent call2_close_ev;
    wait_for_call_state(call2, SIP_CALL_STATE_CLOSED, 5, call2_close_ev);
    sip_call_destroy(call2);

    // Hangup the call
    rc = sip_call_hangup(call, 0, "Hangup");
    assert(rc == 0);

    CallEvent close_ev;
    bool closed = wait_for_call_state(call, SIP_CALL_STATE_CLOSED, 5, close_ev);
    if (!closed) {
        std::printf("FAIL: Call did not close cleanly after hangup\n");
        return 1;
    }
    std::printf("PASS: Audio call closed cleanly (closed=%d, final_duration=%u)\n",
                sip_call_is_closed(call), sip_call_get_duration(call));
    assert(sip_call_is_closed(call) == true);

    sip_call_destroy(call);

    // --- Test 3: Cancel outgoing call before answer ---
    clear_call_events();
    // Dial non-answering / dummy endpoint
    std::string dummy_uri = "sip:9999@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
    sip_call_t *cancel_call = sip_call_make(client, dummy_uri.c_str(), false);
    assert(cancel_call != nullptr);

    // Immediately cancel
    rc = sip_call_hangup(cancel_call, 0, "Cancel");
    assert(rc == 0);

    CallEvent cancel_close_ev;
    bool cancel_closed = wait_for_call_state(cancel_call, SIP_CALL_STATE_CLOSED, 5, cancel_close_ev);
    assert(cancel_closed);
    assert(sip_call_is_closed(cancel_call) == true);
    std::printf("PASS: Cancelled call cleanly terminated before answer\n");

    sip_call_destroy(cancel_call);

    // --- Test 4: UI thread dispatch count check ---
    assert(g_main_dispatch_count.load() > 0);
    std::printf("PASS: Main thread dispatcher invoked %d times for call events\n", g_main_dispatch_count.load());

    // --- Test 5: Incoming call signaling, caller metadata & 486 Busy Here Decline ---
    std::printf("--- Test 5: Incoming call signaling, caller metadata & 486 Busy Here Decline ---\n");
    clear_call_events();
    clear_incoming_events();

    LoopbackCaller caller(15094);
    caller.start();

    // Register client with LoopbackCaller on port 15094
    rc = sip_client_register(client, "1001", "127.0.0.1", "test1001", "udp", 15094);
    TEST_ASSERT(rc == 0, "sip_client_register failed for loopback caller");
    bool reg_ok = caller.wait_for_registration(5);
    TEST_ASSERT(reg_ok, "Client registration did not arrive at LoopbackCaller within 5 sec");
    std::printf("PASS: Client registered with LoopbackCaller\n");

    caller.send_invite("call-id-test5-busy", 10, "Bob Tester", false);

    IncomingCallEvent in_ev5;
    bool in_received = wait_for_incoming_call(5, in_ev5);
    TEST_ASSERT(in_received, "Incoming call event not received within 5 seconds");
    TEST_ASSERT(in_ev5.call != nullptr, "Incoming call pointer is null");
    TEST_ASSERT(in_ev5.client == client, "Incoming client does not match");
    TEST_ASSERT(in_ev5.is_outgoing == false, "Incoming call marked outgoing");
    TEST_ASSERT(in_ev5.has_video == false, "Incoming call marked has_video");
    TEST_ASSERT(g_main_dispatch_count.load() > 0, "Incoming callback not dispatched via main hook");
    TEST_ASSERT(in_ev5.display_name == "Bob Tester", "Caller display name does not match 'Bob Tester'");
    TEST_ASSERT(in_ev5.remote_uri.find("bob") != std::string::npos, "Caller remote URI does not contain 'bob'");
    std::printf("PASS: Incoming call delivered with caller display name '%s' and URI '%s'\n",
                in_ev5.display_name.c_str(), in_ev5.remote_uri.c_str());

    CallEvent in_state_ev;
    bool in_state_ok = wait_for_call_state(in_ev5.call, SIP_CALL_STATE_INCOMING, 5, in_state_ev);
    TEST_ASSERT(in_state_ok, "Call state did not transition to SIP_CALL_STATE_INCOMING");
    TEST_ASSERT(in_state_ev.code == 180, "Incoming call state code is not 180");

    // Decline with 486 Busy Here
    rc = sip_call_hangup(in_ev5.call, 486, "Busy Here");
    TEST_ASSERT(rc == 0, "sip_call_hangup(486) failed");

    int resp5 = caller.wait_for_response(5);
    std::printf("PASS: Loopback caller received SIP response %d\n", resp5);
    TEST_ASSERT(resp5 == 486, "Loopback caller did not receive 486 response");

    CallEvent close_ev5;
    bool closed5 = wait_for_call_state(in_ev5.call, SIP_CALL_STATE_CLOSED, 5, close_ev5);
    TEST_ASSERT(closed5, "Call did not transition to SIP_CALL_STATE_CLOSED after 486 decline");
    TEST_ASSERT(sip_call_is_closed(in_ev5.call) == true, "sip_call_is_closed returned false");
    sip_call_destroy(in_ev5.call);
    std::printf("PASS: Incoming call declined with 486 Busy Here verified\n");

    // --- Test 6: Incoming call & 603 Decline ---
    std::printf("--- Test 6: Incoming call & 603 Decline ---\n");
    clear_call_events();
    clear_incoming_events();

    caller.send_invite("call-id-test6-decline", 20, "Alice Boss", false);

    IncomingCallEvent in_ev6;
    bool in_received6 = wait_for_incoming_call(5, in_ev6);
    TEST_ASSERT(in_received6, "Incoming call 6 not received within 5 seconds");
    TEST_ASSERT(in_ev6.call != nullptr, "Incoming call 6 pointer is null");
    TEST_ASSERT(in_ev6.display_name == "Alice Boss", "Caller display name does not match 'Alice Boss'");

    // Decline with 603 Decline
    rc = sip_call_hangup(in_ev6.call, 603, "Decline");
    TEST_ASSERT(rc == 0, "sip_call_hangup(603) failed");

    int resp6 = caller.wait_for_response(5);
    std::printf("PASS: Loopback caller received SIP response %d\n", resp6);
    TEST_ASSERT(resp6 == 603, "Loopback caller did not receive 603 response");

    CallEvent close_ev6;
    bool closed6 = wait_for_call_state(in_ev6.call, SIP_CALL_STATE_CLOSED, 5, close_ev6);
    TEST_ASSERT(closed6, "Call did not transition to SIP_CALL_STATE_CLOSED after 603 decline");
    TEST_ASSERT(sip_call_is_closed(in_ev6.call) == true, "sip_call_is_closed returned false");
    sip_call_destroy(in_ev6.call);
    std::printf("PASS: Incoming call declined with 603 Decline verified\n");

    // --- Test 7: Incoming audio call answer, established lifecycle, duration & hangup ---
    std::printf("--- Test 7: Incoming audio call answer, established lifecycle, duration & hangup ---\n");
    clear_call_events();
    clear_incoming_events();

    caller.send_invite("call-id-test7-answer", 30, "Charlie Audio", false);

    IncomingCallEvent in_ev7;
    bool in_received7 = wait_for_incoming_call(5, in_ev7);
    TEST_ASSERT(in_received7, "Incoming call 7 not received within 5 seconds");
    TEST_ASSERT(in_ev7.call != nullptr, "Incoming call 7 pointer is null");
    TEST_ASSERT(in_ev7.display_name == "Charlie Audio", "Caller display name does not match 'Charlie Audio'");

    // Answer incoming call in audio-only mode
    rc = sip_call_answer(in_ev7.call, false);
    TEST_ASSERT(rc == 0, "sip_call_answer failed");

    int resp7 = caller.wait_for_response(5);
    std::printf("PASS: Loopback caller received SIP 200 OK answer (%d)\n", resp7);
    TEST_ASSERT(resp7 == 200, "Loopback caller did not receive 200 OK answer");

    std::string sdp7 = caller.last_response_body();
    TEST_ASSERT(sdp7.find("m=audio") != std::string::npos, "SDP does not contain audio media");
    std::printf("PASS: 200 OK contains negotiated audio SDP\n");

    CallEvent estab_ev7;
    bool estab7 = wait_for_call_state(in_ev7.call, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev7);
    TEST_ASSERT(estab7, "Call did not transition to SIP_CALL_STATE_ESTABLISHED");
    std::printf("PASS: Incoming call established (code=%d, reason='%s')\n",
                estab_ev7.code, estab_ev7.reason.c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    uint32_t dur7 = sip_call_get_duration(in_ev7.call);
    std::printf("PASS: Established incoming call duration: %u sec\n", dur7);
    TEST_ASSERT(dur7 >= 1, "Duration did not advance for established incoming call");

    // Hangup the established call
    rc = sip_call_hangup(in_ev7.call, 0, "Hangup");
    TEST_ASSERT(rc == 0, "sip_call_hangup failed");

    bool bye7 = caller.wait_for_bye(5);
    TEST_ASSERT(bye7, "Loopback caller did not receive BYE request");
    std::printf("PASS: Loopback caller received BYE request from client\n");

    CallEvent close_ev7;
    bool closed7 = wait_for_call_state(in_ev7.call, SIP_CALL_STATE_CLOSED, 5, close_ev7);
    TEST_ASSERT(closed7, "Call did not transition to SIP_CALL_STATE_CLOSED after BYE");
    TEST_ASSERT(sip_call_is_closed(in_ev7.call) == true, "sip_call_is_closed returned false");
    sip_call_destroy(in_ev7.call);
    std::printf("PASS: Incoming audio call answer & hangup lifecycle verified\n");

    caller.stop();

    // Teardown
    sip_client_destroy(client);
    std::printf("ALL_AUDIO_CALL_TESTS_PASSED\n");
    return 0;
}
