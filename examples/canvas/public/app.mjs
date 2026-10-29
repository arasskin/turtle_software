import {Drawing, WIDTH, HEIGHT, MAX_POINTS, paintStroke} from './model.mjs';

const $ = id => document.getElementById(id);
const canvas = $('canvas'), ctx = canvas.getContext('2d'), paper = $('paper');
const colors = [['Graphite', '#36453b'], ['Moss', '#64825a'], ['Ochre', '#d4a343'],
  ['Clay', '#cb735d'], ['Rose', '#b87496'], ['Blue', '#608cae'], ['Lilac', '#8f87b5']];
// getRandomValues works on HTTP LAN origins too (randomUUID requires a secure context).
const uuid = () => [...crypto.getRandomValues(new Uint8Array(16))].map(x => x.toString(16).padStart(2, '0')).join('');
function preference(key, fallback) { try { return localStorage.getItem(key) || fallback; } catch { return fallback; } }
function remember(key, value) { try { localStorage.setItem(key, value); } catch { /* The canvas still works in memory. */ } }
const actor = preference('canvas-author', uuid());
remember('canvas-author', actor);
const cursorId = uuid();
let color = preference('canvas-color', colors[1][1]);
if (!colors.some(c => c[1] === color)) color = colors[1][1];
let brush = 5, drawing = new Drawing(), active = null, pointer = null;
let socket, ready = false, fatal = false, retry = 0, reconnectTimer, snapshot = [];
let cache, cacheKey, storageFailed = false, persistTimer, toastTimer, serverOffset = 0;
let nativeSync = {enabled: false, connected: false};
let dirty = true, inkDirty = true, cursorDirty = true, booting = true;
let lastStroke = 0, lastCursor = 0, stagedPoints = 0;
const sent = new Map(), cursorNodes = new Map();
const cursor = {id: cursorId, kind: 'cursor', author: actor, x: WIDTH / 2, y: HEIGHT / 2,
  color, name: preference('canvas-name', `Guest ${actor.slice(0, 3).toUpperCase()}`), active: false, online: true};
$('name').value = cursor.name;
const key = new URLSearchParams(location.hash.slice(1)).get('key') || '';

function toast(text, duration = 4500) {
  $('toast').textContent = text; $('toast').hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').hidden = true, duration);
}
function status() {
  $('connection').className = `connection ${ready ? 'online' : 'offline'}`;
  $('status').textContent = fatal ? 'Invite needed' : !ready ? 'Offline · keep drawing'
    : nativeSync.enabled ? (nativeSync.connected ? 'Littlebear connected' : 'Sync offline · draw locally') : 'Local host connected';
  const count = [...drawing.pending.values()].filter(r => r.kind === 'stroke').length;
  $('save-status').textContent = storageFailed ? 'Browser storage unavailable · keep this tab open'
    : count ? `${count} ${count === 1 ? 'stroke' : 'strokes'} waiting for the host`
    : ready ? (nativeSync.enabled ? 'Saved locally · native sync' : 'Saved on the host') : 'Drawing from your local copy';
  $('undo').disabled = !drawing.all().some(r => r.kind === 'stroke' && !r.deleted && r.author === actor);
}

function openCache() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open('turtle-canvas-cache', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('boards');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
    request.onblocked = () => reject(new Error('Storage is blocked'));
  });
}
function readCache() {
  return new Promise((resolve, reject) => {
    const request = cache.transaction('boards').objectStore('boards').get(cacheKey);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}
function saveCache() {
  clearTimeout(persistTimer);
  if (!cache) return;
  try {
    const transaction = cache.transaction('boards', 'readwrite');
    transaction.objectStore('boards').put(drawing.saved(), cacheKey);
    transaction.onerror = transaction.onabort = () => { storageFailed = true; status(); };
  } catch { storageFailed = true; status(); }
}
function saveSoon() {
  // Coalesce host updates; a completed local stroke is saved immediately.
  clearTimeout(persistTimer); persistTimer = setTimeout(saveCache, 120);
}
function flush() {
  if (!ready || socket?.readyState !== WebSocket.OPEN) return;
  for (const record of drawing.pending.values()) {
    if (sent.get(record.id) >= record.version) continue;
    if (socket.bufferedAmount > 256 * 1024) break;
    socket.send(JSON.stringify({type: 'put', record}));
    sent.set(record.id, record.version);
  }
}
function stageStroke(complete = false) {
  if (!active) return;
  drawing.put({...active, points: active.points.map(p => [...p]), complete});
  stagedPoints = active.points.length;
  dirty = inkDirty = true;
  if (complete) saveCache(); else saveSoon();
  flush(); status();
}
function putCursor() {
  if (!ready) return;
  drawing.put({...cursor, color, online: true});
  flush(); cursorDirty = false;
}
function connect() {
  if (fatal) return;
  clearTimeout(reconnectTimer);
  socket?.close();
  socket = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/sync`);
  const current = socket;
  current.onopen = () => current.send(JSON.stringify({type: 'hello', key, actor, cursor: cursorId}));
  current.onmessage = event => {
    if (socket !== current) return;
    try {
      const message = JSON.parse(event.data);
      if (message.type === 'snapshot-start') snapshot = [];
      else if (message.type === 'snapshot') snapshot.push(...message.records);
      else if (message.type === 'ready') {
        drawing.snapshot(snapshot); snapshot = [];
        serverOffset = message.time - Date.now();
        nativeSync = message.sync || {enabled: false};
        ready = true; retry = 0; sent.clear();
        // Expired presence is never replayed from the browser cache.
        drawing.pending.delete(cursorId);
        putCursor(); flush(); saveSoon(); dirty = inkDirty = true;
      } else if (message.type === 'record') {
        drawing.receive(message.record);
        dirty = true;
        if (message.record.kind === 'stroke') { saveSoon(); inkDirty = true; }
      } else if (message.type === 'sync') {
        nativeSync = message.state;
      } else if (message.type === 'error') {
        toast(message.message, 9000);
        if (message.fatal) { fatal = true; ready = false; current.close(); $('join-dialog').showModal(); }
      }
      status();
    } catch (error) { toast(`Could not read the host update: ${error.message}`); }
  };
  current.onclose = () => {
    if (socket !== current) return;
    ready = false; sent.clear(); dirty = true; status();
    if (!fatal) reconnectTimer = setTimeout(connect, Math.min(8000, 500 * 2 ** retry++) + Math.random() * 250);
  };
  current.onerror = () => current.close();
}

function coordinates(event) {
  const box = canvas.getBoundingClientRect();
  return [Math.round(Math.max(0, Math.min(WIDTH, (event.clientX - box.left) * WIDTH / box.width)) * 10) / 10,
    Math.round(Math.max(0, Math.min(HEIGHT, (event.clientY - box.top) * HEIGHT / box.height)) * 10) / 10];
}
function begin(point) {
  active = {id: uuid(), kind: 'stroke', author: actor, color, width: brush,
    points: [point], complete: false, deleted: false};
  stageStroke();
}
function finish() {
  if (active) { stageStroke(true); active = null; dirty = inkDirty = true; }
  pointer = null;
}
canvas.addEventListener('pointerdown', event => {
  if (pointer !== null || event.button !== 0 || fatal || booting) return;
  event.preventDefault();
  pointer = event.pointerId; canvas.setPointerCapture(pointer);
  const point = coordinates(event);
  [cursor.x, cursor.y] = point; cursor.active = true; cursorDirty = true;
  begin(point);
});
canvas.addEventListener('pointermove', event => {
  if (pointer !== null && event.pointerId !== pointer) return;
  [cursor.x, cursor.y] = coordinates(event); cursor.active = true; cursorDirty = true;
  if (!active) return;
  const coalesced = event.getCoalescedEvents?.() || [];
  for (const sample of coalesced.length ? coalesced : [event]) {
    const point = coordinates(sample), previous = active.points.at(-1);
    if (Math.hypot(point[0] - previous[0], point[1] - previous[1]) < .65) continue;
    if (active.points.length >= MAX_POINTS) {
      const pointerId = pointer;
      finish(); pointer = pointerId; begin(previous);
    }
    active.points.push(point);
  }
  dirty = inkDirty = true;
});
canvas.addEventListener('pointerup', event => {
  if (event.pointerId !== pointer) return;
  // Include the final point even when the browser coalesces the last move.
  if (active && active.points.length < MAX_POINTS) active.points.push(coordinates(event));
  finish();
});
canvas.addEventListener('pointercancel', event => { if (event.pointerId === pointer) finish(); });
canvas.addEventListener('lostpointercapture', event => { if (event.pointerId === pointer) finish(); });
canvas.addEventListener('pointerleave', () => { cursor.active = false; cursorDirty = true; });
window.addEventListener('blur', () => { finish(); cursor.active = false; cursorDirty = true; });
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { finish(); cursor.active = false; putCursor(); saveCache(); }
});
window.addEventListener('pagehide', () => { finish(); saveCache(); socket?.close(); });
window.addEventListener('online', () => { if (!ready && !fatal) connect(); });

function renderCursors() {
  const online = drawing.all().filter(r => r.kind === 'cursor' && r.online
    && Date.now() + serverOffset - r.seenAt < 16000);
  const others = ready ? online.filter(r => r.id !== cursorId) : [];
  const visible = new Set();
  for (const record of others) {
    if (!record.active) continue;
    visible.add(record.id);
    let node = cursorNodes.get(record.id);
    if (!node) {
      node = document.createElement('div'); node.className = 'cursor';
      node.innerHTML = '<svg viewBox="0 0 20 24"><path d="m2 2 15 12-7 1-3 7Z"/></svg><span></span>';
      $('cursors').append(node); cursorNodes.set(record.id, node);
    }
    node.style.transform = `translate(${record.x / WIDTH * paper.clientWidth}px,${record.y / HEIGHT * paper.clientHeight}px)`;
    node.style.color = record.color;
    node.querySelector('span').textContent = record.name;
    node.querySelector('span').style.backgroundColor = record.color;
  }
  for (const [id, node] of cursorNodes) if (!visible.has(id)) { node.remove(); cursorNodes.delete(id); }
  $('avatars').replaceChildren();
  for (const record of [cursor, ...others].slice(0, 6)) {
    const avatar = document.createElement('span'); avatar.className = 'avatar';
    avatar.textContent = record.name.slice(0, 1).toUpperCase(); avatar.title = record.id === cursorId ? `${record.name} (you)` : record.name;
    avatar.style.backgroundColor = record.id === cursorId ? color : record.color;
    $('avatars').append(avatar);
  }
  $('people-count').textContent = others.length ? `${others.length + 1} drawing together` : 'Just you, for now';
}
function render() {
  if (inkDirty) {
    ctx.clearRect(0, 0, WIDTH, HEIGHT);
    const strokes = drawing.strokes();
    for (const record of strokes) paintStroke(ctx, record.id === active?.id ? active : record);
    if (active && !strokes.some(r => r.id === active.id)) paintStroke(ctx, active);
    $('empty').hidden = strokes.length > 0 || !!active;
    inkDirty = false;
  }
  renderCursors(); dirty = false;
}
function tick(time) {
  if (active && active.points.length !== stagedPoints && time - lastStroke > 55) { stageStroke(); lastStroke = time; }
  if (cursorDirty && time - lastCursor > 55) { putCursor(); lastCursor = time; }
  if (dirty) render();
  flush(); requestAnimationFrame(tick);
}

function selectColor(next) {
  color = next; cursor.color = next; remember('canvas-color', next); cursorDirty = true; dirty = true;
  $('you-dot').style.backgroundColor = next;
  for (const button of $('palette').children) button.setAttribute('aria-pressed', String(button.dataset.color === next));
}
for (const [name, value] of colors) {
  const button = document.createElement('button'); button.className = 'swatch';
  button.dataset.color = value; button.style.backgroundColor = button.style.color = value;
  button.title = button.ariaLabel = name; button.addEventListener('click', () => selectColor(value));
  $('palette').append(button);
}
selectColor(color);
$('size').addEventListener('input', event => {
  brush = Number(event.target.value); $('size-label').value = brush;
  $('size-dot').style.width = $('size-dot').style.height = `${Math.max(3, brush * .7)}px`;
});
$('name').addEventListener('change', () => {
  // Limit UTF-8 size as well as characters to match the native validation.
  let name = $('name').value.trim() || 'Guest';
  while (new TextEncoder().encode(name).length > 48) name = name.slice(0, -1);
  cursor.name = $('name').value = name; remember('canvas-name', name); cursorDirty = true; dirty = true;
});
function undo() {
  finish();
  if (drawing.undo(actor)) { saveCache(); flush(); dirty = inkDirty = true; status(); }
}
$('undo').addEventListener('click', undo);
document.addEventListener('keydown', event => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z'
      && !['INPUT', 'TEXTAREA'].includes(document.activeElement.tagName)) {
    event.preventDefault(); undo();
  }
});
$('export').addEventListener('click', () => {
  const exportCanvas = document.createElement('canvas'); exportCanvas.width = WIDTH; exportCanvas.height = HEIGHT;
  const exportCtx = exportCanvas.getContext('2d'); exportCtx.fillStyle = '#fffefa'; exportCtx.fillRect(0, 0, WIDTH, HEIGHT);
  for (const stroke of drawing.strokes()) paintStroke(exportCtx, stroke.id === active?.id ? active : stroke);
  exportCanvas.toBlob(blob => {
    const link = document.createElement('a'); link.href = URL.createObjectURL(blob); link.download = 'common-ground.png';
    link.click(); setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  });
});
$('invite').addEventListener('click', async () => {
  if (!key) return $('join-dialog').showModal();
  const url = `${location.origin}/#key=${key}`;
  try { await navigator.clipboard.writeText(url); toast('Invite link copied. Open it in another tab or send it to a friend.'); }
  catch { $('share-url').value = url; $('share-dialog').showModal(); $('share-url').select(); }
});
$('join-submit').addEventListener('click', event => {
  const value = $('join-key').value.trim();
  if (!/^[a-f0-9]{64}$/.test(value)) return;
  event.preventDefault(); location.hash = `key=${value}`; location.reload();
});
new ResizeObserver(() => dirty = true).observe(paper);
setInterval(() => { cursorDirty = true; dirty = true; }, 4000);

async function start() {
  if (!/^[a-f0-9]{64}$/.test(key)) {
    fatal = true; status(); $('join-dialog').showModal(); requestAnimationFrame(tick); return;
  }
  cacheKey = `${location.origin}/${key}`;
  try { cache = await openCache(); drawing = new Drawing(await readCache()); }
  catch { storageFailed = true; }
  // A reloaded tab must finish any stroke that was interrupted mid-gesture.
  for (const record of [...drawing.pending.values()]) {
    if (!record.complete) drawing.put({...record, complete: true});
  }
  booting = false;
  status(); connect(); requestAnimationFrame(tick);
}
start();
