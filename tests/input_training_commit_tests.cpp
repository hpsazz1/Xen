#include "input_training/input_training_internal.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void run_case(const std::filesystem::path& root, int mode) {
    using namespace input_training;
    Session session;
    Limits limits;
    limits.max_run_events = 1;
    limits.stop_timeout_ms = 20;
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto gate = release.get_future().share();
    auto pause = [&] {
        entered.set_value(); gate.wait();
    };
    if (mode >= 2) detail::SessionTestAccess::after_manifest_commit_admitted(session, pause);
    else detail::SessionTestAccess::before_manifest_commit(session, pause);
    auto batch = std::make_shared<ReadBatch>();
    for (std::uint64_t index = 1; index <= 2; ++index) {
        Event event;
        event.epoch = 1; event.sequence = index;
        event.received_at_ns = static_cast<std::int64_t>(index);
        event.state_valid = true;
        batch->events.push_back(event);
    }
    const auto directory = root / ("case-" + std::to_string(mode));
    expect(session.start(directory, limits, [batch] { ReadBatch result; result.events.swap(batch->events); return result; }),
           "D22 actual Session starts with synthetic reader");
    if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        release.set_value(); session.stop(); expect(false, "D22 worker reaches actual manifest boundary"); return;
    }
    if (mode != 0) {
        const auto started = std::chrono::steady_clock::now();
        session.stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(elapsed < std::chrono::milliseconds(500), "D22 blocked disk worker cannot block bounded stop");
        const auto expected = mode == 1 ? Status::STOP_TIMEOUT : Status::FINALIZING;
        expect(session.snapshot()->status == expected, "D22 stop distinguishes canceled and already admitted commit");
        expect(stop_is_unconfirmed(session.snapshot()->status), "D22 unfinished stop blocks next device work");
        expect(!session.start(root / "must-not-start"), "D22 unsettled worker cannot be replaced");
        ArchiveSummary interim;
        std::string interim_error;
        expect(!visit_archive(directory, [](const Event&) {}, interim, interim_error),
               "D22 pending commit cannot be accepted as a finished archive");
    }
    if (mode == 3) std::filesystem::create_directory(directory / "manifest.txt");
    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!detail::SessionTestAccess::finished(session) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(detail::SessionTestAccess::finished(session), "D22 released worker finishes");
    session.stop();
    ArchiveSummary summary;
    std::string error;
    const bool accepted = visit_archive(directory, [](const Event&) {}, summary, error);
    std::cout << "D22 mode=" << mode << " final_status=" << status_name(session.snapshot()->status)
              << " archive_accepted=" << accepted << '\n';
    if (mode == 1) {
        expect(!accepted && !std::filesystem::exists(directory / "manifest.txt"),
               "D22 timed-out writer must not subsequently publish an acceptable manifest");
    } else if (mode == 3) {
        expect(!accepted && session.snapshot()->status == Status::FAILED &&
               std::filesystem::is_directory(directory / "manifest.txt"),
               "D22 admitted commit failure reports FAILED and preserves conflicting target");
    } else {
        expect(accepted && summary.status == Status::LIMIT && summary.events == 1,
               "D22 healthy limit archive retains exact status and event count");
    }
}
void run_stopped_case(const std::filesystem::path& root, bool admitted, bool release_early) {
    using namespace input_training;
    Session session;
    Limits limits;
    limits.stop_timeout_ms = 100;
    std::promise<void> entered, release, stopped;
    auto ready = entered.get_future();
    auto gate = release.get_future().share();
    auto stop_ready = stopped.get_future();
    auto pause = [&] { entered.set_value(); gate.wait(); };
    if (admitted) detail::SessionTestAccess::after_manifest_commit_admitted(session, pause);
    else detail::SessionTestAccess::before_manifest_commit(session, pause);
    const auto directory = root / (std::string("stopped-") + (admitted ? "admitted" : release_early ? "healthy" : "timeout"));
    expect(session.start(directory, limits), "D22 ordinary STOPPED archive starts without Reader");
    // start/stop依次执行；只有此owner线程调用stop，主线程仅观测并释放磁盘边界。
    std::thread owner([&] { session.stop(); stopped.set_value(); });
    if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        release.set_value(); owner.join(); expect(false, "D22 ordinary stop reaches final commit"); return;
    }
    if (release_early) release.set_value();
    const bool bounded = stop_ready.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    expect(bounded, "D22 ordinary stop returns while admitted or canceled disk work is blocked");
    if (!release_early) {
        expect(session.snapshot()->status == (admitted ? Status::FINALIZING : Status::STOP_TIMEOUT),
               "D22 ordinary stop reports the actual commit winner");
        release.set_value();
    }
    owner.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!detail::SessionTestAccess::finished(session) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(detail::SessionTestAccess::finished(session), "D22 ordinary stop background work settles");
    session.stop();
    ArchiveSummary summary;
    std::string error;
    const bool accepted = visit_archive(directory, [](const Event&) {}, summary, error);
    const bool should_complete = admitted || release_early;
    expect(should_complete ? accepted && summary.status == Status::STOPPED && summary.events == 0 :
           !accepted && !std::filesystem::exists(directory / "manifest.txt"),
           "D22 STOPPED completion obeys timeout/commit arbitration without inventing events");
    std::cout << "D22 stopped admitted=" << admitted << " early=" << release_early
              << " archive_accepted=" << accepted << '\n';
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-input-commit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (int mode = 0; mode < 4; ++mode) run_case(root, mode);
    run_stopped_case(root, false, true);
    run_stopped_case(root, false, false);
    run_stopped_case(root, true, false);
    std::cout << "D22 fixture root: " << root << '\n';
    return failures == 0 ? 0 : 1;
}
