// Real production-server interoperability; pass numeric host and port explicitly.
#include "datafriend/network.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace datafriend;
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("usage: transport_test HOST PORT");
 auto root=std::filesystem::temp_directory_path()/("df-transport-"+std::to_string(getpid()));std::filesystem::remove_all(root);
 struct cleanup{std::filesystem::path p;~cleanup(){std::filesystem::remove_all(p);}} guard{root};
 identity a("test",true),b("test",true);auto ka=public_key_hex(a),kb=public_key_hex(b);
 engine owner((root/"owner").string(),durability::asynchronous,1<<26,ka),replica((root/"replica").string(),durability::asynchronous,1<<26,ka);
 engine other((root/"other").string(),durability::asynchronous,1<<26,kb),other_replica((root/"other-replica").string(),durability::asynchronous,1<<26,kb);
 auto [sid,result]=replica.subscribe({{},{},true});unsigned notifications=0;
 network x(argv[1],uint16_t(std::stoi(argv[2]))),y(argv[1],uint16_t(std::stoi(argv[2])));
 x.measure_requests();x.push(owner,a);x.subscribe(other_replica,kb);y.push(other,b);y.subscribe(replica,ka,[&](const auto& changed){if(!changed.empty())++notifications;});
 auto wait=[&](auto condition){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);while(!condition()||!x.settled()||!y.settled()){x.pump();y.pump();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Timeout: "+x.error()+" / "+y.error());std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
 wait([&]{return true;});
 // First establish a signed baseline, then exercise only the fast path.
 owner.modify({{},{{1,"title","baseline"}}});wait([&]{return result.size()==1;});
 auto responses=x.stats().request_count;auto requests=x.stats().requests,uploads=x.stats().optimistic_uploads,publications=y.stats().optimistic_publications;
 owner.modify({{},{{1,"title","one trip"}}});wait([&]{return owner.export_facts()==replica.export_facts();});
 if(x.stats().requests!=requests+1||x.stats().optimistic_uploads!=uploads+1||y.stats().optimistic_publications!=publications+1)
   throw std::runtime_error("Steady update did not use one upload and direct publication: requests="+std::to_string(requests)+"->"+std::to_string(x.stats().requests)+", uploads="+std::to_string(uploads)+"->"+std::to_string(x.stats().optimistic_uploads)+", publications="+std::to_string(publications)+"->"+std::to_string(y.stats().optimistic_publications));
 if(x.stats().request_count!=responses)throw std::runtime_error("Upload produced a success reply");
 bool measured=false;for(auto& t:x.stats().request_samples)if(t.operation==6&&t.snapshot_revision==owner.revision())measured=t.sent_us>0&&t.elapsed_us==0;
 if(!measured)throw std::runtime_error("Missing send-only timing");
 // Another connection changes the server behind this owner's journal. Queue
 // subsequent edits before recovery; reconcile the newest owner state in full.
 {
   engine rogue((root/"rogue").string(),durability::asynchronous,1<<26,ka);
   rogue.modify({{},{{7000,"title","server-only"}}});
   network z(argv[1],uint16_t(std::stoi(argv[2])));z.push(rogue,a);
   auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
   while(rogue.export_facts()!=replica.export_facts()||!z.settled()){
     z.pump();y.pump();if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Rogue sync timeout");
     std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
 }
 auto owner_fallback=x.stats().optimistic_fallbacks;
 for(int i=0;i<20;i++){owner.modify({{},{{1,"title","pipeline "+std::to_string(i)}}});x.pump();}
 wait([&]{return owner.export_facts()==replica.export_facts();});
 if(x.stats().optimistic_fallbacks<=owner_fallback)throw std::runtime_error("Missing owner recovery");
 // A corrupt replica must reject the candidate transaction and reconcile.
 replica.modify({{},{{9000,"title","replica-only"}}});
 auto fallback=y.stats().optimistic_fallbacks;
 owner.modify({{},{{1,"title","repair subscriber"}}});wait([&]{return owner.export_facts()==replica.export_facts();});
 if(y.stats().optimistic_fallbacks!=fallback+1)throw std::runtime_error("Missing subscriber fallback");
 edit changes;for(int i=1;i<=300;i++)changes.add.push_back({uint64_t(i),"title",std::string(1000,'x')});owner.modify(std::move(changes));other.modify({{},{{1,"title","peer"}}});
 wait([&]{return result.size()==300&&other.export_facts()==other_replica.export_facts();});
 if(owner.export_facts()!=replica.export_facts())throw std::runtime_error("Initial mismatch");
 y.online(false);owner.modify({{{2,"title",std::string(1000,'x')}},{{1,"title","edited 🌍"}}});
 for(int i=0;i<100;i++){x.pump();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
 y.online(true);wait([&]{return result.size()==299&&owner.export_facts()==replica.export_facts();});
 // Several revisions while snapshots/fetches are active: stale pages never publish
 // into a newer exchange; eventual contents equal the current owner exactly.
 for(int i=0;i<50;i++){owner.modify({{},{{1,"title",std::to_string(i)}}});x.pump();y.pump();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
 wait([&]{return owner.export_facts()==replica.export_facts();});
 auto facts=owner.export_facts();std::map<uint64_t,std::optional<bytes>> erase;for(auto& [k,v]:facts)erase[k]={};owner.import_facts(std::move(erase));wait([&]{return result.empty();});
 if(!notifications||x.stats().signed_batches==0||y.stats().signed_batches==0)throw std::runtime_error("Missing publications/signatures");
 std::cout<<"PASS native TCP: signed empty creation, one-trip optimistic updates, direct publication, mismatch recovery, multiplexing, paged sync, Unicode, deletion, offline/reconnect, edits during sync, clear; sent="<<x.stats().sent+y.stats().sent<<" received="<<x.stats().received+y.stats().received<<" notifications="<<notifications<<"\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
