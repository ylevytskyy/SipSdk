// Goertzel DFT tone-isolation tests for sip_core::ConferenceMixer.
#include "../src/conference_mixer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;

// Generate interleaved S16LE sine wave with continuous phase.
std::vector<int16_t> make_sine(double freq_hz, uint32_t srate, uint8_t ch,
                               size_t frames, double amplitude, size_t start_frame)
{
    std::vector<int16_t> out(frames * ch, 0);
    for (size_t i = 0; i < frames; ++i) {
        double phase = 2.0 * kPi * freq_hz * static_cast<double>(start_frame + i) / static_cast<double>(srate);
        int16_t sample = static_cast<int16_t>(std::llround(amplitude * std::sin(phase)));
        for (uint8_t c = 0; c < ch; ++c) {
            out[i * ch + c] = sample;
        }
    }
    return out;
}

// Compute normalized Goertzel magnitude (linear) at target frequency on channel 0.
double goertzel_mag(const std::vector<int16_t> &pcm, uint32_t srate, uint8_t ch, double target_hz)
{
    size_t n = pcm.size() / ch;
    if (n == 0) return 0.0;
    double k = std::round((static_cast<double>(n) * target_hz) / static_cast<double>(srate));
    double omega = (2.0 * kPi * k) / static_cast<double>(n);
    double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(pcm[i * ch]);
        s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return (2.0 * std::sqrt(std::max(0.0, power))) / static_cast<double>(n);
}

double ratio_db(double signal, double noise)
{
    double safe_noise = std::max(noise, 1e-6);
    return 20.0 * std::log10(std::max(signal, 1e-6) / safe_noise);
}

void test_two_leg_mix_minus_48k()
{
    sip_core::ConferenceMixer mixer;
    constexpr uint64_t kLegA = 1;
    constexpr uint64_t kLegB = 2;
    constexpr uint32_t kRate = 48000;
    constexpr size_t kFrame = 960; // 20 ms at 48 kHz
    constexpr int kBlocks = 10;    // 200 ms total
    constexpr double kAmp = 6000.0;

    mixer.add_leg(kLegA);
    mixer.add_leg(kLegB);
    TEST_ASSERT(mixer.is_active() && mixer.leg_count() == 2, "mixer must be active with 2 legs");

    std::vector<int16_t> tx_a_all;
    std::vector<int16_t> tx_b_all;

    for (int b = 0; b < kBlocks; ++b) {
        size_t offset = static_cast<size_t>(b) * kFrame;
        auto rx_a = make_sine(1000.0, kRate, 1, kFrame, kAmp, offset);
        auto rx_b = make_sine(1800.0, kRate, 1, kFrame, kAmp, offset);
        auto tx_a = make_sine(440.0, kRate, 1, kFrame, kAmp, offset);
        auto tx_b = make_sine(440.0, kRate, 1, kFrame, kAmp, offset);

        mixer.on_decode(kLegA, rx_a.data(), rx_a.size(), kRate, 1);
        mixer.on_decode(kLegB, rx_b.data(), rx_b.size(), kRate, 1);
        mixer.on_encode(kLegA, tx_a.data(), tx_a.size(), kRate, 1);
        mixer.on_encode(kLegB, tx_b.data(), tx_b.size(), kRate, 1);

        // Skip first block for steady-state measurement
        if (b >= 1) {
            tx_a_all.insert(tx_a_all.end(), tx_a.begin(), tx_a.end());
            tx_b_all.insert(tx_b_all.end(), tx_b.begin(), tx_b.end());
        }
    }

    double a_440 = goertzel_mag(tx_a_all, kRate, 1, 440.0);
    double a_1000 = goertzel_mag(tx_a_all, kRate, 1, 1000.0);
    double a_1800 = goertzel_mag(tx_a_all, kRate, 1, 1800.0);

    double b_440 = goertzel_mag(tx_b_all, kRate, 1, 440.0);
    double b_1000 = goertzel_mag(tx_b_all, kRate, 1, 1000.0);
    double b_1800 = goertzel_mag(tx_b_all, kRate, 1, 1800.0);

    TEST_ASSERT(a_440 > 4500.0, "peer A TX must contain local mic 440 Hz");
    TEST_ASSERT(a_1800 > 4500.0, "peer A TX must contain peer B 1800 Hz");
    TEST_ASSERT(ratio_db(a_1800, a_1000) >= 30.0, "peer A TX must suppress its own 1000 Hz by >= 30 dB");

    TEST_ASSERT(b_440 > 4500.0, "peer B TX must contain local mic 440 Hz");
    TEST_ASSERT(b_1000 > 4500.0, "peer B TX must contain peer A 1000 Hz");
    TEST_ASSERT(ratio_db(b_1000, b_1800) >= 30.0, "peer B TX must suppress its own 1800 Hz by >= 30 dB");

    std::printf("PASS: 2-leg 48kHz mix-minus (A_iso=%.1f dB, B_iso=%.1f dB)\n",
                ratio_db(a_1800, a_1000), ratio_db(b_1000, b_1800));
}

void test_mixed_sample_rates_8k_and_48k()
{
    sip_core::ConferenceMixer mixer;
    constexpr uint64_t kLeg8k = 10;
    constexpr uint64_t kLeg48k = 20;
    constexpr size_t kFrame8k = 160;   // 20 ms at 8 kHz (G.711)
    constexpr size_t kFrame48k = 960;  // 20 ms at 48 kHz (Opus stereo)
    constexpr int kBlocks = 10;
    constexpr double kAmp = 6000.0;

    mixer.add_leg(kLeg8k);
    mixer.add_leg(kLeg48k);

    std::vector<int16_t> tx_8k_all;
    std::vector<int16_t> tx_48k_all;

    for (int b = 0; b < kBlocks; ++b) {
        auto rx_8k = make_sine(1000.0, 8000, 1, kFrame8k, kAmp, static_cast<size_t>(b) * kFrame8k);
        auto rx_48k = make_sine(1800.0, 48000, 2, kFrame48k, kAmp, static_cast<size_t>(b) * kFrame48k);

        auto tx_8k = make_sine(440.0, 8000, 1, kFrame8k, kAmp, static_cast<size_t>(b) * kFrame8k);
        auto tx_48k = make_sine(440.0, 48000, 2, kFrame48k, kAmp, static_cast<size_t>(b) * kFrame48k);

        mixer.on_decode(kLeg8k, rx_8k.data(), rx_8k.size(), 8000, 1);
        mixer.on_decode(kLeg48k, rx_48k.data(), rx_48k.size(), 48000, 2);
        mixer.on_encode(kLeg8k, tx_8k.data(), tx_8k.size(), 8000, 1);
        mixer.on_encode(kLeg48k, tx_48k.data(), tx_48k.size(), 48000, 2);

        if (b >= 1) {
            tx_8k_all.insert(tx_8k_all.end(), tx_8k.begin(), tx_8k.end());
            tx_48k_all.insert(tx_48k_all.end(), tx_48k.begin(), tx_48k.end());
        }
    }

    double g8_440 = goertzel_mag(tx_8k_all, 8000, 1, 440.0);
    double g8_1000 = goertzel_mag(tx_8k_all, 8000, 1, 1000.0);
    double g8_1800 = goertzel_mag(tx_8k_all, 8000, 1, 1800.0);

    double g48_440 = goertzel_mag(tx_48k_all, 48000, 2, 440.0);
    double g48_1000 = goertzel_mag(tx_48k_all, 48000, 2, 1000.0);
    double g48_1800 = goertzel_mag(tx_48k_all, 48000, 2, 1800.0);

    TEST_ASSERT(g8_440 > 4500.0 && g8_1800 > 4000.0, "8kHz leg must hear 440 Hz + resampled 1800 Hz");
    TEST_ASSERT(ratio_db(g8_1800, g8_1000) >= 30.0, "8kHz leg must suppress its own 1000 Hz");
    TEST_ASSERT(g48_440 > 4500.0 && g48_1000 > 4000.0, "48kHz stereo leg must hear 440 Hz + resampled 1000 Hz");
    TEST_ASSERT(ratio_db(g48_1000, g48_1800) >= 30.0, "48kHz stereo leg must suppress its own 1800 Hz");

    std::printf("PASS: mixed sample-rate 8kHz <-> 48kHz stereo mix-minus (8k_iso=%.1f dB, 48k_iso=%.1f dB)\n",
                ratio_db(g8_1800, g8_1000), ratio_db(g48_1000, g48_1800));
}

void test_three_leg_multi_reader_and_local_mute()
{
    sip_core::ConferenceMixer mixer;
    constexpr uint64_t kA = 101, kB = 102, kC = 103;
    constexpr uint32_t kRate = 16000;
    constexpr size_t kFrame = 320; // 20 ms at 16 kHz (G.722)
    constexpr double kAmp = 5000.0;

    mixer.add_leg(kA);
    mixer.add_leg(kB);
    mixer.add_leg(kC);
    mixer.set_local_muted(true);
    TEST_ASSERT(mixer.is_local_muted(), "local mute must be true");

    std::vector<int16_t> tx_a_all, tx_c_all;
    for (int b = 0; b < 8; ++b) {
        size_t off = static_cast<size_t>(b) * kFrame;
        auto rx_a = make_sine(800.0, kRate, 1, kFrame, kAmp, off);
        auto rx_b = make_sine(1400.0, kRate, 1, kFrame, kAmp, off);
        auto rx_c = make_sine(2200.0, kRate, 1, kFrame, kAmp, off);

        auto tx_a = make_sine(440.0, kRate, 1, kFrame, kAmp, off);
        auto tx_b = make_sine(440.0, kRate, 1, kFrame, kAmp, off);
        auto tx_c = make_sine(440.0, kRate, 1, kFrame, kAmp, off);

        mixer.on_decode(kA, rx_a.data(), rx_a.size(), kRate, 1);
        mixer.on_decode(kB, rx_b.data(), rx_b.size(), kRate, 1);
        mixer.on_decode(kC, rx_c.data(), rx_c.size(), kRate, 1);

        mixer.on_encode(kA, tx_a.data(), tx_a.size(), kRate, 1);
        mixer.on_encode(kB, tx_b.data(), tx_b.size(), kRate, 1);
        mixer.on_encode(kC, tx_c.data(), tx_c.size(), kRate, 1);

        if (b >= 1) {
            tx_a_all.insert(tx_a_all.end(), tx_a.begin(), tx_a.end());
            tx_c_all.insert(tx_c_all.end(), tx_c.begin(), tx_c.end());
        }
    }

    // Local mic 440 Hz must be muted, while peer B (1400 Hz) must reach BOTH A and C!
    double a_440 = goertzel_mag(tx_a_all, kRate, 1, 440.0);
    double a_800 = goertzel_mag(tx_a_all, kRate, 1, 800.0);
    double a_1400 = goertzel_mag(tx_a_all, kRate, 1, 1400.0);
    double a_2200 = goertzel_mag(tx_a_all, kRate, 1, 2200.0);

    double c_440 = goertzel_mag(tx_c_all, kRate, 1, 440.0);
    double c_800 = goertzel_mag(tx_c_all, kRate, 1, 800.0);
    double c_1400 = goertzel_mag(tx_c_all, kRate, 1, 1400.0);
    double c_2200 = goertzel_mag(tx_c_all, kRate, 1, 2200.0);

    TEST_ASSERT(a_1400 > 3500.0 && a_2200 > 3500.0, "leg A must receive both B and C");
    TEST_ASSERT(ratio_db(a_1400, a_440) >= 30.0, "local mute must suppress 440 Hz to leg A");
    TEST_ASSERT(ratio_db(a_1400, a_800) >= 30.0, "leg A must not receive its own 800 Hz");

    TEST_ASSERT(c_800 > 3500.0 && c_1400 > 3500.0, "leg C must receive both A and B (multi-reader ring)");
    TEST_ASSERT(ratio_db(c_1400, c_440) >= 30.0, "local mute must suppress 440 Hz to leg C");
    TEST_ASSERT(ratio_db(c_1400, c_2200) >= 30.0, "leg C must not receive its own 2200 Hz");

    std::printf("PASS: 3-leg multi-reader mix-minus & local mute (mute_iso=%.1f dB)\n",
                ratio_db(a_1400, a_440));
}

void test_saturation_clamping_and_remove_leg()
{
    sip_core::ConferenceMixer mixer;
    mixer.add_leg(1);
    mixer.add_leg(2);

    std::vector<int16_t> hot_pos(960, 25000);
    std::vector<int16_t> hot_neg(960, -25000);

    mixer.on_decode(2, hot_pos.data(), hot_pos.size(), 48000, 1);
    std::vector<int16_t> tx_pos(960, 25000);
    mixer.on_encode(1, tx_pos.data(), tx_pos.size(), 48000, 1);
    for (int16_t s : tx_pos) {
        TEST_ASSERT(s == 32767, "positive overflow must clamp to INT16_MAX");
    }

    mixer.on_decode(2, hot_neg.data(), hot_neg.size(), 48000, 1);
    std::vector<int16_t> tx_neg(960, -25000);
    mixer.on_encode(1, tx_neg.data(), tx_neg.size(), 48000, 1);
    for (int16_t s : tx_neg) {
        TEST_ASSERT(s == -32768, "negative overflow must clamp to INT16_MIN");
    }

    // Removing a leg drops below 2 -> mixer becomes inactive (pass-through)
    mixer.remove_leg(2);
    TEST_ASSERT(!mixer.is_active() && mixer.leg_count() == 1, "removing down to 1 leg deactivates mixer");

    std::vector<int16_t> passthrough(960, 1234);
    mixer.on_encode(1, passthrough.data(), passthrough.size(), 48000, 1);
    TEST_ASSERT(passthrough[0] == 1234, "inactive mixer must pass TX through unchanged");

    std::printf("PASS: S16LE saturation clamping and leg removal pass-through\n");
}

} // namespace

int main()
{
    test_two_leg_mix_minus_48k();
    test_mixed_sample_rates_8k_and_48k();
    test_three_leg_multi_reader_and_local_mute();
    test_saturation_clamping_and_remove_leg();
    std::printf("ALL_CONFERENCE_MIXER_TESTS_PASSED\n");
    return 0;
}
