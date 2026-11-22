#ifndef SIP_DESKTOP_ACCOUNT_HPP
#define SIP_DESKTOP_ACCOUNT_HPP

#include "types.hpp"

#include <cstdint>
#include <memory>
#include <string>

struct sip_account_t;

namespace sip {

class Client;

class Account {
public:
    explicit Account(sip_account_t *handle);
    ~Account();

    Account(const Account &) = delete;
    Account &operator=(const Account &) = delete;

    sip_account_t *raw_handle() const { return m_handle; }

    uint64_t id() const;
    std::string aor() const;
    std::string username() const;
    std::string domain() const;
    bool is_registered() const;
    RegistrationState reg_state() const;

    int reregister();
    int unregister();

    void invalidate();

private:
    friend class Client;
    sip_account_t *m_handle{nullptr};
    bool m_owns_handle{true};
    uint64_t m_cached_id{0};
    std::string m_cached_aor;
    std::string m_cached_user;
    std::string m_cached_domain;
};

} // namespace sip

#endif // SIP_DESKTOP_ACCOUNT_HPP
