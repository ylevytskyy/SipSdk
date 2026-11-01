#include "engine.h"

#include <re.h>
#include <baresip.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace sip_core {

static void ua_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
    auto *engine = static_cast<Engine *>(arg);
    if (engine) {
        engine->handle_ua_event(static_cast<int>(ev), event);
    }
}

Engine::Engine(sip_client_t *owner, const sip_callbacks_t *callbacks, void *user_data)
    : m_owner(owner)
    , m_user_data(user_data)
{
    if (callbacks) {
        m_callbacks = *callbacks;
    }
}

Engine::~Engine()
{
    destroy();
}

int Engine::init(const char *config_path)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_started.load()) {
        return 0; // Already running
    }
    if (m_loop_thread.joinable()) {
        lock.unlock();
        destroy();
        lock.lock();
    }

    m_stopping = false;
    m_start_signaled = false;
    m_start_result = 0;

    std::string path_str = config_path ? config_path : "";

    m_loop_thread = std::thread([this, path = std::move(path_str)]() mutable {
        this->run_loop(std::move(path));
    });

    m_start_cv.wait(lock, [this]() {
        return m_start_signaled;
    });

    if (m_start_result != 0) {
        lock.unlock();
        if (m_loop_thread.joinable()) {
            m_loop_thread.join();
        }
        return m_start_result;
    }

    return 0;
}

void Engine::destroy()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stopping.exchange(true)) {
        return;
    }
    if (!m_loop_thread.joinable()) {
        m_started = false;
        m_stopping = false;
        return;
    }

    if (m_mq != nullptr) {
        (void)mqueue_push(m_mq, 1, nullptr);
    } else {
        re_cancel();
    }
    lock.unlock();

    if (m_loop_thread.joinable()) {
        m_loop_thread.join();
    }

    lock.lock();
    m_started = false;
    m_stopping = false;
}

bool Engine::post_task(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started.load() || m_stopping.load() || !m_mq) {
        return false;
    }

    auto *task_copy = new std::function<void()>(std::move(task));
    int rc = mqueue_push(m_mq, 2, task_copy);
    if (rc != 0) {
        delete task_copy;
        return false;
    }
    return true;
}

int Engine::init_on_loop_thread(const char *config_path)
{
    int rc = libre_init();
    if (rc != 0) {
        return rc;
    }

    if (config_path && std::strlen(config_path) > 0) {
        rc = conf_path_set(config_path);
        if (rc != 0) {
            libre_close();
            return rc;
        }
    }

    rc = conf_configure();
    if (rc != 0) {
        libre_close();
        return rc;
    }

    re_thread_async_init(4);

    rc = baresip_init(conf_config());
    if (rc != 0) {
        re_thread_async_close();
        libre_close();
        return rc;
    }

    if (baresip_network() != nullptr) {
        struct sa lo_addr;
        if (sa_set_str(&lo_addr, "127.0.0.1", 0) == 0 && !net_is_laddr(baresip_network(), &lo_addr)) {
            (void)net_add_address_ifname(baresip_network(), &lo_addr, "lo0");
        }
        struct sa dns_servers[2];
        if (sa_set_str(&dns_servers[0], "8.8.8.8", 53) == 0 &&
            sa_set_str(&dns_servers[1], "1.1.1.1", 53) == 0) {
            (void)net_use_nameserver(baresip_network(), dns_servers, 2);
        }
    }

    rc = ua_init("SipSdk", true, true, false);
    if (rc != 0) {
        baresip_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    rc = bevent_register(ua_event_handler, this);
    if (rc == 0) {
        m_events_registered = true;
    }

    rc = conf_modules();
    if (rc != 0) {
        if (m_events_registered) {
            bevent_unregister(ua_event_handler);
            m_events_registered = false;
        }
        ua_close();
        baresip_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    rc = mqueue_alloc(&m_mq, mqueue_callback, this);
    if (rc != 0) {
        if (m_events_registered) {
            bevent_unregister(ua_event_handler);
            m_events_registered = false;
        }
        module_app_unload();
        conf_close();
        ua_close();
        baresip_close();
        mod_close();
        re_thread_async_close();
        libre_close();
        return rc;
    }

    m_started = true;
    return 0;
}

void Engine::run_loop(std::string config_path)
{
    int rc = init_on_loop_thread(config_path.empty() ? nullptr : config_path.c_str());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_start_result = rc;
        m_start_signaled = true;
    }
    m_start_cv.notify_all();

    if (rc == 0) {
        (void)re_main(nullptr);
        cleanup_on_loop_thread();
    }
}

void Engine::cleanup_on_loop_thread()
{
    if (m_mq != nullptr) {
        m_mq = (struct mqueue *)mem_deref(m_mq);
    }

    if (m_events_registered) {
        bevent_unregister(ua_event_handler);
        m_events_registered = false;
    }

    if (m_ua != nullptr) {
        ua_destroy(m_ua);
        m_ua = nullptr;
    }

    ua_close();
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    m_started = false;
}

int Engine::register_account(const char *user, const char *domain, const char *pwd, const char *transport, int port)
{
    if (!user || std::strlen(user) == 0 ||
        !domain || std::strlen(domain) == 0 ||
        port <= 0 || port > 65535) {
        return EINVAL;
    }

    std::string user_str = user;
    std::string domain_str = domain;
    std::string pwd_str = pwd ? pwd : "";
    std::string transport_str = transport ? transport : "udp";
    int target_port = port;

    bool posted = post_task([this, user_str, domain_str, pwd_str, transport_str, target_port]() {
        if (m_ua != nullptr) {
            ua_destroy(m_ua);
            m_ua = nullptr;
        }

        std::string transport_param = "";
        std::string lower_transport = transport_str;
        for (auto &c : lower_transport) {
            c = (char)::tolower(c);
        }
        if (lower_transport == "tcp") {
            transport_param = ";transport=tcp";
        }

        std::string aor = "sip:" + user_str + "@" + domain_str + ":" + std::to_string(target_port) + transport_param;

        int rc = ua_alloc(&m_ua, aor.c_str());
        if (rc != 0 || m_ua == nullptr) {
            dispatch_reg_state(SIP_REG_STATE_FAILED, rc != 0 ? rc : EINVAL, "Failed to allocate UA");
            return;
        }

        struct account *acc = ua_account(m_ua);
        if (acc != nullptr) {
            (void)account_set_auth_user(acc, user_str.c_str());
            if (!pwd_str.empty()) {
                (void)account_set_auth_pass(acc, pwd_str.c_str());
            }
            (void)account_set_video_codecs(acc, "H264");
        }

        rc = ua_register(m_ua);
        if (rc != 0) {
            dispatch_reg_state(SIP_REG_STATE_FAILED, rc, "Registration submission failed");
        }
    });

    return posted ? 0 : EIO;
}

int Engine::unregister_account()
{
    bool posted = post_task([this]() {
        if (m_ua != nullptr) {
            ua_unregister(m_ua);
        } else {
            dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, 0, "Unregistered");
        }
    });
    return posted ? 0 : EIO;
}

bool Engine::is_registered() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ua != nullptr && ua_isregistered(m_ua);
}

void Engine::handle_ua_event(int ev, struct bevent *event)
{
    if (!event) {
        return;
    }

    const struct sip_msg *msg = bevent_get_msg(event);
    const char *text = bevent_get_text(event);

    auto extract_reason = [msg, text](const char *default_reason) -> std::string {
        if (msg && msg->reason.p && msg->reason.l > 0) {
            return std::string(msg->reason.p, msg->reason.l);
        }
        if (text && std::strlen(text) > 0) {
            return std::string(text);
        }
        return default_reason;
    };

    switch (ev) {
        case BEVENT_REGISTERING: {
            dispatch_reg_state(SIP_REG_STATE_REGISTERING, 0, extract_reason("Registering").c_str());
            break;
        }
        case BEVENT_REGISTER_OK: {
            int scode = msg ? static_cast<int>(msg->scode) : 200;
            dispatch_reg_state(SIP_REG_STATE_REGISTERED, scode, extract_reason("OK").c_str());
            break;
        }
        case BEVENT_REGISTER_FAIL: {
            int scode = msg ? static_cast<int>(msg->scode) : 401;
            dispatch_reg_state(SIP_REG_STATE_FAILED, scode, extract_reason("Registration Failed").c_str());
            break;
        }
        case BEVENT_UNREGISTERING: {
            int scode = msg ? static_cast<int>(msg->scode) : 0;
            dispatch_reg_state(SIP_REG_STATE_UNREGISTERED, scode, extract_reason("Unregistered").c_str());
            break;
        }
        default:
            break;
    }
}

struct RegStateTaskArgs {
    sip_client_t *client{nullptr};
    sip_reg_state_t state{SIP_REG_STATE_UNREGISTERED};
    int code{0};
    std::string reason;
    void (*on_reg_state)(sip_client_t *, sip_reg_state_t, int, const char *, void *){nullptr};
    void *user_data{nullptr};
};

static void run_reg_state_task(void *arg)
{
    auto *args = static_cast<RegStateTaskArgs *>(arg);
    if (args) {
        if (args->on_reg_state) {
            args->on_reg_state(args->client, args->state, args->code, args->reason.c_str(), args->user_data);
        }
        delete args;
    }
}

void Engine::dispatch_reg_state(sip_reg_state_t state, int code, const char *reason)
{
    if (!m_callbacks.on_reg_state) {
        return;
    }

    auto *args = new RegStateTaskArgs{
        m_owner,
        state,
        code,
        reason ? reason : "",
        m_callbacks.on_reg_state,
        m_user_data
    };

    if (m_callbacks.dispatch_to_main) {
        m_callbacks.dispatch_to_main(run_reg_state_task, args, m_user_data);
        return;
    }

#if defined(__APPLE__)
    dispatch_async_f(dispatch_get_main_queue(), args, run_reg_state_task);
#else
    run_reg_state_task(args);
#endif
}

void Engine::mqueue_callback(int id, void *data, void *arg)
{
    auto *engine = static_cast<Engine *>(arg);
    if (!engine) {
        return;
    }

    if (id == 1) {
        re_cancel();
    } else if (id == 2 && data != nullptr) {
        auto *task = static_cast<std::function<void()> *>(data);
        if (task && *task) {
            (*task)();
        }
        delete task;
    }
}

} // namespace sip_core
