#include "recoil/recoil_worker.h"
#include "log/log.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = RecoilClock;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void await(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + 3s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
enum class Completion { Known, Failed, MissingTime };
class MemoryMouse final : public IMouseController {
public:
    explicit MemoryMouse(Completion completion) : completion_(completion) {}
    std::atomic<bool> held{false}, entered{false}, timed_out{false};
    std::atomic<unsigned> moves{0}, polls{0};
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {};
        input.state_valid = true; input.status = InputMonitorStatus::READY;
        input.virtual_keys[1] = held; input.sequence = ++polls;
        return true;
    }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves;
        std::unique_lock lock(mutex_);
        entered = true;
        if (!changed_.wait_for(lock, 2s, [&] { return released_; })) timed_out = true;
        MouseMoveReceipt receipt;
        receipt.succeeded = completion_ != Completion::Failed;
        if (completion_ != Completion::MissingTime) receipt.backend_completed_at = Clock::now();
        return receipt;
    }
    void release() noexcept {
        { std::lock_guard lock(mutex_); released_ = true; }
        changed_.notify_all();
    }
private:
    Completion completion_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_ = false;
};
struct ReleaseOnExit {
    MemoryMouse& mouse;
    ~ReleaseOnExit() { mouse.release(); }
};
void source_during_move(Completion completion) {
    auto mouse = std::make_shared<MemoryMouse>(completion);
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    auto ledger = std::make_shared<MotionLedger>();
    auto profile = std::make_shared<RecoilProfile>();
    std::string error;
    // 仅内存自制离散曲线经既有Loader与显式确认；不保存、采用或登记候选。
    require(load_recoil_profile(R"({"schema_version":3,"id":"source_receipt_fixture","revision":1,
        "weapon_id":"ak47","sensitivity":2.45,"verified":false,"sample_semantics":"discrete_delta",
        "events":[[5,0,0],[40,0,7.5],[80,0,7.5],[120,0,7.5]]})", *profile, error),
        "自制曲线加载失败");
    require(confirm_recoil_profile(*profile, error), "自制回执夹具显式确认失败");
    ledger->reset(14, 16, Clock::now());
    std::atomic<std::uint64_t> source_epoch{0};
    RecoilWorker worker(mouse, arbiter, ledger, [&] {
        RecoilInput input;
        input.permission = input.focused = input.profile_conditions_match = true;
        input.profile = profile; input.device_epoch = input.weapon_generation = input.weapon_trust_generation = 1;
        input.source_context_epoch = source_epoch.load();
        return input;
    }, [] { return TriggerFiringSignal{}; });
    // 比worker晚构造，异常时先放行内存后端再由worker析构等待退出。
    ReleaseOnExit release{*mouse};
    RecoilConfig config; config.enabled = config.mixed_aim = true;
    require(worker.start(config), "普通RecoilWorker启动失败");
    await([&] { return worker.snapshot().phase == RecoilPhase::READY; }, "未消费初始健康释放");
    mouse->held = true;
    await([&] { return mouse->entered.load(); }, "实际非零move未进入屏障");
    source_epoch = 1;
    const auto changed_at = Clock::now();
    mouse->release();
    const bool unknown = completion != Completion::Known;
    await([&] { return unknown ? worker.snapshot().faulted
        : worker.snapshot().phase == RecoilPhase::WAIT_RELEASE; }, "源换代未终止旧弹序");
    const auto first = worker.snapshot();
    const auto log = worker.execution_log();
    require(!mouse->timed_out && mouse->moves == 1 && log.records.size() == 1, "屏障超时或旧非零命令重复");
    const auto& record = log.records.front();
    require(record.backend_called && record.intent.dy_counts != 0 &&
        record.backend_called_at <= changed_at && changed_at <= record.backend_returned_at,
        "来源变化必须发生在真实记录式后端调用内部");
    require(record.receipt.status == (unknown ? RecoilReceiptStatus::UNKNOWN : RecoilReceiptStatus::ACKNOWLEDGED),
        "回执分类未保留");
    require(unknown ? first.confirmed_y == 0 && first.unknown_y == record.intent.dy_counts
        : first.confirmed_y == record.intent.dy_counts && first.unknown_y == 0,
        "在途命令不能被源换代伪装成零执行");
    const auto target_polls = mouse->polls.load() + 20;
    await([&] { return mouse->polls >= target_polls; }, "未执行后续许可复核");
    require(mouse->moves == 1, "旧保持来源产生了后续命令");
    if (unknown) {
        require(!ledger->snapshot(Clock::now()).complete && !arbiter->try_enter_aim().owns_lock(),
            "UNKNOWN必须使账本不完整并阻断共享输出");
        mouse->held = false;
        const auto released_polls = mouse->polls.load() + 10;
        await([&] { return mouse->polls >= released_polls; }, "未知回执后的释放未被读取");
        mouse->held = true;
        const auto reheld_polls = mouse->polls.load() + 10;
        await([&] { return mouse->polls >= reheld_polls; }, "未知回执后的保持未被读取");
        worker.stop();
        require(mouse->moves == 1 && worker.snapshot().faulted && !ledger->healthy(),
            "UNKNOWN不得因新松按或停止而变成可重试");
    } else {
        require(ledger->healthy(), "已知完成不应污染账本");
        mouse->held = false;
        await([&] { return worker.snapshot().phase == RecoilPhase::READY; }, "健康释放未恢复新会话资格");
        mouse->held = true;
        await([&] { return mouse->moves >= 2 && worker.snapshot().session_id == 2; }, "新来源松按未开始新弹序");
        mouse->held = false; worker.stop();
        require(!worker.snapshot().faulted && ledger->healthy(), "健康恢复意外损坏账本");
    }
    require(!arbiter->recoil_y_owned(), "停止后未归还Y所有权");
    std::cout << "source-during-move mode=" << static_cast<int>(completion)
        << " first_confirmed_y=" << first.confirmed_y << " first_unknown_y=" << first.unknown_y
        << " first_records=" << log.records.size() << '\n';
}
} // namespace

int main() {
    LogConfig logging; logging.enable_file = logging.enable_console = logging.enable_ringbuf = false;
    Log::init(logging);
    int result = 0;
    try {
        source_during_move(Completion::Known);
        source_during_move(Completion::Failed);
        source_during_move(Completion::MissingTime);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    Log::shutdown();
    return result;
}
