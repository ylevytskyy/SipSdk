# 0008. Record audio via `core/` `aufilt` WAV writer and video via pluggable native muxers

- **Status**: Accepted
- **Date**: 2026-11-22

`sip_core` captures per-call local (TX) and remote (RX) PCM audio through a portable C++17 Baresip `aufilt` plugin (`sipsdk_rec`) registered via `aufilt_register(baresip_aufiltl(), ...)` and writes standard 48 kHz S16LE WAV files (stereo with left = local, right = remote, or mixed mono) with zero external dependencies and no rebuild of Baresip static prebuilts. For audio+video recording, `sip_core` taps decoded remote YUV420P frames in `MediaDriverManager::handle_display_frame()` and forwards synchronized PCM + YUV420P frames to a pluggable `sip_media_recorder_driver_t` backed by native OS MP4 muxers (`AVAssetWriter` on iOS/macOS, `MediaCodec` + `MediaMuxer` on Android, Media Foundation `IMFSinkWriter` on Windows), falling back to a self-contained C++17 Matroska (`.mkv`) writer in `core/` on Linux and headless CLI targets.
