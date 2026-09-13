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
        input.virtual_keys[5] = activation;
        input.virtual_keys[0x23] = end;
        for (std::size_t key = 0; key < extra_keys.size(); ++key)
            input.virtual_keys[key] = input.virtual_keys[key] || extra_keys[key];
        input.virtual_keys['W'] = (held & 1) != 0;
        input.virtual_keys['A'] = (held & 2) != 0;
        input.virtual_keys['S'] = (held & 4) != 0;
        input.virtual_keys['D'] = (held & 8) != 0;
        return true;
    }
    bool set_wasd_event_subscription(bool value) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); subscribed = value; ++subscriptions; return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); batch = {}; batch.subscribed = subscribed; batch.gap = gap;
        // 与生产环形历史一致：读取只推进调用方游标，不破坏其它游标的事件。
        cursor.epoch = 1;
        for (const auto& event : events) if (event.sequence > cursor.sequence && batch.count < batch.events.size()) {
            batch.events[batch.count++] = event; cursor.sequence = event.sequence;
        }
        delivered_sequence = std::max(delivered_sequence, cursor.sequence);
        return true;
    }
    KeyboardReceipt acknowledged() noexcept {
        KeyboardReceipt result; result.datagram_sent = true; result.disposition = KeyboardDisposition::ACKNOWLEDGED;
        result.backend_completed_at = result.protocol_ack_received_at = Clock::now(); return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        std::unique_lock<std::mutex> lock(mutex); software.push_back(mask); current_software = mask;
        auto result = acknowledged();
        if (software_fail_at == software.size()) result.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        auto callback = software.size() == 1 ? after_first_software : std::function<void()>{};
        lock.unlock();
        if (callback) callback();
        return result;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        std::unique_lock<std::mutex> lock(mutex); masks.push_back(masked ? key : 0);
        if (masked) installed_masks |= key; else installed_masks &= ~key;
        const auto index = masks.size();
        const bool fail = mask_fail_at == index;
        auto callback = after_mask;
        lock.unlock();
        if (callback) callback(index);
        auto result = acknowledged();
        if (fail) result.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
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
        if (physical_during_cleanup >= 0) {
            held = static_cast<std::uint8_t>(physical_during_cleanup);
            events.push_back({held, true, 1, ++sequence, clock_ns()});
            physical_during_cleanup = -1;
        }
        return receipt;
    }
    void close() noexcept override { ++closes; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    void physical(std::uint8_t mask) {
        std::lock_guard<std::mutex> lock(mutex); held = mask;
        events.push_back({mask, true, 1, ++sequence, clock_ns()});
    }
    bool drained() { std::lock_guard<std::mutex> lock(mutex); return delivered_sequence == sequence; }
    bool has_software() { std::lock_guard<std::mutex> lock(mutex); return !software.empty(); }
    bool has_cleanup() { std::lock_guard<std::mutex> lock(mutex); return cleanups != 0; }
    bool has_masks() { std::lock_guard<std::mutex> lock(mutex); return !masks.empty(); }
    bool released() { std::lock_guard<std::mutex> lock(mutex); return current_software == 0 && installed_masks == 0; }
    std::vector<int> reports() { std::lock_guard<std::mutex> lock(mutex); return software; }
    Clock::time_point cleaned_at() { std::lock_guard<std::mutex> lock(mutex); return cleanup_at; }
    std::mutex mutex;
    std::deque<WasdEvent> events;
    std::vector<int> software, masks;
    std::array<bool, 256> extra_keys{};
    std::uint64_t sequence = 0, delivered_sequence = 0;
    std::uint8_t held = 0, installed_masks = 0, current_software = 0;
    bool healthy = true, subscribed = false, gap = false, cleanup_fails = false, end = false, activation = true;
    std::size_t software_fail_at = 0, mask_fail_at = 0;
    int cleanups = 0, cleanup_checks = 0, subscriptions = 0;
    int physical_during_cleanup = -1;
    std::function<void()> after_first_software;
    std::function<void(std::size_t)> after_mask;
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
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "持续侧键下A/D重叠换向回归启动");
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            for (const auto mask : {2, 10, 8, 10, 2}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
            }
            worker.publish_target(Clock::now() + std::chrono::seconds(5));
            const auto limit = Clock::now() + std::chrono::milliseconds(300);
            while (Clock::now() < limit && !fake->has_masks()) {
                for (const auto mask : {10, 8, 10, 2}) {
                    fake->physical(static_cast<std::uint8_t>(mask));
                    wait_for([&] { return fake->drained(); });
                }
            }
            require(fake->has_masks(), "健康A/D重叠换向后进框不得永久等待四键全松而完全不接管");
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::MASKED; });
            require(worker.snapshot().requests == 1 && worker.snapshot().completed == 0 &&
                !worker.snapshot().fire_permitted, "仅屏蔽不是估算停稳，不得授予开火");
            const auto masked_reports = fake->reports();
            require(!masked_reports.empty() && std::all_of(masked_reports.begin(), masked_reports.end(),
                [](int mask) { return mask == 0; }), "模型不可用时只能发送零软件报告");
            worker.publish_target({});
            for (const auto mask : {10, 8, 10, 2, 0, 8, 10, 2, 10, 8}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
                std::lock_guard<std::mutex> lock(fake->mutex);
                require(fake->installed_masks == 15 && fake->current_software == 0 && fake->cleanups == 0,
                    "仅屏蔽后持续乱按AD、重叠及全松均不能解除四键屏蔽");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(520));
            require(worker.snapshot().status == AutoStopStatus::MASKED && worker.snapshot().requests == 1 &&
                fake->reports() == masked_reports && !fake->released(),
                "仅屏蔽保持不受目标失效或500ms制动期限解除，不重复制动");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            // 当前仍持D，不补任何全松事件，恢复侧键应可重新进入仅屏蔽。
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
            worker.publish_target(Clock::now() + std::chrono::seconds(5));
            wait_for([&] { return worker.snapshot().requests == 2 && worker.snapshot().status == AutoStopStatus::MASKED; });
            const auto repeated_reports = fake->reports();
            require(std::all_of(repeated_reports.begin(), repeated_reports.end(), [](int mask) { return mask == 0; }) &&
                worker.snapshot().completed == 0 && !worker.snapshot().fire_permitted,
                "持键重入仅屏蔽不得伪造模型历史或发非零报告");
            worker.stop();
            require(fake->released(), "停止仅屏蔽必须归还全部键盘债务");
        }
        for (int reason = 0; reason < 11; ++reason) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            std::atomic<bool> permitted{true}, focused{true};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [&] { return permitted.load(); },
                [&] { return ++id; }, [&] { return focused.load(); });
            require(worker.start(config), "仅屏蔽安全撤销矩阵启动");
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            for (const auto mask : {2, 10, 8}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
            }
            worker.publish_target(Clock::now() + std::chrono::seconds(5));
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::MASKED; });
            if (reason == 0) focused.store(false);
            if (reason == 1) permitted.store(false);
            if (reason == 2) { std::lock_guard<std::mutex> lock(fake->mutex); fake->gap = true; }
            if (reason == 3) { std::lock_guard<std::mutex> lock(fake->mutex); fake->healthy = false; }
            if (reason == 4) { std::lock_guard<std::mutex> lock(fake->mutex); fake->end = true; }
            if (reason == 5) worker.set_paused(true);
            if (reason == 6) worker.cancel(worker.snapshot().request_id);
            if (reason == 7) { std::lock_guard<std::mutex> lock(fake->mutex);
                fake->events.push_back({8, false, 1, ++fake->sequence, clock_ns()}); }
            if (reason == 8) { std::lock_guard<std::mutex> lock(fake->mutex);
                fake->events.push_back({8, true, 2, ++fake->sequence, clock_ns()}); }
            if (reason == 9) { std::lock_guard<std::mutex> lock(fake->mutex);
                fake->sequence += 2;
                fake->events.push_back({8, true, 1, fake->sequence, clock_ns()}); }
            if (reason == 10) { std::lock_guard<std::mutex> lock(fake->mutex);
                fake->events.push_back({8, true, 1, ++fake->sequence, 0});
                fake->held = 0;
                fake->events.push_back({0, true, 1, ++fake->sequence, clock_ns()}); }
            wait_for([&] { return fake->released() && worker.snapshot().canceled != 0; });
            const auto reports = fake->reports();
            require(std::all_of(reports.begin(), reports.end(), [](int mask) { return mask == 0; }) &&
                worker.snapshot().completed == 0 && !worker.snapshot().fire_permitted,
                "仅屏蔽遇失焦、安全撤销、缺口、失联、End、暂停、取消、无效事件或换代必须归还且不发非零");
            worker.stop();
        }
        for (int failure = 0; failure < 6; ++failure) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            if (failure < 4) fake->mask_fail_at = static_cast<std::size_t>(failure + 1);
            if (failure == 4) fake->software_fail_at = 1;
            require(worker.start(config), "仅屏蔽ACK故障矩阵启动");
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            for (const auto mask : {2, 10, 8}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
            }
            worker.publish_target(Clock::now() + std::chrono::seconds(5));
            if (failure == 5) {
                wait_for([&] { return worker.snapshot().status == AutoStopStatus::MASKED; });
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = true; fake->activation = false; }
            }
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::FAULT; });
            const auto reports = fake->reports();
            require(std::all_of(reports.begin(), reports.end(), [](int mask) { return mask == 0; }) &&
                worker.snapshot().completed == 0 && !worker.snapshot().fire_permitted,
                "任一mask或零报告ACK未知不能冒充成功，更不能发反向报告");
            if (failure < 5) require(fake->released(), "安装或零报告失败必须清理已安装屏蔽");
            else require(worker.snapshot().cleanup_unknown && !fake->released(), "清理ACK未知必须保留债务和FAULT");
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; });
            require(worker.start(config), "显式请求不能借用仅屏蔽降级路径");
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            for (const auto mask : {2, 10, 8}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
            }
            const bool queued = worker.request(1);
            if (queued) wait_for([&] { return worker.snapshot().canceled == 1; });
            require(!fake->has_masks() && !fake->has_software() && worker.snapshot().completed == 0 &&
                !worker.snapshot().fire_permitted, "显式请求遇模型历史失效须拒绝，不能转MASKED");
            worker.stop();
        }
        for (int change = 0; change < 3; ++change) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            fake->after_mask = [&](std::size_t index) {
                if (index != 4) return;
                if (change == 0) { fake->physical(2); fake->physical(1); }
                if (change == 1) { std::lock_guard<std::mutex> lock(fake->mutex); fake->gap = true; }
                if (change == 2) { std::lock_guard<std::mutex> lock(fake->mutex);
                    fake->events.push_back({1, false, 1, ++fake->sequence, clock_ns()}); }
            };
            require(worker.start(config), "屏蔽安装窗口一致性回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().canceled != 0 || fake->has_software(); });
            require(worker.snapshot().canceled == 1 && !fake->has_software() && fake->released(),
                "安装窗口改向或原始输入异常须在首个软件反向报告前拒绝并归还");
            worker.stop();
        }
        for (int change = 0; change < 8; ++change) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            fake->after_first_software = [&] {
                // 在第一次非零报告返回ACK前注入，确保覆盖BRAKING而非完成后的保持。
                if (change == 0) worker.publish_target({}, AutoStopBlockReason::CROSSHAIR_OUTSIDE_TARGET);
                if (change == 1) worker.publish_target(Clock::now() - std::chrono::milliseconds(1));
                if (change == 2) { worker.publish_target({}); worker.publish_target(Clock::now() + std::chrono::seconds(1)); }
                if (change >= 3) fake->physical(static_cast<std::uint8_t>(change == 3 ? 2 : change == 4 ? 0 :
                    change == 5 ? 5 : change == 6 ? 8 : 10));
            };
            require(worker.start(config), "制动途中锁存回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED || worker.snapshot().canceled != 0; });
            require(worker.snapshot().canceled == 0 && worker.snapshot().status == AutoStopStatus::ESTIMATED,
                "已接管后制动途中离框、过期、改向不得取消，必须完成零报告并保持");
            require(fake->reports().front() != 0 && fake->reports().back() == 0 && !fake->released() &&
                worker.snapshot().requests == 1 && !worker.snapshot().fire_permitted,
                "只执行原有限制动并保持四键屏蔽，不重复请求或授予开火");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "清理期间真实按键事件不得丢失");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            fake->physical(0);
            wait_for([&] { return fake->drained(); });
            { std::lock_guard<std::mutex> lock(fake->mutex);
                fake->physical_during_cleanup = 1; fake->activation = false; }
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().requests == 2 && worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "W持续按住时允许键完整循环启动");
            ready(worker, fake);
            for (int cycle = 1; cycle <= 3; ++cycle) {
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
                worker.publish_target(Clock::now() + std::chrono::seconds(1));
                wait_for([&] { return worker.snapshot().requests == cycle &&
                    worker.snapshot().status == AutoStopStatus::ESTIMATED; });
                worker.publish_target({});
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
                wait_for([&] { return fake->released() && worker.snapshot().canceled == cycle; });
                // 全循环中不发布任何W释放/重按事件；下一轮必须从受控归还继续。
                wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "锁存式急停回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            worker.publish_target({}, AutoStopBlockReason::CROSSHAIR_OUTSIDE_TARGET);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(!fake->released() && worker.snapshot().canceled == 0,
                "第一次制动完成后离框不能解除急停，必须保持至允许键释放");
            const auto normal_reports = fake->reports();
            require(!normal_reports.empty() && normal_reports.front() != 0 && normal_reports.back() == 0,
                "正常模型路径必须仍完成真实反向报告及末尾零报告");
            const auto reports_after_braking = normal_reports.size();
            for (const std::uint8_t directions : {0, 2, 4, 8, 3, 12, 15, 1, 2, 10, 8, 10, 2, 0, 8, 10, 2}) {
                fake->physical(directions);
                wait_for([&] { return fake->drained(); });
                { std::lock_guard<std::mutex> lock(fake->mutex);
                    require(fake->installed_masks == 15 && fake->current_software == 0,
                        "锁存后全部WASD必须屏蔽，换向或重按不能绕过急停"); }
                require(worker.snapshot().canceled == 0 && fake->reports().size() == reports_after_braking,
                    "锁存时改向不能解除或重新发送反向制动");
            }
            require(worker.snapshot().status == AutoStopStatus::ESTIMATED && worker.snapshot().requests == 1 &&
                worker.snapshot().completed == 1 && !worker.snapshot().fire_permitted && fake->reports() == normal_reports,
                "正常ESTIMATED路径持续乱按AD仍保持原零软件报告，不重启也不授予开火");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<bool> allowed{true};
            auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [&] { return allowed.load(); });
            require(worker.start(config), "故障救援测试启动");
            ready(worker, fake);
            require(worker.request(1), "救援前建立已持键盘债务");
            wait_for([&] { return fake->has_software(); });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = true; }
            worker.cancel(1);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::FAULT && worker.snapshot().cleanup_unknown; });
            allowed.store(false); worker.set_paused(true);
            const auto reports_before_rescue = fake->reports().size();
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys['1'] = true; }
            wait_for([&] { return worker.snapshot().rescue_failed == 1; });
            require(worker.snapshot().cleanup_unknown && !fake->released(), "救援未收到ACK不能报告已归还");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(worker.snapshot().rescue_attempts == 1, "持续按住救援键不能重复刷清理命令");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys['1'] = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys['1'] = true; }
            wait_for([&] { return fake->released() && worker.snapshot().rescue_succeeded == 1; });
            require(!worker.snapshot().cleanup_unknown && worker.snapshot().rescue_attempts == 2 &&
                fake->reports().size() == reports_before_rescue, "释放再按可重试，救援不能发送新DOWN");
            require(worker.snapshot().status == AutoStopStatus::FAULT, "救援只清急停债务，不解除共享故障锁存");
            require(!arbiter->try_enter_aim().owns_lock(), "救援成功不得清除共享输出故障");
            worker.stop();
        }
        for (const int rescue_key : config.release_virtual_keys) {
            auto fake = std::make_shared<Fake>();
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; });
            require(worker.start(config), "六键救援启动");
            ready(worker, fake);
            require(worker.request(1), "六键救援前建立债务");
            wait_for([&] { return fake->has_software(); });
            const auto reports_before_rescue = fake->reports().size();
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys[rescue_key] = true; }
            wait_for([&] { return worker.snapshot().rescue_succeeded == 1; });
            require(fake->released() && worker.snapshot().release_required &&
                fake->reports().size() == reports_before_rescue, "数字行1至5及Q任意单键均只清理并锁存重触发");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys[rescue_key] = false; }
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            require(worker.snapshot().release_required, "救援后仍按住activation不得清重触发锁存");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return !worker.snapshot().release_required; });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
            fake->physical(1);
            wait_for([&] { return fake->drained(); });
            require(worker.request(2), "显式模式救援后松开重新按下可接收新请求");
            wait_for([&] { return fake->reports().size() > reports_before_rescue; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; });
            require(worker.start(config), "监听失效救援边界启动");
            ready(worker, fake);
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->healthy = false; fake->extra_keys['Q'] = true; }
            wait_for([&] { return worker.snapshot().block_reason == AutoStopBlockReason::INPUT_UNAVAILABLE; });
            require(worker.snapshot().rescue_attempts == 0, "失联快照中的缓存按键不产生救援事实");
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<bool> focused{true};
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [&] { return focused.load(); });
            require(worker.start(config), "焦点恢复重新按键回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(2));
            wait_for([&] { return fake->has_software(); });
            focused.store(false);
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            focused.store(true);
            fake->physical(1);
            wait_for([&] { return fake->drained(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            require(worker.snapshot().requests == 1, "切回仍按住侧键不得自动再次接管键盘");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
            wait_for([&] { return worker.snapshot().requests == 2; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "独立目标急停应启动");
            ready(worker, fake);
            require(worker.snapshot().requests == 0, "只有允许键没有目标不得请求急停");
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return fake->has_software(); });
            require(worker.snapshot().requests == 1, "目标和允许键应独立触发急停，不依赖Trigger");
            worker.publish_target({});
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            require(!fake->released() && worker.snapshot().canceled == 0, "目标仅准入，已触发的制动不能因目标消失撤销");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return fake->released(); });
            require(!worker.snapshot().fire_permitted, "独立急停不得授予开火资格");
            worker.stop();
        }
        for (const bool explicit_first : {true, false}) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> watermark{0}, allocations{0}, focus_reads{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { ++allocations; return ++watermark; },
                [&] { ++focus_reads; return true; });
            require(worker.start(config), "混合请求归属测试启动");
            ready(worker, fake);
            std::uint64_t accepted_id = 0, rejected_id = 0;
            if (explicit_first) {
                accepted_id = ++watermark;
                require(worker.request(accepted_id), "显式先到必须占有唯一请求槽");
                wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
                worker.publish_target(Clock::now() + std::chrono::seconds(1));
                wait_for([&] { return worker.snapshot().target_available; });
                require(allocations.load() == 0, "显式请求在途时独立目标不能分配第二个请求");
                rejected_id = ++watermark;
                require(!worker.request(rejected_id), "占用期间第二个显式请求必须拒绝");
            } else {
                worker.publish_target(Clock::now() + std::chrono::seconds(1));
                wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
                accepted_id = worker.snapshot().request_id;
                rejected_id = ++watermark;
                require(allocations.load() == 1 && !worker.request(rejected_id),
                    "独立先到只分配一次，后到显式请求必须拒绝");
            }
            require(worker.snapshot().requests == 1 && worker.snapshot().request_id == accepted_id,
                "混合调用方只能有一个已接收请求且归属不变");
            worker.cancel(rejected_id);
            const auto next_focus_reads = focus_reads.load() + 2;
            wait_for([&] { return focus_reads.load() >= next_focus_reads; });
            require(worker.snapshot().canceled == 0 && worker.snapshot().request_id == accepted_id,
                "未取得请求槽的调用方取消不得清理已接收owner");
            worker.cancel(accepted_id);
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            require(worker.snapshot().requests == 1, "正确owner取消只能清理原请求，不生成替代请求");
            worker.stop();
        }
        for (int reason = 0; reason < 6; ++reason) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            std::atomic<bool> focused{true}, permitted{true};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [&] { return permitted.load(); },
                [&] { return ++id; }, [&] { return focused.load(); });
            require(worker.start(config), "取消矩阵启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return fake->has_software(); });
            if (reason == 0) worker.publish_target({});
            if (reason == 1) worker.publish_target(Clock::now() + std::chrono::milliseconds(10));
            if (reason == 2) focused.store(false);
            if (reason == 3) { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            if (reason == 4) permitted.store(false);
            if (reason == 5) { worker.publish_target({}); worker.publish_target(Clock::now() + std::chrono::seconds(1)); }
            if (reason == 0 || reason == 1 || reason == 5) {
                wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
                require(!fake->released() && worker.snapshot().canceled == 0,
                    "制动途中目标消失、旧帧与代际变化必须保持原请求");
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            }
            wait_for([&] { return fake->released() && worker.snapshot().canceled != 0; });
            require(!worker.snapshot().fire_permitted, "目标/旧帧/失焦/松键/安全撤销均不能授予开火");
            worker.stop();
        }
        for (int ending = 0; ending < 11; ++ending) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            std::atomic<bool> focused{true};
            auto arbiter = std::make_shared<AutoStopOutputArbiter>();
            AutoStopWorker worker(fake, arbiter, [] { return true; },
                [&] { return ++id; }, [&] { return focused.load(); });
            require(worker.start(config), "持续目标保持测试启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return fake->has_software(); });
            worker.cancel(99);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(worker.snapshot().canceled == 0, "其他owner的id不得取消独立请求");
            const auto until = Clock::now() + std::chrono::milliseconds(650);
            while (Clock::now() < until) {
                worker.publish_target(Clock::now() + std::chrono::milliseconds(50));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(!fake->released() && !fake->has_cleanup() && worker.snapshot().requests == 1 &&
                worker.snapshot().canceled == 0 && worker.snapshot().status == AutoStopStatus::ESTIMATED,
                "持续按住且目标新鲜时超过500ms必须保持屏蔽，不重复制动或提前恢复移动");
            require(fake->reports().back() == 0 && !worker.snapshot().fire_permitted,
                "保持阶段反向软件键必须已释放且不得授予开火");
            require(arbiter->try_enter_aim().owns_lock(), "保持阶段不得继续占住Aim发送机会");
            const auto reports_while_holding = fake->reports().size();
            // 0停止观测刷新；其余覆盖越过初始500ms后的全部主要撤销入口。
            if (ending == 1) worker.publish_target({});
            if (ending == 2) focused.store(false);
            if (ending == 3) { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            if (ending == 4) { std::lock_guard<std::mutex> lock(fake->mutex); fake->end = true; }
            if (ending == 5) { std::lock_guard<std::mutex> lock(fake->mutex); fake->healthy = false; }
            if (ending == 6) { std::lock_guard<std::mutex> lock(fake->mutex); fake->extra_keys['Q'] = true; }
            if (ending == 7) fake->physical(2);
            if (ending == 8) worker.set_paused(true);
            if (ending == 9) { std::lock_guard<std::mutex> lock(fake->mutex); fake->gap = true; }
            if (ending == 10) arbiter->latch_output_fault();
            if (ending == 0 || ending == 1 || ending == 7) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                require(!fake->released() && worker.snapshot().canceled == 0,
                    "锁存后目标到期、离框及改向均不能归还方向键");
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            }
            wait_for([&] {
                // 非目标撤销仍提供新鲜目标，不能让50ms到期掩盖撤销入口失效。
                if (ending >= 2) worker.publish_target(Clock::now() + std::chrono::milliseconds(50));
                return fake->released() && worker.snapshot().canceled == 1;
            });
            require(worker.snapshot().requests == 1 && fake->reports().size() == reports_while_holding,
                "目标到期清理不得重发反向制动或分配第二个请求");
            if (ending == 10) require(worker.snapshot().status == AutoStopStatus::FAULT,
                "共享输出故障必须归还键盘并保留FAULT");
            worker.stop();
        }
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
