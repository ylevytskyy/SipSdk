#include <sip/call.hpp>
#include <sip_abi.h>

namespace sip {

Call::Call(sip_call_t *handle)
    : m_handle(handle)
    , m_owns_handle(false)
{
    if (m_handle) {
        const char *id = sip_call_get_id(m_handle);
        if (id) m_cached_id = id;
        const char *uri = sip_call_get_remote_uri(m_handle);
        if (uri) m_cached_remote_uri = uri;
    }
}

Call::~Call()
{
}

Call::Call(Call &&other) noexcept
    : m_handle(other.m_handle)
    , m_owns_handle(other.m_owns_handle)
    , m_cached_id(std::move(other.m_cached_id))
    , m_cached_remote_uri(std::move(other.m_cached_remote_uri))
{
    other.m_handle = nullptr;
    other.m_owns_handle = false;
}

Call &Call::operator=(Call &&other) noexcept
{
    if (this != &other) {
        m_handle = other.m_handle;
        m_owns_handle = other.m_owns_handle;
        m_cached_id = std::move(other.m_cached_id);
        m_cached_remote_uri = std::move(other.m_cached_remote_uri);
        other.m_handle = nullptr;
        other.m_owns_handle = false;
    }
    return *this;
}

std::string Call::get_id() const
{
    if (!m_handle) return m_cached_id;
    const char *id = sip_call_get_id(m_handle);
    return id ? id : m_cached_id;
}

std::string Call::get_remote_uri() const
{
    if (!m_handle) return m_cached_remote_uri;
    const char *uri = sip_call_get_remote_uri(m_handle);
    return uri ? uri : m_cached_remote_uri;
}

std::string Call::get_peer_display_name() const
{
    if (!m_handle) return "";
    const char *name = sip_call_get_peer_display_name(m_handle);
    return name ? name : "";
}

bool Call::has_video() const
{
    return m_handle ? sip_call_has_video(m_handle) : false;
}

bool Call::is_outgoing() const
{
    return m_handle ? sip_call_is_outgoing(m_handle) : false;
}

bool Call::is_on_hold() const
{
    return m_handle ? sip_call_is_on_hold(m_handle) : false;
}

bool Call::is_muted() const
{
    return m_handle ? sip_call_is_muted(m_handle) : false;
}

bool Call::is_closed() const
{
    return m_handle ? sip_call_is_closed(m_handle) : true;
}

bool Call::is_secure() const
{
    return m_handle ? sip_call_is_secure(m_handle) : false;
}

uint32_t Call::get_duration() const
{
    return m_handle ? sip_call_get_duration(m_handle) : 0;
}

CallStats Call::get_stats() const
{
    CallStats out;
    if (!m_handle) return out;
    sip_call_stats_t raw{};
    if (sip_call_get_stats(m_handle, &raw) == 0) {
        out.audio_loss_rate = raw.audio_loss_rate;
        out.audio_jitter_ms = raw.audio_jitter_ms;
        out.audio_rtt_ms = raw.audio_rtt_ms;
        out.audio_bitrate_kbps = raw.audio_bitrate_kbps;
        out.audio_codec = raw.audio_codec;
        out.video_loss_rate = raw.video_loss_rate;
        out.video_jitter_ms = raw.video_jitter_ms;
        out.video_bitrate_kbps = raw.video_bitrate_kbps;
        out.video_fps = raw.video_fps;
        out.video_codec = raw.video_codec;
    }
    return out;
}

int Call::answer(bool with_video)
{
    if (!m_handle) return -1;
    return sip_call_answer(m_handle, with_video);
}

int Call::hangup(int code, const std::string &reason)
{
    if (!m_handle) return -1;
    return sip_call_hangup(m_handle, code, reason.empty() ? nullptr : reason.c_str());
}

int Call::set_hold(bool hold)
{
    if (!m_handle) return -1;
    return sip_call_set_hold(m_handle, hold);
}

int Call::set_muted(bool muted)
{
    if (!m_handle) return -1;
    return sip_call_set_muted(m_handle, muted);
}

int Call::send_dtmf(const std::string &digits)
{
    if (!m_handle) return -1;
    return sip_call_send_dtmf(m_handle, digits.c_str());
}

int Call::request_keyframe()
{
    if (!m_handle) return -1;
    return sip_call_request_keyframe(m_handle);
}

int Call::transfer(const std::string &target_uri)
{
    if (!m_handle) return -1;
    return sip_call_transfer(m_handle, target_uri.c_str());
}

int Call::attended_transfer(const std::shared_ptr<Call> &consult_call)
{
    if (!m_handle || !consult_call || !consult_call->m_handle) return -1;
    return sip_call_attended_transfer(m_handle, consult_call->m_handle);
}

int Call::accept_transfer(bool accept)
{
    if (!m_handle) return -1;
    return sip_call_accept_transfer(m_handle, accept);
}

void Call::invalidate()
{
    m_handle = nullptr;
    m_owns_handle = false;
}

} // namespace sip
