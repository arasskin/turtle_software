#include "datafriend/engine.h"
#include <set>
#include "datafriend/wire_codec.h"
#include "datafriend/c_api.h"
#include "lmdb.h"
#include <iostream>
#include <filesystem>
#include <random>
#include <unistd.h>
using namespace datafriend;
namespace {
void require(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
struct directory {
  std::string path;
  directory() { char p[]="/tmp/datafriend-test-XXXXXX"; auto* d=mkdtemp(p); if(!d) throw std::runtime_error("mkdtemp"); path=d; }
  ~directory() { std::filesystem::remove_all(path); }
};
term variable(int n) { return {n,{}}; }
term constant(bytes b) { return {-1,std::move(b)}; }
query attribute(std::string name) { return {{{variable(0),std::move(name),variable(1)}},{0}}; }
void semantics() {
  directory dir; query_result retained;
  {
    engine db(dir.path); auto all=db.subscribe({{}, {},true}).first;
    bool duplicate=false; try { engine other(dir.path); } catch(...) { duplicate=true; }
    require(duplicate,"multiple owners admitted");
    auto titles=db.subscribe(attribute("title")).first; auto unrelated=db.subscribe(attribute("color")).first;
    query incomplete{{{variable(0),"done",constant("false")},{variable(0),"title",variable(1)}},{0,1}};
    auto open=db.subscribe(incomplete).first;
    query joined{{{variable(0),"owner",variable(1)},{variable(1),"name",constant("Alex")},{variable(0),"title",variable(2)}},{2}};
    auto owned=db.subscribe(joined).first;
    db.modify(all,{{},{{1,"title","first"},{1,"done","false"},{1,"owner",entity_value(10)},{10,"name","Alex"}}});
    require(db.result(open).size()==2 && db.result(owned).size()==1,"join/projection");
    require(db.last_recomputed()==4,"attribute subscriber index");
    auto unchanged=db.result(unrelated); retained=db.result(open);
    db.modify(open,{{},{{1,"done","true"}}});
    require(db.result(open).empty(),"origin not refreshed after predicate changed");
    require(retained.empty(),"retained handle did not update");
    require(db.result(unrelated).shares_with(unchanged),"unrelated root changed");
    require(db.last_recomputed()==2,"invalidated unrelated query");
    db.modify(titles,{{{1,"title","stale"}}, {}});
    require(db.result(titles).size()==1,"remove did not compare value");
    db.modify(titles,{{{1,"title","first"}},{{1,"title","new"},{2,"title","second"}}});
    require(db.result(titles).size()==2,"batch replace/add");
    auto stable=db.result(titles); auto rev=db.revision();
    db.modify(titles,{{},{{1,"title","new"}}});
    require(db.revision()==rev&&stable.shares_with(db.result(titles)),"no-op edit");
    db.modify(all,{{},{{10,"name","Someone else"}}});
    require(db.result(owned).empty(),"join dependency invalidation");
    bool failed=false;
    try { db.modify(all,{{},{{1,"title","corrupted"},{100,"big",std::string(65535,'x')}}}); } catch(const std::length_error&) {failed=true;}
    require(failed&&db.result(titles).lookup(1,"title")->value->value=="new","failed batch published");
    auto exported=db.export_entities(); auto record=decode_entity(1,exported.at(1));
    require(record.at("done")=="true"&&record.at("title")=="new","unprojected attributes lost");
    directory other; engine replica(other.path); auto remote=replica.subscribe({{},{},true}).first;
    std::map<uint64_t,std::optional<bytes>> imported; for(auto& [id,b]:exported) imported[id]=b;
    replica.import_entities(imported);
    require(replica.export_entities()==db.export_entities(),"entity import/export");
    auto bad=exported.at(1); bad.back()^=1; failed=false;
    try { replica.import_entities({{1,bad}}); } catch(...) {failed=true;}
    require(failed,"corrupt record accepted");
    replica.import_entities({{1,std::nullopt}});
    require(!replica.result(remote).lookup(1,"title"),"entity deletion");
    // One UI action edits several views, with a single transaction/publication.
    db.modify(unrelated,{{},{{1,"color","blue"}}});
    auto before_multi=db.revision();
    db.modify_many({{titles,{{},{{1,"title","multi"}}}}, {unrelated,{{{1,"color","blue"}}, {}}}});
    require(db.revision()==before_multi+1 && db.result(unrelated).empty(),"multi-view atomic edit");
    require(db.result(titles).lookup(1,"title")->value->value=="multi","multi-view title");
    auto old_root=db.result(titles); failed=false;
    try { db.modify_many({{titles,{{},{{1,"title","broken"}}}}, {unrelated,{{},{{99,"big",std::string(65535,'x')}}}}}); } catch(...) {failed=true;}
    require(failed && db.result(titles).shares_with(old_root),"multi-view rollback");
    db.unsubscribe(unrelated);
    // Application calls are serialized by the caller; edits publish before return.
    for(uint64_t i=0;i<4;++i) for(uint64_t j=0;j<50;++j) {
      db.modify(titles,{{},{{1000+i*100+j,"title",std::to_string(j)}}});
      require(db.result(titles).size()==3+i*50+j,"edit returned before publication");
    }
    require(db.result(titles).size()==202,"serialized edits");
  }
  require(retained.empty(),"result cannot outlive engine");
  { engine db(dir.path); auto t=db.subscribe(attribute("title")).first; require(db.result(t).size()==202,"reopen/index rebuild"); }
  // Verify the actual on-disk aggregate, including overwrites and deletion.
  MDB_env* env; require(!mdb_env_create(&env),"env"); mdb_env_set_maxdbs(env,2);
  require(!mdb_env_open(env,dir.path.c_str(),MDB_RDONLY,0600),"readonly env");
  MDB_txn* tx; require(!mdb_txn_begin(env,nullptr,MDB_RDONLY,&tx),"txn"); MDB_dbi db;
  require(!mdb_dbi_open(tx,"facts",0,&db),"db"); MDB_agg total;
  require(!mdb_agg_totals(tx,db,&total),"aggregate");
  MDB_cursor* c; mdb_cursor_open(tx,db,&c); MDB_val k,v; uint64_t count=0; std::array<unsigned char,8> xor_hash{};
  while(mdb_cursor_get(c,&k,&v,MDB_NEXT)==0) {++count; for(size_t i=0;i<8;++i) xor_hash[i]^=static_cast<unsigned char*>(v.mv_data)[i];}
  require(total.mv_agg_entries==count,"stored count");
  for(size_t i=0;i<8;++i) require(total.mv_agg_hashes[i]==xor_hash[i],"stored XOR");
  mdb_cursor_close(c); mdb_txn_abort(tx); mdb_env_close(env);
}
void shared_queries() {
  directory d; engine db(d.path,false);
  auto seed=db.subscribe({{},{},true}).first;
  db.modify(seed,{{},{{1,"a","one"},{1,"b","two"},{2,"a","other"},{2,"b","two"}}});
  db.unsubscribe(seed);
  query q{{{variable(0),"a",variable(1)},{variable(0),"b",variable(2)}},{0}};
  auto [first,old]=db.subscribe(q);
  auto [second,second_result]=db.subscribe(q);
  auto [third,third_result]=db.subscribe(q);
  require(first!=second&&second!=third,"shared subscriptions reused handle ID");
  require(old.size()==2,"subscribe returned an unevaluated result");
  require(old.shares_with(second_result)&&old.shares_with(third_result),"returned query results did not share storage");
  require(old.shares_with(db.result(first)),"returned map differs from handle lookup");
  auto projection=q; projection.projection={1}; auto projected=db.subscribe(projection).first;
  auto constant_query=q; constant_query.clauses[0].value=constant("one"); auto filtered=db.subscribe(constant_query).first;
  require(db.result(projected).lookup(1,"b").has_value(),"projection identity ignored");
  require(db.result(filtered).size()==1,"constant identity ignored");
  db.modify(second,{{},{{1,"a","updated"}}});
  require(db.last_recomputed()==3,"evaluated identical descriptions more than once");
  require(db.result(filtered).empty(),"distinct query shared incorrectly");
  require(second_result.lookup(1,"a")->value->value=="updated","returned alias missed publication");
  require(db.result(first).shares_with(db.result(third)),"publication broke root sharing");
  require(old.lookup(1,"a")->value->value=="updated","shared publication changed retained snapshot");
  // Aliases edit independent working values; all retractions precede additions.
  auto revision=db.revision();
  db.modify_many({{first,{{{1,"a","updated"}}, {}}}, {third,{{},{{1,"a","batch"}}}}});
  require(db.revision()==revision+1&&db.result(second).lookup(1,"a")->value->value=="batch","shared multi-view edit");
  auto before=db.result(first); bool failed=false;
  try { db.modify_many({{first,{{},{{1,"a","bad"}}}}, {third,{{},{{9,"big",std::string(65535,'x')}}}}}); } catch(...) {failed=true;}
  require(failed&&db.result(second).shares_with(before),"failed edit published shared root");
  db.unsubscribe(first); db.unsubscribe(first); db.unsubscribe(projected); db.unsubscribe(filtered);
  db.modify(third,{{},{{1,"a","survivor"}}});
  require(db.last_recomputed()==1&&db.result(second).shares_with(db.result(third)),"unsubscribe removed live group");
  db.unsubscribe(second); db.unsubscribe(third);
  db.import_entities({{1,encode_entity(1,{{"a","imported"},{"b","two"}})}});
  require(db.last_recomputed()==0,"last unsubscribe left dependencies");
  auto reopened=db.subscribe(q).first;
  require(db.result(reopened).lookup(1,"a")->value->value=="imported","resubscribe reused stale result");
  db.unsubscribe(reopened);
  auto all=db.subscribe({{},{},true}).first, all2=db.subscribe({{},{},true}).first;
  require(db.result(all).shares_with(db.result(all2)),"wildcard group not shared");
  db.unsubscribe(all);
  db.import_entities({{2,std::nullopt}});
  require(db.last_recomputed()==1&&db.result(all2).size()==2,"shared wildcard import/invalidation");
  db.unsubscribe(all2);
  require(old.size()==2,"last unsubscribe invalidated snapshot");
}
void fixed_entity_routing() {
  directory d; engine db(d.path,false);
  auto row_query=[](uint32_t id) {
    return query{{{constant(entity_value(id)),"title",variable(0)},
                  {constant(entity_value(id)),"done",constant("false")}}, {0}};
  };
  std::vector<uint64_t> rows;
  edit seed;
  for(uint32_t id=0;id<1000;++id) {
    rows.push_back(db.subscribe(row_query(id)).first);
    seed.add.push_back({id,"title","task"});
    seed.add.push_back({id,"done","false"});
  }
  db.modify(seed);
  auto alias=db.subscribe(row_query(1)).first;
  require(db.result(alias).shares_with(db.result(rows[1])),"fixed queries not shared");
  auto notifications=db.modify({{},{{1,"title","updated"}}});
  require(db.last_recomputed()==1,"title visited unrelated fixed entities");
  require(notifications==std::vector<uint64_t>{rows[1],alias},"fixed query alias notifications");
  db.fail_next_commit_for_testing(); bool failed=false;
  try { db.modify({{},{{1,"title","must roll back"}}}); } catch(...) { failed=true; }
  require(failed&&db.result(alias).lookup(1,"title")->value->value=="updated","fixed result rollback recovery");
  db.modify({{},{{0,"title","zero"},{2,"done","true"}}});
  require(db.last_recomputed()==2&&db.result(rows[2]).empty(),"batch or zero entity routing");
  require(db.result(rows[0]).lookup(0,"title")->value->value=="zero","entity zero confused with wildcard");
  db.import_entities({{1,std::nullopt}});
  require(db.last_recomputed()==1&&db.result(alias).empty(),"fixed entity deletion routing");
  db.unsubscribe(rows[1]);
  db.modify({{},{{1,"title","restored"},{1,"done","false"}}});
  require(db.last_recomputed()==1&&db.result(alias).size()==1,"fixed alias lifetime");
  db.unsubscribe(alias);
  db.modify({{},{{1,"title","unsubscribed"}}});
  require(db.last_recomputed()==0,"fixed unsubscribe left dependencies");
  for(auto id:rows) db.unsubscribe(id);

  auto title_only=db.subscribe({{{constant(entity_value(5)),"title",variable(0)}},{0}}).first;
  auto done_only=db.subscribe({{{constant(entity_value(6)),"done",variable(0)}},{0}}).first;
  db.modify({{},{{5,"done","true"},{6,"title","different"}}});
  require(db.last_recomputed()==0,"batch routed Cartesian product instead of changed pairs");
  db.unsubscribe(title_only); db.unsubscribe(done_only);

  // A fixed entity on one side of a join must not restrict the other side.
  query joined{{{constant(entity_value(1)),"owner",variable(0)},
                {variable(0),"name",variable(1)}},{1}};
  auto join=db.subscribe(joined).first;
  auto names=db.subscribe(attribute("name")).first;
  db.modify({{},{{1,"owner",entity_value(2000)},{2000,"name","Alex"}}});
  require(db.last_recomputed()==2&&db.result(join).size()==1,"mixed fixed/variable join routing");
  db.modify({{},{{2000,"name","New name"}}});
  require(db.last_recomputed()==2&&db.result(join).lookup(2000,"name")->value->value=="New name","joined variable dependency lost");
  db.modify({{},{{2,"owner",entity_value(2000)}}});
  require(db.last_recomputed()==0,"unrelated fixed join dependency routed");
  db.import_entities({{2000,std::nullopt}});
  require(db.result(join).empty()&&db.result(names).empty(),"joined deletion lost");
  db.unsubscribe(join); db.unsubscribe(names);

  query constants{{{constant(entity_value(0)),"title",variable(0)},
                   {constant(entity_value(UINT32_MAX)),"title",variable(1)}},{0,1}};
  auto multiple=db.subscribe(constants).first;
  db.modify({{},{{UINT32_MAX,"title","last"}}});
  require(db.last_recomputed()==1&&db.result(multiple).size()==2,"multiple fixed entities or max ID routing");
  db.modify({{},{{3,"title","irrelevant"}}});
  require(db.last_recomputed()==0,"multiple fixed clauses over-invalidated");
  db.unsubscribe(multiple);
  db.modify({{},{{0,"title","after unsubscribe"},{UINT32_MAX,"title","after unsubscribe"}}});
  require(db.last_recomputed()==0,"multiple fixed dependencies not removed");
}
void fact_storage() {
  directory d, other;
  const auto title=pair_key(7,uint32_t(datafriend::attribute::title));
  const auto done=pair_key(7,uint32_t(datafriend::attribute::done));
  const auto last=pair_key(UINT32_MAX,uint32_t(datafriend::attribute::title));
  std::map<uint64_t,bytes> exported;
  {
    engine db(d.path), replica(other.path);
    auto view=db.subscribe({{},{},true}).first;
    auto remote=replica.subscribe({{},{},true}).first;
    // Subscription/insertion order cannot change attribute identity.
    replica.subscribe(attribute("done")).first; replica.subscribe(attribute("title")).first;
    require(db.result(view).scope()->find("title")==uint32_t(datafriend::attribute::title),"schema ID differs");
    require(replica.result(remote).scope()->find("title")==db.result(view).scope()->find("title"),"client attribute IDs differ");
    auto rev=db.revision();
    db.modify({{},{{7,"title","first"},{7,"done","false"},{UINT32_MAX,"title","last"},{0,"blob",""}}});
    require(db.revision()==rev+1,"local batch not one transaction");
    exported=db.export_facts();
    require(exported.size()==4&&decode_fact(title,exported.at(title))=="first","fact key/value encoding");
    require(decode_fact(pair_key(0,uint32_t(datafriend::attribute::blob)),exported.at(pair_key(0,uint32_t(datafriend::attribute::blob)))).empty(),"empty value confused with deletion");
    require(decode_fact(last,exported.at(last))=="last","maximum entity fact key");
    std::map<uint64_t,std::optional<bytes>> wire;
    for(const auto& [key,value]:exported) wire.emplace(key,value);
    auto notified=replica.import_facts(wire);
    require(!notified.empty()&&replica.export_facts()==exported,"fact import did not publish");
    auto unchanged=exported.at(done);
    db.modify({{{7,"title","stale"}},{{7,"title","second"}}});
    require(db.export_facts().at(done)==unchanged,"unrelated encoded fact changed");
    db.modify({{{7,"title","second"},{7,"title","second"}},{{7,"title","third"},{7,"title","final"}}});
    require(db.result(view).lookup(7,"title")->value->value=="final","batch retract/add semantics");
    auto before=replica.export_facts(); auto corrupt=exported.at(done); corrupt.back()^=1;
    bool failed=false;
    try {replica.import_facts({{title,encode_fact(title,"must not apply")},{done,corrupt}});} catch(...) {failed=true;}
    require(failed&&replica.export_facts()==before,"corrupt import partially applied");
    failed=false;
    try {replica.import_facts({{title,encode_fact(title,"must roll back")},{pair_key(9,0),encode_fact(pair_key(9,0),"bad")}});} catch(...) {failed=true;}
    require(failed&&replica.export_facts()==before,"unknown attribute accepted");
    replica.import_facts({{done,std::nullopt}});
    require(!replica.result(remote).lookup(7,"done")&&replica.result(remote).lookup(7,"title"),"fact deletion removed entity");
    db.import_entities({{7,std::nullopt}});
    require(!db.result(view).lookup(7,"title")&&!db.result(view).lookup(7,"done"),"whole entity prefix deletion");
    // Each value fits the transport limit, even when the entire entity doesn't.
    edit wide;
    for(int i=0;i<32;++i) wide.add.push_back({8,"field"+std::to_string(i),bytes(4096,'x')});
    db.modify(std::move(wide));
    db.modify({{},{{8,"field0","small edit"}}});
    require(db.result(view).lookup(8,"field1")->value->value.size()==4096,"wide entity edit lost another fact");
    exported=db.export_facts(); db.flush();
  }
  { engine db(d.path); require(db.export_facts()==exported,"fact reopen differs"); }
  // Confirm the physical records and count/XOR, rather than just the export API.
  MDB_env* env; require(!mdb_env_create(&env),"fact env"); mdb_env_set_maxdbs(env,2);
  require(!mdb_env_open(env,d.path.c_str(),0,0600),"fact env open");
  MDB_txn* tx; require(!mdb_txn_begin(env,nullptr,MDB_RDONLY,&tx),"fact txn"); MDB_dbi db;
  require(!mdb_dbi_open(tx,"facts",0,&db),"fact database"); MDB_agg total;
  require(!mdb_agg_totals(tx,db,&total)&&total.mv_agg_entries==exported.size(),"aggregate counts facts");
  std::array<unsigned char,8> expected{};
  for(const auto& [key,value]:exported) {
    writer encoded; encoded.u64(key); MDB_val k{8,encoded.data.data()},v;
    require(!mdb_get(tx,db,&k,&v)&&std::string_view(static_cast<char*>(v.mv_data),v.mv_size)==value,"physical fact record differs");
    for(size_t i=0;i<8;++i) expected[i]^=static_cast<unsigned char>(value[i]);
  }
  for(size_t i=0;i<8;++i) require(total.mv_agg_hashes[i]==expected[i],"fact XOR aggregate");
  mdb_txn_abort(tx);
  require(!mdb_txn_begin(env,nullptr,0,&tx),"codec txn"); MDB_dbi meta;
  require(!mdb_dbi_open(tx,"meta",0,&meta),"codec metadata");
  bytes codec_key="codec", old_codec="datafriend/3;xor64;ea32;fact-v1;6105b134821d2173f88730e7bb22129a95388014580dd34caf7ee8d7062ec667";
  MDB_val ck{codec_key.size(),codec_key.data()},cv{old_codec.size(),old_codec.data()};
  require(!mdb_put(tx,meta,&ck,&cv,0)&&!mdb_txn_commit(tx),"codec change");
  mdb_env_close(env);
  {engine upgraded(d.path);require(upgraded.export_facts()==exported,"additive document schema lost existing facts");}
  require(!mdb_env_create(&env)&&!mdb_env_set_maxdbs(env,2)&&!mdb_env_open(env,d.path.c_str(),0,0600),"old codec env");
  require(!mdb_txn_begin(env,nullptr,0,&tx)&&!mdb_dbi_open(tx,"meta",0,&meta),"old codec meta");
  old_codec="datafriend/2;xor64;ea32;boxed;entity-v1";cv={old_codec.size(),old_codec.data()};
  require(!mdb_put(tx,meta,&ck,&cv,0)&&!mdb_txn_commit(tx),"incompatible codec change");mdb_env_close(env);
  bool rejected=false;
  try {engine incompatible(d.path);} catch(const std::runtime_error& e) {
    rejected=std::string_view(e.what())=="incompatible Datafriend codec";
  }
  require(rejected,"old disk format accepted");
}
void repeated_projection() {
  directory d; engine db(d.path,false);
  auto all=db.subscribe({{},{},true}).first;
  // Two join matches and a repeated projection emit the same fact four times.
  query q{{{variable(0),"title",variable(1)},{variable(2),"group",constant("g")}},{0,0}};
  auto view=db.subscribe(q).first;
  db.modify(all,{{},{{1,"title","old"},{2,"group","g"},{3,"group","g"}}});
  auto old=db.result(view);
  require(old.size()==1,"duplicate projection inserted duplicate facts");
  db.modify(view,{{},{{1,"title","new"}}});
  auto updated=db.result(view);
  require(updated.size()==1&&updated.lookup(1,"title")->value->value=="new","duplicate replacement");
  db.modify(all,{{{2,"group","g"}}, {}});
  require(updated.shares_with(db.result(view)),"fewer duplicate matches changed root");
  db.modify(all,{{{3,"group","g"}}, {}});
  require(db.result(view).empty(),"last join match removal left projected fact");
  db.modify(all,{{},{{2,"group","g"}}});
  require(db.result(view).size()==1,"repeated projection reinsertion");
  require(old.lookup(1,"title")->value->value=="new","repeated projection mutated old snapshot");
}
void map_full_rollback() {
  directory d; engine db(d.path,false,1<<20); auto v=db.subscribe({{},{},true}).first;
  db.modify(v,{{},{{1,"a","original"}}}); auto rev=db.revision(); edit batch;
  for(uint64_t i=1;i<80;++i) batch.add.push_back({i,"a",std::string(32000,'x')});
  bool failed=false; try {db.modify(v,std::move(batch));}catch(...){failed=true;}
  require(failed&&db.revision()==rev&&db.result(v).size()==1,"map full rollback");
  db.modify(v,{{},{{1,"a","after failure"}}}); require(db.result(v).lookup(1,"a")->value->value=="after failure","writer unusable after failure");
}
void predicate_routing() {
  directory d; engine db(d.path,false);
  auto grouped=[](uint32_t owner) {
    return query{{{variable(0),"owner",constant(entity_value(owner))},
                  {variable(0),"done",constant("false")},
                  {variable(0),"x",variable(1)}},{2}};
  };
  std::vector<uint64_t> handles;
  for(uint32_t owner=0;owner<64;++owner) handles.push_back(db.subscribe(grouped(owner)).first);
  auto alias=db.subscribe(grouped(0)).first;
  auto first=db.result(alias), second=db.result(handles[1]);
  db.modify({{},{{0,"owner",entity_value(0)},{0,"done","false"},{0,"x","a"},
                 {1,"owner",entity_value(1)},{1,"done","false"},{1,"x","b"},
                 {2,"owner",entity_value(0)},{2,"done","false"},{2,"x","c"}}});
  require(db.last_recomputed()==2&&first.size()==2&&second.size()==1,"predicate routing on initial insertion");
  auto notifications=db.modify({{},{{0,"x","updated"},{2,"x","also updated"},{999,"x","no owner"}}});
  require(db.last_recomputed()==1&&notifications==std::vector<uint64_t>{handles[0],alias},"predicate/alias routing visited unrelated groups");
  // Move between groups and replace projected/other predicate facts together.
  db.modify({{},{{0,"owner",entity_value(1)},{0,"x","moved"},{1,"done","true"}}});
  require(db.last_recomputed()==2&&!first.lookup(0,"x")&&second.lookup(0,"x")->value->value=="moved"&&!second.lookup(1,"x"),"old/new group transition lost");
  db.modify({{{0,"owner",entity_value(1)}},{{0,"x","orphan"},{1,"done","false"}}});
  require(db.last_recomputed()==1&&!second.lookup(0,"x")&&second.lookup(1,"x"),"removed predicate or secondary predicate lost");
  db.modify({{},{{0,"owner",entity_value(0)}}});
  require(db.last_recomputed()==1&&first.lookup(0,"x")->value->value=="orphan","added predicate did not publish existing facts");
  // Failure restores facts and indexes; routing must still use the original owner.
  db.fail_next_commit_for_testing(); bool failed=false;
  try { db.modify({{},{{0,"owner",entity_value(1)},{0,"x","rollback"}}}); } catch(...) { failed=true; }
  require(failed&&first.lookup(0,"x")->value->value=="orphan"&&!second.lookup(0,"x"),"predicate transition rollback");
  db.modify({{},{{0,"x","after rollback"}}});
  require(db.last_recomputed()==1&&first.lookup(0,"x"),"predicate routing broken after rollback");
  db.import_entities({{0,std::nullopt}});
  require(db.last_recomputed()==1&&!first.lookup(0,"x"),"predicate routing missed deletion import");
  db.unsubscribe(handles[0]);
  db.modify({{},{{2,"x","alias remains"}}});
  require(db.last_recomputed()==1&&first.lookup(2,"x")->value->value=="alias remains","shared predicate group removed early");
  db.unsubscribe(alias);
  db.modify({{},{{2,"x","unsubscribed"}}});
  require(db.last_recomputed()==0,"predicate dependencies leaked after unsubscribe");
  for(auto handle:handles) db.unsubscribe(handle);
  auto restored=db.subscribe(grouped(0)).first;
  db.modify({{},{{2,"x","resubscribed"}}});
  require(db.last_recomputed()==1&&db.result(restored).lookup(2,"x")->value->value=="resubscribed","predicate re-registration failed");
}
void late_failure_recovery() {
  directory d; engine db(d.path,false);
  db.modify({{},{{1,"title","before"},{1,"done","false"},{2,"color","blue"}}});
  auto titles=db.subscribe(attribute("title")).first, colors=db.subscribe(attribute("color")).first;
  auto joined=db.subscribe({{{variable(0),"title",constant("before")},{variable(0),"done",variable(1)}},{1}}).first;
  auto t=db.result(titles), c=db.result(colors), j=db.result(joined);
  const auto version=db.revision();
  auto records=db.export_entities();
  db.fail_next_commit_for_testing(); bool failed=false;
  try { db.modify({{},{{1,"title","after"},{1,"done","true"},{3,"title","new"}}}); }
  catch(const std::runtime_error&) { failed=true; }
  require(failed&&db.revision()==version&&db.export_entities()==records,"failed transaction changed storage");
  require(t.size()==1&&t.lookup(1,"title")->value->value=="before","live result not recovered");
  require(j.size()==1&&j.lookup(1,"done")->value->value=="false","join index not recovered");
  require(c.lookup(2,"color")->value->value=="blue","recovery did not rerun all queries");
  require(t.changes().reset&&c.changes().reset&&j.changes().reset,"recovery did not reset change summaries");
  db.modify({{},{{1,"done","true"}}});
  require(j.lookup(1,"done")->value->value=="true","index unusable after recovery");
  // Retraction uses the database, including facts outside this query projection.
  db.modify(titles,{{{2,"color","blue"}}, {}});
  require(c.empty(),"edit incorrectly scoped to query membership");
}
void mutable_churn() {
  directory d; engine db(d.path,false); auto all=db.subscribe({{},{},true}).first;
  auto titles=db.subscribe(attribute("title")).first, alias=db.subscribe(attribute("title")).first;
  auto result=db.result(all), projected=db.result(titles);
  std::map<uint64_t,entity_record> expected; std::mt19937 random(123);
  for(size_t step=0;step<500;++step) {
    const uint64_t id=random()%60;
    const std::string attr=random()%2?"title":"done";
    edit change;
    auto& row=expected[id];
    if(random()%3==0) {
      if(auto it=row.find(attr);it!=row.end()) { change.remove.push_back({id,attr,it->second}); row.erase(it); }
    } else { auto value=std::to_string(random()%8); change.add.push_back({id,attr,value}); row[attr]=value; }
    const auto notifications=db.modify(std::move(change));
    require((std::find(notifications.begin(),notifications.end(),titles)!=notifications.end())==
            (std::find(notifications.begin(),notifications.end(),alias)!=notifications.end()),"shared callback IDs diverged");
    uint64_t facts=0, names=0;
    for(const auto& [e,r]:expected) for(const auto& [a,v]:r) {
      ++facts; if(a=="title") ++names;
      auto actual=result.lookup(uint32_t(e),a);
      require(actual&&actual->value->value==v,"mutable map lost a fact during churn");
    }
    require(result.size()==facts&&projected.size()==names,"mutable prune left stale facts/counts");
  }
}
using facts = std::map<std::pair<uint64_t,std::string>,bytes>;
facts contents(const query_result& result) {
  facts out;
  result.for_each([&](const query_result::entry& entry) {
    auto t=result.decode(entry); out[{t.entity,t.attribute}]=t.value;
  });
  require(out.size()==result.size(),"result count disagrees with contents");
  return out;
}
// Independent reference: enumerate records in original clause order, copying
// variable bindings on each branch. No engine indexes, grouping or planning.
facts reference_query(const std::map<uint64_t,entity_record>& records,const query& q) {
  facts out;
  if(q.all) {
    for(const auto& [e,row]:records) for(const auto& [a,v]:row) out[{e,a}]=v;
    return out;
  }
  using bindings=std::map<int32_t,bytes>;
  std::vector<triple> matched(q.clauses.size());
  auto walk=[&](auto&& self,size_t level,const bindings& bound)->void {
    if(level==q.clauses.size()) {
      for(auto i:q.projection) out[{matched[i].entity,matched[i].attribute}]=matched[i].value;
      return;
    }
    const auto& c=q.clauses[level];
    for(const auto& [id,row]:records) {
      auto it=row.find(c.attribute); if(it==row.end()) continue;
      auto next=bound;
      auto bind=[&](const term& t,const bytes& v) {
        if(t.variable<0) return t.constant==v;
        auto [position,inserted]=next.emplace(t.variable,v);
        return inserted || position->second==v;
      };
      if(bind(c.entity,entity_value(id)) && bind(c.value,it->second)) {
        matched[level]={id,c.attribute,it->second}; self(self,level+1,next);
      }
    }
  };
  walk(walk,0,{}); return out;
}
void incremental_queries() {
  directory d; engine db(d.path,false);
  const std::vector<query> queries={
    // Entity-local projections, predicates, equality, self references, repeated
    // clauses/projections, fixed entity zero and the maximum entity ID.
    {{{variable(0),"kind",constant("task")},{variable(0),"title",variable(1)},
       {variable(0),"done",variable(2)}},{0,1,2}},
    {{{variable(0),"done",constant("false")},{variable(0),"title",variable(1)}},{1}},
    {{{variable(0),"a",variable(1)},{variable(0),"b",variable(1)}},{0,1,1}},
    {{{variable(0),"owner",variable(0)},{variable(0),"title",variable(1)}},{1}},
    {{{constant(entity_value(0)),"title",variable(1)},
       {constant(entity_value(0)),"done",constant("false")}},{0}},
    {{{constant(entity_value(UINT32_MAX)),"title",variable(1)}},{0}},
    {{{variable(0),"title",variable(1)},{variable(0),"title",variable(1)}},{0,1}},
    {{{variable(0),"kind",constant("task")},{variable(0),"done",variable(1)}},{0}},
    // These must retain full evaluation: edits of one entity change facts
    // projected from a different entity, including multiple witnesses.
    {{{variable(0),"owner",variable(1)},{variable(1),"name",constant("Alex")},
       {variable(0),"title",variable(2)}},{2}},
    {{{variable(0),"title",variable(1)},{variable(2),"name",constant("Alex")}},{0,0}},
    {{{constant(entity_value(0)),"title",variable(1)},
       {constant(entity_value(10)),"name",constant("Alex")}},{0}},
    {{{constant(entity_value(0)),"owner",variable(0)},
       {variable(0),"name",variable(1)}},{1}},
  };
  std::vector<uint64_t> handles;
  std::vector<query_result> results;
  for(const auto& q:queries) { handles.push_back(db.subscribe(q).first); results.push_back(db.result(handles.back())); }
  const auto alias=db.subscribe(queries[0]).first;
  require(results[0].shares_with(db.result(alias)),"incremental query not shared");
  std::vector<facts> previous(queries.size());
  auto check=[&](const std::vector<uint64_t>& notifications) {
    std::vector<uint64_t> expected_notifications;
    std::map<uint64_t,entity_record> records;
    for(const auto& [id,encoded]:db.export_entities()) records[id]=decode_entity(id,encoded);
    for(size_t i=0;i<queries.size();++i) {
      // A fresh structurally distinct query uses full initial evaluation. Extra
      // duplicate projections preserve set semantics and prevent group sharing.
      auto oracle=queries[i];
      oracle.projection.insert(oracle.projection.end(),33,oracle.projection.front());
      auto reference=db.subscribe(oracle).first;
      const auto expected=contents(db.result(reference)), actual=contents(results[i]);
      db.unsubscribe(reference);
      require(actual==expected,"incremental result differs from full evaluation");
      require(actual==reference_query(records,queries[i]),"planned query differs from independent reference");
      if(actual!=previous[i]) {
        expected_notifications.push_back(handles[i]);
        std::set<uint32_t> entities, old_members, new_members;
        std::set<std::string> attributes;
        auto differences=[&](const facts& from,const facts& to) {
          for(const auto& [key,value]:from) {
            auto found=to.find(key);
            if(found==to.end() || found->second!=value) {
              entities.insert(uint32_t(key.first)); attributes.insert(key.second);
            }
          }
        };
        differences(actual,previous[i]); differences(previous[i],actual);
        for(const auto& [key,value]:previous[i]) old_members.insert(uint32_t(key.first));
        for(const auto& [key,value]:actual) new_members.insert(uint32_t(key.first));
        const auto& delta=results[i].changes();
        require(!delta.reset,"normal change requested reset");
        require(std::set<uint32_t>(delta.entities.begin(),delta.entities.end())==entities,"incorrect changed result entities");
        std::set<std::string> names;
        for(auto a:delta.attributes) names.insert(results[i].scope()->name_of(a));
        require(names==attributes,"incorrect changed projected attributes");
        require(delta.membership==(old_members!=new_members),"incorrect membership change");
      }
      if(i==0 && actual!=previous[i]) expected_notifications.push_back(alias);
      previous[i]=actual;
    }
    auto actual_notifications=notifications;
    std::sort(actual_notifications.begin(),actual_notifications.end());
    std::sort(expected_notifications.begin(),expected_notifications.end());
    require(actual_notifications==expected_notifications,"incremental notifications differ from result changes");
  };
  check(db.modify({{},{{0,"title","zero"},{0,"done","false"},{0,"kind","task"},
    {0,"owner",entity_value(10)},{1,"title","one"},{1,"done","false"},{1,"kind","task"},
    {10,"name","Alex"},{11,"name","Alex"},{UINT32_MAX,"title","last"}}}));
  // In/out of predicates, required-attribute deletion, whole-entity deletion,
  // cross-entity join dependencies, and nonprojected changes with no notification.
  check(db.modify({{},{{0,"done","true"},{1,"done","true"}}}));
  check(db.modify({{{1,"title","one"}},{{0,"done","false"}}}));
  check(db.import_entities({{10,std::nullopt},{UINT32_MAX,std::nullopt}}));
  check(db.import_entities({{11,std::nullopt},{0,std::nullopt}}));
  check(db.modify({{},{{0,"title","zero"},{0,"owner",entity_value(0)},
    {0,"a","same"},{0,"b","same"},{0,"kind","task"},{0,"done","false"}}}));
  check(db.modify({{},{{0,"b","different"}}}));
  check(db.modify({{},{{0,"unrelated","x"}}}));
  check(db.modify({{{0,"title","stale"}},{{0,"title","zero"}}}));

  std::mt19937 random(785);
  const std::array<std::string,8> attributes={"title","done","kind","a","b","owner","name","unrelated"};
  for(size_t step=0;step<120;++step) {
    if(step%4==0) {
      std::map<uint64_t,std::optional<bytes>> imports;
      for(size_t j=0;j<3;++j) {
        uint32_t id=random()%16;
        if(random()%3==0) imports[id]=std::nullopt;
        else imports[id]=encode_entity(id,{{"title",std::to_string(random()%4)},
          {"done",random()%2?"true":"false"},{"kind","task"},
          {"owner",entity_value(random()%16)},{"name",random()%2?"Alex":"Other"}});
      }
      check(db.import_entities(std::move(imports)));
    } else {
      edit delta;
      for(size_t j=0;j<3;++j) {
        uint32_t id=random()%16; auto a=attributes[random()%attributes.size()];
        bytes v=a=="owner"?entity_value(random()%16):a=="kind"?"task":
          a=="done"?(random()%2?"true":"false"):a=="name"?(random()%2?"Alex":"Other"):std::to_string(random()%4);
        if(random()%3==0) delta.remove.push_back({id,a,v});
        else delta.add.push_back({id,a,v});
      }
      check(db.modify(std::move(delta)));
    }
  }
}
void planned_queries() {
  directory d; engine db(d.path,false);
  std::map<uint64_t,entity_record> records;
  for(uint32_t id=0;id<12;++id) records[id]={
    {"title",std::to_string(id)},{"kind",id%2?"task":"person"},
    {"group",std::to_string(id%3)},{"mirror",std::to_string(id%2)},
    {"parent",entity_value((id+1)%12)},{"other",entity_value((id+2)%12)},
    {"self",entity_value(id)}};
  std::map<uint64_t,std::optional<bytes>> imports;
  for(const auto& [id,row]:records) imports[id]=encode_entity(id,row);
  db.import_entities(std::move(imports));
  std::vector<query> queries={
    // Opposite join direction, cycle, shared non-reference value, and a variable
    // appearing as both an entity and a value inside one grouped gather.
    {{{variable(2),"title",variable(3)},{variable(1),"parent",variable(2)},
      {variable(0),"parent",variable(1)},{variable(0),"title",constant("0")}},{0,1,2,3}},
    {{{variable(0),"parent",variable(1)},{variable(1),"parent",variable(2)},
      {variable(0),"other",variable(2)}},{0,1,2}},
    {{{variable(0),"group",variable(2)},{variable(1),"mirror",variable(2)},
      {variable(0),"title",variable(3)},{variable(1),"kind",constant("person")}},{2,1}},
    {{{variable(0),"self",variable(0)},{variable(0),"group",variable(1)},
      {variable(0),"mirror",variable(1)}},{0,1,2}},
    {{{variable(0),"group",variable(63)},{variable(0),"mirror",variable(63)}},{0,1}},
    {{{variable(0),"title",variable(1)},{variable(0),"missing",variable(2)}},{0}},
  };
  std::mt19937 random(854);
  for(const auto& original:queries) {
    const auto expected=reference_query(records,original);
    for(size_t trial=0;trial<20;++trial) {
      std::vector<uint32_t> order(original.clauses.size());
      for(uint32_t i=0;i<order.size();++i) order[i]=i;
      std::shuffle(order.begin(),order.end(),random);
      query q;
      for(auto i:order) q.clauses.push_back(original.clauses[i]);
      for(auto i:original.projection)
        q.projection.push_back(uint32_t(std::find(order.begin(),order.end(),i)-order.begin()));
      auto view=db.subscribe(q).first;
      require(contents(db.result(view))==expected,"clause permutation changed result/projection");
      db.unsubscribe(view);
    }
  }
  // Maximum clause/group count exercises the 32-bit group mask, with all values
  // unprojected except the last fact, and constant entity zero in the first group.
  query wide;
  edit seed;
  for(uint32_t i=0;i<32;++i) {
    auto a="field"+std::to_string(i); seed.add.push_back({i,a,"v"});
    wide.clauses.push_back({constant(entity_value(i)),a,variable(i)});
  }
  wide.projection={31}; db.modify(std::move(seed));
  auto view=db.subscribe(wide).first;
  require(db.result(view).size()==1&&db.result(view).lookup(31,"field31").has_value(),"32 entity groups");
  db.modify({{{0,"field0","v"}}, {}});
  require(db.result(view).empty(),"unprojected existence check was dropped");
}
void planner_work() {
  directory d; engine db(d.path,false); edit seed;
  for(uint64_t id=0;id<1000;++id) {
    seed.add.push_back({id,"title","task"});
    seed.add.push_back({id,"owner",entity_value(10000+id%10)});
  }
  for(uint64_t id=10000;id<10010;++id) seed.add.push_back({id,"region","common"});
  db.modify(std::move(seed));
  // The selective clause is deliberately last, across a reference join.
  query q{{{variable(0),"title",variable(2)},{variable(0),"owner",variable(1)},
    {variable(1),"region",constant("rare")}},{0}};
  auto view=db.subscribe(q).first;
  db.modify({{},{{10000,"region","rare"}}});
  require(db.result(view).size()==100,"selective join results");
  db.modify({{},{{10000,"region","common"}}});
  require(db.result(view).empty(),"empty anchor left stale results");
}
void incremental_work() {
  directory d; engine db(d.path,false);
  edit seed;
  for(uint32_t id=0;id<1000;++id) for(const auto& [a,v]:entity_record{
    {"kind","task"},{"title","title"},{"done","false"},{"id",std::to_string(id)},{"order",std::to_string(id)}})
    seed.add.push_back({id,a,v});
  db.modify(std::move(seed));
  query q{{{variable(0),"kind",constant("task")},{variable(0),"title",variable(1)},
    {variable(0),"done",variable(2)},{variable(0),"id",variable(3)},
    {variable(0),"order",variable(4)}},{0,1,2,3,4}};
  auto first=db.subscribe(q).first, alias=db.subscribe(q).first;
  auto result=db.result(first); auto unchanged=result.lookup(999,"title")->value;
  auto notifications=db.modify({{},{{0,"done","true"}}});
  require(notifications==std::vector<uint64_t>{first,alias},"incremental alias notifications");
  require(db.last_recomputed()==1&&result.size()==5000,"incremental group/count");
  require(result.lookup(999,"title")->value==unchanged,"untouched box replaced");
}
void packed_identity() {
  require(pair_key(0xffffffffU,0xffffffffU)==UINT64_MAX,"packed key maximum");
  require(pair_key(1,2)!=pair_key(2,1),"packed key ordering/identity");
  directory d, other; query_result retained; std::weak_ptr<const boxed_value> weak;
  uint32_t title_id=0; uint64_t key=0;
  {
    engine db(d.path,false,1<<24,"icare");
    require(db.new_entity()==1&&db.new_entity()==2,"sequential reservations");
    auto all=db.subscribe({{},{},true}).first, title=db.subscribe(attribute("title")).first;
    db.modify(all,{{},{{3,"title",std::string(4096,'v')},{3,"done","false"}}});
    retained=db.result(title); title_id=*retained.scope()->find("title"); key=pair_key(3,title_id);
    require(retained.get(key)->value==db.result(all).get(key)->value,"views copied value box");
    weak=retained.get(key)->value;
    db.modify(all,{{},{{3,"done","true"}}});
    require(db.result(all).get(key)->value==retained.get(key)->value,"unchanged fact lost stable box");
    auto long_name=std::string(1024,'a');
    bool unknown=false; try {db.modify(all,{{},{{3,long_name,"long attribute"}}});} catch(...) {unknown=true;}
    require(unknown,"unknown schema attribute accepted");
    require(db.new_entity()==4,"explicit IDs did not advance allocator");
    auto before=db.result(all); bool rejected=false;
    try {db.modify(all,{{},{{uint64_t(UINT32_MAX)+1,"title","bad"}}});} catch(...) {rejected=true;}
    require(rejected&&before.shares_with(db.result(all)),"out-of-range entity mutated state");
    db.import_entities({{3,std::nullopt}});
    require(db.result(title).empty()&&weak.expired(),"deletion invalidated retained box");
  }
  require(retained.empty()&&retained.scope()->name_of(title_id)=="title","result/scope lost on close");
  {
    bool rejected=false; try {engine wrong(d.path,false,1<<24,"wrong");} catch(...) {rejected=true;}
    require(rejected,"wrong namespace reopened");
    engine db(d.path,false,1<<24,"icare"); auto title=db.subscribe(attribute("title")).first;
    require(*db.result(title).scope()->find("title")==title_id,"attribute dictionary not persisted");
    require(db.new_entity()==5,"deleted/reserved ID reused after reopen");
    require(!db.result(title).scope()->find(std::string(1024,'a')).has_value(),"unknown attribute gained an ID");
    db.modify(title,{{},{{UINT32_MAX,"title","last"}}});
    rejected=false; try {db.new_entity();} catch(...) {rejected=true;}
    require(rejected,"entity counter wrapped");
    db.import_entities({{UINT32_MAX,std::nullopt}});
  }
  { engine db(d.path,false,1<<24,"icare"); bool rejected=false;
    try {db.new_entity();} catch(...) {rejected=true;} require(rejected,"exhausted counter lost on reopen"); }
  retained=query_result{}; require(weak.expired(),"last snapshot leaked value box");
}
void direct_reads() {
  directory d; engine db(d.path,false); auto all=db.subscribe({{},{},true}).first;
  auto integer=[](int64_t n) { writer w; w.u8(3); w.u64(uint64_t(n)); return w.data; };
  db.modify({{},{{1,"order",integer(7)},{2,"order",integer(-9)},{3,"order",integer(7)},
    {1,"part",integer(1)},{2,"part",integer(0)},{3,"part",integer(0)},{1,"blob",bytes("a\0b",3)}}});
  auto result=db.result(all); df_buffer out{};
  auto attr=[&](const char* name) {return df_result_attribute(result.native_handle(),reinterpret_cast<const uint8_t*>(name),std::char_traits<char>::length(name));};
  const auto order=attr("order"), part=attr("part"), blob=attr("blob");
  require(!df_result_attributes(result.native_handle(),1,&out),"entity attribute enumeration");
  reader names{{reinterpret_cast<const char*>(out.data),out.size}};
  auto name_count=names.u32(); std::set<std::string> fields;
  for(uint32_t i=0;i<name_count;++i) fields.insert(names.blob());
  names.end(); df_free(out.data);
  require(fields==std::set<std::string>({"order","part","blob"}),"attribute names include only requested row");
  require(!df_result_attributes(result.native_handle(),99,&out),"absent entity enumeration");
  reader missing_names{{reinterpret_cast<const char*>(out.data),out.size}};
  require(missing_names.u32()==0,"absent entity had attributes"); missing_names.end(); df_free(out.data);
  require(!df_result_entities(result.native_handle(),order,part,&out)&&out.size==12,"direct entity index");
  auto* ids=reinterpret_cast<const uint32_t*>(out.data);
  require(ids[0]==2&&ids[1]==3&&ids[2]==1,"signed/secondary ordering"); df_free(out.data);
  require(df_result_value(result.native_handle(),1,blob,&out)==0&&out.size==3,"borrowed field");
  const auto* borrowed=out.data;
  require(borrowed==reinterpret_cast<const uint8_t*>(result.value(1,blob)->data()),"field was copied");
  const bytes retained(reinterpret_cast<const char*>(out.data),out.size);
  require(df_result_find(result.native_handle(),blob,borrowed,out.size)==1,"direct find");
  require(df_result_count_value(result.native_handle(),blob,borrowed,out.size)==1,"direct count");
  require(df_result_value(result.native_handle(),99,blob,&out)==1&&!out.data,"missing borrowed field");
  db.modify({{},{{0,"kind","other"},{4,"order",integer(INT64_MAX)},
    {5,"order",integer(INT64_MIN)},{6,"order",integer(7)},{7,"kind","other"}}});
  require(result.entities(order)==std::vector<uint32_t>({5,2,1,3,6,4,0,7}),"missing primary sorts last, including integer extremes and ID ties");
  require(result.entities(order,part)==std::vector<uint32_t>({5,2,3,1,6,4,0,7}),"missing secondary sorts last within primary ties");
  require(result.entities()==std::vector<uint32_t>({0,1,2,3,4,5,6,7}),"default entity ID ordering");
  db.modify({{{1,"order",integer(7)}},{}});
  require(result.entities(order)==std::vector<uint32_t>({5,2,3,6,4,0,1,7}),"removed order moves to missing group");
  db.modify({{},{{1,"blob","replacement"},{2,"order","invalid"}}});
  require(retained==bytes("a\0b",3),"owned field copy changed");
  require(df_result_entities(result.native_handle(),order,0,&out)==1,"invalid order accepted"); df_free(out.data);
  query_result poisoned; df_test_poison_view(poisoned.native_handle());
  require(df_result_value(poisoned.native_handle(),1,blob,&out)==-1,"poisoned direct read succeeded");
  require(df_result_entities(poisoned.native_handle(),0,0,&out)==1,"poisoned enumeration succeeded"); df_free(out.data);
  require(df_result_attributes(poisoned.native_handle(),1,&out)==1,"poisoned attributes succeeded"); df_free(out.data);
}
void c_api() {
  directory d; df_buffer out{}; auto* db=df_open(reinterpret_cast<const uint8_t*>(d.path.data()),d.path.size(),1,&out); require(db,"C open");
  require(!df_attribute_schema(&out)&&std::string_view(reinterpret_cast<char*>(out.data),out.size)==attribute_schema,"C schema fingerprint");
  df_free(out.data);
  writer w; w.u8(1); w.u8(1); w.u8(1); w.u32(0); w.u32(0);
  require(!df_call(db,reinterpret_cast<const uint8_t*>(w.data.data()),w.data.size(),&out),"C subscribe"); reader r{{reinterpret_cast<const char*>(out.data),out.size}}; auto id=r.u64(); df_free(out.data);
  for(size_t i=0;i<w.data.size();++i) {require(df_call(db,reinterpret_cast<const uint8_t*>(w.data.data()),i,&out)==1,"truncated API accepted"); df_free(out.data);}
  writer lookup; lookup.u8(1); lookup.u8(9); lookup.u64(id); lookup.u64(1); lookup.blob("missing");
  require(!df_call(db,reinterpret_cast<const uint8_t*>(lookup.data.data()),lookup.data.size(),&out),"C pair lookup");
  require(out.size==1&&out.data[0]==0,"C absent pair"); df_free(out.data);
  for(size_t i=0;i<lookup.data.size();++i) {
    require(df_call(db,reinterpret_cast<const uint8_t*>(lookup.data.data()),i,&out)==1,"truncated lookup accepted"); df_free(out.data);
  }
  auto* snapshot=df_result(db,id,&out); require(snapshot,"C snapshot");
  require(!df_result_changes(snapshot,&out),"C initial change summary");
  reader changes{{reinterpret_cast<const char*>(out.data),out.size}};
  require(changes.u8()==1&&changes.u8()==0&&changes.u32()==0&&changes.u32()==0,"C reset summary payload");
  changes.end(); df_free(out.data);
  auto* same=df_result(db,id,&out); require(df_result_same(snapshot,same),"C root sharing");
  require(!df_result_list(snapshot,&out)&&out.size==4,"C bulk snapshot"); df_free(out.data); df_result_release(same);
  writer flush; flush.u8(1); flush.u8(11);
  require(!df_call(db,reinterpret_cast<const uint8_t*>(flush.data.data()),flush.data.size(),&out),"C flush barrier");
  df_free(out.data);
  require(!df_close_checked(db,&out),"C checked close"); df_free(out.data);
  require(!df_result_info(snapshot,&out)&&out.size==8,"C retained result"); df_free(out.data); df_result_release(snapshot);
}
void asynchronous_persistence() {
  directory d;
  {
    engine db(d.path);
    auto q=db.subscribe(attribute("title")).first;
    auto result=db.result(q);
    db.modify({{},{{1,"title","one"}}});
    db.flush();
    require(db.flush_attempts_for_testing()>0,"default did not use async worker");
    db.fail_flush_for_testing(true);
    db.modify({{},{{1,"title","two"}}});
    bool failed=false; try {db.flush();} catch(...) {failed=true;}
    require(failed,"flush failure not reported");
    failed=false; try {db.modify({{},{{1,"title","rejected"}}});} catch(...) {failed=true;}
    require(failed && result.lookup(1,"title")->value->value=="two","failed flush changed live state");
    db.fail_flush_for_testing(false); db.flush();
    db.new_entity();
    db.import_entities({{3,encode_entity(3,{{"title","imported"}})}});
    // Destruction must drain ID allocation, dictionary and imported records too.
  }
  engine reopened(d.path);
  require(reopened.new_entity()==4,"ID allocation/import not persisted");
  auto records=reopened.export_entities();
  require(decode_entity(1,records.at(1)).at("title")=="two","edit not persisted");
  require(decode_entity(3,records.at(3)).at("title")=="imported","import not persisted");
}
}
int main() {
  try {
    const std::pair<const char*,void(*)()> cases[]={
      {"semantics",semantics},{"shared queries",shared_queries},
      {"fixed entity routing",fixed_entity_routing},
      {"predicate routing",predicate_routing},
      {"fact storage",fact_storage},
      {"repeated projection",repeated_projection},{"map full rollback",map_full_rollback},
      {"packed identity",packed_identity},{"late failure recovery",late_failure_recovery},
      {"mutable churn",mutable_churn},{"incremental queries",incremental_queries},
      {"incremental work",incremental_work},{"planned queries",planned_queries},
      {"planner work",planner_work},
      {"direct reads",direct_reads},{"async persistence",asynchronous_persistence},{"C ABI",c_api}};
    for(auto [name,test]:cases) { std::cerr<<"Running "<<name<<std::endl; test(); }
    std::cout<<"PASS: entities, indexes, queries, live maps, direct edits, recovery, C ABI\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
