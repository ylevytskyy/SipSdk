# SipSdk for the web

Browser SIP calling with audio and H.264 video. Signaling runs over SIP WebSocket
(`ws://` or `wss://`), media over WebRTC. The API follows the other SipSdk platforms:
a `SipClient` owns the account, a `SipCall` is one call, and the state values match
`core/include/sip_abi.h`.

The browser cannot open raw SIP sockets, so this SDK does not use the C++ core. It is
built on [JsSIP](https://jssip.net) 3.13.9 (vendored at `web/jssip-3.13.9.min.js`). The
server must speak SIP over WebSocket and WebRTC media (DTLS-SRTP, ICE), as the local
Asterisk extension 1002 does.

## Use

```html
<script src="jssip-3.13.9.min.js"></script>
<script type="module">
  import { SipClient, SipCallState, SipRegistrationState } from './sipsdk.js';

  const client = new SipClient(); // reads globalThis.JsSIP, or pass { jssip }
  client.start();

  client.on('registrationStateChanged', ({ state, code, reason }) => { /* ... */ });
  client.on('incomingCall', ({ call }) => {
    call.attachMedia({ localVideo, remoteVideo, remoteAudio });
    call.answer({ video: true });          // or { video: false }, or call.hangup(486)
  });

  client.register({ username: '1002', domain: 'pbx.local', password: '...',
                    wsUri: 'ws://pbx.local:8088/ws' });

  const call = client.makeCall('9001', { video: true });
  call.attachMedia({ localVideo, remoteVideo, remoteAudio });
  call.on('stateChanged', ({ state, code, reason }) => { /* SipCallState */ });
</script>
```

| `SipClient` | |
|---|---|
| `start()` / `stop()` | Start the client; `stop()` hangs up every call and closes the WebSocket. |
| `register({username, domain, password, wsUri, displayName, iceServers})` | Register over SIP WebSocket. |
| `unregister()` | Remove all bindings. |
| `makeCall(target, {video})` | `target` is an extension, `user@host`, or a `sip:` URI. Returns a `SipCall`. |
| `isRegistered`, `accountAor`, `calls` | State. |
| events | `registrationStateChanged {state, code, reason}`, `incomingCall {call}` |

| `SipCall` | |
|---|---|
| `answer({video})` | Answer an incoming call, with or without video. |
| `hangup(statusCode?, reason?)` | CANCEL, BYE, or decline with a code (486, 603). |
| `setHold(bool)`, `setMuted(bool)`, `setLocalVideoEnabled(bool)` | In-call controls. |
| `sendDtmf(digits)` | Sends DTMF as SIP INFO. |
| `attachMedia({localVideo, remoteVideo, remoteAudio})` | Bind media elements. |
| `state`, `duration`, `remoteUri`, `peerDisplayName`, `hasVideo`, `isOutgoing`, `isOnHold`, `isMuted`, `isClosed` | State. |
| events | `stateChanged {state, code, reason}`, `mediaChanged {width, height, isOnHold, isMuted}`, `dtmfReceived {digit}` |

## Sample

Serve the repo root and open the sample. It defaults to the local Asterisk (extension 1002):

```sh
python3 -m http.server 5102
open http://localhost:5102/platforms/web/sample/
```

Browsers allow camera and microphone only on `localhost` or HTTPS pages.

## Verify

```sh
bash scripts/verify-web-sdk.sh          # unit tests + headless Chrome with a scripted SIP peer
bash scripts/verify-web-sdk.sh --pbx    # also a live call to the local Asterisk 9001 echo
```

Not supported yet: camera switching, push notifications, SIP over TLS certificates setup.
