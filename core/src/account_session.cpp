#include "account_session.h"

#include <re.h>
#include <baresip.h>

#include <utility>

sip_account_t::sip_account_t(sip_client_t *owner, sip_core::Engine *engine, uint64_t id,
                             std::string aor, std::string user, std::string domain, std::string transport)
    : m_owner(owner)
    , m_engine(engine)
    , m_id(id)
    , m_aor(std::move(aor))
    , m_user(std::move(user))
    , m_domain(std::move(domain))
    , m_transport(std::move(transport))
{
}

sip_account_t::~sip_account_t()
{
    m_raw_ua = nullptr;
}

bool sip_account_t::is_registered() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_registered.load() && !m_removed.load() && m_raw_ua != nullptr && ua_isregistered(m_raw_ua);
}

void sip_account_t::set_reg_state(sip_reg_state_t state)
{
    m_reg_state.store(state);
    m_registered.store(state == SIP_REG_STATE_REGISTERED);
}

void *sip_account_t::get_user_data() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user_data;
}

void sip_account_t::set_user_data(void *data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_user_data = data;
}

void sip_account_t::attach_raw_ua(struct ua *u)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_raw_ua = u;
}

struct ua *sip_account_t::raw_ua() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_raw_ua;
}

struct ua *sip_account_t::detach_raw_ua()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    struct ua *u = m_raw_ua;
    m_raw_ua = nullptr;
    return u;
}

void sip_account_t::retain()
{
    m_ref_count.fetch_add(1, std::memory_order_relaxed);
}

void sip_account_t::release()
{
    if (m_ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}
