# Plan: Windows Platform SDK (.NET / C# Bindings) (`windows-platform-sdk`)

**Goal**: Deliver Windows desktop support for the unified SIP SDK via CMake build targets producing `libsipcore.dll` with exported C ABI symbols, an idiomatic .NET / C# wrapper library (`SipClient`, `SipCall`), Windows Media Foundation hardware H.264 video codec and webcam capture drivers, and a sample WPF desktop application with automated verification and CI integration.

## Global Constraints

- **Language Standards**: C++17 for `sip_core` and Windows media drivers; pure C11 `extern "C"` for `sip_abi.h`; C# 12 / .NET 8.0 (with .NET Standard 2.0 compatibility) for `SipSdk.Net`.
- **Supported Windows Targets**: Windows 10 (version 1809+) and Windows 11 (x64 and ARM64).
- **Build System**: CMake 3.15+ supporting both MSVC (Visual Studio 2022) and MinGW-w64 toolchains; .NET SDK CLI (`dotnet`).
- **Symbol Visibility & Export**: `SIP_CORE_BUILDING_DLL` defined on shared library targets; `__declspec(dllexport)` applied to all `_sip_*` C ABI functions; 0 unmanaged C++ symbols leaked.
- **Network & Socket Safety**: `-DUSE_UNIXSOCK=ON` strictly preserved across all build targets to maintain `struct sa` and `struct sip_msg` ABI layout compatibility.
- **Media Drivers**: Windows Media Foundation (MF/MFT) for hardware H.264 encoding/decoding and webcam capture (`IMFSourceReader`); automatic fallback to synthetic test pattern generator in headless CI or VM environments without cameras.
- **Thread Safety & UI Marshaling**: All public .NET events automatically dispatched to the active `SynchronizationContext` (WPF / WinUI UI thread) without manual `Dispatcher.Invoke`.
- **Preserved Compatibility**: Zero regressions against existing iOS, Android, and macOS/Linux desktop test suites (`scripts/verify-ios-video-tests.sh`, `scripts/verify-android-sdk.sh`, `scripts/verify-desktop-sdk.sh`).

---

## 1. Frame

### Problem
The unified SIP SDK (`sip_core`) provides a shared C++17 core engine and flat C ABI (`sip_abi`) that isolates VoIP call state machines, SDP re-INVITE video stream preservation, and RTCP PLI/FIR keyframe recovery from platform-specific runtimes. While Apple platforms (iOS/macOS via SwiftPM), Android (via Gradle/AAR), and Linux desktop (via CMake C++ wrapper) are supported, Windows desktop developers currently have no official SDK packaging, no native Windows dynamic library (`libsipcore.dll`), no idiomatic .NET / C# library bindings, and no hardware-accelerated video pipeline utilizing Windows Media Foundation (MF).

Windows desktop environments present specific technical requirements:
1. **Dynamic Link Library (DLL) & C ABI Export**: Windows application runtimes (including .NET, WinUI, WPF, and Win32) require a compiled shared library (`libsipcore.dll` or `sipcore.dll`) with explicitly exported C ABI functions (`__declspec(dllexport)`) and zero internal C++ symbol leakage.
2. **Idiomatic .NET / C# Integration**: Windows developers expect high-level .NET classes (`SipClient`, `SipCall`), standard C# event patterns (`EventHandler<T>`), RAII / `IDisposable` memory management, and automatic UI thread marshaling (`SynchronizationContext`) rather than unsafe pointer arithmetic or raw P/Invoke delegates.
3. **Hardware-Accelerated Video via Windows Media Foundation**: Windows relies on Media Foundation (MF) and Media Foundation Transforms (MFT) for webcam capture (`IMFSourceReader`) and hardware H.264 video encoding/decoding (`CLSID_CMSH264EncoderMFT` / `CLSID_CMSH264DecoderMFT`). Without native MF drivers, Windows video calling must fall back to CPU-intensive software codecs or fail entirely in environments lacking physical webcams.
4. **Desktop GUI Reference Implementation**: Windows developers require a turnkey desktop sample application (WPF / WinUI) demonstrating SIP registration against Asterisk PBX, dialer keypad with RFC 4733 DTMF tones, active call telephony controls (hold, mute, duration timer), and remote/local video rendering.

### Solution
Deliver the Windows Platform SDK (`platforms/windows/`) providing:
1. **MSVC & MinGW CMake Target (`libsipcore.dll`)**:
   - CMake build target producing `libsipcore.dll` (and import library `libsipcore.lib`) compiled with `-DSIP_CORE_BUILDING_DLL` and `SIP_ABI_EXPORT = __declspec(dllexport)`.
   - Windows network and media dependencies linked cleanly (`ws2_32`, `iphlpapi`, `mfplat`, `mfuuid`, `mfreadwrite`, `strmiids`, `d3d11`, `dxgi`).
   - Audited symbol table exporting 100% of `_sip_*` C ABI functions and zero unmanaged internal symbols.
2. **Idiomatic .NET / C# Class Library (`SipSdk.Net`)**:
   - Modern .NET 8 / .NET Standard 2.0 class library providing high-level `SipClient` and `SipCall` types.
   - P/Invoke interop layer (`SipSdk.Native.SipAbi`) with automatic cross-platform native library resolution (`NativeLibrary.SetDllImportResolver`).
   - Thread-safe event dispatching automatically synchronized to the application's `SynchronizationContext` (WPF / WinUI UI thread).
3. **Windows Media Foundation Hardware Video Drivers**:
   - `MfVideoCaptureDriver`: Windows webcam capture driver using `IMFSourceReader`, outputting YUV420P frames to `sip_core::IVideoCaptureDriver`, with seamless fallback to synthetic color-bar test patterns in headless CI or VM environments.
   - `MfVideoCodecDriver`: Hardware-accelerated H.264 encoder/decoder driver using Windows Media Foundation Transforms (MFT), conforming to RFC 6184 Constrained Baseline Profile `42e01f`.
   - `MfVideoRendererDriver`: Remote video frame renderer converting YUV420P frames to BGRA/RGB32 buffers consumable by .NET UI surfaces.
4. **Sample Desktop Windows Application (WPF)**:
   - Self-contained WPF application (`platforms/windows/sample/SipSdk.WpfSample`) demonstrating Asterisk PBX registration, outgoing/incoming audio and video calls, in-call actions (hold, mute, DTMF keypad), and video rendering via `SipVideoPanel`.
5. **Automated Verification Harness & CI Pipeline**:
   - Local verification script (`scripts/verify-windows-sdk.sh`) executing DLL cross-compilation, C ABI symbol export audits, and `dotnet test` execution.
   - GitHub Actions CI workflow (`.github/workflows/windows-build.yml`) running on `windows-latest` with MSVC and .NET 8 SDK.

### User Stories
- **US45 (Windows MSVC DLL & C ABI Export)**: As a Windows systems engineer, I want CMake to compile `libsipcore.dll` using MSVC / Windows toolchains with exported C ABI symbols and zero internal symbol leakage, so that Windows applications and runtimes can consume the SIP engine via native dynamic linking.
- **US46 (.NET C# P/Invoke Wrapper & Event Listeners)**: As a .NET / C# developer, I want an idiomatic `SipClient` and `SipCall` class library wrapping `sip_abi` with standard .NET events and automatic UI `SynchronizationContext` marshaling, so that I can manage SIP accounts and calls without writing unmanaged P/Invoke or pointer code.
- **US47 (Media Foundation H.264 Hardware Codec Driver)**: As a Windows video caller, I want the SDK to encode and decode H.264 video streams using Windows Media Foundation (MFT) hardware acceleration, so that video calling achieves low CPU utilization and smooth 30 fps performance.
- **US48 (Media Foundation Webcam Capture & Synthetic Fallback)**: As a Windows video caller, I want the SDK to capture webcam video via Windows Media Foundation SourceReader (with automatic synthetic color-bar fallback in VMs or headless CI), so that my camera video is streamed cleanly.
- **US49 (Sample Desktop Windows WPF Application)**: As a Windows desktop application developer, I want a complete sample WPF application demonstrating SIP registration against Asterisk PBX, dialer controls, in-call actions (hold, mute, DTMF), and video rendering via `SipVideoPanel`, so that I have a reference implementation for embedding VoIP into Windows apps.
- **US50 (Automated Windows CI & Cross-Compilation Verification)**: As a DevOps / CI maintainer, I want an automated verification script and CI pipeline validating the Windows DLL compilation, symbol exports, .NET test execution, and sample application build without manual Windows VM setup.

### Out of Scope
1. **UWP (Universal Windows Platform)**: UWP is deprecated by Microsoft in favor of WinUI 3 and WPF; targeting .NET 8 / WPF / WinUI.
2. **SIP over TLS & SRTP**: Retains `USE_OPENSSL=OFF` across all targets in this release; VoIP signaling uses standard UDP/TCP.
3. **Third-Party Codecs (Opus, VP8, VP9)**: Baseline relies on built-in G.711 (PCMA/PCMU) and H.264.
4. **DirectShow Legacy Camera APIs**: Focuses exclusively on modern Windows Media Foundation (`IMFSourceReader`), bypassing deprecated DirectShow filters.

### Implementation Decisions
- **`platforms/windows/CMakeLists.txt` & `core/CMakeLists.txt`**: Supports building `sip_core_shared` / `libsipcore.dll` when targeting Windows or when `-DBUILD_SHARED_LIBS=ON`. Defines `SIP_CORE_BUILDING_DLL` to activate `__declspec(dllexport)`. Links Windows system libraries: `ws2_32`, `iphlpapi`, `mfplat`, `mfuuid`, `mfreadwrite`, `strmiids`, `d3d11`, `dxgi`.
- **.NET SDK Architecture (`SipSdk.Net`)**:
  - Namespace `SipSdk` containing public classes `SipClient` and `SipCall`.
  - Namespace `SipSdk.Native` containing `SipNativeAbi` with P/Invoke signatures and C-compatible struct/delegate definitions.
  - Native library loader `NativeLibraryResolver` registering a `DllImportResolver` that loads `sipcore.dll` / `libsipcore.dll` on Windows, `libsipcore.so` on Linux, and `libsipcore.dylib` on macOS.
  - Automatic `SynchronizationContext` capture in `SipClient` constructor, ensuring all `RegistrationStateChanged`, `IncomingCall`, `StateChanged`, and `MediaChanged` events post directly to the caller's UI thread.
- **Media Foundation Drivers**:
  - `MfVideoCaptureDriver`: Queries device sources using `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID`, reads frames via `IMFSourceReader`, converts NV12/YUY2 to planar YUV420P, and delivers to `sip_core::IVideoCaptureDriver`. Automatically falls back to synthetic color-bar generator if no webcam is present.
  - `MfVideoCodecDriver`: Wraps `CLSID_CMSH264EncoderMFT` and `CLSID_CMSH264DecoderMFT`, enforces Constrained Baseline Profile `42e01f`, produces Annex B byte-stream NAL units, and handles RTCP PLI/FIR keyframe triggers.
  - `MfVideoRendererDriver`: Converts decoded YUV420P frames to BGRA32 and updates a shared memory buffer or Direct3D surface for UI presentation.
- **WPF Sample Application (`SipSdk.WpfSample`)**:
  - MVVM architecture with `MainViewModel` binding to `SipClient` and `SipCall`.
  - Registration controls (Username, Domain, Password, Port, Transport, Register/Unregister button, Status badge).
  - Dialer controls (SIP URI input, Audio Call, Video Call, Hangup, DTMF keypad).
  - Telephony controls (Mute mic, Hold, Call duration display).
  - `SipVideoPanel`: Custom WPF control hosting an `Image` backed by a `WriteableBitmap`, updated on frame callbacks with zero tearing.
- **Verification & CI**:
  - Docker-based MinGW-w64 cross-compilation container (`platforms/windows/docker/Dockerfile.windows`) capable of building `libsipcore.dll` and auditing symbol exports.
  - `.github/workflows/windows-build.yml` running on native `windows-latest` GitHub Actions runner with MSVC and .NET 8.
  - Single entrypoint verification script `scripts/verify-windows-sdk.sh`.

### Testing Decisions
- **C ABI Dynamic Linking & Symbol Export Tests**: Verifies that `libsipcore.dll` exports all required `_sip_*` functions, exports 0 unmanaged C++ symbols, and successfully loads via `LoadLibrary` / P/Invoke.
- **.NET SDK Unit & P/Invoke Tests (`SipSdk.Tests`)**:
  - Tests client creation, initialization, configuration, and destruction lifecycle.
  - Tests event listener registrations and delegate invocation.
  - Tests call session state transitions (`Incoming`, `Ringing`, `Connecting`, `Established`, `Hold`, `Closed`).
  - Tests telephony controls (answer, hangup with custom code/reason, hold toggle, mute toggle, DTMF digit sending).
  - Tests UI thread marshaling via mock `SynchronizationContext`.
- **Media Foundation Driver Tests**:
  - Verifies webcam enumeration and fallback to synthetic test pattern.
  - Verifies H.264 encoder initialization and parameter set generation.
  - Verifies video renderer format conversion (YUV420P to BGRA32).
- **WPF Sample Application Build Verification**:
  - Compiles `SipSdk.WpfSample.csproj` with zero errors or warnings.
- **Zero-Regression Guard**:
  - Runs `bash scripts/verify-desktop-sdk.sh` and `bash scripts/verify-core-lifecycle.sh` to ensure macOS/Linux desktop and mobile core implementations remain 100% green.

---

## 2. Seam

The test seams attach at:
1. **Dynamic Link Library C ABI Boundary (`core/include/sip_abi.h` / `libsipcore.dll`)**: Flat `extern "C"` export seam tested by symbol audit tools (`nm`, `dumpbin`) and native loader tests.
2. **Idiomatic .NET / C# Public API Boundary (`SipClient` & `SipCall`)**: Managed API seam tested by .NET test runner (`dotnet test platforms/windows/dotnet/tests/SipSdk.Tests/`).
3. **Pluggable Media Driver Boundary (`sip_core::IVideoCaptureDriver`, `IVideoCodecDriver`, `IVideoRendererDriver`)**: Driver interface seam tested by frame delivery and format conversion tests.
4. **WPF Application & PBX Integration Boundary (`SipSdk.WpfSample`)**: Desktop UI and network seam demonstrating Asterisk PBX registration and audio/video calling.

---

## 3. Vertical Slices

```
+-----------------------------------------------------------------------------------+
| Slice 1: Windows CMake Build Pipeline, MSVC DLL Target & C ABI Symbol Export      |
| platforms/windows/CMakeLists.txt, libsipcore.dll, SIP_CORE_BUILDING_DLL           |
+-----------------------------------------------------------------------------------+
                                   |
         +-------------------------+-------------------------+
         |                                                   |
         v                                                   v
+----------------------------------+        +----------------------------------+
| Slice 2: .NET / C# SDK Wrapper   |        | Slice 3: Windows Media Foundation|
| SipClient, SipCall, P/Invoke,    |        | Hardware H.264 MFT Codec,        |
| Events, SynchronizationContext   |        | Webcam Capture & Synthetic       |
+----------------------------------+        +----------------------------------+
         |                                                   |
         +-------------------------+-------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------------------+
| Slice 4: Desktop Windows WPF Sample Application & Automated Verification Suite    |
| SipSdk.WpfSample, SipVideoPanel, verify-windows-sdk.sh, windows-build.yml         |
+-----------------------------------------------------------------------------------+
```

---

### Slice 1: Windows CMake Build Pipeline, MSVC DLL Target & C ABI Symbol Export (`libsipcore.dll`)

- **User stories covered**: US45
- **Device tier**: Windows Desktop / Docker Windows cross-compilation tier
- **Files**:
  - `core/CMakeLists.txt` (shared DLL target support on Windows)
  - `platforms/windows/CMakeLists.txt`
  - `platforms/windows/docker/Dockerfile.windows`
  - `platforms/windows/tests/test_windows_abi.cpp`
- **Interfaces**:
  - CMake targets: `sip_core_shared` producing `libsipcore.dll` (Windows) and `libsipcore.lib`
  - Compiler defines: `SIP_CORE_BUILDING_DLL=1`, `USE_UNIXSOCK=ON`
  - C ABI exports: all `_sip_*` functions defined in `core/include/sip_abi.h` marked with `SIP_ABI_EXPORT` (`__declspec(dllexport)`)
- **Acceptance criteria**:
  1. **Given** a Windows build environment (MSVC or MinGW cross-compiler), **when** CMake configures and builds `platforms/windows`, **then** `libsipcore.dll` and `libsipcore.lib` are produced with zero compilation or linker errors.
  2. **Given** the compiled `libsipcore.dll`, **when** exported symbols are inspected via `nm -g` / `dumpbin /exports`, **then** all public C ABI functions (`_sip_client_create`, `_sip_client_init`, `_sip_call_make`, etc.) are present and zero unmanaged C++ symbols are exported.
  3. **Given** `test_windows_abi`, **when** executed, **then** it dynamically loads `libsipcore.dll` and verifies function pointers for client and call operations.
- **Verify**: `docker build -t sipsdk-windows-test -f platforms/windows/docker/Dockerfile.windows . && docker run --rm sipsdk-windows-test bash -c "nm -gD /build/libsipcore.dll | grep ' T _sip_'"`
- **Blocked by**: None

---

### Slice 2: Idiomatic .NET / C# Wrapper Library (`SipSdk.Net` P/Invoke, `SipClient`, `SipCall`, Events, UI Marshaling)

- **User stories covered**: US46
- **Device tier**: .NET 8 / C# runtime tier
- **Files**:
  - `platforms/windows/dotnet/SipSdk.sln`
  - `platforms/windows/dotnet/src/SipSdk/SipSdk.csproj`
  - `platforms/windows/dotnet/src/SipSdk/Native/SipNativeAbi.cs`
  - `platforms/windows/dotnet/src/SipSdk/Native/NativeLibraryResolver.cs`
  - `platforms/windows/dotnet/src/SipSdk/SipEnums.cs`
  - `platforms/windows/dotnet/src/SipSdk/SipEventArgs.cs`
  - `platforms/windows/dotnet/src/SipSdk/SipClient.cs`
  - `platforms/windows/dotnet/src/SipSdk/SipCall.cs`
  - `platforms/windows/dotnet/tests/SipSdk.Tests/SipSdk.Tests.csproj`
  - `platforms/windows/dotnet/tests/SipSdk.Tests/SipClientTests.cs`
  - `platforms/windows/dotnet/tests/SipSdk.Tests/SipCallTests.cs`
- **Interfaces**:
  - `SipSdk.SipClient`:
    - `public int Start(string configPath = null)`
    - `public void Stop()`
    - `public int Register(string username, string domain, string password, string transport = "udp", int port = 5060)`
    - `public int Unregister()`
    - `public bool IsRegistered { get; }`
    - `public SipCall MakeCall(string targetUri, bool withVideo = false)`
    - `public void SetMicMuted(bool muted)`
    - `public bool IsMicMuted { get; }`
    - `public event EventHandler<RegistrationStateChangedEventArgs> RegistrationStateChanged`
    - `public event EventHandler<IncomingCallEventArgs> IncomingCall`
  - `SipSdk.SipCall`:
    - `public string Id { get; }`
    - `public string RemoteUri { get; }`
    - `public string PeerDisplayName { get; }`
    - `public SipCallState State { get; }`
    - `public bool HasVideo { get; }`
    - `public bool IsOutgoing { get; }`
    - `public bool IsOnHold { get; }`
    - `public bool IsMuted { get; }`
    - `public uint Duration { get; }`
    - `public int Answer(bool withVideo = false)`
    - `public int Hangup(int statusCode = 0, string reason = null)`
    - `public int SetHold(bool hold)`
    - `public int SetMuted(bool muted)`
    - `public int SendDtmf(string digits)`
    - `public event EventHandler<CallStateChangedEventArgs> StateChanged`
    - `public event EventHandler<CallMediaChangedEventArgs> MediaChanged`
    - `public event EventHandler<DtmfReceivedEventArgs> DtmfReceived`
- **Acceptance criteria**:
  1. **Given** `SipSdk.Net`, **when** `dotnet build platforms/windows/dotnet/SipSdk.sln` is executed, **then** `SipSdk.dll` compiles targeting .NET 8.0 and .NET Standard 2.0 with zero warnings or errors.
  2. **Given** a mock native ABI or compiled library, **when** `SipClient.Register()` is called, **then** `RegistrationStateChanged` fires with the correct status code and state without throwing unmanaged delegate exceptions.
  3. **Given** a caller thread with an active `SynchronizationContext`, **when** native callbacks are dispatched from the background worker thread, **then** the corresponding .NET events are marshaled and invoked on the captured `SynchronizationContext`.
  4. **Given** `dotnet test platforms/windows/dotnet/tests/SipSdk.Tests/SipSdk.Tests.csproj`, **when** executed, **then** all unit tests pass 100%.
- **Verify**: `dotnet test platforms/windows/dotnet/tests/SipSdk.Tests/SipSdk.Tests.csproj`
- **Blocked by**: Slice 1

---

### Slice 3: Windows Media Foundation Video Drivers (Hardware H.264 MFT Codec, Webcam Capture & Synthetic Fallback)

- **User stories covered**: US47, US48
- **Device tier**: Windows Media Foundation multimedia driver tier
- **Files**:
  - `platforms/windows/include/sip/windows_media.h`
  - `platforms/windows/src/media/mf_capture.cpp`
  - `platforms/windows/src/media/mf_codec.cpp`
  - `platforms/windows/src/media/mf_renderer.cpp`
  - `platforms/windows/tests/test_mf_drivers.cpp`
- **Interfaces**:
  - `sip::windows::MfVideoCaptureDriver : public sip_core::IVideoCaptureDriver`:
    - `int start(int width, int height, int fps) override`
    - `void stop() override`
    - `void set_frame_handler(FrameHandler handler, void *arg) override`
    - `void set_enabled(bool enabled) override`
    - `bool is_enabled() const override`
  - `sip::windows::MfVideoCodecDriver : public sip_core::IVideoCodecDriver`:
    - `int register_codec() override`
    - `void unregister_codec() override`
  - `sip::windows::MfVideoRendererDriver : public sip_core::IVideoRendererDriver`:
    - `void render_frame(const sip_video_frame_t &frame) override`
    - `void on_stream_resized(int width, int height) override`
    - `void clear() override`
- **Acceptance criteria**:
  1. **Given** a Windows system with webcam devices, **when** `MfVideoCaptureDriver::start()` is called, **then** `IMFSourceReader` captures video frames and streams YUV420P frames to the frame handler.
  2. **Given** a headless Windows environment or VM without physical cameras, **when** `MfVideoCaptureDriver::start()` is called, **then** it automatically detects device absence and generates 30 fps timestamped synthetic color-bar test pattern frames without error.
  3. **Given** `MfVideoCodecDriver`, **when** initialized, **then** it successfully loads MFT H.264 encoder/decoder transforms and registers with Baresip's video codec list.
  4. **Given** `test_mf_drivers`, **when** executed, **then** driver instantiation, format negotiation, and frame delivery assertions pass 100%.
- **Verify**: `cmake --build .build/windows --target test_mf_drivers && .build/windows/tests/test_mf_drivers` (or via Docker Windows CI test)
- **Blocked by**: Slice 1

---

### Slice 4: Desktop Windows WPF Sample Application & Automated Verification Suite

- **User stories covered**: US49, US50
- **Device tier**: Windows WPF UI & End-to-End PBX integration tier
- **Files**:
  - `platforms/windows/sample/SipSdk.WpfSample/SipSdk.WpfSample.csproj`
  - `platforms/windows/sample/SipSdk.WpfSample/App.xaml`
  - `platforms/windows/sample/SipSdk.WpfSample/App.xaml.cs`
  - `platforms/windows/sample/SipSdk.WpfSample/MainWindow.xaml`
  - `platforms/windows/sample/SipSdk.WpfSample/MainWindow.xaml.cs`
  - `platforms/windows/sample/SipSdk.WpfSample/ViewModels/MainViewModel.cs`
  - `platforms/windows/sample/SipSdk.WpfSample/Controls/SipVideoPanel.cs`
  - `scripts/verify-windows-sdk.sh`
  - `.github/workflows/windows-build.yml`
- **Interfaces**:
  - `SipSdk.WpfSample.ViewModels.MainViewModel`:
    - Properties: `Username`, `Domain`, `Password`, `Port`, `Transport`, `RegistrationStatus`, `IsRegistered`, `TargetUri`, `CallStateText`, `IsCallActive`, `IsOnHold`, `IsMuted`, `CallDurationText`
    - Commands: `RegisterCommand`, `UnregisterCommand`, `AudioCallCommand`, `VideoCallCommand`, `HangupCommand`, `ToggleHoldCommand`, `ToggleMuteCommand`, `SendDtmfCommand`
  - `SipSdk.WpfSample.Controls.SipVideoPanel`:
    - WPF `FrameworkElement` / `Image` rendering decoded video frames via `WriteableBitmap`
  - `scripts/verify-windows-sdk.sh`:
    - End-to-end verification script validating Windows DLL build, exported C ABI symbols, .NET SDK tests, sample app compilation, and zero-regression check on existing desktop/mobile test suites.
  - `.github/workflows/windows-build.yml`:
    - GitHub Actions workflow testing on `windows-latest` with MSVC and .NET 8.
- **Acceptance criteria**:
  1. **Given** `SipSdk.WpfSample`, **when** `dotnet build platforms/windows/sample/SipSdk.WpfSample/SipSdk.WpfSample.csproj` is executed, **then** the WPF desktop application compiles with zero errors.
  2. **Given** the WPF sample application launched with valid Asterisk credentials (extension `1001`), **when** the user clicks "Register", **then** the UI displays "Registered (200 OK)" with a green indicator.
  3. **Given** an active video call to extension `9001` (Echo), **when** the call connects, **then** the duration timer increments, `SipVideoPanel` renders the echoed video stream, and clicking "Hold", "Mute", and DTMF buttons sends signaling without crashing the UI.
  4. **Given** `bash scripts/verify-windows-sdk.sh` is executed, **then** all Windows build targets, symbol audits, and .NET tests pass cleanly with zero regressions on existing suites.
- **Verify**: `bash scripts/verify-windows-sdk.sh`
- **Blocked by**: Slice 1, Slice 2, Slice 3

---

## 4. Review

- **Requirement Coverage**:
  - Criteria 1 (CMake MSVC toolchain compiles `libsipcore.dll` with exported C ABI symbols) -> Slice 1 (`platforms/windows/CMakeLists.txt`, `core/CMakeLists.txt`, symbol audit).
  - Criteria 2 (C# P/Invoke wrapper wraps all `sip_abi` functions with .NET event listeners) -> Slice 2 (`SipSdk.Net`, `SipClient`, `SipCall`, `SipSdk.Tests`).
  - Criteria 3 (Windows Media Foundation driver encodes webcam input into H.264 and decodes remote video streams) -> Slice 3 (`MfVideoCaptureDriver`, `MfVideoCodecDriver`, `MfVideoRendererDriver`).
  - Criteria 4 (Sample Windows desktop application registers to Asterisk PBX, places a call, and renders remote video) -> Slice 4 (`SipSdk.WpfSample`, `SipVideoPanel`, `verify-windows-sdk.sh`).
- **No Placeholders**: Exact file paths, class names, method signatures, C ABI symbols, and verification commands are fully specified.
- **Dependency Flow**: Slice 1 establishes the Windows CMake build and dynamic library C ABI export; Slices 2 and 3 build the .NET SDK wrapper and native Media Foundation drivers in parallel; Slice 4 unifies everything in the WPF sample application and automated CI verification suite.
