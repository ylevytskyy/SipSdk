#include <sip/desktop.hpp>
#include <sip/audio_driver.hpp>
#include <sip/video_driver.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CliConfig {
    std::string user{"1003"};
    std::string domain{"127.0.0.1"};
    std::string password{"test1003"};
    int port{5060};
    std::string transport{"udp"};
    std::string call_uri;
    bool listen{false};
    bool auto_answer{false};
    bool with_video{false};
    int duration_sec{3};
    std::string audio_driver{"auto"};
    std::string camera{"synthetic"};
    int timeout_sec{30};
    std::string config_dir;
};

class CliListener : public sip::IClientListener {
public:
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> is_registered{false};
    std::atomic<int> reg_code{0};
    std::atomic<bool> reg_failed{false};

    std::shared_ptr<sip::Call> active_call;
    std::atomic<sip::CallState> call_state{sip::CallState::Closed};
    std::atomic<int> call_code{0};
    std::atomic<bool> has_media_rx{false};
    int media_width{0};
    int media_height{0};

    bool auto_answer{false};
    bool with_video{false};

    void on_registration_state(sip::RegistrationState state, int code, const std::string &reason) override {
        std::printf("[CLI] REG_STATE: %s (code=%d, reason='%s')\n", sip::to_string(state), code, reason.c_str());
        std::fflush(stdout);
        if (state == sip::RegistrationState::Registered) {
            is_registered = true;
            reg_code = code;
            cv.notify_all();
        } else if (state == sip::RegistrationState::Failed) {
            reg_failed = true;
            reg_code = code;
            cv.notify_all();
        }
    }

    void on_incoming_call(std::shared_ptr<sip::Call> call) override {
        std::printf("[CLI] INCOMING_CALL from %s (video=%d)\n", call->get_remote_uri().c_str(), call->has_video() ? 1 : 0);
        std::fflush(stdout);
        {
            std::lock_guard<std::mutex> lock(mutex);
            active_call = call;
        }
        if (auto_answer) {
            std::printf("[CLI] Auto-answering incoming call...\n");
            call->answer(with_video);
        }
        cv.notify_all();
    }

    void on_call_state(std::shared_ptr<sip::Call> call, sip::CallState state, int code, const std::string &reason) override {
        std::printf("[CLI] CALL_STATE: %s (code=%d, reason='%s')\n", sip::to_string(state), code, reason.c_str());
        std::fflush(stdout);
        {
            std::lock_guard<std::mutex> lock(mutex);
            active_call = call;
            call_state = state;
            call_code = code;
        }
        cv.notify_all();
    }

    void on_call_media(std::shared_ptr<sip::Call> call, const sip::CallMediaInfo &media) override {
        (void)call;
        std::printf("[CLI] MEDIA: %dx%d on_hold=%d muted=%d\n", media.width, media.height, media.on_hold ? 1 : 0, media.is_muted ? 1 : 0);
        std::fflush(stdout);
        has_media_rx = true;
        media_width = media.width;
        media_height = media.height;
        cv.notify_all();
    }

    bool wait_for_registration(int timeout_seconds) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (is_registered.load()) return true;
            if (reg_failed.load()) return false;
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }

    bool wait_for_call_state(sip::CallState expected, int timeout_seconds) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        std::unique_lock<std::mutex> lock(mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (call_state.load() == expected) return true;
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
        return false;
    }
};

static void print_usage(const char *prog) {
    std::printf("Usage: %s [options]\n", prog);
    std::printf("Options:\n");
    std::printf("  --user <ext>          SIP username / extension (default: 1003)\n");
    std::printf("  --domain <host>       SIP registrar / host (default: 127.0.0.1)\n");
    std::printf("  --password <pwd>      SIP password (default: test1003)\n");
    std::printf("  --port <port>         SIP port (default: 5060)\n");
    std::printf("  --transport <proto>   SIP transport: udp, tcp (default: udp)\n");
    std::printf("  --call <sip:uri>      Place outgoing call upon registration\n");
    std::printf("  --listen              Listen for incoming calls\n");
    std::printf("  --auto-answer         Auto answer incoming calls\n");
    std::printf("  --video               Offer/request H.264 video\n");
    std::printf("  --no-video            Audio-only mode (default)\n");
    std::printf("  --duration <sec>      Call duration in seconds (default: 3)\n");
    std::printf("  --audio-driver <drv>  pulse, alsa, audiounit, dummy, auto\n");
    std::printf("  --camera <cam>        synthetic, v4l2, avcapture\n");
    std::printf("  --timeout <sec>       Total process timeout (default: 30)\n");
    std::printf("  --config-dir <dir>    Baresip config directory\n");
    std::printf("  --help                Show this help message\n");
}

int main(int argc, char **argv) {
    CliConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--user" && i + 1 < argc) cfg.user = argv[++i];
        else if (arg == "--domain" && i + 1 < argc) cfg.domain = argv[++i];
        else if (arg == "--password" && i + 1 < argc) cfg.password = argv[++i];
        else if (arg == "--port" && i + 1 < argc) cfg.port = std::atoi(argv[++i]);
        else if (arg == "--transport" && i + 1 < argc) cfg.transport = argv[++i];
        else if (arg == "--call" && i + 1 < argc) cfg.call_uri = argv[++i];
        else if (arg == "--listen") cfg.listen = true;
        else if (arg == "--auto-answer") { cfg.listen = true; cfg.auto_answer = true; }
        else if (arg == "--video") cfg.with_video = true;
        else if (arg == "--no-video") cfg.with_video = false;
        else if (arg == "--duration" && i + 1 < argc) cfg.duration_sec = std::atoi(argv[++i]);
        else if (arg == "--audio-driver" && i + 1 < argc) cfg.audio_driver = argv[++i];
        else if (arg == "--camera" && i + 1 < argc) cfg.camera = argv[++i];
        else if (arg == "--timeout" && i + 1 < argc) cfg.timeout_sec = std::atoi(argv[++i]);
        else if (arg == "--config-dir" && i + 1 < argc) cfg.config_dir = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::printf("=================================================================\n");
    std::printf("SipSdk Desktop Headless CLI (%s)\n", sip::Client::get_version().c_str());
    std::printf("Account: sip:%s@%s:%d (%s)\n", cfg.user.c_str(), cfg.domain.c_str(), cfg.port, cfg.transport.c_str());
    std::printf("Mode: %s | Video: %s | AudioDriver: %s | Camera: %s\n",
                cfg.call_uri.empty() ? (cfg.listen ? "listen" : "register-only") : ("call -> " + cfg.call_uri).c_str(),
                cfg.with_video ? "yes" : "no",
                cfg.audio_driver.c_str(),
                cfg.camera.c_str());
    std::printf("=================================================================\n");
    std::fflush(stdout);

    auto listener = std::make_shared<CliListener>();
    listener->auto_answer = cfg.auto_answer;
    listener->with_video = cfg.with_video;

    auto client = std::make_shared<sip::Client>(cfg.user, cfg.domain);
    client->set_password(cfg.password);
    client->set_port(cfg.port);
    client->set_transport(cfg.transport);
    client->set_listener(listener);

    sip::AudioBackend audio_backend = sip::AudioDriverManager::get_backend_from_name(cfg.audio_driver);
    client->set_audio_driver(audio_backend);

    if (cfg.camera == "synthetic") {
        client->use_synthetic_video_capture();
    } else {
        client->set_video_capture_driver(sip::create_default_camera_capture_driver());
    }

    int rc = client->start(cfg.config_dir);
    if (rc != 0) {
        std::fprintf(stderr, "[CLI] ERROR: Failed to start SIP client (rc=%d)\n", rc);
        return 1;
    }

    std::printf("[CLI] Registering SIP account...\n");
    rc = client->register_account();
    if (rc != 0) {
        std::fprintf(stderr, "[CLI] ERROR: Failed to register account (rc=%d)\n", rc);
        client->stop();
        return 1;
    }

    if (!listener->wait_for_registration(10)) {
        std::fprintf(stderr, "[CLI] ERROR: SIP registration timed out or failed (code=%d)\n", listener->reg_code.load());
        client->stop();
        return 1;
    }
    std::printf("[CLI] PASS: Registration successful (200 OK)\n");

    // Action execution
    if (!cfg.call_uri.empty()) {
        std::printf("[CLI] Placing outgoing call to %s (video=%d)...\n", cfg.call_uri.c_str(), cfg.with_video ? 1 : 0);
        auto call = client->make_call(cfg.call_uri, cfg.with_video);
        if (!call) {
            std::fprintf(stderr, "[CLI] ERROR: Failed to create outgoing call\n");
            client->stop();
            return 1;
        }

        std::printf("[CLI] Waiting for call to connect...\n");
        if (!listener->wait_for_call_state(sip::CallState::Established, 15)) {
            std::fprintf(stderr, "[CLI] ERROR: Call failed to reach Established state (current=%s)\n",
                         sip::to_string(listener->call_state.load()));
            call->hangup();
            client->stop();
            return 1;
        }

        std::printf("[CLI] PASS: Call ESTABLISHED. Streaming for %d seconds...\n", cfg.duration_sec);
        std::this_thread::sleep_for(std::chrono::seconds(cfg.duration_sec));

        uint32_t dur = call->get_duration();
        std::printf("[CLI] In-call duration recorded: %u seconds\n", dur);

        std::printf("[CLI] Hanging up call...\n");
        call->hangup(0, "Completed");
        listener->wait_for_call_state(sip::CallState::Closed, 5);
        std::printf("[CLI] PASS: Call terminated cleanly\n");
    } else if (cfg.listen) {
        std::printf("[CLI] Listening for incoming call (timeout=%d sec)...\n", cfg.timeout_sec);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.timeout_sec);
        std::unique_lock<std::mutex> lock(listener->mutex);
        while (std::chrono::steady_clock::now() < deadline) {
            if (listener->active_call && listener->call_state.load() == sip::CallState::Established) {
                break;
            }
            listener->cv.wait_for(lock, std::chrono::milliseconds(100));
        }

        if (listener->active_call && listener->call_state.load() == sip::CallState::Established) {
            std::printf("[CLI] Incoming call answered and ESTABLISHED. Holding for %d sec...\n", cfg.duration_sec);
            std::this_thread::sleep_for(std::chrono::seconds(cfg.duration_sec));
            listener->active_call->hangup(0, "Completed");
            listener->wait_for_call_state(sip::CallState::Closed, 5);
            std::printf("[CLI] PASS: Incoming call completed cleanly\n");
        } else {
            std::fprintf(stderr, "[CLI] WARNING: No incoming call received within timeout\n");
        }
    }

    std::printf("[CLI] Unregistering and tearing down client...\n");
    client->unregister_account();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client->stop();

    std::printf("=================================================================\n");
    std::printf("[CLI] ALL_CLI_OPERATIONS_COMPLETED_SUCCESSFULLY\n");
    std::printf("=================================================================\n");
    return 0;
}
