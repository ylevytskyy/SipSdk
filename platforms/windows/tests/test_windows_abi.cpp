#include "sip_abi.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Static test verification hooks
static int g_reg_callback_count = 0;
static int g_call_callback_count = 0;

static void on_reg_state_test(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    (void)client;
    (void)state;
    (void)code;
    (void)reason;
    (void)user_data;
    g_reg_callback_count++;
}

static void on_call_state_test(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
{
    (void)call;
    (void)state;
    (void)code;
    (void)reason;
    (void)user_data;
    g_call_callback_count++;
}

int main()
{
    printf("==> Running Windows C ABI Export & Client Lifecycle Tests...\n");

    // 1. Verify struct initialization and client creation
    sip_callbacks_t callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_reg_state = on_reg_state_test;
    callbacks.on_call_state = on_call_state_test;

    int test_token = 42;
    sip_client_t *client = sip_client_create(&callbacks, &test_token);
    assert(client != nullptr);
    printf("PASS: sip_client_create succeeded\n");

    // 2. Verify initial client state
    assert(!sip_client_is_registered(client));
    assert(!sip_client_is_mic_muted(client));
    printf("PASS: Initial client state assertions passed\n");

    // 3. Verify microphone muting
    int mute_res = sip_client_set_mic_muted(client, true);
    assert(mute_res == 0);
    assert(sip_client_is_mic_muted(client));
    sip_client_set_mic_muted(client, false);
    assert(!sip_client_is_mic_muted(client));
    printf("PASS: Microphone mute toggle assertions passed\n");

    // 4. Verify preview state assertions
    assert(!sip_client_is_local_preview_active(client));

    // 5. Verify camera controls
    int cam_res = sip_client_set_camera(client, 1);
    assert(cam_res == 0);
    int switch_res = sip_client_switch_camera(client);
    assert(switch_res == 0);
    printf("PASS: Camera control assertions passed\n");

    // 6. Verify client destroy
    sip_client_destroy(client);
    printf("PASS: sip_client_destroy executed cleanly\n");

    // 7. Verify dynamic symbol resolution for all required C ABI functions
    printf("==> Verifying dynamic symbol exports...\n");
    const char *kExpectedSymbols[] = {
        "sip_client_create",
        "sip_client_init",
        "sip_client_register",
        "sip_client_unregister",
        "sip_client_is_registered",
        "sip_client_destroy",
        "sip_call_make",
        "sip_call_answer",
        "sip_call_hangup",
        "sip_call_set_hold",
        "sip_call_set_muted",
        "sip_call_send_dtmf",
        "sip_client_set_mic_muted",
        "sip_client_is_mic_muted",
        "sip_call_get_id",
        "sip_call_get_remote_uri",
        "sip_call_get_peer_display_name",
        "sip_call_has_video",
        "sip_call_is_outgoing",
        "sip_call_is_on_hold",
        "sip_call_is_muted",
        "sip_call_is_closed",
        "sip_call_get_duration",
        "sip_call_get_user_data",
        "sip_call_set_user_data",
        "sip_call_destroy",
        "sip_client_set_audio_driver",
        "sip_client_get_audio_driver",
        "sip_client_set_video_capture_driver",
        "sip_client_set_remote_renderer",
        "sip_client_set_local_renderer",
        "sip_client_set_video_codec_driver",
        "sip_call_request_keyframe",
        "sip_client_switch_camera",
        "sip_client_set_camera",
        "sip_client_set_local_video_enabled",
        "sip_client_is_local_video_enabled",
        "sip_client_start_local_preview",
        "sip_client_stop_local_preview",
        "sip_client_is_local_preview_active",
        nullptr
    };

#if defined(_WIN32) || defined(__CYGWIN__)
    HMODULE lib = LoadLibraryA("libsipcore.dll");
    if (!lib) {
        lib = LoadLibraryA("sipcore.dll");
    }
    assert(lib != nullptr);
    for (int i = 0; kExpectedSymbols[i] != nullptr; ++i) {
        FARPROC proc = GetProcAddress(lib, kExpectedSymbols[i]);
        if (!proc) {
            fprintf(stderr, "Missing Windows exported symbol: %s\n", kExpectedSymbols[i]);
            return 1;
        }
    }
    FreeLibrary(lib);
#else
    void *lib = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        lib = dlopen("core/libsipcore.dylib", RTLD_NOW | RTLD_GLOBAL);
    }
    for (int i = 0; kExpectedSymbols[i] != nullptr; ++i) {
        void *sym = dlsym(lib ? lib : RTLD_DEFAULT, kExpectedSymbols[i]);
        if (!sym) {
            fprintf(stderr, "Missing exported symbol: %s\n", kExpectedSymbols[i]);
            return 1;
        }
    }
    if (lib) dlclose(lib);
#endif

    printf("PASS: All 40 public C ABI symbols dynamically exported and resolvable!\n");
    printf("ALL_WINDOWS_ABI_TESTS_PASSED\n");
    return 0;
}
