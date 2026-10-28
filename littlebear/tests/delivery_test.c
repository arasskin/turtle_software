#define _POSIX_C_SOURCE 200809L
#include "connections.h"
#include <stdio.h>

static client *peer(connections *s, lb_namespace ns) {
  client *c = connection_take(s);
  assert(c);
  c->fd = 1; /* Ownership tests do not submit real socket operations. */
  c->channels = calloc(1, sizeof(channel));
  assert(c->channels);
  c->channel_capacity = c->channel_count = 1;
  channel *ch = c->channels;
  ch->id = 9;
  ch->known = ch->optimistic = ch->subscribed = true;
  memcpy(ch->name, &ns, 16);
  assert(ct_map_put(&c->channel_index, ch->id, 0));
  assert(subscription_add(s, c, ch));
  subscription_clean(ch->subscription);
  queue_remove(c);
  connection_account(s, c);
  return c;
}
static void drop(connections *s, client *c) {
  subscription_remove(s, c->channels[0].subscription);
  output_update_release(s, c);
  free(c->channels);
  ct_map_free(&c->channel_index);
  connection_release(s, c);
}
static void deliver(connections *s, client *c, uint64_t revision) {
  subscription *sub = c->channels[0].subscription;
  assert(sub->head && !sub->resync);
  delivery_start(s, sub, c->channels);
  assert(c->channels[0].announced == revision);
  assert(client_output_size(c) == 12 + c->output_update->size);
  output_update_release(s, c);
}
static void shared_lifetime(void) {
  connections s = CT_ZERO;
  lb_namespace ns = {1, 2};
  uint8_t records[100] = {0};
  s.memory_budget = 64 * 1024 * 1024;
  connections_published(&s, ns, 0, 1, 5, 11, lb_bytes_make(records, sizeof(records)));
  assert(!s.update_bytes); /* No subscribers means no retained history. */
  client *a = peer(&s, ns), *b = peer(&s, ns);
  subscription *as = a->channels[0].subscription, *bs = b->channels[0].subscription;
  connections_published(&s, ns, 0, 1, 5, 11, lb_bytes_make(records, sizeof(records)));
  shared_update *first = as->head->update;
  assert(first == bs->head->update && first->refs == 2);
  assert(s.update_bytes == sizeof(*first) + 25 + sizeof(records));
  assert(s.delivery_count == 2);
  connections_published(&s, ns, 1, 2, 5, 22, lb_bytes_make(records, sizeof(records)));
  assert(as->head->next && bs->head->next && as->tail->update == bs->tail->update);
  connections_changed(&s, ns, NULL); /* Notifications do not replace queued data. */
  delivery_start(&s, as, a->channels);
  assert(a->output_update == first && first->refs == 2);
  /* Detaching a channel cannot free the body's outstanding socket reference. */
  subscription_remove(&s, as);
  a->channels[0].subscription = NULL;
  assert(first->refs == 2 && a->output_update->data[0] == 5);
  deliver(&s, b, 1);
  assert(first->refs == 1);
  deliver(&s, b, 2);
  assert(s.update_bytes == sizeof(*first) + first->size);
  output_update_release(&s, a);
  assert(!s.update_bytes && !s.delivery_count);
  drop(&s, a); drop(&s, b);
  connections_destroy(&s);
}
static void namespaces_over_eight_mib(void) {
  connections s = CT_ZERO;
  client *peers[600];
  uint8_t records[16384] = {0};
  s.memory_budget = 64 * 1024 * 1024;
  for (size_t i = 0; i < 600; ++i) {
    lb_namespace ns = {i + 1, 99};
    peers[i] = peer(&s, ns);
    connections_published(&s, ns, 0, 1, 5, i, lb_bytes_make(records, sizeof(records)));
  }
  assert(s.update_bytes > 8 * 1024 * 1024);
  for (size_t i = 0; i < 600; ++i) {
    assert(!peers[i]->channels[0].subscription->resync);
    deliver(&s, peers[i], 1);
    drop(&s, peers[i]);
  }
  assert(!s.update_bytes && !s.delivery_count);
  connections_destroy(&s);
}
static void slow_peer(void) {
  connections s = CT_ZERO;
  lb_namespace ns = {44, 55};
  uint8_t records[65536] = {0};
  s.memory_budget = 64 * 1024 * 1024;
  client *fast = peer(&s, ns), *slow = peer(&s, ns);
  subscription *ss = slow->channels[0].subscription;
  connections_published(&s, ns, 0, 1, 5, 1, lb_bytes_make(records, sizeof(records)));
  delivery_start(&s, ss, slow->channels);
  shared_update *in_flight = slow->output_update;
  deliver(&s, fast, 1);
  uint64_t revision = 1;
  while (!ss->resync) {
    assert(++revision < 400);
    connections_published(&s, ns, revision - 1, revision, 5, revision,
                          lb_bytes_make(records, sizeof(records)));
    deliver(&s, fast, revision);
    assert(!fast->delivery_bytes && slow->delivery_bytes <= LB_DELIVERY_LIMIT);
  }
  assert(!ss->head && !ss->tail && slow->output_update == in_flight);
  assert(in_flight->refs == 1 && s.update_bytes == sizeof(*in_flight) + in_flight->size);
  /* Bytes already submitted survive overflow; after they complete and a fresh
   * opening establishes the base, normal delta delivery can resume. */
  output_update_release(&s, slow);
  slow->channels[0].announced = revision;
  ss->resync = false;
  connections_published(&s, ns, revision, revision + 1, 5, revision + 1,
                        lb_bytes_make(records, sizeof(records)));
  deliver(&s, fast, revision + 1);
  deliver(&s, slow, revision + 1);
  assert(!s.update_bytes && !s.delivery_count);
  drop(&s, fast); drop(&s, slow);
  connections_destroy(&s);
}
static void pressure_and_gap(void) {
  connections s = CT_ZERO;
  lb_namespace x = {77, 88}, y = {88, 99};
  uint8_t records[1024] = {0};
  s.memory_budget = 64 * 1024 * 1024;
  client *a = peer(&s, x), *b = peer(&s, y);
  subscription *as = a->channels[0].subscription, *bs = b->channels[0].subscription;
  connections_published(&s, x, 0, 1, 5, 1, lb_bytes_make(records, sizeof(records)));
  shared_update *saved = as->head->update;
  s.memory_budget = connection_memory(&s);
  connections_published(&s, y, 0, 1, 5, 1, lb_bytes_make(records, sizeof(records)));
  assert(bs->resync && !bs->head);
  assert(as->head->update == saved && !as->resync); /* No global purge. */
  s.memory_budget = 64 * 1024 * 1024;
  /* An aborted or otherwise missing revision breaks only this chain. */
  connections_published(&s, x, 2, 3, 5, 3, lb_bytes_make(records, sizeof(records)));
  assert(as->resync && !as->head && !s.update_bytes);
  drop(&s, a); drop(&s, b);
  connections_destroy(&s);
}
int main(void) {
  shared_lifetime();
  namespaces_over_eight_mib();
  slow_peer();
  pressure_and_gap();
  puts("PASS: shared update ownership, ordered delivery, 600 namespaces, slow-peer isolation and bounded fallback");
  return 0;
}
