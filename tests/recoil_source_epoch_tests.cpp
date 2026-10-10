#include "recoil/recoil_source_context_internal.h"
#include "recoil/recoil_worker.h"
#include "recoil/recoil_execution_events.h"
#include "recoil_tuner/wall_capture_run.h"
#include "source_context/source_context_internal.h"
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> bool until(F condition) {
    const auto deadline = RecoilClock::now() + 500ms;
    while (!condition() && RecoilClock::now() < deadline) std::this_thread::sleep_for(1ms);
    return condition();
}
class FocusEvidence {
public:
    FocusEvidence() { accept(source_context::detail::Focus::Foreground); }
    void cross(bool discontinuity) {
        if (discontinuity) accept(source_context::detail::Focus::Background);
        accept(source_context::detail::Focus::Foreground);
    }
    source_context::SourceContextSnapshot snapshot() {
        std::lock_guard lock(mutex_); return evidence_.snapshot(RecoilClock::now());
    }
private:
    void accept(source_context::detail::Focus focus) {
        std::lock_guard lock(mutex_);
        response_.response = true; response_.focus = focus; response_.session = 11;
        response_.process_name = "fixture.exe"; response_.sequence += 7; ++response_.nonce[0];
        const auto now = RecoilClock::now(); evidence_.begin(response_.nonce, now);
        require(evidence_.accept(response_, "fixture.exe", now, 1000), "真实Evidence必须接受合成来源事实");
    }
    std::mutex mutex_;
    source_context::detail::Evidence evidence_;
    source_context::detail::Message response_;
};
class MemoryDevice final : public IMouseController {
public:
    std::atomic<int> moves{0}, downs{0}, ups{0};
    std::atomic<bool> physical{false};
    std::function<void()> first_move;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.state_valid = true; input.status = InputMonitorStatus::READY;
        input.sequence = ++sequence_; input.virtual_keys[1] = physical; input.virtual_keys[18] = true; return true;
    }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        if (++moves == 1 && first_move) first_move();
        MouseMoveReceipt receipt; receipt.succeeded = true; receipt.backend_completed_at = RecoilClock::now(); return receipt;
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        if (down) ++downs; else ++ups;
        ButtonReceipt receipt; receipt.disposition = ButtonDisposition::ACKNOWLEDGED;
        receipt.backend_completed_at = receipt.protocol_ack_received_at = RecoilClock::now(); return receipt;
    }
private:
    std::atomic<std::uint64_t> sequence_{0};
};

void binding_cases() {
    FocusEvidence source;
    const auto original = source.snapshot();
    recoil_detail::DebugSourceContext guard;
    require(guard.permits(original, 0), "来源与GSI的初始代次0必须可绑定");
    source.cross(false);
    require(guard.permits(source.snapshot(), 0), "健康序号跳跃保持绑定");
    source.cross(true);
    require(!guard.permits(source.snapshot(), 0) && !guard.permits(original, 0), "来源恢复和较晚旧快照均不能恢复旧许可");
    recoil_detail::DebugSourceContext weapon;
    require(weapon.permits(original, 0) && !weapon.permits(original, 1) && !weapon.permits(original, 0),
        "GSI代次0不能被当成未绑定并允许替换");
    recoil_detail::DebugSourceContext concurrent;
    auto other = original; ++other.session_id; ++other.control_safety_epoch;
    std::barrier start(3); std::atomic<int> admitted{0};
    std::thread first([&] { start.arrive_and_wait(); if (concurrent.permits(original, 0)) ++admitted; });
    std::thread second([&] { start.arrive_and_wait(); if (concurrent.permits(other, 1)) ++admitted; });
    start.arrive_and_wait(); first.join(); second.join();
    require(admitted == 1 && !concurrent.permits(original, 0) && !concurrent.permits(other, 1),
        "并发首次绑定只能接纳一个完整组合，冲突后不得复活");
}

bool worker_case(bool app, bool discontinuity) {
    FocusEvidence source;
    require(source.snapshot().control_safety_epoch == 0, "初始epoch0必须作为有效输入");
    recoil_detail::CalibrationSourceContext cli_guard;
    recoil_detail::DebugSourceContext app_guard;
    auto device = std::make_shared<MemoryDevice>();
    auto profile = std::make_shared<RecoilProfile>();
    std::string error;
    require(load_recoil_profile(R"({"schema_version":3,"id":"source_epoch_fixture","revision":1,
        "weapon_id":"synthetic_weapon","sensitivity":1,"verified":false,"sample_semantics":"discrete_delta",
        "events":[[40,0,1],[80,0,1]]})", *profile, error), "两事件自制曲线通过生产Loader");
    RecoilCalibrationManifest manifest; manifest.session_id = app ? "app-source-fixture" : "cli-source-fixture";
    manifest.profile_semantic_sha256 = recoil_calibration_sha256(serialize_recoil_profile(*profile));
    manifest.profile_file_sha256 = manifest.profile_semantic_sha256; manifest.config_binding_sha256 = std::string(64, 'a');
    manifest.environment = {profile->weapon_id,"synthetic","kmbox_net","synthetic",1.0};
    manifest.environment_fingerprint = recoil_calibration_environment_fingerprint(manifest.environment);
    manifest.hold_virtual_key = 18; manifest.cancel_virtual_key = 27;
    manifest.limits = {1,1000,500,100,14,16,14,100};
    const auto permit = authorize_recoil_calibration(manifest, profile, RecoilClock::now(), error);
    require(permit != nullptr, "测试校准许可必须有效");
    auto ledger = std::make_shared<MotionLedger>(); ledger->reset(14,16,RecoilClock::now());
    std::atomic<bool> firing{false}; std::atomic<RecoilTime> started{};
    RecoilWorker worker(device, std::make_shared<AutoStopOutputArbiter>(), ledger, [&] {
        RecoilInput input; input.permission = input.profile_conditions_match = true;
        input.profile = profile; input.device_epoch = input.weapon_generation = 1;
        const auto focus = source.snapshot();
        // 与CLI/App实际调用的是同一来源适配器，未复制其判定逻辑。
        if (app) app_guard.apply(input, focus, app_guard.permits(focus, 1));
        else cli_guard.apply(input, focus, 1);
        return input;
    }, [&] {
        TriggerFiringSignal signal;
        if (firing) {
            signal.confirmed_down = true; signal.id = 1;
            signal.started_at = signal.backend_completed_at = started.load();
            signal.call_started_at = signal.started_at - 1us; signal.uncertainty = 1us;
        }
        return signal;
    });
    device->first_move = [&] { source.cross(discontinuity); };
    RecoilConfig config; config.enabled = true; config.hold_virtual_key = 18;
    config.game_build = "synthetic"; config.input_path = "kmbox_net"; config.conditions = "synthetic"; config.sensitivity = 1;
    require(worker.start_calibration(config, permit, app), "真实校准Worker启动");
    require(until([&] { return worker.snapshot().phase == RecoilPhase::READY; }), "真实Worker观察健康释放");
    if (app) { started = RecoilClock::now(); firing = true; } else device->physical = true;
    require(until([&] { return worker.calibration_snapshot().terminal; }), "两事件Worker必须有终态");
    const auto budget = worker.calibration_snapshot(); worker.stop();
    const auto events = worker.read_execution_events(0);
    require(!events.events.empty() && events.events.back().kind == RecoilExecutionEventKind::END,
        "真实Worker必须封尾批次");
    const int expected = discontinuity ? 1 : 2;
    std::cout << (app ? "app" : "cli") << " discontinuity=" << discontinuity << " moves=" << device->moves
        << " terminal=" << RecoilCalibrationEndName(budget.end) << '\n';
    return device->moves == expected && budget.end == (discontinuity ? RecoilCalibrationEnd::CONTEXT : RecoilCalibrationEnd::COMPLETED) &&
        events.events.back().end_reason == (discontinuity ? RecoilBatchEndReason::CONTEXT : RecoilBatchEndReason::EXHAUSTED);
}

bool wall_case(const std::filesystem::path& root, bool discontinuity) {
    FocusEvidence source; recoil_detail::DebugSourceContext guard;
    auto device = std::make_shared<MemoryDevice>();
    recoil_tuner::WallRunRequest request;
    request.mode = recoil_tuner::WallRunMode::TEST; request.measurement_required = false;
    request.weapon_id = "synthetic_weapon"; request.sensitivity = 1; request.duration_ms = 100;
    request.trigger_virtual_key = 5; request.cancel_virtual_key = 35;
    request.output_directory = root / (discontinuity ? "wall-recovered" : "wall-healthy-gap");
    request.context_valid = [&] { return guard.permits(source.snapshot(), 1); };
    bool ready = false, stopped = false;
    request.on_ready = [&] { ready = true; source.cross(discontinuity); return true; };
    request.on_firing_stopped = [&] { stopped = true; };
    int capture_calls = 0;
    request.capture_factory = [&](const CaptureConfig&) -> std::unique_ptr<ICapture> { ++capture_calls; return {}; };
    std::atomic<bool> canceled{false};
    const auto result = recoil_tuner::run_wall_capture(request, device, canceled);
    std::cout << "wall discontinuity=" << discontinuity << " downs=" << device->downs << " stopped=" << stopped << '\n';
    require(ready && stopped && capture_calls == 0 && !result.cleanup_unknown, "WallRun实际on_ready/收尾且零Capture");
    return discontinuity ? device->downs == 0 && !result.completed : device->downs == 1 && result.completed;
}
}
int main() {
    try {
        binding_cases();
        int failures = 0;
        for (bool app : {false, true}) for (bool discontinuity : {false, true})
            if (!worker_case(app, discontinuity)) ++failures;
        const auto root = std::filesystem::temp_directory_path() /
            ("xen-recoil-source-" + std::to_string(RecoilClock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root);
        for (bool discontinuity : {false, true}) if (!wall_case(root, discontinuity)) ++failures;
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
