#include "call_session.h"
#include "engine.h"

#include <re.h>
#include <baresip.h>

#include <chrono>
#include <cstring>

sip_call_t::sip_call_t(sip_client_t *owner, sip_core::Engine *engine, struct call *call,
                       const char *remote_uri, bool is_outgoing, bool with_video)
    : m_owner(owner)
    , m_engine(engine)
    , m_raw_call(call)
    , m_is_outgoing(is_outgoing)
    , m_has_video(with_video)
{
    std::string id;
    std::string peer_display_name;
    std::string peer_uri;
    if (call != nullptr) {
        const char *cid = call_id(call);
        if (cid) {
            id = cid;
        }
        const char *peer_name = call_peername(call);
        if (peer_name) {
            peer_display_name = peer_name;
        }
        const char *uri = call_peeruri(call);
        if (uri) {
            peer_uri = uri;
        }
    }
    if (peer_uri.empty() && remote_uri) {
        peer_uri = remote_uri;
    }
    if (id.empty()) {
        id = "call_" + std::to_string(reinterpret_cast<uintptr_t>(this));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    publish_locked(m_id, id.c_str());
    publish_locked(m_peer_display_name, peer_display_name.c_str());
    publish_locked(m_remote_uri, peer_uri.c_str());
}

sip_call_t::~sip_call_t()
{
    m_raw_call = nullptr;
}

const char *sip_call_t::get_id() const
{
    return m_id.load(std::memory_order_acquire);
}

const char *sip_call_t::get_remote_uri() const
{
    return m_remote_uri.load(std::memory_order_acquire);
}

const char *sip_call_t::get_peer_display_name() const
{
    return m_peer_display_name.load(std::memory_order_acquire);
}

void sip_call_t::publish_locked(std::atomic<const char *> &slot, const char *value)
{
    const char *current = slot.load(std::memory_order_relaxed);
    if (current && std::strcmp(current, value) == 0) {
        return;
    }
    // Reuse an earlier string with the same value, so the list only grows with
    // distinct values.
    for (const std::string &published : m_published_strings) {
        if (published == value) {
            slot.store(published.c_str(), std::memory_order_release);
            return;
        }
    }
    m_published_strings.emplace_back(value);
    slot.store(m_published_strings.back().c_str(), std::memory_order_release);
}

bool sip_call_t::has_video() const
{
    return m_has_video;
}

bool sip_call_t::is_outgoing() const
{
    return m_is_outgoing;
}

bool sip_call_t::is_on_hold() const
{
    return m_on_hold.load();
}

bool sip_call_t::is_muted() const
{
    return m_is_muted.load();
}

bool sip_call_t::is_closed() const
{
    return m_closed.load();
}

uint32_t sip_call_t::get_duration() const
{
    if (m_closed.load()) {
        return m_duration.load();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_raw_call != nullptr) {
        uint32_t d = call_duration(m_raw_call);
        if (d > 0) {
            return d;
        }
    }
    if (m_is_established) {
        auto now = std::chrono::steady_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - m_established_at).count();
        return secs > 0 ? static_cast<uint32_t>(secs) : 0;
    }
    return m_duration.load();
}

void *sip_call_t::get_user_data() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user_data;
}

void sip_call_t::set_user_data(void *data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_user_data = data;
}

void sip_call_t::set_on_hold(bool hold)
{
    m_on_hold.store(hold);
}

void sip_call_t::set_muted(bool muted)
{
    m_is_muted.store(muted);
}

void sip_call_t::set_established()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_established = true;
    m_established_at = std::chrono::steady_clock::now();
}

void sip_call_t::set_closed(uint32_t dur)
{
    m_closed.store(true);
    if (dur > 0) {
        m_duration.store(dur);
    } else {
        m_duration.store(get_duration());
    }
}

void sip_call_t::attach_raw_call(struct call *c)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_raw_call = c;
    }
    if (c != nullptr) {
        update_identity(call_id(c), call_peername(c), call_peeruri(c));
    }
}

void sip_call_t::update_identity(const char *cid, const char *peer_name, const char *peer_uri)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cid && std::strlen(cid) > 0) {
        publish_locked(m_id, cid);
    }
    if (peer_name && std::strlen(peer_name) > 0) {
        publish_locked(m_peer_display_name, peer_name);
    }
    if (peer_uri && std::strlen(peer_uri) > 0) {
        publish_locked(m_remote_uri, peer_uri);
    }
}

void sip_call_t::retain()
{
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void sip_call_t::release()
{
    if (m_ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

void sip_call_t::detach_raw_call()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_raw_call = nullptr;
}

int sip_call_t::hangup(int code, const char *reason)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->hangup_call(this, code, reason);
}

int sip_call_t::answer(bool with_video)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->answer_call(this, with_video);
}

int sip_call_t::set_hold(bool hold)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->hold_call(this, hold);
}

int sip_call_t::set_call_muted(bool muted)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->mute_call(this, muted);
}

int sip_call_t::send_dtmf(const char *digits)
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->send_dtmf(this, digits);
}

int sip_call_t::request_keyframe()
{
    if (!m_engine) {
        return EINVAL;
    }
    return m_engine->request_keyframe(this);
}
