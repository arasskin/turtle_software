#include "async_flush.h"
#include <atomic>
#include <future>
#include <iostream>
using namespace datafriend;
using namespace std::chrono_literals;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
  try {
    {
      std::atomic<int> calls=0;
      async_flush f([&]{++calls; return 0;},1h);
      for(int i=0;i<100;++i) f.written();
      f.flush(); require(calls==1,"writes were not coalesced");
      f.flush(); require(calls==1,"clean barrier flushed again");
      f.written(); f.stop(); require(calls==2,"close did not drain pending writes");
    }
    {
      std::atomic<int> calls=0;
      std::promise<void> entered, release;
      auto allow=release.get_future().share();
      async_flush f([&]{ if(++calls==1) {entered.set_value(); allow.wait();} return 0;},1h);
      f.written();
      auto barrier=std::async(std::launch::async,[&]{f.flush();});
      entered.get_future().wait();
      auto write=std::async(std::launch::async,[&]{f.written();});
      const bool nonblocking=write.wait_for(1s)==std::future_status::ready;
      // Stop while a flush is active, with a newer write pending. It must flush
      // that newer write too before allowing the fd to be closed.
      auto close=std::async(std::launch::async,[&]{return f.stop();});
      release.set_value(); write.get(); barrier.get();
      require(close.get()==0 && calls==2,"write during sync lost at close");
      require(nonblocking,"disk flush blocked a writer on the worker mutex");
    }
    {
      std::atomic<bool> fail=true;
      async_flush f([&]{return fail?EIO:0;},1h);
      f.written();
      bool failed=false; try {f.flush();} catch(const std::system_error&) {failed=true;}
      require(failed,"flush error swallowed");
      failed=false; try {f.check();} catch(const std::system_error&) {failed=true;}
      require(failed,"background error not visible to writers");
      fail=false; f.flush(); f.check();
    }
    {
      async_flush f([]{return 0;},20ms);
      const auto until=std::chrono::steady_clock::now()+200ms;
      while(std::chrono::steady_clock::now()<until) { f.written(); std::this_thread::sleep_for(1ms); }
      require(f.attempts()>0,"continuous writes postponed automatic flushes");
    }
    std::cout<<"PASS: batching, periodic progress, nonblocking writes, barriers, shutdown and errors\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
