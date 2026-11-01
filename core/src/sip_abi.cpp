#include "sip_abi.h"
#include "engine.h"
#include "call_session.h"
#include "media_driver.h"

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

SIP_ABI_EXPORT int sip_client_set_audio_driver(sip_client_t *client, const char *src_mod, const char *src_dev, const char *play_mod, const char *play_dev)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->set_audio_driver(src_mod, src_dev, play_mod, play_dev);
}

SIP_ABI_EXPORT int sip_client_get_audio_driver(const sip_client_t *client, char *src_mod, size_t src_mod_sz, char *play_mod, size_t play_mod_sz)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->get_audio_driver(src_mod, src_mod_sz, play_mod, play_mod_sz);
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

SIP_ABI_EXPORT int sip_client_set_video_capture_driver(sip_client_t *client, const sip_video_capture_driver_t *driver)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    if (!driver) {
        return client->engine->set_video_capture_driver(nullptr);
    }
    auto cpp_driver = std::make_shared<sip_core::CApiVideoCaptureDriver>(*driver);
    return client->engine->set_video_capture_driver(cpp_driver);
}

SIP_ABI_EXPORT int sip_client_set_remote_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    if (!renderer) {
        return client->engine->set_remote_renderer(nullptr);
    }
    auto cpp_renderer = std::make_shared<sip_core::CApiVideoRendererDriver>(*renderer);
    return client->engine->set_remote_renderer(cpp_renderer);
}

SIP_ABI_EXPORT int sip_client_set_local_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    if (!renderer) {
        return client->engine->set_local_renderer(nullptr);
    }
    auto cpp_renderer = std::make_shared<sip_core::CApiVideoRendererDriver>(*renderer);
    return client->engine->set_local_renderer(cpp_renderer);
}

SIP_ABI_EXPORT int sip_client_set_video_codec_driver(sip_client_t *client, const sip_video_codec_driver_t *driver)
{
    if (!client || !client->engine) {
        return EINVAL;
    }
    if (!driver) {
        return client->engine->set_video_codec_driver(nullptr);
    }
    auto cpp_codec = std::make_shared<sip_core::CApiVideoCodecDriver>(*driver);
    return client->engine->set_video_codec_driver(cpp_codec);
}

SIP_ABI_EXPORT int sip_call_request_keyframe(sip_call_t *call)
{
    if (!call) return EINVAL;
    return call->request_keyframe();
}

SIP_ABI_EXPORT int sip_client_switch_camera(sip_client_t *client)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->switch_camera();
}

SIP_ABI_EXPORT int sip_client_set_camera(sip_client_t *client, int position)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->set_camera(position);
}

SIP_ABI_EXPORT int sip_client_set_local_video_enabled(sip_client_t *client, bool enabled)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->set_local_video_enabled(enabled);
}

SIP_ABI_EXPORT bool sip_client_is_local_video_enabled(const sip_client_t *client)
{
    if (!client || !client->engine) return false;
    return client->engine->is_local_video_enabled();
}

SIP_ABI_EXPORT int sip_client_start_local_preview(sip_client_t *client)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->start_local_preview();
}

SIP_ABI_EXPORT int sip_client_stop_local_preview(sip_client_t *client)
{
    if (!client || !client->engine) return EINVAL;
    return client->engine->stop_local_preview();
}

SIP_ABI_EXPORT bool sip_client_is_local_preview_active(const sip_client_t *client)
{
    if (!client || !client->engine) return false;
    return client->engine->is_local_preview_active();
}

} // extern "C"
