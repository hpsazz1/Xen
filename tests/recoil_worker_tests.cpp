#include "recoil/recoil_worker.h"
#include "recoil/recoil_execution_events.h"
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
    std::atomic<bool> cancel_key{false};
    std::atomic<bool> calibration_key{true};
    std::atomic<bool> trigger_key{false}, block_up{false}, up_entered{false}, release_up{false};
    std::atomic<int> downs{0}, ups{0};
    std::atomic<bool> pre_call_receipt{false};
    std::atomic<RecoilTime> last_poll_at{RecoilTime{}};
    std::function<void()> on_poll;
    std::atomic<int> moves{0};
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves;entered=true;while(block&&!proceed)std::this_thread::sleep_for(1ms);
        MouseMoveReceipt result; result.succeeded = !unknown; result.backend_completed_at = missing_time?RecoilTime{}:
            pre_call_receipt?last_poll_at.load():RecoilClock::now(); return result;
    }
    bool poll_input(InputSnapshot& out) noexcept override {
        if(on_poll)on_poll();
        last_poll_at=RecoilClock::now();
        out={};out.state_valid = healthy; out.status = healthy?InputMonitorStatus::READY:InputMonitorStatus::STALE;
        out.virtual_keys[1] = held;out.virtual_keys[5]=trigger_key;out.virtual_keys[18]=calibration_key;out.virtual_keys[27]=cancel_key; out.sequence = ++sequence; return true;
    }
    void close() noexcept override {}
    bool left_button_cleanup_required() const noexcept override {return dirty;}
    bool supports_left_button() const noexcept override {return true;}
    ButtonReceipt set_left_button(bool down) noexcept override {
        if(down){++downs;dirty=true;}
        else {++ups;up_entered=true;while(block_up&&!release_up)std::this_thread::sleep_for(1ms);dirty=false;}
        ButtonReceipt receipt;receipt.disposition=ButtonDisposition::ACKNOWLEDGED;
        receipt.backend_completed_at=receipt.protocol_ack_received_at=RecoilClock::now();
        receipt.cleanup_required=dirty;receipt.datagram_sent=true;return receipt;
    }
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
    std::atomic<bool> throw_context{false};
    std::atomic<std::int64_t> synthetic_uncertainty_ns{-1};
    std::atomic<bool> replace_signal_after_read{false};
    std::atomic<std::shared_ptr<TriggerWorker>> live_trigger;
    std::unique_ptr<RecoilWorker> worker;
    Fixture(bool rapid=false, bool change_generation=true, bool zero_curve=false, bool calibration=false,
            int legacy_permission_key=0, double recovery_ms=20, bool automated_debug_firing=false, bool user_confirmed=false) {
        profile->id = "synthetic"; profile->weapon_id = "synthetic_weapon";
        profile->state = RecoilProfileState::CALIBRATED; profile->phase_tolerance_ms = 100; profile->recovery_ms = recovery_ms;
        profile->source.sha256 = std::string(64, 'a'); profile->source.source_unit = "synthetic";
        profile->calibration = {"synthetic", "fake", "synthetic", "synthetic:test_only", 1.0};
        profile->points = rapid?std::vector<RecoilPoint>{{0,0,0},{1,0,10}}:std::vector<RecoilPoint>{{0,0,0},{400,0,80}};
        if(zero_curve)profile->points={{0,0,0},{10,0,0}};
        if(calibration) {
            profile->state=RecoilProfileState::SCHEMA_VALID;profile->calibration={};
            profile->phase_tolerance_ms.reset();profile->recovery_ms.reset();
        }
        if(user_confirmed) {
            std::string error;check(confirm_recoil_profile(*profile,error),"人工确认测试曲线有效");
        }
        ledger->reset(14, 16, RecoilClock::now());
        worker = std::make_unique<RecoilWorker>(mouse, arbiter, ledger, [this] {
            if(throw_context)throw std::runtime_error("synthetic context failure");
            RecoilInput input; input.enabled = input.permission = input.profile_conditions_match = true;
            input.focused = focused; input.profile = profile; input.device_epoch = 1; input.weapon_generation = generation;
            return input;
        }, [this] {
            if(auto trigger=live_trigger.load())return trigger->firing_signal();
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
        RecoilConfig config; config.enabled = true; config.hold_virtual_key = legacy_permission_key;
        if(calibration) {
            RecoilCalibrationManifest manifest;manifest.session_id="synthetic-calibration";
            manifest.profile_semantic_sha256=recoil_calibration_sha256(serialize_recoil_profile(*profile));
            manifest.profile_file_sha256=manifest.profile_semantic_sha256;manifest.config_binding_sha256=std::string(64,'a');
            manifest.environment={profile->weapon_id,"synthetic","kmbox_net","synthetic",1.0};
            manifest.environment_fingerprint=recoil_calibration_environment_fingerprint(manifest.environment);
            manifest.hold_virtual_key=18;manifest.cancel_virtual_key=27;
            manifest.limits={1,1000,500,100,14,16,14,100};
            std::string error;auto permit=authorize_recoil_calibration(manifest,profile,RecoilClock::now(),error);
            check(permit!=nullptr,"fake校准permit有效");
            config.hold_virtual_key=18;config.game_build="synthetic";config.input_path="kmbox_net";
            config.conditions="synthetic";config.sensitivity=1.0;
            check(worker->start_calibration(config,permit,automated_debug_firing),"独立校准worker启动");
        } else check(worker->start(config), "worker启动");
    }
    void ready() { check(until([&]{return worker->snapshot().phase == RecoilPhase::READY;}), "等待新按下资格"); }
};
}
int main() {
    try {
        {
            Fixture f(true,false,false,false,0,20,false,true);f.ready();
            for(int run=1;run<=2;++run) {
                f.mouse->held=true;
                check(until([&]{return f.worker->snapshot().session_id==run&&f.worker->snapshot().phase==RecoilPhase::EXHAUSTED;}),
                    "人工确认曲线普通手动两次独立按下均完成首发重放");
                const auto moves=f.mouse->moves.load();std::this_thread::sleep_for(10ms);
                check(f.mouse->moves==moves&&f.worker->snapshot().session_id==run,"持续手动按下不循环尾部");
                f.mouse->held=false;f.ready();
            }
        }
        {
            Fixture f(true,false,false,false,0,20,false,true);f.ready();
            auto trigger=std::make_shared<TriggerWorker>(f.mouse,f.arbiter,[]{return true;},[]{return true;},
                []{return std::uint64_t{1};},[](std::uint64_t){return true;},[](std::uint64_t){});
            TriggerConfig cfg;cfg.enabled=true;cfg.require_stop=false;cfg.hold_virtual_key=5;
            cfg.fire_delay_ms=0;cfg.press_duration_ms=40;cfg.shot_interval_ms=80;cfg.max_observation_age_ms=1000;
            check(trigger->start(cfg),"人工确认双worker重放启动扳机");f.live_trigger.store(trigger);
            check(until([&]{return trigger->snapshot().reason==TriggerReason::RELEASED;}),"扳机先观察释放");
            for(int run=1;run<=2;++run) {
                auto observation=std::make_shared<TriggerObservation>();
                observation->detections.push_back({30,30,70,70,0.9f,0});observation->center_x=observation->center_y=50;
                observation->roi_width=observation->roi_height=100;observation->epoch=1;observation->sequence=run;
                observation->valid=observation->timing_valid=true;observation->observed_at=TriggerClock::now();
                f.mouse->trigger_key=true;trigger->publish(observation);
                check(until([&]{return f.worker->snapshot().session_id==run&&f.worker->snapshot().phase==RecoilPhase::EXHAUSTED;}),
                    "两次独立真实TriggerWorker命令均重放人工确认曲线");
                f.mouse->trigger_key=false;
                check(until([&]{return !trigger->firing_signal().confirmed_down&&trigger->snapshot().reason==TriggerReason::RELEASED;}),
                    "每次命令确认UP并释放扳机保持键");
                f.ready();
            }
            check(f.mouse->downs==2&&f.mouse->ups==2&&f.worker->snapshot().session_id==2,
                "两个独立扳机弹序各执行一次，不伪造恢复毫秒");
            trigger->stop();f.worker->stop();
        }
        {
            Fixture f(false,false,false,true,0,20,true);f.mouse->calibration_key=false;f.ready();
            f.synthetic_uncertainty_ns=1000;f.synthetic_start=RecoilClock::now();f.synthetic_down=true;
            check(until([&]{return f.mouse->moves>0;}),"显式自动调试permit允许已确认命令源且不伪造保持键");
            ++f.synthetic_id;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"同permit命令id变化必须终止");
            f.worker->stop();check(f.mouse->downs==0&&f.mouse->ups==0,"压枪Worker不占有左键");
        }
        {
            Fixture f(false,false,false,true,0,20,true);f.ready();
            f.synthetic_start=RecoilClock::now();f.synthetic_down=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"缺少ACK完成区间拒绝自动调试");
            check(f.mouse->moves==0,"无有效回执区间不得移动");
        }
        {
            Fixture f(false,false,false,true,0,20,true);f.ready();f.mouse->held=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"自动调试拒绝混入人工左键");
            check(f.mouse->moves==0,"人工左键不能启动自动调试permit");
        }
        {
            Fixture f(false,true,false,false,0,1000);f.ready();
            f.synthetic_start=RecoilClock::now();f.synthetic_down=true;
            check(until([&]{return f.mouse->moves>0;}),"冷却接管前先完成软件点射");
            const auto session=f.worker->snapshot().session_id;f.synthetic_down=false;f.ready();
            f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().session_id==session+1;}),
                "点射UP后的冷却空档人工按下也从第一发开始，不等待旧弹道恢复");
        }
        {
            // 两个真实worker共享owner：UP在途时不把known-down误当失联，人工重新起压只执行一次。
            Fixture f;f.ready();
            auto trigger=std::make_shared<TriggerWorker>(f.mouse,f.arbiter,[]{return true;},[]{return true;},
                []{return std::uint64_t{1};},[](std::uint64_t){return true;},[](std::uint64_t){});
            TriggerConfig cfg;cfg.enabled=true;cfg.require_stop=false;cfg.hold_virtual_key=5;
            cfg.fire_delay_ms=0;cfg.press_duration_ms=500;cfg.shot_interval_ms=600;cfg.max_observation_age_ms=1000;
            check(trigger->start(cfg),"双worker测试启动扳机");f.live_trigger.store(trigger);
            check(until([&]{return trigger->snapshot().reason==TriggerReason::RELEASED;}),"双worker取得释放边沿");
            auto observation=std::make_shared<TriggerObservation>();
            observation->detections.push_back({30,30,70,70,0.9f,0});observation->center_x=observation->center_y=50;
            observation->roi_width=observation->roi_height=100;observation->epoch=observation->sequence=1;
            observation->valid=observation->timing_valid=true;observation->observed_at=TriggerClock::now();
            f.mouse->trigger_key=true;trigger->publish(observation);
            check(until([&]{return f.mouse->moves>1;}),"自动点射先启动压枪");
            const auto session=f.worker->snapshot().session_id;
            f.mouse->block_up=true;f.mouse->held=true;
            const bool entered=until([&]{return f.mouse->up_entered.load();});
            std::this_thread::sleep_for(10ms);
            const bool retained_signal=trigger->firing_signal().confirmed_down;
            f.mouse->release_up=true;
            check(entered&&retained_signal,"UP在途保留已确认DOWN，不提前伪造释放");
            check(until([&]{return f.worker->snapshot().session_id==session+1&&f.worker->snapshot().phase==RecoilPhase::FIRING;}),
                "人工接管从第一发重新压，UP短事务不永久取消新弹序");
            const auto count=f.mouse->moves.load();
            check(until([&]{return f.mouse->moves>count;}),"UP确认后人工持续补偿");
            check(f.mouse->downs==1&&f.mouse->ups==1&&!trigger->firing_signal().confirmed_down,
                "人工优先期间只清理自有软件按下，没有第二次自动DOWN");
            trigger->stop();f.worker->stop();
        }
        {
            // 普通压枪仅跟随实际射击来源，旧额外许可不能留下隐藏阻断。
            Fixture f(false, true, false, false, 999); f.ready(); f.mouse->held = true;
            check(until([&]{return f.mouse->moves.load() > 0;}), "普通压枪不等待已移除的额外许可键");
        }
        {
            Fixture f(false, true, false, true); f.ready();
            f.mouse->calibration_key = false; f.mouse->held = true;
            std::this_thread::sleep_for(25ms);
            check(f.mouse->moves == 0, "独立校准仍需人工保持键，不受普通压枪许可移除影响");
        }
        {
            Fixture f(true,false,false,true);f.ready();f.mouse->held=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"一次校准结束");
            check(f.worker->calibration_snapshot().end==RecoilCalibrationEnd::COMPLETED&&f.mouse->moves>0&&
                f.profile->state==RecoilProfileState::SCHEMA_VALID&&!f.profile->phase_tolerance_ms,
                "permit执行候选不伪造校准状态或相位证据");
            const auto count=f.mouse->moves.load();f.mouse->held=false;std::this_thread::sleep_for(10ms);f.mouse->held=true;
            std::this_thread::sleep_for(10ms);check(f.mouse->moves==count,"一次校准permit不能靠释放重复执行");
        }
        {
            Fixture f(false,true,false,true);f.ready();f.mouse->cancel_key=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"校准取消键立即关闭资格");
            f.mouse->cancel_key=false;f.mouse->held=true;std::this_thread::sleep_for(10ms);
            check(f.mouse->moves==0&&f.worker->calibration_snapshot().end==RecoilCalibrationEnd::CANCELED,
                "取消后按住不恢复校准资格");
        }
        {
            Fixture f(false,true,false,true);f.ready();f.mouse->unknown=true;f.mouse->held=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"校准UNKNOWN结束");
            check(f.mouse->moves==1&&f.worker->calibration_snapshot().sent_l1_counts>0&&
                f.worker->calibration_snapshot().end==RecoilCalibrationEnd::UNKNOWN_RECEIPT,
                "校准UNKNOWN保留消耗且不重试");
        }
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
            const auto events=f.worker->read_execution_events(0);
            check(events.events.size()==log.records.size()+2&&events.events.front().kind==RecoilExecutionEventKind::BEGIN&&
                events.events.back().kind==RecoilExecutionEventKind::END&&events.events.back().end_reason==RecoilBatchEndReason::CONTEXT,
                "首命令前BEGIN、失焦唯一END与命令共用序号");
            const auto again=f.worker->read_execution_events(0);
            check(again.events.size()==events.events.size()&&again.latest_sequence==events.latest_sequence,
                "事件读取非破坏性，两个消费者可独立读取");
        }
        {
            Fixture f; f.ready(); f.mouse->unknown = true; f.mouse->held = true;
            check(until([&]{return f.worker->snapshot().faulted;}), "未知回执故障");
            check(f.mouse->moves == 1 && !f.ledger->snapshot(RecoilClock::now()).complete, "未知位移不能重试");
            auto log=f.worker->execution_log();check(log.records.size()==1&&log.records[0].receipt.status==RecoilReceiptStatus::UNKNOWN,"未知回执独立保留");
            check(f.worker->read_execution_events(0).events.back().end_reason==RecoilBatchEndReason::UNKNOWN,"未知回执必须结束批次且不标自然耗尽");
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
            Fixture f;f.ready();f.mouse->pre_call_receipt=true;f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().faulted;}),"早于实际调用的完成时间不能接受");
            const auto record=f.worker->execution_log().records.front();
            check(record.receipt.completed_at>=record.intent.planned_at&&record.receipt.completed_at<record.backend_called_at&&
                record.receipt.status==RecoilReceiptStatus::UNKNOWN,"回执必须属于实际调用区间，不仅晚于规划时刻");
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
            const auto old_session=f.worker->snapshot().session_id;
            f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().session_id==old_session+1;}),"人工接管只建立一次从第一发开始的新弹序");
            auto count=f.mouse->moves.load();f.synthetic_down=false;
            check(until([&]{return f.mouse->moves>count;}),"软件UP后持续人工左键继续压枪");
            std::this_thread::sleep_for(25ms);
            check(f.worker->snapshot().session_id==old_session+1,"持续人工按住不能逐帧重置弹序");
            const auto events=f.worker->read_execution_events(0);
            std::vector<RecoilExecutionEvent> begins;
            for(const auto& event:events.events)if(event.kind==RecoilExecutionEventKind::BEGIN)begins.push_back(event);
            check(begins.size()==2&&begins[1].firing_source==RecoilFiringSource::INPUT_ESTIMATED&&
                begins[1].firing_started_at>begins[0].firing_started_at,"新人工批次记录自己的起点而不继承软件点射起点");
            f.mouse->healthy=false;
            check(until([&]{return f.worker->snapshot().reason==RecoilReason::CONTEXT;}),"接管后失联仍取消");
            count=f.mouse->moves.load();
            f.mouse->held=false;f.mouse->healthy=false;std::this_thread::sleep_for(10ms);
            f.mouse->held=true;f.mouse->healthy=true;std::this_thread::sleep_for(25ms);
            check(f.mouse->moves==count,"不可信释放不能解除来源冲突");
            f.mouse->held=false;check(until([&]{return f.worker->snapshot().phase==RecoilPhase::READY;}),"必须完整健康释放");
        }
        {
            Fixture f;f.ready();f.mouse->block=true;f.mouse->held=true;
            check(until([&]{return f.mouse->entered.load();}),"取消前move在途");
            f.worker->cancel();f.mouse->proceed=true;
            check(until([&]{return f.worker->snapshot().phase==RecoilPhase::WAIT_RELEASE;}),"迟到ACK后取消不复活，保持等待健康释放");
            check(f.mouse->moves==1&&f.worker->execution_log().records.size()==1,"在途完成仍有执行证据且不追发");
        }
        {
            Fixture f(false,true,true);f.ready();f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().phase==RecoilPhase::EXHAUSTED;}),"全零曲线按时自然结束");
            const auto events=f.worker->read_execution_events(0);
            check(f.mouse->moves==0&&events.events.size()==2&&events.events.front().kind==RecoilExecutionEventKind::BEGIN&&
                events.events.back().end_reason==RecoilBatchEndReason::EXHAUSTED,
                "零增量终点仍有BEGIN/END且不伪造零ACK");
        }
        {
            Fixture f(true,false);f.ready();f.mouse->held=true;
            check(until([&]{return f.worker->snapshot().phase==RecoilPhase::EXHAUSTED;}),"ACK终点自然结束");
            const auto events=f.worker->read_execution_events(0);
            check(events.events.size()>=3&&events.events[events.events.size()-2].kind==RecoilExecutionEventKind::COMMAND&&
                events.events.back().end_reason==RecoilBatchEndReason::EXHAUSTED,"最后ACK之后也必须END");
        }
        {
            Fixture f(false,true,false,true);f.ready();f.mouse->held=true;
            check(until([&]{return f.mouse->moves>0;}),"校准异常先进入执行");f.throw_context=true;
            check(until([&]{return f.worker->calibration_snapshot().terminal;}),"校准异常关闭预算");
            check(f.worker->calibration_snapshot().end==RecoilCalibrationEnd::UNKNOWN_RECEIPT,
                "校准异常不能先误标取消覆盖UNKNOWN");
        }
        {
            Fixture f;f.ready();f.mouse->held=true;
            check(until([&]{return f.mouse->moves>0;}),"停止测试先进入执行");f.worker->stop();
            check(f.worker->read_execution_events(0).events.back().end_reason==RecoilBatchEndReason::STOPPED,
                "worker停机发布最后STOPPED事件供归档drain");
        }
        {
            Fixture f;f.ready();f.mouse->held=true;
            check(until([&]{return f.mouse->moves>0;}),"异常测试先进入执行");f.throw_context=true;
            check(until([&]{return f.worker->snapshot().faulted;}),"上下文异常被收敛");
            check(f.worker->read_execution_events(0).events.back().end_reason==RecoilBatchEndReason::EXCEPTION,
                "异常结束不能伪造正常停止");
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
