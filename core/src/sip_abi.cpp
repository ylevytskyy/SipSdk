#include "sip_abi.h"
#include "engine.h"
#include "call_session.h"

#include <cerrno>
#include <memory>
#include <new>

struct sip_client_t {
    std::unique_ptr<sip_core::Engine> engine;
};

extern "C" {

SIP_ABI_EXPORT sip_client_t *sip_client_create(const sip_callbacks_t *callbacks, void *user_data)
{
    auto *client = new (std::nothrow) sip_client_t();
    if (!client) {
        return nullptr;
    }
    client->engine = std::make_unique<sip_core::Engine>(client, callbacks, user_data);
    return client;
}

SIP_ABI_EXPORT int sip_client_init(sip_client_t *client, const char *config_path)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    return client->engine->init(config_path);
}

SIP_ABI_EXPORT int sip_client_register(sip_client_t *client,
                                      const char *user,
                                      const char *domain,
                                      const char *pwd,
                                      const char *transport,
                                      int port)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    return client->engine->register_account(user, domain, pwd, transport, port);
}

SIP_ABI_EXPORT int sip_client_unregister(sip_client_t *client)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    return client->engine->unregister_account();
}

SIP_ABI_EXPORT bool sip_client_is_registered(const sip_client_t *client)
{
    if (!client || !client->engine) {
        return false;
    }
    return client->engine->is_registered();
}

SIP_ABI_EXPORT void sip_client_destroy(sip_client_t *client)
{
    if (!client) {
        return;
    }
    if (client->engine) {
        client->engine->destroy();
        client->engine.reset();
    }
    delete client;
}

SIP_ABI_EXPORT sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video)
{
    if (!client || !client->engine) {
        return nullptr;
    }
    return client->engine->make_call(target_uri, with_video);
}

SIP_ABI_EXPORT int sip_call_answer(sip_call_t *call, bool with_video)
{
    if (!call) return EINVAL;
    return call->answer(with_video);
}

SIP_ABI_EXPORT int sip_call_hangup(sip_call_t *call, int code, const char *reason)
{
    if (!call) return EINVAL;
    return call->hangup(code, reason);
}

SIP_ABI_EXPORT int sip_call_set_hold(sip_call_t *call, bool hold)
{
    if (!call) return EINVAL;
    return call->set_hold(hold);
}

SIP_ABI_EXPORT int sip_call_set_muted(sip_call_t *call, bool muted)
{
    if (!call) return EINVAL;
    return call->set_call_muted(muted);
}

SIP_ABI_EXPORT int sip_call_send_dtmf(sip_call_t *call, const char *digits)
{
    if (!call) return EINVAL;
    return call->send_dtmf(digits);
}

SIP_ABI_EXPORT int sip_client_set_mic_muted(sip_client_t *client, bool muted)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->set_client_mic_muted(muted);
}

SIP_ABI_EXPORT bool sip_client_is_mic_muted(const sip_client_t *client)
{
    if (!client || !client->engine) return false;
    return client->engine->is_client_mic_muted();
}

SIP_ABI_EXPORT const char *sip_call_get_id(const sip_call_t *call)
{
    if (!call) return "";
    return call->get_id();
}

SIP_ABI_EXPORT const char *sip_call_get_remote_uri(const sip_call_t *call)
{
    if (!call) return "";
    return call->get_remote_uri();
}

SIP_ABI_EXPORT const char *sip_call_get_peer_display_name(const sip_call_t *call)
{
    if (!call) return "";
    return call->get_peer_display_name();
}

SIP_ABI_EXPORT bool sip_call_has_video(const sip_call_t *call)
{
    if (!call) return false;
    return call->has_video();
}

SIP_ABI_EXPORT bool sip_call_is_outgoing(const sip_call_t *call)
{
    if (!call) return false;
    return call->is_outgoing();
}

SIP_ABI_EXPORT bool sip_call_is_on_hold(const sip_call_t *call)
{
    if (!call) return false;
    return call->is_on_hold();
}

SIP_ABI_EXPORT bool sip_call_is_muted(const sip_call_t *call)
{
    if (!call) return false;
    return call->is_muted();
}

SIP_ABI_EXPORT bool sip_call_is_closed(const sip_call_t *call)
{
    if (!call) return true;
    return call->is_closed();
}

SIP_ABI_EXPORT uint32_t sip_call_get_duration(const sip_call_t *call)
{
    if (!call) return 0;
    return call->get_duration();
}

SIP_ABI_EXPORT void *sip_call_get_user_data(const sip_call_t *call)
{
    if (!call) return nullptr;
    return call->get_user_data();
}

SIP_ABI_EXPORT void sip_call_set_user_data(sip_call_t *call, void *user_data)
{
    if (!call) return;
    call->set_user_data(user_data);
}

SIP_ABI_EXPORT void sip_call_destroy(sip_call_t *call)
{
    if (!call) return;
    if (call->engine()) {
        call->engine()->destroy_call(call);
    } else {
        call->release();
    }
}

} // extern "C"
