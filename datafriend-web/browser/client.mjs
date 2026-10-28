import createModule from './datafriend.mjs';

// One serialized WASM instance per session; browser stores/keys are ephemeral.
export async function createSession({url, onError = console.error} = {}) {
  const wasm = await createModule(url ? {websocket: {url, subprotocol: 'binary'}} : {});
  const call = (name, result, args = []) => wasm.cwrap(`dfb_${name}`, result, args);
  const error = call('error', 'string');
  const identity = call('identity', 'number');
  const key = call('key', 'string', ['number']);
  const open = call('open', 'number', ['string', 'string']);
  const read = call('read', 'string', ['number']);
  const write = call('write', 'number', ['number', 'string']);
  const revision = call('revision', 'number', ['number']);
  const close = call('close', 'number', ['number']);
  const closeIdentity = call('identity_close', null, ['number']);
  const push = call('push', 'number', ['number', 'number', 'number']);
  const follow = call('follow', 'number', ['number', 'number']);
  const pump = call('pump', null, ['number']);
  const disconnect = call('disconnect', null, ['number']);
  const networkError = call('network_error', 'string', ['number']);
  const connected = call('connected', 'number', ['number']);
  const stores = [];
  let network = 0, closed = false, lastError = '';
  if (url) {
    const endpoint = new URL(url);
    if (!['ws:', 'wss:'].includes(endpoint.protocol)) throw new Error('Expected a WebSocket URL');
    network = call('connect', 'number')();
    if (!network) throw new Error(error());
  }
  function checkOpen() { if (closed) throw new Error('Datafriend session is closed'); }
  function publish(store) {
    const next = revision(store.handle);
    if (next === store.revision) return;
    store.revision = next;
    const value = read(store.handle);
    if (value == null) throw new Error(error());
    store.value = JSON.parse(value);
    for (const listener of [...store.listeners]) listener(store.value);
  }
  function makeStore(publicKey, signer = 0) {
    checkOpen();
    if (!/^[0-9a-f]{64}$/.test(publicKey)) throw new Error('Expected a 64-character owner public key');
    const handle = open(`/session/store-${stores.length}`, publicKey);
    if (!handle) { if (signer) closeIdentity(signer); throw new Error(error()); }
    if (network && !(signer ? push(network, handle, signer) : follow(network, handle))) {
      close(handle); if (signer) closeIdentity(signer); throw new Error(error());
    }
    const state = {handle, signer, listeners: new Set(), revision: -1, value: null};
    stores.push(state);
    publish(state);
    return {
      key: publicKey,
      read() { checkOpen(); return state.value; },
      write(value) {
        checkOpen();
        if (!signer) throw new Error('Replica stores are read-only');
        const json = JSON.stringify(value);
        if (json === undefined || new TextEncoder().encode(json).length > 60000)
          throw new Error('State document must be JSON and at most 60 KB');
        if (write(handle, json) !== 0) throw new Error(error());
        publish(state);
      },
      subscribe(listener) {
        checkOpen(); state.listeners.add(listener); listener(state.value);
        return () => state.listeners.delete(listener);
      }
    };
  }
  const timer = network ? setInterval(() => {
    try {
      pump(network);
      const message = networkError(network);
      if (message && message !== lastError) onError(new Error(message));
      lastError = message;
      for (const store of stores) publish(store);
    } catch (e) { onError(e); }
  }, 16) : null;
  return {
    owner() {
      checkOpen(); const signer = identity();
      if (!signer) throw new Error(error());
      return makeStore(key(signer), signer);
    },
    follow(publicKey) { return makeStore(publicKey); },
    connected() { checkOpen(); return !!(network && connected(network)); },
    close() {
      if (closed) return;
      closed = true;
      if (timer) clearInterval(timer);
      if (network) disconnect(network);
      for (const store of stores) {
        close(store.handle);
        if (store.signer) closeIdentity(store.signer);
        store.listeners.clear();
      }
    }
  };
}
