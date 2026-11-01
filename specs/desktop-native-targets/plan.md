# Plan: Desktop Native Targets (macOS & Linux CLI / SDK) (`desktop-native-targets`)

**Goal**: Deliver CMake desktop build targets and native drivers for macOS and Linux systems (`platforms/desktop/`), providing a modern C++17 library wrapper (`sip::Client`, `sip::Call`), native audio drivers (PulseAudio, ALSA, CoreAudio), camera capture (V4L2, AVFoundation, synthetic test pattern), and an automated headless CLI test harness (`sip_cli`) running in Docker Linux against Asterisk PBX with zero regressions on existing mobile targets.

## Global Constraints

- **Language Standards**: C++17 for `sip_core` and `sip::` desktop wrapper; pure C11 `extern "C"` for `sip_abi.h`.
- **Supported Desktop Operating Systems**: macOS (12+ Darwin arm64 / x86_64) and Linux (Ubuntu 20.04+, Debian 11+ x86_64 / aarch64).
- **Build System**: Standard CMake 3.15+ with clean export targets (`sip::desktop` and `libsipcore`).
- **Symbol Visibility**: `-fvisibility=hidden` on all C/C++ sources; only public C++ SDK symbols and C ABI functions (`_sip_*`) exported.
- **Network & Socket ABI Safety**: `-DUSE_UNIXSOCK=ON` strictly enforced across all CMake targets to guarantee `struct sa` and `struct sip_msg` layout compatibility.
- **Audio Drivers**: Linux supports PulseAudio (`pulse`) and ALSA (`alsa`); macOS supports CoreAudio / AudioUnit (`audiounit`).
- **Video Capture**: Linux supports V4L2 (`v4l2`); macOS supports AVFoundation (`avcapture`); headless/CI environments automatically fall back to `SyntheticVideoCaptureDriver`.
- **Headless Operation**: The CLI test runner (`sip_cli`) must execute without X11, Wayland, or graphical desktop dependencies inside Docker containers.
- **Preserved Compatibility**: Zero regressions against existing iOS and Android test suites (`scripts/verify-ios-video-tests.sh`, `scripts/verify-android-sdk.sh`, `scripts/verify-audio-calling.sh`).

---

## 1. Frame

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

### User Stories
- **US35 (Desktop CMake & C++ Wrapper)**: As a desktop C++ application developer (Linux or macOS), I want to link against `libsipcore` using CMake and interact via an idiomatic modern C++ wrapper (`sip::Client`, `sip::Call`, `sip::IClientListener`), so that I can embed VoIP telephony into desktop and server applications without managing raw C ABI handles or pointers.
- **US36 (Headless Linux CLI Client)**: As a DevOps / CI engineer, I want a lightweight headless CLI client (`sip_cli`) that runs in a Docker container on Linux without X11 or Wayland, registers to a SIP server, places or answers audio/video calls, and exits with standard exit codes, so that VoIP regression testing can be fully automated in CI pipelines.
- **US38 (Sample Desktop App Integration)**: As a desktop developer, I want a clean, self-contained CMake sample application showing `target_link_libraries(my_app PRIVATE sip::desktop)`, so that I can quickly verify integration into my existing application build system.
- **US39 (Docker Linux CI Container)**: As a CI maintainer, I want a reproducible Docker container recipe that compiles `libsipcore` and `sip_cli` on Ubuntu/Debian and verifies end-to-end media transmission against Asterisk PBX with zero manual setup.
- **US40 (Linux PulseAudio & ALSA)**: As a Linux desktop user, I want the SDK to support both PulseAudio and ALSA audio drivers with runtime backend selection, so that audio capture and playback function across diverse Linux distributions and desktop environments.
- **US41 (macOS CoreAudio)**: As a macOS desktop user, I want the SDK to use native Apple AudioUnit / CoreAudio for low-latency microphone capture and speaker playback.
- **US42 (Linux V4L2 Camera Capture)**: As a Linux desktop user, I want the SDK to capture video from `/dev/video*` devices using standard V4L2 APIs, so that local webcam video is transmitted in video calls.
- **US43 (macOS AVFoundation Camera Capture)**: As a macOS desktop user, I want the SDK to capture video using macOS AVFoundation webcam inputs, so that my camera video is streamed cleanly.
- **US44 (Headless Synthetic Fallback)**: As an automated CI test runner without a physical webcam, I want the SDK to automatically generate synthetic video frames, so that video call negotiation and RTP transmission can be verified headlessly in Docker without camera hardware.

### Out of Scope
1. **Windows Platform SDK (.NET / C#)**: Reserved for Issue #18.
2. **Web (WASM / WebRTC)**: Explicitly deferred to Phase 2.
3. **SIP over TLS (SIPS) and SRTP**: TLS remains disabled (`USE_OPENSSL=OFF`) in this release; communication uses standard UDP/TCP.
4. **Third-Party Software Codecs (Opus, VP8, VP9)**: Baseline relies on built-in G.711 (PCMA/PCMU) and H.264.
5. **Desktop GUI Widget Libraries (Qt / GTK / Cocoa Views)**: Phase 1 provides the headless CLI, headless frame renderer driver, and C++ library. Rich GUI window integrations are layered above the driver interfaces.

### Implementation Decisions
- **`platforms/desktop/CMakeLists.txt` Structure**: Defines targets `sip_desktop` (static library containing C++ wrapper and drivers), `desktop_sample` (executable demonstrating integration), `sip_cli` (standalone CLI tool), and test targets. Configures include directories for `core/include`, `platforms/desktop/include`, and links against `sip_core` and platform audio/video frameworks.
- **`scripts/build-baresip-desktop.sh`**: Reproducible host build script compiling pinned `libre` (v4.12.0) and `baresip` (v4.12.0) for desktop host environments (macOS or Linux). On Linux, compiles modules `g711;pulse;alsa;account;stun;turn;ice;srtp`. On macOS, compiles modules `g711;audiounit;account;stun;turn;ice;srtp`.
- **C++ Object Model**: Modern C++17 in `platforms/desktop/include/sip/`:
  - `sip::Client`: Encapsulates `sip_client_t*`, RAII lifecycle, thread-safe listener dispatch, audio/video driver registration.
  - `sip::Call`: Encapsulates `sip_call_t*`, state queries, answer/hangup/hold/mute/DTMF actions.
  - `sip::IClientListener`: Pure virtual interface for typed callback notifications.
- **Audio Driver Architecture (`sip::AudioDriver`)**: Provides a unified audio backend selector (`AudioBackend::Auto`, `AudioBackend::PulseAudio`, `AudioBackend::Alsa`, `AudioBackend::CoreAudio`, `AudioBackend::Dummy`), which dynamically sets Baresip's audio module parameters before call start.
- **Video Capture Drivers**:
  - `sip::V4L2VideoCaptureDriver`: Linux-specific implementation using `/dev/video*`, `ioctl(VIDIOC_REQBUFS)`, `mmap`, and format conversion to YUV420P.
  - `sip::AVFoundationVideoCaptureDriver`: macOS-specific Objective-C++ implementation using `AVCaptureSession` and `AVCaptureVideoDataOutputSampleBufferDelegate`.
  - Automatic fallback to `sip_core::SyntheticVideoCaptureDriver` when no physical capture device is detected or when running in `--synthetic` mode.
- **Headless CLI (`sip_cli`)**: Self-contained C++ application in `platforms/desktop/src/cli/main.cpp`. Implements clean command-line option parsing, attaches `sip::IClientListener`, manages call timeouts, logs structured status lines, and returns exit code `0` on call success or `1` on error/timeout.
- **Docker CI Pipeline (`platforms/desktop/docker/Dockerfile.linux`)**: Multi-stage Debian/Ubuntu container image with `build-essential`, `cmake`, `libasound2-dev`, `libpulse-dev`, `libv4l-dev`, and Asterisk network integration.
- **`scripts/verify-desktop-sdk.sh`**: Automated end-to-end verification script testing local macOS build and running containerized Linux registration and audio/video call tests against local Asterisk PBX.

### Testing Decisions
- **Modern C++ API Unit Tests**: Verifies initialization, destruction, configuration, account registration, and state events via `platforms/desktop/tests/test_desktop_lifecycle.cpp`.
- **Audio Driver Tests**: Verifies audio backend selection, driver loading without error, and loopback audio frame processing via `platforms/desktop/tests/test_audio_drivers.cpp`.
- **Video Capture Tests**: Verifies frame delivery, format negotiation, and camera switching via `platforms/desktop/tests/test_video_capture.cpp`.
- **Sample Integration Verification**: Compiles `platforms/desktop/sample/` using CMake `add_subdirectory` and confirms proper linking against `sip::desktop`.
- **Live Asterisk 22 PBX Verification**: Runs `sip_cli` in Docker against Asterisk container:
  - Registers as Extension `1003`.
  - Dials Echo extension `9001` with audio and H.264 video.
  - Asserts call state `ESTABLISHED` and duration progression (`>= 2s`).
  - Hangs up cleanly and exits code 0.
- **Regression Guard**: Zero regressions on `scripts/verify-ios-video-tests.sh` and `scripts/verify-android-sdk.sh`.

---

## 2. Seam

The test seams attach at:
1. **Desktop C++ Public API Boundary (`sip::Client` & `sip::Call`)**: High-level C++ API seam tested by native desktop unit and integration test runners.
2. **Pluggable Media Driver Boundary (`sip_core::IVideoCaptureDriver` & Audio Backends)**: Driver interface seam tested by frame capture and backend selection tests.
3. **Headless CLI Process Seam (`sip_cli`)**: Command-line interface seam tested by exit code and structured stdout/stderr assertions.
4. **Containerized Linux Network Boundary (Docker Linux Container & Asterisk PBX)**: Full system integration seam tested by `scripts/verify-desktop-sdk.sh` running `sip_cli` inside Docker against the Asterisk PBX container.

---

## 3. Vertical Slices

```
+-----------------------------------------------------------------------------------+
| Slice 1: Desktop Native Build Pipeline & C++ SDK Wrapper (macOS & Linux)          |
| CMake targets, sip::Client, sip::Call, IClientListener, desktop_sample            |
+-----------------------------------------------------------------------------------+
                                   |
         +-------------------------+-------------------------+
         |                                                   |
         v                                                   v
+----------------------------------+        +----------------------------------+
| Slice 2: Desktop Audio Drivers   |        | Slice 3: Desktop Video Capture   |
| PulseAudio, ALSA, CoreAudio      |        | V4L2 (Linux), AVFoundation (Mac) |
+----------------------------------+        +----------------------------------+
         |                                                   |
         +-------------------------+-------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------------------+
| Slice 4: Headless Linux CLI Runner & Docker CI Harness                            |
| sip_cli, Dockerfile.linux, Asterisk Echo test, verify-desktop-sdk.sh               |
+-----------------------------------------------------------------------------------+
```

---

### Slice 1: Desktop Native CMake Build Pipeline & C++ SDK Wrapper (`platforms/desktop/`)

- **Files**:
  - `scripts/build-baresip-desktop.sh` (reproducible host build script for macOS & Linux)
  - `platforms/desktop/CMakeLists.txt`
  - `platforms/desktop/include/sip/types.hpp`
  - `platforms/desktop/include/sip/listener.hpp`
  - `platforms/desktop/include/sip/call.hpp`
  - `platforms/desktop/include/sip/client.hpp`
  - `platforms/desktop/include/sip/desktop.hpp`
  - `platforms/desktop/src/call.cpp`
  - `platforms/desktop/src/client.cpp`
  - `platforms/desktop/sample/CMakeLists.txt`
  - `platforms/desktop/sample/main.cpp`
  - `platforms/desktop/tests/test_desktop_lifecycle.cpp`
- **Interfaces**:
  - `sip::Client`:
    - `Client(std::string username, std::string domain)`
    - `int start(const std::string &config_path = "")`
    - `void stop()`
    - `int register_account()`
    - `int unregister_account()`
    - `bool is_registered() const`
    - `std::shared_ptr<Call> make_call(const std::string &target_uri, bool with_video = false)`
    - `void set_listener(std::shared_ptr<IClientListener> listener)`
    - `void set_mic_muted(bool muted)`
    - `bool is_mic_muted() const`
  - `sip::Call`:
    - `std::string get_id() const`
    - `std::string get_remote_uri() const`
    - `int answer(bool with_video = false)`
    - `int hangup(int code = 0, const std::string &reason = "Hangup")`
    - `int set_hold(bool hold)`
    - `int send_dtmf(const std::string &digits)`
    - `uint32_t get_duration() const`
    - `bool is_closed() const`
  - `sip::IClientListener`:
    - `virtual void on_registration_state(RegistrationState state, int code, const std::string &reason)`
    - `virtual void on_incoming_call(std::shared_ptr<Call> call)`
    - `virtual void on_call_state(std::shared_ptr<Call> call, CallState state, int code, const std::string &reason)`
- **Acceptance Criteria**:
  - Given a desktop system (macOS or Linux), When `cmake -S platforms/desktop -B .build/desktop` is configured and built, Then `libsipcore.a`, `libsip_desktop.a`, and `desktop_sample` compile with zero errors.
  - Given a `sip::Client` configured with valid Asterisk credentials (`1001` or `1003`), When `register_account()` is invoked, Then `on_registration_state` fires with `RegistrationState::Registered` and `code == 200`.
  - Given a third-party C++ desktop application with `CMakeLists.txt` containing `target_link_libraries(app PRIVATE sip::desktop)`, When built, Then the app links and executes cleanly.
- **Verify**: `cmake -S platforms/desktop -B .build/desktop && cmake --build .build/desktop --parallel && .build/desktop/tests/test_desktop_lifecycle`
- **Blocked by**: None

---

### Slice 2: Desktop Audio Drivers (PulseAudio & ALSA for Linux, CoreAudio for macOS)

- **Files**:
  - `platforms/desktop/include/sip/audio_driver.hpp`
  - `platforms/desktop/src/audio_driver.cpp`
  - Updates to `platforms/desktop/include/sip/client.hpp` & `platforms/desktop/src/client.cpp`
  - `platforms/desktop/tests/test_audio_drivers.cpp`
- **Interfaces**:
  - `sip::AudioBackend`: `enum class AudioBackend { Auto = 0, PulseAudio, Alsa, CoreAudio, Dummy }`
  - `sip::Client::set_audio_driver(AudioBackend backend, const std::string &device = "")`
  - `sip::Client::get_audio_driver() const -> AudioBackend`
  - `sip::Client::get_available_audio_backends() const -> std::vector<AudioBackend>`
- **Acceptance Criteria**:
  - Given Linux with PulseAudio or ALSA (or macOS with CoreAudio), When `set_audio_driver()` is called with the available backend, Then the audio subsystem initializes without throwing or falling back to invalid device errors.
  - Given an active audio call, When audio packets are captured and received, Then bidirectional audio RTP flows and mute/unmute functions correctly.
- **Verify**: `cmake --build .build/desktop --target test_audio_drivers && .build/desktop/tests/test_audio_drivers`
- **Blocked by**: Slice 1

---

### Slice 3: Desktop Video Capture Drivers (V4L2 for Linux, AVFoundation for macOS, Synthetic Fallback)

- **Files**:
  - `platforms/desktop/include/sip/video_driver.hpp`
  - `platforms/desktop/src/v4l2_capture.cpp`
  - `platforms/desktop/src/avfoundation_capture.mm`
  - Updates to `platforms/desktop/include/sip/client.hpp` & `platforms/desktop/src/client.cpp`
  - `platforms/desktop/tests/test_video_capture.cpp`
- **Interfaces**:
  - `sip::V4L2VideoCaptureDriver : public sip_core::IVideoCaptureDriver` (Linux `/dev/video*` capture)
  - `sip::AVFoundationVideoCaptureDriver : public sip_core::IVideoCaptureDriver` (macOS `AVCaptureSession`)
  - `sip::Client::set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver)`
  - `sip::Client::use_synthetic_video_capture()`
- **Acceptance Criteria**:
  - Given a macOS desktop with webcam (or Linux with V4L2 device), When `start()` is invoked on the capture driver, Then frames are read, converted to YUV420P `sip_core::VideoFrame`, and passed to the frame handler.
  - Given a headless environment without physical webcam, When `use_synthetic_video_capture()` is invoked, Then 30 fps synthetic test pattern frames are generated continuously without hardware errors.
- **Verify**: `cmake --build .build/desktop --target test_video_capture && .build/desktop/tests/test_video_capture`
- **Blocked by**: Slice 1

---

### Slice 4: Headless Linux CLI Test Runner & Automated Docker CI Harness (`sip_cli`, Dockerfile, PBX call verification script)

- **Files**:
  - `platforms/desktop/src/cli/main.cpp`
  - `platforms/desktop/docker/Dockerfile.linux`
  - `platforms/desktop/docker/entrypoint.sh`
  - `scripts/verify-desktop-sdk.sh`
- **Interfaces**:
  - `sip_cli` executable arguments:
    - `--user <ext>`, `--domain <host>`, `--password <pwd>`, `--port <port>`, `--transport <udp|tcp>`
    - `--call <uri>`, `--listen`, `--auto-answer`, `--duration <sec>`, `--video`, `--no-video`
    - `--audio-driver <pulse|alsa|coreaudio|dummy>`, `--camera <v4l2|avcapture|synthetic>`
    - `--timeout <sec>`
    - Exit code `0` on success; non-zero on failure or timeout.
  - `scripts/verify-desktop-sdk.sh`: single command verifying desktop targets end-to-end on both macOS host and Linux Docker container.
- **Acceptance Criteria**:
  - Given a Docker Linux environment, When `docker build -f platforms/desktop/docker/Dockerfile.linux -t sipsdk-desktop-test .` is executed, Then the image compiles `libsipcore` and `sip_cli` cleanly with zero warnings or errors.
  - Given the Asterisk PBX container is running, When `sip_cli` runs in the Linux Docker container with `--user 1003 --domain asterisk --call sip:9001@asterisk:5060 --video --duration 3`, Then it registers to Asterisk, negotiates an audio/video call with echo extension 9001, receives echoed media, hangs up cleanly after 3 seconds, and exits with code 0.
  - Given `bash scripts/verify-desktop-sdk.sh` is executed, Then both macOS desktop tests and Linux Docker tests execute and pass 100% with zero regressions on existing iOS and Android suites.
- **Verify**: `bash scripts/verify-desktop-sdk.sh`
- **Blocked by**: Slice 1, Slice 2, Slice 3

---

## 4. Review

- **Requirement Coverage**:
  - Criteria 1 (CMake builds `libsipcore` and desktop client binaries cleanly on macOS and Linux) -> Slice 1 & Slice 4.
  - Criteria 2 (PulseAudio and ALSA audio drivers successfully capture microphone audio and play remote audio) -> Slice 2.
  - Criteria 3 (Headless Linux CLI test runner executes in a Docker container, registers to Asterisk, and answers an audio/video call) -> Slice 4.
  - Criteria 4 (C++ desktop library can be integrated into standard desktop C++ applications) -> Slice 1 (`sip::desktop` & `platforms/desktop/sample/`).
- **No Placeholders**: Exact file paths, class names, method signatures, CLI flags, and verification commands are fully defined.
- **Dependency Flow**: Slice 1 establishes the CMake build and modern C++ wrapper; Slices 2 and 3 can be built in parallel; Slice 4 brings everything together into the headless CLI and Docker CI test harness.
