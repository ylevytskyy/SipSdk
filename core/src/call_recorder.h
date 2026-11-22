#ifndef SIP_CORE_CALL_RECORDER_H
#define SIP_CORE_CALL_RECORDER_H

#include "mkv_writer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sip_core {

enum class RecordingMode : int {
    AudioStereo = 0, // 48 kHz S16LE 2ch WAV (left = local, right = remote)
    AudioMono   = 1, // 48 kHz S16LE 1ch WAV (local + remote mixed)
    AudioVideo  = 2  // Audio + remote video (native driver MP4 or portable MKV fallback)
};

struct RecordedVideoFrame {
    int width{0};
    int height{0};
    const uint8_t *y{nullptr};
    const uint8_t *u{nullptr};
    const uint8_t *v{nullptr};
    int y_stride{0};
    int u_stride{0};
    int v_stride{0};
    uint64_t timestamp_ns{0};
};

class IMediaRecorderDriver {
public:
    virtual ~IMediaRecorderDriver() = default;
    virtual int start(const char *path, int sample_rate, int channels,
                      int video_width, int video_height) = 0;
    virtual void write_audio(const int16_t *pcm, size_t sample_count, uint64_t timestamp_ns) = 0;
    virtual void write_video(const RecordedVideoFrame &frame) = 0;
    virtual int stop() = 0;
};

/**
 * Thread-safe per-call audio/video recorder.
 *
 * Captures per-call local mic (on_encode) and remote peer (on_decode) S16LE PCM,
 * resamples mixed rates (8/16/48 kHz, mono/stereo) to 48 kHz, and writes either:
 *   - Stereo or Mono WAV directly in core/ (zero dependencies), or
 *   - Synchronized stereo PCM + remote YUV420P video to a registered platform
 *     IMediaRecorderDriver (AVAssetWriter / MediaMuxer / IMFSinkWriter), falling
 *     back to PortableMkvWriter (.mkv) when no platform driver is registered.
 */
class CallRecorder {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr size_t kChunkFrames = 960;      // 20 ms at 48 kHz
    static constexpr size_t kMaxSkewFrames = 2880;   // 60 ms max skew before padding

    CallRecorder() = default;
    ~CallRecorder();

    CallRecorder(const CallRecorder &) = delete;
    CallRecorder &operator=(const CallRecorder &) = delete;

    void set_media_recorder_driver(std::shared_ptr<IMediaRecorderDriver> driver);

    int start_recording(uint64_t leg_id, const std::string &path, RecordingMode mode,
                        int initial_video_width = 0, int initial_video_height = 0);
    int stop_recording(uint64_t leg_id, std::string *out_path = nullptr);
    void stop_all();

    bool is_recording(uint64_t leg_id) const;
    bool has_any_recording() const;
    std::string recording_path(uint64_t leg_id) const;

    void set_local_muted(uint64_t leg_id, bool muted);

    // Called from Baresip aufilt (local mic outgoing PCM)
    void on_encode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                   uint32_t srate, uint8_t ch);

    // Called from Baresip aufilt (remote peer incoming PCM)
    void on_decode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                   uint32_t srate, uint8_t ch);

    // Called from MediaDriverManager::handle_display_frame (remote video YUV420P)
    void on_video_frame(const RecordedVideoFrame &frame);

private:
    struct Session {
        uint64_t leg_id{0};
        std::string path;
        RecordingMode mode{RecordingMode::AudioStereo};
        bool local_muted{false};

        // WAV output (AudioStereo / AudioMono)
        std::FILE *wav_fp{nullptr};
        uint32_t wav_data_bytes{0};
        uint8_t wav_channels{2};

        // AudioVideo output
        std::shared_ptr<IMediaRecorderDriver> driver;
        std::unique_ptr<PortableMkvWriter> mkv;
        uint64_t audio_frames_written{0};
        uint64_t video_frames_written{0};
        uint64_t first_video_ts_ns{0};
        bool has_first_video_ts{false};

        // Resampled 48 kHz mono queues
        std::deque<int16_t> tx_queue;
        std::deque<int16_t> rx_queue;
    };

    static void resample_to_48k_mono(const int16_t *pcm, size_t sample_count,
                                     uint32_t srate, uint8_t ch,
                                     std::deque<int16_t> &dst);
    static int16_t clamp_s16(int32_t v);

    bool write_wav_header(std::FILE *fp, uint8_t channels);
    bool finalize_wav(Session &s);
    void drain_chunks_locked(Session &s, bool flush_all);
    void write_interleaved_locked(Session &s, const int16_t *samples, size_t frame_count, uint8_t ch);
    void finalize_session_locked(Session &s);

    mutable std::mutex m_mutex;
    std::shared_ptr<IMediaRecorderDriver> m_driver;
    std::unordered_map<uint64_t, std::unique_ptr<Session>> m_sessions;
};

} // namespace sip_core

#endif // SIP_CORE_CALL_RECORDER_H
