// SPDX-License-Identifier: GPL-2.0-or-later
#include "diagnostics.h"
#include <util/base.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <windows.h>
namespace xen::recorder {
struct Diagnostics::Impl {
    struct Entry { int level = 0; std::array<char,2048> text{}; };
    std::array<Entry,512> ring{};
    std::size_t begin = 0,count = 0;
    std::uint64_t dropped = 0;
    std::mutex mutex;
    std::condition_variable condition;
    std::ofstream file;
    std::thread thread;
    bool closing = false;
    log_handler_t previous = nullptr; void* previous_param = nullptr;
    static void callback(int level,const char* format,va_list args,void* parameter) {
        auto& p = *static_cast<Impl*>(parameter);
        // libobs可从编码/渲染线程日志；这些线程仅格式化到有界内存，不接触磁盘。
        std::lock_guard lock(p.mutex);
        if (p.count == p.ring.size() || p.closing) { ++p.dropped; return; }
        auto& entry = p.ring[(p.begin+p.count)%p.ring.size()]; entry.level = level;
        vsnprintf(entry.text.data(),entry.text.size(),format,args);
        ++p.count; p.condition.notify_one();
    }
    void run() {
        for (;;) {
            Entry entry;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock,[&]{return count || closing;});
                if (!count) break;
                entry = ring[begin]; begin = (begin+1)%ring.size(); --count;
            }
            file << '[' << entry.level << "] " << entry.text.data() << '\n'; file.flush();
        }
        file << "diagnostic_queue_dropped=" << dropped << '\n';
    }
};
Diagnostics::Diagnostics(const std::filesystem::path& directory) : impl_(std::make_unique<Impl>()) {
    std::filesystem::create_directories(directory);
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto& p = *impl_;
    p.file.open(directory/(L"recorder-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(stamp)+L".log"),std::ios::binary);
    if (!p.file) throw std::runtime_error("录制诊断日志创建失败");
    base_get_log_handler(&p.previous,&p.previous_param);
    p.thread = std::thread([&p]{p.run();});
    base_set_log_handler(Impl::callback,&p);
}
Diagnostics::~Diagnostics() {
    auto& p = *impl_; base_set_log_handler(p.previous,p.previous_param);
    { std::lock_guard lock(p.mutex); p.closing = true; }
    p.condition.notify_one(); if (p.thread.joinable()) p.thread.join();
}
}
