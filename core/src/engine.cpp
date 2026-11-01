#include "engine.h"

#include <re.h>
#include <baresip.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace sip_core {

static void ua_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
    (void)ev;
    (void)event;
    (void)arg;
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

    ua_close();
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    m_started = false;
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
