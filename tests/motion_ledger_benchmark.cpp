#include "recoil/motion_ledger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t calls = 0;
thread_local std::size_t bytes = 0;
}

void* operator new(std::size_t bytes) {
    if (void* value = std::malloc(bytes == 0 ? 1 : bytes)) {
        if (allocation_probe::enabled) {
            ++allocation_probe::calls;
            allocation_probe::bytes += bytes;
        }
        return value;
    }
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {
using Clock = std::chrono::steady_clock;
volatile std::uint64_t sink = 0;

bool same_window(const ExternalMotionWindow& a, const ExternalMotionWindow& b) {
    if (a.enabled != b.enabled || a.complete != b.complete ||
        a.revision != b.revision || a.covered_from != b.covered_from ||
        a.covered_until != b.covered_until || a.events.size() != b.events.size()) return false;
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        const auto& x = a.events[i];
        const auto& y = b.events[i];
        if (x.id != y.id || x.completed_at != y.completed_at ||
            x.dx_counts != y.dx_counts || x.dy_counts != y.dy_counts) return false;
    }
    return true;
}

// 只比较稳定夹具上的拷贝策略，不能替代生产并发/生命周期等价证明。
struct CopyPrototype {
    ExternalMotionWindow metadata;
    std::deque<ExternalMotionEvent> events;
    std::mutex mutex;

    explicit CopyPrototype(const ExternalMotionWindow& fixture)
        : metadata{fixture.enabled, fixture.complete, fixture.revision,
                   fixture.covered_from, fixture.covered_until, {}},
          events(fixture.events.begin(), fixture.events.end()) {}

    ExternalMotionWindow reserved_snapshot() {
        ExternalMotionWindow result;
        result.events.reserve(4096); // allocator 移出内锁，仍在实际 Runtime outerguard 内。
        std::lock_guard lock(mutex);
        result.enabled = metadata.enabled;
        result.complete = metadata.complete;
        result.revision = metadata.revision;
        result.covered_from = metadata.covered_from;
        result.covered_until = metadata.covered_until;
        result.events.assign(events.begin(), events.end());
        return result;
    }
};

struct Stats {
    double mean, p50, p95, p99, maximum;
};
Stats stats(std::vector<double> values) {
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1];
    };
    return {mean, percentile(.50), percentile(.95), percentile(.99), values.back()};
}

void emit(const std::string& mode, std::size_t size, std::size_t iterations,
          std::size_t warmup, const std::vector<double>& times,
          std::size_t allocations, std::size_t allocated_bytes) {
    const auto s = stats(times);
    std::cout << mode << ',' << size << ',' << size * sizeof(ExternalMotionEvent)
              << ',' << iterations << ',' << warmup << ',' << allocations << ','
              << allocated_bytes << ',' << s.mean << ',' << s.p50 << ',' << s.p95
              << ',' << s.p99 << ',' << s.maximum << '\n';
}

template<class Factory>
void measure(const std::string& name, std::size_t size, std::size_t iterations,
             std::size_t warmup, Factory factory) {
    for (std::size_t i = 0; i < warmup; ++i) {
        auto window = factory();
        sink = window.revision;
    }
    std::vector<double> times;
    times.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        auto window = factory();
        const auto end = Clock::now();
        sink = window.events.empty() ? window.revision : window.events.back().id;
        times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    allocation_probe::calls = allocation_probe::bytes = 0;
    allocation_probe::enabled = true;
    auto allocation_sample = factory();
    allocation_probe::enabled = false;
    sink = allocation_sample.revision;
    emit(name, size, iterations, warmup, times,
         allocation_probe::calls, allocation_probe::bytes);
}

void run_case(std::size_t total_records, std::size_t iterations, std::size_t warmup) {
    MotionLedger ledger;
    const auto start = Clock::time_point{} + std::chrono::seconds(1);
    ledger.reset(100, 16, start);
    for (std::size_t i = 0; i < total_records; ++i) {
        MouseMoveReceipt receipt;
        receipt.succeeded = true;
        receipt.backend_completed_at = start + std::chrono::microseconds(i + 1);
        const MouseMoveCommand command{
            static_cast<int>(i % 7) - 3, static_cast<int>(i % 11) - 5};
        if (!ledger.record(command, receipt, true)) throw std::runtime_error("fixture record failed");
    }
    const auto now = start + std::chrono::seconds(1);
    const auto fixture = ledger.snapshot(now);
    CopyPrototype prototype(fixture);
    if (!same_window(fixture, prototype.reserved_snapshot())) {
        throw std::runtime_error("stable snapshot prototype differs from production");
    }
    const std::size_t size = fixture.events.size();
    measure(total_records > 4096 ? "production_retention" : "production_snapshot",
            size, iterations, warmup, [&] { return ledger.snapshot(now); });
    measure("prototype_reserve4096_outside_inner_lock", size, iterations, warmup,
            [&] { return prototype.reserved_snapshot(); });

    // 预分配目标的纯拷贝下界，明确排除分配、ledger mutex及真实 outerguard。
    std::vector<ExternalMotionEvent> scratch;
    scratch.reserve(4096);
    std::vector<double> copy_times;
    copy_times.reserve(iterations);
    for (std::size_t i = 0; i < iterations + warmup; ++i) {
        const auto begin = Clock::now();
        scratch.assign(prototype.events.begin(), prototype.events.end());
        const auto end = Clock::now();
        sink = scratch.empty() ? 0 : scratch.back().id;
        if (i >= warmup) copy_times.push_back(
            std::chrono::duration<double, std::micro>(end - begin).count());
    }
    emit("copy_only_preallocated_lower_bound", size, iterations, warmup, copy_times, 0, 0);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t iterations = argc > 1 ? std::stoull(argv[1]) : 20000;
        const std::size_t warmup = argc > 2 ? std::stoull(argv[2]) : 1000;
        if (iterations == 0 || iterations > 1000000 || warmup > 1000000) return 2;
        std::cout << "# Synthetic CPU-only; per-call us; excludes returned-vector destruction; "
                     "production_snapshot is uncontended total API time, not measured lock wait.\n";
#ifdef _MSC_VER
        std::cout << "# MSVC=" << _MSC_VER << "; event_bytes=" << sizeof(ExternalMotionEvent) << '\n';
#endif
        std::cout << "mode,events,payload_bytes,iterations,warmup,allocations_per_snapshot,allocated_bytes_per_snapshot,mean_us,p50_us,p95_us,p99_us,max_us\n";
        std::cout << std::fixed << std::setprecision(4);
        for (const std::size_t size : {0U, 64U, 256U, 1024U, 4096U, 4097U}) {
            run_case(size, iterations, warmup);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}