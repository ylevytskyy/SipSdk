# Local Call Recording (`call-recording`)

Adds local audio and audio+video call recording across `core/` and all 6 platform SDKs (iOS, macOS, Android, Desktop C++, Windows .NET, Web), without rebuilding any Baresip static prebuilts.

## Scope

1. **Core C ABI & Recording Engine (`core/`)**
   - **Modes (`sip_recording_mode_t`):**
     - `SIP_RECORDING_AUDIO_STEREO` (default): 48 kHz, 16-bit signed PCM WAV with 2 channels (**left = local mic**, **right = remote peer**).
     - `SIP_RECORDING_AUDIO_MONO`: 48 kHz, 16-bit signed PCM WAV with 1 channel (local + remote mixed with saturation clamping).
     - `SIP_RECORDING_AUDIO_VIDEO`: Synchronized audio (stereo 48 kHz S16LE) + remote video frames, muxed via a registered platform `sip_media_recorder_driver_t` (MP4/MOV) or `core/`'s built-in portable Matroska (`.mkv`) fallback when no platform driver is registered.
   - **C ABI (`core/include/sip_abi.h`):**
     - `sip_client_set_recording_callbacks(sip_client_t *client, const sip_recording_callbacks_t *callbacks)` — separate callback struct so `sip_callbacks_t` keeps its existing binary layout.
     - `sip_client_set_media_recorder_driver(sip_client_t *client, const sip_media_recorder_driver_t *driver)` — pluggable native video+audio muxer hook.
     - `sip_call_start_recording(sip_call_t *call, const char *path, sip_recording_mode_t mode)`
     - `sip_call_stop_recording(sip_call_t *call)`
     - `sip_call_is_recording(const sip_call_t *call)`
     - `sip_call_get_recording_path(const sip_call_t *call)`
   - **Audio capture (`aufilt`, ADR 0008):**
     - Registers a Baresip `aufilt` plugin (`sipsdk_rec`) in `baresip_aufiltl()` alongside `sipsdk_conf`.
     - Captures outgoing local mic PCM (`on_encode`) and incoming remote PCM (`on_decode`) per call (`struct audio *`), resampling mixed rates (8 / 16 / 48 kHz, mono/stereo) to 48 kHz.
     - Writes RIFF/WAVE header on start and patches final RIFF and `data` chunk sizes on stop so every file is valid and playable.
   - **Video capture:**
     - Taps decoded remote YUV420P frames inside `MediaDriverManager::handle_display_frame()` even when no UI `remote_renderer` is attached (so headless CLI and tests can record video).
   - **Lifecycle rules:**
     - Starting recording on a call that is already recording returns `EALREADY`.
     - Starting recording on a closed call or with an empty/null path returns `EINVAL`.
     - Hanging up, remote call close, or `sip_client_destroy()` automatically stops and finalizes any active recording and fires `on_recording_state(..., SIP_RECORDING_STATE_IDLE, path, 0)`.
     - Local mute writes silence on the local channel while keeping the remote channel recorded.

2. **Platform Muxers, Bindings & Sample UI (All 6 Platforms)**
   - **iOS (`platforms/ios`):** `AppleMediaRecorder` using `AVAssetWriter` (H.264 + AAC/LPCM `.mp4`). Expose `startRecording(to:mode:)`, `stopRecording()`, `isRecording`, `recordingPath` on `SipCall`, `.recordingStateChanged` on `SipClient`, and Record / Stop button in `SipPhone` `InCallView`.
   - **macOS (`platforms/macos`):** Share `AppleMediaRecorder` (`AVAssetWriter` `.mp4`), expose recording methods on `SipMacClient` / `SipMacClientDelegate`, and add Record / Stop button in `SipSdkMac` `InCallView`.
   - **Android (`platforms/android`):** `AndroidMediaRecorder` using `MediaCodec` + `MediaMuxer` (`.mp4`), expose `startRecording(path, mode)`, `stopRecording()`, `isRecording`, `recordingPath`, and `onRecordingStateChanged` over JNI in `SipCall` / `SipClientListener`, plus Record / Stop button in `InCallScreen`.
   - **Desktop C++ (`platforms/desktop`):** Expose `start_recording`, `stop_recording`, `is_recording`, `recording_path`, and `on_recording_state` on `sip::Call` / `sip::IClientListener`, plus `--record <path>` / `--record-mode` flags in `sip_cli` using the built-in WAV / portable MKV writers.
   - **Windows .NET (`platforms/windows`):** `WindowsMediaRecorder` using Media Foundation `IMFSinkWriter` (`.mp4`), bind ABI functions in `SipNativeAbi.cs`, expose `StartRecording`, `StopRecording`, `IsRecording`, `RecordingPath`, and `RecordingStateChanged` on `SipCall` / `SipClient`, plus Record / Stop button in `SipSdk.WpfSample`.
   - **Web (`platforms/web`):** Implement `startRecording({ mode })` / `stopRecording()` (returning a `Blob`) and `recordingStateChanged` in `platforms/web/src/sipsdk.js` using `MediaRecorder` + Web Audio API stereo/mono routing + remote video track, plus Record / Save button in `platforms/web/sample/`.

## Definition of done
- **Core unit test (`sip_core_call_recorder_test`):**
  - Stereo WAV test: feeds 440 Hz on local TX (8 kHz) and 1000 Hz on remote RX (48 kHz); parses the output WAV header (`RIFF`, `WAVE`, `fmt `, `data`, 48000 Hz, 2 ch, 16-bit) and proves with Goertzel DFT that left channel has 440 Hz (and suppresses 1000 Hz by >= 30 dB) and right channel has 1000 Hz (and suppresses 440 Hz by >= 30 dB).
  - Mono WAV test: feeds both tones and proves both 440 Hz and 1000 Hz are present in the single channel with no overflow clipping.
  - Portable MKV video+audio test: feeds YUV420P frames + stereo PCM and proves valid EBML/Matroska header, video + audio tracks, and non-empty clusters.
  - Error & auto-finalize tests: double-start returns `EALREADY`, invalid path returns error, and destructor/stop finalizes the header cleanly.
- **Core live PBX CTest against Docker Asterisk (`9001`):**
  - Places an audio call to `9001`, starts stereo WAV recording, verifies `is_recording == true`, stops recording (and also tests auto-stop on hangup), and validates the resulting WAV file header and non-zero PCM payload.
- **All platform verify scripts pass:**
  - `bash scripts/verify-desktop-sdk.sh`
  - `bash scripts/verify-ios-video-tests.sh`
  - `bash scripts/verify-macos-gui.sh`
  - `bash scripts/verify-android-sdk.sh`
  - `bash scripts/verify-windows-sdk.sh`
  - `bash scripts/verify-web-sdk.sh --pbx`

## Deliberately does NOT do
- Rebuilding Baresip static prebuilts (`BaresipCore.xcframework` / Android / Desktop / Windows).
- Recording local camera video or Picture-in-Picture video compositing (video recording captures the remote stream).
- Server-side PBX recording or cloud upload.
- Pause/resume within a single recording file (stop and start a new file instead).
