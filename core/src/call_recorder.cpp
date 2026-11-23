#include "call_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace sip_core {

namespace {

void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

} // namespace

CallRecorder::~CallRecorder()
{
    stop_all();
}

int16_t CallRecorder::clamp_s16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

void CallRecorder::resample_to_48k_mono(const int16_t *pcm, size_t sample_count,
                                        uint32_t srate, uint8_t ch,
                                        std::deque<int16_t> &dst)
{
    if (!pcm || sample_count == 0 || srate == 0 || ch == 0) return;
    const size_t in_frames = sample_count / ch;
    if (in_frames == 0) return;

    auto read_mono = [&](size_t idx) -> int16_t {
        if (idx >= in_frames) idx = in_frames - 1;
        if (ch == 1) return pcm[idx];
        int32_t acc = 0;
        for (uint8_t c = 0; c < ch; ++c) {
            acc += pcm[idx * ch + c];
        }
        return static_cast<int16_t>(acc / ch);
    };

    if (srate == kSampleRate) {
        for (size_t i = 0; i < in_frames; ++i) {
            dst.push_back(read_mono(i));
        }
        return;
    }

    const size_t out_frames = static_cast<size_t>(
        (static_cast<uint64_t>(in_frames) * kSampleRate + srate / 2) / srate);
    if (out_frames == 0) return;

    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        double pos = static_cast<double>(i) * step;
        size_t idx0 = static_cast<size_t>(pos);
        if (idx0 >= in_frames) idx0 = in_frames - 1;
        size_t idx1 = std::min(idx0 + 1, in_frames - 1);
        double frac = pos - static_cast<double>(idx0);
        double s0 = static_cast<double>(read_mono(idx0));
        double s1 = static_cast<double>(read_mono(idx1));
        dst.push_back(clamp_s16(static_cast<int32_t>(std::llround(s0 + (s1 - s0) * frac))));
    }
}

void CallRecorder::set_media_recorder_driver(std::shared_ptr<IMediaRecorderDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_driver = std::move(driver);
}

bool CallRecorder::write_wav_header(std::FILE *fp, uint8_t channels)
{
    uint8_t hdr[44] = {};
    std::memcpy(hdr + 0, "RIFF", 4);
    put_le32(hdr + 4, 36); // placeholder file size - 8
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put_le32(hdr + 16, 16); // PCM fmt chunk size
    put_le16(hdr + 20, 1);  // PCM format
    put_le16(hdr + 22, channels);
    put_le32(hdr + 24, kSampleRate);
    const uint32_t byte_rate = kSampleRate * channels * 2;
    put_le32(hdr + 28, byte_rate);
    put_le16(hdr + 32, static_cast<uint16_t>(channels * 2)); // block align
    put_le16(hdr + 34, 16); // bits per sample
    std::memcpy(hdr + 36, "data", 4);
    put_le32(hdr + 40, 0); // placeholder data bytes
    return std::fwrite(hdr, 1, sizeof(hdr), fp) == sizeof(hdr);
}

bool CallRecorder::finalize_wav(Session &s)
{
    if (!s.wav_fp) return true;
    std::fflush(s.wav_fp);
    bool ok = true;
    if (std::fseek(s.wav_fp, 4, SEEK_SET) == 0) {
        uint8_t riff_sz[4];
        put_le32(riff_sz, 36u + s.wav_data_bytes);
        ok = ok && (std::fwrite(riff_sz, 1, 4, s.wav_fp) == 4);
    }
    if (std::fseek(s.wav_fp, 40, SEEK_SET) == 0) {
        uint8_t data_sz[4];
        put_le32(data_sz, s.wav_data_bytes);
        ok = ok && (std::fwrite(data_sz, 1, 4, s.wav_fp) == 4);
    }
    std::fflush(s.wav_fp);
    std::fclose(s.wav_fp);
    s.wav_fp = nullptr;
    return ok;
}

int CallRecorder::start_recording(uint64_t leg_id, const std::string &path, RecordingMode mode,
                                  int initial_video_width, int initial_video_height)
{
    if (leg_id == 0 || path.empty()) {
        return EINVAL;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sessions.find(leg_id) != m_sessions.end()) {
        return EALREADY;
    }

    auto session = std::make_unique<Session>();
    session->leg_id = leg_id;
    session->path = path;
    session->mode = mode;

    if (mode == RecordingMode::AudioStereo || mode == RecordingMode::AudioMono) {
        session->wav_channels = (mode == RecordingMode::AudioStereo) ? 2 : 1;
        session->wav_fp = std::fopen(path.c_str(), "wb");
        if (!session->wav_fp) {
            return errno ? errno : EIO;
        }
        if (!write_wav_header(session->wav_fp, session->wav_channels)) {
            std::fclose(session->wav_fp);
            session->wav_fp = nullptr;
            return EIO;
        }
    } else if (mode == RecordingMode::AudioVideo) {
        session->wav_channels = 2;
        if (m_driver) {
            int rc = m_driver->start(path.c_str(), static_cast<int>(kSampleRate), 2,
                                     initial_video_width, initial_video_height);
            if (rc != 0) {
                return rc;
            }
            session->driver = m_driver;
        } else {
            session->mkv = std::make_unique<PortableMkvWriter>();
            if (!session->mkv->open(path, kSampleRate, 2,
                                    initial_video_width, initial_video_height)) {
                return errno ? errno : EIO;
            }
        }
    } else {
        return EINVAL;
    }

    m_sessions[leg_id] = std::move(session);
    return 0;
}

void CallRecorder::write_interleaved_locked(Session &s, const int16_t *samples,
                                            size_t frame_count, uint8_t ch)
{
    if (frame_count == 0) return;
    const size_t sample_count = frame_count * ch;
    if (s.wav_fp) {
        const size_t bytes = sample_count * sizeof(int16_t);
        if (std::fwrite(samples, 1, bytes, s.wav_fp) == bytes) {
            s.wav_data_bytes += static_cast<uint32_t>(bytes);
        }
    } else if (s.driver) {
        const uint64_t ts_ns = (s.audio_frames_written * 1000000000ULL) / kSampleRate;
        s.driver->write_audio(samples, sample_count, ts_ns);
    } else if (s.mkv) {
        const uint64_t ts_ms = (s.audio_frames_written * 1000ULL) / kSampleRate;
        s.mkv->write_audio(samples, sample_count, ts_ms);
    }
    s.audio_frames_written += frame_count;
}

void CallRecorder::drain_chunks_locked(Session &s, bool flush_all)
{
    const uint8_t out_ch = (s.mode == RecordingMode::AudioMono) ? 1 : 2;
    std::vector<int16_t> out;

    while (true) {
        size_t tx_sz = s.tx_queue.size();
        size_t rx_sz = s.rx_queue.size();

        size_t emit_frames = 0;
        if (flush_all) {
            emit_frames = std::max(tx_sz, rx_sz);
            if (emit_frames == 0) break;
        } else if (tx_sz >= kChunkFrames && rx_sz >= kChunkFrames) {
            emit_frames = kChunkFrames;
        } else if (tx_sz >= kMaxSkewFrames || rx_sz >= kMaxSkewFrames) {
            emit_frames = kChunkFrames;
        } else {
            break;
        }

        out.resize(emit_frames * out_ch);
        for (size_t i = 0; i < emit_frames; ++i) {
            int16_t l = 0;
            if (!s.tx_queue.empty()) {
                l = s.tx_queue.front();
                s.tx_queue.pop_front();
            }
            int16_t r = 0;
            if (!s.rx_queue.empty()) {
                r = s.rx_queue.front();
                s.rx_queue.pop_front();
            }
            if (out_ch == 2) {
                out[i * 2 + 0] = l;
                out[i * 2 + 1] = r;
            } else {
                out[i] = clamp_s16(static_cast<int32_t>(l) + static_cast<int32_t>(r));
            }
        }
        write_interleaved_locked(s, out.data(), emit_frames, out_ch);
        if (flush_all) break;
    }
}

void CallRecorder::finalize_session_locked(Session &s)
{
    drain_chunks_locked(s, true);
    if (s.wav_fp) {
        finalize_wav(s);
    }
    if (s.driver) {
        s.driver->stop();
        s.driver.reset();
    }
    if (s.mkv) {
        s.mkv->close();
        s.mkv.reset();
    }
}

int CallRecorder::stop_recording(uint64_t leg_id, std::string *out_path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(leg_id);
    if (it == m_sessions.end()) {
        return ENOENT;
    }
    if (out_path) {
        *out_path = it->second->path;
    }
    finalize_session_locked(*it->second);
    m_sessions.erase(it);
    return 0;
}

void CallRecorder::stop_all()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &kv : m_sessions) {
        finalize_session_locked(*kv.second);
    }
    m_sessions.clear();
}

bool CallRecorder::is_recording(uint64_t leg_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.find(leg_id) != m_sessions.end();
}

bool CallRecorder::has_any_recording() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_sessions.empty();
}

std::string CallRecorder::recording_path(uint64_t leg_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(leg_id);
    return (it != m_sessions.end()) ? it->second->path : std::string();
}

void CallRecorder::set_local_muted(uint64_t leg_id, bool muted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(leg_id);
    if (it != m_sessions.end()) {
        it->second->local_muted = muted;
    }
}

void CallRecorder::on_encode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                             uint32_t srate, uint8_t ch)
{
    if (!pcm || sample_count == 0 || srate == 0 || ch == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(leg_id);
    if (it == m_sessions.end()) return;

    Session &s = *it->second;
    if (s.local_muted) {
        const size_t in_frames = sample_count / ch;
        const size_t out_frames = static_cast<size_t>(
            (static_cast<uint64_t>(in_frames) * kSampleRate + srate / 2) / srate);
        s.tx_queue.insert(s.tx_queue.end(), out_frames, 0);
    } else {
        resample_to_48k_mono(pcm, sample_count, srate, ch, s.tx_queue);
    }
    drain_chunks_locked(s, false);
}

void CallRecorder::on_decode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                             uint32_t srate, uint8_t ch)
{
    if (!pcm || sample_count == 0 || srate == 0 || ch == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(leg_id);
    if (it == m_sessions.end()) return;

    Session &s = *it->second;
    resample_to_48k_mono(pcm, sample_count, srate, ch, s.rx_queue);
    drain_chunks_locked(s, false);
}

void CallRecorder::on_video_frame(const RecordedVideoFrame &frame)
{
    if (!frame.y || !frame.u || !frame.v || frame.width <= 0 || frame.height <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &kv : m_sessions) {
        Session &s = *kv.second;
        if (s.mode != RecordingMode::AudioVideo) continue;

        if (!s.has_first_video_ts) {
            s.first_video_ts_ns = frame.timestamp_ns;
            s.has_first_video_ts = true;
        }
        uint64_t rel_ns = (frame.timestamp_ns >= s.first_video_ts_ns)
                              ? (frame.timestamp_ns - s.first_video_ts_ns)
                              : (s.video_frames_written * 33333333ULL);
        if (rel_ns == 0 && s.video_frames_written > 0) {
            rel_ns = s.video_frames_written * 33333333ULL;
        }

        if (s.driver) {
            RecordedVideoFrame rel = frame;
            rel.timestamp_ns = rel_ns;
            s.driver->write_video(rel);
        } else if (s.mkv) {
            s.mkv->write_video_i420(frame.y, frame.y_stride,
                                    frame.u, frame.u_stride,
                                    frame.v, frame.v_stride,
                                    frame.width, frame.height,
                                    rel_ns / 1000000ULL);
        }
        s.video_frames_written++;
    }
}

} // namespace sip_core
