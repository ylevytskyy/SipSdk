#ifndef SIP_CORE_CALL_SESSION_H
#define SIP_CORE_CALL_SESSION_H

#include "sip_abi.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

struct call;
struct tmr;

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
    bool is_secure() const;
    uint32_t get_duration() const;
    int get_stats(sip_call_stats_t *stats) const;

    void *get_user_data() const;
    void set_user_data(void *data);

    void set_on_hold(bool hold);
    void set_muted(bool muted);
    void set_established();
    void set_closed(uint32_t dur);
    void start_stats_timer();
    void stop_stats_timer();
    void start_hold_check_timer();
    void stop_hold_check_timer();
    bool is_hold_reinvite_pending() const { return m_hold_reinvite_pending.load(); }
    int increment_hold_check_ticks() { return ++m_hold_check_ticks; }
    void attach_raw_call(struct call *c);
    void detach_raw_call();

    void retain();
    void release();

    int hangup(int code, const char *reason);
    int answer(bool with_video);
    int set_hold(bool hold);
    int set_call_muted(bool muted);
    int send_dtmf(const char *digits);
    int request_keyframe();
    int transfer(const char *target_uri);
    int attended_transfer(sip_call_t *consult_call);
    int accept_transfer(bool accept);

    std::string pending_transfer_uri() const;
    void set_pending_transfer_uri(const std::string &uri);

    void set_consult_call(sip_call_t *consult_call);
    sip_call_t *take_consult_call();

    struct call *raw_call() const { return m_raw_call; }
    sip_client_t *owner() const { return m_owner; }
    sip_core::Engine *engine() const { return m_engine; }

private:
    sip_client_t *m_owner{nullptr};
    sip_core::Engine *m_engine{nullptr};
    struct call *m_raw_call{nullptr};

    std::string m_id;
    std::string m_remote_uri;
    std::string m_peer_display_name;

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
    std::string m_pending_transfer_uri;
    sip_call_t *m_consult_call{nullptr};

    static void stats_timer_handler(void *arg);
    struct tmr *m_stats_tmr{nullptr};
    bool m_stats_timer_running{false};
    sip_call_stats_t m_last_stats{};

    static void hold_check_timer_handler(void *arg);
    struct tmr *m_hold_check_tmr{nullptr};
    std::atomic<bool> m_hold_reinvite_pending{false};
    std::atomic<int> m_hold_check_ticks{0};
};

#endif // SIP_CORE_CALL_SESSION_H
