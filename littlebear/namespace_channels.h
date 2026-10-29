#ifndef LB_NAMESPACE_CHANNELS_H
#define LB_NAMESPACE_CHANNELS_H
#include "models.h"
static inline channel *namespace_channel(lb_peer *peer) {
  return peer->active == SIZE_MAX ? NULL : &peer->channels[peer->active];
}
static inline bool namespace_channel_bind(lb_peer *peer, uint32_t id, const uint8_t name[LB_NAMESPACE_BYTES]) {
  ct_pair *found;
  found = ct_map_get(&peer->channel_index, id);
  if (found) peer->active = (size_t)found->value;
  else {
    if (!ct_array_reserve_limit(&peer->channels, &peer->channel_capacity, peer->channel_count + 1,
                                sizeof(*peer->channels), UINT32_MAX) ||
        !ct_map_put(&peer->channel_index, id, peer->channel_count)) return false;
    peer->active = peer->channel_count++;
    memset(&peer->channels[peer->active], 0, sizeof(channel));
    peer->channels[peer->active].id = id;
    memcpy(peer->channels[peer->active].name, name, LB_NAMESPACE_BYTES);
  }
  return !memcmp(namespace_channel(peer)->name, name, LB_NAMESPACE_BYTES);
}
static inline void namespace_channel_release(lb_peer *peer) {
  channel *ch = namespace_channel(peer);
  assert(ch);
  ct_map_erase(&peer->channel_index, ch->id);
  --peer->channel_count;
  if (peer->active != peer->channel_count) {
    *ch = peer->channels[peer->channel_count];
    ct_map_get(&peer->channel_index, ch->id)->value = peer->active;
  }
  peer->active = SIZE_MAX;
}
#endif
