#include "input_training/input_training_internal.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <thread>

namespace fault {
thread_local bool next_allocation = false;
thread_local bool keep_failing = false;
thread_local unsigned hits = 0;
}

// 独立测试EXE限定当前调用线程的下一次分配，不替换std::thread或生产catch。
// MSVC thread::_Start的make_unique<Tuple>是构造线程的首次分配。
void* operator new(std::size_t size) {
    if (fault::next_allocation) {
        fault::next_allocation = fault::keep_failing;
        ++fault::hits;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void run_case(const std::filesystem::path& root, const std::filesystem::path& recorded,
              bool replay, bool after_completed) {
    using namespace input_training;
    const auto name = std::string(replay ? "replay-" : "record-") + (after_completed ? "after" : "initial");
    Session session;
    if (after_completed) {
        expect(session.start(root / (name + "-warm")), "D23 warm start succeeds");
        session.stop();
        expect(session.snapshot()->status == Status::STOPPED, "D23 warm round completes");
    }
    detail::SessionTestAccess::before_worker_start(session, [] {
        fault::keep_failing = true;
        fault::next_allocation = true;
    });
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> subscription = owner;
    std::atomic<int> reader_calls{0};
    Session::Reader reader = [token = std::move(owner), &reader_calls] { ++reader_calls; return ReadBatch{}; };
    const auto directory = replay ? recorded : root / (name + "-failed");
    const unsigned before = fault::hits;
    const bool started = replay ? session.load(directory) : session.start(directory, {}, std::move(reader));
    fault::next_allocation = false;
    fault::keep_failing = false;
    if (replay) reader = {};
    const auto failed = session.snapshot();
    std::cout << "D23 " << name << " started=" << started << " allocation_faults=" << fault::hits - before
              << " status=" << status_name(failed->status) << " reader_released=" << subscription.expired() << '\n';
    expect(!started && fault::hits == before + 1 && reader_calls == 0,
           "D23 actual thread construction fails; failure cleanup makes no further allocations or Reader calls");
    expect(failed->status == Status::FAILED && !failed->error.empty() && failed->directory == directory.string(),
           "D23 failed thread construction must publish FAILED, not an active snapshot");
    expect(subscription.expired(), "D23 failed start must release retained Reader subscription");
    if (!replay) expect(!std::filesystem::exists(directory / "manifest.txt"), "D23 failed start cannot publish an archive");
    session.stop();
    detail::SessionTestAccess::before_worker_start(session, {});
    expect(session.start(root / (name + "-retry")), "D23 same Session must restart after failure");
    session.stop();
    expect(session.snapshot()->status == Status::STOPPED, "D23 retry completes normally");
    expect(session.load(recorded), "D23 same Session must replay after failure");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session.snapshot()->status == Status::REPLAYING && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(session.snapshot()->status == Status::STOPPED, "D23 healthy replay finishes");
    session.stop();
}
}

int main() {
    bool caught = false;
    fault::next_allocation = true;
    try { std::thread worker([] {}); worker.join(); }
    catch (const std::bad_alloc&) { caught = true; }
    fault::next_allocation = false;
    expect(caught && fault::hits == 1, "D23 raw actual std::thread allocation failure control");
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-input-start-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto recorded = root / "recorded";
    input_training::Session seed;
    expect(seed.start(recorded), "D23 prepares an actual empty archive for replay");
    seed.stop();
    expect(seed.snapshot()->status == input_training::Status::STOPPED, "D23 seed archive completes");
    for (const bool after : {false, true})
        for (const bool replay : {false, true}) run_case(root, recorded, replay, after);
    std::cout << "D23 fixture root: " << root << '\n';
    return failures == 0 ? 0 : 1;
}
