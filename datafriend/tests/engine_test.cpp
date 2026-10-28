#include "lmdb.h"
#include "test_support.h"
#include <random>

namespace {
void notified(void *context, df_view *) { ++*static_cast<unsigned *>(context); }
uint32_t attr(std::string_view name) { return df_attribute_id(name.data(), name.size()); }
std::string entity_value(uint32_t id) {
  std::string s(9, 0);
  s[0] = 6;
  for (unsigned i = 0; i < 8; ++i)
    s[8 - i] = char(uint64_t(id) >> (i * 8));
  return s;
}
// Only the independent oracle uses these data descriptions. The database gets
// native df_match arrays, and all subscriptions/edits/reads use the C API.
using bytes = std::string;
using row = std::map<std::string, bytes>;
struct term {
  int variable;
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
term variable(int n) { return {n, {}}; }
term constant(bytes b) { return {-1, std::move(b)}; }
std::vector<df_match> matches(const query &q) {
  std::vector<df_match> out;
  auto convert = [](const term &t) {
    return t.variable < 0 ? df_const(raw(t.constant)) : df_var(t.variable);
  };
  for (size_t i = 0; i < q.clauses.size(); ++i) {
    auto &c = q.clauses[i];
    out.push_back({convert(c.entity), attr(c.attribute), convert(c.value),
                   std::find(q.projection.begin(), q.projection.end(), i) != q.projection.end()});
  }
  return out;
}
facts reference_query(const std::map<uint32_t, row> &records, const query &q) {
  facts out;
  if (q.all) {
    for (auto &[e, r] : records)
      for (auto &[a, v] : r)
        out[{e, attr(a)}] = v;
    return out;
  }
  using bindings = std::map<int, bytes>;
  std::vector<std::pair<std::pair<uint32_t, uint32_t>, bytes>> matched(q.clauses.size());
  auto walk = [&](auto &&self, size_t level, const bindings &bound) -> void {
    if (level == q.clauses.size()) {
      for (auto i : q.projection)
        out[matched[i].first] = matched[i].second;
      return;
    }
    auto &c = q.clauses[level];
    for (auto &[id, r] : records) {
      auto it = r.find(c.attribute);
      if (it == r.end())
        continue;
      auto next = bound;
      auto bind = [&](const term &t, const bytes &v) {
        if (t.variable < 0)
          return t.constant == v;
        auto [position, inserted] = next.emplace(t.variable, v);
        return inserted || position->second == v;
      };
      if (bind(c.entity, entity_value(id)) && bind(c.value, it->second)) {
        matched[level] = {{id, attr(c.attribute)}, it->second};
        self(self, level + 1, next);
      }
    }
  };
  walk(walk, 0, {});
  return out;
}
void semantics_and_sharing() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  df_options duplicate_options{d.path.c_str(), "default", DF_BUFFERED, 0};
  df_buffer error{};
  require(!df_store_open(&duplicate_options, &error), "multiple owners admitted");
  df_free(error.data);
  df_match title[] = {{df_var(0), DF_ATTR_TITLE, df_var(1), true}};
  df_match color[] = {{df_var(0), DF_ATTR_COLOR, df_var(1), true}};
  df_match incomplete[] = {{df_var(0), DF_ATTR_DONE, df_const(raw("false")), true},
                           {df_var(0), DF_ATTR_TITLE, df_var(1), true}};
  df_match joined[] = {{df_var(0), DF_ATTR_OWNER, df_var(1), false},
                       {df_var(1), DF_ATTR_NAME, df_const(raw("Alex")), false},
                       {df_var(0), DF_ATTR_TITLE, df_var(2), true}};
  unsigned first_calls = 0, alias_calls = 0;
  auto all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr);
  auto titles = df_watch_query(db, {title, 1, false}, notified, &first_calls);
  auto alias = df_watch_query(db, {title, 1, false}, notified, &alias_calls);
  auto unrelated = df_watch_query(db, {color, 1, false}, nullptr, nullptr);
  auto open = df_watch_query(db, {incomplete, 2, false}, nullptr, nullptr);
  auto owned = df_watch_query(db, {joined, 3, false}, nullptr, nullptr);
  require(all && titles && alias && unrelated && open && owned, "subscriptions");
  require(titles->id != alias->id && df_result_same(titles->result, alias->result),
          "shared query handles");
  auto retained = open->result;
  df_view_retain(retained);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("first")) &&
              df_edit_set(&edit, 1, DF_ATTR_DONE, raw("false")) &&
              df_edit_set(&edit, 1, DF_ATTR_OWNER, df_entity(10)) &&
              df_edit_set(&edit, 10, DF_ATTR_NAME, raw("Alex")),
          "seed");
  require(df_edit_apply(db, &edit) == DF_OK, "seed commit");
  require(df_view_size(open->result) == 2 && df_view_size(owned->result) == 1 &&
              df_recomputed(db) == 4,
          "join/projection/invalidation");
  auto box = df_view_box(titles->result, pair_key(1, DF_ATTR_TITLE));
  require(box == df_view_box(all->result, pair_key(1, DF_ATTR_TITLE)), "shared value boxes");
  require(df_edit_set(&edit, 1, DF_ATTR_DONE, raw("true")) && df_edit_apply(db, &edit) == DF_OK,
          "predicate edit");
  require(!df_view_size(retained) && df_recomputed(db) == 2, "retained live predicate result");
  require(df_view_box(titles->result, pair_key(1, DF_ATTR_TITLE)) == box, "untouched box replaced");
  require(df_edit_retract(&edit, 1, DF_ATTR_TITLE, raw("stale")) &&
              df_edit_apply(db, &edit) == DF_OK,
          "stale retract");
  require(has_value(titles->result, 1, DF_ATTR_TITLE, "first"), "compare before retract");
  require(df_edit_retract(&edit, 1, DF_ATTR_TITLE, raw("first")) &&
              df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("new")) &&
              df_edit_set(&edit, 2, DF_ATTR_TITLE, raw("second")) &&
              df_edit_apply(db, &edit) == DF_OK,
          "replace/add");
  auto revision = df_revision(db);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("new")) && df_edit_apply(db, &edit) == DF_OK &&
              df_revision(db) == revision,
          "no-op revision");
  require(first_calls == alias_calls, "alias callbacks diverged");
  df_match filtered[] = {{df_var(0), DF_ATTR_TITLE, df_const(raw("new")), true}};
  auto filter = df_watch_query(db, {filtered, 1, false}, nullptr, nullptr);
  require(filter && df_view_size(filter->result) == 1, "constant identity");
  auto before = contents(all->result);
  revision = df_revision(db);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("corrupt")), "failure fixture");
  std::string big(65535, 'x');
  df_edit_set(&edit, 99, DF_ATTR_BIG, raw(big));
  require(df_edit_apply(db, &edit) == DF_ERROR && contents(all->result) == before &&
              df_revision(db) == revision,
          "oversized batch rollback");
  // A late failure restores facts, query indexes, and retained views together.
  df_test_commit_failure(db);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("broken")) &&
              df_edit_set(&edit, 10, DF_ATTR_NAME, raw("Other")),
          "late failure fixture");
  require(df_edit_apply(db, &edit) == DF_ERROR && contents(all->result) == before &&
              df_revision(db) == revision,
          "commit rollback");
  for (auto w : {titles, unrelated, owned}) {
    df_buffer out{};
    require(!df_result_changes(w->result, &out) && out.size && out.data[0] == 1, "recovery reset");
    df_free(out.data);
  }
  require(df_edit_set(&edit, 10, DF_ATTR_NAME, raw("Other")) && df_edit_apply(db, &edit) == DF_OK &&
              !df_view_size(owned->result),
          "join index after recovery");
  df_watch_remove(db, &titles);
  df_watch_remove(db, &titles);
  for (uint32_t i = 0; i < 200; ++i) {
    require(df_edit_set(&edit, 1000 + i, DF_ATTR_TITLE, raw(std::to_string(i))) &&
                df_edit_apply(db, &edit) == DF_OK,
            "serialized edits");
    require(df_view_size(alias->result) == 3 + i, "edit returned before publication");
  }
  require(df_store_flush(db) == DF_OK, "flush");
  df_edit_free(&edit);
  close_store(db);
  require(!df_view_size(retained), "view lifetime after close");
  df_view_release(retained);
  db = open_store(d.path);
  auto once = df_query_once(db, {title, 1, false});
  require(once && df_view_size(once) == 202, "reopen/index rebuild");
  df_view_release(once);
  close_store(db);
}

void routing() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  std::vector<df_watch *> rows;
  unsigned changed = 0, alias_changed = 0;
  for (uint32_t id = 0; id < 1000; ++id) {
    df_match q[] = {{df_id(id), DF_ATTR_TITLE, df_var(0), true},
                    {df_id(id), DF_ATTR_DONE, df_const(raw("false")), false}};
    auto w = df_watch_query(db, {q, 2, false}, notified, &changed);
    require(w, "fixed watch");
    rows.push_back(w);
    require(df_edit_set(&edit, id, DF_ATTR_TITLE, raw("task")) &&
                df_edit_set(&edit, id, DF_ATTR_DONE, raw("false")),
            "fixed seed");
  }
  require(df_edit_apply(db, &edit) == DF_OK, "seed");
  df_match q[] = {{df_id(1), DF_ATTR_TITLE, df_var(0), true},
                  {df_id(1), DF_ATTR_DONE, df_const(raw("false")), false}};
  auto alias = df_watch_query(db, {q, 2, false}, notified, &alias_changed);
  require(alias && df_result_same(alias->result, rows[1]->result), "fixed alias");
  changed = alias_changed = 0;
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("updated")) && df_edit_apply(db, &edit) == DF_OK,
          "fixed edit");
  require(df_recomputed(db) == 1 && changed == 1 && alias_changed == 1, "fixed alias routing");
  df_test_commit_failure(db);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("bad")) && df_edit_apply(db, &edit) == DF_ERROR,
          "fixed failure");
  require(has_value(alias->result, 1, DF_ATTR_TITLE, "updated"), "fixed rollback");
  require(df_edit_set(&edit, 0, DF_ATTR_TITLE, raw("zero")) &&
              df_edit_set(&edit, 2, DF_ATTR_DONE, raw("true")) && df_edit_apply(db, &edit) == DF_OK,
          "zero/batch edit");
  require(df_recomputed(db) == 2 && !df_view_size(rows[2]->result) &&
              has_value(rows[0]->result, 0, DF_ATTR_TITLE, "zero"),
          "fixed zero/batch routing");
  require(df_edit_delete(&edit, 1) && df_edit_apply(db, &edit) == DF_OK && df_recomputed(db) == 1 &&
              !df_view_size(alias->result),
          "fixed deletion");
  df_watch_remove(db, &rows[1]);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("back")) &&
              df_edit_set(&edit, 1, DF_ATTR_DONE, raw("false")) &&
              df_edit_apply(db, &edit) == DF_OK && df_recomputed(db) == 1,
          "fixed alias lifetime");
  df_watch_remove(db, &alias);
  for (auto &w : rows)
    df_watch_remove(db, &w);
  df_match fixed_title[] = {{df_id(5), DF_ATTR_TITLE, df_var(0), true}},
           fixed_done[] = {{df_id(6), DF_ATTR_DONE, df_var(0), true}};
  auto t = df_watch_query(db, {fixed_title, 1, false}, nullptr, nullptr),
       f = df_watch_query(db, {fixed_done, 1, false}, nullptr, nullptr);
  require(t && f, "pair watches");
  require(df_edit_set(&edit, 5, DF_ATTR_DONE, raw("true")) &&
              df_edit_set(&edit, 6, DF_ATTR_TITLE, raw("other")) &&
              df_edit_apply(db, &edit) == DF_OK && df_recomputed(db) == 0,
          "Cartesian product routing");
  df_watch_remove(db, &t);
  df_watch_remove(db, &f);
  df_match join[] = {{df_id(1), DF_ATTR_OWNER, df_var(0), false},
                     {df_var(0), DF_ATTR_NAME, df_var(1), true}};
  auto j = df_watch_query(db, {join, 2, false}, nullptr, nullptr);
  require(j, "join");
  require(df_edit_set(&edit, 1, DF_ATTR_OWNER, df_entity(2000)) &&
              df_edit_set(&edit, 2000, DF_ATTR_NAME, raw("Alex")) &&
              df_edit_apply(db, &edit) == DF_OK,
          "join seed");
  require(df_edit_set(&edit, 2000, DF_ATTR_NAME, raw("New")) && df_edit_apply(db, &edit) == DF_OK &&
              has_value(j->result, 2000, DF_ATTR_NAME, "New"),
          "variable side of fixed join");
  require(df_edit_set(&edit, 2, DF_ATTR_OWNER, df_entity(2000)) &&
              df_edit_apply(db, &edit) == DF_OK && df_recomputed(db) == 0,
          "fixed join over-invalidation");
  require(df_edit_delete(&edit, 2000) && df_edit_apply(db, &edit) == DF_OK &&
              !df_view_size(j->result),
          "joined deletion");
  df_watch_remove(db, &j);
  df_match endpoints[] = {{df_id(0), DF_ATTR_TITLE, df_var(0), true},
                          {df_id(UINT32_MAX), DF_ATTR_TITLE, df_var(1), true}};
  auto ends = df_watch_query(db, {endpoints, 2, false}, nullptr, nullptr);
  require(ends, "endpoints");
  require(df_edit_set(&edit, UINT32_MAX, DF_ATTR_TITLE, raw("last")) &&
              df_edit_apply(db, &edit) == DF_OK && df_view_size(ends->result) == 2,
          "multiple fixed endpoints");
  df_watch_remove(db, &ends);
  // Predicate index: transitions route both old/new groups, even when projected
  // fields and the predicate change in the same transaction.
  std::vector<df_watch *> groups;
  for (uint32_t owner = 0; owner < 64; ++owner) {
    df_match group[] = {{df_var(0), DF_ATTR_OWNER, df_const(df_entity(owner)), false},
                        {df_var(0), DF_ATTR_DONE, df_const(raw("false")), false},
                        {df_var(0), DF_ATTR_X, df_var(1), true}};
    auto w = df_watch_query(db, {group, 3, false}, nullptr, nullptr);
    require(w, "predicate watch");
    groups.push_back(w);
  }
  require(df_edit_set(&edit, 0, DF_ATTR_OWNER, df_entity(0)) &&
              df_edit_set(&edit, 0, DF_ATTR_DONE, raw("false")) &&
              df_edit_set(&edit, 0, DF_ATTR_X, raw("a")) && df_edit_apply(db, &edit) == DF_OK,
          "predicate seed");
  require(df_recomputed(db) == 1 && df_view_size(groups[0]->result) == 1, "predicate seed routing");
  require(df_edit_set(&edit, 0, DF_ATTR_OWNER, df_entity(1)) &&
              df_edit_set(&edit, 0, DF_ATTR_X, raw("moved")) && df_edit_apply(db, &edit) == DF_OK,
          "predicate move");
  require(df_recomputed(db) == 2 && !df_view_size(groups[0]->result) &&
              has_value(groups[1]->result, 0, DF_ATTR_X, "moved"),
          "old/new predicate routing");
  df_test_commit_failure(db);
  require(df_edit_set(&edit, 0, DF_ATTR_OWNER, df_entity(0)) &&
              df_edit_apply(db, &edit) == DF_ERROR,
          "predicate failure");
  require(df_edit_set(&edit, 0, DF_ATTR_X, raw("recovered")) && df_edit_apply(db, &edit) == DF_OK &&
              df_recomputed(db) == 1 && has_value(groups[1]->result, 0, DF_ATTR_X, "recovered"),
          "predicate rollback index");
  require(df_edit_unset(&edit, 0, DF_ATTR_OWNER) && df_edit_apply(db, &edit) == DF_OK &&
              !df_view_size(groups[1]->result),
          "predicate removal");
  require(df_edit_set(&edit, 0, DF_ATTR_OWNER, df_entity(0)) && df_edit_apply(db, &edit) == DF_OK &&
              has_value(groups[0]->result, 0, DF_ATTR_X, "recovered"),
          "added predicate uses existing fields");
  df_watch_remove(db, &groups[0]);
  require(df_edit_set(&edit, 0, DF_ATTR_X, raw("unwatched")) && df_edit_apply(db, &edit) == DF_OK &&
              df_recomputed(db) == 0,
          "predicate unsubscribe");
  df_edit_free(&edit);
  close_store(db);
}

void check_changes(df_view *view, const facts &before, const facts &after) {
  std::set<uint32_t> entities, attributes, old_members, new_members;
  auto differences = [&](const facts &a, const facts &b) {
    for (auto &[key, value] : a) {
      auto p = b.find(key);
      if (p == b.end() || p->second != value) {
        entities.insert(key.first);
        attributes.insert(key.second);
      }
    }
  };
  differences(before, after);
  differences(after, before);
  for (auto &[key, value] : before)
    old_members.insert(key.first);
  for (auto &[key, value] : after)
    new_members.insert(key.first);
  df_buffer out{};
  require(!df_result_changes(view, &out), "change summary");
  lb_reader r{lb_bytes_make(out.data, out.size), false};
  require(lb_read_u8(&r) == 0, "normal edit requested reset");
  require(bool(lb_read_u8(&r)) == (old_members != new_members), "membership summary");
  std::set<uint32_t> actual_entities, actual_attributes;
  auto n = lb_read_u32(&r);
  while (n--)
    actual_entities.insert(lb_read_u32(&r));
  n = lb_read_u32(&r);
  while (n--) {
    auto name = lb_read_blob(&r);
    actual_attributes.insert(df_attribute_id(name.data, name.size));
  }
  require(lb_reader_end(&r) && actual_entities == entities && actual_attributes == attributes,
          "changed entities/attributes");
  df_free(out.data);
}
void incremental_queries() {
  directory d;
  auto db = open_store(d.path);
  const std::vector<query> queries = {
      // Entity-local projections, predicates, equality, self references, repeated
      // clauses/projections, fixed entity zero and the maximum entity ID.
      {{{variable(0), "kind", constant("task")},
        {variable(0), "title", variable(1)},
        {variable(0), "done", variable(2)}},
       {0, 1, 2}},
      {{{variable(0), "done", constant("false")}, {variable(0), "title", variable(1)}}, {1}},
      {{{variable(0), "a", variable(1)}, {variable(0), "b", variable(1)}}, {0, 1, 1}},
      {{{variable(0), "owner", variable(0)}, {variable(0), "title", variable(1)}}, {1}},
      {{{constant(entity_value(0)), "title", variable(1)},
        {constant(entity_value(0)), "done", constant("false")}},
       {0}},
      {{{constant(entity_value(UINT32_MAX)), "title", variable(1)}}, {0}},
      {{{variable(0), "title", variable(1)}, {variable(0), "title", variable(1)}}, {0, 1}},
      {{{variable(0), "kind", constant("task")}, {variable(0), "done", variable(1)}}, {0}},
      // These must retain full evaluation: edits of one entity change facts
      // projected from a different entity, including multiple witnesses.
      {{{variable(0), "owner", variable(1)},
        {variable(1), "name", constant("Alex")},
        {variable(0), "title", variable(2)}},
       {2}},
      {{{variable(0), "title", variable(1)}, {variable(2), "name", constant("Alex")}}, {0, 0}},
      {{{constant(entity_value(0)), "title", variable(1)},
        {constant(entity_value(10)), "name", constant("Alex")}},
       {0}},
      {{{constant(entity_value(0)), "owner", variable(0)}, {variable(0), "name", variable(1)}},
       {1}},
  };
  std::vector<df_watch *> watches;
  std::vector<unsigned> calls(queries.size());
  for (size_t i = 0; i < queries.size(); ++i) {
    auto native = matches(queries[i]);
    auto w = df_watch_query(db, {native.data(), native.size(), false}, notified, &calls[i]);
    require(w, "query watch");
    watches.push_back(w);
  }
  auto native = matches(queries[0]);
  unsigned alias_calls = 0;
  auto alias = df_watch_query(db, {native.data(), native.size(), false}, notified, &alias_calls);
  require(alias && df_result_same(alias->result, watches[0]->result), "query alias");
  std::map<uint32_t, row> records;
  std::vector<facts> previous(queries.size());
  auto check = [&] {
    for (size_t i = 0; i < queries.size(); ++i) {
      auto expected = reference_query(records, queries[i]), actual = contents(watches[i]->result);
      require(actual == expected, "incremental query differs from independent backtracking oracle");
      auto native = matches(queries[i]);
      auto once = df_query_once(db, {native.data(), native.size(), false});
      require(once && contents(once) == expected, "incremental differs from full evaluation");
      df_view_release(once);
      bool changed = actual != previous[i];
      require(calls[i] == unsigned(changed), "callback differs from result changes");
      if (changed)
        check_changes(watches[i]->result, previous[i], actual);
      previous[i] = actual;
      calls[i] = 0;
      if (i == 0)
        require(alias_calls == unsigned(changed), "alias callback");
    }
    alias_calls = 0;
  };
  std::fill(calls.begin(), calls.end(), 0);
  alias_calls = 0;
  df_edit edit{};
  auto set = [&](uint32_t id, const std::string &a, const std::string &v) {
    require(df_edit_set(&edit, id, attr(a), raw(v)), "model edit");
    records[id][a] = v;
  };
  auto unset = [&](uint32_t id, const std::string &a) {
    require(df_edit_unset(&edit, id, attr(a)), "model unset");
    records[id].erase(a);
  };
  auto commit = [&] {
    require(df_edit_apply(db, &edit) == DF_OK, "model commit");
    check();
  };
  for (auto &[id, fields] : std::map<uint32_t, row>{
           {0,
            {{"title", "zero"}, {"done", "false"}, {"kind", "task"}, {"owner", entity_value(10)}}},
           {1, {{"title", "one"}, {"done", "false"}, {"kind", "task"}}},
           {10, {{"name", "Alex"}}},
           {11, {{"name", "Alex"}}},
           {UINT32_MAX, {{"title", "last"}}}})
    for (auto &[a, v] : fields)
      set(id, a, v);
  commit();
  set(0, "done", "true");
  set(1, "done", "true");
  commit();
  unset(1, "title");
  set(0, "done", "false");
  commit();
  for (uint32_t id : {10U, UINT32_MAX, 11U, 0U}) {
    require(df_edit_delete(&edit, id), "model delete");
    records.erase(id);
  }
  commit();
  for (auto &[a, v] : row{{"title", "zero"},
                          {"owner", entity_value(0)},
                          {"a", "same"},
                          {"b", "same"},
                          {"kind", "task"},
                          {"done", "false"}})
    set(0, a, v);
  commit();
  set(0, "b", "different");
  commit();
  set(0, "unrelated", "x");
  commit();
  require(df_edit_retract(&edit, 0, DF_ATTR_TITLE, raw("stale")), "stale fixture");
  set(0, "title", "zero");
  commit();
  std::mt19937 random(785);
  const std::array<std::string, 8> attributes = {"title", "done",  "kind", "a",
                                                 "b",     "owner", "name", "unrelated"};
  for (size_t step = 0; step < 120; ++step) {
    if (step % 4 == 0) {
      std::map<uint64_t, std::optional<std::string>> imports;
      for (unsigned j = 0; j < 3; ++j) {
        uint32_t id = random() % 16;
        if (random() % 3 == 0) {
          imports[id] = std::nullopt;
          records.erase(id);
        } else {
          row fields = {{"title", std::to_string(random() % 4)},
                        {"done", random() % 2 ? "true" : "false"},
                        {"kind", "task"},
                        {"owner", entity_value(random() % 16)},
                        {"name", random() % 2 ? "Alex" : "Other"}};
          imports[id] = entity_record(id, fields);
          records[id] = fields;
        }
      }
      import_records(db, imports, 7);
      check();
    } else {
      for (unsigned j = 0; j < 3; ++j) {
        uint32_t id = random() % 16;
        auto a = attributes[random() % attributes.size()];
        std::string v = a == "owner"  ? entity_value(random() % 16)
                        : a == "kind" ? "task"
                        : a == "done" ? (random() % 2 ? "true" : "false")
                        : a == "name" ? (random() % 2 ? "Alex" : "Other")
                                      : std::to_string(random() % 4);
        if (random() % 3 == 0) {
          require(df_edit_retract(&edit, id, attr(a), raw(v)), "model retract");
          auto it = records[id].find(a);
          if (it != records[id].end() && it->second == v)
            records[id].erase(it);
        } else
          set(id, a, v);
      }
      commit();
    }
  }
  df_edit_free(&edit);
  close_store(db);
}
void planned_queries() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  std::map<uint32_t, row> records;
  for (uint32_t id = 0; id < 12; ++id)
    records[id] = {{"title", std::to_string(id)},
                   {"kind", id % 2 ? "task" : "person"},
                   {"group", std::to_string(id % 3)},
                   {"mirror", std::to_string(id % 2)},
                   {"parent", entity_value((id + 1) % 12)},
                   {"other", entity_value((id + 2) % 12)},
                   {"self", entity_value(id)}};
  for (auto &[id, fields] : records)
    for (auto &[a, v] : fields)
      require(df_edit_set(&edit, id, attr(a), raw(v)), "planner seed");
  require(df_edit_apply(db, &edit) == DF_OK, "planner commit");
  std::vector<query> queries = {
      // Opposite join direction, cycle, shared non-reference value, and a variable
      // appearing as both an entity and a value inside one grouped gather.
      {{{variable(2), "title", variable(3)},
        {variable(1), "parent", variable(2)},
        {variable(0), "parent", variable(1)},
        {variable(0), "title", constant("0")}},
       {0, 1, 2, 3}},
      {{{variable(0), "parent", variable(1)},
        {variable(1), "parent", variable(2)},
        {variable(0), "other", variable(2)}},
       {0, 1, 2}},
      {{{variable(0), "group", variable(2)},
        {variable(1), "mirror", variable(2)},
        {variable(0), "title", variable(3)},
        {variable(1), "kind", constant("person")}},
       {2, 1}},
      {{{variable(0), "self", variable(0)},
        {variable(0), "group", variable(1)},
        {variable(0), "mirror", variable(1)}},
       {0, 1, 2}},
      {{{variable(0), "group", variable(63)}, {variable(0), "mirror", variable(63)}}, {0, 1}},
      {{{variable(0), "title", variable(1)}, {variable(0), "missing", variable(2)}}, {0}},
  };
  std::mt19937 random(854);
  for (auto &original : queries) {
    auto expected = reference_query(records, original);
    for (unsigned trial = 0; trial < 20; ++trial) {
      auto native = matches(original);
      std::shuffle(native.begin(), native.end(), random);
      auto result = df_query_once(db, {native.data(), native.size(), false});
      require(result && contents(result) == expected, "clause permutation/planner");
      df_view_release(result);
    }
  }
  std::vector<df_match> wide;
  for (uint32_t i = 0; i < 32; ++i) {
    auto a = attr("field" + std::to_string(i));
    require(df_edit_set(&edit, i, a, raw("v")), "wide seed");
    wide.push_back({df_id(i), a, df_var(i), i == 31});
  }
  require(df_edit_apply(db, &edit) == DF_OK, "wide commit");
  auto w = df_watch_query(db, {wide.data(), wide.size(), false}, nullptr, nullptr);
  require(w && df_view_size(w->result) == 1, "32 groups");
  require(df_edit_unset(&edit, 0, attr("field0")) && df_edit_apply(db, &edit) == DF_OK &&
              !df_view_size(w->result),
          "unprojected existence check");
  df_edit_free(&edit);
  close_store(db);
}
void work_and_churn() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  for (uint32_t id = 0; id < 1000; ++id) {
    require(df_edit_set(&edit, id, DF_ATTR_KIND, raw("task")) &&
                df_edit_set(&edit, id, DF_ATTR_TITLE, raw("title")) &&
                df_edit_set(&edit, id, DF_ATTR_DONE, raw("false")) &&
                df_edit_set(&edit, id, DF_ATTR_ID, raw(std::to_string(id))) &&
                df_edit_set(&edit, id, DF_ATTR_ORDER, raw(std::to_string(id))) &&
                df_edit_set(&edit, id, DF_ATTR_OWNER, df_entity(10000 + id % 10)),
            "work seed");
  }
  for (uint32_t id = 10000; id < 10010; ++id)
    require(df_edit_set(&edit, id, DF_ATTR_REGION, raw("common")), "region seed");
  require(df_edit_apply(db, &edit) == DF_OK, "work commit");
  df_match q[] = {{df_var(0), DF_ATTR_KIND, df_const(raw("task")), true},
                  {df_var(0), DF_ATTR_TITLE, df_var(1), true},
                  {df_var(0), DF_ATTR_DONE, df_var(2), true},
                  {df_var(0), DF_ATTR_ID, df_var(3), true},
                  {df_var(0), DF_ATTR_ORDER, df_var(4), true}};
  unsigned calls = 0, aliases = 0;
  auto first = df_watch_query(db, {q, 5, false}, notified, &calls),
       alias = df_watch_query(db, {q, 5, false}, notified, &aliases);
  require(first && alias, "work watches");
  auto unchanged = df_view_box(first->result, pair_key(999, DF_ATTR_TITLE));
  require(df_edit_set(&edit, 0, DF_ATTR_DONE, raw("true")) && df_edit_apply(db, &edit) == DF_OK,
          "incremental edit");
  require(calls == 2 && aliases == 2 && df_recomputed(db) == 1 &&
              df_view_size(first->result) == 5000 &&
              df_view_box(first->result, pair_key(999, DF_ATTR_TITLE)) == unchanged,
          "incremental work/sharing");
  df_match selective[] = {{df_var(0), DF_ATTR_TITLE, df_var(2), true},
                          {df_var(0), DF_ATTR_OWNER, df_var(1), false},
                          {df_var(1), DF_ATTR_REGION, df_const(raw("rare")), false}};
  auto select = df_watch_query(db, {selective, 3, false}, nullptr, nullptr);
  require(select, "selective watch");
  require(df_edit_set(&edit, 10000, DF_ATTR_REGION, raw("rare")) &&
              df_edit_apply(db, &edit) == DF_OK && df_view_size(select->result) == 100,
          "selective join");
  require(df_edit_set(&edit, 10000, DF_ATTR_REGION, raw("common")) &&
              df_edit_apply(db, &edit) == DF_OK && !df_view_size(select->result),
          "empty planner anchor");
  df_edit_free(&edit);
  close_store(db);
  directory churn;
  db = open_store(churn.path);
  auto all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr);
  require(all, "churn watch");
  facts expected;
  std::mt19937 random(123);
  for (unsigned step = 0; step < 500; ++step) {
    uint32_t id = random() % 60, a = random() % 2 ? DF_ATTR_TITLE : DF_ATTR_DONE;
    auto key = std::make_pair(id, a);
    if (random() % 3 == 0) {
      auto it = expected.find(key);
      if (it != expected.end()) {
        require(df_edit_retract(&edit, id, a, raw(it->second)), "churn retract");
        expected.erase(it);
      }
    } else {
      auto value = std::to_string(random() % 8);
      require(df_edit_set(&edit, id, a, raw(value)), "churn set");
      expected[key] = value;
    }
    require(df_edit_apply(db, &edit) == DF_OK && contents(all->result) == expected,
            "mutable churn/reference");
  }
  df_edit_free(&edit);
  close_store(db);
}

void storage_and_failures() {
  directory d, other;
  auto db = open_store(d.path), replica = open_store(other.path);
  df_edit edit{};
  auto all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr),
       remote = df_watch_query(replica, {nullptr, 0, true}, nullptr, nullptr);
  require(all && remote, "storage watches");
  uint64_t title = pair_key(7, DF_ATTR_TITLE), done = pair_key(7, DF_ATTR_DONE);
  require(df_edit_set(&edit, 7, DF_ATTR_TITLE, raw("first")) &&
              df_edit_set(&edit, 7, DF_ATTR_DONE, raw("false")) &&
              df_edit_set(&edit, UINT32_MAX, DF_ATTR_TITLE, raw("last")) &&
              df_edit_set(&edit, 0, DF_ATTR_BLOB, raw("")) && df_edit_apply(db, &edit) == DF_OK,
          "storage seed");
  auto exported = export_records(db);
  require(exported.size() == 4 && exported.at(title) == fact_record(title, "first") &&
              exported.at(pair_key(0, DF_ATTR_BLOB)) == fact_record(pair_key(0, DF_ATTR_BLOB), ""),
          "fact encoding/empty");
  std::map<uint64_t, std::optional<std::string>> imports;
  for (auto &[k, v] : exported)
    imports[k] = v;
  require(import_records(replica, imports) == std::vector<uint64_t>{remote->id} &&
              export_records(replica) == exported,
          "fact import");
  auto before = export_records(replica);
  auto corrupt = exported.at(done);
  corrupt.back() ^= 1;
  bool rejected = false;
  try {
    import_records(replica, {{title, fact_record(title, "bad")}, {done, corrupt}});
  } catch (...) {
    rejected = true;
  }
  require(rejected && export_records(replica) == before, "corrupt import atomicity");
  rejected = false;
  try {
    import_records(replica, {{title, fact_record(title, "bad")},
                             {pair_key(9, 0), fact_record(pair_key(9, 0), "bad")}});
  } catch (...) {
    rejected = true;
  }
  require(rejected && export_records(replica) == before, "unknown attribute atomicity");
  import_records(replica, {{done, std::nullopt}});
  require(absent(remote->result, 7, DF_ATTR_DONE) &&
              has_value(remote->result, 7, DF_ATTR_TITLE, "first"),
          "fact deletion scope");
  auto entities = export_records(db, 6);
  imports.clear();
  for (auto &[k, v] : entities)
    imports[k] = v;
  import_records(replica, imports, 7);
  require(export_records(replica) == exported, "entity import/export");
  auto bad_entity = entities.at(7);
  bad_entity.back() ^= 1;
  rejected = false;
  try {
    import_records(replica, {{7, bad_entity}}, 7);
  } catch (...) {
    rejected = true;
  }
  require(rejected && export_records(replica) == exported, "corrupt entity");
  import_records(db, {{7, std::nullopt}}, 7);
  require(absent(all->result, 7, DF_ATTR_TITLE) && absent(all->result, 7, DF_ATTR_DONE),
          "entity prefix deletion");
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("first")) && df_edit_apply(db, &edit) == DF_OK,
          "retract seed");
  require(df_edit_retract(&edit, 1, DF_ATTR_TITLE, raw("first")) &&
              df_edit_retract(&edit, 1, DF_ATTR_TITLE, raw("first")) &&
              df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("third")) &&
              df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("final")) &&
              df_edit_apply(db, &edit) == DF_OK &&
              has_value(all->result, 1, DF_ATTR_TITLE, "final"),
          "retractions before additions/last wins");
  std::string wide(4096, 'x');
  for (unsigned i = 0; i < 32; ++i)
    require(df_edit_set(&edit, 8, attr("field" + std::to_string(i)), raw(wide)), "wide entity");
  require(df_edit_apply(db, &edit) == DF_OK &&
              df_edit_set(&edit, 8, attr("field0"), raw("small")) &&
              df_edit_apply(db, &edit) == DF_OK && has_value(all->result, 8, attr("field1"), wide),
          "wide entity edit");
  exported = export_records(db);
  require(df_store_flush(db) == DF_OK, "storage flush");
  close_store(db);
  close_store(replica);
  df_edit_free(&edit);
  db = open_store(d.path);
  require(export_records(db) == exported, "fact reopen");
  close_store(db);
  MDB_env *env;
  MDB_txn *tx;
  MDB_dbi facts_db, meta;
  require(!mdb_env_create(&env) && !mdb_env_set_maxdbs(env, 2) &&
              !mdb_env_open(env, d.path.c_str(), 0, 0600),
          "physical env");
  require(!mdb_txn_begin(env, nullptr, MDB_RDONLY, &tx) && !mdb_dbi_open(tx, "facts", 0, &facts_db),
          "physical txn");
  MDB_agg total;
  require(!mdb_agg_totals(tx, facts_db, &total) && total.mv_agg_entries == exported.size(),
          "physical count");
  std::array<uint8_t, 8> expected{};
  for (auto &[key, value] : exported) {
    uint8_t encoded[8];
    for (unsigned i = 0; i < 8; ++i)
      encoded[i] = uint8_t(key >> (56 - 8 * i));
    MDB_val k{8, encoded}, v{};
    require(!mdb_get(tx, facts_db, &k, &v) &&
                std::string_view(static_cast<char *>(v.mv_data), v.mv_size) == value,
            "physical record");
    for (unsigned i = 0; i < 8; ++i)
      expected[i] ^= uint8_t(value[i]);
  }
  require(!memcmp(total.mv_agg_hashes, expected.data(), 8), "physical XOR");
  mdb_txn_abort(tx);
  auto codec = [&](std::string value) {
    require(!mdb_txn_begin(env, nullptr, 0, &tx) && !mdb_dbi_open(tx, "meta", 0, &meta),
            "codec txn");
    char name[] = "codec";
    MDB_val k{5, name}, v{value.size(), value.data()};
    require(!mdb_put(tx, meta, &k, &v, 0) && !mdb_txn_commit(tx), "codec change");
  };
  codec("datafriend/"
        "3;xor64;ea32;fact-v1;6105b134821d2173f88730e7bb22129a95388014580dd34caf7ee8d7062ec667");
  mdb_env_close(env);
  db = open_store(d.path);
  require(export_records(db) == exported, "additive schema upgrade");
  close_store(db);
  require(!mdb_env_create(&env) && !mdb_env_set_maxdbs(env, 2) &&
              !mdb_env_open(env, d.path.c_str(), 0, 0600),
          "codec env");
  codec("datafriend/2;xor64;ea32;boxed;entity-v1");
  mdb_env_close(env);
  df_options options{d.path.c_str(), "default", DF_BUFFERED, 0};
  df_buffer error{};
  require(!df_store_open(&options, &error) &&
              text(lb_bytes_make(error.data, error.size)) == "incompatible Datafriend codec",
          "incompatible format");
  df_free(error.data);
  directory full;
  db = open_store(full.path, DF_BUFFERED, 1 << 20);
  all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr);
  require(all, "map watch");
  require(df_edit_set(&edit, 1, DF_ATTR_A, raw("original")) && df_edit_apply(db, &edit) == DF_OK,
          "map seed");
  auto revision = df_revision(db);
  std::string large(32000, 'x');
  for (uint32_t i = 1; i < 80; ++i)
    require(df_edit_set(&edit, i, DF_ATTR_A, raw(large)), "map full fixture");
  require(df_edit_apply(db, &edit) == DF_ERROR && df_revision(db) == revision &&
              df_view_size(all->result) == 1 && has_value(all->result, 1, DF_ATTR_A, "original"),
          "map full rollback");
  require(df_edit_set(&edit, 1, DF_ATTR_A, raw("after")) && df_edit_apply(db, &edit) == DF_OK &&
              has_value(all->result, 1, DF_ATTR_A, "after"),
          "writer after map full");
  df_edit_free(&edit);
  close_store(db);
}
void identity_and_persistence() {
  require(pair_key(UINT32_MAX, UINT32_MAX) == UINT64_MAX && pair_key(1, 2) != pair_key(2, 1),
          "packed keys");
  directory d;
  auto db = open_store(d.path, DF_ASYNC, 1 << 24, "icare");
  df_edit edit{};
  require(df_edit_create(&edit, db) == 1 && df_edit_create(&edit, db) == 2 &&
              df_edit_apply(db, &edit) == DF_OK,
          "ID reservations");
  auto all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr);
  require(all, "identity watch");
  auto retained = all->result;
  df_view_retain(retained);
  require(df_edit_set(&edit, 3, DF_ATTR_TITLE, raw("value")) && df_edit_apply(db, &edit) == DF_OK,
          "explicit ID");
  auto box = df_view_box(retained, pair_key(3, DF_ATTR_TITLE));
  require(box, "box");
  df_box_keep(box);
  require(df_edit_create(&edit, db) == 4 && df_edit_apply(db, &edit) == DF_OK,
          "explicit IDs advance counter");
  require(df_edit_delete(&edit, 3) && df_edit_apply(db, &edit) == DF_OK && !df_view_size(retained),
          "deletion");
  const uint8_t *bytes;
  size_t size;
  uint64_t hash;
  df_box_value(box, &bytes, &size, &hash);
  require(text(lb_bytes_make(bytes, size)) == "value", "retained box after delete");
  require(df_store_flush(db) == DF_OK && df_test_flush_attempts(db) > 0, "async worker");
  df_test_flush_failure(db, 1);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("two")) && df_edit_apply(db, &edit) == DF_OK &&
              df_store_flush(db) == DF_ERROR,
          "flush failure");
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("rejected")) &&
              df_edit_apply(db, &edit) == DF_ERROR && has_value(retained, 1, DF_ATTR_TITLE, "two"),
          "flush error visible to writer");
  df_test_flush_failure(db, 0);
  require(df_store_flush(db) == DF_OK, "flush recovery");
  import_records(db, {{6, entity_record(6, {{"title", "imported"}})}}, 7);
  close_store(db);
  require(has_value(retained, 6, DF_ATTR_TITLE, "imported"), "view outlives store");
  df_box_value(box, &bytes, &size, &hash);
  require(text(lb_bytes_make(bytes, size)) == "value", "box outlives store");
  df_box_drop(box);
  df_view_release(retained);
  df_options options{d.path.c_str(), "wrong", DF_BUFFERED, 1 << 24};
  df_buffer error{};
  require(!df_store_open(&options, &error), "wrong namespace accepted");
  df_free(error.data);
  db = open_store(d.path, DF_ASYNC, 1 << 24, "icare");
  require(df_edit_create(&edit, db) == 7 && df_edit_apply(db, &edit) == DF_OK,
          "allocator/import persisted");
  df_slice value{};
  require(df_store_get(db, 1, DF_ATTR_TITLE, &value) == DF_OK &&
              text(lb_bytes_make(value.data, value.size)) == "two",
          "async edit persisted");
  require(df_edit_set(&edit, UINT32_MAX, DF_ATTR_TITLE, raw("last")) &&
              df_edit_apply(db, &edit) == DF_OK,
          "max ID");
  require(df_edit_create(&edit, db) == 0 && df_edit_apply(db, &edit) == DF_ERROR,
          "counter exhaustion");
  require(df_edit_delete(&edit, UINT32_MAX) && df_edit_apply(db, &edit) == DF_OK, "delete max");
  close_store(db);
  db = open_store(d.path, DF_ASYNC, 1 << 24, "icare");
  require(df_edit_create(&edit, db) == 0 && df_edit_apply(db, &edit) == DF_ERROR,
          "exhaustion persisted");
  df_edit_free(&edit);
  close_store(db);
}
void direct_reads() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  auto all = df_watch_query(db, {nullptr, 0, true}, nullptr, nullptr);
  require(all, "read watch");
  auto result = all->result;
  for (auto [id, value] : std::map<uint32_t, int64_t>{{1, 7}, {2, -9}, {3, 7}})
    require(df_edit_set(&edit, id, DF_ATTR_ORDER, df_int(value)), "order seed");
  require(df_edit_set(&edit, 1, DF_ATTR_PART, df_int(1)) &&
              df_edit_set(&edit, 2, DF_ATTR_PART, df_int(0)) &&
              df_edit_set(&edit, 3, DF_ATTR_PART, df_int(0)) &&
              df_edit_set(&edit, 1, DF_ATTR_BLOB, df_encoded("a\0b", 3)) &&
              df_edit_apply(db, &edit) == DF_OK,
          "read seed");
  df_buffer out{};
  require(!df_result_attributes(result, 1, &out), "attributes");
  lb_reader r{lb_bytes_make(out.data, out.size), false};
  std::set<std::string> names;
  auto n = lb_read_u32(&r);
  while (n--)
    names.insert(text(lb_read_blob(&r)));
  require(lb_reader_end(&r) && names == std::set<std::string>{"blob", "order", "part"},
          "attribute enumeration");
  df_free(out.data);
  require(!df_result_attributes(result, 99, &out) && out.size == 4 &&
              !memcmp(out.data, "\0\0\0\0", 4),
          "absent attributes");
  df_free(out.data);
  df_entities ids{};
  auto ordered = [&](uint32_t order, uint32_t secondary, std::vector<uint32_t> expected) {
    require(df_view_entities(result, order, secondary, &ids) == DF_OK &&
                std::vector<uint32_t>(ids.data, ids.data + ids.count) == expected,
            "entity ordering");
  };
  ordered(DF_ATTR_ORDER, DF_ATTR_PART, {2, 3, 1});
  df_slice borrowed{};
  require(df_view_get(result, 1, DF_ATTR_BLOB, &borrowed) == DF_OK && borrowed.size == 3,
          "borrowed value");
  require(!df_result_value(result, 1, DF_ATTR_BLOB, &out) && out.data == borrowed.data,
          "borrowed bytes copied");
  std::string kept = text(lb_bytes_make(out.data, out.size));
  require(df_result_find(result, DF_ATTR_BLOB, out.data, out.size) == 1 &&
              df_result_count_value(result, DF_ATTR_BLOB, out.data, out.size) == 1,
          "find/count");
  require(df_result_value(result, 99, DF_ATTR_BLOB, &out) == 1 && !out.data, "absent value");
  require(df_edit_set(&edit, 0, DF_ATTR_KIND, raw("other")) &&
              df_edit_set(&edit, 4, DF_ATTR_ORDER, df_int(INT64_MAX)) &&
              df_edit_set(&edit, 5, DF_ATTR_ORDER, df_int(INT64_MIN)) &&
              df_edit_set(&edit, 6, DF_ATTR_ORDER, df_int(7)) &&
              df_edit_set(&edit, 7, DF_ATTR_KIND, raw("other")) &&
              df_edit_apply(db, &edit) == DF_OK,
          "extreme sort fixture");
  ordered(DF_ATTR_ORDER, 0, {5, 2, 1, 3, 6, 4, 0, 7});
  ordered(DF_ATTR_ORDER, DF_ATTR_PART, {5, 2, 3, 1, 6, 4, 0, 7});
  ordered(0, 0, {0, 1, 2, 3, 4, 5, 6, 7});
  require(df_edit_retract(&edit, 1, DF_ATTR_ORDER, df_int(7)) && df_edit_apply(db, &edit) == DF_OK,
          "remove order");
  ordered(DF_ATTR_ORDER, 0, {5, 2, 3, 6, 4, 0, 1, 7});
  require(df_edit_set(&edit, 1, DF_ATTR_BLOB, raw("replacement")) &&
              df_edit_set(&edit, 2, DF_ATTR_ORDER, raw("invalid")) &&
              df_edit_apply(db, &edit) == DF_OK && kept == std::string("a\0b", 3),
          "owned copy lifetime");
  require(df_view_entities(result, DF_ATTR_ORDER, 0, &ids) == DF_ERROR, "invalid sort accepted");
  auto poisoned = df_view_empty(nullptr, 0);
  require(poisoned, "empty view");
  df_test_poison_view(poisoned);
  require(df_result_value(poisoned, 1, DF_ATTR_BLOB, &out) == -1, "poisoned value");
  require(df_result_entities(poisoned, 0, 0, &out) == 1, "poisoned enumeration");
  df_free(out.data);
  require(df_result_attributes(poisoned, 1, &out) == 1, "poisoned attributes");
  df_free(out.data);
  df_result_release(poisoned);
  df_entities_free(&ids);
  df_edit_free(&edit);
  close_store(db);
}
void repeated_projection() {
  directory d;
  auto db = open_store(d.path);
  df_edit edit{};
  // The wire API permits repeated projection indices. Multiple join witnesses
  // and repeated projections must still produce a set of facts.
  auto w = request(1);
  lb_u8(&w, 0);
  lb_u32(&w, 2);
  lb_u8(&w, 1);
  lb_u8(&w, 0);
  lb_blob(&w, span("title"));
  lb_u8(&w, 1);
  lb_u8(&w, 1);
  lb_u8(&w, 1);
  lb_u8(&w, 2);
  lb_blob(&w, span("group"));
  lb_u8(&w, 0);
  lb_blob(&w, span("g"));
  lb_u32(&w, 2);
  lb_u32(&w, 0);
  lb_u32(&w, 0);
  auto packet = call(db, &w);
  lb_reader r{span(packet), false};
  auto id = lb_read_u64(&r);
  df_buffer error{};
  auto view = static_cast<df_view *>(df_result(db, id, &error));
  require(view, "repeated projection view");
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("old")) &&
              df_edit_set(&edit, 2, DF_ATTR_GROUP, raw("g")) &&
              df_edit_set(&edit, 3, DF_ATTR_GROUP, raw("g")) && df_edit_apply(db, &edit) == DF_OK &&
              df_view_size(view) == 1,
          "duplicate facts from witnesses");
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("new")) && df_edit_apply(db, &edit) == DF_OK &&
              df_view_size(view) == 1 && has_value(view, 1, DF_ATTR_TITLE, "new"),
          "duplicate replacement");
  require(df_edit_unset(&edit, 2, DF_ATTR_GROUP) && df_edit_apply(db, &edit) == DF_OK &&
              df_view_size(view) == 1,
          "fewer witnesses");
  require(df_edit_unset(&edit, 3, DF_ATTR_GROUP) && df_edit_apply(db, &edit) == DF_OK &&
              !df_view_size(view),
          "last witness");
  require(df_edit_set(&edit, 2, DF_ATTR_GROUP, raw("g")) && df_edit_apply(db, &edit) == DF_OK &&
              df_view_size(view) == 1,
          "witness reinsertion");
  w = request(4);
  lb_u64(&w, id);
  call(db, &w);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("unsubscribed")) &&
              df_edit_apply(db, &edit) == DF_OK && df_recomputed(db) == 0 &&
              has_value(view, 1, DF_ATTR_TITLE, "new"),
          "last unsubscribe retains view and removes dependencies");
  // A projection change must form a different group, while aliases share one.
  df_match clauses[] = {{df_var(0), DF_ATTR_TITLE, df_var(1), true},
                        {df_var(0), DF_ATTR_DONE, df_var(2), false}};
  auto title = df_watch_query(db, {clauses, 2, false}, nullptr, nullptr);
  auto alias = df_watch_query(db, {clauses, 2, false}, nullptr, nullptr);
  clauses[0].project = false;
  clauses[1].project = true;
  auto done = df_watch_query(db, {clauses, 2, false}, nullptr, nullptr);
  require(title && alias && done && df_result_same(title->result, alias->result) &&
              !df_result_same(title->result, done->result),
          "projection group identity");
  require(df_edit_set(&edit, 1, DF_ATTR_DONE, raw("false")) && df_edit_apply(db, &edit) == DF_OK &&
              has_value(done->result, 1, DF_ATTR_DONE, "false") &&
              absent(done->result, 1, DF_ATTR_TITLE),
          "distinct projection");
  auto kept = title->result;
  df_view_retain(kept);
  df_watch_remove(db, &title);
  df_watch_remove(db, &alias);
  require(df_edit_set(&edit, 1, DF_ATTR_TITLE, raw("later")) && df_edit_apply(db, &edit) == DF_OK &&
              has_value(kept, 1, DF_ATTR_TITLE, "unsubscribed"),
          "alias unsubscribe lifetime");
  df_edit_free(&edit);
  close_store(db);
  require(has_value(view, 1, DF_ATTR_TITLE, "new") && df_view_size(kept) == 1,
          "unsubscribed views after close");
  df_view_release(kept);
  df_view_release(view);
}
void wire_api() {
  directory d;
  auto db = open_store(d.path);
  df_buffer out{};
  require(!df_attribute_schema(&out) &&
              text(lb_bytes_make(out.data, out.size)) == DATAFRIEND_SCHEMA,
          "schema fingerprint");
  df_free(out.data);
  auto w = request(1);
  lb_u8(&w, 1);
  lb_u32(&w, 0);
  lb_u32(&w, 0);
  require(!df_call(db, w.data, w.size, &out), "wire subscribe");
  lb_reader r{lb_bytes_make(out.data, out.size), false};
  auto id = lb_read_u64(&r);
  df_free(out.data);
  for (size_t i = 0; i < w.size; ++i) {
    require(df_call(db, w.data, i, &out) == 1, "truncated subscription");
    df_free(out.data);
  }
  lb_buffer_free(&w);
  w = request(9);
  lb_u64(&w, id);
  lb_u64(&w, 1);
  lb_blob(&w, span("missing"));
  require(!df_call(db, w.data, w.size, &out) && out.size == 1 && !out.data[0], "absent pair");
  df_free(out.data);
  for (size_t i = 0; i < w.size; ++i) {
    require(df_call(db, w.data, i, &out) == 1, "truncated lookup");
    df_free(out.data);
  }
  lb_buffer_free(&w);
  auto view = df_result(db, id, &out), same = df_result(db, id, &out);
  require(view && same && df_result_same(view, same), "wire shared result");
  df_result_release(same);
  require(!df_result_changes(view, &out) && out.size == 10 && out.data[0] == 1, "initial reset");
  df_free(out.data);
  require(!df_result_list(view, &out) && out.size == 4, "empty list");
  df_free(out.data);
  // Wire IDs are 64-bit: reject overflow before narrowing into the native API.
  w = request(10);
  lb_u32(&w, 0);
  lb_u32(&w, 1);
  lb_u64(&w, uint64_t(UINT32_MAX) + 1);
  lb_blob(&w, span("title"));
  lb_blob(&w, span("bad"));
  require(df_call(db, w.data, w.size, &out) == 1 && df_view_size(view) == 0, "out-of-range entity");
  df_free(out.data);
  lb_buffer_free(&w);
  // Multi-view edits retain their wire-level atomic transaction semantics.
  w = request(8);
  lb_u32(&w, 2);
  for (unsigned i = 0; i < 2; ++i) {
    lb_u64(&w, id);
    lb_u32(&w, 0);
    lb_u32(&w, 1);
    lb_u64(&w, i + 1);
    lb_blob(&w, span("title"));
    lb_blob(&w, span("multi"));
  }
  auto revision = df_revision(db);
  require(!df_call(db, w.data, w.size, &out) && df_revision(db) == revision + 1 &&
              df_view_size(view) == 2,
          "multi-view atomic edit");
  df_free(out.data);
  lb_buffer_free(&w);
  w = request(8);
  lb_u32(&w, 2);
  for (unsigned i = 0; i < 2; ++i) {
    lb_u64(&w, id);
    lb_u32(&w, 0);
    lb_u32(&w, 1);
    lb_u64(&w, i + 1);
    lb_blob(&w, span("title"));
    lb_blob(&w, span(i ? std::string(65535, 'x') : "broken"));
  }
  require(df_call(db, w.data, w.size, &out) == 1 &&
              has_value(static_cast<df_view *>(view), 1, DF_ATTR_TITLE, "multi"),
          "multi-view rollback");
  df_free(out.data);
  lb_buffer_free(&w);
  w = request(11);
  require(!df_call(db, w.data, w.size, &out), "wire flush");
  df_free(out.data);
  lb_buffer_free(&w);
  require(!df_close_checked(db, &out), "checked close");
  df_free(out.data);
  require(!df_result_info(view, &out) && out.size == 8, "wire retained result");
  df_free(out.data);
  df_result_release(view);
}
} // namespace
int main() {
  try {
    for (auto [name, test] : std::initializer_list<std::pair<const char *, void (*)()>>{
             {"semantics/sharing", semantics_and_sharing},
             {"routing", routing},
             {"incremental oracle", incremental_queries},
             {"planner oracle", planned_queries},
             {"work/churn", work_and_churn},
             {"storage/failures", storage_and_failures},
             {"identity/persistence", identity_and_persistence},
             {"direct reads", direct_reads},
             {"repeated projection", repeated_projection},
             {"wire API", wire_api}}) {
      std::cerr << "Running " << name << std::endl;
      test();
    }
    std::cout << "PASS: direct C edits, queries, independent models, routing, storage, rollback, "
                 "ownership and wire ABI\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
