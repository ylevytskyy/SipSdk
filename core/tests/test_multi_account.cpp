#include "sip_abi.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stderr);                                                         \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

namespace {

struct AccountRegEvent {
    sip_account_t *account{nullptr};
    sip_reg_state_t state{SIP_REG_STATE_UNREGISTERED};
    int code{0};
    std::string reason;
};

struct CallEvent {
    sip_call_t *call{nullptr};
    sip_call_state_t state{SIP_CALL_STATE_CLOSED};
    int code{0};
};

struct AccountMessageEvent {
    sip_account_t *account{nullptr};
    std::string from;
    std::string content_type;
    std::string body;
};

struct MsgStatusEvent {
    uint64_t id{0};
    sip_message_status_t status{SIP_MESSAGE_STATUS_FAILED};
    int code{0};
};

struct TestState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<AccountRegEvent> reg_events;
    std::vector<CallEvent> call_events;
    std::vector<sip_call_t *> incoming_calls;
    std::vector<AccountMessageEvent> account_messages;
    std::vector<MsgStatusEvent> msg_statuses;
};

static void inline_dispatch(void (*task)(void *arg), void *arg, void *user_data)
{
    (void)user_data;
    if (task) task(arg);
}

static void on_account_reg_state(sip_client_t *client, sip_account_t *account,
                                 sip_reg_state_t state, int code, const char *reason,
                                 void *user_data)
{
    (void)client;
    auto *st = static_cast<TestState *>(user_data);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->reg_events.push_back({account, state, code, reason ? reason : ""});
    }
    st->cv.notify_all();
}

static void on_account_message_received(sip_client_t *client, sip_account_t *account,
                                        const char *from_uri, const char *content_type,
                                        const uint8_t *body, size_t body_len,
                                        void *user_data)
{
    (void)client;
    auto *st = static_cast<TestState *>(user_data);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->account_messages.push_back({
            account,
            from_uri ? from_uri : "",
            content_type ? content_type : "",
            body ? std::string(reinterpret_cast<const char *>(body), body_len) : ""
        });
    }
    st->cv.notify_all();
}

static void on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data)
{
    (void)client;
    auto *st = static_cast<TestState *>(user_data);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->incoming_calls.push_back(call);
    }
    st->cv.notify_all();
}

static void on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
{
    (void)reason;
    auto *st = static_cast<TestState *>(user_data);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->call_events.push_back({call, state, code});
    }
    st->cv.notify_all();
}

static void on_message_status(sip_client_t *client, uint64_t message_id, sip_message_status_t status,
                              int code, const char *reason, void *user_data)
{
    (void)client;
    (void)reason;
    auto *st = static_cast<TestState *>(user_data);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->msg_statuses.push_back({message_id, status, code});
    }
    st->cv.notify_all();
}

static bool wait_for_account_reg(TestState &st, sip_account_t *acc, sip_reg_state_t target, int timeout_sec)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lk(st.mutex);
    return st.cv.wait_until(lk, deadline, [&]() {
        for (const auto &ev : st.reg_events) {
            if (ev.account == acc && ev.state == target) return true;
        }
        return false;
    });
}

static bool wait_for_call_state(TestState &st, sip_call_t *call, sip_call_state_t target, int timeout_sec)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lk(st.mutex);
    return st.cv.wait_until(lk, deadline, [&]() {
        for (const auto &ev : st.call_events) {
            if (ev.call == call && ev.state == target) return true;
        }
        return false;
    });
}

static sip_call_t *wait_for_incoming(TestState &st, int timeout_sec)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lk(st.mutex);
    if (!st.cv.wait_until(lk, deadline, [&]() { return !st.incoming_calls.empty(); })) {
        return nullptr;
    }
    return st.incoming_calls.back();
}

static bool wait_for_account_message(TestState &st, sip_account_t *acc, const std::string &expected_body, int timeout_sec)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lk(st.mutex);
    return st.cv.wait_until(lk, deadline, [&]() {
        for (const auto &m : st.account_messages) {
            if (m.account == acc && m.body == expected_body) return true;
        }
        return false;
    });
}

static bool wait_for_msg_status(TestState &st, uint64_t id, sip_message_status_t target, int timeout_sec)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::unique_lock<std::mutex> lk(st.mutex);
    return st.cv.wait_until(lk, deadline, [&]() {
        for (const auto &ev : st.msg_statuses) {
            if (ev.id == id && ev.status == target) return true;
        }
        return false;
    });
}

} // namespace

int main()
{
    const char *pbx_host_env = std::getenv("ASTERISK_HOST");
    std::string pbx_host = (pbx_host_env && pbx_host_env[0] != '\0') ? pbx_host_env : "127.0.0.1";
    const char *pbx_port_env = std::getenv("ASTERISK_PORT");
    int pbx_port = (pbx_port_env && pbx_port_env[0] != '\0') ? std::atoi(pbx_port_env) : 5060;

    std::printf("[multi_account_test] Using Asterisk at %s:%d\n", pbx_host.c_str(), pbx_port);

    TestState state;
    sip_callbacks_t cbs{};
    cbs.dispatch_to_main = inline_dispatch;
    cbs.on_incoming_call = on_incoming_call;
    cbs.on_call_state = on_call_state;

    sip_client_t *client = sip_client_create(&cbs, &state);
    CHECK(client != nullptr);
    CHECK(sip_client_init(client, nullptr) == 0);

    sip_account_callbacks_t acc_cbs{};
    acc_cbs.on_account_reg_state = on_account_reg_state;
    acc_cbs.on_account_message_received = on_account_message_received;
    CHECK(sip_client_set_account_callbacks(client, &acc_cbs) == 0);

    sip_message_callbacks_t msg_cbs{};
    msg_cbs.on_message_status = on_message_status;
    CHECK(sip_client_set_message_callbacks(client, &msg_cbs) == 0);

    // 1. Register two accounts simultaneously (1001 and 1003) on one client
    sip_account_config_t cfg1{};
    cfg1.user = "1001";
    cfg1.domain = pbx_host.c_str();
    cfg1.pwd = "test1001";
    cfg1.transport = "udp";
    cfg1.port = pbx_port;
    cfg1.max_calls = 4;

    sip_account_config_t cfg2{};
    cfg2.user = "1003";
    cfg2.domain = pbx_host.c_str();
    cfg2.pwd = "test1003";
    cfg2.transport = "udp";
    cfg2.port = pbx_port;
    cfg2.max_calls = 4;

    sip_account_t *acc1 = nullptr;
    sip_account_t *acc2 = nullptr;
    CHECK(sip_client_add_account(client, &cfg1, &acc1) == 0);
    CHECK(acc1 != nullptr);
    CHECK(sip_client_add_account(client, &cfg2, &acc2) == 0);
    CHECK(acc2 != nullptr);
    CHECK(sip_account_get_id(acc1) != sip_account_get_id(acc2));
    CHECK(sip_client_get_account_count(client) == 2);
    CHECK(sip_client_get_default_account(client) == acc1);

    // Duplicate AOR is rejected with EEXIST
    sip_account_t *dup = nullptr;
    CHECK(sip_client_add_account(client, &cfg1, &dup) == EEXIST);
    CHECK(dup == nullptr);

    CHECK(wait_for_account_reg(state, acc1, SIP_REG_STATE_REGISTERED, 10));
    CHECK(wait_for_account_reg(state, acc2, SIP_REG_STATE_REGISTERED, 10));
    CHECK(sip_account_is_registered(acc1));
    CHECK(sip_account_is_registered(acc2));
    std::printf("[multi_account_test] PASS: Simultaneous registration (1001 + 1003)\n");

    // 2. Per-account SIP MESSAGE: 1001 -> 1003 delivered to acc2
    std::string uri_1003 = "sip:1003@" + pbx_host + ":" + std::to_string(pbx_port);
    const std::string msg_body = "hello-multi-account";
    uint64_t msg_id = 0;
    CHECK(sip_account_send_message(acc1, uri_1003.c_str(), "text/plain",
                                   reinterpret_cast<const uint8_t *>(msg_body.data()),
                                   msg_body.size(), &msg_id) == 0);
    CHECK(msg_id > 0);
    CHECK(wait_for_msg_status(state, msg_id, SIP_MESSAGE_STATUS_SENT, 10));
    CHECK(wait_for_account_message(state, acc2, msg_body, 10));
    std::printf("[multi_account_test] PASS: Per-account SIP MESSAGE routing (1001 -> 1003)\n");

    // 3. Per-account dialing & incoming call attribution (1001 -> 1003 inside same client)
    sip_call_t *out_call = nullptr;
    CHECK(sip_account_dial(acc1, uri_1003.c_str(), false, &out_call) == 0);
    CHECK(out_call != nullptr);
    CHECK(sip_call_get_account(out_call) == acc1);

    sip_call_t *in_call = wait_for_incoming(state, 10);
    CHECK(in_call != nullptr);
    CHECK(sip_call_get_account(in_call) == acc2);
    CHECK(sip_call_hangup(in_call, 486, "Busy Here") == 0);
    CHECK(wait_for_call_state(state, out_call, SIP_CALL_STATE_CLOSED, 10));
    CHECK(wait_for_call_state(state, in_call, SIP_CALL_STATE_CLOSED, 10));
    sip_call_destroy(out_call);
    sip_call_destroy(in_call);
    std::printf("[multi_account_test] PASS: Incoming call attributed to receiving account (1003)\n");

    // 4. Cross-account auto-hold & local audio conference (acc1 -> 9001, acc2 -> 9001)
    std::string uri_9001 = "sip:9001@" + pbx_host + ":" + std::to_string(pbx_port);
    sip_call_t *c1 = nullptr;
    sip_call_t *c2 = nullptr;
    CHECK(sip_account_dial(acc1, uri_9001.c_str(), false, &c1) == 0);
    CHECK(wait_for_call_state(state, c1, SIP_CALL_STATE_ESTABLISHED, 10));
    CHECK(sip_account_dial(acc2, uri_9001.c_str(), false, &c2) == 0);
    CHECK(wait_for_call_state(state, c1, SIP_CALL_STATE_HOLD, 10));
    CHECK(wait_for_call_state(state, c2, SIP_CALL_STATE_ESTABLISHED, 10));

    CHECK(sip_client_merge_calls(client, c1, c2) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(sip_call_is_in_conference(c1));
    CHECK(sip_call_is_in_conference(c2));
    CHECK(sip_client_leave_conference(client) == 0);
    CHECK(wait_for_call_state(state, c1, SIP_CALL_STATE_CLOSED, 10));
    CHECK(wait_for_call_state(state, c2, SIP_CALL_STATE_CLOSED, 10));
    sip_call_destroy(c1);
    sip_call_destroy(c2);
    std::printf("[multi_account_test] PASS: Cross-account auto-hold & conference\n");

    // 5. Default account switching, per-account unregister, removal & default re-election
    CHECK(sip_client_set_default_account(client, acc2) == 0);
    CHECK(sip_client_get_default_account(client) == acc2);

    CHECK(sip_account_unregister(acc1) == 0);
    CHECK(wait_for_account_reg(state, acc1, SIP_REG_STATE_UNREGISTERED, 10));
    CHECK(!sip_account_is_registered(acc1));
    CHECK(sip_account_is_registered(acc2));

    CHECK(sip_client_remove_account(client, acc2) == 0);
    CHECK(sip_client_get_account_count(client) == 1);
    CHECK(sip_client_get_default_account(client) == acc1);
    sip_account_destroy(acc2);

    // 6. Cap enforcement (SIP_CLIENT_MAX_ACCOUNTS == 8)
    std::vector<sip_account_t *> extra_accounts;
    for (int i = 2; i <= SIP_CLIENT_MAX_ACCOUNTS; ++i) {
        std::string u = "capuser" + std::to_string(i);
        sip_account_config_t cap_cfg{};
        cap_cfg.user = u.c_str();
        cap_cfg.domain = "127.0.0.1";
        cap_cfg.pwd = "x";
        cap_cfg.transport = "udp";
        cap_cfg.port = 5099;
        sip_account_t *a = nullptr;
        CHECK(sip_client_add_account(client, &cap_cfg, &a) == 0);
        extra_accounts.push_back(a);
    }
    CHECK(sip_client_get_account_count(client) == SIP_CLIENT_MAX_ACCOUNTS);
    sip_account_config_t over_cfg{};
    over_cfg.user = "capuser9";
    over_cfg.domain = "127.0.0.1";
    over_cfg.pwd = "x";
    over_cfg.transport = "udp";
    over_cfg.port = 5099;
    sip_account_t *over = nullptr;
    CHECK(sip_client_add_account(client, &over_cfg, &over) == EBUSY);
    CHECK(over == nullptr);

    for (auto *a : extra_accounts) {
        CHECK(sip_client_remove_account(client, a) == 0);
        sip_account_destroy(a);
    }
    CHECK(sip_client_remove_account(client, acc1) == 0);
    sip_account_destroy(acc1);

    sip_client_destroy(client);
    std::printf("[multi_account_test] ALL TESTS PASSED\n");
    return 0;
}
