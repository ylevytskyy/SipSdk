#ifndef SIP_CORE_ACCOUNT_SESSION_H
#define SIP_CORE_ACCOUNT_SESSION_H

#include "sip_abi.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct ua;

namespace sip_core {
class Engine;
struct AccountConfig;
}

struct sip_account_t {
public:
    sip_account_t(sip_client_t *owner, sip_core::Engine *engine, uint64_t id,
                  std::string aor, std::string user, std::string domain, std::string transport);
    ~sip_account_t();

    sip_account_t(const sip_account_t &) = delete;
    sip_account_t &operator=(const sip_account_t &) = delete;

    uint64_t get_id() const { return m_id; }
    const char *get_aor() const { return m_aor.c_str(); }
    const char *get_user() const { return m_user.c_str(); }
    const char *get_domain() const { return m_domain.c_str(); }
    const std::string &aor_str() const { return m_aor; }
    const std::string &transport_str() const { return m_transport; }

    bool is_registered() const;
    sip_reg_state_t get_reg_state() const { return m_reg_state.load(); }
    void set_reg_state(sip_reg_state_t state);

    bool is_removed() const { return m_removed.load(); }
    void mark_removed() { m_removed.store(true); }

    void *get_user_data() const;
    void set_user_data(void *data);

    void attach_raw_ua(struct ua *u);
    struct ua *raw_ua() const;
    struct ua *detach_raw_ua();

    void retain();
    void release();

    sip_client_t *owner() const { return m_owner; }
    sip_core::Engine *engine() const { return m_engine; }

private:
    sip_client_t *m_owner{nullptr};
    sip_core::Engine *m_engine{nullptr};
    uint64_t m_id{0};
    std::string m_aor;
    std::string m_user;
    std::string m_domain;
    std::string m_transport;

    mutable std::mutex m_mutex;
    struct ua *m_raw_ua{nullptr};
    void *m_user_data{nullptr};

    std::atomic<sip_reg_state_t> m_reg_state{SIP_REG_STATE_UNREGISTERED};
    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_removed{false};
    std::atomic<int> m_ref_count{1};
};

#endif // SIP_CORE_ACCOUNT_SESSION_H
