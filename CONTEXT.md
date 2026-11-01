# CONTEXT.md — SipSdk Domain Glossary

Canonical definitions for domain terms across the `sipsdk` repository (`Sources/SipSdk`, `SipSdk/`, and `web/`).

- **AOR (Address of Record)**: The canonical SIP identity URI (`sip:username@domain:port`) registered by a `SipClient` user agent with a SIP registrar.
- **Baresip Core (`BaresipCore.xcframework`)**: The pinned static C library bundle combining `libre` v4.12.0 and `libbaresip` v4.12.0 for iOS device (`arm64`) and Simulator (`arm64`, `x86_64`).
- **Video Mode (`vidmode`)**: Baresip per-call media mode (`VIDMODE_ON` for bidirectional audio+video SDP negotiation, `VIDMODE_OFF` for audio-only).
- **Video Codec (`vidcodec`)**: A Baresip plugin registered in `baresip_vidcodecl()` that encodes outgoing `vidframe` buffers into RTP packets and decodes incoming RTP packets into `vidframe` buffers. In `SipSdk`, `H264` (`packetization-mode=1` and `packetization-mode=0`, profile-level-id `42e01f`) is implemented natively with Apple `VideoToolbox` and `re_h264`.
- **Video Source (`vidsrc`)**: A Baresip capture plugin registered in `baresip_vidsrcl()` (`avcapture`) that supplies `VID_FMT_YUV420P` frames from `AVCaptureSession` on physical devices (or a 30 fps synthetic test pattern on Simulator) to the active video stream and local preview.
- **Video Display (`vidisp`)**: A Baresip rendering plugin registered in `baresip_vidispl()` (`iosview`) that receives decoded remote `vidframe` buffers and renders them onto a `SipVideoView` in the iOS UI.
- **CallKit Bridge (`SipCallKitManager`)**: The iOS system telephony coordinator (`CXProvider` + `CXCallController`) that mirrors incoming/outgoing SIP calls, hold, mute, and DTMF actions between iOS system call UI and `SipClient` using each `SipCall`'s `uuid`.
