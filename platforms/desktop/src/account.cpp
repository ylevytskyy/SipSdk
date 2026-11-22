#include <sip/account.hpp>
#include <sip_abi.h>

#include <cerrno>

namespace sip {

Account::Account(sip_account_t *handle)
    : m_handle(handle)
{
    if (m_handle) {
        m_cached_id = sip_account_get_id(m_handle);
        const char *aor = sip_account_get_aor(m_handle);
        const char *user = sip_account_get_user(m_handle);
        const char *domain = sip_account_get_domain(m_handle);
        if (aor) m_cached_aor = aor;
        if (user) m_cached_user = user;
        if (domain) m_cached_domain = domain;
    }
}

Account::~Account()
{
    if (m_handle && m_owns_handle) {
        sip_account_destroy(m_handle);
    }
    m_handle = nullptr;
}

uint64_t Account::id() const
{
    return m_handle ? sip_account_get_id(m_handle) : m_cached_id;
}

std::string Account::aor() const
{
    if (m_handle) {
        const char *a = sip_account_get_aor(m_handle);
        if (a && a[0] != '\0') return a;
    }
    return m_cached_aor;
}

std::string Account::username() const
{
    if (m_handle) {
        const char *u = sip_account_get_user(m_handle);
        if (u && u[0] != '\0') return u;
    }
    return m_cached_user;
}

std::string Account::domain() const
{
    if (m_handle) {
        const char *d = sip_account_get_domain(m_handle);
        if (d && d[0] != '\0') return d;
    }
    return m_cached_domain;
}

bool Account::is_registered() const
{
    return m_handle ? sip_account_is_registered(m_handle) : false;
}

RegistrationState Account::reg_state() const
{
    if (!m_handle) return RegistrationState::Unregistered;
    return static_cast<RegistrationState>(sip_account_get_reg_state(m_handle));
}

int Account::reregister()
{
    return m_handle ? sip_account_reregister(m_handle) : EINVAL;
}

int Account::unregister()
{
    return m_handle ? sip_account_unregister(m_handle) : EINVAL;
}

void Account::invalidate()
{
    if (m_handle && m_owns_handle) {
        sip_account_destroy(m_handle);
    }
    m_handle = nullptr;
    m_owns_handle = false;
}

} // namespace sip
