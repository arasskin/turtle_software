import {test} from 'node:test';
import assert from 'node:assert/strict';
import {Drawing} from '../public/model.mjs';

const stroke = (id, author, points = [[10, 10], [20, 20]]) =>
  ({id, author, kind: 'stroke', color: '#608cae', width: 5, points, deleted: false, complete: true});

test('simultaneous overlapping records converge regardless of delivery order', () => {
  const a = new Drawing(), b = new Drawing();
  const first = {...a.put(stroke('a', 'alice')), order: 1};
  const second = {...b.put(stroke('b', 'bob')), order: 2};
  a.receive(second); a.receive(first);
  b.receive(first); b.receive(second);
  assert.deepEqual(a.strokes(), b.strokes());
  assert.equal(a.strokes().length, 2);
  assert.equal(a.pending.size, 0);
});

test('a partial acknowledgement does not discard a newer stroke or its outbox', () => {
  const a = new Drawing();
  const initial = a.put(stroke('a', 'alice', [[1, 1]]));
  a.put(stroke('a', 'alice', [[1, 1], [2, 2]]));
  a.receive({...initial, order: 3});
  assert.equal(a.get('a').points.length, 2);
  assert.equal(a.get('a').order, 3);
  assert.equal(a.pending.size, 1);
  const reloaded = new Drawing(a.saved());
  reloaded.receive({...reloaded.get('a'), order: 3});
  assert.equal(reloaded.pending.size, 0);
  reloaded.receive({...initial, order: 3});
  assert.equal(reloaded.get('a').points.length, 2);
});

test('reconnect merges offline strokes with the authoritative snapshot', () => {
  const a = new Drawing();
  a.put(stroke('offline', 'alice'));
  a.snapshot([{...stroke('remote', 'bob'), version: 1, order: 1}]);
  assert.deepEqual(a.strokes().map(r => r.id), ['remote', 'offline']);
  a.receive({...a.get('offline'), order: 2});
  a.snapshot([...a.records.values()]);
  assert.equal(a.strokes().length, 2);
  assert.equal(a.pending.size, 0);
});

test('undo only hides the local author’s last stroke and survives reload', () => {
  const a = new Drawing();
  a.receive({...stroke('mine', 'alice'), order: 1, version: 1});
  a.receive({...stroke('theirs', 'bob'), order: 2, version: 1});
  a.undo('alice');
  const copy = new Drawing(a.saved());
  assert.deepEqual(copy.strokes().map(r => r.id), ['theirs']);
  assert.equal(copy.pending.get('mine').deleted, true);
  assert.equal(copy.undo('alice'), null);
});

test('cursor updates use the same records but old presence is not replayed after reload', () => {
  const a = new Drawing();
  const cursor = a.put({id: 'cursor', author: 'alice', kind: 'cursor', x: 10, y: 20});
  a.receive(cursor);
  a.put({...cursor, x: 30});
  assert.equal(a.get('cursor').x, 30);
  assert.equal(a.strokes().length, 0);
  const copy = new Drawing(a.saved());
  assert.equal(copy.all().length, 0);
});
