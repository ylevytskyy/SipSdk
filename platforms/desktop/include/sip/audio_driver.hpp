#ifndef SIP_DESKTOP_AUDIO_DRIVER_HPP
#define SIP_DESKTOP_AUDIO_DRIVER_HPP

#include "types.hpp"
#include <string>
#include <vector>

namespace sip {

struct AudioDeviceInfo {
    std::string name;
    std::string driver;
    bool is_input{false};
    bool is_output{false};
};

class AudioDriverManager {
public:
    static bool is_backend_available(AudioBackend backend);
    static int apply_audio_backend(AudioBackend backend, const std::string &device = "");
    static std::string get_backend_name(AudioBackend backend);
    static AudioBackend get_backend_from_name(const std::string &name);
    static std::vector<AudioBackend> get_supported_backends();
};

} // namespace sip

#endif // SIP_DESKTOP_AUDIO_DRIVER_HPP
