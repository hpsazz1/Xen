#include "aim/aim.h"
#include "log/log.h"
#include "recoil/recoil_worker.h"
#include "weapon/weapon_catalog.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void wait_for(F condition, const char* message) {
    const auto deadline = RecoilClock::now() + 2s;
    while (!condition() && RecoilClock::now() < deadline) std::this_thread::sleep_for(1ms);
    check(condition(), message);
}
// 仅内存假设备；Aim 与 Recoil 都经过同一个真实仲裁器和账本。
class FakeMouse final : public IMouseController {
public:
    std::atomic<bool> held{false};
    std::atomic<unsigned> calls{0};
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot& out) noexcept override {
        out = {}; out.state_valid = true; out.status = InputMonitorStatus::READY;
        out.virtual_keys[1] = held; out.sequence = ++sequence_; return true;
    }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++calls;
        MouseMoveReceipt receipt; receipt.succeeded = true;
        receipt.backend_completed_at = RecoilClock::now(); return receipt;
    }
private:
    std::atomic<std::uint64_t> sequence_{0};
};
void test_live_worker_handoff() {
    check(weapon::normalize_weapon_id("weapon_cz75a") == "cz75", "CZ75 GSI名称归一");
    auto profile = std::make_shared<RecoilProfile>();
    std::string error;
    check(load_recoil_profile(R"({"schema_version":3,"id":"synthetic_cz75","revision":1,
        "weapon_id":"cz75","sensitivity":1.4,"verified":true,"sample_semantics":"discrete_delta",
        "events":[[40,0,2],[100,0,2],[200,0,2],[300,0,2],[400,0,2]]})", *profile, error),
        "CZ75自制离散曲线通过生产Loader");
    check(profile->weapon_id == weapon::normalize_weapon_id("weapon_cz75a"), "GSI身份匹配Loader曲线");
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    auto ledger = std::make_shared<MotionLedger>();
    ledger->reset(14, 16, RecoilClock::now());
    RecoilWorker worker(mouse, arbiter, ledger, [profile] {
        RecoilInput input; input.permission = input.focused = input.profile_conditions_match = true;
        input.profile = profile; input.device_epoch = input.weapon_generation = input.weapon_trust_generation = 1;
        return input;
    }, [] { return TriggerFiringSignal{}; });
    RecoilConfig recoil; recoil.enabled = recoil.mixed_aim = true;
    check(worker.start(recoil), "假设备Worker启动");
    wait_for([&] { return worker.snapshot().phase == RecoilPhase::READY; }, "健康释放建立READY");
    AimConfig config; config.min_confirmed_hits = 1; config.control_delay_ms = 0;
    config.enable_prediction = config.enable_delay_compensation = false;
    config.max_counts_per_frame = 14; config.soft_zone_radius_percent = 30;
    config.soft_zone_min_strength = .2f; config.body_aim_height_ratio = .5f;
    config.counts_per_pixel_x = .425f; config.counts_per_pixel_y = .4f; config.smoothing = .475f;
    Aim aim(config);
    mouse->held = true;
    wait_for([&] { return arbiter->recoil_y_owned(); }, "生产Worker接管Y");
    unsigned aim_owned_sends = 0, owned_external_sends = 0, restored_y_sends = 0, seen_external = 0;
    std::uint64_t sequence = 0;
    const auto deadline = RecoilClock::now() + 900ms;
    while (RecoilClock::now() < deadline) {
        {
            auto guard = arbiter->enter_aim_until(RecoilClock::now() + 50ms);
            check(guard.owns_lock(), "Aim取得共享输出事务");
            AimFrame frame; frame.sequence = ++sequence; frame.observation_epoch = 1;
            frame.captured_at = frame.control_at = RecoilClock::now();
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160;
            frame.lock_active = true; frame.ease_first_activation = true;
            frame.detections = {{210,190,250,250,.95f,0}};
            frame.external_motion = ledger->snapshot(frame.control_at);
            frame.recoil_y_owned = arbiter->recoil_y_owned();
            seen_external += !frame.external_motion.events.empty();
            const auto result = aim.process(frame);
            check(result.status == AimStatus::SUCCESS && result.has_target, "生产Aim持续跟踪");
            if (frame.recoil_y_owned) check(result.command.dy_counts == 0, "Worker持有Y期间Aim不发送Y");
            if (result.has_command) {
                const MouseMoveCommand command{result.command.dx_counts, result.command.dy_counts};
                check(frame.external_motion.revision == ledger->revision() &&
                      frame.recoil_y_owned == arbiter->recoil_y_owned(), "锁内账本与所有权快照稳定");
                check(ledger->permits(command, RecoilClock::now()), "Aim实际命令满足生产输出合同");
                const auto receipt = mouse->move(command);
                check(ledger->record(command, receipt, false), "Aim回执进入共享账本");
                check(aim.record_backend_completed_command(result.command.sequence, receipt.backend_completed_at,
                      command.dx_counts, command.dy_counts), "Aim真实假后端回执绑定当前命令");
                aim_owned_sends += frame.recoil_y_owned && command.dx_counts != 0;
                owned_external_sends += frame.recoil_y_owned && frame.external_motion.revision > 0 && command.dx_counts != 0;
                restored_y_sends += !frame.recoil_y_owned && command.dy_counts != 0;
            }
        }
        std::this_thread::sleep_for(4ms);
    }
    check(worker.snapshot().phase == RecoilPhase::EXHAUSTED && !arbiter->recoil_y_owned(), "完整弹序耗尽归还Y");
    const auto execution = worker.execution_log();
    check(execution.records.size() == 5 && execution.dropped_count == 0, "CZ75自制五步恰好执行一次");
    for (const auto& record : execution.records)
        check(record.backend_called && record.receipt.status == RecoilReceiptStatus::ACKNOWLEDGED &&
              record.intent.dx_counts == 0 && record.intent.dy_counts == 2, "每步通过实际假后端且不合并或改量");
    check(ledger->revision() == 5 && seen_external > 0, "非空Worker外部运动账本传入生产Aim");
    check(owned_external_sends > 0, "首个Recoil ACK之后且Y尚未归还期间Aim X确实提交");
    check(aim_owned_sends > 0 && restored_y_sends > 0, "实际Aim X与压枪并行且耗尽后恢复Y输出");
    check(mouse->calls > 5, "同一个假后端确有Aim和Recoil两类输出");
    worker.stop();
}
}
int main() {
    LogConfig config; config.enable_console = config.enable_file = config.enable_ringbuf = false;
    Log::init(config);
    try { test_live_worker_handoff(); Log::shutdown(); std::cout << "aim_recoil_integration_tests passed\n"; }
    catch (const std::exception& error) { Log::shutdown(); std::cerr << error.what() << '\n'; return 1; }
}
