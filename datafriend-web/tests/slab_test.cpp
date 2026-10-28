#include <cstring>
#include "datafriend/wide_tree.h"
#include <iostream>
#include <random>

namespace {
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
uint64_t key_hash(uint64_t key) { return key * UINT64_C(11400714819323198485); }
}

int main() {
  try {
    datafriend::slab_pool pool;
    std::vector<size_t> sizes;
    for(size_t cap=4;cap<1500;cap+=std::max<size_t>(1,cap/4)) sizes.push_back(24+8*cap);
    struct block { void* p=nullptr; size_t bytes=0; uint8_t marker=0; };
    std::vector<block> live(4000);
    std::mt19937_64 rng(735);
    auto release=[&](block& b) {
      if(!b.p) return;
      const auto* p=static_cast<uint8_t*>(b.p);
      for(size_t i=0;i<b.bytes;++i) require(p[i]==b.marker,"slab overlap/corruption");
      pool.deallocate(b.p,b.bytes); b.p=nullptr;
    };
    for(size_t step=0;step<40000;++step) {
      auto& b=live[rng()%live.size()]; release(b);
      b.bytes=sizes[rng()%sizes.size()]; b.marker=uint8_t(rng());
      b.p=pool.allocate(b.bytes);
      require(reinterpret_cast<uintptr_t>(b.p)%8==0,"slab alignment");
      std::memset(b.p,b.marker,b.bytes);
      if(step%1000==0) pool.trim(); // must preserve every live block
    }
    for(auto& b:live) release(b);
    require(pool.stats().live_bytes==0 && pool.stats().fallback_live_bytes==0,"slab leak");
    require(pool.stats().slab_frees>0 && pool.stats().fallback_allocations>0,"unexercised paths");
    pool.trim(); require(pool.stats().reserved_bytes==0,"trim retained slabs");
    // Fill multiple slabs in one class, release/reuse without a system allocation.
    std::vector<void*> blocks;
    for(size_t i=0;i<3000;++i) blocks.push_back(pool.allocate(56));
    pool.deallocate(blocks[100],56);
    const auto allocs=pool.stats().slab_allocations;
    blocks[100]=pool.allocate(56);
    require(pool.stats().slab_allocations==allocs,"free slot not reused");
    for(auto* p:blocks) pool.deallocate(p,56);
    require(pool.stats().reserved_bytes==datafriend::slab_pool::slab_bytes,"empty slab cache is unbounded");
    pool.trim();
    {
      datafriend::wide_tree reference(key_hash);
      datafriend::slab_wide_tree survivor(key_hash,datafriend::slab_nodes(pool));
      for(size_t i=0;i<2500;++i) {auto r=datafriend::record{key_hash(i),i+1,i+1};survivor.set(r);reference.set(r);}
      {
        datafriend::slab_wide_tree temporary(key_hash,datafriend::slab_nodes(pool));
        for(size_t i=0;i<2500;++i) temporary.set(datafriend::record{key_hash(i),i+7,i+7});
        temporary.validate();
      }
      pool.trim(); survivor.validate();
      require(survivor.list({},SIZE_MAX)==reference.list({},SIZE_MAX),"shared slab lifetime");
      require(survivor.shape_digest()==reference.shape_digest(),"allocator changed shape");
      require(survivor.allocations().copied_bytes==reference.allocations().copied_bytes,"allocator changed copies");
      survivor.erase(key_hash(71)); survivor.set(datafriend::record{UINT64_MAX,7,7});
      reference.erase(key_hash(71)); reference.set({UINT64_MAX,7,7});
      require(survivor.list({},SIZE_MAX)==reference.list({},SIZE_MAX),"shared root edits");
    }
    require(pool.stats().live_bytes==0,"shared roots leaked");
    pool.trim(); require(pool.stats().reserved_bytes==0,"shared trim failed");
    std::cout<<"PASS: slab overlap/alignment, every size class, fallback, reuse, reclamation, shared roots and edits\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
