# 0008. Multi-account `sip_account_t` handles with separate account callbacks

- **Status**: Accepted
- **Date**: 2026-11-22

`sip_core` represents each registered SIP identity as an opaque ref-counted `sip_account_t` handle owned by `sip_client_t` (up to `SIP_CLIENT_MAX_ACCOUNTS = 8`, each wrapping its own Baresip `struct ua *`), with a separate `sip_account_callbacks_t` struct registered via `sip_client_set_account_callbacks`. Keeping account callbacks out of `sip_callbacks_t` and `sip_message_callbacks_t` preserves binary layout for existing bindings, while keeping a designated default account preserves 100% backwards compatibility for single-account callers (`sip_client_register`, `sip_call_dial`, `sip_client_send_message`, `on_reg_state`).
