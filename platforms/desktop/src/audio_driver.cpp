#include <sip/audio_driver.hpp>
#include <sip_abi.h>

namespace sip {

std::string AudioDriverManager::get_backend_name(AudioBackend backend)
{
    switch (backend) {
        case AudioBackend::Auto:       return "auto";
        case AudioBackend::PulseAudio: return "pulse";
        case AudioBackend::Alsa:       return "alsa";
        case AudioBackend::CoreAudio:  return "audiounit";
        case AudioBackend::Dummy:      return "dummy";
    }
    return "auto";
}

AudioBackend AudioDriverManager::get_backend_from_name(const std::string &name)
{
    if (name == "pulse" || name == "pulseaudio") return AudioBackend::PulseAudio;
    if (name == "alsa") return AudioBackend::Alsa;
    if (name == "audiounit" || name == "coreaudio") return AudioBackend::CoreAudio;
    if (name == "dummy" || name == "none") return AudioBackend::Dummy;
    return AudioBackend::Auto;
}

std::vector<AudioBackend> AudioDriverManager::get_supported_backends()
{
    std::vector<AudioBackend> backends;
    backends.push_back(AudioBackend::Auto);
#if defined(__APPLE__)
    backends.push_back(AudioBackend::CoreAudio);
#else
    backends.push_back(AudioBackend::PulseAudio);
    backends.push_back(AudioBackend::Alsa);
#endif
    backends.push_back(AudioBackend::Dummy);
    return backends;
}

bool AudioDriverManager::is_backend_available(AudioBackend backend)
{
    auto supported = get_supported_backends();
    for (auto b : supported) {
        if (b == backend) return true;
    }
    return false;
}

int AudioDriverManager::apply_audio_backend(AudioBackend backend, const std::string &device)
{
    (void)backend;
    (void)device;
    return 0;
}

} // namespace sip
