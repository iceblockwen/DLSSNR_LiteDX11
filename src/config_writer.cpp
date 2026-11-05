#include "config_writer.hpp"
#include "runtime.hpp"
#include "graphics.hpp"
#include "config_storage.hpp"
#include <algorithm>
#include <atomic>

namespace lite {
namespace {std::atomic<ConfigWriter*> writer{};}
ConfigWriter::ConfigWriter(Write output,std::function<void()> onError)
    :write(std::move(output)),error(std::move(onError)),worker([this]{Run();}) {}
ConfigWriter::~ConfigWriter() {
    {std::lock_guard lock(mutex);stopping=true;}
    changed.notify_one();worker.join();
}
void ConfigWriter::Queue(const wchar_t* section,const wchar_t* key,const wchar_t* value) {
    std::lock_guard lock(mutex);if(stopping)return;
    Key k{section,key};auto p=pending.find(k);
    if(p!=pending.end() && p->second.text==value)return;
    auto old=saved.find(k);
    if(!writing && p==pending.end() && old!=saved.end() && old->second==value)return;
    const auto now=std::chrono::steady_clock::now();
    if(pending.empty())first=now;
    pending[k]={value,0};
    due=(std::min)(now+std::chrono::milliseconds(150),first+std::chrono::seconds(1));
    changed.notify_one();
}
void ConfigWriter::Flush() {
    {std::lock_guard lock(mutex);due=std::chrono::steady_clock::now();}
    changed.notify_one();
}
void ConfigWriter::Run() {
    std::unique_lock lock(mutex);
    for(;;) {
        changed.wait(lock,[&]{return stopping||!pending.empty();});
        if(stopping&&pending.empty())return;
        if(!stopping && std::chrono::steady_clock::now()<due) {changed.wait_until(lock,due);continue;}
        auto batch=std::move(pending);pending.clear();writing=true;lock.unlock();
        for(auto& [key,value]:batch) {
            bool ok=false;try {ok=write(key.first,key.second,value.text);}catch(...) {}
            if(!ok&&error)try {error();}catch(...) {}
            lock.lock();
            if(ok)saved[key]=value.text;
            else if(!stopping && !pending.contains(key) && ++value.attempts<3) {
                if(pending.empty())first=std::chrono::steady_clock::now();
                pending[key]=value;due=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            }
            lock.unlock();
        }
        lock.lock();writing=false;
    }
}
void StartConfigWriter() {
    try {
        const auto s=Snapshot();if(!s.enabled || !s.pluginPath[0])return;
        writer=new ConfigWriter(WriteConfigValue,[]{Log(L"Cannot save configuration in either selected or fallback directory; background retry is bounded");});
    }catch(...) {Log(L"Cannot start configuration writer; runtime settings still apply");}
}
void QueueConfig(const wchar_t* section,const wchar_t* key,const wchar_t* value) noexcept {
    try {if(auto* p=writer.load())p->Queue(section,key,value);}catch(...) {Log(L"Cannot queue resource setting");}
}
void FlushConfig() noexcept {try {if(auto* p=writer.load())p->Flush();}catch(...) {}}
}
