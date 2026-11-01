#ifndef SIP_DESKTOP_TYPES_HPP
#define SIP_DESKTOP_TYPES_HPP

#include <cstdint>
#include <string>

namespace sip {

enum class RegistrationState {
    Unregistered = 0,
    Registering,
    Registered,
    Failed
};

enum class CallState {
    Incoming = 0,
    Ringing,
    Connecting,
    Established,
    Hold,
    Closed
};

enum class AudioBackend {
    Auto = 0,
    PulseAudio,
    Alsa,
    CoreAudio,
    Dummy
};

struct CallMediaInfo {
    int width{0};
    int height{0};
    bool on_hold{false};
    bool is_muted{false};
};

inline const char *to_string(RegistrationState state) {
    switch (state) {
        case RegistrationState::Unregistered: return "Unregistered";
        case RegistrationState::Registering:  return "Registering";
        case RegistrationState::Registered:   return "Registered";
        case RegistrationState::Failed:       return "Failed";
    }
    return "Unknown";
}

inline const char *to_string(CallState state) {
    switch (state) {
        case CallState::Incoming:    return "Incoming";
        case CallState::Ringing:     return "Ringing";
        case CallState::Connecting:  return "Connecting";
        case CallState::Established: return "Established";
        case CallState::Hold:        return "Hold";
        case CallState::Closed:      return "Closed";
    }
    return "Unknown";
}

inline const char *to_string(AudioBackend backend) {
    switch (backend) {
        case AudioBackend::Auto:       return "auto";
        case AudioBackend::PulseAudio: return "pulse";
        case AudioBackend::Alsa:       return "alsa";
        case AudioBackend::CoreAudio:  return "audiounit";
        case AudioBackend::Dummy:      return "dummy";
    }
    return "unknown";
}

} // namespace sip

#endif // SIP_DESKTOP_TYPES_HPP
