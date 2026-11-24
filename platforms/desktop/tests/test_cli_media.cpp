// Runs sip_cli against a scripted SIP peer on loopback (no PBX) and checks what the CLI promises:
// it exits when a call ends, prints one MEDIA_STATS line, sends silent G.711 audio with
// "--audio-driver dummy", and exits 1 when no call arrives in listen mode.
//
// usage: test_cli_media <path to sip_cli> [scenario: hangup | duration | nocall | call]
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
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

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kSipPort = 15210;
constexpr uint16_t kRtpPort = 15212;

std::string header(const std::string &msg, const std::string &name)
{
    std::string key = "\r\n" + name + ":";
    auto pos = msg.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = msg.find("\r\n", pos);
    std::string v = msg.substr(pos, end - pos);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
}

int bind_udp(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT(s >= 0, "socket");
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    TEST_ASSERT(bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "bind");
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

// A scripted far end: registrar, caller and callee in one, driven by pump().
class Peer {
public:
    Peer() : m_sip(bind_udp(kSipPort)), m_rtp(bind_udp(kRtpPort)) {}
    ~Peer() { close(m_sip); close(m_rtp); }

    bool registered{false};
    bool call_up{false};         // the dialog is confirmed (we sent or got the ACK)
    bool bye_from_cli{false};
    bool bye_answered{false};    // the CLI answered our BYE
    int rtp_from_cli{0};
    bool send_rtp{false};

    void send_invite()
    {
        m_call_id = "peer-call-1";
        std::string sdp = sdp_body();
        std::string msg =
            "INVITE " + m_cli_contact + " SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(kSipPort) + ";branch=z9hG4bK-inv1;rport\r\n"
            "Max-Forwards: 70\r\n"
            "From: <sip:1001@127.0.0.1>;tag=peer1\r\n"
            "To: <sip:1003@127.0.0.1>\r\n"
            "Call-ID: " + m_call_id + "\r\n"
            "CSeq: 1 INVITE\r\n"
            "Contact: <sip:1001@127.0.0.1:" + std::to_string(kSipPort) + ">\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
        send_sip(msg);
        m_outgoing_invite = true;
    }

    void send_bye()
    {
        std::string msg =
            "BYE " + m_cli_contact + " SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(kSipPort) + ";branch=z9hG4bK-bye1;rport\r\n"
            "Max-Forwards: 70\r\n"
            "From: " + m_dlg_from + "\r\n"
            "To: " + m_dlg_to + "\r\n"
            "Call-ID: " + m_call_id + "\r\n"
            "CSeq: 2 BYE\r\n"
            "Content-Length: 0\r\n\r\n";
        send_sip(msg);
    }

    // Runs the peer for `ms`, or until `done` says so; returns whether `done` held.
    bool pump(int ms, const std::function<bool()> &done = nullptr)
    {
        auto end = Clock::now() + std::chrono::milliseconds(ms);
        auto next_rtp = Clock::now();
        while (Clock::now() < end) {
            read_sip();
            read_rtp();
            if (send_rtp && m_remote_rtp.sin_port != 0 && Clock::now() >= next_rtp) {
                emit_rtp();
                next_rtp += std::chrono::milliseconds(20);
            }
            if (done && done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done ? done() : true;
    }

private:
    std::string sdp_body() const
    {
        return "v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
               "m=audio " + std::to_string(kRtpPort) + " RTP/AVP 0 8\r\n"
               "a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\na=sendrecv\r\n";
    }

    void send_sip(const std::string &msg)
    {
        sendto(m_sip, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr *>(&m_cli_addr), sizeof(m_cli_addr));
    }

    void reply(const std::string &req, const std::string &status, const std::string &extra = "", const std::string &body = "")
    {
        std::string to = header(req, "To");
        if (to.find("tag=") == std::string::npos) to += ";tag=peertag";
        std::string msg =
            "SIP/2.0 " + status + "\r\n"
            "Via: " + header(req, "Via") + "\r\n"
            "From: " + header(req, "From") + "\r\n"
            "To: " + to + "\r\n"
            "Call-ID: " + header(req, "Call-ID") + "\r\n"
            "CSeq: " + header(req, "CSeq") + "\r\n" + extra +
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send_sip(msg);
    }

    void read_sip()
    {
        char buf[8192];
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            ssize_t n = recvfrom(m_sip, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n <= 0) return;
            m_cli_addr = from;
            std::string msg(buf, static_cast<size_t>(n));
            msg = "\r\n" + msg;  // so that the first header is found like the others
            if (msg.compare(2, 8, "REGISTER") == 0) {
                std::string contact = header(msg, "Contact");
                reply(msg, "200 OK", "Contact: " + contact + ";expires=3600\r\nExpires: 3600\r\n");
                auto lt = contact.find('<'), gt = contact.find('>');
                m_cli_contact = (lt != std::string::npos && gt != std::string::npos)
                    ? contact.substr(lt + 1, gt - lt - 1) : contact;
                registered = true;
            } else if (msg.compare(2, 6, "INVITE") == 0) {
                // The CLI is calling us: answer at once with our audio offer.
                m_call_id = header(msg, "Call-ID");
                m_dlg_from = header(msg, "To") + ";tag=peertag";
                m_dlg_to = header(msg, "From");
                learn_rtp_target(msg);
                reply(msg, "200 OK",
                      "Contact: <sip:1003@127.0.0.1:" + std::to_string(kSipPort) + ">\r\n"
                      "Content-Type: application/sdp\r\n", sdp_body());
            } else if (msg.compare(2, 3, "ACK") == 0) {
                call_up = true;
            } else if (msg.compare(2, 3, "BYE") == 0) {
                reply(msg, "200 OK");
                bye_from_cli = true;
                send_rtp = false;
            } else if (msg.compare(2, 7, "SIP/2.0") == 0) {
                int code = std::atoi(msg.c_str() + 10);
                std::string cseq = header(msg, "CSeq");
                if (code == 200 && cseq.find("INVITE") != std::string::npos) {
                    m_dlg_from = header(msg, "From");
                    m_dlg_to = header(msg, "To");
                    learn_rtp_target(msg);
                    std::string ack =
                        "ACK " + m_cli_contact + " SIP/2.0\r\n"
                        "Via: SIP/2.0/UDP 127.0.0.1:" + std::to_string(kSipPort) + ";branch=z9hG4bK-ack1;rport\r\n"
                        "Max-Forwards: 70\r\n"
                        "From: " + m_dlg_from + "\r\nTo: " + m_dlg_to + "\r\n"
                        "Call-ID: " + m_call_id + "\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n";
                    send_sip(ack);
                    call_up = true;
                } else if (code == 200 && cseq.find("BYE") != std::string::npos) {
                    bye_answered = true;
                }
            }
        }
    }

    void learn_rtp_target(const std::string &msg)
    {
        auto body = msg.find("\r\n\r\n");
        if (body == std::string::npos) return;
        std::string sdp = msg.substr(body + 4);
        std::string ip = "127.0.0.1";
        auto c = sdp.find("c=IN IP4 ");
        if (c != std::string::npos) ip = sdp.substr(c + 9, sdp.find("\r\n", c) - c - 9);
        auto m = sdp.find("m=audio ");
        if (m == std::string::npos) return;
        int port = std::atoi(sdp.c_str() + m + 8);
        m_remote_rtp = {};
        m_remote_rtp.sin_family = AF_INET;
        m_remote_rtp.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, ip.c_str(), &m_remote_rtp.sin_addr);
    }

    void read_rtp()
    {
        uint8_t buf[2048];
        for (;;) {
            ssize_t n = recv(m_rtp, buf, sizeof(buf), 0);
            if (n <= 0) return;
            // Count RTP only (payload type 0 or 8), not RTCP.
            if (n >= 12 && (buf[0] >> 6) == 2 && ((buf[1] & 0x7f) == 0 || (buf[1] & 0x7f) == 8)) ++rtp_from_cli;
        }
    }

    void emit_rtp()
    {
        uint8_t pkt[12 + 160];
        std::memset(pkt, 0xff, sizeof(pkt));  // mu-law silence
        pkt[0] = 0x80;
        pkt[1] = 0;
        pkt[2] = static_cast<uint8_t>(m_seq >> 8);
        pkt[3] = static_cast<uint8_t>(m_seq);
        uint32_t ts = m_seq * 160u;
        pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
        pkt[8] = 0x12; pkt[9] = 0x34; pkt[10] = 0x56; pkt[11] = 0x78;
        ++m_seq;
        sendto(m_rtp, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&m_remote_rtp), sizeof(m_remote_rtp));
    }

    int m_sip;
    int m_rtp;
    sockaddr_in m_cli_addr{};
    sockaddr_in m_remote_rtp{};
    std::string m_cli_contact;
    std::string m_call_id;
    std::string m_dlg_from;
    std::string m_dlg_to;
    bool m_outgoing_invite{false};
    uint16_t m_seq{1};
};

// A failed check exits at once; this makes sure no CLI is left running.
std::vector<pid_t> g_children;
void kill_children() { for (pid_t p : g_children) kill(p, SIGKILL); }

// The CLI under test, as a child process with its output in a file.
class Cli {
public:
    Cli(const std::string &path, const std::vector<std::string> &args, const std::string &out_file)
        : m_out(out_file)
    {
        m_pid = fork();
        TEST_ASSERT(m_pid >= 0, "fork");
        if (m_pid != 0) g_children.push_back(m_pid);
        if (m_pid == 0) {
            int fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd, 1);
            dup2(fd, 2);
            std::vector<char *> argv;
            argv.push_back(const_cast<char *>(path.c_str()));
            for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            execv(path.c_str(), argv.data());
            _exit(127);
        }
    }

    ~Cli()
    {
        if (!m_done) { kill(m_pid, SIGKILL); waitpid(m_pid, nullptr, 0); }
    }

    // True when the process has ended; the exit code is then in code().
    bool exited()
    {
        if (m_done) return true;
        int status = 0;
        if (waitpid(m_pid, &status, WNOHANG) == m_pid) {
            m_done = true;
            m_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        return m_done;
    }
    int code() const { return m_code; }

    std::string output() const
    {
        std::ifstream f(m_out);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

private:
    pid_t m_pid{-1};
    bool m_done{false};
    int m_code{-1};
    std::string m_out;
};

struct Stats {
    int lines{0};
    long frames{-1}, video_rtp{-1}, audio_rtp{-1};
};

Stats parse_stats(const std::string &out)
{
    Stats s;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("[CLI] MEDIA_STATS:", 0) != 0) continue;
        ++s.lines;
        std::sscanf(line.c_str(),
                    "[CLI] MEDIA_STATS: video_frames_decoded=%ld video_rtp_received=%ld audio_rtp_received=%ld",
                    &s.frames, &s.video_rtp, &s.audio_rtp);
    }
    return s;
}

std::string tmp_dir(const char *name)
{
    std::string d = std::string("/tmp/test_cli_media_") + std::to_string(getpid()) + "_" + name;
    mkdir(d.c_str(), 0755);
    return d;
}

std::vector<std::string> listen_args(const std::string &cfg, const char *duration, const char *timeout)
{
    return {"--user", "1003", "--password", "x", "--domain", "127.0.0.1", "--port", std::to_string(kSipPort),
            "--listen", "--auto-answer", "--audio-driver", "dummy", "--camera", "synthetic",
            "--config-dir", cfg, "--duration", duration, "--timeout", timeout};
}

double secs_since(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

void check_audio_stats(const std::string &out, long min_audio)
{
    Stats s = parse_stats(out);
    TEST_ASSERT(s.lines == 1, "exactly one MEDIA_STATS line");
    TEST_ASSERT(s.frames == 0 && s.video_rtp == 0, "an audio-only call prints zeros for video");
    TEST_ASSERT(s.audio_rtp >= min_audio, "audio_rtp_received counts what the peer sent");
}

void scenario_far_end_hangs_up(const std::string &cli)
{
    std::printf("--- listen mode: the far end hangs up ---\n");
    Peer peer;
    Cli proc(cli, listen_args(tmp_dir("a"), "60", "20"), tmp_dir("a") + "/out.txt");
    TEST_ASSERT(peer.pump(15000, [&] { return peer.registered; }), "the CLI registers");
    peer.send_invite();
    TEST_ASSERT(peer.pump(10000, [&] { return peer.call_up; }), "the CLI answers the call");
    peer.send_rtp = true;
    peer.pump(3000);
    peer.send_rtp = false;
    peer.send_bye();
    auto t0 = Clock::now();
    bool ended = peer.pump(5000, [&] { return proc.exited(); });
    std::string out = proc.output();
    TEST_ASSERT(ended, "the CLI exits within 5 s of the far end's BYE");
    std::printf("INFO: exited %.2f s after the BYE\n", secs_since(t0));
    TEST_ASSERT(proc.code() == 0, "the CLI exits 0");
    TEST_ASSERT(peer.rtp_from_cli >= 40, "the CLI sends G.711 every 20 ms with the dummy audio driver");
    check_audio_stats(out, 100);
    std::printf("PASS: far end hangs up (rtp from cli=%d)\n", peer.rtp_from_cli);
}

void scenario_listen_ends_itself(const std::string &cli)
{
    std::printf("--- listen mode: --duration ends the call ---\n");
    Peer peer;
    Cli proc(cli, listen_args(tmp_dir("b"), "3", "20"), tmp_dir("b") + "/out.txt");
    TEST_ASSERT(peer.pump(15000, [&] { return peer.registered; }), "the CLI registers");
    peer.send_invite();
    TEST_ASSERT(peer.pump(10000, [&] { return peer.call_up; }), "the CLI answers the call");
    peer.send_rtp = true;
    TEST_ASSERT(peer.pump(8000, [&] { return peer.bye_from_cli; }), "the CLI hangs up after --duration");
    auto t0 = Clock::now();
    bool ended = peer.pump(5000, [&] { return proc.exited(); });
    TEST_ASSERT(ended, "the CLI exits within 5 s of its own hangup");
    std::printf("INFO: exited %.2f s after the BYE\n", secs_since(t0));
    TEST_ASSERT(proc.code() == 0, "the CLI exits 0");
    check_audio_stats(proc.output(), 50);
    std::printf("PASS: listen mode ends itself\n");
}

void scenario_no_call(const std::string &cli)
{
    std::printf("--- listen mode: no call within --timeout ---\n");
    Peer peer;
    Cli proc(cli, listen_args(tmp_dir("c"), "3", "3"), tmp_dir("c") + "/out.txt");
    TEST_ASSERT(peer.pump(15000, [&] { return peer.registered; }), "the CLI registers");
    TEST_ASSERT(peer.pump(10000, [&] { return proc.exited(); }), "the CLI gives up and exits");
    TEST_ASSERT(proc.code() == 1, "no call within --timeout exits 1");
    TEST_ASSERT(parse_stats(proc.output()).lines == 0, "no call means no MEDIA_STATS line");
    std::printf("PASS: no call exits 1\n");
}

void scenario_call_mode(const std::string &cli)
{
    std::printf("--- call mode: MEDIA_STATS at hangup ---\n");
    Peer peer;
    peer.send_rtp = true;
    std::vector<std::string> args = {"--user", "1003", "--password", "x", "--domain", "127.0.0.1",
        "--port", std::to_string(kSipPort), "--call", "sip:1001@127.0.0.1:" + std::to_string(kSipPort),
        "--audio-driver", "dummy", "--camera", "synthetic", "--config-dir", tmp_dir("d"),
        "--duration", "3", "--timeout", "20"};
    Cli proc(cli, args, tmp_dir("d") + "/out.txt");
    TEST_ASSERT(peer.pump(30000, [&] { return proc.exited(); }), "the CLI ends its call and exits");
    TEST_ASSERT(proc.code() == 0, "the CLI exits 0");
    TEST_ASSERT(peer.bye_from_cli, "the CLI sent BYE");
    TEST_ASSERT(peer.rtp_from_cli >= 50, "the CLI sends G.711 with the dummy audio driver");
    check_audio_stats(proc.output(), 50);
    std::printf("PASS: call mode (rtp from cli=%d)\n", peer.rtp_from_cli);
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TEST_ASSERT(argc > 1, "usage: test_cli_media <path to sip_cli>");
    std::string cli = argv[1];
    std::atexit(kill_children);
    std::string only = argc > 2 ? argv[2] : "";
    if (only.empty() || only == "hangup") scenario_far_end_hangs_up(cli);
    if (only.empty() || only == "duration") scenario_listen_ends_itself(cli);
    if (only.empty() || only == "nocall") scenario_no_call(cli);
    if (only.empty() || only == "call") scenario_call_mode(cli);
    std::printf("ALL_CLI_MEDIA_TESTS_PASSED\n");
    return 0;
}
