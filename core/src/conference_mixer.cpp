#include "conference_mixer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace sip_core {

namespace {

inline int16_t clamp_s16(int32_t sample)
{
    if (sample > std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    if (sample < std::numeric_limits<int16_t>::min()) {
        return std::numeric_limits<int16_t>::min();
    }
    return static_cast<int16_t>(sample);
}

// Downmix interleaved S16LE frames to mono.
void downmix_to_mono(const int16_t *pcm, size_t frames, uint8_t ch, std::vector<int16_t> &mono)
{
    mono.resize(frames);
    if (ch == 1) {
        std::copy(pcm, pcm + frames, mono.begin());
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (uint8_t c = 0; c < ch; ++c) {
            acc += pcm[i * ch + c];
        }
        mono[i] = static_cast<int16_t>(acc / static_cast<int32_t>(ch));
    }
}

// Linear interpolation resampler for mono S16LE -> S16LE.
void resample_linear_i16(const int16_t *in, size_t in_frames, uint32_t in_rate,
                         std::vector<int16_t> &out, size_t out_frames, uint32_t out_rate)
{
    out.resize(out_frames);
    if (in_frames == 0 || out_frames == 0) {
        std::fill(out.begin(), out.end(), 0);
        return;
    }
    if (in_rate == out_rate && in_frames == out_frames) {
        std::copy(in, in + out_frames, out.begin());
        return;
    }
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        const double pos = static_cast<double>(i) * step;
        const size_t idx0 = static_cast<size_t>(pos);
        const size_t idx1 = std::min(idx0 + 1, in_frames - 1);
        const double frac = pos - static_cast<double>(idx0);
        const double v = (1.0 - frac) * static_cast<double>(in[idx0]) +
                         frac * static_cast<double>(in[idx1]);
        out[i] = clamp_s16(static_cast<int32_t>(std::llround(v)));
    }
}

// Linear interpolation resampler for mono int32 -> int32.
void resample_linear_i32(const int32_t *in, size_t in_frames, uint32_t in_rate,
                         std::vector<int32_t> &out, size_t out_frames, uint32_t out_rate)
{
    out.resize(out_frames);
    if (in_frames == 0 || out_frames == 0) {
        std::fill(out.begin(), out.end(), 0);
        return;
    }
    if (in_rate == out_rate && in_frames == out_frames) {
        std::copy(in, in + out_frames, out.begin());
        return;
    }
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        const double pos = static_cast<double>(i) * step;
        const size_t idx0 = static_cast<size_t>(pos);
        const size_t idx1 = std::min(idx0 + 1, in_frames - 1);
        const double frac = pos - static_cast<double>(idx0);
        const double v = (1.0 - frac) * static_cast<double>(in[idx0]) +
                         frac * static_cast<double>(in[idx1]);
        out[i] = static_cast<int32_t>(std::llround(v));
    }
}

} // namespace

void ConferenceMixer::add_leg(uint64_t leg_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_legs.find(leg_id) != m_legs.end()) {
        return;
    }
    LegBuffer buf{};
    m_legs.emplace(leg_id, std::move(buf));
}

void ConferenceMixer::remove_leg(uint64_t leg_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_legs.erase(leg_id);
    for (auto &kv : m_legs) {
        kv.second.read_pos.erase(leg_id);
    }
}

void ConferenceMixer::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_legs.clear();
    m_local_muted = false;
}

bool ConferenceMixer::has_leg(uint64_t leg_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_legs.find(leg_id) != m_legs.end();
}

size_t ConferenceMixer::leg_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_legs.size();
}

bool ConferenceMixer::is_active() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_legs.size() >= 2;
}

void ConferenceMixer::set_local_muted(bool muted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_local_muted = muted;
}

bool ConferenceMixer::is_local_muted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_local_muted;
}

void ConferenceMixer::on_decode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                                uint32_t srate, uint8_t ch)
{
    if (!pcm || sample_count == 0 || srate == 0 || ch == 0) {
        return;
    }

    const size_t in_frames = sample_count / ch;
    if (in_frames == 0) {
        return;
    }

    const size_t bus_frames = std::max<size_t>(
        1, static_cast<size_t>(std::llround(
               static_cast<double>(in_frames) * static_cast<double>(kBusSampleRate) /
               static_cast<double>(srate))));

    std::vector<int16_t> mono;
    std::vector<int16_t> bus_pcm;
    downmix_to_mono(pcm, in_frames, ch, mono);
    resample_linear_i16(mono.data(), in_frames, srate, bus_pcm, bus_frames, kBusSampleRate);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_legs.size() < 2) {
        return;
    }
    auto it = m_legs.find(leg_id);
    if (it == m_legs.end()) {
        return;
    }

    LegBuffer &buf = it->second;
    for (size_t i = 0; i < bus_frames; ++i) {
        buf.ring[buf.write_pos % kRingCapacity] = bus_pcm[i];
        ++buf.write_pos;
    }
}

void ConferenceMixer::on_encode(uint64_t leg_id, int16_t *pcm, size_t sample_count,
                                uint32_t srate, uint8_t ch)
{
    if (!pcm || sample_count == 0 || srate == 0 || ch == 0) {
        return;
    }

    const size_t out_frames = sample_count / ch;
    if (out_frames == 0) {
        return;
    }

    const size_t bus_frames = std::max<size_t>(
        1, static_cast<size_t>(std::llround(
               static_cast<double>(out_frames) * static_cast<double>(kBusSampleRate) /
               static_cast<double>(srate))));

    std::vector<int32_t> mix_bus(bus_frames, 0);
    bool muted = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_legs.size() < 2 || m_legs.find(leg_id) == m_legs.end()) {
            return;
        }
        muted = m_local_muted;

        for (auto &kv : m_legs) {
            if (kv.first == leg_id) {
                continue; // Mix-minus: never feed a leg its own audio back.
            }
            LegBuffer &src = kv.second;
            auto rit = src.read_pos.find(leg_id);
            uint64_t rpos = 0;
            if (rit == src.read_pos.end()) {
                // First read for this reader: start at most bus_frames behind write_pos.
                rpos = (src.write_pos > bus_frames) ? (src.write_pos - bus_frames) : 0;
            } else {
                rpos = rit->second;
            }

            // If reader fell too far behind, snap forward to avoid stale latency buildup.
            if (src.write_pos > rpos + kMaxLagSamples) {
                rpos = src.write_pos - std::min<uint64_t>(bus_frames, kRingCapacity - 1);
            }

            const uint64_t avail = (src.write_pos > rpos) ? (src.write_pos - rpos) : 0;
            const size_t to_read = static_cast<size_t>(std::min<uint64_t>(bus_frames, avail));
            for (size_t i = 0; i < to_read; ++i) {
                mix_bus[i] += static_cast<int32_t>(src.ring[(rpos + i) % kRingCapacity]);
            }
            src.read_pos[leg_id] = rpos + to_read;
        }
    }

    std::vector<int32_t> mix_out;
    resample_linear_i32(mix_bus.data(), bus_frames, kBusSampleRate, mix_out, out_frames, srate);

    for (size_t i = 0; i < out_frames; ++i) {
        const int32_t peer_sample = mix_out[i];
        for (uint8_t c = 0; c < ch; ++c) {
            const int32_t local_sample = muted ? 0 : static_cast<int32_t>(pcm[i * ch + c]);
            pcm[i * ch + c] = clamp_s16(local_sample + peer_sample);
        }
    }
}

} // namespace sip_core
