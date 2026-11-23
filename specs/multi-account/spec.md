# Multi-Account Registration & Routing (`multi-account`)

Second milestone of Phase 3. Builds on Phase 2 (`specs/enterprise-telephony/spec.md`, PR #47) and
Local Audio Conference (`specs/local-audio-conference/spec.md`, PR #50).
VP8 / H.265 video codecs remain a separate follow-up spec.

## Scope

1. **Multi-Account Support in `core/` (`sip_abi.h`)**
   - A single `SipClient` (`sip_client_t`) hosts `1..8` concurrent SIP accounts (`sip_account_t`,
     capped at `SIP_CLIENT_MAX_ACCOUNTS = 8`), each backed by its own Baresip `struct ua *`.
   - **Account lifecycle API:**
     - `sip_client_add_account(client, config, &out_account)` — adds and registers a new account.
       Returns `EINVAL` on invalid args, `EEXIST` for a duplicate active AOR, `EBUSY` at the
       account cap, and `EIO` if the engine is not started. The first added account becomes the
       default account.
     - `sip_client_remove_account(client, account)` — hangs up active calls owned by `account`,
       unregisters and destroys its `struct ua *`, removes it from the client, and reassigns the
       default account to the first remaining account (if any).
     - `sip_account_reregister(account)` / `sip_account_unregister(account)` — per-account
       registration control without removing the account.
     - `sip_client_set_default_account(client, account)` / `sip_client_get_default_account(client)`
       — selects or queries the default account used by single-account convenience APIs.
     - `sip_client_get_account_count(client)` / `sip_client_get_accounts(client, out, max)` —
       enumerates active accounts on the client.
     - `sip_account_destroy(account)` — releases the caller's reference to a `sip_account_t`.
   - **Account inspection API:**
     - `sip_account_get_id`, `sip_account_get_aor`, `sip_account_get_user`,
       `sip_account_get_domain`, `sip_account_is_registered`, `sip_account_get_reg_state`,
       `sip_account_get_user_data`, `sip_account_set_user_data`.
   - **Per-account calling & messaging API:**
     - `sip_account_dial(account, target_uri, with_video, &out_call)` — dials an outgoing call
       from a specific account.
     - `sip_call_get_account(call)` — returns the `sip_account_t *` that owns the call (both
       outgoing and incoming calls).
     - `sip_account_send_message(account, peer_uri, content_type, body, body_len, &message_id)` —
       sends a SIP MESSAGE from a specific account.
   - **Separate account callbacks struct (`sip_account_callbacks_t`):**
     - `sip_client_set_account_callbacks(client, callbacks)` sets `on_account_reg_state` and
       `on_account_message_received` without changing the layout of `sip_callbacks_t` or
       `sip_message_callbacks_t` (ADR 0008).
   - **Cross-account call & transport semantics:**
     - Client-wide `max_calls` cap, auto-hold (ADR 0006), and local audio conference (ADR 0007)
       apply across all calls regardless of which account owns each leg.
     - `answer`, `hangup`, `accept_transfer`, `leave_conference`, and attended-transfer cleanup
       always operate on the call's own `struct ua *`, never a single global `m_ua`.
     - `sip_client_reset_transports(client, re_register)` re-registers every active account.
   - **Full backwards compatibility:**
     - Existing `sip_client_register` / `sip_client_register_account` registers or replaces the
       default account.
     - `sip_client_unregister`, `sip_client_is_registered`, `sip_call_dial`, `sip_call_make`,
       `sip_client_send_message`, and `on_reg_state` continue to operate on the default account.
     - `on_message_received` continues to fire for incoming messages on any account.

2. **Platform Bindings (All 6 Platforms)**
   - **Desktop C++ (`platforms/desktop`)**: `sip::Account` handle, `Client::add_account`,
     `remove_account`, `set_default_account`, `accounts`, `dial_call` on account, `send_message`
     on account, `Call::account()`, and `IClientListener::on_account_reg_state` /
     `on_account_message_received`.
   - **iOS (`platforms/ios`)**: `SipAccount` class, `SipClient.addAccount`, `removeAccount`,
     `setDefaultAccount`, `accounts`, `SipAccount.dial`, `SipAccount.sendMessage`, `SipCall.account`,
     and `SipEvent.accountRegistrationStateChanged` / `.accountMessageReceived`.
   - **macOS (`platforms/macos`)**: Multi-account bridge methods on `SipMacClient` / `SipMacAccount`.
   - **Android (`platforms/android`)**: `SipAccount` Kotlin wrapper over JNI (`sip_jni.cpp`),
     `SipClient.addAccount`, `removeAccount`, `setDefaultAccount`, `accounts`, `SipCall.account`,
     and `SipClientListener.onAccountRegistrationStateChanged` / `onAccountMessageReceived`.
   - **Windows .NET (`platforms/windows/dotnet`)**: P/Invoke bindings in `SipNativeAbi.cs`,
     `SipAccount` class, and `SipClient` multi-account methods/events.
   - **Web (`platforms/web/src/sipsdk.js`)**: Multi-UA registry inside `SipClient` (one JsSIP `UA`
     per `SipAccount`), matching the same add/remove/default/dial/message/call-attribution API.

## Definition of done

Each item is a check that can fail:
- **Core CTest (`sip_core_multi_account_test`) against Docker Asterisk (`1001` + `1003`):**
  - Registers `1001` and `1003` simultaneously on one `sip_client_t`; asserts both reach
    `SIP_REG_STATE_REGISTERED` via `on_account_reg_state` and `sip_client_get_account_count == 2`.
  - Dials `1003` from `1001` inside the same `sip_client_t`; asserts the incoming call is
    attributed to `1003` (`sip_call_get_account(incoming) == acc_1003`) and outgoing call to
    `1001`; answers and asserts both legs reach `ESTABLISHED`.
  - Dials `9001` on `1001` and `9002` on `1003`; asserts cross-account auto-hold holds the first
    call, and merging them into a local audio conference keeps both `ESTABLISHED`.
  - Sends a SIP MESSAGE from `1001` to `1003` inside the same `sip_client_t`; asserts
    `on_account_message_received` delivers it to `acc_1003`.
  - Switches default account (`sip_client_set_default_account`), removes an account
    (`sip_client_remove_account`), verifies default re-election, duplicate AOR rejection (`EEXIST`),
    and cap enforcement (`EBUSY` past 8 accounts).
  - Runs existing `sip_core_registration_test` and `sip_core_audio_call_test` with zero regressions.
- **All platform verify scripts pass with multi-account tests:**
  - `bash scripts/verify-desktop-sdk.sh`
  - `bash scripts/verify-ios-video-tests.sh`
  - `bash scripts/verify-macos-gui.sh`
  - `bash scripts/verify-android-sdk.sh`
  - `bash scripts/verify-windows-sdk.sh`
  - `bash scripts/verify-web-sdk.sh --pbx`

## Deliberately does NOT do
- **Per-account audio device routing or separate call caps per account.** Audio session and
  `max_calls` remain client-wide.
- **Rebuilding Baresip static prebuilts.** Baresip's `uag` already supports multiple `struct ua *`.
- **VP8 / H.265 codecs.** Separate Phase 3 spec.

## Next implementation step
Add `sip_account_t`, `sip_account_callbacks_t`, and multi-UA routing in `core/` with a failing
CTest (`sip_core_multi_account_test`) against Docker Asterisk before touching platform bindings.
