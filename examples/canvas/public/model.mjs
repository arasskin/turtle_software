// Browser-side projection and an outbox, not a database engine.
export const WIDTH = 1600, HEIGHT = 1000, MAX_POINTS = 1024;

export class Drawing {
  constructor(saved = {}) {
    this.records = new Map((saved.records || []).map(r => [r.id, r]));
    this.pending = new Map((saved.pending || []).filter(r => r.kind === 'stroke').map(r => [r.id, r]));
  }
  get(id) {
    const pending = this.pending.get(id), confirmed = this.records.get(id);
    return pending ? {...pending, order: confirmed?.order ?? pending.order} : confirmed;
  }
  put(record) {
    const previous = this.get(record.id);
    const next = {...record, version: (previous?.version || 0) + 1};
    this.pending.set(next.id, next);
    return next;
  }
  receive(record) {
    if (record.version >= (this.records.get(record.id)?.version || 0)) this.records.set(record.id, record);
    if (record.version >= (this.pending.get(record.id)?.version || Infinity)) this.pending.delete(record.id);
  }
  snapshot(records) {
    this.records.clear();
    for (const record of records) this.receive(record);
  }
  all() {
    const ids = new Set([...this.records.keys(), ...this.pending.keys()]);
    return [...ids].map(id => this.get(id));
  }
  strokes() {
    return this.all().filter(r => r.kind === 'stroke' && !r.deleted)
      .sort((a, b) => (a.order ?? Number.MAX_SAFE_INTEGER) - (b.order ?? Number.MAX_SAFE_INTEGER) || a.id.localeCompare(b.id));
  }
  undo(author) {
    const record = this.strokes().findLast(r => r.author === author);
    return record ? this.put({...record, deleted: true, complete: true}) : null;
  }
  saved() {
    return {records: [...this.records.values()].filter(r => r.kind === 'stroke'),
      pending: [...this.pending.values()].filter(r => r.kind === 'stroke')};
  }
}

export function paintStroke(ctx, stroke) {
  if (!stroke.points.length || stroke.deleted) return;
  ctx.strokeStyle = ctx.fillStyle = stroke.color;
  ctx.lineWidth = stroke.width;
  ctx.lineCap = ctx.lineJoin = 'round';
  ctx.beginPath();
  if (stroke.points.length === 1) {
    ctx.arc(...stroke.points[0], stroke.width / 2, 0, Math.PI * 2);
    ctx.fill();
  } else {
    ctx.moveTo(...stroke.points[0]);
    for (const point of stroke.points.slice(1)) ctx.lineTo(...point);
    ctx.stroke();
  }
}
