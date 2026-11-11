#ifndef SIP_CORE_CALL_SESSION_H
#define SIP_CORE_CALL_SESSION_H

#include "sip_abi.h"

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>

struct call;

namespace sip_core {
class Engine;
}

struct sip_call_t {
public:
    sip_call_t(sip_client_t *owner, sip_core::Engine *engine, struct call *call,
               const char *remote_uri, bool is_outgoing, bool with_video);
    ~sip_call_t();

    sip_call_t(const sip_call_t &) = delete;
    sip_call_t &operator=(const sip_call_t &) = delete;

    const char *get_id() const;
    const char *get_remote_uri() const;
    const char *get_peer_display_name() const;
    bool has_video() const;
    bool is_outgoing() const;
    bool is_on_hold() const;
    bool is_muted() const;
    bool is_closed() const;
    uint32_t get_duration() const;

    void *get_user_data() const;
    void set_user_data(void *data);

    void set_on_hold(bool hold);
    void set_muted(bool muted);
    void set_established();
    void set_closed(uint32_t dur);
    void attach_raw_call(struct call *c);
    // Updates the call identity strings; empty or null values are ignored.
    void update_identity(const char *call_id, const char *peer_name, const char *peer_uri);
    void detach_raw_call();

    void retain();
    void release();

    int hangup(int code, const char *reason);
    int answer(bool with_video);
    int set_hold(bool hold);
    int set_call_muted(bool muted);
    int send_dtmf(const char *digits);
    int request_keyframe();

    struct call *raw_call() const { return m_raw_call; }
    sip_client_t *owner() const { return m_owner; }
    sip_core::Engine *engine() const { return m_engine; }

private:
    sip_client_t *m_owner{nullptr};
    sip_core::Engine *m_engine{nullptr};
    struct call *m_raw_call{nullptr};

    // Publishes value as the current string of slot. Requires m_mutex.
    void publish_locked(std::atomic<const char *> &slot, const char *value);

    // Strings handed out through the C ABI. A published string is never changed
    // or freed while the call exists, so pointers returned by get_id(),
    // get_remote_uri() and get_peer_display_name() stay valid until the call is
    // destroyed. An update publishes the new value; the list holds each distinct
    // value once. Guarded by m_mutex.
    std::list<std::string> m_published_strings;
    std::atomic<const char *> m_id{""};
    std::atomic<const char *> m_remote_uri{""};
    std::atomic<const char *> m_peer_display_name{""};

    bool m_is_outgoing{false};
    bool m_has_video{false};
    std::atomic<bool> m_on_hold{false};
    std::atomic<bool> m_is_muted{false};
    std::atomic<bool> m_closed{false};
    std::atomic<uint32_t> m_duration{0};
    std::atomic<int> m_ref_count{1};

    mutable std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_established_at{};
    bool m_is_established{false};
    void *m_user_data{nullptr};
};

#endif // SIP_CORE_CALL_SESSION_H
