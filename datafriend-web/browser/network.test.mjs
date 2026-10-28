import assert from 'node:assert/strict';
import {createSession} from '../build/browser/client.mjs';

const url = process.env.DATAFRIEND_WS_URL || 'ws://localhost:7778';
const errors = [];
const host = await createSession({url, onError: error => errors.push(error.message)});
const client = await createSession({url, onError: error => errors.push(error.message)});
async function until(test) {
  const deadline = Date.now() + 15000;
  while (!test()) {
    if (Date.now() > deadline) throw new Error(`Sync timed out: ${errors.join('; ')}`);
    await new Promise(resolve => setTimeout(resolve, 20));
  }
}
try {
  const world = host.owner();
  const proposals = client.owner();
  const worldReplica = client.follow(world.key);
  const proposalReplica = host.follow(proposals.key);
  world.write({revision: 0, x: 100});
  await until(() => worldReplica.read()?.x === 100);
  proposals.write({sequence: 1, x: 103});
  await until(() => proposalReplica.read()?.sequence === 1);
  world.write({revision: 1, x: proposalReplica.read().x});
  await until(() => worldReplica.read()?.revision === 1);
  assert.deepEqual(worldReplica.read(), {revision: 1, x: 103});
  for (let i = 2; i <= 15; i++) {
    world.write({revision: i, x: 100 + i});
    await until(() => worldReplica.read()?.revision === i);
  }
  console.log('Datafriend WASM -> WebSocket -> Littlebear: signed host/client sync and repeated updates passed.');
} finally { client.close(); host.close(); }
