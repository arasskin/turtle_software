#pragma once
// Test fixtures and protocol payloads. Production operations use the C API.
#include "../../littlebear/codec.h"
#include "datafriend/db.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
inline void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct directory {
  std::string path;
  directory() {
    char p[] = "/tmp/datafriend-test-XXXXXX";
    auto d = mkdtemp(p);
    require(d, "mkdtemp");
    path = d;
  }
  ~directory() { std::filesystem::remove_all(path); }
};
inline std::string text(lb_bytes b) {
  return {b.data ? reinterpret_cast<const char *>(b.data) : "", b.size};
}
inline lb_bytes span(std::string_view b) { return lb_bytes_make(b.data(), b.size()); }
inline df_value raw(std::string_view b) { return df_encoded(b.data(), b.size()); }
inline uint64_t pair_key(uint32_t e, uint32_t a) { return uint64_t(e) << 32 | a; }
inline df_store *open_store(const std::string &path, df_durability mode = DF_BUFFERED,
                            size_t bytes = 1 << 26, const char *name = "default") {
  df_options options{path.c_str(), name, mode, bytes};
  df_buffer error{};
  auto db = df_store_open(&options, &error);
  std::string message = text(lb_bytes_make(error.data, error.size));
  df_free(error.data);
  if (!db)
    throw std::runtime_error(message);
  return db;
}
inline void close_store(df_store *db) {
  df_buffer error{};
  auto rc = df_store_close(db, &error);
  df_free(error.data);
  require(rc == DF_OK, "close store");
}
inline std::string call(df_store *db, lb_buffer *w) {
  require(!w->failed, "request allocation");
  df_buffer reply{};
  int rc = df_call(db, w->data, w->size, &reply);
  lb_buffer_free(w);
  auto result = text(lb_bytes_make(reply.data, reply.size));
  df_free(reply.data);
  if (rc)
    throw std::runtime_error(result);
  return result;
}
inline lb_buffer request(unsigned op) {
  lb_buffer w{};
  lb_u8(&w, 1);
  lb_u8(&w, op);
  return w;
}
inline std::vector<uint64_t> changes(std::string_view packet) {
  lb_reader r{span(packet), false};
  std::vector<uint64_t> out;
  auto n = lb_read_u32(&r);
  while (n--)
    out.push_back(lb_read_u64(&r));
  require(lb_reader_end(&r), "changes packet");
  return out;
}
inline std::string sync(df_store *db, unsigned op, std::string_view packet = {}) {
  auto w = request(14);
  lb_u8(&w, op);
  lb_blob(&w, span(packet));
  return call(db, &w);
}
inline std::vector<uint64_t> sync_apply(df_store *db, std::string_view packet) {
  auto w = request(15);
  lb_blob(&w, span(packet));
  return changes(call(db, &w));
}
using records = std::map<uint64_t, std::string>;
inline records export_records(df_store *db, unsigned op = 12) {
  auto w = request(op);
  auto packet = call(db, &w);
  lb_reader r{span(packet), false};
  records out;
  auto n = lb_read_u32(&r);
  while (n--) {
    auto key = lb_read_u64(&r);
    out.emplace(key, text(lb_read_blob(&r)));
  }
  require(lb_reader_end(&r), "export packet");
  return out;
}
inline std::vector<uint64_t>
import_records(df_store *db, const std::map<uint64_t, std::optional<std::string>> &records,
               unsigned op = 13) {
  auto w = request(op);
  lb_u32(&w, records.size());
  for (auto &[key, value] : records) {
    lb_u64(&w, key);
    lb_u8(&w, value.has_value());
    if (value)
      lb_blob(&w, span(*value));
  }
  return changes(call(db, &w));
}
inline std::string fact_record(uint64_t key, std::string_view value) {
  lb_buffer w{};
  lb_u64(&w, df_hash_record(key, value.data(), value.size()));
  lb_append(&w, value.data(), value.size());
  require(!w.failed, "fact fixture");
  auto out = text(lb_buffer_bytes(&w));
  lb_buffer_free(&w);
  return out;
}
inline std::string entity_record(uint32_t id, const std::map<std::string, std::string> &fields) {
  lb_buffer w{};
  lb_u8(&w, 1);
  lb_u32(&w, fields.size());
  for (auto &[name, value] : fields) {
    lb_blob(&w, span(name));
    lb_blob(&w, span(value));
  }
  auto out = fact_record(id, text(lb_buffer_bytes(&w)));
  lb_buffer_free(&w);
  return out;
}
struct fact {
  uint32_t entity;
  uint32_t attribute;
  std::string value;
};
// Batch fixture for transport scenarios: exercise the native edit builder.
inline void edit_store(df_store *db, const std::vector<fact> &add,
                       const std::vector<fact> &remove = {}) {
  df_edit e{};
  for (auto &f : remove)
    require(df_edit_retract(&e, f.entity, f.attribute, raw(f.value)), "retract");
  for (auto &f : add)
    require(df_edit_set(&e, f.entity, f.attribute, raw(f.value)), "set");
  auto rc = df_edit_apply(db, &e);
  df_edit_free(&e);
  require(rc == DF_OK, "apply edit");
}
inline bool has_value(df_view *v, uint32_t e, uint32_t a, std::string_view expected) {
  df_slice b{};
  return df_view_get(v, e, a, &b) == DF_OK && text(lb_bytes_make(b.data, b.size)) == expected;
}
inline bool absent(df_view *v, uint32_t e, uint32_t a) {
  df_slice b{};
  return df_view_get(v, e, a, &b) == DF_ABSENT;
}
using facts = std::map<std::pair<uint32_t, uint32_t>, std::string>;
inline facts contents(df_view *v) {
  facts out;
  for (size_t i = 0; i < df_view_size(v); ++i) {
    uint64_t key, hash;
    const uint8_t *bytes;
    size_t size;
    auto box = df_view_at(v, i, &key);
    require(box, "view entry");
    df_box_value(box, &bytes, &size, &hash);
    out[{uint32_t(key >> 32), uint32_t(key)}] = text(lb_bytes_make(bytes, size));
  }
  require(out.size() == df_view_size(v), "view count");
  return out;
}
