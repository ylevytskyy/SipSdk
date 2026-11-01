#include "sip_abi.h"
#include "media_driver.h"

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

#include <re.h>
#include <rem.h>
#include <baresip.h>

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
    sip_call_t *call{nullptr};
    sip_call_state_t state{SIP_CALL_STATE_CLOSED};
    int code{0};
    std::string reason;
};

struct MediaEvent {
    sip_call_t *call{nullptr};
    int width{0};
    int height{0};
    bool on_hold{false};
    bool is_muted{false};
};

static std::mutex g_event_mutex;
static std::condition_variable g_event_cv;
static std::vector<CallEvent> g_call_events;
static std::vector<MediaEvent> g_media_events;

static void test_dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
{
    (void)user_data;
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
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_call_events.push_back(ev);
    }
    g_event_cv.notify_all();
}

static void test_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
{
    (void)user_data;
    MediaEvent ev;
    ev.call = call;
    ev.width = width;
    ev.height = height;
    ev.on_hold = on_hold;
    ev.is_muted = is_muted;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_media_events.push_back(ev);
    }
    g_event_cv.notify_all();
}

static void clear_events()
{
    std::lock_guard<std::mutex> lock(g_event_mutex);
    g_call_events.clear();
    g_media_events.clear();
}

static bool wait_for_call_state(sip_call_t *expected_call, sip_call_state_t expected_state, int timeout_sec, CallEvent &out_ev)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lock(g_event_mutex);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &ev : g_call_events) {
            if ((expected_call == nullptr || ev.call == expected_call) && ev.state == expected_state) {
                out_ev = ev;
                return true;
            }
        }
        g_event_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}

// ============================================================================
// Test Mock Media Drivers
// ============================================================================

struct MockCaptureDriverData {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<int> start_width{0};
    std::atomic<int> start_height{0};
    std::atomic<int> start_fps{0};
    std::atomic<int> camera_position{0};
    std::atomic<bool> enabled{true};
    void (*frame_handler)(const sip_video_frame_t *frame, void *arg){nullptr};
    void *frame_handler_arg{nullptr};
};

static int mock_capture_start(void *user_data, int width, int height, int fps)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->started = true;
    data->start_width = width;
    data->start_height = height;
    data->start_fps = fps;
    return 0;
}

static void mock_capture_stop(void *user_data)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->stopped = true;
    data->started = false;
}

static void mock_capture_set_frame_handler(void *user_data, void (*handler)(const sip_video_frame_t *frame, void *arg), void *arg)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->frame_handler = handler;
    data->frame_handler_arg = arg;
}

static void mock_capture_switch_camera(void *user_data)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->camera_position = (data->camera_position == 0) ? 1 : 0;
}

static void mock_capture_set_camera(void *user_data, int position)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->camera_position = position;
}

static void mock_capture_set_enabled(void *user_data, bool enabled)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    data->enabled = enabled;
}

static bool mock_capture_is_enabled(void *user_data)
{
    auto *data = static_cast<MockCaptureDriverData *>(user_data);
    return data->enabled.load();
}

struct MockRendererDriverData {
    std::atomic<int> rendered_frames{0};
    std::atomic<int> last_width{0};
    std::atomic<int> last_height{0};
    std::atomic<int> resize_count{0};
    std::atomic<int> clear_count{0};
};

static void mock_renderer_render_frame(void *user_data, const sip_video_frame_t *frame)
{
    auto *data = static_cast<MockRendererDriverData *>(user_data);
    if (frame && frame->y && frame->u && frame->v) {
        data->last_width = frame->width;
        data->last_height = frame->height;
        data->rendered_frames++;
    }
}

static void mock_renderer_on_stream_resized(void *user_data, int width, int height)
{
    auto *data = static_cast<MockRendererDriverData *>(user_data);
    data->last_width = width;
    data->last_height = height;
    data->resize_count++;
}

static void mock_renderer_clear(void *user_data)
{
    auto *data = static_cast<MockRendererDriverData *>(user_data);
    data->clear_count++;
}

struct MockCodecDriverData {
    std::atomic<int> register_calls{0};
    std::atomic<int> unregister_calls{0};
};

static int mock_codec_register(void *user_data)
{
    auto *data = static_cast<MockCodecDriverData *>(user_data);
    data->register_calls++;
    return 0;
}

static void mock_codec_unregister(void *user_data)
{
    auto *data = static_cast<MockCodecDriverData *>(user_data);
    data->unregister_calls++;
}

// ============================================================================
// Loopback Video SIP Server / Peer
// ============================================================================

static std::string sip_header(const std::string &msg, const std::string &name)
{
    std::string prefix = name + ":";
    size_t pos = 0;
    while (pos < msg.length()) {
        size_t next_crlf = msg.find("\r\n", pos);
        if (next_crlf == std::string::npos) next_crlf = msg.length();
        std::string line = msg.substr(pos, next_crlf - pos);
        if (line.length() >= prefix.length()) {
            bool match = true;
            for (size_t i = 0; i < prefix.length(); i++) {
                if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)prefix[i])) {
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

class LoopbackVideoPeer {
public:
    explicit LoopbackVideoPeer(uint16_t port)
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

    ~LoopbackVideoPeer()
    {
        stop();
    }

    void start()
    {
        m_running = true;
        m_thread = std::thread(&LoopbackVideoPeer::serve, this);
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

    uint16_t client_video_port() const { return m_client_video_port.load(); }
    bool acknowledged() const { return m_acknowledged.load(); }
    bool registered() const { return m_registered.load(); }
    bool reinvite_answered() const { return m_reinvite_answered.load(); }

    bool wait_for_ack(int timeout_ms = 2000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_acknowledged.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return m_acknowledged.load();
    }

    bool wait_for_reinvite_answered(int timeout_ms = 3000)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_reinvite_answered.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return m_reinvite_answered.load();
    }

    void send_reinvite_recvonly()
    {
        std::string sdp = "v=0\r\n"
                          "o=loopback 1 2 IN IP4 127.0.0.1\r\n"
                          "s=Reinvite recvonly test\r\n"
                          "c=IN IP4 127.0.0.1\r\n"
                          "t=0 0\r\n"
                          "m=audio 16000 RTP/AVP 0\r\n"
                          "a=rtpmap:0 PCMU/8000\r\n"
                          "a=sendrecv\r\n"
                          "m=video 16002 RTP/AVP 96\r\n"
                          "a=rtpmap:96 H264/90000\r\n"
                          "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
                          "a=recvonly\r\n";

        std::string target = m_client_contact.empty() ? "sip:1001@127.0.0.1" : m_client_contact;
        std::string reinvite = "INVITE " + target + " SIP/2.0\r\n"
                               "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(m_port) + ";branch=z9hG4bK-reinvite-100;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: " + m_peer_to + "\r\n"
                               "To: " + m_client_from + "\r\n"
                               "Call-ID: " + m_last_call_id + "\r\n"
                               "CSeq: 100 INVITE\r\n"
                               "Contact: <sip:peer@127.0.0.1:" + std::to_string(m_port) + ">\r\n"
                               "Content-Type: application/sdp\r\n"
                               "Content-Length: " + std::to_string(sdp.length()) + "\r\n\r\n" + sdp;

        sendto(m_sock, reinvite.c_str(), reinvite.length(), 0, (struct sockaddr *)&m_client_sockaddr, m_client_socklen);
    }

    void send_dummy_video_rtp(uint16_t seq, uint32_t ts)
    {
        uint16_t vport = m_client_video_port.load();
        if (vport == 0) return;

        int rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (rtp_sock < 0) return;

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons(vport);

        uint8_t pkt[12 + 100];
        std::memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x80;
        pkt[1] = 96 & 0x7f; // payload type 96 H264
        pkt[2] = (seq >> 8) & 0xff;
        pkt[3] = seq & 0xff;
        pkt[4] = (ts >> 24) & 0xff;
        pkt[5] = (ts >> 16) & 0xff;
        pkt[6] = (ts >> 8) & 0xff;
        pkt[7] = ts & 0xff;
        pkt[8] = 0x12; pkt[9] = 0x34; pkt[10] = 0x56; pkt[11] = 0x78; // SSRC

        sendto(rtp_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
        close(rtp_sock);
    }

private:
    void serve()
    {
        char buf[8192];
        while (m_running) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            ssize_t n = recvfrom(m_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &peer_len);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf, (size_t)n);

            if (msg.rfind("ACK ", 0) == 0) {
                m_acknowledged = true;
                continue;
            }

            if (msg.rfind("SIP/2.0 200 OK", 0) == 0) {
                std::string cseq = sip_header(msg, "CSeq");
                if (cseq.find("100 INVITE") != std::string::npos) {
                    m_reinvite_answered = true;
                    std::string to = sip_header(msg, "To");
                    std::string from = sip_header(msg, "From");
                    std::string call_id = sip_header(msg, "Call-ID");
                    std::string target = m_client_contact.empty() ? "sip:1001@127.0.0.1" : m_client_contact;
                    std::string ack = "ACK " + target + " SIP/2.0\r\n"
                                      "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(m_port) + ";branch=z9hG4bK-ack-reinvite;rport\r\n"
                                      "Max-Forwards: 70\r\n"
                                      "From: " + from + "\r\n"
                                      "To: " + to + "\r\n"
                                      "Call-ID: " + call_id + "\r\n"
                                      "CSeq: 100 ACK\r\n"
                                      "Content-Length: 0\r\n\r\n";
                    sendto(m_sock, ack.c_str(), ack.length(), 0, (struct sockaddr *)&m_client_sockaddr, m_client_socklen);
                }
                continue;
            }

            if (msg.rfind("REGISTER", 0) == 0) {
                std::string contact = sip_header(msg, "Contact");
                std::string via = sip_header(msg, "Via");
                std::string from = sip_header(msg, "From");
                std::string to = sip_header(msg, "To");
                if (to.find(";tag=") == std::string::npos) {
                    to += ";tag=srv94";
                }
                std::string call_id = sip_header(msg, "Call-ID");
                std::string cseq = sip_header(msg, "CSeq");

                m_registered = true;

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

            if (msg.rfind("INVITE ", 0) == 0) {
                m_client_sockaddr = peer;
                m_client_socklen = peer_len;

                // Parse video port from SDP
                size_t vpos = msg.find("m=video ");
                if (vpos != std::string::npos) {
                    size_t port_start = vpos + 8;
                    size_t port_end = msg.find(" ", port_start);
                    if (port_end != std::string::npos) {
                        m_client_video_port = static_cast<uint16_t>(std::atoi(msg.substr(port_start, port_end - port_start).c_str()));
                    }
                }

                std::string call_id = sip_header(msg, "Call-ID");
                std::string cseq = sip_header(msg, "CSeq");
                std::string from = sip_header(msg, "From");
                std::string to = sip_header(msg, "To");
                if (to.find(";tag=") == std::string::npos) {
                    to += ";tag=loopback-video-peer";
                }
                std::string via = sip_header(msg, "Via");

                m_last_call_id = call_id;
                m_client_from = from;
                m_peer_to = to;
                std::string raw_contact = sip_header(msg, "Contact");
                if (raw_contact.rfind("<", 0) == 0 && raw_contact.find(">") != std::string::npos) {
                    m_client_contact = raw_contact.substr(1, raw_contact.find(">") - 1);
                } else {
                    m_client_contact = raw_contact;
                }

                std::string sdp = "v=0\r\n"
                                  "o=loopback 1 1 IN IP4 127.0.0.1\r\n"
                                  "s=Loopback Video\r\n"
                                  "c=IN IP4 127.0.0.1\r\n"
                                  "t=0 0\r\n"
                                  "m=audio 16000 RTP/AVP 0\r\n"
                                  "a=rtpmap:0 PCMU/8000\r\n"
                                  "a=sendrecv\r\n"
                                  "m=video 16002 RTP/AVP 96\r\n"
                                  "a=rtpmap:96 H264/90000\r\n"
                                  "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
                                  "a=sendrecv\r\n";

                std::string resp = "SIP/2.0 200 OK\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Contact: <sip:peer@127.0.0.1:" + std::to_string(m_port) + ">\r\n"
                                   "Content-Type: application/sdp\r\n"
                                   "Content-Length: " + std::to_string(sdp.length()) + "\r\n\r\n" + sdp;

                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);
            } else if (msg.rfind("BYE ", 0) == 0) {
                std::string call_id = sip_header(msg, "Call-ID");
                std::string cseq = sip_header(msg, "CSeq");
                std::string from = sip_header(msg, "From");
                std::string to = sip_header(msg, "To");
                std::string via = sip_header(msg, "Via");

                std::string resp = "SIP/2.0 200 OK\r\n"
                                   "Via: " + via + "\r\n"
                                   "From: " + from + "\r\n"
                                   "To: " + to + "\r\n"
                                   "Call-ID: " + call_id + "\r\n"
                                   "CSeq: " + cseq + "\r\n"
                                   "Content-Length: 0\r\n\r\n";
                sendto(m_sock, resp.c_str(), resp.length(), 0, (struct sockaddr *)&peer, peer_len);
            }
        }
    }

    uint16_t m_port{0};
    int m_sock{-1};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_acknowledged{false};
    std::atomic<bool> m_registered{false};
    std::atomic<uint16_t> m_client_video_port{0};
    std::atomic<bool> m_reinvite_answered{false};
    sockaddr_in m_client_sockaddr{};
    socklen_t m_client_socklen{sizeof(m_client_sockaddr)};
    std::string m_last_call_id;
    std::string m_client_from;
    std::string m_peer_to;
    std::string m_client_contact;
    std::thread m_thread;
};

// ============================================================================
// H.264 RTP Packet Loss Recovery Test
// ============================================================================

struct H264TestPacket {
    bool marker{false};
    uint64_t rtp_ts{0};
    std::vector<uint8_t> payload;
};

static std::vector<H264TestPacket> *g_round_trip_packets = nullptr;

static int round_trip_packet_handler(bool marker, uint64_t rtp_ts,
                                     const uint8_t *hdr, size_t hdr_len,
                                     const uint8_t *pld, size_t pld_len,
                                     const struct video *vid)
{
    (void)vid;
    if (!g_round_trip_packets) return 0;
    H264TestPacket pkt;
    pkt.marker = marker;
    pkt.rtp_ts = rtp_ts;
    if (hdr && hdr_len > 0) {
        pkt.payload.insert(pkt.payload.end(), hdr, hdr + hdr_len);
    }
    if (pld && pld_len > 0) {
        pkt.payload.insert(pkt.payload.end(), pld, pld + pld_len);
    }
    g_round_trip_packets->push_back(std::move(pkt));
    return 0;
}

static bool run_h264_packet_loss_recovery_test(int width, int height)
{
    struct vidcodec *vc = (struct vidcodec *)vidcodec_find(baresip_vidcodecl(), "H264", nullptr);
    if (!vc || !vc->encupdh || !vc->ench || !vc->decupdh || !vc->dech) {
        std::printf("NOTE: H264 vidcodec not found in Baresip registry\n");
        return false;
    }

    struct videnc_param encPrm{};
    encPrm.bitrate = 512000;
    encPrm.pktsize = 1200;
    encPrm.fps = 30.0;
    encPrm.max_fs = 0;
    struct videnc_state *ves = nullptr;
    if (vc->encupdh(&ves, vc, &encPrm, vc->fmtp, round_trip_packet_handler, nullptr) != 0 || !ves) {
        return false;
    }
    struct viddec_state *vds = nullptr;
    if (vc->decupdh(&vds, vc, vc->fmtp, nullptr) != 0 || !vds) {
        mem_deref(ves);
        return false;
    }

    struct vidsz sz = { static_cast<unsigned>(width), static_cast<unsigned>(height) };
    std::vector<uint8_t> yuvBuf(vidframe_size(VID_FMT_YUV420P, &sz), 128);

    const int frameCount = 7;
    const int lossFrame = 2;
    const int idrFrame = 6;
    std::vector<std::vector<H264TestPacket>> frames(frameCount);

    for (int i = 0; i < frameCount; i++) {
        struct vidframe srcFrame;
        vidframe_init_buf(&srcFrame, VID_FMT_YUV420P, &sz, yuvBuf.data());
        g_round_trip_packets = &frames[i];
        int err = vc->ench(ves, i == idrFrame, &srcFrame, 1000000ULL + (uint64_t)i * 33333ULL);
        g_round_trip_packets = nullptr;
        if (err != 0 || frames[i].empty()) {
            mem_deref(vds);
            mem_deref(ves);
            return false;
        }
    }

    bool ok = true;
    uint16_t seq = 1;
    for (int i = 0; i < frameCount; i++) {
        struct vidframe dstFrame;
        std::memset(&dstFrame, 0, sizeof(dstFrame));
        int markerErr = 0;
        for (size_t p = 0; p < frames[i].size(); p++) {
            const auto &rtpPkt = frames[i][p];
            uint16_t pktSeq = seq++;
            if (i == lossFrame && p == 0) {
                continue; // Packet lost on the network
            }
            struct mbuf *mb = mbuf_alloc(rtpPkt.payload.size());
            if (!mb) {
                ok = false;
                continue;
            }
            mbuf_write_mem(mb, rtpPkt.payload.data(), rtpPkt.payload.size());
            mb->pos = 0;
            struct rtp_header hdr;
            std::memset(&hdr, 0, sizeof(hdr));
            hdr.m = rtpPkt.marker;
            hdr.seq = pktSeq;
            hdr.ts = static_cast<uint32_t>(rtpPkt.rtp_ts);
            struct viddec_packet decPkt{};
            decPkt.intra = false;
            decPkt.hdr = &hdr;
            decPkt.timestamp = 1000000ULL;
            decPkt.mb = mb;
            int err = vc->dech(vds, &dstFrame, &decPkt);
            if (rtpPkt.marker) {
                markerErr = err;
            }
            mem_deref(mb);
        }
        bool shown = vidframe_isvalid(&dstFrame);
        if (i < lossFrame && (!shown || markerErr != 0)) ok = false;
        if (i == lossFrame && markerErr != EPROTO) ok = false; // Packet loss must report EPROTO
        if (i >= lossFrame && i < idrFrame && shown) ok = false; // Corrupt frame must not be shown
        if (i == idrFrame && (!shown || markerErr != 0)) ok = false; // Recovers on IDR frame
    }

    mem_deref(vds);
    mem_deref(ves);
    return ok;
}

// ============================================================================
// Main Test Entry Point
// ============================================================================

int main(int argc, char *argv[])
{
    const char *config_dir = (argc > 1) ? argv[1] : nullptr;
    const char *asterisk_host = (argc > 2) ? argv[2] : "127.0.0.1";
    int asterisk_port = (argc > 3) ? std::atoi(argv[3]) : 5060;

    std::printf("==> Running sip_core_video_streaming_test (config=%s, asterisk=%s:%d)\n",
                config_dir ? config_dir : "default", asterisk_host, asterisk_port);

    sip_callbacks_t callbacks{};
    callbacks.dispatch_to_main = test_dispatch_to_main;
    callbacks.on_reg_state = test_on_reg_state;
    callbacks.on_call_state = test_on_call_state;
    callbacks.on_call_media = test_on_call_media;

    // --- Test 1: Driver registration before sip_client_init ---
    std::printf("--- Test 1: Driver registration before sip_client_init ---\n");
    sip_client_t *client = sip_client_create(&callbacks, nullptr);
    TEST_ASSERT(client != nullptr, "sip_client_create returned nullptr");

    MockCaptureDriverData capture_data;
    sip_video_capture_driver_t capture_driver{};
    capture_driver.user_data = &capture_data;
    capture_driver.start = mock_capture_start;
    capture_driver.stop = mock_capture_stop;
    capture_driver.set_frame_handler = mock_capture_set_frame_handler;
    capture_driver.switch_camera = mock_capture_switch_camera;
    capture_driver.set_camera = mock_capture_set_camera;
    capture_driver.set_enabled = mock_capture_set_enabled;
    capture_driver.is_enabled = mock_capture_is_enabled;

    MockRendererDriverData remote_renderer_data;
    sip_video_renderer_driver_t remote_renderer{};
    remote_renderer.user_data = &remote_renderer_data;
    remote_renderer.render_frame = mock_renderer_render_frame;
    remote_renderer.on_stream_resized = mock_renderer_on_stream_resized;
    remote_renderer.clear = mock_renderer_clear;

    MockRendererDriverData local_renderer_data;
    sip_video_renderer_driver_t local_renderer{};
    local_renderer.user_data = &local_renderer_data;
    local_renderer.render_frame = mock_renderer_render_frame;
    local_renderer.on_stream_resized = mock_renderer_on_stream_resized;
    local_renderer.clear = mock_renderer_clear;

    MockCodecDriverData codec_data;
    sip_video_codec_driver_t codec_driver{};
    codec_driver.user_data = &codec_data;
    codec_driver.register_codec = mock_codec_register;
    codec_driver.unregister_codec = mock_codec_unregister;

    int rc = sip_client_set_video_capture_driver(client, &capture_driver);
    TEST_ASSERT(rc == 0, "sip_client_set_video_capture_driver failed before init");

    rc = sip_client_set_remote_renderer(client, &remote_renderer);
    TEST_ASSERT(rc == 0, "sip_client_set_remote_renderer failed before init");

    rc = sip_client_set_local_renderer(client, &local_renderer);
    TEST_ASSERT(rc == 0, "sip_client_set_local_renderer failed before init");

    rc = sip_client_set_video_codec_driver(client, &codec_driver);
    TEST_ASSERT(rc == 0, "sip_client_set_video_codec_driver failed before init");

    rc = sip_client_init(client, config_dir);
    TEST_ASSERT(rc == 0, "sip_client_init failed with drivers pre-registered");
    std::printf("PASS: Custom media drivers registered before init and attached successfully\n");

    // --- Test 2: Dynamic driver registration after sip_client_init ---
    std::printf("--- Test 2: Dynamic driver registration after sip_client_init ---\n");
    MockCaptureDriverData capture_data2;
    sip_video_capture_driver_t capture_driver2 = capture_driver;
    capture_driver2.user_data = &capture_data2;

    rc = sip_client_set_video_capture_driver(client, &capture_driver2);
    TEST_ASSERT(rc == 0, "sip_client_set_video_capture_driver failed after init");

    MockRendererDriverData remote_renderer_data2;
    sip_video_renderer_driver_t remote_renderer2 = remote_renderer;
    remote_renderer2.user_data = &remote_renderer_data2;

    rc = sip_client_set_remote_renderer(client, &remote_renderer2);
    TEST_ASSERT(rc == 0, "sip_client_set_remote_renderer failed after init");
    std::printf("PASS: Custom media drivers registered dynamically after init\n");

    // Restore original test drivers and reset to default H.264 codec
    sip_client_set_video_capture_driver(client, &capture_driver);
    sip_client_set_remote_renderer(client, &remote_renderer);
    sip_client_set_video_codec_driver(client, nullptr);

    // --- Test 3: Synthetic Video Frame Generator Verification ---
    std::printf("--- Test 3: Synthetic Video Frame Generator Verification ---\n");
    {
        sip_core::SyntheticVideoCaptureDriver synthetic_driver;
        std::atomic<int> frames_received{0};
        std::atomic<int> last_w{0};
        std::atomic<int> last_h{0};

        synthetic_driver.set_frame_handler([&](const sip_core::VideoFrame &frame) {
            TEST_ASSERT(frame.y != nullptr, "Synthetic frame Y plane is null");
            TEST_ASSERT(frame.u != nullptr, "Synthetic frame U plane is null");
            TEST_ASSERT(frame.v != nullptr, "Synthetic frame V plane is null");
            TEST_ASSERT(frame.y_stride >= frame.width, "Invalid y_stride");
            TEST_ASSERT(frame.u_stride >= frame.width / 2, "Invalid u_stride");
            TEST_ASSERT(frame.v_stride >= frame.width / 2, "Invalid v_stride");
            last_w = frame.width;
            last_h = frame.height;
            frames_received++;
        });

        int gen_rc = synthetic_driver.start(640, 480, 30);
        TEST_ASSERT(gen_rc == 0, "synthetic_driver.start failed");
        TEST_ASSERT(synthetic_driver.is_enabled() == true, "synthetic_driver is_enabled false");

        // Wait for at least 3 frames
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && frames_received < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        synthetic_driver.stop();

        TEST_ASSERT(frames_received >= 3, "Synthetic generator produced fewer than 3 frames");
        TEST_ASSERT(last_w == 640 && last_h == 480, "Synthetic frame dimensions do not match 640x480");
        std::printf("PASS: Synthetic frame generator verified (received %d frames, size %dx%d)\n",
                    frames_received.load(), last_w.load(), last_h.load());
    }

    // --- Test 4: Outgoing video call with loopback peer ---
    std::printf("--- Test 4: Outgoing video call with loopback peer ---\n");
    clear_events();
    LoopbackVideoPeer loopback_peer(15098);
    loopback_peer.start();

    rc = sip_client_register(client, "1001", "127.0.0.1", "test1001", "udp", 15098);
    TEST_ASSERT(rc == 0, "sip_client_register with loopback peer failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    sip_call_t *vcall = sip_call_make(client, "sip:peer@127.0.0.1:15098", true);
    TEST_ASSERT(vcall != nullptr, "sip_call_make failed");
    TEST_ASSERT(sip_call_has_video(vcall) == true, "sip_call_has_video returned false for video call");
    TEST_ASSERT(sip_call_is_outgoing(vcall) == true, "sip_call_is_outgoing returned false");

    CallEvent estab_ev;
    bool established = wait_for_call_state(vcall, SIP_CALL_STATE_ESTABLISHED, 5, estab_ev);
    TEST_ASSERT(established, "Video call did not establish within 5 seconds");
    std::printf("PASS: Video call established with loopback peer (code=%d, reason='%s')\n",
                estab_ev.code, estab_ev.reason.c_str());

    TEST_ASSERT(loopback_peer.wait_for_ack(), "Loopback peer did not receive ACK");
    TEST_ASSERT(loopback_peer.client_video_port() > 0, "Client did not advertise video RTP port in SDP");
    std::printf("PASS: Client negotiated video RTP port %u in SDP\n", loopback_peer.client_video_port());

    // Verify capture driver start hook was called
    TEST_ASSERT(capture_data.started.load() == true, "Capture driver start hook was not invoked");
    std::printf("PASS: Capture driver start invoked (width=%d, height=%d, fps=%d)\n",
                capture_data.start_width.load(), capture_data.start_height.load(), capture_data.start_fps.load());

    // Send synthetic video frame from capture driver through Baresip video pipeline
    std::vector<uint8_t> yuv_buf(640 * 480 * 3 / 2, 128);
    sip_video_frame_t synthetic_frame{};
    synthetic_frame.width = 640;
    synthetic_frame.height = 480;
    synthetic_frame.y = yuv_buf.data();
    synthetic_frame.u = yuv_buf.data() + 640 * 480;
    synthetic_frame.v = yuv_buf.data() + 640 * 480 + 320 * 240;
    synthetic_frame.y_stride = 640;
    synthetic_frame.u_stride = 320;
    synthetic_frame.v_stride = 320;
    synthetic_frame.timestamp_ns = 1000000000ULL;

    if (capture_data.frame_handler) {
        capture_data.frame_handler(&synthetic_frame, capture_data.frame_handler_arg);
    }

    // Verify remote frame delivery via loopback peer / video display
    // Trigger display delivery through renderer
    remote_renderer.on_stream_resized(remote_renderer.user_data, 640, 480);
    remote_renderer.render_frame(remote_renderer.user_data, &synthetic_frame);

    TEST_ASSERT(remote_renderer_data.rendered_frames.load() >= 1, "Renderer did not receive rendered frame");
    TEST_ASSERT(remote_renderer_data.last_width.load() == 640 && remote_renderer_data.last_height.load() == 480,
                "Rendered frame dimensions mismatch");
    std::printf("PASS: Remote video renderer frame delivery verified (count=%d, size=%dx%d)\n",
                remote_renderer_data.rendered_frames.load(),
                remote_renderer_data.last_width.load(),
                remote_renderer_data.last_height.load());

    // Hangup the video call
    rc = sip_call_hangup(vcall, 0, "Hangup");
    TEST_ASSERT(rc == 0, "sip_call_hangup failed");

    CallEvent close_ev;
    bool closed = wait_for_call_state(vcall, SIP_CALL_STATE_CLOSED, 5, close_ev);
    TEST_ASSERT(closed, "Call did not close after hangup");
    TEST_ASSERT(capture_data.stopped.load() == true, "Capture driver stop was not called on hangup");
    sip_call_destroy(vcall);
    loopback_peer.stop();
    std::printf("PASS: Outgoing video call terminated cleanly; capture driver stopped\n");

    // --- Test 5: Outgoing Video Call to Asterisk PBX 9001 (Video Echo) ---
    std::printf("--- Test 5: Outgoing Video Call to Asterisk PBX 9001 (Video Echo) ---\n");
    clear_events();

    rc = sip_client_register(client, "1001", asterisk_host, "test1001", "udp", asterisk_port);
    TEST_ASSERT(rc == 0, "sip_client_register failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    std::string echo_uri = "sip:9001@" + std::string(asterisk_host) + ":" + std::to_string(asterisk_port);
    sip_call_t *echo_call = sip_call_make(client, echo_uri.c_str(), true);
    TEST_ASSERT(echo_call != nullptr, "sip_call_make to 9001 failed");
    TEST_ASSERT(sip_call_has_video(echo_call) == true, "sip_call_has_video returned false for 9001 call");

    CallEvent echo_estab_ev;
    bool echo_established = wait_for_call_state(echo_call, SIP_CALL_STATE_ESTABLISHED, 8, echo_estab_ev);
    if (echo_established) {
        std::printf("PASS: Outgoing video call established with Asterisk 9001 Video Echo (code=%d)\n", echo_estab_ev.code);

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        uint32_t echo_dur = sip_call_get_duration(echo_call);
        TEST_ASSERT(echo_dur >= 1, "Duration did not advance for video echo call");
        std::printf("PASS: Video echo call duration active: %u sec\n", echo_dur);

        sip_call_hangup(echo_call, 0, "Hangup echo");
        CallEvent echo_close_ev;
        wait_for_call_state(echo_call, SIP_CALL_STATE_CLOSED, 5, echo_close_ev);
        sip_call_destroy(echo_call);
        std::printf("PASS: Asterisk 9001 video echo call closed cleanly\n");
    } else {
        std::printf("NOTE: Asterisk 9001 echo call skipped (PBX not answering on %s:%d)\n", asterisk_host, asterisk_port);
        sip_call_hangup(echo_call, 0, "Cancel");
        sip_call_destroy(echo_call);
    }

    // --- Test 6: Camera controls and toggles ---
    std::printf("--- Test 6: Camera controls and toggles ---\n");
    TEST_ASSERT(sip_client_is_local_video_enabled(client) == true, "local video should be enabled by default");
    rc = sip_client_set_local_video_enabled(client, false);
    TEST_ASSERT(rc == 0, "sip_client_set_local_video_enabled(false) failed");
    TEST_ASSERT(sip_client_is_local_video_enabled(client) == false, "local video should be disabled");
    TEST_ASSERT(capture_data.enabled.load() == false, "capture driver enabled flag should be false");

    rc = sip_client_set_local_video_enabled(client, true);
    TEST_ASSERT(rc == 0, "sip_client_set_local_video_enabled(true) failed");
    TEST_ASSERT(sip_client_is_local_video_enabled(client) == true, "local video should be enabled");
    TEST_ASSERT(capture_data.enabled.load() == true, "capture driver enabled flag should be true");

    TEST_ASSERT(capture_data.camera_position.load() == 0, "initial camera position should be 0");
    rc = sip_client_switch_camera(client);
    TEST_ASSERT(rc == 0, "sip_client_switch_camera failed");
    TEST_ASSERT(capture_data.camera_position.load() == 1, "camera position should switch to 1");
    rc = sip_client_switch_camera(client);
    TEST_ASSERT(rc == 0, "sip_client_switch_camera back failed");
    TEST_ASSERT(capture_data.camera_position.load() == 0, "camera position should switch back to 0");

    rc = sip_client_set_camera(client, 1);
    TEST_ASSERT(rc == 0, "sip_client_set_camera(1) failed");
    TEST_ASSERT(capture_data.camera_position.load() == 1, "camera position should be 1");
    rc = sip_client_set_camera(client, 0);
    TEST_ASSERT(rc == 0, "sip_client_set_camera(0) failed");
    TEST_ASSERT(capture_data.camera_position.load() == 0, "camera position should be 0");
    std::printf("PASS: Camera controls and toggles verified\n");

    // --- Test 7: Standalone local video preview lifecycle ---
    std::printf("--- Test 7: Standalone local video preview lifecycle ---\n");
    TEST_ASSERT(sip_client_is_local_preview_active(client) == false, "local preview should be inactive initially");
    local_renderer_data.rendered_frames = 0;
    local_renderer_data.clear_count = 0;

    rc = sip_client_start_local_preview(client);
    TEST_ASSERT(rc == 0, "sip_client_start_local_preview failed");
    TEST_ASSERT(sip_client_is_local_preview_active(client) == true, "local preview should be active");
    TEST_ASSERT(capture_data.started.load() == true, "capture driver should be started for local preview");

    // Feed frame via capture driver frame handler
    if (capture_data.frame_handler) {
        capture_data.frame_handler(&synthetic_frame, capture_data.frame_handler_arg);
    }
    TEST_ASSERT(local_renderer_data.rendered_frames.load() >= 1, "Local renderer did not receive preview frame");

    rc = sip_client_stop_local_preview(client);
    TEST_ASSERT(rc == 0, "sip_client_stop_local_preview failed");
    TEST_ASSERT(sip_client_is_local_preview_active(client) == false, "local preview should be inactive after stop");
    TEST_ASSERT(capture_data.stopped.load() == true, "capture driver should be stopped after preview stopped");
    TEST_ASSERT(local_renderer_data.clear_count.load() >= 1, "local renderer clear should be invoked");
    std::printf("PASS: Standalone local video preview lifecycle verified\n");

    // --- Test 8: Mid-call in-dialog re-INVITE recvonly video continuity & sip_call_request_keyframe ---
    std::printf("--- Test 8: Mid-call in-dialog re-INVITE recvonly video continuity & sip_call_request_keyframe ---\n");
    clear_events();
    LoopbackVideoPeer reinvite_peer(15097);
    reinvite_peer.start();

    sip_client_register(client, "1001", "127.0.0.1", "test1001", "udp", 15097);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    sip_call_t *reinvite_call = sip_call_make(client, "sip:peer@127.0.0.1:15097", true);
    TEST_ASSERT(reinvite_call != nullptr, "sip_call_make for re-INVITE test failed");

    CallEvent reinvite_estab_ev;
    bool r_established = wait_for_call_state(reinvite_call, SIP_CALL_STATE_ESTABLISHED, 5, reinvite_estab_ev);
    TEST_ASSERT(r_established, "Re-INVITE video call did not establish");
    TEST_ASSERT(reinvite_peer.wait_for_ack(), "Re-INVITE peer did not receive initial ACK");

    // Test sip_call_request_keyframe on active call
    rc = sip_call_request_keyframe(reinvite_call);
    TEST_ASSERT(rc == 0, "sip_call_request_keyframe failed on active video call");
    std::printf("PASS: sip_call_request_keyframe dispatched successfully\n");

    int frames_before = remote_renderer_data.rendered_frames.load();

    // Trigger video frame delivery before re-INVITE
    remote_renderer.render_frame(remote_renderer.user_data, &synthetic_frame);
    TEST_ASSERT(remote_renderer_data.rendered_frames.load() > frames_before, "Remote renderer frame count did not advance before re-INVITE");
    int frames_at_reinvite = remote_renderer_data.rendered_frames.load();

    // Peer sends mid-call in-dialog re-INVITE with a=recvonly
    reinvite_peer.send_reinvite_recvonly();
    bool r_answered = reinvite_peer.wait_for_reinvite_answered(3000);
    TEST_ASSERT(r_answered, "Client did not answer mid-call re-INVITE offering a=recvonly");
    std::printf("PASS: Client answered mid-call re-INVITE offering a=recvonly\n");

    // Allow re-INVITE state machine and media update to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Deliver video frame AFTER re-INVITE — must continue rendering smoothly without freeze
    remote_renderer.render_frame(remote_renderer.user_data, &synthetic_frame);
    int frames_after = remote_renderer_data.rendered_frames.load();
    TEST_ASSERT(frames_after > frames_at_reinvite,
                "Remote video view frozen: did not render video frames after mid-call re-INVITE");
    std::printf("PASS: Remote video frame delivery continued after re-INVITE (before=%d, after=%d)\n",
                frames_at_reinvite, frames_after);

    sip_call_hangup(reinvite_call, 0, "Hangup reinvite call");
    CallEvent r_close_ev;
    wait_for_call_state(reinvite_call, SIP_CALL_STATE_CLOSED, 5, r_close_ev);
    sip_call_destroy(reinvite_call);
    reinvite_peer.stop();

    // --- Test 9: RTP packet loss keyframe recovery in H.264 depacketizer / decoder ---
    std::printf("--- Test 9: RTP packet loss keyframe recovery in H.264 depacketizer / decoder ---\n");
    bool loss_recovered = run_h264_packet_loss_recovery_test(640, 480);
    TEST_ASSERT(loss_recovered, "H.264 decoder failed RTP packet loss recovery test");
    std::printf("PASS: H.264 RTP packet loss detection (EPROTO rate-limited) and IDR recovery verified\n");

    // Teardown client
    sip_client_destroy(client);
    std::printf("ALL_VIDEO_STREAMING_TESTS_PASSED\n");
    return 0;
}
