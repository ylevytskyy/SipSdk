# Architecture audit: SipSdk — Full Repository (`core/`, `platforms/`, `web/`) — 2026-11-10

- **Base:** `master` @ `3bfd078` (dirty: no)
- **Scope:** `core/`, `platforms/`, `web/` (106 non-test source files, 21,542 LOC); excluded: `**/Tests/**`, `**/tests/**`, `**/test/**`, `*Test*`, `build/`, `.build/`, `prebuilt/`, `.cxx/`, `.gradle/`, `node_modules/`, `*.min.js`
- **Stack profile:** `none` (multi-platform C++17/20, Swift 5.9/6, Objective-C++, Kotlin/JNI, C#/.NET, JavaScript; ran the four core dimensions)
- **Ran:** `wc -l` / `find` source census: 106 source files (21,542 LOC), 23 files >300 LOC, 62 functions/methods >50 LOC; `git log` 60-day hotspot analysis (`core/src/engine.cpp` 10 commits, `core/src/engine.h` 7, `web/index.html` 6, `core/src/sip_abi.cpp` 6, `core/include/sip_abi.h` 6); static structural counts across `core/`, `platforms/`, `web/` (65 includes and 17 platform `#ifdef`s in `core/`, 8 process-wide mutable singletons, 286 lock sites, 49 thread/queue spawns, 160 raw pointer/bridge casts)
- **Read only:** Full static inspection of `core/`, `platforms/ios/`, `platforms/android/`, `platforms/macos/`, `platforms/desktop/`, `platforms/windows/`, `platforms/web/`, and `web/`
- **Not run:** Physical iOS/Android/Windows/Linux device execution, live Asterisk PBX media calls, and AddressSanitizer/ThreadSanitizer runtime suites (read-only static architecture audit)
- **Verification:** 31 High/Critical raw findings sent to independent adversarial verifiers across 4 dimensions → 30 `confirmed`, 1 `doubtful` (lowered one severity level), 0 `refuted` (dropped); after cross-dimension deduplication, 33 unique findings remain (5 Critical, 14 High, 12 Medium, 2 Low)

## 1. Executive Summary

**Health index: 0/10** = `max(0, 10 − 2·5 Critical − 1·14 High − 0.25·12 Medium)` (`10 − 10 − 14 − 3.0 = −17.0 → 0/10` over surviving deduplicated findings).
**Failure risk: Critical** — from `SOLID-1`, `BOUND-1`, `BOUND-2`, `COMP-2`, `RES-2` (all core-path).
**Collapse risk (judged): High** — while the single-client iOS/macOS happy path against local Asterisk works, cross-platform drift, unretained/dangling FFI pointers, and copy-pasted media pipelines make concurrent lifecycle changes and non-Apple platform parity brittle.

What works:
- **Opaque C ABI contract (`core/include/sip_abi.h`, `core/src/sip_abi.cpp`):** Clean `extern "C"` boundary with `SIP_ABI_EXPORT` visibility, null-pointer guards on all 38 exported functions, and explicit symbol export filtering on Android (`platforms/android/sipsdk/src/main/cpp/sipcore.map`).
- **Single-threaded Baresip loop serialization (`core/src/engine.cpp:74-135`):** `Engine` serializes `ua_*` and `call_*` operations onto the `re_main` worker thread via a lock-protected task queue and `mqueue_push` wakeup, and joins the worker thread outside `m_mutex` on shutdown.
- **Consistent `USE_UNIXSOCK=ON` build hygiene:** Enforced across `Package.swift`, `core/CMakeLists.txt`, `platforms/desktop/CMakeLists.txt`, `platforms/windows/CMakeLists.txt`, and `platforms/android/sipsdk/src/main/cpp/CMakeLists.txt` to prevent the `libre` `struct sa` ABI size trap documented in `AGENTS.md`.
- **Clean sample UI layering on iOS (`platforms/ios/sample/SipPhone/Models/SipAppModel.swift`):** The iOS sample model is 155 lines with zero functions over 50 lines and consumes only the public `SipSdk` Swift module.

Why the risk is Critical:
- **`BOUND-1` (`platforms/ios/Sources/SipSdk/SipClient.swift:246-260`):** `SipClient.register(_:)` extracts `baseAddress` from temporary `[CChar]` arrays inside immediately-returning `withUnsafeBufferPointer` closures and passes dangling C-string pointers for `display_name`, `outbound_proxy`, `stun_server`, `stun_user`, `stun_pass`, and `media_nat` into `sip_client_register_account`.
- **`SOLID-1` (`platforms/android/sipsdk/src/main/cpp/sip_jni.cpp:54-61`, `SipClient.kt:282-289`):** Android's C ABI capture callbacks `android_capture_start` and `android_capture_stop` are no-op stubs; `SipCameraCapture` is never started when answering an incoming video call (`SipCall.answer(withVideo = true)`) and never stopped when a call ends (`CLOSED`).
- **`BOUND-2` (`platforms/desktop/src/client.cpp:348-357`):** Desktop `sip::Client::set_video_capture_driver` stores the driver in `m_capture_driver` and never forwards it to `sip_client_set_video_capture_driver`, rendering `AVFoundationVideoCaptureDriver` and `V4L2VideoCaptureDriver` dead code when configured via `sip::Client`.
- **`COMP-2` (`platforms/windows/src/media/mf_capture.cpp:88-117`, `mf_codec.cpp:21-38`):** Windows Media Foundation capture and H.264 codec drivers are non-functional stubs (releasing `IMFActivate` / `CLSID_CMSH264EncoderMFT` immediately without capturing or registering a codec), and `MfVideoCaptureDriver::start()` detaches a worker thread capturing raw `this` without joining in `stop()`, causing a deterministic use-after-free on destruction.
- **`RES-2` (`core/src/engine.cpp:711-843, 1042-1050`, `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt:339-405`):** `Engine` retains `sip_call_t` before `post_task` without releasing on failure, keeps closed calls in `m_all_calls` until `sip_call_destroy`, and Android/macOS/Desktop never call `sip_call_destroy`—while Android creates a fresh `SipCall` wrapper on every callback and leaves `SipCall.nativeHandle` dangling after `SipClient.stop()`.
- **`SOLID-3` (`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt:253-266`):** Android, Desktop, and Windows bind only the 5-argument `sip_client_register` and silently drop `displayName`, `outboundProxy`, `stunServer`, `mediaNat`, and `regInterval`.
- **`BOUND-3` (`core/src/media_driver.cpp:340, 1040-1068, 1387-1409`):** Process-wide mutable statics (`s_active_manager`, static `struct vidcodec` intrusive list nodes `gH264CodecMode1`/`0`, and global `libre_init`/`baresip_close`) corrupt state if two `SipClient` instances overlap.
- **`BOUND-5` (`core/src/engine.cpp:951-973`, `core/src/media_driver.cpp:342-1070`):** Apple `VideoToolbox`, Objective-C `AVAudioSession` (`objc_msgSend`), `libdispatch`, and hardcoded `8.8.8.8`/`1.1.1.1` DNS servers leak directly into portable `core/`.
- **`COMP-5` (`core/src/media_driver.cpp:886-965`, `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp:483-653`):** ~650 lines of RFC 6184 H.264 RTP packetization/depacketization are copy-pasted between `core/` and Android and have already diverged in SPS resolution parsing.
- **`COMP-6` (`web/index.html:373-460`):** `web/index.html` duplicates `platforms/web/src/sipsdk.js` and misses 3 fixes (`offerHasVideo` check on answer, `@domain` completion on bare extensions, and `{ transportType: 'INFO' }` on DTMF).
- **`RES-4` (`platforms/ios/Sources/SipSdk/SipClient.swift:120-169, 314, 379`):** Unretained `Unmanaged.passUnretained` pointers for `SipClient` and `SipCall` are dereferenced asynchronously inside `DispatchQueue.main.async` blocks without lifetime guards.
- **`RES-5` (`core/src/call_session.cpp:45-61`):** `sip_call_t::get_id()`, `get_remote_uri()`, and `get_peer_display_name()` return raw `.c_str()` pointers after unlocking `m_mutex` while `attach_raw_call` mutates those `std::string` members on the worker thread.
- **`RES-6` (`platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp:714-730`):** `set_remote_window` releases `g_remote_window` on the UI thread while `AMediaCodec` on the Baresip worker thread uses the unretained `ANativeWindow*`.
- **`RES-7` (`core/src/media_driver.cpp:225-235`):** `CApiVideoCaptureDriver::set_frame_handler` mutates `m_cpp_handler` without a lock while camera capture threads invoke `c_frame_callback`.
- **`RES-8` (`platforms/ios/Sources/SipSdk/Media/IosAudioManager.swift:33-63`):** `prepareCallAudioSession()` increments `activeCallCount` 2–3 times per call without matching decrements on call close or activation failure, wedging the audio session active.
- **`RES-9` (`platforms/ios/Sources/SipSdk/SipClient.swift:387-427`):** `handleIncomingCall` reports incoming calls to CallKit, but `handleCallState(.closed)` never tells CallKit the call ended, leaving the iOS system call UI stuck active.
- **`RES-10` (`core/src/engine.cpp:344-360, 443-447`):** `Engine::is_registered()` reads `m_ua` on the caller thread while the worker thread destroys and reallocates `m_ua` without holding `m_mutex`.

## 2. Findings by dimension

### 2.1 SOLID & Module Design (`solid`)

| Sev | File / entity | Principle | Risk | Evidence | Verdict |
|---|---|---|---|---|---|
| Critical | `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp:54-61` `android_capture_start` / `android_capture_stop` (`SOLID-1`, `COMP-3a`, core-path) | LSP | `sip_video_capture_driver_t` on Android implements `start` and `stop` as no-ops (`return 0;`). `SipClient.kt:282-289` starts `SipCameraCapture` only in `makeCall(withVideo = true)` and `startLocalPreview()`. Answering an incoming video call (`SipCall.answer(withVideo = true)`) never starts the camera, and ending a call (`CLOSED`) never stops the camera until `SipClient.stop()`. | CODE | confirmed |
| High | `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt:253-266` `SipClient.register` (`SOLID-3`, `BOUND-12`, core-path) | ISP, LSP | `sip_abi.h:78-97` defines `sip_client_register_account` (`display_name`, `outbound_proxy`, `stun_server`, `media_nat`, `reg_interval`), but Android (`SipClient.kt:253-266`), Desktop (`client.cpp:255-265`), and Windows (`SipClient.cs:81-89`) only bind 5-arg `sip_client_register`. Android's `SettingsScreen.kt:118-135` collects STUN, ICE, outbound proxy, display name, and reg interval, and `SipClient.register()` silently drops them all. | CODE | confirmed |
| High | `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.mm:682-699` `set_default_audio_device_by_name` (`SOLID-8`, `COMP-8`, `BOUND-10`, `COMP-12`) | SRP, DIP | `SipMacClient.mm` (1,104 LOC) embeds camera capture, C ABI glue, config generation, and CoreAudio routing inside the sample app. Worse, `selectAudioInputDeviceByName:` (`979-987`) and `selectAudioOutputDeviceByName:` (`1013-1021`) call `AudioObjectSetPropertyData` with `kAudioHardwarePropertyDefaultInputDevice` / `OutputDevice` on `kAudioObjectSystemObject`, changing the OS-wide default audio devices for all macOS applications, while `selectVideoOutputDeviceByName:` (`1080-1083`) is a no-op stub. | CODE | confirmed |
| Medium | `core/src/engine.cpp:1052-1248` `Engine::dispatch_*` (`SOLID-10`, `COMP-11b`, core-path) | OCP | `Engine` defines 5 separate heap-allocated task structs and static runner functions (`RegStateTaskArgs`, `CallStateTaskArgs`, `IncomingCallTaskArgs`, `CallMediaTaskArgs`, `DtmfDigitTaskArgs`), repeating identical `dispatch_to_main` / `dispatch_async_f` branching across ~200 lines. Adding 1 new engine callback event requires touching 10 files across C ABI, C++ core, and 5 platform wrappers. | CODE | N/A (Medium) |
| Medium | `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.mm:1044-1058` `selectCameraByName:` & `SipClient.kt:139` `prepareDefaultConfigDirectory` (`SOLID-11`) | OCP, DIP | `selectCameraByName:` switches to synthetic mode via substring match `[cameraName containsString:@"Synthetic"]`, and Android's `prepareDefaultConfigDirectory` checks `!configFile.readText().contains("opensles")`, overwriting custom Baresip config files. Default Baresip config generation is duplicated across 4 platform files (`SipClient.swift:465`, `SipClient.kt:127`, `client.cpp:15`, `SipMacClient.mm:481`). | CODE | N/A (Medium) |
| Low | `platforms/android/sample/src/main/kotlin/com/sipsdk/sample/ui/screens/SettingsScreen.kt:118-135` `SettingsScreen` / `SipAppViewModel` (`SOLID-12`) | SRP, DIP | `SettingsScreen.kt` (671 LOC) and `HomeScreen.kt` (538 LOC) reach directly into `viewModel.accountStore.account` and instantiate/bind SDK `SipSurfaceView` inside Compose UI. `SipAppViewModel.kt:372` updates call duration on hangup via `list.indexOfFirst { it.remoteUri == call.remoteUri }`, mutating the wrong history entry on repeat calls to the same URI. | CODE | N/A (Low) |

**Measurements (`solid`):** Inspected 18 core and platform files (9,645 LOC). `core/src/media_driver.cpp` (1,606 LOC) mixes 5 distinct responsibilities; `core/src/engine.cpp` (1,440 LOC) has a 17-branch switch in `handle_ua_event` and 5 duplicated callback task structs; `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.mm` (1,104 LOC) combines 2 Objective-C classes, AVFoundation capture, CoreAudio HAL mutations, and config writing; `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp` (845 LOC) exports 34 JNI entrypoints. Adding 1 new engine callback requires editing 10 files across layers.

### 2.2 Boundaries & Coupling (`boundaries`)

| Sev | File / entity | Principle | Risk | Evidence | Verdict |
|---|---|---|---|---|---|
| Critical | `platforms/ios/Sources/SipSdk/SipClient.swift:246-260` `SipClient.register(_:)` (`BOUND-1`, core-path) | Public API / FFI Boundary | Optional account fields (`displayName`, `outboundProxy`, `stunServer`, `stunUser`, `stunPassword`, `mediaNat`) are converted to temporary `[CChar]` arrays via `.cString(using: .utf8)`, and `$0.baseAddress` is assigned to `cConfig` inside immediately-returning one-line `withUnsafeBufferPointer` closures (lines 253-258) before `sip_client_register_account(client, &cConfig)` is called on line 260. Because `$0.baseAddress` is valid only inside the closure body, `core/src/sip_abi.cpp:45-50` dereferences dangling pointers across the C ABI boundary. | CODE | confirmed |
| Critical | `platforms/desktop/src/client.cpp:348-357` `sip::Client::set_video_capture_driver` (`BOUND-2`, `COMP-1`, `SOLID-4`, core-path) | Layering | `platforms/desktop/include/sip/client.hpp` exposes `sip_core::IVideoCaptureDriver` from `core/include/media_driver.h`, while `client.cpp` talks to `core/` exclusively via opaque `sip_client_t*` and `sip_abi.h`. `set_video_capture_driver` stores the driver in `m_capture_driver`, which is never read anywhere or bridged to `sip_client_set_video_capture_driver`. Hardware capture drivers (`AVFoundationVideoCaptureDriver`, `V4L2VideoCaptureDriver`) configured in `sip_cli` (`platforms/desktop/src/cli/main.cpp:194-198`) are completely ignored at runtime. | CODE | confirmed |
| High | `core/src/media_driver.cpp:1387-1409` `MediaDriverManager::register_baresip_plugins` (`BOUND-3`, `SOLID-2`, core-path) | Global State | Although `sip_client_t` and platform `SipClient` classes are instance-scoped, `MediaDriverManager::register_baresip_plugins` writes `s_active_manager = this` (`line 1394`) and registers file-static `struct vidcodec` structs (`gH264CodecMode1`, `gH264CodecMode0` at lines 1040-1068, and `gH264MediaCodecMode1`/`0` in `media_codec_driver.cpp:655-683`) with embedded intrusive list nodes (`struct le`). Simultaneously, `Engine` calls process-global `libre_init()`, `baresip_init()`, `baresip_close()`, and `libre_close()` per instance. Overlapping two `SipClient` instances corrupts codec lists and tears down Baresip under the surviving client. | CODE | confirmed |
| High | `core/src/engine.cpp:951-973` `Engine::prepare_audio_session` & `core/src/media_driver.cpp:342-1070` (`BOUND-5`, `SOLID-9`, core-path) | Layering / DIP | Violating ADR-0001 and Repo Rule 1, `core/src/media_driver.cpp` embeds 730+ lines of Apple `VideoToolbox`/`CoreMedia`/`CoreVideo` H.264 implementation (`#if defined(__APPLE__)`), and `core/src/engine.cpp` includes `<objc/message.h>`, `<objc/runtime.h>`, and `<dispatch/dispatch.h>` to invoke `AVAudioSession` via `objc_msgSend` and `dispatch_get_main_queue()`, while hardcoding public DNS servers `8.8.8.8` and `1.1.1.1` in `init_on_loop_thread` (`engine.cpp:216-220`). | CODE | confirmed |
| Medium | `core/src/engine.cpp:449-472, 623-626, 1341-1344` `sip_core::sdp_media_internal` (`BOUND-6`, `SOLID-6`, `COMP-4`, `RES-3`, core-path) | Coupling / Encapsulation | `Engine` redeclares Baresip's private internal `struct sdp_media` as `struct sdp_media_internal` (including four `struct sa` fields whose size depends on `HAVE_UNIXSOCK`) and `reinterpret_cast`s opaque `struct sdp_media *` pointers to overwrite `im->rdir = SDP_SENDRECV`. *(Verifier marked `RES-3` doubtful at Critical severity and lowered one level because build scripts pin `libre` v4.12.0 with `-DUSE_UNIXSOCK=ON` and lines 624/1342 guard the write with an `ldir`/`rdir` check.)* | CODE | doubtful (lowered) |
| Medium | `platforms/windows/CMakeLists.txt:57-63` `sip_core_dll` & `SipNativeAbi.cs` (`BOUND-7`) | Public API | `platforms/windows/CMakeLists.txt` compiles `mf_capture.cpp`, `mf_codec.cpp`, and `mf_renderer.cpp` only into `test_mf_drivers` (`lines 176-181`) and excludes them from `sip_core_dll`. `SipNativeAbi.cs` also omits P/Invoke declarations for all media driver setters, `sip_client_register_account`, and `sip_client_reset_transports`. | CODE | N/A (Medium) |
| Medium | `platforms/android/sipsdk/src/main/cpp/CMakeLists.txt:39-46` `sipcore` (`BOUND-9`) | Layering | `core/` has two headers named `media_driver.h` (`core/include/media_driver.h` and private `core/src/media_driver.h`). Both Android (`CMakeLists.txt:41`) and Desktop (`CMakeLists.txt:83`) add `${CORE_DIR}/src` to their include paths, enabling `media_codec_driver.cpp:2, 466, 622, 637` to bypass `sip_abi.h` and call `sip_core::MediaDriverManager::active_manager()` directly. | CODE | N/A (Medium) |

**Measurements (`boundaries`):** `core/` has 9 files (4,397 LOC), 65 `#include` directives, and 17 platform `#ifdef` blocks (12 in `engine.cpp`, 4 in `media_driver.cpp`, 1 in `sip_abi.h`). Native code contains 7 process-wide mutable statics (`s_active_manager`, `gH264CodecMode1`, `gH264CodecMode0` in `core/src/media_driver.cpp`; `g_jvm` in `sip_jni.cpp`; `g_remote_window`, `gH264MediaCodecMode1`, `gH264MediaCodecMode0` in `media_codec_driver.cpp`) plus `IosAudioManager.shared` in Swift. Feature scatter: Video capture/rendering spans 16 files; `resetTransports` spans 8 files (missing in Desktop, Windows, Web); `sip_account_config_t` spans 5 files (missing in Android, Desktop, Windows, Web).

### 2.3 Cognitive Load & Hygiene (`complexity`)

| Sev | File / entity | Principle | Risk | Evidence | Verdict |
|---|---|---|---|---|---|
| Critical | `platforms/windows/src/media/mf_capture.cpp:88-117` `MfVideoCaptureDriver::start` & `mf_codec.cpp:21-38` (`COMP-2`, `RES-1`, `BOUND-8`, core-path) | POLA, Concurrency | `MfVideoCaptureDriver::start` enumerates `MFEnumDeviceSources`, immediately releases every `IMFActivate` handle without opening a source reader, and spawns `std::thread([this]() { capture_thread_worker(); }).detach()` which writes synthetic color bars into `this->m_y_plane`/`m_u_plane`/`m_v_plane`. `stop()` and `~MfVideoCaptureDriver()` only set `m_running = false` without joining, causing a deterministic use-after-free on destruction. `MfVideoCodecDriver::register_codec` similarly creates `CLSID_CMSH264EncoderMFT` and immediately releases it without registering a Baresip `vidcodec`. | CODE | confirmed |
| High | `core/src/media_driver.cpp:886-965` & `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp:483-653` `h264_dec_decode` / `sendRfc6184Nal` (`COMP-5`, `SOLID-7`, core-path) | DRY | ~650 lines of RFC 6184 H.264 NAL packetization (`sendRfc6184Nal`), RTP depacketization, STAP-A unpacking, FU-A reassembly, and 500ms RTCP PLI throttling (`EPROTO`) are duplicated almost verbatim between `core/src/media_driver.cpp` and Android's `media_codec_driver.cpp`. The copies have already diverged: Android decodes SPS via `h264_sps_decode` to notify stream resize (`media_codec_driver.cpp:455-471`) while `core/src/media_driver.cpp:805-808` does not. | CODE | confirmed |
| High | `web/index.html:373-460` `answerIncoming` / `call` / `sanitizeSdp` (`COMP-6`, core-path) | DRY | `web/index.html` (lines 217-460) re-implements `platforms/web/src/sipsdk.js` inline instead of importing it, leaving 3 copy-paste divergence bugs: (1) `answerIncoming` (`390-403`) omits `offerHasVideo(sdp)` (`sipsdk.js:58, 161`), requesting local video on audio-only incoming calls when `#enableVideo` is checked; (2) `call(target)` (`375`) prefixes `sip:` without appending `@${domain}` for bare extensions like `9001` (`sipsdk.js:53-56`); (3) DTMF (`459`) calls `session.sendDTMF(d)` without `{ transportType: 'INFO' }` (`sipsdk.js:207`). | CODE | confirmed |
| High | `platforms/desktop/src/avfoundation_capture.mm:72-87` `-[SipDesktopAVFCaptureDelegate captureOutput:...]` (`COMP-7`) | DRY, Memory Safety | One of 3 duplicate `AVCaptureSession` implementations (alongside `IosVideoCapture.swift` and `SipMacClient.mm`). Its non-biplanar fallback branch (`line 74`) calls `CVPixelBufferGetBaseAddress(imageBuffer)` instead of `CVPixelBufferGetBaseAddressOfPlane` (returning NULL or a plane descriptor table on planar `CVPixelBuffer`s) and computes U/V pointers by arithmetic offset. Also, `self.frameHandler` (`line 85`) is read on the capture queue without locking `m_impl->mutex` (`line 201`). | CODE | confirmed |
| Medium | `platforms/ios/Sources/SipSdk/Media/IosVideoCapture.swift:383-444` `deliverSyntheticFrame` (`COMP-9`, core-path) | DRY | Synthetic BT.601 YUV420P 8-color-bar generation is implemented 6 times (`core/src/media_driver.cpp:28-78`, `SipMacClient.mm:164-211`, `IosVideoCapture.swift:383-444`, `SipVideoView.swift:134-183`, `SipCameraCapture.kt:299-374`, `mf_capture.cpp:16-56`). `IosVideoCapture.swift:386-404` allocates and deallocates 3 heap buffers (460 KB at 640x480) on every frame at 30 fps and passes raw `mach_absolute_time()` ticks instead of nanoseconds. | CODE | N/A (Medium) |
| Medium | `platforms/ios/Sources/SipSdk/SipVideoView.swift:55-91` `SipVideoUIView.renderYUV420` (`COMP-10`, `RES-15`, core-path) | DRY, POLA | After `vtDecompressionOutputCallback` (`core/src/media_driver.cpp:707-748`) locks the hardware-decoded NV12 `CVImageBufferRef` and de-interleaves UV on the CPU into planar YUV420P buffers, `SipVideoUIView` (`SipVideoView.swift:55-91`), `SipMacVideoView` (`SipMacVideoView.mm:46-87`), and `MfVideoRendererDriver` (`mf_renderer.cpp:14-51`) each allocate a 1.22 MB RGBA buffer per frame and run scalar CPU pixel-by-pixel YUV-to-RGB loops on the calling thread (`re_main` for remote video). | CODE | N/A (Medium) |
| Medium | `core/src/engine.cpp:1250-1296, 1404-1408` `Engine::set_local_renderer` / `start_local_preview` (`COMP-11a`, core-path) | CQS, POLA | `Engine::set_capture_driver`, `set_remote_renderer`, and `set_local_renderer` post asynchronously to the Baresip thread when `m_started` is true, whereas `Engine::start_local_preview` (`1404-1408`) invokes `m_media_manager->start_local_preview()` synchronously on the caller's thread, racing with a preceding setter call. | CODE | N/A (Medium) |
| Low | `platforms/macos/sample/SipSdkMac/Models/SipAppModel.swift:246-252` `performNetworkHandover` (`COMP-13`) | POLA, YAGNI | In macOS (`SipAppModel.swift:246-252`), iOS (`SettingsView.swift:311-317`), and Android (`SipAppViewModel.kt:282-290`), `performNetworkHandover` sets `isHandoverInProgress = true` and `false` synchronously in the same block, making the UI progress spinner unreachable. Also, `platforms/macos/sample/SipSdkMac/Views/AccountSettingsView.swift` (13 LOC) is unused dead code. | CODE | N/A (Low) |

**Measurements (`complexity`):** Scanned 106 in-scope source files (21,542 LOC). **23 files exceed 300 lines:** `core/src/media_driver.cpp` (1,606), `core/src/engine.cpp` (1,440), `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.mm` (1,104), `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp` (845), `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp` (741), `SettingsScreen.kt` (671), `HomeScreen.kt` (538), `SipClient.swift` (494), `web/index.html` (484), `platforms/web/src/sipsdk.js` (455), `IosVideoCapture.swift` (445), `SipClient.kt` (445), macOS `InCallView.swift` (436), macOS `SettingsView.swift` (413), `InCallScreen.kt` (408), macOS `SipAppModel.swift` (406), `SipAppViewModel.kt` (399), `SipCameraCapture.kt` (397), `platforms/desktop/src/client.cpp` (378), `core/include/sip_abi.h` (357), `core/src/sip_abi.cpp` (338), `platforms/desktop/src/v4l2_capture.cpp` (323), iOS `SettingsView.swift` (318). **62 functions/methods exceed 50 lines** (largest: `SettingsScreen` 520 LOC, macOS `InCallView.body` 398 LOC, `HomeScreen` 379 LOC, `Engine::handle_ua_event` 178 LOC, `android_h264_dec_decode` 171 LOC, `h264_dec_decode` 153 LOC, `Engine::init_on_loop_thread` 140 LOC). Top nesting depth: 11 levels in UI (`InCallView.swift:72`), 8 levels in C++ (`media_codec_driver.cpp:582`).

### 2.4 Fault Tolerance & Data Integrity (`resilience`)

| Sev | File / entity | Principle | Risk | Evidence | Verdict |
|---|---|---|---|---|---|
| Critical | `core/src/engine.cpp:711-843, 1042-1050` & `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt:339-405` `Engine::make_call` / `SipClient.onNativeCallState` (`RES-2`, `SOLID-5`, `BOUND-11`, `COMP-3b`, core-path) | Memory Safety, Fail-Fast | `make_call`, `answer_call`, `hangup_call`, `hold_call`, `mute_call`, and `send_dtmf` call `call->retain()` before `post_task(...)` without releasing if `post_task` returns `false`. Furthermore, `remove_call_session` (`1042-1050`) only detaches `m_raw_call` without erasing from `m_all_calls`, and Android (`SipClient.kt`), macOS (`SipMacClient.mm`), and Desktop (`client.cpp`) never call `sip_call_destroy`, leaking every `sip_call_t` until `Engine` destruction—while Android allocates a fresh `SipCall` wrapper on every callback and leaves `SipCall.nativeHandle` dangling after `SipClient.stop()`. | CODE | confirmed |
| High | `platforms/ios/Sources/SipSdk/SipClient.swift:120-169, 314, 379, 458` `SipClient.start` (`RES-4`, core-path) | Memory Safety, Concurrency | `SipClient` passes `Unmanaged.passUnretained(self).toOpaque()` to `sip_client_create` and `Unmanaged.passUnretained(sipCall).toOpaque()` to `sip_call_set_user_data`, while `dispatch_to_main` queues callbacks asynchronously via `DispatchQueue.main.async`. If `SipClient` or `SipCall` is deallocated while a callback block is already queued on `DispatchQueue.main`, `takeUnretainedValue()` dereferences freed memory. | CODE | confirmed |
| High | `core/src/call_session.cpp:45-61` `sip_call_t::get_id` / `get_remote_uri` / `get_peer_display_name` (`RES-5`, core-path) | Concurrency, Memory Safety | `get_id()`, `get_remote_uri()`, and `get_peer_display_name()` lock `m_mutex` only while evaluating `.c_str()`, unlocking before the caller copies the string buffer. During outgoing call creation (`Engine::make_call`), the caller thread immediately invokes `sip_call_get_id` / `sip_call_get_remote_uri` while the Baresip worker thread concurrently runs `attach_raw_call` (`call_session.cpp:147-165`), mutating `m_id`, `m_peer_display_name`, and `m_remote_uri`. | CODE | confirmed |
| High | `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp:393, 714-730` `AndroidMediaCodecDriver::set_remote_window` (`RES-6`, `BOUND-4`, core-path) | Concurrency, Memory Safety | `set_remote_window` holds the sole `ANativeWindow_acquire` reference in static `g_remote_window` and releases the old window immediately under `g_window_mutex` on the UI thread when the surface is destroyed (`SipClient.kt:85-89`). Meanwhile on the Baresip worker thread, `get_remote_window()` returns the raw `ANativeWindow*` without acquiring a reference for `ensureDecoderSession` (`line 393`) and `android_h264_dec_decode` (`lines 600-648`), releasing the window out from under the active `AMediaCodec`. | CODE | confirmed |
| High | `core/src/media_driver.cpp:225-235` `CApiVideoCaptureDriver::set_frame_handler` (`RES-7`, core-path) | Concurrency, Memory Safety | `CApiVideoCaptureDriver::set_frame_handler` mutates `m_cpp_handler = std::move(handler)` without any mutex before updating the C callback pointer, racing directly with camera capture threads executing `c_frame_callback` (`self->m_cpp_handler(vf)` at `lines 263-279`). In addition, `SipMacCameraDriver` (`SipMacClient.mm:392-399`) and `IosVideoCaptureDriver` (`IosVideoCapture.swift:138-145`) do not drain in-flight capture callbacks before `core_vidsrc_destructor` frees `struct vidsrc_st`. | CODE | confirmed |
| High | `platforms/ios/Sources/SipSdk/Media/IosAudioManager.swift:33-63, 86-91` `prepareCallAudioSession` (`RES-8`, core-path) | SSOT, Fail-Fast | `prepareCallAudioSession()` unconditionally increments `activeCallCount` (`line 37`) before checking `if isSessionActive` (`line 38`) and does not decrement on `catch` (`lines 59-62`). Because `prepareCallAudioSession()` is called 2–3 times per call (`SipClient.swift:299, 382`, `engine.cpp:709, 744`, `SipCallKitCoordinator.swift:176`) while `releaseCallAudioSessionIfIdle()` is called fewer times on close, `activeCallCount` stays `> 0` and the VoIP `AVAudioSession` never deactivates. Also, `setSpeakerphone` mutates `isSpeakerEnabled` under `lock`, firing `didSet` under the lock and calling `applySpeakerphone` twice. | CODE | confirmed |
| High | `platforms/ios/Sources/SipSdk/SipClient.swift:387-427` `SipClient.handleCallState` (`RES-9`, core-path) | SSOT | `handleIncomingCall` automatically reports every incoming call to CallKit via `callKit.reportIncomingCall(sipCall)` (`line 390`), but `handleCallState` on `SIP_CALL_STATE_CLOSED` (`lines 421-426`) removes the call without calling `callKit.endCall(sipCall)` or `provider.reportCall(with:endedAt:reason:)`, leaving the iOS system CallKit call UI stuck active after remote or in-app hangup. | CODE | confirmed |
| High | `core/src/engine.cpp:443-447, 872-885` `Engine::is_registered` / `set_audio_driver` (`RES-10`, core-path) | Concurrency | `Engine::is_registered()` locks `m_mutex` and calls `ua_isregistered(m_ua)` on the caller thread, while `m_ua` is destroyed (`ua_destroy(m_ua)`) and reallocated (`ua_alloc(&m_ua, ...)`) on the Baresip worker thread in `register_account` (`lines 344-360`) and `cleanup_on_loop_thread` (`lines 316-319`) without locking `m_mutex`. Similarly, `set_audio_driver` (`872-885`) writes to Baresip's global `conf_config()->audio` char buffers from the caller thread without synchronization. | CODE | confirmed |
| Medium | `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp:600-613, 650` `android_h264_dec_decode` (`RES-11`, core-path) | Bulkheads, Fail-Fast | Both VideoToolbox (`VTDecompressionSessionWaitForAsynchronousFrames` at `media_driver.cpp:1018`) and Android MediaCodec (`AMediaCodec_dequeueInputBuffer` with 2000us/5000us timeouts at `media_codec_driver.cpp:296, 601`) block the Baresip `re_main` thread. In `android_h264_dec_decode`, if `AMediaCodec_dequeueInputBuffer` returns `< 0`, `vds->au_buffer` is cleared at `line 650` without setting `vds->need_idr = true`, silently dropping IDR frames. | CODE | N/A (Medium) |
| Medium | `core/src/media_driver.cpp:929-956` & `media_codec_driver.cpp:526-553` `h264_dec_decode` (`RES-12`, core-path) | Bulkheads, Memory Safety | `vds->fu_buffer` and `vds->au_buffer` append RTP payload fragments without any upper size bound as long as `pkt->hdr->ts` remains constant and the RTP marker bit is withheld, permitting unbounded heap growth on malformed RTP streams. | CODE | N/A (Medium) |
| Medium | `core/src/engine.cpp:792, 846-863` `Engine::set_client_mic_muted` / `hold_call` (`RES-13`, core-path) | SSOT | `Engine::set_client_mic_muted` applies `m_mic_muted` only to existing calls in `m_all_calls` and never applies it to subsequently created or established calls (`BEVENT_CALL_ESTABLISHED`). `Engine::hold_call` (`line 792`) ignores `call_hold(c, hold)` return values and unconditionally dispatches `SIP_CALL_STATE_HOLD`. | CODE | N/A (Medium) |
| Medium | `core/src/engine.cpp:235-239` `init_on_loop_thread` & `sip_jni.cpp:136, 680-684` (`RES-14`, core-path) | Fail-Fast, Memory Safety | `init_on_loop_thread` ignores non-zero return codes from `bevent_register(ua_event_handler, this)`, succeeding with `m_events_registered = false` so no SIP events fire. On Android (`sip_jni.cpp`), `jni_dispatch_to_main` (`line 136`) ignores `Handler.post` returning `false` (leaking `TaskArgs` and `sip_call_t` refcounts), `nativeGetAudioDriver` (`680-684`) leaks two `NewStringUTF` local refs, and JNI callbacks omit `env->ExceptionCheck()`. | CODE | N/A (Medium) |

**Measurements (`resilience`):** Scanned 112 files across `core/`, `platforms/`, and `web/`: 286 lock/mutex sites (`std::mutex`, `std::recursive_mutex`, `NSLock`, `@Synchronized`, C# `lock`), 49 thread/queue spawns, 160 raw pointer/bridge casts (`reinterpret_cast`, `static_cast<T*>`, `const_cast`, `Unmanaged`, `__bridge`, `GCHandle`), 24 explicit `(void)` ignored C return codes, 32 discarded `_ =` calls in Swift, and 9 empty `catch` blocks.

## 3. Done well

- **Opaque C ABI boundary with defensive null guards (`core/include/sip_abi.h`, `core/src/sip_abi.cpp`):** Every C ABI function validates its input handles and returns typed `SIP_OK` / `SIP_ERR_INVALID_ARG` codes without leaking Baresip headers to platform consumers.
- **Worker-thread command serialization and clean join on shutdown (`core/src/engine.cpp:56-135`):** `Engine::init` and `Engine::destroy` coordinate startup via `m_start_cv`, guard shutdown with `m_stopping.exchange(true)`, wake `re_main` via `mqueue_push`, and join `m_loop_thread` outside `m_mutex` to prevent join-under-lock deadlocks.
- **RTP sequence continuity tracking and throttled RTCP PLI recovery (`core/src/media_driver.cpp:893-1025`):** The H.264 depacketizer detects sequence gaps (`last_seq + 1`), discards corrupt FU-A state before touching the hardware decoder, and throttles `EPROTO` RTCP Picture Loss Indication requests to once per 500 ms.
- **Consistent `USE_UNIXSOCK=ON` enforcement across build systems:** `Package.swift:27`, `core/CMakeLists.txt:15`, `platforms/desktop/CMakeLists.txt`, `platforms/windows/CMakeLists.txt`, and `platforms/android/sipsdk/src/main/cpp/CMakeLists.txt` all define `USE_UNIXSOCK=ON` / `HAVE_UNIXSOCK=1`, avoiding the `struct sa` ABI layout corruption documented in `AGENTS.md`.
- **Clean iOS SwiftPM SDK surface and sample separation (`platforms/ios/Sources/SipSdk/SipClient.swift`, `platforms/ios/sample/SipPhone/Models/SipAppModel.swift`):** `@Observable @MainActor SipClient` and `SipCall` keep the iOS sample model down to 155 lines with zero functions over 50 lines.
- **Fault-isolated event emission in Web SDK (`platforms/web/src/sipsdk.js:43-50`):** `Emitter.emit` wraps listener callbacks in `try/catch` blocks so an exception in UI code cannot break JsSIP session state transitions.

## 4. Refactoring plan

Sketches, not written or verified.

### Example 1. Fix dangling C-string pointers in iOS account registration (`platforms/ios/Sources/SipSdk/SipClient.swift:246-260`, `BOUND-1`)

**Before:**
```swift
let dname = account.displayName?.cString(using: .utf8)
let ob = account.outboundProxy?.cString(using: .utf8)
let stun = account.stunServer?.cString(using: .utf8)
let stunUser = account.stunUser?.cString(using: .utf8)
let stunPass = account.stunPassword?.cString(using: .utf8)
let nat = account.mediaNat?.cString(using: .utf8)

dname?.withUnsafeBufferPointer { cConfig.display_name = $0.baseAddress }
ob?.withUnsafeBufferPointer { cConfig.outbound_proxy = $0.baseAddress }
stun?.withUnsafeBufferPointer { cConfig.stun_server = $0.baseAddress }
stunUser?.withUnsafeBufferPointer { cConfig.stun_user = $0.baseAddress }
stunPass?.withUnsafeBufferPointer { cConfig.stun_pass = $0.baseAddress }
nat?.withUnsafeBufferPointer { cConfig.media_nat = $0.baseAddress }

_ = sip_client_register_account(client, &cConfig)
```

**After (unwritten and unverified sketch):**
```swift
func withOptionalCString<R>(_ string: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
    guard let string else { return body(nil) }
    return string.withCString { body($0) }
}

withOptionalCString(account.displayName) { dnamePtr in
    withOptionalCString(account.outboundProxy) { obPtr in
        withOptionalCString(account.stunServer) { stunPtr in
            withOptionalCString(account.stunUser) { stunUserPtr in
                withOptionalCString(account.stunPassword) { stunPassPtr in
                    withOptionalCString(account.mediaNat) { natPtr in
                        cConfig.display_name = dnamePtr
                        cConfig.outbound_proxy = obPtr
                        cConfig.stun_server = stunPtr
                        cConfig.stun_user = stunUserPtr
                        cConfig.stun_pass = stunPassPtr
                        cConfig.media_nat = natPtr
                        _ = sip_client_register_account(client, &cConfig)
                    }
                }
            }
        }
    }
}
```

**What it gives:** Guarantees all optional C-string pointers remain alive on the stack for the entire duration of `sip_client_register_account(client, &cConfig)`, eliminating the FFI use-after-free across the C ABI boundary.

### Example 2. Wire Android C ABI capture `start`/`stop` callbacks and track canonical `SipCall` lifecycle (`platforms/android/sipsdk/src/main/cpp/sip_jni.cpp:54-61`, `SipClient.kt:339-353`, `SOLID-1` & `RES-2`)

**Before:**
```cpp
static int android_capture_start(void *user_data, int width, int height, int fps) {
    (void)user_data; (void)width; (void)height; (void)fps;
    return 0;
}

static void android_capture_stop(void *user_data) {
    (void)user_data;
}
```

**After (unwritten and unverified sketch):**
```cpp
static int android_capture_start(void *user_data, int width, int height, int fps) {
    auto *ctx = static_cast<JniClientContext *>(user_data);
    if (!ctx) return -1;
    // Dispatch or invoke JniClientContext::onCaptureStart(width, height, fps)
    // so SipClient.kt starts SipCameraCapture on both outgoing and incoming video calls.
    return ctx->requestCaptureStart(width, height, fps);
}

static void android_capture_stop(void *user_data) {
    auto *ctx = static_cast<JniClientContext *>(user_data);
    if (!ctx) return;
    ctx->requestCaptureStop();
}
```

**What it gives:** Restores the `sip_video_capture_driver_t` Liskov Substitution contract on Android so Baresip's `vidsrc` allocation starts camera capture automatically on incoming and outgoing video calls and stops the camera when the video stream closes.

### Example 3. Forward Desktop `Client::set_video_capture_driver` to `sip_abi` and join capture worker threads (`platforms/desktop/src/client.cpp:348-357`, `platforms/windows/src/media/mf_capture.cpp:107-117`, `BOUND-2` & `COMP-2`)

**Before:**
```cpp
void Client::set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capture_driver = std::move(driver);
}
```

**After (unwritten and unverified sketch):**
```cpp
void Client::set_video_capture_driver(std::shared_ptr<sip_core::IVideoCaptureDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_capture_driver = std::move(driver);
    if (m_client && m_capture_driver) {
        sip_video_capture_driver_t c_drv = make_c_capture_driver(m_capture_driver.get());
        sip_client_set_video_capture_driver(m_client, &c_drv);
    }
}
```

**What it gives:** Connects `AVFoundationVideoCaptureDriver` and `V4L2VideoCaptureDriver` to `sip_core` through the C ABI boundary (`sip_client_set_video_capture_driver`), and—paired with replacing `.detach()` in `MfVideoCaptureDriver::start()` with a member `std::thread m_worker_thread` joined in `stop()`—closes the use-after-free on driver teardown.

### Next steps, by priority

1. **[x] Close immediate FFI & memory-safety defects (`BOUND-1`, `RES-2`, `COMP-2` / `RES-1`; remaining Phase 2 items `RES-4`, `RES-5`, `RES-6`, `RES-7`):** Fixed dangling `withUnsafeBufferPointer` pointers in `SipClient.swift:246-260` (`62c9f91`), joined the worker thread in `MfVideoCaptureDriver::stop()` (`8a1dbb4`), and balanced `call->retain()` when `post_task` returns `false` + bound and invoked `sip_call_destroy` on Android/macOS/Desktop call close (`9a161a6`). Remaining for Phase 2: guard `SipClient`/`SipCall` async main-queue lifetimes (`RES-4`), synchronize `sip_call_t` string getters (`RES-5`), acquire `ANativeWindow` references for the Android decoder thread (`RES-6`), and guard `CApiVideoCaptureDriver::m_cpp_handler` / drain `IosVideoCaptureDriver.stop()` (`RES-7`).
2. **[x] (Phase 1 subset) Restore broken platform driver and CallKit contracts (`SOLID-1` [x], `BOUND-2` [x]; remaining Phase 2 items `SOLID-3`, `RES-8`, `RES-9`):** Wired `android_capture_start`/`stop` to `SipCameraCapture` (`9a161a6`) and forwarded `sip::Client::set_video_capture_driver` to `sip_client_set_video_capture_driver` (`9a161a6`). Remaining for Phase 2: bind `sip_client_register_account` on Android/Desktop/Windows (`SOLID-3`), fix `IosAudioManager` refcounting (`RES-8`), and call `callKit.endCall(sipCall)` when a call closes on iOS (`RES-9`).
3. **[ ] Deduplicate RFC 6184 H.264 RTP logic and Web client code (`COMP-5`, `COMP-6`, `BOUND-5`, `SOLID-8`):** Extract the ~650-line RFC 6184 NAL packetizer/depacketizer into a portable `core/src/h264_rtp.cpp` shared by Apple `VideoToolbox` and Android `MediaCodec`, move Apple `VideoToolbox` and `AVAudioSession` out of `core/` into `platforms/ios/` & `platforms/macos/`, make `web/index.html` import `platforms/web/src/sipsdk.js`, and stop `SipMacClient.mm` from mutating OS-wide default audio devices.

## 5. Not checked

- **Runtime and physical device media verification:** Live camera hardware capture, CallKit lock-screen audio routing on physical iPhones, Android `MediaCodec` surface recreation under load, Linux V4L2 device capture, and Windows Media Foundation runtime execution were not run in this read-only static audit.
- **Prebuilt static binary internals:** `BaresipCore.xcframework` and `platforms/*/prebuilt/` (`libbaresip.a`, `libre.a`) were inspected only at their header and C ABI boundaries.
- **Refuted findings:** None (0 of 31 High/Critical findings were refuted by the adversarial verifiers; 1 finding, `RES-3` / `BOUND-6` on `struct sdp_media_internal` layout duplication in `core/src/engine.cpp:449-472`, was marked `doubtful` at Critical/High severity and lowered to Medium because all build scripts pin `libre` v4.12.0 with `-DUSE_UNIXSOCK=ON` and `engine.cpp:624, 1342` checks `ldir`/`rdir` before mutating).
