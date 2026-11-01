#ifndef SIP_ABI_H
#define SIP_ABI_H

#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef SIP_CORE_BUILDING_DLL
    #define SIP_ABI_EXPORT __declspec(dllexport)
  #else
    #define SIP_ABI_EXPORT __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define SIP_ABI_EXPORT __attribute__((visibility("default")))
  #else
    #define SIP_ABI_EXPORT
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sip_client_t sip_client_t;
typedef struct sip_call_t sip_call_t;

typedef enum {
    SIP_REG_STATE_UNREGISTERED = 0,
    SIP_REG_STATE_REGISTERING,
    SIP_REG_STATE_REGISTERED,
    SIP_REG_STATE_FAILED
} sip_reg_state_t;

typedef enum {
    SIP_CALL_STATE_INCOMING = 0,
    SIP_CALL_STATE_RINGING,
    SIP_CALL_STATE_CONNECTING,
    SIP_CALL_STATE_ESTABLISHED,
    SIP_CALL_STATE_HOLD,
    SIP_CALL_STATE_CLOSED
} sip_call_state_t;

typedef struct sip_callbacks_t {
    void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);
    void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);
    void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);
    void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);
    void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);
} sip_callbacks_t;

/**
 * Creates an uninitialized SIP client instance with the given callbacks.
 * user_data is stored and passed back to all callback invocations.
 */
SIP_ABI_EXPORT sip_client_t *sip_client_create(const sip_callbacks_t *callbacks, void *user_data);

/**
 * Initializes the SIP core stack and spawns the background worker event loop.
 * config_path: Path to directory containing Baresip configuration (or nullptr for default).
 * Returns 0 on success, or a positive error code (errno/libre error) on failure.
 */
SIP_ABI_EXPORT int sip_client_init(sip_client_t *client, const char *config_path);

/**
 * Shuts down the background worker event loop, releases all VoIP and networking resources,
 * and frees the sip_client_t instance. Safe to pass nullptr.
 */
SIP_ABI_EXPORT void sip_client_destroy(sip_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* SIP_ABI_H */
