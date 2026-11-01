# Spec: Desktop Native Targets (macOS & Linux CLI / SDK) (`desktop-native-targets`)

## 1. Problem & Architecture Overview

### Problem
The unified SIP SDK (`sip_core`) provides a shared C++17 core engine and flat C ABI (`sip_abi`) decoupling VoIP state machines, re-INVITE stream preservation, and RTCP PLI/FIR keyframe recovery from mobile OS runtimes. While mobile platforms (iOS via SwiftPM and Android via Gradle/AAR) have turnkey SDK bindings, desktop developers (macOS and Linux) currently have no first-class SDK packaging, no modern C++ object-oriented library wrapper, and no automated Linux CI regression harness.

Desktop environments present distinct technical challenges:
1. **Linux Audio Diversity**: Linux desktop and server systems rely on either PulseAudio (`pulse`) or ALSA (`alsa`), requiring robust driver selection and runtime configuration without hardcoded mobile audio session hooks (`AVAudioSession` / `AudioManager`).
2. **Video Capture Fragmentation**: Video capture on Linux requires Video4Linux2 (`v4l2`) buffer management (`ioctl` memory-mapped ring buffers), while macOS desktop requires `AVFoundation` capture. Furthermore, headless server and CI container environments have no physical camera connected, requiring automated synthetic test-pattern fallback.
3. **Headless Linux CI Validation**: Without a lightweight, GUI-independent CLI client executable capable of running in Docker Linux containers, DevOps pipelines cannot automate regression testing of SIP registration, call negotiation, and bidirectional RTP audio/video transmission against PBX systems.
4. **Desktop C++ Integration Ergonomics**: Consuming raw C ABI pointers (`sip_client_t*`, `sip_call_t*`) directly in standard desktop C++ applications requires boilerplate memory management, manual thread synchronization, and repetitive callback mapping. Desktop developers need an idiomatic modern C++17 wrapper (`sip::Client`, `sip::Call`, `sip::IClientListener`) consumable via standard CMake `find_package` or `add_subdirectory`.

### Solution
Deliver the Desktop Platform SDK (`platforms/desktop/`) providing native macOS and Linux targets:
1. **Modern C++17 Object Model (`sip::Client` & `sip::Call`)**:
   - High-level C++17 classes wrapping `sip_abi` with RAII memory management, `std::shared_ptr` call lifecycle, and typed listener callbacks (`sip::IClientListener`).
   - Clean CMake target export (`sip::desktop` / `libsip_desktop.a`) that can be integrated seamlessly into third-party desktop C++ applications.
2. **Native Desktop Audio Driver Support**:
   - PulseAudio (`pulse`) and ALSA (`alsa`) audio backend integration for Linux systems via Baresip's native modules.
   - CoreAudio / AudioUnit (`audiounit`) audio backend integration for macOS desktop.
   - Runtime audio driver selection (`Client::set_audio_driver()`, `get_available_audio_backends()`).
3. **Pluggable Desktop Video Capture Drivers**:
   - Linux V4L2 video capture driver (`V4L2VideoCaptureDriver`) querying `/dev/video*`, configuring pixel formats (YUYV, YUV420), and streaming YUV420P frames into `sip_core::IVideoCaptureDriver`.
   - macOS AVFoundation video capture driver (`AVFoundationVideoCaptureDriver`) streaming webcam frames into `sip_core::IVideoCaptureDriver`.
   - Synthetic frame fallback (`SyntheticVideoCaptureDriver`) generating 30 fps color bar patterns with timestamps when running headless in CI.
4. **Headless Linux CLI Client (`sip_cli`)**:
   - Lightweight command-line binary supporting automated registration, outgoing call dialing, auto-answering incoming calls, media stream verification, configurable durations, and clean status exit codes.
5. **Docker Linux CI Test Harness**:
   - Automated Dockerfile (`platforms/desktop/docker/Dockerfile.linux`) compiling `libsipcore`, `sip_desktop`, and `sip_cli` on Debian/Ubuntu Linux with PulseAudio and ALSA.
   - End-to-end verification script (`scripts/verify-desktop-sdk.sh`) executing registration and two-way audio/video echo calls against Asterisk 22 PBX in Docker CI.

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #17.

---

## 2. Public API Specification

### Modern C++ Wrapper API (`platforms/desktop/include/sip/`)

#### Types & Enums (`platforms/desktop/include/sip/types.hpp`)
```cpp
namespace sip {

enum class RegistrationState {
    Unregistered = 0,
    Registering,
    Registered,
    Failed
};

enum class CallState {
    Incoming = 0,
    Ringing,
    Connecting,
    Established,
    Hold,
    Closed
};

enum class AudioBackend {
    Auto = 0,
    PulseAudio,
    Alsa,
    CoreAudio,
    Dummy
};

struct CallMediaInfo {
    int width{0};
    int height{0};
    bool on_hold{false};
    bool is_muted{false};
};

} // namespace sip
```

#### Client Listener (`platforms/desktop/include/sip/listener.hpp`)
```cpp
namespace sip {

class Call;

class IClientListener {
public:
    virtual ~IClientListener() = default;
    virtual void on_registration_state(RegistrationState state, int code, const std::string &reason) {}
    virtual void on_incoming_call(std::shared_ptr<Call> call) {}
    virtual void on_call_state(std::shared_ptr<Call> call, CallState state, int code, const std::string &reason) {}
    virtual void on_call_media(std::shared_ptr<Call> call, const CallMediaInfo &media) {}
    virtual void on_dtmf_digit(std::shared_ptr<Call> call, const std::string &digit) {}
};

} // namespace sip
```

#### Client & Call Classes (`platforms/desktop/include/sip/client.hpp` & `call.hpp`)
```cpp
namespace sip {

class Call {
public:
    std::string get_id() const;
    std::string get_remote_uri() const;
    std::string get_peer_display_name() const;
    bool has_video() const;
    bool is_outgoing() const;
    bool is_on_hold() const;
    bool is_muted() const;
    bool is_closed() const;
    uint32_t get_duration() const;

    int answer(bool with_video = false);
    int hangup(int code = 0, const std::string &reason = "Hangup");
    int set_hold(bool hold);
    int set_muted(bool muted);
    int send_dtmf(const std::string &digits);
    int request_keyframe();
};

class Client {
public:
    Client(std::string username, std::string domain);
    ~Client();

    void set_password(std::string password);
    void set_port(int port);
    void set_transport(std::string transport);
    void set_listener(std::shared_ptr<IClientListener> listener);

    int start(const std::string &config_path = "");
    void stop();

    int register_account();
    int unregister_account();
    bool is_registered() const;

    std::shared_ptr<Call> make_call(const std::string &target_uri, bool with_video = false);

    void set_mic_muted(bool muted);
    bool is_mic_muted() const;

    int set_audio_driver(AudioBackend backend, const std::string &device = "");
    AudioBackend get_audio_driver() const;
    std::vector<AudioBackend> get_available_audio_backends() const;

    void set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver);
    void use_synthetic_video_capture();

    static std::string get_version();
};

} // namespace sip
```

### CLI Client Interface (`sip_cli`)
Command-line syntax:
```bash
sip_cli --user <extension> --domain <host> --password <secret> [options]
```
Arguments:
- `--user <ext>`: SIP username / extension (e.g. `1003`).
- `--domain <host>`: Registrar / PBX host (e.g. `127.0.0.1` or `asterisk`).
- `--password <secret>`: SIP authentication secret.
- `--port <port>`: SIP port (default: `5060`).
- `--transport <udp|tcp>`: Protocol transport (default: `udp`).
- `--call <sip:target@host>`: Place outgoing call upon registration.
- `--listen`: Wait for incoming calls (auto-answers if specified).
- `--auto-answer`: Automatically answer incoming calls.
- `--video`: Enable H.264 video mode.
- `--duration <sec>`: Call duration in seconds before automatic hangup (default: `5`).
- `--audio-driver <pulse|alsa|coreaudio|dummy>`: Select audio backend.
- `--camera <v4l2|avcapture|synthetic>`: Select video capture backend.
- `--timeout <sec>`: Overall process timeout before error exit (default: `30`).

---

## 3. User Stories

### Desktop C++ SDK & CMake Integration
- **US35 (Desktop CMake & C++ Wrapper)**: As a desktop C++ application developer (Linux or macOS), I want to link against `libsipcore` using CMake and interact via an idiomatic modern C++ wrapper (`sip::Client`, `sip::Call`, `sip::IClientListener`), so that I can embed VoIP telephony into desktop and server applications without managing raw C ABI handles or pointers.
- **US38 (Sample Desktop App Integration)**: As a desktop developer, I want a clean, self-contained CMake sample application showing `target_link_libraries(my_app PRIVATE sip::desktop)`, so that I can quickly verify integration into my existing application build system.

### Headless Linux CI & Automation
- **US36 (Headless Linux CLI Client)**: As a DevOps / CI engineer, I want a lightweight headless CLI client (`sip_cli`) that runs in a Docker container on Linux without X11 or Wayland, registers to a SIP server, places or answers audio/video calls, and exits with standard exit codes, so that VoIP regression testing can be fully automated in CI pipelines.
- **US39 (Docker Linux CI Container)**: As a CI maintainer, I want a reproducible Docker container recipe that compiles `libsipcore` and `sip_cli` on Ubuntu/Debian and verifies end-to-end media transmission against Asterisk PBX with zero manual setup.

### Desktop Audio Subsystems
- **US40 (Linux PulseAudio & ALSA)**: As a Linux desktop user, I want the SDK to support both PulseAudio and ALSA audio drivers with runtime backend selection, so that audio capture and playback function across diverse Linux distributions and desktop environments.
- **US41 (macOS CoreAudio)**: As a macOS desktop user, I want the SDK to use native Apple AudioUnit / CoreAudio for low-latency microphone capture and speaker playback.

### Desktop Video Capture Subsystems
- **US42 (Linux V4L2 Camera Capture)**: As a Linux desktop user, I want the SDK to capture video from `/dev/video*` devices using standard V4L2 APIs, so that local webcam video is transmitted in video calls.
- **US43 (macOS AVFoundation Camera Capture)**: As a macOS desktop user, I want the SDK to capture video using macOS AVFoundation webcam inputs, so that my camera video is streamed cleanly.
- **US44 (Headless Synthetic Fallback)**: As an automated CI test runner without a physical webcam, I want the SDK to automatically generate synthetic video frames, so that video call negotiation and RTP transmission can be verified headlessly in Docker without camera hardware.

---

## 4. Out of Scope

1. **Windows Platform SDK (.NET / C#)**: Reserved for Issue #18.
2. **Web (WASM / WebRTC)**: Explicitly deferred to Phase 2.
3. **SIP over TLS (SIPS) and SRTP**: TLS remains disabled (`USE_OPENSSL=OFF`) in this release; communication uses standard UDP/TCP.
4. **Third-Party Software Codecs (Opus, VP8, VP9)**: Baseline relies on built-in G.711 (PCMA/PCMU) and H.264.
5. **Desktop GUI Widget Libraries (Qt / GTK / Cocoa Views)**: Phase 1 provides the headless CLI, headless frame renderer driver, and C++ library. Rich GUI window integrations are layered above the driver interfaces.

---

## 5. Alternatives Considered

- **Bundling Full FFmpeg/avcodec on Linux**: Rejected for the baseline because building ffmpeg from source across Linux distributions adds enormous build time and dependency bloat. We use native V4L2 capture and modular video codec drivers.
- **Requiring X11 / Wayland for All Desktop Tests**: Rejected because CI systems running in cloud containers have no display servers; a headless CLI and synthetic media pipeline is strictly required for CI regression testing.
- **Pure C ABI Only on Desktop**: Rejected because desktop C++ developers expect RAII, `std::string`, `std::shared_ptr`, and type-safe listeners rather than raw `extern "C"` pointer manipulation.

---

## 6. Definition of Done

The feature is considered complete when all of the following falsifiable predicates pass:

1. CMake builds `libsipcore.a`, `libsip_desktop.a`, `desktop_sample`, and `sip_cli` cleanly on macOS host with zero compilation or linker errors.
2. CMake builds `libsipcore.a`, `libsip_desktop.a`, and `sip_cli` cleanly inside an Ubuntu/Debian Linux Docker container.
3. PulseAudio and ALSA audio driver initialization tests pass cleanly.
4. The headless Linux CLI test runner (`sip_cli`) executes in a Docker container, registers to Asterisk (extension `1003`), dials the video echo service (`9001`), verifies active call duration and RTP transmission, and exits with status 0.
5. `scripts/verify-desktop-sdk.sh` executes all desktop test suites on macOS and inside Linux Docker, passing 100% of test cases with zero regressions on existing iOS and Android test suites.

---

## 7. Next Implementation Step

Create `specs/desktop-native-targets/plan.md` breaking this specification into vertical tracer-bullet slices.
