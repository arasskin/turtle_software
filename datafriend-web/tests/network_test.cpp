#include "datafriend/engine.h"
#include "datafriend/wire_codec.h"
#include "sync.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace datafriend;
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
struct directory {
  std::string path;
  directory() { auto base=std::getenv("TMPDIR"); std::string pattern=std::string(base?base:"/tmp")+"/datafriend-network-XXXXXX"; auto* d=mkdtemp(pattern.data()); if(!d) throw std::runtime_error("mkdtemp"); path=d; }
  ~directory() { std::filesystem::remove_all(path); }
};
struct exchange {
  lb_sync::state& server;
  uint64_t epoch=0,revision=0;
  bool absent=false;
  std::string name=std::string(16,'a');
  bytes request(unsigned op,std::string_view payload={},bool writable=true) {
    auto packet=lb_sync::handle(&server,op,name,writable,epoch,revision,payload); reader r{packet};
    auto status=r.u8(); epoch=r.u64(); revision=r.u64();
    if(op==0&&status==3) { absent=true; return {}; }
    if(op==0) absent=false;
    if(status) throw std::runtime_error("stale");
    return std::string(r.data);
  }
  void reconcile(engine& db,bool push=false,bool server_starts=false) {
    bytes packet;
    if(server_starts) {
      auto opening=lb_sync::opening(&server,name); reader r{opening};
      require(r.u8()==0,"server opening failed"); epoch=r.u64(); revision=r.u64();
      packet=db.sync(5,r.data);
      if(packet.size()==4) return;
    } else { request(0,std::string(1,push?1:0)); packet=db.sync(0); }
    for(unsigned i=0;;++i) {
      require(i<128,"round limit");
      packet=db.sync(1,request(1,packet)); if(packet.size()==4) break;
    }
  }
  void push(engine& db) {
    reconcile(db,true); for(;;) { auto batch=db.sync(2); reader r{batch}; if(!r.u32()&&!absent) break; request(3,batch); absent=false; } db.sync(4);
  }
  bytes fetch(engine& db) { auto keys=db.sync(3); return request(2,std::string_view(keys).substr(1)); }
  auto pull(engine& db) {
    reconcile(db,false,true); std::vector<uint64_t> changes;
    for(;;) { auto keys=db.sync(3); changes=db.sync_apply(request(2,std::string_view(keys).substr(1))); if(keys[0]) break; }
    db.sync(4); return changes;
  }
};
int main() {
  directory root;
  engine owner(root.path+"/owner"), replica(root.path+"/replica");
  lb_sync::state server;
  lb_sync::create(&server,(root.path+"/cache").c_str(),77,1<<26);
  exchange x{server};
  auto [all,handle]=replica.subscribe({{}, {},true});
  edit initial;
  for(uint64_t i=0;i<4000;++i) { initial.add.push_back({i,"title","task"+std::to_string(i)}); initial.add.push_back({i,"done","false"}); }
  initial.add.push_back({UINT32_MAX,"title","last"});
  owner.modify(std::move(initial)); x.push(owner);
  auto changed=x.pull(replica);
  require(changed==std::vector<uint64_t>{all},"pull notification");
  require(owner.export_facts()==replica.export_facts()&&handle.size()==8001,"initial convergence");
  require(x.pull(replica).empty(),"equal pull notified");
  owner.modify({{{3,"title","task3"}},{{5,"title","changed"},{7000,"done","true"}}});
  x.push(owner); x.pull(replica);
  require(owner.export_facts()==replica.export_facts(),"edit/delete convergence");
  // Stable snapshot survives local edits; a later exchange publishes the latest.
  x.reconcile(owner); auto before=owner.export_facts();
  owner.modify({{},{{5,"title","during exchange"}}});
  x.request(3,owner.sync(2)); owner.sync(4); x.pull(replica);
  require(replica.export_facts()==before,"snapshot changed during exchange");
  x.push(owner); x.pull(replica);
  // Pull must not overwrite a local transaction that happened during the exchange.
  x.reconcile(replica); replica.modify({{},{{0,"title","local concurrent edit"}}});
  bool stale=false; try { replica.sync_apply(x.fetch(replica)); } catch(...) { stale=true; }
  require(stale,"stale pull accepted"); replica.sync(4); x.pull(replica);
  require(owner.export_facts()==replica.export_facts(),"retry convergence");
  exchange old{server}; old.reconcile(replica);
  owner.modify({{},{{4,"done","true"}}}); x.push(owner);
  stale=false; try { old.fetch(replica); } catch(...) { stale=true; }
  require(stale,"stale server version accepted"); replica.sync(4);
  // Authentication's write decision reaches storage before mutation.
  bool rejected=false; writer empty; empty.u32(0);
  try { x.request(3,empty.data,false); } catch(...) { rejected=true; }
  require(rejected,"read-only write accepted");
  // Malformed entire batch is rejected before a valid earlier record is changed.
  writer bad; bad.u32(2); auto facts=owner.export_facts(); auto [key,record]=*facts.begin();
  bad.u64(key); bad.u8(0); bad.u64(key+1); bad.u8(1); bad.blob("bad");
  rejected=false; try { x.request(3,bad.data); } catch(...) { rejected=true; }
  require(rejected,"malformed batch accepted"); x.pull(replica);
  require(owner.export_facts()==replica.export_facts(),"malformed batch partially applied");
  // Namespace separation, empty-owner deletions, and uint32 endpoint IDs.
  x.name=std::string(16,'b'); engine emptyOwner(root.path+"/empty"); x.push(emptyOwner); x.pull(replica);
  require(replica.export_facts().empty(),"remote authoritative deletion");
  x.name=std::string(16,'a'); x.pull(replica); require(owner.export_facts()==replica.export_facts(),"namespace isolation");
  // Every truncation fails without importing anything.
  x.reconcile(replica); auto records=x.fetch(replica);
  for(size_t n=0;n<records.size();++n) { rejected=false; try { replica.sync_apply(std::string_view(records).substr(0,n)); } catch(...) { rejected=true; } require(rejected,"truncated import accepted"); }
  replica.sync(4);
  // Prepared async writes own their buffers, block conflicting namespace
  // exchanges, and can be cancelled before submission without changing facts.
  writer asyncBatch; asyncBatch.u32(1); asyncBatch.u64(pair_key(9000,1)); asyncBatch.u8(1);
  asyncBatch.blob(encode_fact(pair_key(9000,1),"async"));
  auto task=lb_sync::handle_async(&server,3,std::string(16,'a'),true,x.epoch,x.revision,asyncBatch.data);
  task.resume(); require(task.pending()&&!task.done(),"async write did not suspend");
  auto busy=lb_sync::handle(&server,0,std::string(16,'a'),true,0,0,std::string(1,0));
  require(reader{busy}.u8()==1,"concurrent request observed transferring namespace");
  task={}; // cancels a prepared request and releases the namespace guard
  x.pull(replica); require(owner.export_facts()==replica.export_facts(),"cancelled async write published");
  // A subscriber pins one file read, not its whole namespace. An owner can
  // publish another record while that read is outstanding; the old fetch then
  // returns a fresh opening instead of publishing its stale bytes.
  writer fetchOne; fetchOne.u32(1); fetchOne.u64(owner.export_facts().begin()->first);
  auto reading=lb_sync::handle_async(&server,2,x.name,false,x.epoch,x.revision,fetchOne.data);
  reading.resume(); require(reading.pending(),"fetch did not suspend");
  storage::io_operation readIO{};
  require(storage::start_io(reading.pending(),&readIO).code==storage::error::ok,"start pinned read");
  auto readBytes=pread(readIO.fd,readIO.buffer,readIO.length,readIO.offset);
  owner.modify({{},{{9001,"title","owner continues during subscriber fetch"}}});
  x.push(owner);
  storage::complete_io(reading.pending(),readBytes<0?-errno:int(readBytes)); reading.resume();
  require(reading.done(),"stale read did not finish");
  auto restart=reading.result(); reader restartReader{restart};
  require(restartReader.u8()==1,"stale read did not restart");
  restartReader.u64(); restartReader.u64();
  require(restartReader.data.starts_with("LBR\3"),"restart omitted new opening");
  reading={}; x.pull(replica);
  require(owner.export_facts()==replica.export_facts(),"subscriber restart convergence");
  // Complete a write through short kernel-style completions, then read it.
  task=lb_sync::handle_async(&server,3,std::string(16,'a'),true,x.epoch,x.revision,asyncBatch.data);
  task.resume();
  while(!task.done()) {
    auto* pending=task.pending(); require(pending!=nullptr,"missing pending request");
    while(pending->state==storage::phase::ready) {
      storage::io_operation operation{};
      require(storage::start_io(pending,&operation).code==storage::error::ok,"start async I/O");
      auto n=pwrite(operation.fd,operation.buffer,std::min(operation.length,uint32_t(3)),operation.offset);
      storage::complete_io(pending,n<0?-errno:int(n));
    }
    task.resume();
  }
  require(reader{task.result()}.u8()==0,"async completion failed"); task={};
  x.push(owner); // removes the test-only record before the large transfer
  // Transfer larger than a wire frame: pages must publish exactly once.
  edit large;
  for(uint64_t i=10000;i<10160;++i) large.add.push_back({i,"title",std::string(60000,'x')});
  owner.modify(std::move(large)); x.push(owner);
  require(x.pull(replica)==std::vector<uint64_t>{all},"paged import notification");
  require(owner.export_facts()==replica.export_facts(),"large paged transfer");
  // Optimistic batches are paired with the same stable snapshot fingerprint.
  // Subscriber mismatch must leave durable data, indexes and live views intact.
  x.name=std::string(16,'c');engine fast(root.path+"/fast"),follower(root.path+"/follower");
  fast.sync_track();x.push(fast);x.pull(follower);
  auto [watch,view]=follower.subscribe({{},{},true});
  auto baseline=fast.revision();fast.modify({{},{{1,"title","first"},{2,"done","false"}}});
  auto delta=fast.sync_delta(baseline);require(bool(delta),"missing optimistic batch");
  x.request(6,*delta);fast.sync(4);
  auto published=follower.sync_apply_delta(*delta);
  require(published&&*published==std::vector<uint64_t>{watch}&&view.size()==2,"optimistic publication");
  require(follower.sync_apply_delta(*delta)->empty(),"duplicate delta notified");
  baseline=fast.revision();fast.modify({{{2,"done","false"}},{{1,"title","second"}}});
  delta=fast.sync_delta(baseline);
  follower.modify({{},{{3,"title","extra"}}});auto unchanged=follower.export_facts();auto rev=follower.revision();
  require(!follower.sync_apply_delta(*delta),"mismatch accepted");
  require(follower.export_facts()==unchanged&&follower.revision()==rev&&view.size()==3,"mismatch partially published");
  x.request(6,*delta);fast.sync(4);x.pull(follower);
  require(follower.export_facts()==fast.export_facts(),"subscriber mismatch recovery");
  // Missed server record: applying only the new frame cannot repair the missing
  // prior fact, so continue reconciliation against the retained owner snapshot.
  hash_table::key ns{};std::memcpy(&ns,x.name.data(),sizeof ns);
  lb_sync::check(storage::erase_record(&server.cache,ns,pair_key(1,uint32_t(attribute::title))));
  baseline=fast.revision();fast.modify({{},{{2,"done","true"}}});delta=fast.sync_delta(baseline);
  auto mismatch=lb_sync::handle(&server,6,x.name,true,x.epoch,x.revision,*delta);reader mr{mismatch};
  require(mr.u8()==1,"server fingerprint mismatch not detected");x.epoch=mr.u64();x.revision=mr.u64();
  auto continuation=fast.sync(1,mr.data);
  while(continuation.size()!=4)continuation=fast.sync(1,x.request(1,continuation));
  // An intervening local edit must not change that retained snapshot.
  auto snapshot=fast.export_facts();fast.modify({{},{{4,"title","during fallback"}}});
  for(;;){auto batch=fast.sync(2);reader r{batch};if(!r.u32())break;x.request(3,batch);}fast.sync(4);x.pull(follower);
  require(follower.export_facts()==snapshot,"optimistic fallback snapshot changed");
  auto next=fast.sync_delta(baseline+1);require(bool(next),"pending edit lost");x.request(6,*next);fast.sync(4);x.pull(follower);
  require(fast.export_facts()==follower.export_facts(),"following delta mismatch");
  // Bounded journal drops to ordinary reconciliation, never loses an edit.
  baseline=fast.revision();edit churn;for(unsigned i=100;i<4300;++i)churn.add.push_back({i,"title","x"});fast.modify(std::move(churn));
  require(!fast.sync_delta(baseline),"journal cap did not fall back");
  x.push(fast);x.pull(follower);require(fast.export_facts()==follower.export_facts(),"journal overflow convergence");
  std::cout<<"PASS: AELMDB/Littlebear reconciliation, snapshots, versions, imports, notifications and namespaces\n";
}
