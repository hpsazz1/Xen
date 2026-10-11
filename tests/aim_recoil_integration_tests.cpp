#include "aim/aim.h"
#include "log/log.h"
#include "recoil/recoil_worker.h"
#include "weapon/weapon_catalog.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

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
    std::atomic<bool> block_move{false}, entered{false}, release_move{false};
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
        entered = true;
        while (block_move && !release_move) std::this_thread::sleep_for(1ms);
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
        "weapon_id":"cz75","sensitivity":1.4,"verified":false,"sample_semantics":"discrete_delta",
        "events":[[40,0,2],[100,0,2],[200,0,2],[300,0,2],[400,0,2]]})", *profile, error),
        "CZ75自制离散曲线通过生产Loader");
    check(confirm_recoil_profile(*profile,error),"CZ75自制离散夹具显式人工确认");
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

void test_late_worker_issue_time(std::chrono::milliseconds backend_hold) {
    auto profile = std::make_shared<RecoilProfile>();
    std::string error;
    check(load_recoil_profile(R"({"schema_version":3,"id":"late_synthetic","revision":1,
        "weapon_id":"ak47","sensitivity":1.4,"verified":false,"sample_semantics":"discrete_delta",
        "events":[[16,6,0]]})", *profile, error), "迟到曲线经真实Loader验证");
    check(confirm_recoil_profile(*profile,error),"迟到执行夹具显式人工确认");
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    auto ledger = std::make_shared<MotionLedger>();
    ledger->reset(14, 16, RecoilClock::now() - 1s);
    RecoilWorker worker(mouse, arbiter, ledger, [profile] {
        RecoilInput input; input.permission = input.focused = input.profile_conditions_match = true;
        input.profile = profile; input.device_epoch = input.weapon_generation = input.weapon_trust_generation = 1;
        return input;
    }, [] { return TriggerFiringSignal{}; });
    // 异常路径先解开内存假后端，避免Worker析构被夹具自身卡住。
    struct Release { std::shared_ptr<FakeMouse> mouse; ~Release() { mouse->release_move = true; } } release{mouse};
    RecoilConfig recoil; recoil.enabled = recoil.mixed_aim = true;
    check(worker.start(recoil), "迟到假设备Worker启动");
    wait_for([&] { return worker.snapshot().phase == RecoilPhase::READY; }, "迟到场景建立健康释放");
    auto owner = arbiter->enter_aim_until(RecoilClock::now() + 1s);
    check(owner.owns_lock(), "占有真实仲裁器，延后合法离散事件的实际提交");
    mouse->block_move = mouse->held = true;
    wait_for([&] { return arbiter->snapshot().sources[2].lock_busy > 0; }, "Worker确实遇到仲裁阻塞");
    const auto not_before = RecoilClock::now() + 150ms;
    wait_for([&] { return RecoilClock::now() >= not_before; }, "产生大于配置延迟的真实事件迟到");
    const auto before = ledger->snapshot(RecoilClock::now());
    check(mouse->calls == 0 && before.complete && before.events.empty(), "发送前计划事件不能冒充已执行账本");
    owner.unlock();
    wait_for([&] { return mouse->entered.load(); }, "真实Worker进入假后端");
    const auto in_flight = ledger->snapshot(RecoilClock::now());
    check(in_flight.complete && in_flight.events.empty(), "后端未完成时不能发布外部执行事实");
    const auto complete_after = RecoilClock::now() + backend_hold;
    wait_for([&] { return RecoilClock::now() >= complete_after; }, "实际后端延迟达到本场景下限");
    mouse->release_move = true;
    wait_for([&] { return worker.execution_log().records.size() == 1; }, "真实Worker记录执行和ACK");
    worker.stop();
    const auto record = worker.execution_log().records.front();
    const auto control = record.backend_called_at;
    const auto completed = record.receipt.completed_at;
    const auto backend_ms = std::chrono::duration<double,std::milli>(completed-control).count();
    const int delay_ms = static_cast<int>(std::ceil(backend_ms)) + 15;
    const auto effect = control + std::chrono::milliseconds(delay_ms);
    std::cout << "late actual K=" << backend_ms << " D=" << delay_ms << " C-P="
              << std::chrono::duration<double,std::milli>(control-record.intent.planned_at).count() << '\n';
    const auto after = ledger->snapshot(std::max(RecoilClock::now(), effect + 1ms));
    check(record.backend_called && record.receipt.status == RecoilReceiptStatus::ACKNOWLEDGED &&
          record.intent.dx_counts == 6 && record.intent.dy_counts == 0 &&
          after.complete && after.events.size() == 1 && mouse->calls == 1,
          "真实Worker恰好执行一次并发布健康账本，不通过丢失事件获得零库存");
    check(record.intent.planned_at == record.firing_started_at + 16ms && control - record.intent.planned_at >= 60ms,
          "曲线计划P必须保留16ms绝对偏移，同时实际提交C确实迟到");
    check(completed >= control + backend_hold && delay_ms <= 100 && control-record.intent.planned_at > 100ms,
          "使用实测后端区间校准D=ceil(K)+15，且迟到大于允许的D上界；调度超预算不可当通过");

    AimConfig config; config.min_confirmed_hits = 1; config.control_delay_ms = static_cast<float>(delay_ms);
    config.max_delay_compensation_ms = 100; config.enable_prediction = false;
    config.enable_delay_compensation = true; config.acquisition_range_percent = 100;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .5f;
    config.soft_zone_radius_percent = 0;
    Aim aim(config);
    AimFrame previous;
    std::uint64_t identity = 0;
    float previous_camera = 0;
    unsigned mismatches = 0, pending_samples = 0, effected_samples = 0;
    const auto origin = before.covered_until - 160ms;
    auto frame_at = [&](RecoilTime observed, RecoilTime aim_control, const ExternalMotionWindow& window, bool verify) {
        const float camera = observed >= effect ? 12.0f : 0.0f; // 独立plant：6count / 0.5count每source像素。
        const float world = 180 + 100 * std::chrono::duration<float>(observed - origin).count();
        const float center = world - camera;
        AimFrame frame; frame.sequence = previous.sequence + 1; frame.observation_epoch = 1;
        frame.captured_at = observed; frame.control_at = aim_control;
        frame.roi_width = frame.roi_height = 320; frame.control_center_x = frame.control_center_y = 160;
        frame.lock_active = true; frame.external_motion = window;
        frame.detections = {{center-40,141.1f,center+40,195.1f,.95f,0}};
        if (previous.sequence) frame.background_motion_x = {AimBackgroundMotionStatus::VALID,
            previous.sequence,frame.sequence,previous.captured_at,observed,1,-(camera-previous_camera),1,0,2};
        const auto result = aim.process(frame);
        if (!identity && result.has_target) identity = result.target.track_id;
        check(result.status == AimStatus::SUCCESS && result.has_target && identity != 0 &&
              result.target.track_id == identity && result.target.state == TrackState::CONFIRMED &&
              result.target.matched_observation_valid && !result.target.predicted, "迟到链逐帧保持同一实测目标");
        if (previous.sequence) check(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                                     "迟到链逐帧消费真实同源背景");
        if (!verify && previous.sequence > 20)
            check(result.control.residual_role_x && std::abs(result.control.execution_unseen_command_x_counts) < .001,
                  "发送前的真实账本在Aim中必须是零未见库存");
        if (verify) {
            check(result.control.residual_role_x, "迟到库存必须由真实Aim执行误差角色消费");
            const double expected = observed < effect ? 6 : 0;
            if (expected) ++pending_samples; else ++effected_samples;
            if (std::abs(result.control.execution_unseen_command_x_counts - expected) > .001) ++mismatches;
            std::cout << "late Kmin=" << backend_hold.count() << " O-C="
                      << std::chrono::duration<double,std::milli>(observed-control).count()
                      << " true_unseen=" << expected << " aim_unseen="
                      << result.control.execution_unseen_command_x_counts << '\n';
        }
        check(aim.record_backend_completed_command(frame.sequence, aim_control, 0, 0), "Aim自身以真实零回执隔离外部库存");
        previous = frame; previous_camera = camera;
    };
    for (int n = 0; n < 40; ++n) frame_at(origin + n*4ms, origin + n*4ms + 1ms, before, false);
    // 使用完成后的不可变真实账本做确定性源时间回放，覆盖C+D之前、恰好到达和之后。
    std::vector<int> offsets;
    for (int offset = 0; offset < delay_ms; offset += 4) offsets.push_back(offset);
    offsets.insert(offsets.end(), {delay_ms-1, delay_ms, delay_ms+1, delay_ms+4});
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    for (int offset : offsets) {
        const auto observed = control + std::chrono::milliseconds(offset);
        const auto aim_control = completed + 1ms + std::chrono::milliseconds(offset);
        frame_at(observed, aim_control, ledger->snapshot(aim_control), true);
    }
    check(pending_samples == offsets.size()-3 && pending_samples > 1 && effected_samples == 3,
          "必须检查非空预计效果前后与边界样本");
    check(mismatches == 0, "迟到但刚执行的Recoil命令必须保留到实际C+D，不能按曲线P提前出库");
    check(after.events.front().issued_at == control, "Worker外部账本使用实际提交C并保留独立计划P");
}
}
int main() {
    LogConfig config; config.enable_console = config.enable_file = config.enable_ringbuf = false;
    Log::init(config);
    try { test_live_worker_handoff(); test_late_worker_issue_time(5ms); test_late_worker_issue_time(15ms);
        Log::shutdown(); std::cout << "aim_recoil_integration_tests passed\n"; }
    catch (const std::exception& error) { Log::shutdown(); std::cerr << error.what() << '\n'; return 1; }
}
