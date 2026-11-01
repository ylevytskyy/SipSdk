#ifndef SIP_CORE_ENGINE_H
#define SIP_CORE_ENGINE_H

#include "sip_abi.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct mqueue;

namespace sip_core {

class Engine {
public:
    Engine(sip_client_t *owner, const sip_callbacks_t *callbacks, void *user_data);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    int init(const char *config_path);
    void destroy();

    bool is_started() const { return m_started.load(); }
    sip_client_t *owner() const { return m_owner; }
    void *user_data() const { return m_user_data; }
    const sip_callbacks_t &callbacks() const { return m_callbacks; }

    /**
     * Dispatches a callable task onto the dedicated Baresip re_main worker loop.
     * Safe to invoke from any caller thread.
     */
    bool post_task(std::function<void()> task);

private:
    int init_on_loop_thread(const char *config_path);
    void run_loop(std::string config_path);
    void cleanup_on_loop_thread();

    static void mqueue_callback(int id, void *data, void *arg);

    sip_client_t *m_owner{nullptr};
    sip_callbacks_t m_callbacks{};
    void *m_user_data{nullptr};

    std::thread m_loop_thread;
    std::mutex m_mutex;
    std::condition_variable m_start_cv;

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopping{false};

    struct mqueue *m_mq{nullptr};
    bool m_events_registered{false};
    int m_start_result{0};
    bool m_start_signaled{false};
};

} // namespace sip_core

#endif // SIP_CORE_ENGINE_H
