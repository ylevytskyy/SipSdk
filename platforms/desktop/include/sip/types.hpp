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

/** What a call has received. Complete once the call is closed. */
struct CallMediaStats {
    uint64_t video_frames_decoded{0};  // frames that left the video decoder (not frames sent)
    uint64_t video_rtp_received{0};    // RTP packets received on the video stream
    uint64_t audio_rtp_received{0};    // RTP packets received on the audio stream
};

/** Status of a sent SIP MESSAGE; values match sip_message_status_t. */
enum class MessageStatus {
    Sent = 0,
    Failed,
    Delivered,  // reserved: IMDN delivery notification
    Displayed   // reserved: IMDN display (read) notification
};

/** A SIP MESSAGE received for the registered account. */
struct IncomingMessage {
    std::string from;          // sender URI, e.g. "sip:1002@pbx"
    std::string content_type;  // MIME type, e.g. "text/plain"
    std::string body;          // raw body bytes
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

inline const char *to_string(MessageStatus status) {
    switch (status) {
        case MessageStatus::Sent:      return "Sent";
        case MessageStatus::Failed:    return "Failed";
        case MessageStatus::Delivered: return "Delivered";
        case MessageStatus::Displayed: return "Displayed";
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
