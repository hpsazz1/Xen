#include "movement/movement.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void wait_for(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error("身法专项等待超时");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class Fake final : public IMouseController {
public:
    mutable std::mutex mutex;
    std::vector<InputReportEvent> reports;
    std::vector<int> moves, software;
    std::vector<bool> masks;
    std::uint64_t sequence = 0, epoch = 1;
    std::atomic<int> cleanups{0};
    int keyboard_delay_ms = 0;
    std::atomic<bool> fail_move{false}, fail_cleanup{false}, fail_mask{false};
    bool subscribed = false;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard lock(mutex);
        moves.push_back(command.dx_counts);
        MouseMoveReceipt result;
        result.succeeded = !fail_move.load() && command.dy_counts == 0;
        return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        if (keyboard_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(keyboard_delay_ms));
        std::lock_guard lock(mutex); software.push_back(mask);
        return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        std::lock_guard lock(mutex); masks.push_back(masked);
        return {key == 1 && !fail_mask.load() ? KeyboardDisposition::ACKNOWLEDGED : KeyboardDisposition::APPLICATION_UNKNOWN};
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups;
        return {fail_cleanup.load() ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED};
    }
    bool set_movement_report_subscription(bool value) noexcept override {
        std::lock_guard lock(mutex); subscribed = value; return true;
    }
    bool read_movement_reports(InputReportCursor& cursor, InputReportBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        batch = {}; batch.subscribed = subscribed; batch.epoch = epoch; batch.final_sequence = sequence;
        batch.status = InputMonitorStatus::READY;
        if (cursor.epoch != epoch) cursor = {epoch, 0};
        for (const auto& event : reports) {
            if (event.epoch == epoch && event.sequence > cursor.sequence && batch.count < batch.events.size()) {
                batch.events[batch.count++] = event; cursor.sequence = event.sequence;
            }
        }
        return true;
    }
    void report(int x, int wheel, bool valid = true, std::uint8_t usage = 0) {
        std::lock_guard lock(mutex);
        InputReportEvent event;
        event.epoch = epoch; event.sequence = ++sequence; event.state_valid = valid;
        event.received_at_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        event.keyboard_usages[0] = usage;
        event.raw_x = static_cast<std::int16_t>(x); event.raw_wheel = static_cast<std::int16_t>(wheel);
        reports.push_back(event);
    }
    void new_epoch() { std::lock_guard lock(mutex); ++epoch; sequence = 0; reports.clear(); }
    int total() const { std::lock_guard lock(mutex); int result = 0; for (int value : moves) result += value; return result; }
    std::vector<int> keys() const { std::lock_guard lock(mutex); return software; }
};
movement::Config config() {
    movement::Config result;
    result.enabled = true; result.spin_trigger = movement::Trigger::WHEEL_DOWN; result.jump_delay_ms = 0; result.spin_duration_ms = 12;
    result.large_duration_ms = 12; result.spin_angle_degrees = 1.0; result.large_angle_degrees = 1.0;
    result.sensitivity = 1.0; result.yaw_degrees_per_count = 0.01;
    return result;
}
void trigger(const std::shared_ptr<Fake>& fake, int x = -10) {
    fake->report(0, 0); fake->report(x, 1);
}
void spin_and_symmetric_large() {
    for (auto mode : {movement::Mode::SPIN, movement::Mode::LARGE_JUMP}) {
        for (int sign : {-1, 1}) {
            auto fake = std::make_shared<Fake>();
            movement::Worker worker(fake, [] { return true; });
            auto value = config(); value.spin_enabled = mode == movement::Mode::SPIN; value.large_enabled = !value.spin_enabled;
            require(worker.start(value), "启动失败"); trigger(fake, sign * 10);
            wait_for([&] { return worker.snapshot().completed == 1; });
            require(fake->total() == (mode == movement::Mode::SPIN ? sign * 100 : 0), "角度累计或大跳对称错误");
            const auto keys = fake->keys();
            require(keys.front() == (sign < 0 ? 2 : 8), "左右方向键映射错误");
            if (mode == movement::Mode::LARGE_JUMP)
                require(keys.size() == 2 && keys[1] == (sign < 0 ? 8 : 2), "大跳第二段未反向");
            require(fake->cleanups == 1, "动作完成未清理");
            worker.stop(); require(fake->cleanups == 1, "无债务停止重复发送清理");
        }
    }
}
void disabled_has_no_output() {
    auto fake = std::make_shared<Fake>();
    movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.enabled = false;
    require(worker.start(value), "禁用启动失败"); trigger(fake);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); worker.stop();
    require(fake->cleanups == 0 && fake->keys().empty() && fake->total() == 0, "禁用功能产生输出");
}
void cancel_and_permission() {
    for (bool permission_cancel : {false, true}) {
        auto fake = std::make_shared<Fake>(); std::atomic<bool> allowed{true};
        movement::Worker worker(fake, [&] { return allowed.load(); });
        auto value = config(); value.spin_duration_ms = 300;
        require(worker.start(value), "启动失败"); trigger(fake);
        wait_for([&] { return !fake->keys().empty(); });
        if (permission_cancel) allowed = false; else worker.cancel();
        wait_for([&] { return worker.snapshot().canceled == 1; });
        require(fake->cleanups == 1, "取消未清理"); worker.stop();
    }
}
void fault_latches() {
    auto fake = std::make_shared<Fake>(); fake->fail_mask = true; fake->fail_cleanup = true;
    movement::Worker worker(fake, [] { return true; });
    require(worker.start(config()), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::FAULT; });
    fake->report(-20, 2); fake->report(-30, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(worker.snapshot().started == 1 && !worker.snapshot().error.empty(), "故障未闭锁");
    worker.stop(); require(!worker.start(config()), "清理未确认仍重新启动");
}
void wrap_and_relative_dedup() {
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        require(worker.start(config()), "启动失败");
        fake->report(32760, 32767); fake->report(-32760, -32768);
        wait_for([&] { return worker.snapshot().completed == 1; });
        require(fake->total() == 100, "signed16回绕方向错误"); worker.stop();
    }
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        auto value = config(); value.report_mode = movement::ReportMode::RELATIVE_DELTA;
        require(worker.start(value), "启动失败"); trigger(fake);
        wait_for([&] { return worker.snapshot().completed == 1; });
        for (int i = 0; i < 5; ++i) fake->report(-10, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(worker.snapshot().started == 1, "相对复带包重复触发");
        fake->report(0, 0); fake->report(10, 1);
        wait_for([&] { return worker.snapshot().completed == 2; }); worker.stop();
    }
}
void running_config_is_frozen() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_duration_ms = 60;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::TURNING; });
    value.spin_angle_degrees = 2; worker.configure(value);
    fake->report(-20, 2); fake->report(-30, 3);
    wait_for([&] { return worker.snapshot().completed == 1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(fake->total() == -100 && worker.snapshot().started == 1, "运行配置未冻结或执行中滚轮排队");
    fake->report(-30, 3); fake->report(-40, 4);
    wait_for([&] { return worker.snapshot().completed == 2; });
    require(fake->total() == -300, "新动作未使用新参数"); worker.stop();
}
void epoch_and_delay_cancel() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.jump_delay_ms = 200;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::DELAY; });
    worker.cancel();
    wait_for([&] { return worker.snapshot().canceled == 1; });
    require(fake->cleanups == 0 && fake->keys().empty(), "延迟期取消产生无主清理");
    fake->new_epoch(); fake->report(1000, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(worker.snapshot().started == 1, "新epoch首包被误作滚轮");
    fake->report(1010, 1001);
    wait_for([&] { return worker.snapshot().started == 2; });
    fake->report(1020, 1002, false);
    wait_for([&] { return worker.snapshot().canceled == 2; });
    require(fake->keys().empty(), "失效输入仍执行方向键"); worker.stop();
}
void disabling_active_item_cleans_up() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_duration_ms = 500;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return !fake->keys().empty(); });
    value.spin_enabled = false; worker.configure(value);
    wait_for([&] { return worker.snapshot().canceled == 1; });
    require(fake->cleanups == 1, "关闭单项未立即取消并清理"); worker.stop();
}
void shared_trigger_clock() {
    require(movement::Config{}.jump_delay_ms == 0, "默认仍起跳后等待");
    auto fake = std::make_shared<Fake>(); fake->keyboard_delay_ms = 30;
    movement::Worker worker(fake, [] { return true; });
    require(worker.start(config()), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().completed == 1; });
    { std::lock_guard lock(fake->mutex);
      require(fake->moves.size() == 1 && fake->moves[0] == -100,
          "方向键ACK之后重新起算鼠标计时，额外延长动作"); }
    worker.stop();
}
void latest_direction_and_independent_triggers() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_trigger = movement::Trigger::WHEEL_UP;
    value.large_enabled = true;
    require(worker.start(value), "双项启动失败");
    fake->report(0, 0); fake->report(-100, 0); fake->report(-99, -1);
    wait_for([&] { return worker.snapshot().completed == 1; });
    require(fake->total() == 100 && fake->keys().front() == 8, "起跳瞬间反向被旧方向覆盖或上滚未触发旋转跳");
    fake->report(-109, 0);
    wait_for([&] { return worker.snapshot().completed == 2; });
    auto keys = fake->keys();
    require(keys.size() == 3 && keys[1] == 2 && keys[2] == 8 && fake->total() == 100,
        "双项并存下滚未独立触发大跳");
    worker.stop();
}
void key_trigger_and_stale_direction() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_trigger = movement::Trigger::KEY; value.spin_virtual_key = 0x20;
    require(worker.start(value), "按键模式启动失败");
    fake->report(0, 0); fake->report(-1, 0, true, 0x2c);
    wait_for([&] { return worker.snapshot().completed == 1; });
    fake->report(-2, 0, true, 0x2c);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(worker.snapshot().started == 1, "按键长按重复触发");
    fake->report(-2, 0); fake->report(-3, 0, true, 0x2c);
    wait_for([&] { return worker.snapshot().completed == 2; });
    worker.stop();
    auto stale = std::make_shared<Fake>(); movement::Worker other(stale, [] { return true; });
    require(other.start(config()), "启动失败"); stale->report(0,0); stale->report(-10,0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); stale->report(-10,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(other.snapshot().started == 0, "陈旧方向或默认左侧触发动作"); other.stop();
}
void mouse_failure_and_polarity() {
    {
        auto fake = std::make_shared<Fake>(); fake->fail_move = true;
        movement::Worker worker(fake, [] { return true; });
        require(worker.start(config()), "启动失败"); trigger(fake);
        wait_for([&] { return worker.snapshot().canceled == 1; });
        require(worker.snapshot().state == movement::State::FAULT && fake->cleanups == 1,
            "鼠标失败未清理并闭锁"); worker.stop();
    }
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        auto value = config(); value.wheel_down_positive = false;
        require(worker.start(value), "启动失败");
        fake->report(0, 0); fake->report(10, -1);
        wait_for([&] { return worker.snapshot().completed == 1; });
        fake->report(20, -2);
        wait_for([&] { return worker.snapshot().completed == 2; });
        require(fake->total() == 200, "滚轮负向或动作结束后首个滚轮被丢弃"); worker.stop();
    }
}
}
int main() {
    try {
        require(movement::valid_config(movement::Config{}), "默认配置非法");
        auto invalid = config(); invalid.sensitivity = 0;
        require(!movement::valid_config(invalid), "零灵敏度未拒绝");
        auto unsupported = config(); unsupported.spin_trigger = movement::Trigger::KEY; unsupported.spin_virtual_key = 0x60;
        require(!movement::valid_config(unsupported), "未识别的小键盘键被接受为触发");
        spin_and_symmetric_large(); disabled_has_no_output(); cancel_and_permission();
        fault_latches(); wrap_and_relative_dedup(); running_config_is_frozen(); epoch_and_delay_cancel();
        mouse_failure_and_polarity(); shared_trigger_clock(); disabling_active_item_cleans_up(); latest_direction_and_independent_triggers(); key_trigger_and_stale_direction();
        std::cout << "身法专项通过\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
