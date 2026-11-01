#include "sip_abi.h"
#include "engine.h"

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

} // extern "C"
