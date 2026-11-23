#include "mkv_writer.h"

#include <cstring>
#include <vector>

namespace sip_core {

namespace {

// EBML / Matroska element IDs (RFC 8794 / Matroska v2)
constexpr uint32_t kIdEbml              = 0x1A45DFA3;
constexpr uint32_t kIdEbmlVersion       = 0x4286;
constexpr uint32_t kIdEbmlReadVersion   = 0x42F7;
constexpr uint32_t kIdEbmlMaxIdLength   = 0x42F2;
constexpr uint32_t kIdEbmlMaxSizeLength = 0x42F3;
constexpr uint32_t kIdDocType           = 0x4282;
constexpr uint32_t kIdDocTypeVersion    = 0x4287;
constexpr uint32_t kIdDocTypeReadVer    = 0x4285;

constexpr uint32_t kIdSegment           = 0x18538067;
constexpr uint32_t kIdInfo              = 0x1549A966;
constexpr uint32_t kIdTimecodeScale     = 0x2AD7B1;
constexpr uint32_t kIdMuxingApp         = 0x4D80;
constexpr uint32_t kIdWritingApp        = 0x5741;

constexpr uint32_t kIdTracks            = 0x1654AE6B;
constexpr uint32_t kIdTrackEntry        = 0xAE;
constexpr uint32_t kIdTrackNumber       = 0xD7;
constexpr uint32_t kIdTrackUid          = 0x73C5;
constexpr uint32_t kIdTrackType         = 0x83;
constexpr uint32_t kIdCodecId           = 0x86;
constexpr uint32_t kIdCodecPrivate      = 0x63A2;

constexpr uint32_t kIdAudio             = 0xE1;
constexpr uint32_t kIdSamplingFreq      = 0xB5;
constexpr uint32_t kIdChannels          = 0x9F;
constexpr uint32_t kIdBitDepth          = 0x6264;

constexpr uint32_t kIdVideo             = 0xE0;
constexpr uint32_t kIdPixelWidth        = 0xB0;
constexpr uint32_t kIdPixelHeight       = 0xBA;

constexpr uint32_t kIdCluster           = 0x1F43B675;
constexpr uint32_t kIdClusterTimecode   = 0xE7;
constexpr uint32_t kIdSimpleBlock       = 0xA3;

void append_id(std::vector<uint8_t> &buf, uint32_t id)
{
    if (id <= 0xFF) {
        buf.push_back(static_cast<uint8_t>(id));
    } else if (id <= 0xFFFF) {
        buf.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(id & 0xFF));
    } else if (id <= 0xFFFFFF) {
        buf.push_back(static_cast<uint8_t>((id >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(id & 0xFF));
    } else {
        buf.push_back(static_cast<uint8_t>((id >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((id >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(id & 0xFF));
    }
}

void append_vint(std::vector<uint8_t> &buf, uint64_t val)
{
    if (val < 0x7F) {
        buf.push_back(static_cast<uint8_t>(0x80 | val));
    } else if (val < 0x3FFF) {
        buf.push_back(static_cast<uint8_t>(0x40 | (val >> 8)));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    } else if (val < 0x1FFFFF) {
        buf.push_back(static_cast<uint8_t>(0x20 | (val >> 16)));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    } else if (val < 0x0FFFFFFF) {
        buf.push_back(static_cast<uint8_t>(0x10 | (val >> 24)));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    } else {
        for (int i = 7; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
            if (i == 7) b = static_cast<uint8_t>(0x01 | (b & 0x00));
            buf.push_back(b);
        }
    }
}

void append_uint_elem(std::vector<uint8_t> &buf, uint32_t id, uint64_t val)
{
    append_id(buf, id);
    int len = 1;
    uint64_t tmp = val;
    while (tmp > 0xFF) {
        tmp >>= 8;
        ++len;
    }
    append_vint(buf, static_cast<uint64_t>(len));
    for (int i = len - 1; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

void append_float64_elem(std::vector<uint8_t> &buf, uint32_t id, double val)
{
    append_id(buf, id);
    append_vint(buf, 8);
    uint64_t bits = 0;
    std::memcpy(&bits, &val, sizeof(bits));
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    }
}

void append_string_elem(std::vector<uint8_t> &buf, uint32_t id, const std::string &str)
{
    append_id(buf, id);
    append_vint(buf, str.size());
    buf.insert(buf.end(), str.begin(), str.end());
}

void append_binary_elem(std::vector<uint8_t> &buf, uint32_t id, const uint8_t *data, size_t len)
{
    append_id(buf, id);
    append_vint(buf, len);
    buf.insert(buf.end(), data, data + len);
}

void append_master_elem(std::vector<uint8_t> &buf, uint32_t id, const std::vector<uint8_t> &payload)
{
    append_id(buf, id);
    append_vint(buf, payload.size());
    buf.insert(buf.end(), payload.begin(), payload.end());
}

void write_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void write_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

} // namespace

PortableMkvWriter::~PortableMkvWriter()
{
    close();
}

bool PortableMkvWriter::open(const std::string &path,
                             uint32_t audio_sample_rate,
                             uint8_t audio_channels,
                             int video_width,
                             int video_height)
{
    close();
    if (path.empty() || audio_sample_rate == 0 || audio_channels == 0) {
        return false;
    }
    m_fp = std::fopen(path.c_str(), "wb");
    if (!m_fp) {
        return false;
    }
    m_sample_rate = audio_sample_rate;
    m_channels = audio_channels;
    m_video_width = video_width > 0 ? video_width : 640;
    m_video_height = video_height > 0 ? video_height : 480;
    m_tracks_written = false;
    m_has_cluster = false;
    m_cluster_tc_ms = 0;

    if (!write_ebml_header() || !write_segment_preamble()) {
        close();
        return false;
    }
    if (video_width > 0 && video_height > 0) {
        if (!ensure_tracks_written(video_width, video_height)) {
            close();
            return false;
        }
    }
    return true;
}

bool PortableMkvWriter::write_bytes(const void *data, size_t len)
{
    if (!m_fp || len == 0) return m_fp != nullptr;
    return std::fwrite(data, 1, len, m_fp) == len;
}

bool PortableMkvWriter::write_id(uint32_t id)
{
    std::vector<uint8_t> buf;
    append_id(buf, id);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_vint_size(uint64_t size)
{
    std::vector<uint8_t> buf;
    append_vint(buf, size);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_uint_elem(uint32_t id, uint64_t val)
{
    std::vector<uint8_t> buf;
    append_uint_elem(buf, id, val);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_float64_elem(uint32_t id, double val)
{
    std::vector<uint8_t> buf;
    append_float64_elem(buf, id, val);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_string_elem(uint32_t id, const std::string &str)
{
    std::vector<uint8_t> buf;
    append_string_elem(buf, id, str);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_binary_elem(uint32_t id, const uint8_t *data, size_t len)
{
    std::vector<uint8_t> buf;
    append_binary_elem(buf, id, data, len);
    return write_bytes(buf.data(), buf.size());
}

bool PortableMkvWriter::write_ebml_header()
{
    std::vector<uint8_t> header;
    append_uint_elem(header, kIdEbmlVersion, 1);
    append_uint_elem(header, kIdEbmlReadVersion, 1);
    append_uint_elem(header, kIdEbmlMaxIdLength, 4);
    append_uint_elem(header, kIdEbmlMaxSizeLength, 8);
    append_string_elem(header, kIdDocType, "matroska");
    append_uint_elem(header, kIdDocTypeVersion, 2);
    append_uint_elem(header, kIdDocTypeReadVer, 2);

    std::vector<uint8_t> out;
    append_master_elem(out, kIdEbml, header);
    return write_bytes(out.data(), out.size());
}

bool PortableMkvWriter::write_segment_preamble()
{
    // Segment with unknown size (streaming Matroska: 0x01FFFFFFFFFFFFFF)
    const uint8_t seg_unknown_size[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!write_id(kIdSegment) || !write_bytes(seg_unknown_size, sizeof(seg_unknown_size))) {
        return false;
    }

    std::vector<uint8_t> info;
    append_uint_elem(info, kIdTimecodeScale, 1000000ULL); // 1 ms ticks
    append_string_elem(info, kIdMuxingApp, "SipSdk");
    append_string_elem(info, kIdWritingApp, "SipSdk");

    std::vector<uint8_t> out;
    append_master_elem(out, kIdInfo, info);
    return write_bytes(out.data(), out.size());
}

bool PortableMkvWriter::ensure_tracks_written(int video_width, int video_height)
{
    if (m_tracks_written) return true;
    if (video_width > 0) m_video_width = video_width;
    if (video_height > 0) m_video_height = video_height;

    std::vector<uint8_t> tracks;

    // Track 1: Audio (A_PCM/INT/LIT)
    {
        std::vector<uint8_t> audio;
        append_float64_elem(audio, kIdSamplingFreq, static_cast<double>(m_sample_rate));
        append_uint_elem(audio, kIdChannels, m_channels);
        append_uint_elem(audio, kIdBitDepth, 16);

        std::vector<uint8_t> entry;
        append_uint_elem(entry, kIdTrackNumber, 1);
        append_uint_elem(entry, kIdTrackUid, 1);
        append_uint_elem(entry, kIdTrackType, 2); // audio
        append_string_elem(entry, kIdCodecId, "A_PCM/INT/LIT");
        append_master_elem(entry, kIdAudio, audio);

        append_master_elem(tracks, kIdTrackEntry, entry);
    }

    // Track 2: Video (V_MS/VFW/FOURCC with BITMAPINFOHEADER I420)
    {
        uint8_t bih[40] = {};
        write_le32(bih + 0, 40); // biSize
        write_le32(bih + 4, static_cast<uint32_t>(m_video_width));
        write_le32(bih + 8, static_cast<uint32_t>(m_video_height));
        write_le16(bih + 12, 1);  // biPlanes
        write_le16(bih + 14, 12); // biBitCount (I420 = 12 bpp)
        bih[16] = 'I';
        bih[17] = '4';
        bih[18] = '2';
        bih[19] = '0';
        const uint32_t img_size =
            static_cast<uint32_t>(m_video_width * m_video_height * 3 / 2);
        write_le32(bih + 20, img_size);

        std::vector<uint8_t> video;
        append_uint_elem(video, kIdPixelWidth, static_cast<uint64_t>(m_video_width));
        append_uint_elem(video, kIdPixelHeight, static_cast<uint64_t>(m_video_height));

        std::vector<uint8_t> entry;
        append_uint_elem(entry, kIdTrackNumber, 2);
        append_uint_elem(entry, kIdTrackUid, 2);
        append_uint_elem(entry, kIdTrackType, 1); // video
        append_string_elem(entry, kIdCodecId, "V_MS/VFW/FOURCC");
        append_binary_elem(entry, kIdCodecPrivate, bih, sizeof(bih));
        append_master_elem(entry, kIdVideo, video);

        append_master_elem(tracks, kIdTrackEntry, entry);
    }

    std::vector<uint8_t> out;
    append_master_elem(out, kIdTracks, tracks);
    if (!write_bytes(out.data(), out.size())) {
        return false;
    }
    m_tracks_written = true;
    return true;
}

bool PortableMkvWriter::start_cluster(uint64_t cluster_timecode_ms)
{
    const uint8_t unknown_size[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!write_id(kIdCluster) || !write_bytes(unknown_size, sizeof(unknown_size))) {
        return false;
    }
    if (!write_uint_elem(kIdClusterTimecode, cluster_timecode_ms)) {
        return false;
    }
    m_has_cluster = true;
    m_cluster_tc_ms = cluster_timecode_ms;
    return true;
}

bool PortableMkvWriter::write_simple_block(uint8_t track_num,
                                           int16_t rel_timecode_ms,
                                           uint8_t flags,
                                           const uint8_t *data,
                                           size_t len)
{
    if (!write_id(kIdSimpleBlock)) return false;
    const uint64_t block_size = 4 + len; // 1-byte track vint + 2-byte tc + 1-byte flags + payload
    if (!write_vint_size(block_size)) return false;

    const uint8_t hdr[4] = {
        static_cast<uint8_t>(0x80 | track_num),
        static_cast<uint8_t>((rel_timecode_ms >> 8) & 0xFF),
        static_cast<uint8_t>(rel_timecode_ms & 0xFF),
        flags
    };
    if (!write_bytes(hdr, sizeof(hdr))) return false;
    return write_bytes(data, len);
}

bool PortableMkvWriter::write_audio(const int16_t *pcm, size_t sample_count, uint64_t timestamp_ms)
{
    if (!m_fp || !pcm || sample_count == 0) return false;
    if (!ensure_tracks_written(m_video_width, m_video_height)) return false;

    if (!m_has_cluster || timestamp_ms < m_cluster_tc_ms || (timestamp_ms - m_cluster_tc_ms) >= 5000) {
        if (!start_cluster(timestamp_ms)) return false;
    }
    const int16_t rel_tc = static_cast<int16_t>(timestamp_ms - m_cluster_tc_ms);
    return write_simple_block(
        1,
        rel_tc,
        0x80, // keyframe
        reinterpret_cast<const uint8_t *>(pcm),
        sample_count * sizeof(int16_t));
}

bool PortableMkvWriter::write_video_i420(const uint8_t *y, int y_stride,
                                         const uint8_t *u, int u_stride,
                                         const uint8_t *v, int v_stride,
                                         int width, int height,
                                         uint64_t timestamp_ms)
{
    if (!m_fp || !y || !u || !v || width <= 0 || height <= 0) return false;
    if (!ensure_tracks_written(width, height)) return false;

    if (!m_has_cluster || timestamp_ms < m_cluster_tc_ms || (timestamp_ms - m_cluster_tc_ms) >= 5000) {
        if (!start_cluster(timestamp_ms)) return false;
    }

    const int uv_w = width / 2;
    const int uv_h = height / 2;
    std::vector<uint8_t> packed;
    packed.reserve(static_cast<size_t>(width * height + 2 * uv_w * uv_h));

    for (int row = 0; row < height; ++row) {
        const uint8_t *src = y + static_cast<size_t>(row) * static_cast<size_t>(y_stride);
        packed.insert(packed.end(), src, src + width);
    }
    for (int row = 0; row < uv_h; ++row) {
        const uint8_t *src = u + static_cast<size_t>(row) * static_cast<size_t>(u_stride);
        packed.insert(packed.end(), src, src + uv_w);
    }
    for (int row = 0; row < uv_h; ++row) {
        const uint8_t *src = v + static_cast<size_t>(row) * static_cast<size_t>(v_stride);
        packed.insert(packed.end(), src, src + uv_w);
    }

    const int16_t rel_tc = static_cast<int16_t>(timestamp_ms - m_cluster_tc_ms);
    return write_simple_block(2, rel_tc, 0x80, packed.data(), packed.size());
}

void PortableMkvWriter::close()
{
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
        m_fp = nullptr;
    }
    m_tracks_written = false;
    m_has_cluster = false;
}

} // namespace sip_core
