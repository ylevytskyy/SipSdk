// Unit tests for sip_core::CallRecorder (WAV stereo/mono, portable MKV, driver hook, error paths).
#include "../src/call_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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

double goertzel_mag(const std::vector<int16_t> &pcm, uint32_t srate,
                    uint8_t ch, uint8_t ch_idx, double target_hz)
{
    size_t n = pcm.size() / ch;
    if (n == 0) return 0.0;
    double k = std::round((static_cast<double>(n) * target_hz) / static_cast<double>(srate));
    double omega = (2.0 * kPi * k) / static_cast<double>(n);
    double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(pcm[i * ch + ch_idx]);
        s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return (2.0 * std::sqrt(std::max(0.0, power))) / static_cast<double>(n);
}

double ratio_db(double signal, double noise)
{
    return 20.0 * std::log10(std::max(signal, 1e-6) / std::max(noise, 1e-6));
}

uint16_t read_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> read_file_bytes(const std::string &path)
{
    std::ifstream ifs(path, std::ios::binary);
    TEST_ASSERT(ifs.good(), "failed to open recorded file");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
}

void test_stereo_wav_mixed_rates_and_isolation()
{
    const std::string path = "/tmp/sipsdk_test_stereo.wav";
    std::remove(path.c_str());

    sip_core::CallRecorder rec;
    constexpr uint64_t kLeg = 42;
    TEST_ASSERT(rec.start_recording(kLeg, path, sip_core::RecordingMode::AudioStereo) == 0,
                "start stereo recording must succeed");
    TEST_ASSERT(rec.is_recording(kLeg), "is_recording must be true");
    TEST_ASSERT(rec.recording_path(kLeg) == path, "recording_path must match");
    TEST_ASSERT(rec.start_recording(kLeg, path, sip_core::RecordingMode::AudioStereo) == EALREADY,
                "double-start must return EALREADY");

    // Feed 10 blocks (200 ms): local TX at 8 kHz (440 Hz), remote RX at 48 kHz (1000 Hz)
    for (int b = 0; b < 10; ++b) {
        auto tx = make_sine(440.0, 8000, 1, 160, 8000.0, static_cast<size_t>(b) * 160);
        auto rx = make_sine(1000.0, 48000, 1, 960, 8000.0, static_cast<size_t>(b) * 960);
        rec.on_encode(kLeg, tx.data(), tx.size(), 8000, 1);
        rec.on_decode(kLeg, rx.data(), rx.size(), 48000, 1);
    }

    std::string stopped_path;
    TEST_ASSERT(rec.stop_recording(kLeg, &stopped_path) == 0, "stop_recording must succeed");
    TEST_ASSERT(stopped_path == path, "stopped_path must match");
    TEST_ASSERT(!rec.is_recording(kLeg), "is_recording must be false after stop");

    auto bytes = read_file_bytes(path);
    TEST_ASSERT(bytes.size() > 44, "WAV file must contain header + PCM payload");
    TEST_ASSERT(std::memcmp(bytes.data(), "RIFF", 4) == 0, "RIFF magic");
    TEST_ASSERT(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0, "WAVE magic");
    TEST_ASSERT(std::memcmp(bytes.data() + 12, "fmt ", 4) == 0, "fmt chunk");
    TEST_ASSERT(read_le16(bytes.data() + 20) == 1, "PCM format = 1");
    TEST_ASSERT(read_le16(bytes.data() + 22) == 2, "channels = 2");
    TEST_ASSERT(read_le32(bytes.data() + 24) == 48000, "sample rate = 48000");
    TEST_ASSERT(read_le16(bytes.data() + 34) == 16, "bits per sample = 16");
    TEST_ASSERT(std::memcmp(bytes.data() + 36, "data", 4) == 0, "data chunk");

    uint32_t riff_sz = read_le32(bytes.data() + 4);
    uint32_t data_sz = read_le32(bytes.data() + 40);
    TEST_ASSERT(riff_sz == bytes.size() - 8, "RIFF chunk size must match file size - 8");
    TEST_ASSERT(data_sz == bytes.size() - 44, "data chunk size must match payload size");

    size_t sample_count = data_sz / 2;
    std::vector<int16_t> pcm(sample_count);
    std::memcpy(pcm.data(), bytes.data() + 44, data_sz);

    double l_440 = goertzel_mag(pcm, 48000, 2, 0, 440.0);
    double l_1000 = goertzel_mag(pcm, 48000, 2, 0, 1000.0);
    double r_440 = goertzel_mag(pcm, 48000, 2, 1, 440.0);
    double r_1000 = goertzel_mag(pcm, 48000, 2, 1, 1000.0);

    TEST_ASSERT(l_440 > 5500.0, "left channel must contain local 440 Hz tone");
    TEST_ASSERT(ratio_db(l_440, l_1000) >= 30.0, "left channel must isolate local from remote (>=30 dB)");
    TEST_ASSERT(r_1000 > 5500.0, "right channel must contain remote 1000 Hz tone");
    TEST_ASSERT(ratio_db(r_1000, r_440) >= 30.0, "right channel must isolate remote from local (>=30 dB)");

    std::remove(path.c_str());
    std::printf("PASS: stereo WAV 8k/48k isolation (L=%.1f dB, R=%.1f dB)\n",
                ratio_db(l_440, l_1000), ratio_db(r_1000, r_440));
}

void test_mono_wav_mix_and_mute()
{
    const std::string path = "/tmp/sipsdk_test_mono.wav";
    std::remove(path.c_str());

    sip_core::CallRecorder rec;
    constexpr uint64_t kLeg = 7;
    TEST_ASSERT(rec.start_recording(kLeg, path, sip_core::RecordingMode::AudioMono) == 0,
                "start mono recording must succeed");

    // 10 blocks = 200 ms (exact integer bin for 440 Hz and 1500 Hz at 48 kHz)
    for (int b = 0; b < 10; ++b) {
        auto tx = make_sine(440.0, 48000, 1, 960, 7000.0, static_cast<size_t>(b) * 960);
        auto rx = make_sine(1500.0, 48000, 1, 960, 7000.0, static_cast<size_t>(b) * 960);
        rec.on_encode(kLeg, tx.data(), tx.size(), 48000, 1);
        rec.on_decode(kLeg, rx.data(), rx.size(), 48000, 1);
    }

    TEST_ASSERT(rec.stop_recording(kLeg) == 0, "stop mono recording must succeed");

    auto bytes = read_file_bytes(path);
    TEST_ASSERT(read_le16(bytes.data() + 22) == 1, "mono channels = 1");
    uint32_t data_sz = read_le32(bytes.data() + 40);
    std::vector<int16_t> pcm(data_sz / 2);
    std::memcpy(pcm.data(), bytes.data() + 44, data_sz);

    double m_440 = goertzel_mag(pcm, 48000, 1, 0, 440.0);
    double m_1500 = goertzel_mag(pcm, 48000, 1, 0, 1500.0);
    TEST_ASSERT(m_440 > 5500.0, "mono mix must contain local 440 Hz");
    TEST_ASSERT(m_1500 > 5500.0, "mono mix must contain remote 1500 Hz");

    // Verify local mute suppresses left channel while keeping remote right channel
    const std::string mute_path = "/tmp/sipsdk_test_mute.wav";
    std::remove(mute_path.c_str());
    TEST_ASSERT(rec.start_recording(kLeg, mute_path, sip_core::RecordingMode::AudioStereo) == 0,
                "start muted stereo recording must succeed");
    rec.set_local_muted(kLeg, true);
    for (int b = 0; b < 10; ++b) {
        auto tx = make_sine(440.0, 48000, 1, 960, 7000.0, static_cast<size_t>(b) * 960);
        auto rx = make_sine(1500.0, 48000, 1, 960, 7000.0, static_cast<size_t>(b) * 960);
        rec.on_encode(kLeg, tx.data(), tx.size(), 48000, 1);
        rec.on_decode(kLeg, rx.data(), rx.size(), 48000, 1);
    }
    TEST_ASSERT(rec.stop_recording(kLeg) == 0, "stop muted recording must succeed");
    auto mbytes = read_file_bytes(mute_path);
    uint32_t mdata_sz = read_le32(mbytes.data() + 40);
    std::vector<int16_t> mpcm(mdata_sz / 2);
    std::memcpy(mpcm.data(), mbytes.data() + 44, mdata_sz);
    TEST_ASSERT(goertzel_mag(mpcm, 48000, 2, 0, 440.0) < 1.0, "muted local channel must be silent");
    TEST_ASSERT(goertzel_mag(mpcm, 48000, 2, 1, 1500.0) > 5500.0, "remote channel must stay recorded while muted");
    std::remove(mute_path.c_str());

    std::remove(path.c_str());
    std::printf("PASS: mono WAV mix (440=%.0f, 1500=%.0f)\n", m_440, m_1500);
}

void test_portable_mkv_video_and_audio()
{
    const std::string path = "/tmp/sipsdk_test_av.mkv";
    std::remove(path.c_str());

    sip_core::CallRecorder rec;
    constexpr uint64_t kLeg = 99;
    TEST_ASSERT(rec.start_recording(kLeg, path, sip_core::RecordingMode::AudioVideo, 64, 48) == 0,
                "start AudioVideo MKV recording must succeed");

    std::vector<uint8_t> y(64 * 48, 128);
    std::vector<uint8_t> u(32 * 24, 64);
    std::vector<uint8_t> v(32 * 24, 192);

    for (int f = 0; f < 5; ++f) {
        auto tx = make_sine(440.0, 48000, 1, 960, 5000.0, static_cast<size_t>(f) * 960);
        auto rx = make_sine(880.0, 48000, 1, 960, 5000.0, static_cast<size_t>(f) * 960);
        rec.on_encode(kLeg, tx.data(), tx.size(), 48000, 1);
        rec.on_decode(kLeg, rx.data(), rx.size(), 48000, 1);

        sip_core::RecordedVideoFrame vf;
        vf.width = 64;
        vf.height = 48;
        vf.y = y.data();
        vf.u = u.data();
        vf.v = v.data();
        vf.y_stride = 64;
        vf.u_stride = 32;
        vf.v_stride = 32;
        vf.timestamp_ns = static_cast<uint64_t>(f) * 33333333ULL;
        rec.on_video_frame(vf);
    }

    TEST_ASSERT(rec.stop_recording(kLeg) == 0, "stop MKV recording must succeed");

    auto bytes = read_file_bytes(path);
    TEST_ASSERT(bytes.size() > 20000, "MKV must contain header + audio + 5 I420 frames");
    // EBML magic 0x1A 0x45 0xDF 0xA3
    TEST_ASSERT(bytes[0] == 0x1A && bytes[1] == 0x45 && bytes[2] == 0xDF && bytes[3] == 0xA3,
                "MKV must begin with EBML header magic");
    std::string as_str(bytes.begin(), bytes.end());
    TEST_ASSERT(as_str.find("matroska") != std::string::npos, "MKV DocType must be matroska");
    TEST_ASSERT(as_str.find("A_PCM/INT/LIT") != std::string::npos, "MKV must declare PCM audio track");
    TEST_ASSERT(as_str.find("V_MS/VFW/FOURCC") != std::string::npos, "MKV must declare video track");

    std::remove(path.c_str());
    std::printf("PASS: portable MKV video+audio (%zu bytes)\n", bytes.size());
}

class MockRecorderDriver : public sip_core::IMediaRecorderDriver {
public:
    int start_calls{0};
    int stop_calls{0};
    size_t audio_samples{0};
    size_t video_frames{0};
    std::string last_path;

    int start(const char *path, int, int, int, int) override
    {
        ++start_calls;
        last_path = path ? path : "";
        return 0;
    }
    void write_audio(const int16_t *, size_t sample_count, uint64_t) override
    {
        audio_samples += sample_count;
    }
    void write_video(const sip_core::RecordedVideoFrame &) override
    {
        ++video_frames;
    }
    int stop() override
    {
        ++stop_calls;
        return 0;
    }
};

void test_pluggable_driver_and_validation()
{
    sip_core::CallRecorder rec;
    auto drv = std::make_shared<MockRecorderDriver>();
    rec.set_media_recorder_driver(drv);

    TEST_ASSERT(rec.start_recording(0, "/tmp/x.mp4", sip_core::RecordingMode::AudioVideo) == EINVAL,
                "leg_id=0 must return EINVAL");
    TEST_ASSERT(rec.start_recording(1, "", sip_core::RecordingMode::AudioVideo) == EINVAL,
                "empty path must return EINVAL");

    TEST_ASSERT(rec.start_recording(5, "/tmp/mock.mp4", sip_core::RecordingMode::AudioVideo, 320, 240) == 0,
                "driver start must succeed");
    TEST_ASSERT(drv->start_calls == 1, "driver start must be called once");

    auto tx = make_sine(440.0, 48000, 1, 960, 4000.0, 0);
    auto rx = make_sine(880.0, 48000, 1, 960, 4000.0, 0);
    rec.on_encode(5, tx.data(), tx.size(), 48000, 1);
    rec.on_decode(5, rx.data(), rx.size(), 48000, 1);

    std::vector<uint8_t> y(16 * 16, 100), u(8 * 8, 128), v(8 * 8, 128);
    sip_core::RecordedVideoFrame vf{16, 16, y.data(), u.data(), v.data(), 16, 8, 8, 1000000ULL};
    rec.on_video_frame(vf);

    rec.stop_all();
    TEST_ASSERT(drv->stop_calls == 1, "stop_all must stop driver");
    TEST_ASSERT(drv->audio_samples >= 1920, "driver must receive stereo PCM samples");
    TEST_ASSERT(drv->video_frames == 1, "driver must receive video frame");

    std::printf("PASS: pluggable driver + validation\n");
}

} // namespace

int main()
{
    test_stereo_wav_mixed_rates_and_isolation();
    test_mono_wav_mix_and_mute();
    test_portable_mkv_video_and_audio();
    test_pluggable_driver_and_validation();
    std::printf("ALL CALL RECORDER TESTS PASSED\n");
    return 0;
}
