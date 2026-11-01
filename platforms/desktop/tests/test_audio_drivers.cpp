#include <sip/desktop.hpp>
#include <sip/audio_driver.hpp>

#include <cassert>
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

int main() {
    std::printf("=================================================================\n");
    std::printf("Running Desktop Audio Driver Tests\n");
    std::printf("=================================================================\n");

    // Test 1: Supported Backends List
    std::printf("--- Test 1: Supported Audio Backends ---\n");
    auto backends = sip::AudioDriverManager::get_supported_backends();
    TEST_ASSERT(!backends.empty(), "Supported backends list must not be empty");

    bool has_auto = false;
    for (auto b : backends) {
        if (b == sip::AudioBackend::Auto) has_auto = true;
        std::printf("  Supported backend: %s\n", sip::AudioDriverManager::get_backend_name(b).c_str());
    }
    TEST_ASSERT(has_auto, "Auto backend must be present in supported backends");
    std::printf("PASS: Supported audio backends queried successfully\n");

    // Test 2: Name Conversion
    std::printf("--- Test 2: Audio Backend Name Mapping ---\n");
    TEST_ASSERT(sip::AudioDriverManager::get_backend_from_name("pulse") == sip::AudioBackend::PulseAudio, "pulse mapping failed");
    TEST_ASSERT(sip::AudioDriverManager::get_backend_from_name("alsa") == sip::AudioBackend::Alsa, "alsa mapping failed");
    TEST_ASSERT(sip::AudioDriverManager::get_backend_from_name("audiounit") == sip::AudioBackend::CoreAudio, "audiounit mapping failed");
    TEST_ASSERT(sip::AudioDriverManager::get_backend_from_name("dummy") == sip::AudioBackend::Dummy, "dummy mapping failed");
    std::printf("PASS: Backend name conversion verified\n");

    // Test 3: Client Integration with Audio Backend Configuration
    std::printf("--- Test 3: Client Audio Backend Configuration ---\n");
    auto client = std::make_shared<sip::Client>("1001", "127.0.0.1");

#if defined(__APPLE__)
    int rc = client->set_audio_driver(sip::AudioBackend::CoreAudio);
    TEST_ASSERT(rc == 0, "set_audio_driver CoreAudio failed");
    TEST_ASSERT(client->get_audio_driver() == sip::AudioBackend::CoreAudio, "Audio driver mismatch");
#else
    int rc = client->set_audio_driver(sip::AudioBackend::PulseAudio);
    TEST_ASSERT(rc == 0, "set_audio_driver PulseAudio failed");
    TEST_ASSERT(client->get_audio_driver() == sip::AudioBackend::PulseAudio, "Audio driver mismatch");
#endif

    rc = client->start();
    TEST_ASSERT(rc == 0, "client->start() with configured audio driver failed");
    client->stop();
    std::printf("PASS: Client initialized cleanly with audio driver\n");

    std::printf("=================================================================\n");
    std::printf("ALL AUDIO DRIVER TESTS PASSED\n");
    std::printf("=================================================================\n");
    return 0;
}
