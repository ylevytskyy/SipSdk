// Minimal stand-in for the JsSIP 3.x API surface the SDK uses. Tests drive it by
// emitting the same events JsSIP emits and by reading the calls it recorded.

class Emitter {
  constructor() { this.handlers = new Map(); }
  on(type, fn) {
    if (!this.handlers.has(type)) this.handlers.set(type, []);
    this.handlers.get(type).push(fn);
    return this;
  }
  emit(type, data) {
    for (const fn of [...(this.handlers.get(type) || [])]) fn(data);
  }
}

export class FakeSession extends Emitter {
  constructor({ direction, uri, displayName = '' }) {
    super();
    this.direction = direction;
    this.remote_identity = { uri: { toString: () => uri }, display_name: displayName };
    this.connection = null;
    this.answered = null;
    this.terminated = null;
    this.holdCalls = [];
    this.muteCalls = [];
    this.dtmfSent = [];
    this.referCalls = [];
    this.established = false;
    this.ended = false;
  }
  answer(options) { this.answered = options; }
  terminate(options = {}) { this.terminated = options; }
  hold() { this.holdCalls.push('hold'); return true; }
  unhold() { this.holdCalls.push('unhold'); return true; }
  mute(options) { this.muteCalls.push(['mute', options]); }
  unmute(options) { this.muteCalls.push(['unmute', options]); }
  sendDTMF(tones, options) { this.dtmfSent.push([tones, options]); }
  refer(target, options = {}) {
    if (!this.established) return false;
    this.referCalls.push([target, options]);
    return { id: this.referCalls.length };
  }
  isEstablished() { return this.established; }
  isEnded() { return this.ended; }
}

export function createFakeJsSIP() {
  const uas = [];
  class WebSocketInterface {
    constructor(url) { this.url = url; }
  }
  class UA extends Emitter {
    constructor(config) {
      super();
      this.config = config;
      this.started = false;
      this.stopped = false;
      this.unregisterCalls = [];
      this.outgoing = [];
      this.messages = [];
      uas.push(this);
    }
    sendMessage(target, body, options) {
      const message = { target, body, options };
      this.messages.push(message);
      return message;
    }
    start() { this.started = true; }
    stop() { this.stopped = true; }
    register() {}
    unregister(options) { this.unregisterCalls.push(options); }
    call(target, options) {
      const session = new FakeSession({ direction: 'outgoing', uri: target });
      session.callOptions = options;
      this.outgoing.push(session);
      return session;
    }
  }
  return { JsSIP: { UA, WebSocketInterface }, uas };
}
