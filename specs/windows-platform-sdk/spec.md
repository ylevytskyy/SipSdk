# Spec: Windows Platform SDK (.NET / C# Bindings) (`windows-platform-sdk`)

## 1. Problem & Architecture Overview

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

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #18.

---

## 2. Public API Specification

### .NET / C# Public API (`platforms/windows/dotnet/src/SipSdk/`)

#### Enums (`SipEnums.cs`)
```csharp
namespace SipSdk
{
    public enum SipRegistrationState
    {
        Unregistered = 0,
        Registering = 1,
        Registered = 2,
        Failed = 3
    }

    public enum SipCallState
    {
        Incoming = 0,
        Ringing = 1,
        Connecting = 2,
        Established = 3,
        Hold = 4,
        Closed = 5
    }
}
```

#### Event Arguments (`SipEventArgs.cs`)
```csharp
namespace SipSdk
{
    public class RegistrationStateChangedEventArgs : EventArgs
    {
        public SipRegistrationState State { get; }
        public int StatusCode { get; }
        public string Reason { get; }

        public RegistrationStateChangedEventArgs(SipRegistrationState state, int statusCode, string reason)
        {
            State = state;
            StatusCode = statusCode;
            Reason = reason;
        }
    }

    public class IncomingCallEventArgs : EventArgs
    {
        public SipCall Call { get; }

        public IncomingCallEventArgs(SipCall call)
        {
            Call = call;
        }
    }

    public class CallStateChangedEventArgs : EventArgs
    {
        public SipCallState State { get; }
        public int StatusCode { get; }
        public string Reason { get; }

        public CallStateChangedEventArgs(SipCallState state, int statusCode, string reason)
        {
            State = state;
            StatusCode = statusCode;
            Reason = reason;
        }
    }

    public class CallMediaChangedEventArgs : EventArgs
    {
        public int Width { get; }
        public int Height { get; }
        public bool IsOnHold { get; }
        public bool IsMuted { get; }

        public CallMediaChangedEventArgs(int width, int height, bool isOnHold, bool isMuted)
        {
            Width = width;
            Height = height;
            IsOnHold = isOnHold;
            IsMuted = isMuted;
        }
    }

    public class DtmfReceivedEventArgs : EventArgs
    {
        public string Digit { get; }

        public DtmfReceivedEventArgs(string digit)
        {
            Digit = digit;
        }
    }
}
```

#### Client Interface (`SipClient.cs`)
```csharp
namespace SipSdk
{
    public sealed class SipClient : IDisposable
    {
        public bool IsRegistered { get; }
        public bool IsMicMuted { get; }
        public string AccountAor { get; }

        public event EventHandler<RegistrationStateChangedEventArgs> RegistrationStateChanged;
        public event EventHandler<IncomingCallEventArgs> IncomingCall;

        public SipClient();

        public int Start(string configPath = null);
        public void Stop();

        public int Register(string username, string domain, string password, string transport = "udp", int port = 5060);
        public int Unregister();

        public SipCall MakeCall(string targetUri, bool withVideo = false);

        public void SetMicMuted(bool muted);
        public void SwitchCamera();
        public void SetLocalVideoEnabled(bool enabled);
        public bool IsLocalVideoEnabled();

        public void Dispose();
    }
}
```

#### Call Session Interface (`SipCall.cs`)
```csharp
namespace SipSdk
{
    public sealed class SipCall : IDisposable
    {
        public string Id { get; }
        public string RemoteUri { get; }
        public string PeerDisplayName { get; }
        public SipCallState State { get; }
        public bool HasVideo { get; }
        public bool IsOutgoing { get; }
        public bool IsOnHold { get; }
        public bool IsMuted { get; }
        public bool IsClosed { get; }
        public uint Duration { get; }

        public event EventHandler<CallStateChangedEventArgs> StateChanged;
        public event EventHandler<CallMediaChangedEventArgs> MediaChanged;
        public event EventHandler<DtmfReceivedEventArgs> DtmfReceived;

        public int Answer(bool withVideo = false);
        public int Hangup(int statusCode = 0, string reason = null);
        public int SetHold(bool hold);
        public int SetMuted(bool muted);
        public int SendDtmf(string digits);
        public int RequestKeyframe();

        public void Dispose();
    }
}
```

---

## 3. Native Media Foundation Driver Specification

### Video Capture Driver (`sip::windows::MfVideoCaptureDriver`)
- **Webcam Enumeration**: Queries `MFEnumDeviceSources` with `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID`.
- **Stream Configuration**: Configures `IMFSourceReader` to stream uncompressed formats (`MFVideoFormat_YUY2` or `MFVideoFormat_NV12`).
- **Color Conversion**: Fast conversion of incoming samples to planar `VID_FMT_YUV420P` `sip_video_frame_t`.
- **Synthetic Fallback**: When no hardware video source is detected, seamlessly switches to synthetic 30 fps timestamped color bar patterns for headless testing and CI environments.

### Hardware H.264 Video Codec Driver (`sip::windows::MfVideoCodecDriver`)
- **Encoder MFT**: Uses `CLSID_CMSH264EncoderMFT` with RFC 6184 Constrained Baseline Profile `42e01f`.
- **Decoder MFT**: Uses `CLSID_CMSH264DecoderMFT` supporting byte-stream NAL units, SPS/PPS parameter set extraction, and RTP depacketization.
- **Keyframe Recovery**: Generates instant IDR keyframes upon RTCP PLI/FIR requests from the core engine.

### Video Rendering Driver (`sip::windows::MfVideoRendererDriver`)
- **Frame Sink**: Converts YUV420P decoded frames into BGRA32 pixel buffers.
- **WPF Integration**: Delivers frames to a thread-safe `WriteableBitmap` or direct swapchain backbuffer inside `SipVideoPanel`.

---

## 4. Acceptance Criteria Traceability

| Issue Criteria | Spec Component | Verification Gate |
|---|---|---|
| CMake MSVC toolchain compiles `libsipcore.dll` with exported C ABI symbols | Section 1, Slice 1 | MSVC build produces `libsipcore.dll` & `libsipcore.lib`; `nm`/`dumpbin` asserts all `_sip_*` exported and 0 unmanaged leaks |
| C# P/Invoke wrapper wraps all `sip_abi` functions with .NET event listeners | Section 2, Slice 2 | `SipSdk.Tests` verifies 100% `sip_abi` function coverage, `SipClient`/`SipCall` events, and UI thread marshaling |
| Windows Media Foundation driver encodes webcam input into H.264 and decodes remote video streams | Section 3, Slice 3 | Native driver tests verify MF capture, H.264 MFT encode/decode roundtrip, and synthetic fallback |
| Sample Windows desktop application registers to Asterisk PBX, places a call, and renders remote video | Section 1, Slice 4 | `SipSdk.WpfSample` compiles, registers to Asterisk PBX, places audio/video call, and renders video feed |
