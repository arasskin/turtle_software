/* Native Janet bridge. The same Datafriend watches publish local and remote edits.
 * Resource references keep attached stores/identities alive until network teardown. */
#include "janet.h"
#include "datafriend/db.h"
#include "datafriend/network.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
    df_store *db;
    df_watch *watch;
    unsigned refs, attached;
    bool dirty;
} StoreData;
typedef struct { StoreData *data; } Store;
typedef struct { void *handle; unsigned refs; } IdentityData;
typedef struct { IdentityData *data; } Identity;
typedef struct { StoreData *store; IdentityData *identity; } Attachment;
typedef struct { df_network *net; Attachment *items; size_t count, capacity; } Network;

static void owned_error(df_buffer error) {
    Janet message = janet_stringv(error.data, (int32_t)error.size);
    df_free(error.data);
    janet_panicv(message);
}
static void release_store(StoreData *s) {
    if (s && !--s->refs) {
        df_buffer error = {0};
        df_watch_remove(s->db, &s->watch);
        df_store_close(s->db, &error);
        df_free(error.data);
        free(s);
    }
}
static void release_identity(IdentityData *id) {
    if (id && !--id->refs) { df_identity_close(id->handle); free(id); }
}
static int collect_store(void *data, size_t len) {
    (void)len; Store *s = data; release_store(s->data); s->data = NULL; return 0;
}
static int collect_identity(void *data, size_t len) {
    (void)len; Identity *id = data; release_identity(id->data); id->data = NULL; return 0;
}
static int collect_network(void *data, size_t len) {
    (void)len; Network *n = data;
    if (n->net) df_network_close(n->net);
    n->net = NULL;
    for (size_t i = 0; i < n->count; ++i) {
        --n->items[i].store->attached;
        release_store(n->items[i].store);
        release_identity(n->items[i].identity);
    }
    free(n->items); n->items = NULL; n->count = n->capacity = 0;
    return 0;
}
static const JanetAbstractType store_type = {"canvas/store", collect_store, JANET_ATEND_GC};
static const JanetAbstractType identity_type = {"canvas/identity", collect_identity, JANET_ATEND_GC};
static const JanetAbstractType network_type = {"canvas/network", collect_network, JANET_ATEND_GC};
static StoreData *get_store(Janet *argv, int i) {
    Store *s = janet_getabstract(argv, i, &store_type);
    if (!s->data) janet_panic("store is closed");
    return s->data;
}
static Network *get_network(Janet *argv) {
    Network *n = janet_getabstract(argv, 0, &network_type);
    if (!n->net) janet_panic("network is closed");
    return n;
}
static void changed(void *context, df_view *view) { (void)view; ((StoreData *)context)->dirty = true; }

static Janet open_store(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    const char *path = janet_getcstring(argv, 0);
    const char *ns = argc == 2 ? janet_getcstring(argv, 1) : "turtle-canvas-v1";
    Store *wrapper = janet_abstract(&store_type, sizeof(Store));
    wrapper->data = NULL;
    StoreData *s = calloc(1, sizeof(*s));
    if (!s) janet_panic("out of memory");
    df_options options = {path, ns, DF_SYNC, 1u << 28};
    df_buffer error = {0};
    s->db = df_store_open(&options, &error);
    if (!s->db) { free(s); owned_error(error); }
    df_free(error.data);
    s->refs = 1;
    df_match clause = {df_var(0), DF_ATTR_DOCUMENT_STATE, df_var(1), true};
    df_query_spec spec = {&clause, 1, false};
    s->watch = df_watch_query(s->db, spec, changed, s);
    if (!s->watch) { release_store(s); janet_panic("cannot watch canvas records"); }
    wrapper->data = s;
    return janet_wrap_abstract(wrapper);
}
static Janet put_record(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 3);
    StoreData *s = get_store(argv, 0);
    double requested = janet_getnumber(argv, 1);
    if (!isfinite(requested) || requested < 0 || requested > UINT32_MAX || requested != (uint32_t)requested)
        janet_panic("invalid entity id");
    JanetByteView text = janet_getbytes(argv, 2);
    df_edit edit = {0};
    uint32_t id = (uint32_t)requested;
    if (!id) id = df_edit_create(&edit, s->db);
    df_edit_set(&edit, id, DF_ATTR_DOCUMENT_STATE, df_string((const char *)text.bytes, (size_t)text.len));
    df_status status = df_edit_apply(s->db, &edit);
    df_edit_free(&edit);
    if (status != DF_OK) janet_panic(df_store_error(s->db));
    return janet_wrap_number(id);
}
static Janet records(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    StoreData *s = get_store(argv, 0);
    df_view *view = s->watch->result;
    df_entities ids = {0};
    if (df_view_entities(view, 0, 0, &ids) != DF_OK) {
        df_entities_free(&ids); janet_panic("cannot enumerate canvas records");
    }
    JanetArray *result = janet_array((int32_t)ids.count);
    for (size_t i = 0; i < ids.count; ++i) {
        df_slice encoded, text;
        if (df_view_get(view, ids.data[i], DF_ATTR_DOCUMENT_STATE, &encoded) != DF_OK ||
            df_value_text(encoded, &text) != DF_OK || text.size > INT32_MAX) {
            df_entities_free(&ids); janet_panic("invalid persisted canvas record");
        }
        Janet pair[2] = {janet_wrap_number(ids.data[i]), janet_stringv(text.data, (int32_t)text.size)};
        janet_array_push(result, janet_wrap_tuple(janet_tuple_n(pair, 2)));
    }
    df_entities_free(&ids);
    return janet_wrap_array(result);
}
static Janet store_changed(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1); StoreData *s = get_store(argv, 0);
    bool dirty = s->dirty; s->dirty = false; return janet_wrap_boolean(dirty);
}
static Janet close_store(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    Store *wrapper = janet_getabstract(argv, 0, &store_type);
    StoreData *s = get_store(argv, 0);
    if (s->attached) janet_panic("close the network before its stores");
    df_buffer error = {0};
    df_watch_remove(s->db, &s->watch);
    df_status status = df_store_close(s->db, &error);
    wrapper->data = NULL; free(s);
    if (status != DF_OK) owned_error(error);
    df_free(error.data);
    return janet_wrap_nil();
}
static Janet identity_open(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 2);
    JanetByteView alias = janet_getbytes(argv, 0);
    int ephemeral = argc == 2 && janet_getboolean(argv, 1);
    Identity *id = janet_abstract(&identity_type, sizeof(Identity));
    id->data = NULL;
    IdentityData *data = calloc(1, sizeof(*data));
    if (!data) janet_panic("out of memory");
    df_buffer error = {0};
    data->handle = df_identity_open(alias.bytes, (size_t)alias.len, ephemeral, &error);
    if (!data->handle) { free(data); owned_error(error); }
    df_free(error.data); data->refs = 1; id->data = data;
    return janet_wrap_abstract(id);
}
static Janet identity_key(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    Identity *id = janet_getabstract(argv, 0, &identity_type);
    if (!id->data) janet_panic("identity is closed");
    const uint8_t *pub = df_identity_public(id->data->handle);
    char hex[65];
    for (int i = 0; i < 32; ++i) { hex[2*i] = "0123456789abcdef"[pub[i] >> 4]; hex[2*i+1] = "0123456789abcdef"[pub[i] & 15]; }
    hex[64] = 0; return janet_cstringv(hex);
}
static Janet network_open(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    const char *address = janet_getcstring(argv, 0);
    int32_t port = janet_getinteger(argv, 1);
    if (port < 1 || port > 65535) janet_panic("invalid sync port");
    Network *n = janet_abstract(&network_type, sizeof(Network));
    memset(n, 0, sizeof(*n));
    df_buffer error = {0};
    n->net = df_network_open(address, (uint16_t)port, &error);
    if (!n->net) owned_error(error);
    df_free(error.data); return janet_wrap_abstract(n);
}
static Janet attach(int32_t argc, Janet *argv) {
    janet_arity(argc, 2, 3);
    Network *n = get_network(argv);
    StoreData *s = get_store(argv, 1);
    IdentityData *id = NULL;
    if (argc == 3) {
        Identity *wrapper = janet_getabstract(argv, 2, &identity_type);
        id = wrapper->data;
        if (!id) janet_panic("identity is closed");
    }
    if (n->count == n->capacity) {
        size_t capacity = n->capacity ? n->capacity * 2 : 4;
        Attachment *items = realloc(n->items, capacity * sizeof(*items));
        if (!items) janet_panic("out of memory");
        n->items = items; n->capacity = capacity;
    }
    df_buffer error = {0};
    uint32_t channel = id ? df_network_push(n->net, s->db, id->handle, &error)
                          : df_network_follow(n->net, s->db, &error);
    if (!channel) owned_error(error);
    df_free(error.data);
    ++s->refs; ++s->attached; if (id) ++id->refs;
    n->items[n->count++] = (Attachment){s, id};
    return janet_wrap_number(channel);
}
static Janet pump(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1); Network *n = get_network(argv);
    df_network_pump(n->net, 2000);
    const df_network_stats *stats = df_network_statistics(n->net);
    JanetTable *out = janet_table(8);
    janet_table_put(out, janet_ckeywordv("connected"), janet_wrap_boolean(df_network_connected(n->net)));
    janet_table_put(out, janet_ckeywordv("settled"), janet_wrap_boolean(df_network_settled(n->net)));
    janet_table_put(out, janet_ckeywordv("error"), janet_cstringv(df_network_error(n->net)));
    janet_table_put(out, janet_ckeywordv("sent"), janet_wrap_number((double)stats->sent));
    janet_table_put(out, janet_ckeywordv("received"), janet_wrap_number((double)stats->received));
    return janet_wrap_table(out);
}
static Janet online(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2); df_network_online(get_network(argv)->net, janet_getboolean(argv, 1)); return janet_wrap_nil();
}
static Janet network_close(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1); collect_network(janet_getabstract(argv, 0, &network_type), 0); return janet_wrap_nil();
}
static const JanetReg functions[] = {
    {"open", open_store, "(store/open directory &opt namespace) Open synchronous persistent storage."},
    {"put", put_record, "(store/put db entity json) Update a record; 0 allocates an entity."},
    {"records", records, "(store/records db) Read live [entity json] pairs."},
    {"changed?", store_changed, "(store/changed? db) Consume its native watch's changed flag."},
    {"close", close_store, "(store/close db) Close after detaching the network."},
    {"identity", identity_open, "(store/identity alias &opt ephemeral) Open a native signing identity."},
    {"public-key", identity_key, "(store/public-key identity) Public namespace, hex encoded."},
    {"network", network_open, "(store/network numeric-address port) Native TCP sync driver."},
    {"attach", attach, "(store/attach network db &opt identity) Push with identity; follow without one."},
    {"pump", pump, "(store/pump network) Progress native sync and watches on this thread."},
    {"online", online, "(store/online network enabled) Disconnect/reconnect for offline testing."},
    {"network-close", network_close, "(store/network-close network) Detach before closing stores."},
    {NULL, NULL, NULL}
};
JANET_MODULE_ENTRY(JanetTable *env) { janet_cfuns(env, "canvas-store", functions); }
