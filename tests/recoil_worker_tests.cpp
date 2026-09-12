#include "recoil/recoil_worker.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> bool until(F condition) {
    const auto end = RecoilClock::now() + 500ms;
    while (RecoilClock::now() < end) { if (condition()) return true; std::this_thread::sleep_for(1ms); }
    return condition();
}
class Mouse final : public IMouseController {
public:
    std::atomic<bool> held{false}, unknown{false}, healthy{true}, block{false}, proceed{false}, entered{false},dirty{false},missing_time{false};
    std::function<void()> on_poll;
    std::atomic<int> moves{0};
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves;entered=true;while(block&&!proceed)std::this_thread::sleep_for(1ms);
        MouseMoveReceipt result; result.succeeded = !unknown; result.backend_completed_at = missing_time?RecoilTime{}:RecoilClock::now(); return result;
    }
    bool poll_input(InputSnapshot& out) noexcept override {
        if(on_poll)on_poll();
        out={};out.state_valid = healthy; out.status = healthy?InputMonitorStatus::READY:InputMonitorStatus::STALE;
        out.virtual_keys[1] = held; out.sequence = ++sequence; return true;
    }
    void close() noexcept override {}
    bool left_button_cleanup_required() const noexcept override {return dirty;}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
private:
    std::atomic<std::uint64_t> sequence{0};
};
struct Fixture {
    std::shared_ptr<Mouse> mouse = std::make_shared<Mouse>();
    std::shared_ptr<AutoStopOutputArbiter> arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::shared_ptr<MotionLedger> ledger = std::make_shared<MotionLedger>();
    std::shared_ptr<RecoilProfile> profile = std::make_shared<RecoilProfile>();
    std::atomic<bool> focused{true};
    std::atomic<std::uint64_t> generation{1};
    std::atomic<bool> synthetic_down{false};
    std::atomic<std::uint64_t> synthetic_id{1};
    std::atomic<RecoilTime> synthetic_start{RecoilTime{}};
    std::atomic<std::int64_t> synthetic_uncertainty_ns{-1};
    std::atomic<bool> replace_signal_after_read{false};
    std::unique_ptr<RecoilWorker> worker;
    Fixture(bool rapid=false, bool change_generation=true) {
        profile->id = "synthetic"; profile->weapon_id = "synthetic_weapon";
        profile->state = RecoilProfileState::CALIBRATED; profile->phase_tolerance_ms = 100; profile->recovery_ms = 20;
        profile->source.sha256 = std::string(64, 'a'); profile->source.source_unit = "synthetic";
        profile->calibration = {"synthetic", "fake", "synthetic", "synthetic:test_only", 1.0};
        profile->points = rapid?std::vector<RecoilPoint>{{0,0,0},{1,0,10}}:std::vector<RecoilPoint>{{0,0,0},{400,0,80}};
        ledger->reset(14, 16, RecoilClock::now());
        worker = std::make_unique<RecoilWorker>(mouse, arbiter, ledger, [this] {
            RecoilInput input; input.enabled = input.permission = input.profile_conditions_match = true;
            input.focused = focused; input.profile = profile; input.device_epoch = 1; input.weapon_generation = generation;
            return input;
        }, [this] {
            TriggerFiringSignal signal{synthetic_down.load(),synthetic_id.load(),synthetic_start.load()};
            const auto uncertainty=synthetic_uncertainty_ns.load();
            if(uncertainty>=0) {
                signal.backend_completed_at=signal.started_at;
                signal.call_started_at=signal.started_at-std::chrono::nanoseconds(uncertainty);
                signal.uncertainty=std::chrono::nanoseconds(uncertainty);
            }
            if(signal.confirmed_down&&replace_signal_after_read.exchange(false)) {
                ++synthetic_id;synthetic_uncertainty_ns=4000;
            }
            return signal;
        });
        if(rapid&&change_generation)mouse->on_poll=[this]{if(worker->snapshot().phase==RecoilPhase::FIRING&&generation==1)++generation;};
        RecoilConfig config; config.enabled = true;
        check(worker->start(config), "worker启动");
    }
    void ready() { check(until([&]{return worker->snapshot().phase == RecoilPhase::READY;}), "等待新按下资格"); }
};
}
int main() {
    try {
        {
            Fixture f; f.ready(); f.mouse->held = true;
            check(until([&]{return f.mouse->moves.load() > 1;}), "无检测帧也可按独立轨迹推进");
            f.focused = false;
            check(until([&]{return f.worker->snapshot().reason == RecoilReason::CONTEXT;}), "失焦取消");
            const auto count = f.mouse->moves.load(); f.focused = true; std::this_thread::sleep_for(30ms);
            check(f.mouse->moves == count, "按住失焦恢复不能重放");
            f.worker->stop(); check(f.ledger->snapshot(RecoilClock::now()).events.size() == count, "每个确认恰好一次入账");
            const auto log=f.worker->execution_log();check(log.records.size()==static_cast<std::size_t>(count)&&log.dropped_count==0,"执行证据与确认次数一致");
            for(const auto& record:log.records)check(record.profile==f.profile&&record.backend_called&&record.receipt.status==RecoilReceiptStatus::ACKNOWLEDGED&&
                record.firing_started_at!=RecoilTime{}&&record.firing_started_at<=record.intent.planned_at&&
                record.firing_source==RecoilFiringSource::INPUT_ESTIMATED,"日志保留确切profile、起点与输入估计来源");
            for(const auto& record:log.records)check(record.dispatch_rejection==RecoilDispatchRejection::NONE&&
                record.sampled_at!=RecoilTime{}&&record.sampled_at<=record.intent.planned_at&&
                record.intent.planned_at<=record.arbitration_at&&record.arbitration_at<=record.context_checked_at&&
                record.context_checked_at<=record.backend_called_at&&record.backend_called_at<=record.receipt.completed_at&&
                record.receipt.completed_at<=record.backend_returned_at,"派发时点与真实回执有序且成功不伪造拒绝");
            for(const auto& record:log.records)check(record.source_firing_id==0&&!record.firing_uncertainty_ns,
                "实体输入没有软件命令id和软件调用区间");
        }
        {
            Fixture f; f.ready(); f.mouse->unknown = true; f.mouse->held = true;
            check(until([&]{return f.worker->snapshot().faulted;}), "未知回执故障");
            check(f.mouse->moves == 1 && !f.ledger->snapshot(RecoilClock::now()).complete, "未知位移不能重试");
            auto log=f.worker->execution_log();check(log.records.size()==1&&log.records[0].receipt.status==RecoilReceiptStatus::UNKNOWN,"未知回执独立保留");
        }
        {
            Fixture f;f.ready();f.ledger->reset(0,16,RecoilClock::now());f.mouse->held=true;
            check(until([&]{return !f.worker->execution_log().records.empty();}),"拒绝预算产生NOT_SENT证据");
            auto log=f.worker->execution_log();check(f.mouse->moves==0&&!log.records[0].backend_called&&
                log.records[0].receipt.status==RecoilReceiptStatus::NOT_SENT,"NOT_SENT不伪造提交或成功");
            check(log.records[0].dispatch_rejection==RecoilDispatchRejection::BUDGET_EXCEEDED&&
                log.records[0].backend_called_at==RecoilTime{}&&log.records[0].backend_returned_at==RecoilTime{},
                "额度拒绝独立记录且不伪造调用时点");
        }
        {
            Fixture f;f.ready();auto owner=f.arbiter->try_enter_aim();f.mouse->held=true;
            const bool recorded=until([&]{return !f.worker->execution_log().records.empty();});owner.unlock();
            check(recorded,"争用应产生独立拒绝记录");
            const auto record=f.worker->execution_log().records.front();
            check(record.dispatch_rejection==RecoilDispatchRejection::ARBITER_LOCK_BUSY&&
                record.receipt.status==RecoilReceiptStatus::NOT_SENT&&record.context_checked_at==RecoilTime{}&&
                record.backend_called_at==RecoilTime{}&&f.mouse->moves==0,"争用不调用后端且不会伪造复核时点");
            std::this_thread::sleep_for(20ms);
            check(f.mouse->moves==0&&f.arbiter->aim_skips()==0&&f.arbiter->snapshot().sources[2].lock_busy==1,
                "争用仍取消当前曲线，不追发且不污染Aim统计");
        }
        {
            Fixture f(true);f.ready();f.mouse->held=true;
            check(until([&]{return !f.worker->execution_log().records.empty();}),"提交前新代际形成拒绝证据");
            check(f.mouse->moves==0&&f.worker->execution_log().records.front().receipt.status==RecoilReceiptStatus::NOT_SENT,"二次复核拒绝旧上下文");
            check(f.worker->execution_log().records.front().dispatch_rejection==RecoilDispatchRejection::CONTEXT_CHANGED,
                "代际变更拒绝与额度、仲裁拒绝区分");
        }
        {
            Fixture f;f.ready();f.mouse->missing_time=true;f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().faulted;}),"成功但缺完成时刻视为未知");
            auto record=f.worker->execution_log().records.front();
            check(record.receipt.status==RecoilReceiptStatus::UNKNOWN&&record.receipt.completed_at==RecoilTime{},"不以now伪造完成证据");
        }
        {
            Fixture f;f.ready();f.mouse->dirty=true;f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().reason==RecoilReason::CONTEXT;}),"未对应Trigger的按钮债务阻止运行");
            check(f.mouse->moves==0,"known-down不能冒充干净状态");
        }
        {
            Fixture f(true,false);f.ready();f.synthetic_start=RecoilClock::now()-2ms;
            f.synthetic_uncertainty_ns=2000;f.replace_signal_after_read=true;f.synthetic_down=true;
            check(until([&]{return !f.worker->execution_log().records.empty();}),"二次复核换信号仍保存旧意图关联");
            const auto record=f.worker->execution_log().records.front();
            check(record.source_firing_id==1&&record.firing_uncertainty_ns==2000&&
                record.receipt.status==RecoilReceiptStatus::NOT_SENT&&f.mouse->moves==0,
                "旧意图不能被第二次输入的新命令id或区间覆盖");
        }
        {
            Fixture f;f.ready();f.synthetic_start=RecoilClock::now();f.synthetic_down=true;
            check(until([&]{return f.mouse->moves>0;}),"Trigger已确认down启动补偿");
            check(until([&]{return !f.worker->execution_log().records.empty();}),"确认软件命令证据完成");
            check(f.worker->execution_log().records.front().source_firing_id==1&&
                !f.worker->execution_log().records.front().firing_uncertainty_ns,
                "旧三字段信号保留命令id但不能伪造零不确定区间");
            f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().reason==RecoilReason::CONTEXT;}),"实体与软件重叠需取消");
            auto count=f.mouse->moves.load();f.synthetic_down=false;std::this_thread::sleep_for(25ms);
            check(f.mouse->moves==count,"直接切至实体不能续压");
            f.mouse->held=false;f.mouse->healthy=false;std::this_thread::sleep_for(10ms);
            f.mouse->held=true;f.mouse->healthy=true;std::this_thread::sleep_for(25ms);
            check(f.mouse->moves==count,"不可信释放不能解除来源冲突");
            f.mouse->held=false;check(until([&]{return f.worker->snapshot().phase==RecoilPhase::READY;}),"必须完整健康释放");
        }
        {
            Fixture f;f.ready();f.mouse->block=true;f.mouse->held=true;
            check(until([&]{return f.mouse->entered.load();}),"取消前move在途");
            f.worker->cancel();f.mouse->proceed=true;
            check(until([&]{return f.worker->snapshot().reason==RecoilReason::WAIT_RELEASE;}),"迟到ACK后取消不复活");
            check(f.mouse->moves==1&&f.worker->execution_log().records.size()==1,"在途完成仍有执行证据且不追发");
        }
        {
            Fixture f; f.ready(); f.mouse->held = true;
            check(until([&]{return f.mouse->moves.load() > 0;}), "换枪前先执行");
            ++f.generation;
            check(until([&]{return f.worker->snapshot().reason == RecoilReason::WAIT_RELEASE;}), "换枪需新边沿");
            const auto count = f.mouse->moves.load(); std::this_thread::sleep_for(25ms);
            check(count == f.mouse->moves, "换枪按住不重新起压");
        }
        std::cout << "recoil_worker_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
