#include "auto_stop/auto_stop_worker.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::int64_t clock_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void wait_for(Predicate predicate) {
    const auto limit = Clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        if (Clock::now() >= limit) throw std::runtime_error("worker专项等待超时");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class Fake final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool poll_input(InputSnapshot& input) noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        input = {}; input.status = healthy ? InputMonitorStatus::READY : InputMonitorStatus::FAILURE;
        input.state_valid = healthy;
        input.sequence = sequence;
        input.virtual_keys[5] = true;
        input.virtual_keys[0x23] = end;
        input.virtual_keys['W'] = (held & 1) != 0;
        input.virtual_keys['A'] = (held & 2) != 0;
        input.virtual_keys['S'] = (held & 4) != 0;
        input.virtual_keys['D'] = (held & 8) != 0;
        return true;
    }
    bool set_wasd_event_subscription(bool value) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); subscribed = value; ++subscriptions; return true;
    }
    bool read_wasd_events(WasdEventCursor&, WasdEventBatch& batch) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); batch = {}; batch.subscribed = subscribed; batch.gap = gap;
        while (!events.empty() && batch.count < batch.events.size()) {
            batch.events[batch.count++] = events.front(); events.pop_front();
        }
        return true;
    }
    KeyboardReceipt acknowledged() noexcept {
        KeyboardReceipt result; result.datagram_sent = true; result.disposition = KeyboardDisposition::ACKNOWLEDGED;
        result.backend_completed_at = result.protocol_ack_received_at = Clock::now(); return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); software.push_back(mask); current_software = mask;
        auto result = acknowledged();
        if (software_fail_at == software.size()) result.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        return result;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); masks.push_back(masked ? key : 0);
        if (masked) installed_masks |= key; else installed_masks &= ~key;
        auto result = acknowledged();
        if (mask_fail_at == masks.size()) result.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        return result;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        std::lock_guard<std::mutex> lock(mutex); ++cleanup_checks;
        if (installed_masks == 0 && current_software == 0) {
            KeyboardReceipt result; result.disposition = KeyboardDisposition::ACKNOWLEDGED; return result;
        }
        ++cleanups; auto receipt = acknowledged();
        cleanup_at = Clock::now();
        if (cleanup_fails) receipt.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        else { installed_masks = 0; current_software = 0; }
        return receipt;
    }
    void close() noexcept override { ++closes; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    void physical(std::uint8_t mask) {
        std::lock_guard<std::mutex> lock(mutex); held = mask;
        events.push_back({mask, true, 1, ++sequence, clock_ns()});
    }
    bool drained() { std::lock_guard<std::mutex> lock(mutex); return events.empty(); }
    bool has_software() { std::lock_guard<std::mutex> lock(mutex); return !software.empty(); }
    bool has_cleanup() { std::lock_guard<std::mutex> lock(mutex); return cleanups != 0; }
    bool has_masks() { std::lock_guard<std::mutex> lock(mutex); return !masks.empty(); }
    bool released() { std::lock_guard<std::mutex> lock(mutex); return current_software == 0 && installed_masks == 0; }
    std::vector<int> reports() { std::lock_guard<std::mutex> lock(mutex); return software; }
    Clock::time_point cleaned_at() { std::lock_guard<std::mutex> lock(mutex); return cleanup_at; }
    std::mutex mutex;
    std::deque<WasdEvent> events;
    std::vector<int> software, masks;
    std::uint64_t sequence = 0;
    std::uint8_t held = 0, installed_masks = 0, current_software = 0;
    bool healthy = true, subscribed = false, gap = false, cleanup_fails = false, end = false;
    std::size_t software_fail_at = 0, mask_fail_at = 0;
    int cleanups = 0, cleanup_checks = 0, subscriptions = 0;
    std::atomic<int> closes{0};
    Clock::time_point cleanup_at{};
};
void ready(AutoStopWorker& worker, const std::shared_ptr<Fake>& fake) {
    fake->physical(0);
    wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
    fake->physical(1);
    wait_for([&] { return fake->drained(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
}
int main() {
    try {
        const AutoStopConfig config{true, 5};
        {
            AutoStopOutputArbiter arbiter;
            auto owner = arbiter.try_enter_aim();
            OutputArbiterRejection reason{};
            bool entered = false;
            std::thread contender([&] { entered = arbiter.try_enter_aim(OutputArbiterSource::TRIGGER, &reason).owns_lock(); });
            contender.join();
            require(!entered && reason == OutputArbiterRejection::LOCK_BUSY, "跨线程争用必须标明锁竞争");
            owner.unlock();
            require(arbiter.aim_skips() == 0 && arbiter.snapshot().sources[1].lock_busy == 1,
                "Trigger拒绝不得污染Aim计数");
            require(arbiter.try_enter_aim(OutputArbiterSource::RECOIL, &reason).owns_lock() && reason == OutputArbiterRejection::NONE,
                "Recoil成功取得门应独立计数");
            arbiter.latch_output_fault();
            require(!arbiter.try_enter_aim(OutputArbiterSource::RECOIL, &reason).owns_lock() && reason == OutputArbiterRejection::OUTPUT_FAULT,
                "故障锁存必须保持拒绝并标明原因");
            const auto stats = arbiter.snapshot();
            require(stats.sources[0].acquired == 1 && stats.sources[2].acquired == 1 && stats.sources[2].output_fault == 1,
                "成功与故障按来源保存");
            require(arbiter.try_enter_cleanup().owns_lock(), "统计改造不能阻断故障后的清理");
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config, 100), "worker启动失败");
            ready(worker, fake);
            require(!fake->has_software() && !fake->has_masks(), "无请求不得发包");
            auto aim = arbiter->try_enter_aim();
            require(aim.owns_lock(), "空闲Aim门不可用");
            const auto requested_at = Clock::now();
            require(worker.request(1), "请求未接收");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!fake->has_software(), "在途Aim期间不得进入设备");
            aim.unlock();
            wait_for([&] { return fake->has_software(); });
            OutputArbiterRejection blocked{};
            require(!arbiter->try_enter_aim(OutputArbiterSource::AIM, &blocked).owns_lock(), "制动时Aim不应阻塞或进入");
            require(blocked == OutputArbiterRejection::AUXILIARY_PENDING && arbiter->snapshot().sources[0].auxiliary_pending > 0,
                "制动独占必须与普通锁竞争区分");
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            worker.cancel(999);
            fake->physical(1);
            wait_for([&] { return fake->drained(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(worker.snapshot().status == AutoStopStatus::ESTIMATED && !fake->has_cleanup(), "同键报告不应提前归还物理屏蔽");
            require(arbiter->try_enter_aim().owns_lock(), "估算阶段应归还Aim发送机会");
            require(!worker.snapshot().fire_permitted && !worker.request(1), "不能开火或续旧请求");
            // 不投递任何帧或取消消息，租期本身必须推动归还。
            wait_for([&] { return fake->has_cleanup(); });
            require(fake->cleaned_at() - requested_at >= std::chrono::milliseconds(500), "租期结束前不得无故提前归还");
            worker.stop();
            require(fake->closes == 0 && fake->reports().back() == 0 && fake->released(), "共享owner不能关闭且反向键必须释放");
            require(worker.snapshot().aim_skips > 0 && worker.snapshot().arbiter_wait_samples > 0, "耦合实测计数缺失");
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config), "worker启动失败"); ready(worker, fake);
            require(worker.request(10), "取消测试请求失败"); wait_for([&] { return fake->has_software(); });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = true; }
            worker.set_paused(true);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::FAULT; });
            require(worker.snapshot().cleanup_unknown && !worker.request(11), "清理未知必须锁存FAULT");
            require(!arbiter->try_enter_aim().owns_lock(), "清理未知不能放行Aim");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = false; }
            worker.stop();
            require(!worker.snapshot().cleanup_unknown && worker.snapshot().status == AutoStopStatus::FAULT &&
                !arbiter->try_enter_aim().owns_lock(), "成功补清理需保留旧会话故障锁存");
        }
        {
            auto fake = std::make_shared<Fake>();
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->current_software = 4; fake->installed_masks = 1; fake->cleanup_fails = true; }
            auto first_arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker first(fake, first_arbiter, [] { return true; });
            require(!first.start(config) && first.snapshot().cleanup_unknown &&
                !first_arbiter->try_enter_aim().owns_lock(), "新worker必须拒绝未确认历史债务");
            auto next_arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker next(fake, next_arbiter, [] { return true; });
            require(!next.start(config) && !next_arbiter->try_enter_aim().owns_lock(), "换worker和arbiter不能绕过同owner债务");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = false; }
            auto recovered_arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker recovered(fake, recovered_arbiter, [] { return true; });
            require(recovered.start(config) && fake->released(), "新会话必须先确认历史债务已清理");
            recovered.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config), "worker启动失败"); ready(worker, fake);
            require(worker.request(20), "缺口测试请求失败"); wait_for([&] { return fake->has_software(); });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->gap = true; }
            wait_for([&] { return fake->has_cleanup(); }); worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start({false, 0}), "关闭配置应允许初始化"); worker.stop();
            require(fake->subscriptions == 0 && fake->cleanup_checks == 0 && !fake->has_software() && !fake->has_cleanup(), "关闭时不应订阅或输出");
        }
        for (int reason = 0; reason < 3; ++reason) {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            std::atomic<bool> permission{true};
            AutoStopWorker worker(fake, arbiter, [&] { return permission.load(); });
            require(worker.start(config), "安全取消启动失败"); ready(worker, fake);
            require(worker.request(30 + reason), "安全取消请求失败"); wait_for([&] { return fake->has_software(); });
            if (reason == 0) { std::lock_guard<std::mutex> lock(fake->mutex); fake->end = true; }
            else if (reason == 1) permission.store(false);
            else fake->physical(8);
            wait_for([&] { return fake->has_cleanup(); });
            require(fake->released(), "End/许可/改向必须清理"); worker.stop();
        }
        for (int failure_kind = 0; failure_kind < 2; ++failure_kind) {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            if (failure_kind == 0) fake->software_fail_at = 1; else fake->mask_fail_at = 2;
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config), "未知ACK测试启动失败"); ready(worker, fake);
            if (failure_kind == 1) {
                fake->physical(3); wait_for([&] { return fake->drained(); });
            }
            require(worker.request(40 + failure_kind), "未知ACK测试请求失败");
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::FAULT; });
            require(fake->has_cleanup() && fake->released() && !worker.snapshot().cleanup_unknown, "未知/部分ACK需清理全部责任");
            require(!worker.request(99), "FAULT不得接新请求"); worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config), "起始持键测试启动失败");
            fake->physical(1); wait_for([&] { return fake->drained(); });
            require(!worker.request(50) && !fake->has_software() && !fake->has_masks(), "起始已持键不能伪造历史");
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start({true, 0}), "未绑定测试启动失败"); fake->physical(0);
            wait_for([&] { return fake->drained(); });
            require(worker.snapshot().status == AutoStopStatus::UNBOUND && !worker.request(51), "未绑定不得假待命"); worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; });
            require(worker.start(config), "双轴测试启动失败"); ready(worker, fake);
            fake->physical(3); wait_for([&] { return fake->drained(); });
            // 为第二轴提供明确的短持键历史，不把最终双键集合当同时按下。
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            require(worker.request(60), "双轴请求失败");
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            const auto reports = fake->reports();
            require(reports.size() >= 3 && reports.front() == 12 && reports.back() == 0 &&
                std::find(reports.begin(), reports.end(), 4) != reports.end(), "双轴必须按各自deadline更新报告");
            fake->physical(0);
            wait_for([&] { return fake->has_cleanup(); });
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            require(fake->released(), "双轴mask必须全清理");
            fake->physical(1); wait_for([&] { return fake->drained(); });
            require(worker.request(61), "清理后的合法释放不能被丢弃"); worker.cancel(); worker.stop();
        }
        std::cout << "自动急停worker专项通过\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
