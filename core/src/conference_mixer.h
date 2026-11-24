#ifndef SIP_CORE_CONFERENCE_MIXER_H
#define SIP_CORE_CONFERENCE_MIXER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace sip_core {

/**
 * Thread-safe, multi-leg mix-minus PCM audio mixer for local audio conferencing.
 *
 * All participating legs push their decoded RX audio via on_decode() into a per-leg
 * 48 kHz mono ring buffer. When each leg encodes its outgoing TX audio in on_encode(),
 * it receives:
 *
 *   out_tx[leg] = (local_muted ? 0 : local_mic) + sum_{other != leg}(rx[other])
 *
 * Each (reader_leg, source_leg) pair maintains an independent monotonic read cursor,
 * so 2..8 conference legs can read every other leg's RX stream without interfering
 * with one another. Sample rates (e.g., 8 kHz G.711, 16 kHz G.722, 48 kHz Opus) and
 * channel counts (mono/stereo) are resampled/converted automatically.
 */
class ConferenceMixer {
public:
    static constexpr uint32_t kBusSampleRate = 48000;
    static constexpr size_t kRingCapacity = 4800;     // 100 ms at 48 kHz
    static constexpr size_t kMaxLagSamples = 2880;    // 60 ms max backlog before snapping

    ConferenceMixer() = default;
    ~ConferenceMixer() = default;

    ConferenceMixer(const ConferenceMixer &) = delete;
    ConferenceMixer &operator=(const ConferenceMixer &) = delete;

    void add_leg(uint64_t leg_id);
    void remove_leg(uint64_t leg_id);
    void clear();

    bool has_leg(uint64_t leg_id) const;
    size_t leg_count() const;
    bool is_active() const;

    void set_local_muted(bool muted);
    bool is_local_muted() const;

    /**
     * Capture decoded RX PCM (S16LE interleaved, sample_count = frames * ch) from a leg.
     * Does not modify pcm in place (OS mixer sums per-call auplay streams for local playout).
     */
    void on_decode(uint64_t leg_id, const int16_t *pcm, size_t sample_count,
                   uint32_t srate, uint8_t ch);

    /**
     * Mix local mic PCM (in-place S16LE interleaved, sample_count = frames * ch)
     * with all other conference legs' RX audio (minus leg_id's own RX).
     */
    void on_encode(uint64_t leg_id, int16_t *pcm, size_t sample_count,
                   uint32_t srate, uint8_t ch);

private:
    struct LegBuffer {
        std::array<int16_t, kRingCapacity> ring{};
        uint64_t write_pos{0};
        std::unordered_map<uint64_t, uint64_t> read_pos;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, LegBuffer> m_legs;
    bool m_local_muted{false};
};

} // namespace sip_core

#endif // SIP_CORE_CONFERENCE_MIXER_H
