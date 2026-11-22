#ifndef SIP_CORE_MKV_WRITER_H
#define SIP_CORE_MKV_WRITER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sip_core {

/**
 * Self-contained, zero-dependency Matroska (.mkv) muxer used as the portable
 * fallback for SIP_RECORDING_AUDIO_VIDEO on platforms without a registered native
 * MP4 recorder driver (e.g. Linux / headless CLI).
 *
 * Track 1: Audio — A_PCM/INT/LIT (S16LE interleaved PCM)
 * Track 2: Video — V_MS/VFW/FOURCC (I420 / YUV420P uncompressed)
 */
class PortableMkvWriter {
public:
    PortableMkvWriter() = default;
    ~PortableMkvWriter();

    PortableMkvWriter(const PortableMkvWriter &) = delete;
    PortableMkvWriter &operator=(const PortableMkvWriter &) = delete;

    bool open(const std::string &path,
              uint32_t audio_sample_rate,
              uint8_t audio_channels,
              int video_width,
              int video_height);

    bool write_audio(const int16_t *pcm, size_t sample_count, uint64_t timestamp_ms);

    bool write_video_i420(const uint8_t *y, int y_stride,
                          const uint8_t *u, int u_stride,
                          const uint8_t *v, int v_stride,
                          int width, int height,
                          uint64_t timestamp_ms);

    void close();
    bool is_open() const { return m_fp != nullptr; }

private:
    bool write_ebml_header();
    bool write_segment_preamble();
    bool ensure_tracks_written(int video_width, int video_height);
    bool start_cluster(uint64_t cluster_timecode_ms);
    bool write_simple_block(uint8_t track_num, int16_t rel_timecode_ms,
                            uint8_t flags, const uint8_t *data, size_t len);

    bool write_bytes(const void *data, size_t len);
    bool write_id(uint32_t id);
    bool write_vint_size(uint64_t size);
    bool write_uint_elem(uint32_t id, uint64_t val);
    bool write_float64_elem(uint32_t id, double val);
    bool write_string_elem(uint32_t id, const std::string &str);
    bool write_binary_elem(uint32_t id, const uint8_t *data, size_t len);

    std::FILE *m_fp{nullptr};
    uint32_t m_sample_rate{48000};
    uint8_t m_channels{2};
    int m_video_width{0};
    int m_video_height{0};
    bool m_tracks_written{false};
    bool m_has_cluster{false};
    uint64_t m_cluster_tc_ms{0};
};

} // namespace sip_core

#endif // SIP_CORE_MKV_WRITER_H
