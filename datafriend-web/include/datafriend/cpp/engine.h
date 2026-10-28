#pragma once
// Optional ownership/exception adapter. Persistence, indexes, evaluation,
// publication and reconciliation live entirely in the C99 library.
#include "../attributes.h"
#include "../wire_codec.h"
#include <ankerl/unordered_dense.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <stdexcept>
#include <map>
#include <functional>
#include <utility>
#include <cstring>
namespace datafriend {
using bytes = std::string;
namespace detail {
struct buffer {
  df_buffer value{};
  ~buffer() { df_free(value.data); }
  std::string_view view() const {
    return {value.data ? reinterpret_cast<const char *>(value.data) : "", value.size};
  }
  bytes take() const { return bytes(view()); }
};
inline void check(int rc, const buffer &b) {
  if (rc)
    throw std::runtime_error(b.take());
}
inline std::vector<uint64_t> changes(std::string_view b) {
  reader r{b};
  auto n = r.count(8);
  std::vector<uint64_t> ids;
  ids.reserve(n);
  while (n--)
    ids.push_back(r.u64());
  r.end();
  return ids;
}
inline void valid_entity(uint64_t id) {
  if (id > UINT32_MAX)
    throw std::invalid_argument("entity ID exceeds namespace uint32 range");
}
} // namespace detail
struct triple {
  uint64_t entity;
  std::string attribute;
  bytes value;
  bool operator==(const triple &) const = default;
};
struct boxed_value {
  void *handle;
  std::string_view value;
  uint64_t hash;
  explicit boxed_value(void *p) : handle(p) {
    const uint8_t *b;
    size_t n;
    df_box_value(p, &b, &n, &hash);
    value = {reinterpret_cast<const char *>(b), n};
    df_box_keep(p);
  }
  ~boxed_value() { df_box_drop(handle); }
  boxed_value(const boxed_value &) = delete;
};
using value_ref = std::shared_ptr<const boxed_value>;
struct value_hash {
  size_t operator()(const value_ref &v) const noexcept { return v->hash; }
};
struct value_equal {
  bool operator()(const value_ref &a, const value_ref &b) const noexcept {
    return a == b || a->value == b->value;
  }
};
class view_scope {
  struct boxes {
    ankerl::unordered_dense::map<void *, std::weak_ptr<const boxed_value>> values;
  };
  std::shared_ptr<boxes> boxes_ = std::make_shared<boxes>();

public:
  const std::string name;
  explicit view_scope(std::string n = "default") : name(std::move(n)) {}
  std::optional<uint32_t> find(std::string_view name) const {
    for (auto a : attribute_definitions)
      if (a.name == name)
        return a.id;
    return {};
  }
  std::string name_of(uint32_t id) const {
    for (auto a : attribute_definitions)
      if (a.id == id)
        return std::string(a.name);
    throw std::out_of_range("unknown shared attribute");
  }
  uint64_t next_id() const {
    uint64_t n = 1;
    for (auto a : attribute_definitions)
      n = std::max(n, uint64_t(a.id) + 1);
    return n;
  }
  uint32_t intern(std::string name) {
    auto id = find(name);
    if (!id)
      throw std::invalid_argument("unknown shared attribute");
    return *id;
  }
  value_ref box(void *p) const {
    auto it = boxes_->values.find(p);
    if (it != boxes_->values.end())
      if (auto v = it->second.lock())
        return v;
    std::weak_ptr<boxes> cache = boxes_;
    value_ref v(new boxed_value(p), [cache, p](const boxed_value *b) {
      if (auto c = cache.lock())
        c->values.erase(p);
      delete b;
    });
    boxes_->values[p] = v;
    return v;
  }
};
inline uint64_t pair_key(uint32_t e, uint32_t a) noexcept {
  return uint64_t(e) << 32 | a;
}
inline uint64_t triple_hash(uint64_t key, const boxed_value &value) {
  writer w;
  w.u64(key);
  w.u64(value.hash);
  return df_hash_bytes(w.data.data(), w.data.size());
}
class query_result {
  std::shared_ptr<void> handle_;
  std::shared_ptr<view_scope> scope_;
  mutable std::string_view borrowed_;
  void check() const {
    if (!df_view_valid(handle_.get()))
      throw std::runtime_error("Datafriend recovery failed; reopen store");
  }

public:
  struct entry {
    uint64_t key;
    value_ref value;
  };
  struct change_summary {
    ankerl::unordered_dense::set<uint32_t> entities, attributes;
    bool membership = false, reset = true;
    void clear() {
      entities.clear();
      attributes.clear();
      membership = reset = false;
    }
  };

private:
  mutable change_summary changes_;

public:
  explicit query_result(std::shared_ptr<view_scope> scope = std::make_shared<view_scope>())
      : scope_(std::move(scope)) {
    auto *p =
        df_view_empty(reinterpret_cast<const uint8_t *>(scope_->name.data()), scope_->name.size());
    if (!p)
      throw std::bad_alloc();
    handle_ = {p, df_result_release};
  }
  query_result(void *owned, std::shared_ptr<view_scope> scope)
      : handle_(owned, df_result_release), scope_(std::move(scope)) {}
  void *native_handle() const { return handle_.get(); }
  auto scope() const { return scope_; }
  bool shares_with(const query_result &o) const {
    return df_result_same(native_handle(), o.native_handle());
  }
  uint64_t size() const {
    check();
    return df_view_size(native_handle());
  }
  bool empty() const { return size() == 0; }
  const change_summary &changes() const {
    detail::buffer b;
    detail::check(df_result_changes(native_handle(), &b.value), b);
    reader r{b.view()};
    changes_.clear();
    changes_.reset = r.u8();
    changes_.membership = r.u8();
    auto n = r.count(4);
    while (n--)
      changes_.entities.insert(r.u32());
    n = r.count(4);
    while (n--)
      changes_.attributes.insert(scope_->intern(r.blob()));
    r.end();
    return changes_;
  }
  std::optional<entry> get(uint64_t key) const {
    check();
    auto *v = df_view_box(native_handle(), key);
    if (!v)
      return {};
    return entry{key, scope_->box(v)};
  }
  auto lookup(uint32_t entity, std::string_view attribute) const {
    auto a = scope_->find(attribute);
    return a ? get(pair_key(entity, *a)) : std::optional<entry>{};
  }
  triple decode(const entry &e) const {
    return {e.key >> 32, scope_->name_of(uint32_t(e.key)), bytes(e.value->value)};
  }
  template <class F> void for_each(F &&f) const {
    check();
    auto n = size();
    for (size_t i = 0; i < n; ++i) {
      uint64_t key;
      void *v = df_view_at(native_handle(), i, &key);
      f(entry{key, scope_->box(v)});
    }
  }
  // No copy of value bytes. Consume the descriptor before another value() call;
  // its bytes remain borrowed until the next edit or release of the result.
  const std::string_view *value(uint32_t e, uint32_t a) const {
    df_buffer b{};
    int rc = df_result_value(native_handle(), e, a, &b);
    if (rc < 0)
      throw std::runtime_error("invalid result");
    if (rc)
      return nullptr;
    borrowed_ = {reinterpret_cast<const char *>(b.data), b.size};
    return &borrowed_;
  }
  std::vector<uint32_t> entities(uint32_t order = 0, uint32_t secondary = 0) const {
    detail::buffer b;
    detail::check(df_result_entities(native_handle(), order, secondary, &b.value), b);
    std::vector<uint32_t> ids(b.value.size / 4);
    if (!ids.empty())
      std::memcpy(ids.data(), b.value.data, b.value.size);
    return ids;
  }
  std::vector<std::string> attribute_names(uint32_t entity) const {
    detail::buffer b;
    detail::check(df_result_attributes(native_handle(), entity, &b.value), b);
    reader r{b.view()};
    auto n = r.count(4);
    std::vector<std::string> names;
    while (n--)
      names.push_back(r.blob());
    r.end();
    return names;
  }
  std::optional<uint32_t> find_entity(uint32_t a, std::string_view v) const {
    auto id =
        df_result_find(native_handle(), a, reinterpret_cast<const uint8_t *>(v.data()), v.size());
    if (id == -2)
      throw std::runtime_error("invalid result");
    return id < 0 ? std::optional<uint32_t>{} : uint32_t(id);
  }
  uint64_t count_value(uint32_t a, std::string_view v) const {
    auto n = df_result_count_value(native_handle(), a, reinterpret_cast<const uint8_t *>(v.data()),
                                   v.size());
    if (n < 0)
      throw std::runtime_error("invalid result");
    return uint64_t(n);
  }
  bytes encode() const {
    detail::buffer b;
    detail::check(df_result_list(native_handle(), &b.value), b);
    return b.take();
  }
};
using entity_record = std::map<std::string, bytes>;
struct term {
  int32_t variable = -1;
  bytes constant;
};
struct clause {
  term entity;
  std::string attribute;
  term value;
};
struct query {
  std::vector<clause> clauses;
  std::vector<uint32_t> projection;
  bool all = false;
};
struct edit {
  std::vector<triple> remove, add;
};
inline bytes entity_value(uint64_t id) {
  detail::valid_entity(id);
  writer w;
  w.u8(6);
  w.u64(id);
  return w.data;
}
inline bytes encode_fact(uint64_t key, std::string_view v) {
  if (v.size() > 65527)
    throw std::length_error("oversized value");
  writer w;
  w.u64(df_hash_record(key, v.data(), v.size()));
  w.data.append(v);
  return w.data;
}
inline bytes decode_fact(uint64_t key, std::string_view record) {
  if (record.size() < 8 || record.size() > 65535)
    throw std::length_error("invalid fact record size");
  reader r{record};
  auto hash = r.u64();
  if (hash != df_hash_record(key, r.data.data(), r.data.size()))
    throw std::invalid_argument("fact fingerprint mismatch");
  return bytes(r.data);
}
inline bytes encode_entity(uint64_t id, const entity_record &record) {
  detail::valid_entity(id);
  writer w;
  w.u8(1);
  w.u32(uint32_t(record.size()));
  for (auto &[a, v] : record) {
    if (a.empty() || a.size() > 1024 || v.size() > 65527)
      throw std::length_error("invalid attribute or oversized value");
    w.blob(a);
    w.blob(v);
  }
  writer out;
  out.u64(df_hash_record(id, w.data.data(), w.data.size()));
  out.data += w.data;
  return out.data;
}
inline entity_record decode_entity(uint64_t id, std::string_view input) {
  detail::valid_entity(id);
  if (input.size() > 64 * 1024 * 1024)
    throw std::length_error("oversized entity export");
  reader r{input};
  auto hash = r.u64();
  if (hash != df_hash_record(id, r.data.data(), r.data.size()))
    throw std::invalid_argument("entity fingerprint mismatch");
  if (r.u8() != 1)
    throw std::invalid_argument("unsupported entity codec");
  entity_record out;
  std::string previous;
  auto n = r.count(8);
  for (uint32_t i = 0; i < n; ++i) {
    auto a = r.blob(), v = r.blob();
    if (a.empty() || a.size() > 1024 || v.size() > 65527 || (i && a <= previous))
      throw std::invalid_argument("invalid/noncanonical attributes");
    previous = a;
    out.emplace(std::move(a), std::move(v));
  }
  r.end();
  return out;
}
enum class durability { buffered = 0, synchronous = 1, asynchronous = 2 };
class engine {
  void *handle_ = nullptr;
  std::shared_ptr<view_scope> scope_;
  static writer request(unsigned op) {
    writer w;
    w.u8(1);
    w.u8(uint8_t(op));
    return w;
  }
  bytes call(const writer &w) {
    detail::buffer b;
    detail::check(
        df_call(handle_, reinterpret_cast<const uint8_t *>(w.data.data()), w.data.size(), &b.value),
        b);
    return b.take();
  }
  static void write_term(writer &w, const term &t) {
    if (t.variable < -1 || t.variable >= 64)
      throw std::invalid_argument("query variable outside 0..63");
    w.u8(t.variable >= 0);
    if (t.variable >= 0)
      w.u8(uint8_t(t.variable));
    else
      w.blob(t.constant);
  }
  static void write_edit(writer &w, const edit &e) {
    for (auto *list : {&e.remove, &e.add}) {
      w.u32(uint32_t(list->size()));
      for (const auto &t : *list) {
        detail::valid_entity(t.entity);
        if (t.attribute.empty() || t.attribute.size() > 1024 || t.value.size() > 65527)
          throw std::length_error("invalid attribute or oversized value");
        w.u64(t.entity);
        w.blob(t.attribute);
        w.blob(t.value);
      }
    }
  }
  std::vector<uint64_t> import(unsigned op,
                               const std::map<uint64_t, std::optional<bytes>> &records) {
    auto w = request(op);
    w.u32(uint32_t(records.size()));
    for (auto &[id, v] : records) {
      w.u64(id);
      w.u8(v.has_value());
      if (v)
        w.blob(*v);
    }
    return detail::changes(call(w));
  }
  std::map<uint64_t, bytes> export_records(unsigned op) {
    auto b = call(request(op));
    reader r{b};
    std::map<uint64_t, bytes> out;
    auto n = r.count(12);
    while (n--) {
      auto id = r.u64();
      out.emplace(id, r.blob());
    }
    r.end();
    return out;
  }

public:
  explicit engine(const std::string &path, durability mode = durability::asynchronous,
                  size_t map_bytes = size_t(1) << 30, std::string name = "default")
      : scope_(std::make_shared<view_scope>(name)) {
    detail::buffer b;
    handle_ = df_open_sized(reinterpret_cast<const uint8_t *>(path.data()), path.size(),
                            reinterpret_cast<const uint8_t *>(name.data()), name.size(), int(mode),
                            map_bytes, &b.value);
    if (!handle_)
      throw std::runtime_error(b.take());
  }
  engine(const std::string &path, bool durable, size_t map_bytes = size_t(1) << 30,
         std::string name = "default")
      : engine(path, durable ? durability::synchronous : durability::buffered, map_bytes,
               std::move(name)) {}
  ~engine() { df_close(handle_); }
  engine(const engine &) = delete;
  engine &operator=(const engine &) = delete;
  void *native_handle() const { return handle_; }
  std::pair<uint64_t, query_result> subscribe(query q) {
    auto w = request(1);
    w.u8(q.all);
    w.u32(uint32_t(q.clauses.size()));
    for (auto &c : q.clauses) {
      write_term(w, c.entity);
      w.blob(c.attribute);
      write_term(w, c.value);
    }
    w.u32(uint32_t(q.projection.size()));
    for (auto p : q.projection)
      w.u32(p);
    auto b = call(w);
    reader r{b};
    auto id = r.u64();
    return {id, result(id)};
  }
  void unsubscribe(uint64_t id) {
    auto w = request(4);
    w.u64(id);
    call(w);
  }
  query_result result(uint64_t id) {
    detail::buffer b;
    auto *p = df_result(handle_, id, &b.value);
    if (!p)
      throw std::runtime_error(b.take());
    return {p, scope_};
  }
  std::vector<uint64_t> modify(edit e) {
    auto w = request(10);
    write_edit(w, e);
    return detail::changes(call(w));
  }
  std::vector<uint64_t> modify(uint64_t id, edit e) {
    auto w = request(3);
    w.u64(id);
    write_edit(w, e);
    return detail::changes(call(w));
  }
  std::vector<uint64_t> modify_many(std::vector<std::pair<uint64_t, edit>> batches) {
    auto w = request(8);
    w.u32(uint32_t(batches.size()));
    for (auto &[id, e] : batches) {
      w.u64(id);
      write_edit(w, e);
    }
    return detail::changes(call(w));
  }
  uint64_t new_entity() {
    auto b = call(request(5));
    return reader{b}.u64();
  }
  void flush() { call(request(11)); }
  std::vector<uint64_t> import_facts(std::map<uint64_t, std::optional<bytes>> r) {
    return import(13, r);
  }
  std::map<uint64_t, bytes> export_facts() { return export_records(12); }
  std::vector<uint64_t> import_entities(std::map<uint64_t, std::optional<bytes>> r) {
    return import(7, r);
  }
  std::map<uint64_t, bytes> export_entities() { return export_records(6); }
  uint64_t revision() { return df_revision(handle_); }
  uint64_t last_recomputed() { return df_recomputed(handle_); }
  std::string_view namespace_name() const { return scope_->name; }
  bytes sync(unsigned op, std::string_view packet = {}) {
    auto w = request(14);
    w.u8(uint8_t(op));
    w.blob(packet);
    return call(w);
  }
  std::vector<uint64_t> sync_apply(std::string_view packet) {
    auto w = request(15);
    w.blob(packet);
    return detail::changes(call(w));
  }
  void sync_track() { df_track(handle_); }
  std::optional<bytes> sync_delta(uint64_t after) {
    detail::buffer b;
    int rc = df_delta(handle_, after, &b.value);
    if (rc == 2)
      return {};
    detail::check(rc, b);
    return b.take();
  }
  std::optional<std::vector<uint64_t>> sync_apply_delta(std::string_view packet) {
    detail::buffer b;
    int rc = df_apply_delta(handle_, reinterpret_cast<const uint8_t *>(packet.data()),
                            packet.size(), &b.value);
    if (rc == 2)
      return {};
    detail::check(rc, b);
    return detail::changes(b.view());
  }
#ifdef DATAFRIEND_TESTING
  void fail_next_commit_for_testing() { df_test_commit_failure(handle_); }
  void fail_flush_for_testing(bool fail) { df_test_flush_failure(handle_, fail); }
  uint64_t flush_attempts_for_testing() { return df_test_flush_attempts(handle_); }
#endif
};
} // namespace datafriend
