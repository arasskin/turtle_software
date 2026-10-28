import assert from 'node:assert/strict';
import {createSession} from '../build/browser/client.mjs';

const session = await createSession();
try {
  const owner = session.owner();
  assert.match(owner.key, /^[0-9a-f]{64}$/);
  assert.equal(owner.read(), null);
  const seen = [];
  const unsubscribe = owner.subscribe(value => seen.push(value));
  for (let i = 0; i < 100; i++) {
    owner.write({x: i, text: 'Dino 🦕'});
    assert.deepEqual(owner.read(), {x: i, text: 'Dino 🦕'});
  }
  assert.equal(seen.length, 101);
  unsubscribe();
  owner.write({x: 100});
  assert.equal(seen.length, 101);
  const replica = session.follow(owner.key);
  assert.throws(() => replica.write({x: 0}), /read-only/);
  assert.throws(() => session.follow('not-a-key'), /public key/);
  assert.throws(() => owner.write('x'.repeat(60001)), /60 KB/);
  session.close();
  assert.throws(() => owner.read(), /closed/);
  assert.throws(() => owner.write({}), /closed/);
  session.close();
  console.log('Datafriend WASM: read/write, subscriptions, Unicode, ownership, limits, and cleanup passed.');
} finally { session.close(); }
