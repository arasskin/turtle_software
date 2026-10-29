#ifndef LB_SUBSCRIBER_DELIVERY_H
#define LB_SUBSCRIBER_DELIVERY_H
#include "models.h"
#include "namespaces.h"
#include "codec.h"
#include "exchange.h"
#include "index_sync.h"
typedef struct watch watch;
struct shared_update {
  size_t refs, size;
  uint64_t before, after;
  uint8_t data[]; /* status, epoch, revision, fingerprint, records */
};
typedef struct delivery {
  struct delivery *next;
  shared_update *update;
} delivery;
enum { LB_DELIVERY_LIMIT = 16 * 1024 * 1024 };
struct watch {
  lb_namespace ns;
  watch *collision_next;
  subscription *head;
};
struct subscription {
  lb_peer *owner;
  uint32_t channel_id;
  struct watch *watch;
  subscription *prev, *next, *dirty_prev, *dirty_next;
  bool dirty;
  bool resync;
  delivery *head, *tail;
};
typedef struct lb_subscribers {
  size_t subscription_count, watch_count;
  ct_map watches;
  size_t update_bytes, delivery_count;
  void *context;
  bool (*reserve)(void *, size_t);
  void (*wake)(void *, lb_peer *);
} lb_subscribers;
static inline size_t subscriber_memory(const lb_subscribers *s) {
  return s->subscription_count * sizeof(subscription) + s->watch_count * sizeof(watch) +
      s->watches.capacity * sizeof(ct_pair) + s->watches.buckets * sizeof(uint32_t) +
      s->update_bytes + s->delivery_count * sizeof(delivery);
}
static inline bool delivery_reserve(lb_subscribers *s, size_t bytes) {
  return s->reserve && s->reserve(s->context, bytes);
}
static inline void update_release(lb_subscribers *s, shared_update *update) {
  assert(update && update->refs);
  if (!--update->refs) {
    s->update_bytes -= sizeof(*update) + update->size;
    free(update);
  }
}
/* Transfer a queue's reference to the caller, including its logical byte cost. */
static inline shared_update *delivery_pop(lb_subscribers *s, subscription *sub) {
  delivery *d = sub->head;
  shared_update *update;
  if (!d) return NULL;
  update = d->update;
  sub->head = d->next;
  if (!sub->head) sub->tail = NULL;
  sub->owner->delivery_bytes -= sizeof(*d);
  --s->delivery_count;
  free(d);
  return update;
}
static inline void delivery_release(lb_subscribers *s, lb_peer *c, shared_update *update) {
  c->delivery_bytes -= sizeof(c->output_header) + update->size;
  update_release(s, update);
}
static inline void deliveries_clear(lb_subscribers *s, subscription *sub) {
  shared_update *update;
  while ((update = delivery_pop(s, sub))) delivery_release(s, sub->owner, update);
}
static inline void output_update_release(lb_subscribers *s, lb_peer *c) {
  if (!c->output_update) return;
  delivery_release(s, c, c->output_update);
  c->output_update = NULL;
}
static inline void delivery_integer(uint8_t *p, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    p[bytes - i - 1] = (uint8_t)(value >> (8 * i));
}
static inline void delivery_start(lb_subscribers *s, subscription *sub, channel *ch) {
  lb_peer *c = sub->owner;
  assert(!c->output_update && sub->head);
  c->output_update = delivery_pop(s, sub);
  /* Connection-owned frame prefix; shared body is never copied per peer. */
  delivery_integer(c->output_header, c->output_update->size + 8, 4);
  delivery_integer(c->output_header + 4, 0, 4);
  delivery_integer(c->output_header + 8, ch->id, 4);
  ch->known = true;
  ch->announced = c->output_update->after;
}
static inline void subscription_clean(subscription *sub) {
  lb_peer *c = sub->owner;
  if (!sub->dirty) return;
  if (sub->dirty_prev) sub->dirty_prev->dirty_next = sub->dirty_next;
  else c->dirty_head = sub->dirty_next;
  if (sub->dirty_next) sub->dirty_next->dirty_prev = sub->dirty_prev;
  else c->dirty_tail = sub->dirty_prev;
  sub->dirty_prev = sub->dirty_next = NULL;
  sub->dirty = false;
}
static inline void subscription_dirty(lb_subscribers *s, subscription *sub) {
  lb_peer *c = sub->owner;
  if (c->closing) return;
  if (!sub->dirty) {
    sub->dirty = true;
    sub->dirty_prev = c->dirty_tail;
    if (c->dirty_tail) c->dirty_tail->dirty_next = sub;
    else c->dirty_head = sub;
    c->dirty_tail = sub;
  }
  if (s->wake) s->wake(s->context, c);
}
static inline watch *watch_find(lb_subscribers *s, lb_namespace ns) {
  ct_pair *p = ct_map_get(&s->watches, ns.low64);
  watch *w = p ? (watch *)p->value : NULL;
  while (w && !lb_namespace_equal(w->ns, ns)) w = w->collision_next;
  return w;
}
static inline bool subscription_add(lb_subscribers *s, lb_peer *c, channel *ch) {
  lb_namespace ns = lb_name(ch->name);
  watch *w = watch_find(s, ns);
  subscription *sub;
  if (ch->subscription) {
    subscription_dirty(s, ch->subscription);
    return true;
  }
  sub = (subscription *)calloc(1, sizeof(*sub));
  if (!sub) return false;
  if (!w) {
    ct_pair *p = ct_map_get(&s->watches, ns.low64);
    w = (watch *)calloc(1, sizeof(*w));
    if (!w) { free(sub); return false; }
    w->ns = ns;
    w->collision_next = p ? (watch *)p->value : NULL;
    if (!ct_map_put(&s->watches, ns.low64, (uintptr_t)w)) {
      free(w); free(sub); return false;
    }
    ++s->watch_count;
  }
  sub->owner = c;
  sub->channel_id = ch->id;
  sub->watch = w;
  sub->next = w->head;
  if (w->head) w->head->prev = sub;
  w->head = sub;
  ch->subscription = sub;
  ++s->subscription_count;
  subscription_dirty(s, sub);
  return true;
}
static inline void subscription_remove(lb_subscribers *s, subscription *sub) {
  watch *w;
  if (!sub) return;
  w = sub->watch;
  subscription_clean(sub);
  deliveries_clear(s, sub);
  if (sub->prev) sub->prev->next = sub->next;
  else w->head = sub->next;
  if (sub->next) sub->next->prev = sub->prev;
  free(sub);
  --s->subscription_count;
  if (!w->head) {
    ct_pair *p = ct_map_get(&s->watches, w->ns.low64);
    watch *head = (watch *)p->value;
    if (head == w) p->value = (uintptr_t)w->collision_next;
    else {
      while (head->collision_next != w) head = head->collision_next;
      head->collision_next = w->collision_next;
    }
    if (!p->value) ct_map_erase(&s->watches, w->ns.low64);
    free(w);
    --s->watch_count;
  }
}
static inline void subscribers_published(lb_subscribers *s, lb_namespace ns, uint64_t before,
                                         uint64_t after, uint64_t epoch, uint64_t fingerprint,
                                         lb_bytes records) {
  watch *w = watch_find(s, ns);
  shared_update *update = NULL;
  bool unavailable = records.size > LB_MAX_FRAME - 33;
  if (!w) return; /* No history retained for absent/future subscribers. */
  for (subscription *sub = w->head; sub; sub = sub->next) {
    lb_peer *c = sub->owner;
    ct_pair *found;
    channel *ch;
    size_t cost = sizeof(delivery) + sizeof(c->output_header) + 25 + records.size;
    delivery *d;
    if (c->closing) continue;
    found = ct_map_get(&c->channel_index, sub->channel_id);
    assert(found);
    ch = &c->channels[found->value];
    if (!ch->optimistic || !ch->known || sub->resync ||
        (sub->tail ? sub->tail->update->after : ch->announced) != before ||
        cost > LB_DELIVERY_LIMIT || c->delivery_bytes > LB_DELIVERY_LIMIT - cost)
      goto fallback;
    if (!update && !unavailable) {
      size_t bytes = sizeof(*update) + 25 + records.size;
      if (delivery_reserve(s, bytes + sizeof(*d))) update = (shared_update *)malloc(bytes);
      if (update) {
        update->refs = 1; /* Publisher retains a reference through fanout. */
        update->size = 25 + records.size;
        update->before = before;
        update->after = after;
        update->data[0] = lb_status_record_changes;
        delivery_integer(update->data + 1, epoch, 8);
        delivery_integer(update->data + 9, after, 8);
        delivery_integer(update->data + 17, fingerprint, 8);
        if (records.size) memcpy(update->data + 25, records.data, records.size);
        s->update_bytes += bytes;
      } else unavailable = true;
    }
    if (!update || !delivery_reserve(s, sizeof(*d))) goto fallback;
    d = (delivery *)malloc(sizeof(*d));
    if (!d) goto fallback;
    d->next = NULL;
    d->update = update;
    ++update->refs;
    ++s->delivery_count;
    c->delivery_bytes += cost;
    if (sub->tail) sub->tail->next = d;
    else sub->head = d;
    sub->tail = d;
    subscription_dirty(s, sub);
    continue;
fallback:
    /* Only this subscription loses its chain. Already submitted socket bytes
     * remain pinned until completion; send a fresh opening after they drain. */
    deliveries_clear(s, sub);
    sub->resync = true;
    subscription_dirty(s, sub);
  }
  if (update) update_release(s, update);
}
static inline void subscribers_changed(lb_subscribers *s, lb_namespace ns,
                                       const lb_publication *publication) {
  if (publication) {
    subscribers_published(s, ns, publication->before, publication->after,
                           publication->epoch, publication->fingerprint, publication->records);
    return;
  }
  watch *w = watch_find(s, ns);
  if (w) for (subscription *sub = w->head; sub; sub = sub->next)
    subscription_dirty(s, sub);
}
/* The caller decides when a fresh namespace view supersedes queued batches. */
static inline void subscription_reset_revision(lb_subscribers *s, channel *ch, uint64_t revision) {
  if (!ch || !ch->subscribed) return;
  ch->announced = revision;
  ch->known = true;
  if (ch->subscription) {
    deliveries_clear(s, ch->subscription);
    ch->subscription->resync = false;
  }
}
/* At most one dirty subscription per turn. Busy namespaces are reawakened by
 * publication after unlock; an unchanged subscription does not spin. */
static inline bool subscriber_next(lb_subscribers *s, lb_peer *peer, const lb_directory *namespaces, uint64_t epoch,
                                    lb_buffer *reply, uint32_t *channel_id) {
  subscription *sub = peer->dirty_head;
  channel *ch;
  lb_namespace ns;
  uint64_t revision;
  ct_pair *found;
  if (!sub) return true;
  found = ct_map_get(&peer->channel_index, sub->channel_id);
  assert(found);
  ch = &peer->channels[found->value];
  subscription_clean(sub);
  if (sub->head && !sub->resync) {
    assert(ch->known && ch->announced == sub->head->update->before);
    delivery_start(s, sub, ch);
    subscription_dirty(s, sub);
    return true;
  }
  ns = lb_name(ch->name);
  if (lb_namespace_writing(namespaces, ns)) return true;
  revision = lb_namespace_revision(namespaces, ns);
  if (ch->known && ch->announced == revision && !sub->resync) return true;
  lb_namespace_entry *entry = lb_directory_entry(namespaces, ns);
  lb_reply_header(reply, entry ? lb_status_namespace_changed : lb_status_absent, epoch, revision);
  if (entry && !lb_exchange_begin(lb_index_source(&entry->value), reply)) return false;
  if (reply->failed) return false;
  *channel_id = ch->id;
  ch->known = true;
  ch->announced = revision;
  sub->resync = false;
  return true;
}
#endif
