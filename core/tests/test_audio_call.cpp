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

static std::atomic<int> g_call_stats_count{0};
static sip_call_stats_t g_last_call_stats{};
static std::mutex g_stats_mutex;

static void test_on_call_stats(sip_call_t *call, const sip_call_stats_t *stats, void *user_data)
{
    (void)call;
    (void)user_data;
    if (stats) {
        std::lock_guard<std::mutex> lock(g_stats_mutex);
        g_last_call_stats = *stats;
        g_call_stats_count.fetch_add(1);
    }
}

struct TransferStatusEvent {
    sip_call_t *call{nullptr};
    int code{0};
    std::string reason;
};

struct TransferRequestEvent {
    sip_call_t *call{nullptr};
    std::string refer_to_uri;
    bool has_replaces{false};
};

static std::mutex g_transfer_mutex;
static std::condition_variable g_transfer_cv;
static std::vector<TransferStatusEvent> g_transfer_status_events;
static std::vector<TransferRequestEvent> g_transfer_request_events;

static void test_on_transfer_status(sip_call_t *call, int code, const char *reason, void *user_data)
{
    (void)user_data;
    std::lock_guard<std::mutex> lock(g_transfer_mutex);
    g_transfer_status_events.push_back({call, code, reason ? reason : ""});
    g_transfer_cv.notify_all();
}

static void test_on_transfer_request(sip_call_t *call, const char *refer_to_uri, bool has_replaces, void *user_data)
{
    (void)user_data;
    std::lock_guard<std::mutex> lock(g_transfer_mutex);
    g_transfer_request_events.push_back({call, refer_to_uri ? refer_to_uri : "", has_replaces});
    g_transfer_cv.notify_all();
}

static bool wait_for_transfer_status(sip_call_t *call, int expected_code, int timeout_sec, TransferStatusEvent &matched)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_transfer_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_transfer_status_events) {
            if ((call == nullptr || ev.call == call) && (expected_code <= 0 || ev.code == expected_code)) {
                matched = ev;
                return true;
            }
        }
        g_transfer_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

static bool wait_for_transfer_request(sip_call_t *call, int timeout_sec, TransferRequestEvent &matched)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_transfer_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_transfer_request_events) {
            if (call == nullptr || ev.call == call) {
                matched = ev;
                return true;
            }
        }
        g_transfer_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
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

    bool wait_for_refer(int timeout_sec, std::string &refer_to)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_refer_received) {
                refer_to = m_refer_to;
                return true;
            }
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    void clear_refer()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refer_received = false;
        m_refer_to.clear();
    }

    bool wait_for_client_invite(int timeout_sec, std::string &sdp_out)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        std::unique_lock<std::mutex> lock(m_mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_client_invite_received) {
                sdp_out = m_last_client_invite_sdp;
                return true;
            }
            m_cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    void clear_client_invite()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_client_invite_received = false;
        m_last_client_invite_sdp.clear();
    }

    void set_auto_answer_sdp(const std::string &sdp)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_auto_answer_sdp = sdp;
    }

    bool send_refer(const std::string &target_uri)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        char buf[2048];
        m_last_cseq++;
        std::string ruri = m_client_contact_uri.empty() ?
            ("sip:1001@127.0.0.1:" + std::to_string(ntohs(m_client_addr.sin_port))) :
            m_client_contact_uri;

        int len = std::snprintf(buf, sizeof(buf),
            "REFER %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-refer-%u;rport\r\n"
            "Max-Forwards: 70\r\n"
            "From: %s\r\n"
            "To: %s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %u REFER\r\n"
            "Refer-To: %s\r\n"
            "Referred-By: <sip:loopback@127.0.0.1:%u>\r\n"
            "Content-Length: 0\r\n\r\n",
            ruri.c_str(),
            m_port, m_last_cseq,
            m_last_from.c_str(),
            m_last_to.c_str(),
            m_last_call_id.c_str(),
            m_last_cseq,
            target_uri.c_str(),
            m_port);

        ssize_t sent = sendto(m_sock, buf, len, 0, (struct sockaddr *)&m_client_addr, sizeof(m_client_addr));
        return sent == len;
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
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_last_from = from;
                            m_last_to = to;
                        }
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

            // 4. REFER
            if (msg.rfind("REFER", 0) == 0) {
                std::string refer_to = extract_header(msg, "Refer-To");
                std::string via = extract_header(msg, "Via");
                std::string from = extract_header(msg, "From");
                std::string to = extract_header(msg, "To");
                std::string call_id = extract_header(msg, "Call-ID");
                std::string cseq = extract_header(msg, "CSeq");

                std::string resp = "SIP/2.0 202 Accepted\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Contact: <sip:loopback@127.0.0.1:" + std::to_string(m_port) + ">\r\n"
                                   "Content-Length: 0\r\n\r\n";
                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_refer_received = true;
                    m_refer_to = refer_to;
                }
                m_cv.notify_all();

                // Send NOTIFY with sipfrag 200 OK
                std::string ruri = m_client_contact_uri.empty() ?
                    ("sip:1001@127.0.0.1:" + std::to_string(ntohs(peer.sin_port))) :
                    m_client_contact_uri;

                std::string notify = "NOTIFY " + ruri + " SIP/2.0\r\n"
                                     "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(m_port) + ";branch=z9hG4bK-not-1;rport\r\n"
                                     "Max-Forwards: 70\r\n"
                                     "From: " + to + "\r\n"
                                     "To: " + from + "\r\n"
                                     "Call-ID: " + call_id + "\r\n"
                                     "CSeq: 101 NOTIFY\r\n"
                                     "Event: refer\r\n"
                                     "Subscription-State: terminated;reason=noresource\r\n"
                                     "Content-Type: message/sipfrag\r\n"
                                     "Content-Length: 16\r\n\r\n"
                                     "SIP/2.0 200 OK\r\n";
                sendto(m_sock, notify.c_str(), notify.length(), 0, (struct sockaddr *)&peer, peer_len);
                continue;
            }

            // 5. Outgoing INVITE from client (or re-INVITE)
            if (msg.rfind("INVITE", 0) == 0) {
                std::string via = extract_header(msg, "Via");
                std::string from = extract_header(msg, "From");
                std::string to = extract_header(msg, "To");
                std::string call_id = extract_header(msg, "Call-ID");
                std::string cseq = extract_header(msg, "CSeq");
                if (to.find(";tag=") == std::string::npos) {
                    to += ";tag=loopback-inv";
                }
                std::string body;
                auto body_pos = msg.find("\r\n\r\n");
                if (body_pos != std::string::npos) {
                    body = msg.substr(body_pos + 4);
                }

                std::string answer_sdp;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_client_invite_received = true;
                    m_last_client_invite_sdp = body;
                    answer_sdp = m_auto_answer_sdp;
                }
                m_cv.notify_all();

                if (answer_sdp.empty()) {
                    answer_sdp = "v=0\r\n"
                                 "o=loopback 1 1 IN IP4 127.0.0.1\r\n"
                                 "s=Loopback Answer\r\n"
                                 "c=IN IP4 127.0.0.1\r\n"
                                 "t=0 0\r\n"
                                 "m=audio 16000 RTP/AVP 0\r\n"
                                 "a=rtpmap:0 PCMU/8000\r\n"
                                 "a=sendrecv\r\n";
                }

                std::string resp = "SIP/2.0 200 OK\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Contact: <sip:loopback@127.0.0.1:" + std::to_string(m_port) + ">\r\n"
                                   "Content-Type: application/sdp\r\n"
                                   "Content-Length: " + std::to_string(answer_sdp.size()) + "\r\n\r\n" +
                                   answer_sdp;
                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);
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
    std::atomic<bool> m_refer_received{false};
    std::string m_refer_to;
    std::string m_last_from;
    std::string m_last_to;
    bool m_client_invite_received{false};
    std::string m_last_client_invite_sdp;
    std::string m_auto_answer_sdp;
};

static int run_test_9(const sip_callbacks_t &callbacks, const char *config_dir, const char *asterisk_host, int asterisk_port)
{
    std::printf("--- Test 9: Opus wideband audio & real-time call quality telemetry ---\n");
    clear_call_events();
    g_call_stats_count.store(0);
    {
        sip_client_t *telemetry_client = sip_client_create(&callbacks, nullptr);
        TEST_ASSERT(telemetry_client != nullptr, "telemetry_client creation failed");
        int rc_init = sip_client_init(telemetry_client, config_dir);
        TEST_ASSERT(rc_init == 0, "sip_client_init failed for telemetry client");

        int rc = sip_client_register(telemetry_client, "1001", asterisk_host, "test1001", "udp", asterisk_port);
        TEST_ASSERT(rc == 0, "sip_client_register failed for telemetry client");

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string echo_uri = "sip:9001@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
        sip_call_t *telem_call = sip_call_make(telemetry_client, echo_uri.c_str(), false);
        TEST_ASSERT(telem_call != nullptr, "sip_call_make failed for telemetry call");

        CallEvent estab_ev9;
        bool estab9 = wait_for_call_state(telem_call, SIP_CALL_STATE_ESTABLISHED, 10, estab_ev9);
        TEST_ASSERT(estab9, "Telemetry call did not transition to ESTABLISHED");
        std::printf("PASS: Telemetry call established to %s\n", echo_uri.c_str());

        // Let media flow for 4.5 seconds to allow periodic 2s telemetry timer to fire at least twice
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));

        // 1. Verify synchronous pull telemetry
        sip_call_stats_t pull_stats{};
        rc = sip_call_get_stats(telem_call, &pull_stats);
        TEST_ASSERT(rc == 0, "sip_call_get_stats failed");
        std::printf("PASS: sip_call_get_stats: codec='%s', bitrate=%u kbps, loss=%.2f, jitter=%u ms, rtt=%u ms\n",
                    pull_stats.audio_codec, pull_stats.audio_bitrate_kbps, pull_stats.audio_loss_rate,
                    pull_stats.audio_jitter_ms, pull_stats.audio_rtt_ms);
        TEST_ASSERT(std::string(pull_stats.audio_codec) == "opus", "Synchronous pull audio codec is not opus");
        TEST_ASSERT(pull_stats.audio_bitrate_kbps > 0, "Synchronous pull audio bitrate is 0 kbps");

        // 2. Verify asynchronous periodic push callback
        int stats_count = g_call_stats_count.load();
        std::printf("PASS: on_call_stats callback invoked %d times during 4.5s call\n", stats_count);
        TEST_ASSERT(stats_count >= 2, "Expected on_call_stats callback to fire at least twice in 4.5s");

        {
            std::lock_guard<std::mutex> lock(g_stats_mutex);
            std::printf("PASS: push stats: codec='%s', bitrate=%u kbps, loss=%.2f, jitter=%u ms, rtt=%u ms\n",
                        g_last_call_stats.audio_codec, g_last_call_stats.audio_bitrate_kbps,
                        g_last_call_stats.audio_loss_rate, g_last_call_stats.audio_jitter_ms,
                        g_last_call_stats.audio_rtt_ms);
            TEST_ASSERT(std::string(g_last_call_stats.audio_codec) == "opus", "Push stats audio codec is not opus");
            TEST_ASSERT(g_last_call_stats.audio_bitrate_kbps > 0, "Push stats audio bitrate is 0 kbps");
        }

        rc = sip_call_hangup(telem_call, 0, "Hangup");
        TEST_ASSERT(rc == 0, "sip_call_hangup failed for telemetry call");

        CallEvent close_ev9;
        bool closed9 = wait_for_call_state(telem_call, SIP_CALL_STATE_CLOSED, 5, close_ev9);
        TEST_ASSERT(closed9, "Telemetry call did not transition to CLOSED");
        sip_call_destroy(telem_call);
        sip_client_destroy(telemetry_client);
        std::printf("PASS: Telemetry call cleanly terminated\n");
    }

    // --- Test 9b: Wideband codec priority (opus > G722 > PCMU/PCMA) & G.722 negotiation ---
    std::printf("--- Test 9b: Wideband codec priority (opus > G722 > PCMU/PCMA) & G.722 negotiation ---\n");
    clear_call_events();
    {
        LoopbackCaller g722_peer(15097);
        g722_peer.set_auto_answer_sdp(
            "v=0\r\n"
            "o=loopback 1 1 IN IP4 127.0.0.1\r\n"
            "s=G722 Answer\r\n"
            "c=IN IP4 127.0.0.1\r\n"
            "t=0 0\r\n"
            "m=audio 16000 RTP/AVP 9\r\n"
            "a=rtpmap:9 G722/8000\r\n"
            "a=sendrecv\r\n");
        g722_peer.start();

        sip_client_t *g722_client = sip_client_create(&callbacks, nullptr);
        TEST_ASSERT(g722_client != nullptr, "g722_client creation failed");
        TEST_ASSERT(sip_client_init(g722_client, config_dir) == 0, "sip_client_init failed for g722_client");
        TEST_ASSERT(sip_client_register(g722_client, "1001", "127.0.0.1", "test1001", "udp", 15097) == 0,
                    "sip_client_register failed for g722_client");
        TEST_ASSERT(g722_peer.wait_for_registration(5), "g722_peer did not receive registration");

        sip_call_t *g722_call = sip_call_make(g722_client, "sip:bob@127.0.0.1:15097", false);
        TEST_ASSERT(g722_call != nullptr, "sip_call_make failed for g722_call");

        std::string offer_sdp;
        TEST_ASSERT(g722_peer.wait_for_client_invite(5, offer_sdp), "g722_peer did not receive INVITE");
        auto pos_opus = offer_sdp.find("opus/48000/2");
        auto pos_g722 = offer_sdp.find("G722/");
        auto pos_pcmu = offer_sdp.find("PCMU/8000");
        auto pos_pcma = offer_sdp.find("PCMA/8000");
        TEST_ASSERT(pos_opus != std::string::npos, "SDP offer missing opus/48000/2");
        TEST_ASSERT(pos_g722 != std::string::npos, "SDP offer missing G722");
        TEST_ASSERT(pos_pcmu != std::string::npos, "SDP offer missing PCMU/8000");
        TEST_ASSERT(pos_pcma != std::string::npos, "SDP offer missing PCMA/8000");
        TEST_ASSERT(pos_opus < pos_g722 && pos_g722 < pos_pcmu, "SDP codec priority order is not opus > G722 > PCMU");
        std::printf("PASS: Outgoing SDP offer prioritizes opus/48000/2 > G722 > PCMU/PCMA\n");

        CallEvent estab_g722;
        TEST_ASSERT(wait_for_call_state(g722_call, SIP_CALL_STATE_ESTABLISHED, 5, estab_g722),
                    "G722 call did not establish");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        sip_call_stats_t g722_stats{};
        TEST_ASSERT(sip_call_get_stats(g722_call, &g722_stats) == 0, "sip_call_get_stats failed for G722 call");
        TEST_ASSERT(std::string(g722_stats.audio_codec) == "G722", "Negotiated codec is not G722");
        std::printf("PASS: G722 call negotiated codec='%s'\n", g722_stats.audio_codec);

        sip_call_hangup(g722_call, 0, "Hangup");
        CallEvent close_g722;
        wait_for_call_state(g722_call, SIP_CALL_STATE_CLOSED, 5, close_g722);
        sip_call_destroy(g722_call);
        g722_peer.stop();
        sip_client_destroy(g722_client);
    }
    return 0;
}

static int run_test_10(const sip_callbacks_t &callbacks, const char *config_dir, const char *asterisk_host, int asterisk_port)
{
    (void)asterisk_host;
    (void)asterisk_port;
    std::printf("--- Test 10: Blind & attended call transfer (RFC 3515 REFER) ---\n");
    clear_call_events();
    clear_incoming_events();
    {
        std::lock_guard<std::mutex> lock(g_transfer_mutex);
        g_transfer_status_events.clear();
        g_transfer_request_events.clear();
    }

    LoopbackCaller caller(15096);
    caller.start();

    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    TEST_ASSERT(client != nullptr, "client creation failed");
    int rc_init = sip_client_init(client, config_dir);
    TEST_ASSERT(rc_init == 0, "sip_client_init failed");

    int rc = sip_client_register(client, "1001", "127.0.0.1", "test1001", "udp", 15096);
    TEST_ASSERT(rc == 0, "sip_client_register failed for transfer test");
    TEST_ASSERT(caller.wait_for_registration(5), "Client registration not received by loopback");

    // Establish incoming call from caller to client
    caller.send_invite("call-id-test10-a", 10, "Bob Transfer", false);
    IncomingCallEvent in_ev;
    TEST_ASSERT(wait_for_incoming_call(5, in_ev), "Incoming call not received");
    rc = sip_call_answer(in_ev.call, false);
    TEST_ASSERT(rc == 0, "sip_call_answer failed");
    TEST_ASSERT(caller.wait_for_response(5) == 200, "Expected 200 OK answer");

    CallEvent estab_ev;
    TEST_ASSERT(wait_for_call_state(in_ev.call, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev), "Call did not establish");
    std::printf("PASS: Established call for transfer test\n");

    // Part A: Blind Transfer Initiation (sip_call_transfer)
    std::string target = "sip:9002@127.0.0.1:5060";
    rc = sip_call_transfer(in_ev.call, target.c_str());
    TEST_ASSERT(rc == 0, "sip_call_transfer returned non-zero");

    std::string received_refer_to;
    TEST_ASSERT(caller.wait_for_refer(5, received_refer_to), "Loopback caller did not receive REFER");
    TEST_ASSERT(received_refer_to.find("9002") != std::string::npos, "Refer-To does not contain target");
    std::printf("PASS: Remote peer received REFER with Refer-To: %s\n", received_refer_to.c_str());

    TransferStatusEvent status_ev;
    TEST_ASSERT(wait_for_transfer_status(in_ev.call, 200, 5, status_ev), "Did not receive transfer status 200");
    std::printf("PASS: Transfer status callback received code 200 ('%s')\n", status_ev.reason.c_str());

    CallEvent close_ev;
    TEST_ASSERT(wait_for_call_state(in_ev.call, SIP_CALL_STATE_CLOSED, 5, close_ev), "Call did not close after transfer");
    sip_call_destroy(in_ev.call);
    std::printf("PASS: Original call closed after successful transfer\n");

    // Part B: Incoming Transfer Request (on_transfer_request & sip_call_accept_transfer)
    clear_call_events();
    clear_incoming_events();
    caller.send_invite("call-id-test10-b", 20, "Bob Transfer 2", false);
    IncomingCallEvent in_ev2;
    TEST_ASSERT(wait_for_incoming_call(5, in_ev2), "Second incoming call not received");
    rc = sip_call_answer(in_ev2.call, false);
    TEST_ASSERT(rc == 0, "sip_call_answer failed");
    TEST_ASSERT(caller.wait_for_response(5) == 200, "Expected 200 OK");
    TEST_ASSERT(wait_for_call_state(in_ev2.call, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev), "Second call did not establish");

    // Loopback sends REFER to client
    TEST_ASSERT(caller.send_refer("<sip:9001@127.0.0.1:5060>"), "send_refer failed");
    TransferRequestEvent req_ev;
    TEST_ASSERT(wait_for_transfer_request(in_ev2.call, 5, req_ev), "Did not receive on_transfer_request");
    TEST_ASSERT(req_ev.refer_to_uri.find("9001") != std::string::npos, "refer_to_uri mismatch");
    TEST_ASSERT(req_ev.has_replaces == false, "Expected has_replaces to be false for blind transfer");
    std::printf("PASS: on_transfer_request invoked with target '%s', has_replaces=%d\n",
                req_ev.refer_to_uri.c_str(), req_ev.has_replaces);

    // Decline incoming transfer
    rc = sip_call_accept_transfer(in_ev2.call, false);
    TEST_ASSERT(rc == 0, "sip_call_accept_transfer failed");
    std::printf("PASS: sip_call_accept_transfer(false) cleanly declined incoming transfer\n");

    sip_call_hangup(in_ev2.call, 0, "Hangup");
    wait_for_call_state(in_ev2.call, SIP_CALL_STATE_CLOSED, 5, close_ev);
    sip_call_destroy(in_ev2.call);

    // Part C: Attended Transfer (sip_call_attended_transfer with Replaces)
    clear_call_events();
    clear_incoming_events();
    caller.clear_refer();
    {
        std::lock_guard<std::mutex> lock(g_transfer_mutex);
        g_transfer_status_events.clear();
        g_transfer_request_events.clear();
    }

    // 1. Primary call A (incoming from caller, then placed on hold)
    caller.send_invite("call-id-test10-primary", 30, "Alice Primary", false);
    IncomingCallEvent in_ev_a;
    TEST_ASSERT(wait_for_incoming_call(5, in_ev_a), "Primary call A not received");
    TEST_ASSERT(sip_call_answer(in_ev_a.call, false) == 0, "sip_call_answer failed for call A");
    TEST_ASSERT(caller.wait_for_response(5) == 200, "Expected 200 OK for call A");
    TEST_ASSERT(wait_for_call_state(in_ev_a.call, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev), "Call A did not establish");
    TEST_ASSERT(sip_call_set_hold(in_ev_a.call, true) == 0, "sip_call_set_hold failed for call A");
    CallEvent hold_ev_a;
    TEST_ASSERT(wait_for_call_state(in_ev_a.call, SIP_CALL_STATE_HOLD, 5, hold_ev_a), "Call A did not enter HOLD");

    // 2. Consultation call B (outgoing from client to caller)
    caller.clear_client_invite();
    sip_call_t *consult_call = sip_call_make(client, "sip:consult@127.0.0.1:15096", false);
    TEST_ASSERT(consult_call != nullptr, "sip_call_make failed for consult_call");
    CallEvent estab_ev_b;
    TEST_ASSERT(wait_for_call_state(consult_call, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev_b), "Consult call B did not establish");
    std::printf("PASS: Call A (on hold) and Call B (consultation active) established\n");

    // 3. Execute attended transfer: transfer Call A to Call B
    rc = sip_call_attended_transfer(in_ev_a.call, consult_call);
    TEST_ASSERT(rc == 0, "sip_call_attended_transfer returned non-zero");

    std::string attended_refer_to;
    TEST_ASSERT(caller.wait_for_refer(5, attended_refer_to), "Did not receive REFER for attended transfer");
    TEST_ASSERT(attended_refer_to.find("Replaces=") != std::string::npos, "Attended REFER missing Replaces= parameter");
    TEST_ASSERT(attended_refer_to.find("%3Bto-tag%3D") != std::string::npos, "Attended REFER missing URL-escaped to-tag");
    TEST_ASSERT(attended_refer_to.find("%3Bfrom-tag%3D") != std::string::npos, "Attended REFER missing URL-escaped from-tag");
    std::printf("PASS: Attended transfer sent REFER with Replaces: %s\n", attended_refer_to.c_str());

    TransferStatusEvent attended_status;
    TEST_ASSERT(wait_for_transfer_status(in_ev_a.call, 200, 5, attended_status),
                "Did not receive transfer status 200 for attended transfer");
    CallEvent close_ev_a, close_ev_b;
    TEST_ASSERT(wait_for_call_state(in_ev_a.call, SIP_CALL_STATE_CLOSED, 5, close_ev_a),
                "Primary call A did not close after attended transfer");
    TEST_ASSERT(wait_for_call_state(consult_call, SIP_CALL_STATE_CLOSED, 5, close_ev_b),
                "Consultation call B did not close after attended transfer");
    std::printf("PASS: Both primary and consultation calls transitioned to SIP_CALL_STATE_CLOSED\n");

    sip_call_destroy(in_ev_a.call);
    sip_call_destroy(consult_call);

    caller.stop();
    sip_client_destroy(client);
    std::printf("PASS: All call transfer tests passed\n");
    return 0;
}

static int run_test_8b(const sip_callbacks_t &callbacks, const char *config_dir, const char *asterisk_host, int asterisk_port);

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
    callbacks.on_call_stats = test_on_call_stats;
    callbacks.on_transfer_status = test_on_transfer_status;
    callbacks.on_transfer_request = test_on_transfer_request;

    if (argc > 4 && std::string(argv[4]) == "--only-dtls") {
        return run_test_8b(callbacks, config_dir, asterisk_host, asterisk_port);
    }
    if (argc > 4 && std::string(argv[4]) == "--only-telem") {
        return run_test_9(callbacks, config_dir, asterisk_host, asterisk_port);
    }
    if (argc > 4 && std::string(argv[4]) == "--only-transfer") {
        return run_test_10(callbacks, config_dir, asterisk_host, asterisk_port);
    }

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

    // Teardown first client before starting TLS client
    sip_client_destroy(client);
    client = nullptr;

    // --- Test 8: SIP over TLS + SRTP audio call to Echo 9001 ---
    std::printf("--- Test 8: SIP over TLS + SRTP audio call to Echo 9001 ---\n");
    clear_call_events();
    {
        sip_client_t *tls_client = sip_client_create(&callbacks, nullptr);
        TEST_ASSERT(tls_client != nullptr, "tls_client creation failed");
        int rc_init = sip_client_init(tls_client, config_dir);
        TEST_ASSERT(rc_init == 0, "sip_client_init failed for TLS client");

        sip_account_config_t cfg{};
        cfg.user = "1004";
        cfg.domain = asterisk_host;
        cfg.pwd = "test1004";
        cfg.transport = "tls";
        cfg.port = 5061;
        cfg.verify_tls_cert = true;
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
        cfg.tls_ca_file = ca_path;
        cfg.media_enc = "srtp-mand";

        rc = sip_client_register_account(tls_client, &cfg);
        TEST_ASSERT(rc == 0, "sip_client_register_account failed for TLS client");

        // Wait a moment for registration
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string echo_uri = "sip:9001@" + std::string(asterisk_host) + ":5061;transport=tls";
        sip_call_t *tls_call = sip_call_make(tls_client, echo_uri.c_str(), false);
        TEST_ASSERT(tls_call != nullptr, "sip_call_make failed for TLS/SRTP echo call");

        CallEvent estab_ev8;
        bool estab8 = wait_for_call_state(tls_call, SIP_CALL_STATE_ESTABLISHED, 10, estab_ev8);
        TEST_ASSERT(estab8, "TLS/SRTP call did not transition to ESTABLISHED");
        std::printf("PASS: TLS/SRTP call established to %s\n", echo_uri.c_str());

        // Verify media duration advances
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        uint32_t dur8 = sip_call_get_duration(tls_call);
        TEST_ASSERT(dur8 >= 1, "Call duration did not advance for TLS/SRTP call");
        std::printf("PASS: TLS/SRTP call duration: %u sec\n", dur8);

        // Verify call is secure (SRTP active)
        TEST_ASSERT(sip_call_is_secure(tls_call) == true, "sip_call_is_secure returned false for SRTP call");
        std::printf("PASS: TLS/SRTP call media is encrypted (sip_call_is_secure == true)\n");

        rc = sip_call_hangup(tls_call, 0, "Hangup");
        TEST_ASSERT(rc == 0, "sip_call_hangup failed for TLS/SRTP call");

        CallEvent close_ev8;
        bool closed8 = wait_for_call_state(tls_call, SIP_CALL_STATE_CLOSED, 5, close_ev8);
        TEST_ASSERT(closed8, "TLS/SRTP call did not transition to CLOSED");
        sip_call_destroy(tls_call);
        sip_client_destroy(tls_client);
        std::printf("PASS: TLS/SRTP audio call cleanly terminated\n");
    }

    // --- Test 8b: DTLS-SRTP (media_enc = "dtls_srtp") SDP & Echo 9001 call ---
    int rc8b = run_test_8b(callbacks, config_dir, asterisk_host, asterisk_port);
    if (rc8b != 0) return rc8b;

    // --- Test 9: Opus wideband audio & real-time call quality telemetry ---
    run_test_9(callbacks, config_dir, asterisk_host, asterisk_port);

    // --- Test 10: Blind & attended call transfer (RFC 3515 REFER) ---
    run_test_10(callbacks, config_dir, asterisk_host, asterisk_port);

    std::printf("ALL_AUDIO_CALL_TESTS_PASSED\n");
    return 0;
}

static int run_test_8b(const sip_callbacks_t &callbacks, const char *config_dir, const char *asterisk_host, int asterisk_port)
{
    int rc = 0;
    std::printf("--- Test 8b: DTLS-SRTP (media_enc = \"dtls_srtp\") SDP & Echo 9001 call ---\n");
    clear_call_events();
    {
        // 1. Verify outgoing SDP offer with media_enc = "dtls_srtp" has UDP/TLS/RTP/SAVPF and a=fingerprint
        LoopbackCaller dtls_sdp_peer(15098);
        dtls_sdp_peer.start();

        sip_client_t *dtls_sdp_client = sip_client_create(&callbacks, nullptr);
        TEST_ASSERT(dtls_sdp_client != nullptr, "dtls_sdp_client creation failed");
        TEST_ASSERT(sip_client_init(dtls_sdp_client, config_dir) == 0, "sip_client_init failed for dtls_sdp_client");

        sip_account_config_t sdp_cfg{};
        sdp_cfg.user = "1001";
        sdp_cfg.domain = "127.0.0.1";
        sdp_cfg.pwd = "test1001";
        sdp_cfg.transport = "udp";
        sdp_cfg.port = 15098;
        sdp_cfg.media_enc = "dtls_srtp";

        TEST_ASSERT(sip_client_register_account(dtls_sdp_client, &sdp_cfg) == 0, "register failed for dtls_sdp_client");
        TEST_ASSERT(dtls_sdp_peer.wait_for_registration(5), "dtls_sdp_peer did not receive registration");

        sip_call_t *dtls_sdp_call = sip_call_make(dtls_sdp_client, "sip:bob@127.0.0.1:15098", false);
        TEST_ASSERT(dtls_sdp_call != nullptr, "sip_call_make failed for dtls_sdp_call");

        std::string dtls_offer_sdp;
        TEST_ASSERT(dtls_sdp_peer.wait_for_client_invite(5, dtls_offer_sdp), "dtls_sdp_peer did not receive INVITE");
        TEST_ASSERT(dtls_offer_sdp.find("UDP/TLS/RTP/SAVP") != std::string::npos,
                    "DTLS-SRTP SDP offer missing UDP/TLS/RTP/SAVP(F)");
        TEST_ASSERT(dtls_offer_sdp.find("a=fingerprint:") != std::string::npos,
                    "DTLS-SRTP SDP offer missing a=fingerprint");
        std::printf("PASS: DTLS-SRTP SDP offer contains UDP/TLS/RTP/SAVP(F) and a=fingerprint\n");

        sip_call_hangup(dtls_sdp_call, 0, "Hangup");
        CallEvent close_dtls_sdp;
        wait_for_call_state(dtls_sdp_call, SIP_CALL_STATE_CLOSED, 5, close_dtls_sdp);
        sip_call_destroy(dtls_sdp_call);
        dtls_sdp_peer.stop();
        sip_client_destroy(dtls_sdp_client);

        // 2. Live DTLS-SRTP audio call against Asterisk DTLS endpoint (1005 -> 9001)
        clear_call_events();
        sip_client_t *dtls_client = sip_client_create(&callbacks, nullptr);
        TEST_ASSERT(dtls_client != nullptr, "dtls_client creation failed");
        TEST_ASSERT(sip_client_init(dtls_client, config_dir) == 0, "sip_client_init failed for dtls_client");

        sip_account_config_t dtls_cfg{};
        dtls_cfg.user = "1005";
        dtls_cfg.domain = asterisk_host;
        dtls_cfg.pwd = "test1005";
        dtls_cfg.transport = "udp";
        dtls_cfg.port = asterisk_port;
        dtls_cfg.media_enc = "dtls_srtp";

        TEST_ASSERT(sip_client_register_account(dtls_client, &dtls_cfg) == 0,
                    "sip_client_register_account failed for DTLS client");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::string dtls_echo_uri = "sip:9001@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
        sip_call_t *dtls_call = sip_call_make(dtls_client, dtls_echo_uri.c_str(), false);
        TEST_ASSERT(dtls_call != nullptr, "sip_call_make failed for DTLS-SRTP echo call");

        CallEvent estab_dtls;
        TEST_ASSERT(wait_for_call_state(dtls_call, SIP_CALL_STATE_ESTABLISHED, 10, estab_dtls),
                    "DTLS-SRTP call did not transition to ESTABLISHED");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        TEST_ASSERT(sip_call_is_secure(dtls_call) == true, "sip_call_is_secure returned false for DTLS-SRTP call");

        sip_call_stats_t dtls_stats{};
        TEST_ASSERT(sip_call_get_stats(dtls_call, &dtls_stats) == 0, "sip_call_get_stats failed for DTLS-SRTP call");
        TEST_ASSERT(dtls_stats.audio_bitrate_kbps > 0, "DTLS-SRTP call audio_bitrate_kbps is 0");
        std::printf("PASS: DTLS-SRTP call established & exchanged audio (secure=true, bitrate=%u kbps)\n",
                    dtls_stats.audio_bitrate_kbps);

        sip_call_hangup(dtls_call, 0, "Hangup");
        CallEvent close_dtls;
        wait_for_call_state(dtls_call, SIP_CALL_STATE_CLOSED, 5, close_dtls);
        sip_call_destroy(dtls_call);
        sip_client_destroy(dtls_client);
    }
    return 0;
}
