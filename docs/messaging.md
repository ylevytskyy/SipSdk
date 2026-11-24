# Text messaging

SipSdk sends and receives pager-mode text messages with SIP MESSAGE (RFC 3428).
Messages travel outside of calls. They use the registered account and the same
SIP transport as registration.

## How it works

- **Send.** `sip_client_send_message(client, peer_uri, content_type, body, body_len, &message_id)`
  queues a SIP MESSAGE and returns a local `message_id` (1, 2, 3, ...). The server
  or peer answers later. `on_message_status` then reports `SIP_MESSAGE_STATUS_SENT`
  for a 2xx answer, or `SIP_MESSAGE_STATUS_FAILED` with the SIP code (e.g. 404, 415)
  or code 0 when no answer came (timeout, no account).
- **Receive.** The core listens for MESSAGE requests to the registered account,
  answers `200 OK`, and calls `on_message_received(from_uri, content_type, body, body_len)`.
  Any content type is accepted and passed through unchanged. Retransmissions of the
  same request are absorbed, so a lost `200 OK` does not deliver a message twice.
  The sender URI is not authenticated (no SIP TLS in this release): treat it like
  caller ID, and escape bodies before showing them as HTML.
- **Callbacks.** Set them with `sip_client_set_message_callbacks`. They run through
  `dispatch_to_main` like the call callbacks. The messaging callbacks are a
  separate struct, so `sip_callbacks_t` keeps its layout for existing bindings.
- **Input checks.** The URI and content type must not contain control characters
  (no header injection), the content type must be `type/subtype`, and the body must
  have 1 to `SIP_MESSAGE_MAX_BODY_SIZE` (64 KiB) bytes. Bodies above about 1300 bytes
  should use TCP: RFC 3428 limits MESSAGE over UDP.

| Platform | Send | Receive / status |
|---|---|---|
| Core C ABI | `sip_client_send_message` | `sip_message_callbacks_t` |
| iOS (`SipClient`) | `sendMessage(to:text:)`, `sendMessage(to:body:contentType:)` | `events`: `.messageReceived`, `.messageStatusChanged` |
| macOS bridge (`SipMacClient`) | `sendMessageToUri:text:`, `sendMessageToUri:body:contentType:` | `SipMacClientDelegate` message methods |
| Android (`SipClient`) | `sendMessage(target, text)`, `sendMessage(target, body, contentType)` | `SipClientListener.onMessageReceived`, `onMessageStatusChanged` |
| Desktop C++ (`sip::Client`) | `send_message(uri, body, content_type, &id)` | `IClientListener::on_message_received`, `on_message_status` |
| Windows .NET (`SipClient`) | `SendMessage(uri, text)`, `SendMessage(uri, body, contentType)` | `MessageReceived`, `MessageStatusChanged` events |
| Web (`SipClient`) | `sendMessage(target, body, { contentType })` | `messageReceived`, `messageStatusChanged` events |

## Local PBX

`docker/asterisk` routes MESSAGE through the `messages` dialplan context
(`message_context=messages` on every endpoint). It forks a message to every
registered contact of the target extension (1001-1005). Asterisk relays only
`text/plain`; it answers `415 Unsupported Media Type` for other content types.
A message to an unknown extension gets `404 Not Found`.

## Room for future extensions

The API was shaped so these can be added without breaking it.

## Future extension: delivery and read receipts

- Receipts are IMDN (RFC 5438): the sender adds `Imdn-Message-ID` and
  `Disposition-Notification: positive-delivery, display` to the request, and the
  peer answers with a MESSAGE whose body has content type `message/imdn+xml`.
- The status enum already reserves `SIP_MESSAGE_STATUS_DELIVERED` and
  `SIP_MESSAGE_STATUS_DISPLAYED`, and every binding maps them. A receipt will be
  reported through the existing `on_message_status` callback for the same
  `message_id`, so apps that handle status today keep working.
- Needed later: an options struct for `sip_client_send_message` (or a new
  `sip_client_send_message_ex`) to ask for receipts and add headers, a map from
  `Imdn-Message-ID` to the local id, and a "mark as displayed" call.

## Future extension: rich content types

- Every send already takes a MIME content type, and every receive reports it. The
  body is bytes, not text, in the core, iOS, Android, desktop and .NET APIs.
- Asterisk relays only `text/plain`. Rich content (JSON, images, files) needs a
  proxy that relays any type, or direct peer-to-peer MESSAGE. The core test proves
  that `application/json` is sent with its own `Content-Type` header.
- Large files do not fit in one MESSAGE (64 KiB cap; UDP limits). They need either
  an upload link sent as a small JSON or `message/external-body` message, or an
  MSRP session (see the next section).

## Future extension: group chat sessions

- `peer_uri` can already name a group or conference URI if the server expands it
  (for example an Asterisk dialplan that forks to members).
- Real chat sessions (MSRP, RFC 4975, or Ad-Hoc lists, RFC 5365) need a session
  handle like `sip_call_t`. Plan: a new `sip_chat_t` type with its own create,
  send and close calls, and its own callbacks struct beside
  `sip_message_callbacks_t`. Pager-mode messaging stays as it is.

## Future extension: typing indicators

- Typing state is isComposing (RFC 3994): a MESSAGE with content type
  `application/im-iscomposing+xml` and a small XML body (`<state>active</state>`
  or `idle`, plus `<refresh>`).
- This already works with the current API: send it with that content type, and the
  receiver gets it through `on_message_received` with that content type.
- Needed later: helper calls (`setComposing(peer, active)`) that build the XML,
  throttle refreshes, and turn incoming isComposing bodies into a typed
  "peer is typing" event instead of a plain message.
