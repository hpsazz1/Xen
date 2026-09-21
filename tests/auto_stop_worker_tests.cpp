#include "auto_stop/auto_stop_worker.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <future>
#include <initializer_list>
#include <string_view>
#include <source_location>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::int64_t clock_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void wait_for(Predicate predicate,
        const std::source_location source = std::source_location::current()) {
    const auto limit = Clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        if (Clock::now() >= limit) throw std::runtime_error(std::string("worker专项等待超时: ") +
            source.file_name() + ":" + std::to_string(source.line()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class Fake final : public IMouseController {
public:
    struct BackendCall {
        Fake& owner;
        explicit BackendCall(Fake& fake) : owner(fake) {
            const int count = ++owner.active_backend_calls;
            auto maximum = owner.max_backend_calls.load();
            while (maximum < count && !owner.max_backend_calls.compare_exchange_weak(maximum, count)) {}
        }
        ~BackendCall() { --owner.active_backend_calls; }
    };
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        BackendCall call(*this);
        ++moves;
        MouseMoveReceipt result; result.succeeded = true; result.backend_completed_at = Clock::now(); return result;
    }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool poll_input(InputSnapshot& input) noexcept override {
        std::unique_lock<std::mutex> lock(mutex);
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
        const bool inject_release = release_after_poll.exchange(false);
        lock.unlock();
        if (inject_release) physical(0);
        return true;
    }
    bool set_wasd_event_subscription(bool value) noexcept override {
        std::lock_guard<std::mutex> lock(mutex); subscribed = value; ++subscriptions; return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        if (before_wasd_read) before_wasd_read();
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
        BackendCall call(*this);
        std::unique_lock<std::mutex> lock(mutex); software.push_back(mask); current_software = mask;
        software_started.push_back(Clock::now());
        if (mask != 0 && reverse_ack_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(reverse_ack_delay_ms));
        auto result = acknowledged();
        software_ack.push_back(result.protocol_ack_received_at);
        if (software_fail_at == software.size()) result.disposition = KeyboardDisposition::APPLICATION_UNKNOWN;
        auto callback = software.size() == 1 ? after_first_software : std::function<void()>{};
        lock.unlock();
        if (callback) callback();
        return result;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        BackendCall call(*this);
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
        BackendCall call(*this);
        std::lock_guard<std::mutex> lock(mutex); ++cleanup_checks;
        cleanup_started.push_back(Clock::now());
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
        if (cleanup_report_mode != 0) {
            // 真实监听会重复报告同一按住状态；只有事实断裂才应中断归还。
            const int mode = cleanup_report_mode;
            cleanup_report_mode = 0;
            if (mode == 2) ++sequence;
            const auto timestamp = mode == 5 ? events.back().received_at_steady_ns - 1 : clock_ns();
            events.push_back({held, mode != 3, mode == 4 ? 2u : 1u, ++sequence, timestamp});
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
    void physical_batch(std::initializer_list<std::uint8_t> masks) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto mask : masks) {
            held = mask;
            events.push_back({mask, true, 1, ++sequence, clock_ns()});
        }
    }
    bool drained() { std::lock_guard<std::mutex> lock(mutex); return delivered_sequence == sequence; }
    bool has_software() { std::lock_guard<std::mutex> lock(mutex); return !software.empty(); }
    bool has_cleanup() { std::lock_guard<std::mutex> lock(mutex); return cleanups != 0; }
    bool has_masks() { std::lock_guard<std::mutex> lock(mutex); return !masks.empty(); }
    bool released() { std::lock_guard<std::mutex> lock(mutex); return current_software == 0 && installed_masks == 0; }
    std::vector<int> reports() { std::lock_guard<std::mutex> lock(mutex); return software; }
    Clock::time_point cleaned_at() { std::lock_guard<std::mutex> lock(mutex); return cleanup_at; }
    std::mutex mutex;
    std::atomic<bool> release_after_poll{false};
    std::atomic<int> active_backend_calls{0}, max_backend_calls{0}, moves{0};
    std::deque<WasdEvent> events;
    std::vector<int> software, masks;
    std::vector<Clock::time_point> software_started, software_ack;
    std::vector<Clock::time_point> cleanup_started;
    int reverse_ack_delay_ms = 0;
    std::array<bool, 256> extra_keys{};
    std::uint64_t sequence = 0, delivered_sequence = 0;
    std::uint8_t held = 0, installed_masks = 0, current_software = 0;
    bool healthy = true, subscribed = false, gap = false, cleanup_fails = false, end = false, activation = true;
    std::size_t software_fail_at = 0, mask_fail_at = 0;
    int cleanups = 0, cleanup_checks = 0, subscriptions = 0;
    int physical_during_cleanup = -1;
    int cleanup_report_mode = 0;
    std::function<void()> after_first_software;
    std::function<void()> before_wasd_read;
    std::function<void(std::size_t)> after_mask;
    std::atomic<int> closes{0};
    Clock::time_point cleanup_at{};
};
void bounded_aim_transaction_wait() {
    AutoStopOutputArbiter arbiter;
    auto owner = arbiter.try_enter_cleanup();
    require(owner.owns_lock(), "等待测试须先占用真实事务门");
    std::promise<void> entered;
    auto started = entered.get_future();
    auto waiting = std::async(std::launch::async, [&] {
        entered.set_value();
        auto opportunity = arbiter.enter_aim_until(Clock::now() + std::chrono::milliseconds(200));
        return opportunity.owns_lock();
    });
    started.wait();
    const auto waiting_status = waiting.wait_for(std::chrono::milliseconds(5));
    owner.unlock();
    require(waiting_status == std::future_status::timeout && waiting.get(),
        "Aim须等待短事务完成后进入，不能遇忙立即拒绝");
    require(arbiter.aim_skips() == 0 && arbiter.snapshot().sources[0].acquired == 1,
        "健康事务等待不能制造跳过计数");

    auto held = arbiter.try_enter_cleanup();
    auto expired = std::async(std::launch::async, [&] {
        OutputArbiterRejection reason{};
        auto opportunity = arbiter.enter_aim_until(Clock::now() + std::chrono::milliseconds(5), &reason);
        return !opportunity.owns_lock() && reason == OutputArbiterRejection::LOCK_BUSY;
    });
    require(expired.get(), "在途事务超出deadline必须明确退出");
    held.unlock();
    require(!arbiter.enter_aim_until({}).owns_lock(), "无效或过期deadline不得开始发送");

    auto fault_owner = arbiter.try_enter_cleanup();
    std::promise<void> fault_entered;
    auto fault_started = fault_entered.get_future();
    auto fault_waiter = std::async(std::launch::async, [&] {
        fault_entered.set_value();
        OutputArbiterRejection reason{};
        auto opportunity = arbiter.enter_aim_until(Clock::now() + std::chrono::milliseconds(200), &reason);
        return !opportunity.owns_lock() && reason == OutputArbiterRejection::OUTPUT_FAULT;
    });
    fault_started.wait();
    arbiter.latch_output_fault();
    fault_owner.unlock();
    require(fault_waiter.get(), "等锁期间出现故障不能在锁释放后误放行");
    require(arbiter.try_enter_cleanup().owns_lock(), "故障后仍可进入清理事务");
}

void manual_release_contracts() {
    using namespace std::chrono_literals;
    {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return ++id; }, [] { return true; });
        require(worker.start(AutoStopConfig{true, 0}), "跨快照松键回归启动");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(1); wait_for([&] { return fake->drained(); });
        // 固定交错：快照仍为W DOWN，紧接着事件流出现W UP，不能丢弃唯一释放边沿。
        fake->release_after_poll = true;
        wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), 4) != reports.end(); });
        worker.stop();
    }
    // 无目标、无允许键绑定也消费真实全松；仍有任一WASD按住时不制动。
    for (const auto masks : {std::array<std::uint8_t, 3>{1, 0, 4}, {8, 0, 2}, {2, 0, 8}, {4, 0, 1},
             {9, 8, 2}, {9, 1, 4}, {9, 0, 6}}) {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        auto arbiter = std::make_shared<AutoStopOutputArbiter>();
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, arbiter, [] { return true; }, [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 0};
        require(worker.start(config), "manual无绑定启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(masks[0]); wait_for([&] { return fake->drained(); });
        require(fake->reports().empty(), "manual按下不得提前反向");
        fake->physical(masks[1]);
        if (masks[1] != 0) {
            wait_for([&] { return fake->drained(); });
            std::this_thread::sleep_for(10ms);
            require(fake->reports().empty() && !fake->has_masks(),
                "W+D变为D或W时仍有人为方向，必须不制动");
            fake->physical(0);
        }
        wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), masks[2]) != reports.end(); });
        require(!fake->has_masks() && worker.estimated_completion_id() == 0,
            "manual不得屏蔽物理键或授予扳机估计资格");
        wait_for([&] { std::lock_guard lock(fake->mutex); return fake->cleanup_checks >= 2; });
        {
            std::lock_guard lock(fake->mutex);
            const auto reverse = std::find(fake->software.begin(), fake->software.end(), masks[2]);
            const auto index = static_cast<std::size_t>(reverse - fake->software.begin());
            require(std::count_if(fake->software.begin(), fake->software.end(), [](int mask) { return mask != 0; }) == 1,
                "manual单个释放边沿只能产生一段反向");
            require(index + 1 < fake->software.size() && fake->software[index + 1] == 0,
                "manual反向后必须发送软件零报告");
            require(fake->software_started[index + 1] >= fake->software_ack[index] + 40ms,
                "manual持有时间必须从反向ACK起算");
            require(fake->cleanup_started.back() >= fake->software_ack[index + 1] + 18ms,
                "manual归还清理必须保留释放ACK后的等待");
            require(fake->held == 0 && fake->installed_masks == 0,
                "manual仅全松后动作且不得安装物理屏蔽");
        }
        const auto before_repeat = fake->reports().size();
        fake->physical(0); wait_for([&] { return fake->drained(); });
        std::this_thread::sleep_for(70ms);
        require(fake->reports().size() == before_repeat && worker.estimated_completion_id() == 0,
            "重复相同键态不得重触发manual或迟授开火资格");
        worker.stop();
    }

    // 同批快速变化只使用最后释放的D；按住热键但没有目标也不得禁用manual。
    {
        auto fake = std::make_shared<Fake>(); fake->activation = true;
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 5};
        require(worker.start(config), "manual同批边沿启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical_batch({9, 8, 0});
        wait_for([&] { std::lock_guard lock(fake->mutex); return fake->cleanup_checks >= 2; });
        const auto reports = fake->reports();
        require(std::count(reports.begin(), reports.end(), 2) == 1 &&
            std::count_if(reports.begin(), reports.end(), [](int mask) { return mask != 0; }) == 1,
            "同批W+D到D到全松只能轻点A，不能累积已释放W的反向");
        require(!fake->has_masks() && worker.estimated_completion_id() == 0,
            "热键按住但无目标的manual仍不得安装屏蔽或授予开火");
        worker.stop();
    }
    // 取消反向的新D按下仍是下一轮合法arm，松D后应反A。
    {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 0}; config.counter_hold_ms = 200;
        require(worker.start(config), "manual取消后重新arm启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(1); wait_for([&] { return fake->drained(); });
        fake->physical(0); wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), 4) != reports.end(); });
        fake->physical(8); wait_for([&] { return fake->has_cleanup(); });
        const auto while_held = fake->reports().size();
        std::this_thread::sleep_for(20ms);
        require(fake->reports().size() == while_held, "新方向D保持期间不得反向");
        fake->physical(0);
        wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), 2) != reports.end(); });
        wait_for([&] { std::lock_guard lock(fake->mutex); return fake->cleanup_checks >= 3; });
        std::vector<int> nonzero;
        for (const int report : fake->reports()) if (report) nonzero.push_back(report);
        require(nonzero == std::vector<int>{4, 2} && !fake->has_masks(),
            "manual新按键取消后，下一次真实全松必须建立新反向且不继承旧方向");
        worker.stop();
    }
    // 等待通道时快速D按下又松开不能被最终全松快照掩盖。
    {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        auto arbiter = std::make_shared<AutoStopOutputArbiter>();
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, arbiter, [] { return true; }, [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 0};
        require(worker.start(config), "manual等待期间复核启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(1); wait_for([&] { return fake->drained(); });
        auto in_flight = arbiter->try_enter_cleanup();
        require(in_flight.owns_lock(), "manual复核必须真实占住输出通道");
        fake->physical(0); wait_for([&] { return id.load() > 0; });
        fake->physical_batch({8, 0});
        in_flight.unlock();
        wait_for([&] { return fake->drained(); });
        std::this_thread::sleep_for(100ms);
        const auto reports = fake->reports();
        require(std::none_of(reports.begin(), reports.end(), [](int mask) { return mask != 0; }),
            "等锁期间发生新DOWN到UP后，旧manual不得依据最终全松快照发出");
        require(!fake->has_masks() && worker.estimated_completion_id() == 0,
            "等待后取消不得安装屏蔽或授予开火");
        worker.stop();
    }

    // 旧显式构造、缺少focused、旧模型模式、未见健康全松都不得伪造manual。
    for (int mode = 0; mode < 7; ++mode) {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        std::atomic<std::uint64_t> id{0};
        std::function<std::uint64_t()> allocate;
        std::function<bool()> focused;
        if (mode != 0) allocate = [&] { return ++id; };
        if (mode != 1) focused = [] { return true; };
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; }, allocate, focused);
        AutoStopConfig config{mode != 4, 0}; config.use_counterpulse_timing = mode != 2;
        require(worker.start(config), "manual边界构造启动失败");
        if (mode != 3) fake->physical(0);
        if (mode != 4 && mode != 3) wait_for([&] { return fake->drained(); });
        fake->physical(mode == 6 ? 5 : 1);
        if (mode != 4) wait_for([&] { return fake->drained(); });
        if (mode == 5) { std::lock_guard lock(fake->mutex); fake->gap = true; }
        fake->physical(0);
        if (mode != 4) wait_for([&] { return fake->drained(); });
        std::this_thread::sleep_for(70ms);
        require(fake->reports().empty() && !fake->has_masks(), "manual缺资格不得输出");
        worker.stop();
    }

    // 新人工方向、暂停、失焦、事件缺口、End和共享故障应尽快停止当前反向。
    for (int ending = 0; ending < 6; ++ending) {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        auto arbiter = std::make_shared<AutoStopOutputArbiter>();
        std::atomic<std::uint64_t> id{0}; std::atomic<bool> focused{true};
        AutoStopWorker worker(fake, arbiter, [] { return true; }, [&] { return ++id; }, [&] { return focused.load(); });
        AutoStopConfig config{true, 0}; config.counter_hold_ms = 200;
        require(worker.start(config), "manual取消启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(1); wait_for([&] { return fake->drained(); });
        fake->physical(0); wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), 4) != reports.end(); });
        if (ending == 0) fake->physical(8);
        else if (ending == 1) worker.set_paused(true);
        else if (ending == 2) focused = false;
        else if (ending == 3) { std::lock_guard lock(fake->mutex); fake->gap = true; }
        else if (ending == 4) { std::lock_guard lock(fake->mutex); fake->end = true; }
        else arbiter->latch_output_fault();
        wait_for([&] { return fake->has_cleanup(); });
        require(fake->released() && worker.estimated_completion_id() == 0 && !fake->has_masks(),
            "manual取消须清掉软件键且不接管物理方向");
        {
            std::lock_guard lock(fake->mutex);
            const auto reverse = std::find(fake->software.begin(), fake->software.end(), 4);
            const auto index = static_cast<std::size_t>(reverse - fake->software.begin());
            require(fake->cleanup_at < fake->software_ack[index] + 200ms,
                "manual取消不能等完整反向时长才清理");
        }
        const auto reports = fake->reports().size();
        std::this_thread::sleep_for(30ms);
        require(fake->reports().size() == reports, "manual取消后不能补发旧边沿");
        worker.stop();
    }

    // 按键急停接管内的真实释放不排队补发manual。
    {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        std::atomic<std::uint64_t> id{0};
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 5};
        require(worker.start(config), "manual抑制启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(1); wait_for([&] { return fake->drained(); });
        worker.publish_target(Clock::now() + 1s);
        { std::lock_guard lock(fake->mutex); fake->activation = true; }
        wait_for([&] { return fake->has_masks() && fake->has_software(); });
        fake->physical(0); wait_for([&] { return fake->drained(); });
        { std::lock_guard lock(fake->mutex); fake->activation = false; }
        wait_for([&] { return fake->has_cleanup(); });
        const auto reports = fake->reports().size();
        std::this_thread::sleep_for(100ms);
        require(fake->reports().size() == reports, "按键急停接管期间的释放不得延迟补发manual");
        worker.stop();
    }
    // manual反向时，用户重新按方向并获得热键与目标资格，应先清理再安装屏蔽。
    {
        auto fake = std::make_shared<Fake>(); fake->activation = false;
        std::atomic<std::uint64_t> id{0};
        std::atomic<bool> cleaned_before_mask{true};
        AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
            [&] { return ++id; }, [] { return true; });
        AutoStopConfig config{true, 5}; config.counter_hold_ms = 200;
        require(worker.start(config), "manual抢占启动失败");
        fake->physical(0); wait_for([&] { return fake->drained(); });
        fake->physical(9); wait_for([&] { return fake->drained(); });
        fake->physical(0); wait_for([&] { const auto reports = fake->reports(); return std::find(reports.begin(), reports.end(), 6) != reports.end(); });
        { std::lock_guard lock(fake->mutex);
            fake->after_mask = [&](std::size_t) { if (!fake->has_cleanup()) cleaned_before_mask = false; };
        }
        worker.publish_target(Clock::now() + 1s);
        { std::lock_guard lock(fake->mutex); fake->activation = true; }
        fake->physical(8);
        wait_for([&] { return fake->has_masks(); });
        require(cleaned_before_mask && fake->has_cleanup(), "热键抢占manual必须先清理旧软件键再安装屏蔽");
        worker.stop();
    }
}

void ready(AutoStopWorker& worker, const std::shared_ptr<Fake>& fake) {
    fake->physical(0);
    wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
    fake->physical(1);
    wait_for([&] { return fake->drained(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
}
void manual_fire_retains_stop_contracts() {
    for (int ending = 0; ending < 8; ++ending) {
        auto fake = std::make_shared<Fake>();
        auto arbiter = std::make_shared<AutoStopOutputArbiter>();
        std::atomic<std::uint64_t> id{0}, generation{1};
        std::atomic<bool> valid{true}, focused{true}, block_context{false}, context_entered{false};
        AutoStopWorker worker(fake, arbiter, [] { return true; }, [&] { return ++id; },
            [&] { return focused.load(); }, [&] {
                if (block_context.load()) {
                    context_entered = true;
                    while (block_context.load()) std::this_thread::yield();
                }
                return AutoStopWeaponContext{true, valid.load(), generation.load(), "ak47"};
            });
        AutoStopConfig config{true, 5}; config.cycle_enabled = true;
        require(worker.start(config), "人工开火保持回归启动");
        ready(worker, fake);
        worker.publish_target(Clock::now() + std::chrono::seconds(2));
        worker.publish_tracking_target(Clock::now() + std::chrono::seconds(2));
        wait_for([&] { return worker.estimated_completion_id() != 0; });
        const auto stop_id = worker.estimated_completion_id();
        require(!worker.retain_for_manual_fire(stop_id), "没有物理左键不得声明人工接管");
        { std::lock_guard lock(fake->mutex); fake->extra_keys[1] = true; }
        require(!worker.retain_for_manual_fire(stop_id + 1), "人工接管不能冒用其他请求");
        require(worker.retain_for_manual_fire(stop_id), "物理左键可接管当前已完成急停");
        require(!worker.resume_movement(stop_id, Clock::now()), "人工保持拒绝点射归还");
        { std::lock_guard lock(fake->mutex); fake->extra_keys[1] = false; fake->activation = false; }
        worker.publish_target({}, AutoStopBlockReason::CROSSHAIR_OUTSIDE_TARGET);
        const auto deadline = Clock::now() + std::chrono::milliseconds(650);
        while (Clock::now() < deadline) {
            worker.publish_tracking_target(Clock::now() + std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        require(!fake->released() && worker.estimated_completion_id() == stop_id &&
            worker.snapshot().cycle_count == 0, "人工接管后松开两个键仍保持原急停且不归还移动");
        if (ending == 0) worker.publish_tracking_target({});
        if (ending == 1) worker.publish_tracking_target(Clock::now() - std::chrono::milliseconds(1));
        if (ending == 2) valid = false; // Runtime将换弹、死亡映射为无效武器上下文。
        if (ending == 3) ++generation;
        if (ending == 4) focused = false;
        if (ending == 5) worker.cancel(stop_id);
        if (ending == 6) { std::lock_guard lock(fake->mutex); fake->end = true; }
        if (ending == 7) {
            // 阻塞在外部上下文读取，不占worker mutex；主线程仍能发布真实期限。
            worker.publish_tracking_target(Clock::now() + std::chrono::seconds(1));
            block_context = true;
            wait_for([&] { return context_entered.load(); });
            const auto expired_at = Clock::now() + std::chrono::milliseconds(20);
            worker.publish_tracking_target(expired_at);
            std::this_thread::sleep_until(expired_at + std::chrono::milliseconds(1));
            worker.publish_tracking_target(Clock::now() + std::chrono::seconds(1));
            block_context = false;
        }
        wait_for([&] {
            if (ending >= 2) worker.publish_tracking_target(Clock::now() + std::chrono::milliseconds(50));
            return fake->released() && worker.estimated_completion_id() == 0;
        });
        require(worker.snapshot().requests == 1 && worker.snapshot().cycle_count == 0,
            "人工保持结束只清理原请求，不恢复点射周期或分配新请求");
        worker.stop();
    }
}

void trigger_idle_contracts() {
    auto fake = std::make_shared<Fake>();
    std::atomic<std::uint64_t> id{0};
    const auto test_thread = std::this_thread::get_id();
    std::atomic<int> inject{0};
    fake->before_wasd_read = [&] {
        if (std::this_thread::get_id() != test_thread) return;
        const int mode = inject.exchange(0);
        if (mode == 1) fake->physical(0);
        if (mode == 2) fake->physical_batch({2, 0});
    };
    AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
        [&] { return ++id; }, [] { return true; });
    AutoStopConfig config{true, 5};
    require(worker.start(config), "扳机空闲消费屏障回归启动");
    fake->physical(0);
    InputSnapshot input;
    wait_for([&] { fake->poll_input(input); return worker.idle_for_trigger(input); });
    // 鼠标全量序号与WASD游标不是同一时间线，不能要求两者逐报告相等。
    input.sequence += 100000;
    wait_for([&] { return worker.idle_for_trigger(input); });
    inject = 1;
    wait_for([&] { return worker.idle_for_trigger(input); });
    require(inject == 0, "重复零报告须经副游标验证且不永久阻塞原地扳机");
    inject = 2;
    bool accepted_unprocessed = false;
    wait_for([&] {
        const bool accepted = worker.idle_for_trigger(input);
        if (inject == 0) { accepted_unprocessed = accepted; return true; }
        return false;
    });
    require(!accepted_unprocessed, "新按下再松开不能被最终零键洗掉未消费事件");
    wait_for([&] { return worker.snapshot().requests == 1 && fake->released() &&
        worker.snapshot().status != AutoStopStatus::BRAKING; });
    wait_for([&] { fake->poll_input(input); return worker.idle_for_trigger(input); });
    require(worker.estimated_completion_id() == 0 && !worker.snapshot().cleanup_unknown,
        "人工松键制动及清理完成即恢复空闲，不要求独立急停完成ID");
    input.virtual_keys[0x23] = true;
    require(!worker.idle_for_trigger(input), "原地不能绕过End急停");
    input.virtual_keys[0x23] = false;
    input.virtual_keys[0x51] = true;
    require(!worker.idle_for_trigger(input), "原地不能绕过救援按键");
    input.virtual_keys[0x51] = false;
    worker.set_paused(true);
    require(!worker.idle_for_trigger(input), "原地不能绕过暂停");
    worker.stop();
    require(!worker.idle_for_trigger(input), "停止owner不再发布空闲事实");
}

int main(int argc, char** argv) {
    try {
        trigger_idle_contracts();
        if (argc == 2 && std::string_view(argv[1]) == "--manual-fire") {
            manual_fire_retains_stop_contracts();
            std::cout << "人工开火保持急停专项通过\n";
            return 0;
        }
        manual_release_contracts();
        if (argc == 2 && std::string_view(argv[1]) == "--manual-release") {
            std::cout << "手动松键急停worker专项通过\n";
            return 0;
        }
        manual_fire_retains_stop_contracts();
        bounded_aim_transaction_wait();
        const AutoStopConfig config = [] {
            AutoStopConfig legacy{true, 5};
            legacy.use_counterpulse_timing = false;
            return legacy;
        }();
        for (int cleanup_case = 0; cleanup_case < 7; ++cleanup_case) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            AutoStopConfig cycle{true, 5}; cycle.cycle_enabled = true;
            require(worker.start(cycle), "持续按键循环回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(2));
            wait_for([&] { return worker.estimated_completion_id() != 0; });
            const auto completed_id = worker.estimated_completion_id();
            require(!worker.resume_movement(completed_id + 1, Clock::now() + std::chrono::seconds(1)),
                "旧或错误请求不得归还其他周期");
            if (cleanup_case == 2) {
                std::lock_guard<std::mutex> lock(fake->mutex); fake->physical_during_cleanup = 8;
            } else if (cleanup_case != 0) {
                std::lock_guard<std::mutex> lock(fake->mutex);
                fake->cleanup_report_mode = cleanup_case == 1 ? 1 : cleanup_case - 1;
            }
            const auto deadline = Clock::now() + std::chrono::milliseconds(180);
            require(worker.resume_movement(completed_id, deadline), "点射释放确认后可投递归还");
            wait_for([&] { return fake->released(); });
            if (cleanup_case >= 2) {
                wait_for([&] { return worker.snapshot().release_required; });
                require(worker.snapshot().cycle_count == 0, "清理期间变向、缺口、无效报告、换代或时间倒退不得伪造模型承接");
            } else {
                wait_for([&] { return worker.snapshot().cycle_count == 1; });
                require(worker.snapshot().cycle_moving, "归还成功显示真实移动阶段");
                require(worker.snapshot().requests == 1, "移动期限前不得立即重接管");
                wait_for([&] { return worker.snapshot().requests == 2; });
                require(Clock::now() >= deadline, "按住方向与允许键时按期限发起下一次急停");
                require(!worker.snapshot().release_required, "成功循环不要求松键重按");
            }
            worker.stop();
        }
        for (int change = 0; change < 3; ++change) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            std::atomic<bool> changed{false};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; }, [&] {
                    const bool next = changed.load();
                    return AutoStopWeaponContext{true, !(change == 0 && next),
                        change == 2 && next ? 2u : 1u, change == 1 && next ? "m4a4" : "ak47"};
                });
            require(worker.start(AutoStopConfig{true, 5}), "GSI上下文回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(2));
            wait_for([&] { return worker.estimated_completion_id() != 0; });
            changed.store(true);
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 1; });
            require(worker.estimated_completion_id() == 0, "武器失效或变化撤销估计资格");
            require(worker.snapshot().release_required, "武器变化需松键重新触发");
            if (change == 0) {
                wait_for([&] { return worker.snapshot().block_reason == AutoStopBlockReason::WEAPON_CONTEXT; });
                changed.store(false);
            }
            fake->physical(0);
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            fake->physical(1);
            wait_for([&] { return fake->drained(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(worker.snapshot().requests == 1, "GSI恢复不能在持续按键下再次接管");
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            wait_for([&] { return !worker.snapshot().release_required; });
            { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = true; }
            wait_for([&] { return worker.snapshot().requests == 2; });
            worker.stop();
        }
        {
            auto fake = std::make_shared<Fake>(); fake->reverse_ack_delay_ms = 25;
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            AutoStopConfig h40{true, 5};
            require(worker.start(h40), "H40生产worker回归启动");
            ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().completion_ready_ns != 0; });
            const auto settling = worker.snapshot();
            require(settling.use_counterpulse_timing && settling.counter_hold_ms == 40 && settling.shot_after_release_ms == 18 &&
                settling.completion_ready_ns - settling.counter_release_ack_ns == 18000000,
                "生产快照明确H40与最终zero ACK加18ms截止");
            while (clock_ns() < settling.completion_ready_ns) {
                const auto before = clock_ns();
                const auto completion = worker.estimated_completion_id();
                if (clock_ns() < settling.completion_ready_ns && before < settling.completion_ready_ns)
                    require(completion == 0, "18ms到期前不能发布独立完成id");
                std::this_thread::yield();
            }
            wait_for([&] { return worker.estimated_completion_id() != 0; });
            { std::lock_guard<std::mutex> lock(fake->mutex);
              require(fake->software == std::vector<int>({0, 4, 0}), "H40只发zero反向zero，不注入正向移动或循环");
              require(fake->software_started[2] - fake->software_ack[1] >= std::chrono::milliseconds(40),
                  "反向ACK迟到后仍完整等待40ms才发zero"); }
            worker.stop();
            require(fake->released(), "H40停止归还全部屏蔽及软件键");
        }
        for (int revoke = 0; revoke < 3; ++revoke) {
            auto fake = std::make_shared<Fake>(); std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            auto h40 = config; h40.use_counterpulse_timing = true; h40.shot_after_release_ms = 150;
            require(worker.start(h40), "H40等待期取消回归启动"); ready(worker, fake);
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().completion_ready_ns != 0; });
            if (revoke == 0) worker.set_paused(true);
            if (revoke == 1) { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            if (revoke == 2) {
                { std::lock_guard<std::mutex> lock(fake->mutex); fake->cleanup_fails = true; }
                worker.cancel(worker.snapshot().request_id);
            }
            wait_for([&] { return worker.snapshot().canceled != 0; });
            require(worker.estimated_completion_id() == 0 && worker.snapshot().completed == 0,
                "等待期暂停/松键/清理故障不能提升为完成");
            if (revoke == 2) require(worker.snapshot().cleanup_unknown, "未知清理保留债务");
            worker.stop();
        }
        for (int revoke = 0; revoke < 3; ++revoke) {
            auto fake = std::make_shared<Fake>();
            std::atomic<std::uint64_t> id{0};
            AutoStopWorker worker(fake, std::make_shared<AutoStopOutputArbiter>(), [] { return true; },
                [&] { return ++id; }, [] { return true; });
            require(worker.start(config), "独立估计资格回归启动");
            ready(worker, fake);
            require(worker.estimated_completion_id() == 0, "尚未完成不能提供估计资格");
            worker.publish_target(Clock::now() + std::chrono::seconds(1));
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            const auto completed_id = worker.estimated_completion_id();
            require(completed_id != 0 && completed_id == worker.snapshot().request_id && !worker.snapshot().fire_permitted,
                "独立完成提供对应id，不能伪造严格观察开火资格");
            if (revoke == 0) worker.cancel(completed_id);
            if (revoke == 1) worker.set_paused(true);
            if (revoke == 2) { std::lock_guard<std::mutex> lock(fake->mutex); fake->activation = false; }
            require(worker.estimated_completion_id() == 0, "取消、暂停、允许键松开必须立即撤销估计资格");
            wait_for([&] { return fake->released(); });
            worker.stop();
            require(worker.estimated_completion_id() == 0, "停止后估计资格保持归零");
        }
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
            require(worker.estimated_completion_id() == 0, "仅屏蔽不能提供估计完成id");
            const auto masked_reports = fake->reports();
            require(!masked_reports.empty() && std::all_of(masked_reports.begin(), masked_reports.end(),
                [](int mask) { return mask == 0; }), "模型不可用时只能发送零软件报告");
            worker.publish_target({});
            for (const auto mask : {10, 8, 10, 2, 8, 10, 2, 10, 8}) {
                fake->physical(static_cast<std::uint8_t>(mask));
                wait_for([&] { return fake->drained(); });
                std::lock_guard<std::mutex> lock(fake->mutex);
                require(fake->installed_masks == 15 && fake->current_software == 0 && fake->cleanups == 0,
                    "仅屏蔽后持续乱按AD及重叠不能解除四键屏蔽");
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
            fake->physical(0);
            wait_for([&] { return fake->released() && worker.snapshot().canceled == 2; });
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::READY; });
            require(worker.estimated_completion_id() == 0 && worker.snapshot().completed == 0 &&
                !worker.snapshot().cleanup_unknown && !worker.snapshot().fire_permitted,
                "仅屏蔽全松清理后回空闲，不伪造估计完成或停稳资格");
            fake->physical(0);
            wait_for([&] { return fake->drained(); });
            require(fake->reports() == repeated_reports && worker.snapshot().requests == 2,
                "持续按许可键全松后不得补发反向或重新申请屏蔽");
            { std::lock_guard<std::mutex> lock(fake->mutex);
              require(fake->activation && fake->installed_masks == 0 && fake->current_software == 0,
                  "许可键持续按住时全松仍归还四键屏蔽"); }
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
            AutoStopConfig timed{true, 5};
            timed.counter_hold_ms = 40; timed.shot_after_release_ms = 18;
            require(worker.start(timed, 100), "worker启动失败");
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
            bool braking_move = false, settling_move = false;
            const auto acquire_until = Clock::now() + std::chrono::milliseconds(200);
            while (Clock::now() < acquire_until && (!braking_move || !settling_move)) {
                auto opportunity = arbiter->try_enter_aim();
                if (opportunity.owns_lock()) {
                    const auto reports = fake->reports();
                    const auto state = worker.snapshot();
                    if (reports.size() == 2 && reports.back() != 0) {
                        braking_move = fake->move({1, 0}).succeeded;
                    } else if (reports.size() >= 3 && reports.back() == 0 && state.status != AutoStopStatus::ESTIMATED) {
                        settling_move = fake->move({1, 0}).succeeded;
                    }
                    opportunity.unlock();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(braking_move && settling_move, "反向持有与释放后等待都必须允许Aim实际发送");
            require(fake->max_backend_calls == 1, "Aim与键盘后端事务必须串行");
            wait_for([&] { return worker.snapshot().status == AutoStopStatus::ESTIMATED; });
            worker.cancel(999);
            require(worker.estimated_completion_id() == 0, "显式请求完成不能冒充独立锁存估计资格");
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
            require(worker.snapshot().arbiter_wait_samples > 0 && fake->moves >= 2, "短事务等待与实际Aim发送证据缺失");
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
