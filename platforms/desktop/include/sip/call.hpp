#ifndef SIP_DESKTOP_CALL_HPP
#define SIP_DESKTOP_CALL_HPP

#include "types.hpp"
#include <cstdint>
#include <memory>
#include <string>

// Forward declaration of C ABI handle
struct sip_call_t;

namespace sip {

class Client;

class Call {
public:
    explicit Call(sip_call_t *handle);
    ~Call();

    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;
    Call(Call &&other) noexcept;
    Call &operator=(Call &&other) noexcept;

    sip_call_t *raw_handle() const { return m_handle; }

    std::string get_id() const;
    std::string get_remote_uri() const;
    std::string get_peer_display_name() const;
    bool has_video() const;
    bool is_outgoing() const;
    bool is_on_hold() const;
    bool is_muted() const;
    bool is_closed() const;
    bool is_secure() const;
    bool is_in_conference() const;
    bool is_recording() const;
    std::string get_recording_path() const;
    uint32_t get_duration() const;
    /** Counts of what the call received; complete once the call is closed. */
    CallMediaStats get_media_stats() const;
    CallStats get_stats() const;

    int answer(bool with_video = false);
    int hangup(int code = 0, const std::string &reason = "Hangup");
    int set_hold(bool hold);
    int set_muted(bool muted);
    int send_dtmf(const std::string &digits);
    int request_keyframe();
    int transfer(const std::string &target_uri);
    int attended_transfer(const std::shared_ptr<Call> &consult_call);
    int accept_transfer(bool accept);
    int start_recording(const std::string &path, RecordingMode mode = RecordingMode::AudioStereo);
    int stop_recording();

    void invalidate();

private:
    friend class Client;
    sip_call_t *m_handle{nullptr};
    bool m_owns_handle{false};
    std::string m_cached_id;
    std::string m_cached_remote_uri;
};

} // namespace sip

#endif // SIP_DESKTOP_CALL_HPP
