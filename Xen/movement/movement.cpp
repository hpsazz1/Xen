#include "movement/movement.h"
#include "mouse/input_internal.h"
#include "log/log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace movement {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
bool acknowledged(const KeyboardReceipt& receipt) noexcept {
    return receipt.disposition == KeyboardDisposition::ACKNOWLEDGED;
}
bool supported_trigger_key(int key) noexcept {
    static const auto supported = [] {
        std::array<bool, 256> keys{};
        keys[0] = true;
        for (int usage = 0; usage < 256; ++usage)
            keys[mouse::detail::hid_usage_to_virtual_key(static_cast<std::uint8_t>(usage))] = true;
        for (int value : {1, 2, 4, 5, 6, 0x10, 0x11, 0x12}) keys[value] = true;
        return keys;
    }();
    return key >= 0 && key < 256 && supported[static_cast<std::size_t>(key)];
}
int wrapped_delta(std::int16_t value, std::int16_t previous) noexcept {
    const auto delta = static_cast<std::uint16_t>(static_cast<unsigned>(
        static_cast<std::uint16_t>(value)) - static_cast<std::uint16_t>(previous));
    return delta < 32768 ? static_cast<int>(delta) : static_cast<int>(delta) - 65536;
}
}
bool valid_config(const Config& config) noexcept {
    const auto angle_valid = [](double value) { return std::isfinite(value) && value > 0 && value <= 360; };
    const double scale = config.sensitivity * config.yaw_degrees_per_count;
    const auto trigger_valid = [](Trigger trigger, int key) {
        return (trigger == Trigger::WHEEL_DOWN || trigger == Trigger::WHEEL_UP || trigger == Trigger::KEY) &&
            key >= 0 && key <= 255 && (trigger != Trigger::KEY || supported_trigger_key(key));
    };
    const bool collision = config.spin_enabled && config.large_enabled &&
        config.spin_trigger == config.large_trigger &&
        (config.spin_trigger != Trigger::KEY ||
         (config.spin_virtual_key != 0 && config.spin_virtual_key == config.large_virtual_key));
    return !collision && trigger_valid(config.spin_trigger, config.spin_virtual_key) &&
        trigger_valid(config.large_trigger, config.large_virtual_key) &&
        (config.report_mode == ReportMode::CUMULATIVE || config.report_mode == ReportMode::RELATIVE_DELTA) &&
        config.jump_delay_ms >= 0 && config.jump_delay_ms <= 2000 &&
        config.spin_duration_ms >= 1 && config.spin_duration_ms <= 5000 &&
        config.large_duration_ms >= 1 && config.large_duration_ms <= 5000 &&
        angle_valid(config.spin_angle_degrees) && angle_valid(config.large_angle_degrees) &&
        std::isfinite(config.sensitivity) && config.sensitivity > 0 &&
        std::isfinite(config.yaw_degrees_per_count) && config.yaw_degrees_per_count > 0 &&
        std::isfinite(scale) && scale > 0 &&
        std::max(config.spin_angle_degrees, config.large_angle_degrees) / scale <= 1000000;
}

class Worker::Impl {
public:
    Impl(std::shared_ptr<IMouseController> device, std::function<bool()> permission)
        : mouse(std::move(device)), output_permission(std::move(permission)) {}
    std::shared_ptr<IMouseController> mouse;
    std::function<bool()> output_permission;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    Config config;
    Snapshot view;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> cancel_generation{0};
    std::uint64_t config_revision = 0;
    InputReportCursor cursor;
    bool baseline = false, debt = false, active = false, fault = false;
    InputReportEvent previous;
    std::int64_t direction_observed_ns = 0;
    Direction direction = Direction::LEFT;
    Mode action_mode = Mode::SPIN;
    Config action_config;
    Clock::time_point action_start{}, phase_start{};
    int phase = 0, phase_sent = 0, phase_counts = 0;
    bool phase_key_sent = false;
    std::uint64_t action_cancel_generation = 0;

    void set_state(State state) { std::lock_guard lock(mutex); view.state = state; }
    bool permitted() noexcept {
        try { return output_permission && output_permission(); } catch (...) { return false; }
    }
    void fail(const char* error) {
        fault = true;
        { std::lock_guard lock(mutex); view.state = State::FAULT; view.error = error; }
        LOG_ERROR("movement", "{}", error);
    }
    bool cleanup() {
        if (!debt) return true;
        set_state(State::CLEANING);
        // 一次有界尝试；未知 ACK 保留责任，禁止继续下一动作。
        if (!acknowledged(mouse->cleanup_wasd_keyboard())) {
            fail("身法键盘清理未确认，已锁定输出");
            return false;
        }
        debt = false;
        return true;
    }
    void reset_input() {
        baseline = false;
        direction_observed_ns = 0;
    }
    void finish(bool completed) {
        active = false;
        const bool cleaned = cleanup();
        // 丢弃执行和清理期间的尾水位，不将延迟滚轮排队为第二个动作。
        InputReportBatch tail;
        if (!mouse->read_movement_reports(cursor, tail)) fail("身法输入尾水位读取失败");
        cursor.epoch = tail.epoch;
        cursor.sequence = std::max(cursor.sequence, tail.final_sequence);
        if (tail.count && tail.events[tail.count - 1].state_valid &&
            tail.events[tail.count - 1].sequence == tail.final_sequence) {
            previous = tail.events[tail.count - 1]; baseline = true;
        } else if (tail.count || tail.gap || tail.dropped_count) baseline = false;
        direction_observed_ns = 0;
        // 发布完成意味着尾水位已经消费；否则用户看到完成后立即再滚会被清尾吞掉。
        { std::lock_guard lock(mutex);
          if (completed && cleaned && !fault) ++view.completed; else ++view.canceled;
          view.state = fault ? State::FAULT : State::IDLE; }
    }
    bool action_allowed() {
        bool enabled;
        { std::lock_guard lock(mutex);
          enabled = config.enabled && (action_mode == Mode::SPIN ? config.spin_enabled : config.large_enabled); }
        return !stopping.load() && enabled && permitted() &&
            action_cancel_generation == cancel_generation.load();
    }
    bool command_ok(const KeyboardReceipt& result, const char* error) {
        if (acknowledged(result)) return true;
        fail(error);
        return false;
    }
    void advance() {
        if (!active) return;
        if (!action_allowed()) { finish(false); return; }
        if (phase == 0) {
            if (Clock::now() < action_start + std::chrono::milliseconds(action_config.jump_delay_ms)) return;
            // 发送前即承担潜在清理责任；超时并不意味着设备未应用。
            debt = true;
            if (!command_ok(mouse->set_wasd_mask(1, true), "身法屏蔽 W 未确认")) { finish(false); return; }
            if (!action_allowed()) { finish(false); return; }
            phase = 1;
            // 使用同一个触发时刻，ACK 往返不再重新启动转动计时。
            phase_start = action_start + std::chrono::milliseconds(action_config.jump_delay_ms);
            set_state(State::TURNING);
        }
        const bool left = (direction == Direction::LEFT) != (phase == 2);
        if (!phase_key_sent) {
            if (!command_ok(mouse->set_wasd_keyboard(left ? 2 : 8), "身法方向键未确认")) { finish(false); return; }
            phase_key_sent = true;
        }
        if (!action_allowed()) { finish(false); return; }
        const int duration = action_mode == Mode::SPIN ? action_config.spin_duration_ms : action_config.large_duration_ms;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - phase_start).count();
        const int target = static_cast<int>(std::llround(phase_counts * std::clamp(elapsed / duration, 0.0, 1.0)));
        const int remaining = target - phase_sent;
        if (remaining) {
            // 后端使用有符号16位位移；慢 ACK 时只补当前累计目标，不溢出协议字段。
            const int step = std::min(remaining, 32767);
            if (!mouse->move({left ? -step : step, 0})) {
                fail("身法鼠标发送失败，已锁定输出"); finish(false); return;
            }
            phase_sent += step;
            { std::lock_guard lock(mutex); view.sent_dx += left ? -step : step; }
        }
        if (phase_sent != phase_counts || elapsed < duration) return;
        if (phase == 1 && action_mode == Mode::LARGE_JUMP) {
            phase = 2; phase_sent = 0; phase_key_sent = false;
            phase_start = Clock::now();
        } else finish(true);
    }
    void begin(const Config& current, Mode mode) {
        if (!current.enabled || fault || active || !permitted() || stopping.load()) return;
        action_config = current;
        action_mode = mode;
        action_cancel_generation = cancel_generation.load();
        action_start = Clock::now(); phase = 0; phase_sent = 0; phase_key_sent = false;
        const double angle = mode == Mode::SPIN ? current.spin_angle_degrees : current.large_angle_degrees;
        phase_counts = static_cast<int>(std::llround(angle / (current.sensitivity * current.yaw_degrees_per_count)));
        active = true;
        { std::lock_guard lock(mutex); view.state = State::DELAY; view.direction = direction; view.sent_dx = 0; ++view.started; }
    }
    void observe(const InputReportEvent& event, const Config& current, bool allow_begin) {
        if (baseline && event.epoch == previous.epoch && event.sequence <= previous.sequence) return;
        if (!event.state_valid) { reset_input(); if (active) finish(false); return; }
        if (!baseline || event.epoch != previous.epoch) {
            if (active) finish(false);
            reset_input(); previous = event; baseline = true; return;
        }
        int dx = 0, wheel = 0;
        if (current.report_mode == ReportMode::CUMULATIVE) {
            dx = wrapped_delta(event.raw_x, previous.raw_x);
            wheel = wrapped_delta(event.raw_wheel, previous.raw_wheel);
        } else if (event.raw_x != previous.raw_x || event.raw_y != previous.raw_y ||
                   event.raw_wheel != previous.raw_wheel) {
            // 键盘包可能复带鼠标字段；重复字段不当作新的相对运动或滚轮。
            dx = event.raw_x; wheel = event.raw_wheel;
        }
        const auto key_down = [](const InputReportEvent& report, int key) {
            if (key <= 0 || key > 255) return false;
            std::array<bool, 256> keys{};
            mouse::detail::apply_hid_keyboard_report(report.keyboard_modifiers,
                report.keyboard_usages.data(), report.keyboard_usages.size(), keys);
            keys[0x01] = (report.mouse_buttons & 1) != 0;
            keys[0x02] = (report.mouse_buttons & 2) != 0;
            keys[0x04] = (report.mouse_buttons & 4) != 0;
            keys[0x05] = (report.mouse_buttons & 8) != 0;
            keys[0x06] = (report.mouse_buttons & 16) != 0;
            return keys[static_cast<std::size_t>(key)];
        };
        const auto triggered = [&](Trigger trigger, int key) {
            if (trigger == Trigger::KEY) return key_down(event, key) && !key_down(previous, key);
            const bool down = current.wheel_down_positive ? wheel > 0 : wheel < 0;
            return wheel != 0 && (trigger == Trigger::WHEEL_DOWN ? down : !down);
        };
        const bool spin = current.spin_enabled && triggered(current.spin_trigger, current.spin_virtual_key);
        const bool large = current.large_enabled && triggered(current.large_trigger, current.large_virtual_key);
        previous = event;
        if (active) return;
        // 最近一次实际转向优先；不让早先的大幅移动压过起跳瞬间的小幅反向。
        if (dx) {
            direction = dx < 0 ? Direction::LEFT : Direction::RIGHT;
            direction_observed_ns = event.received_at_steady_ns;
        }
        const bool fresh_direction = direction_observed_ns != 0 &&
            event.received_at_steady_ns >= direction_observed_ns &&
            event.received_at_steady_ns - direction_observed_ns <= 100000000;
        if (allow_begin && fresh_direction && spin != large)
            begin(current, spin ? Mode::SPIN : Mode::LARGE_JUMP);
    }
    void run() noexcept {
        try {
            std::uint64_t observed_revision = 0;
            ReportMode observed_mode = ReportMode::CUMULATIVE;
            while (!stopping.load()) {
                Config current;
                { std::lock_guard lock(mutex);
                  current = config;
                  if (!active && observed_revision != config_revision) {
                      if (observed_mode != current.report_mode) reset_input();
                      observed_mode = current.report_mode; observed_revision = config_revision;
                  } }
                if (!valid_config(current)) {
                    if (!fault) fail("身法配置非法");
                    if (active) finish(false);
                }
                InputReportBatch batch;
                if (!mouse->read_movement_reports(cursor, batch) || !batch.subscribed || batch.gap || batch.dropped_count ||
                    batch.status == InputMonitorStatus::FAILURE || batch.status == InputMonitorStatus::CLOSED) {
                    reset_input();
                    if (active) finish(false);
                } else {
                    bool consumed_action = active;
                    for (std::size_t i = 0; i < batch.count; ++i) {
                        observe(batch.events[i], active ? action_config : current, !consumed_action && !fault && valid_config(current));
                        consumed_action = consumed_action || active;
                    }
                }
                advance();
                std::unique_lock lock(mutex);
                wake.wait_for(lock, 1ms);
            }
            if (active) finish(false);
            else if (debt) cleanup();
        } catch (...) {
            try { fail("身法执行异常"); active = false; cleanup(); } catch (...) {}
        }
        mouse->set_movement_report_subscription(false);
        std::lock_guard lock(mutex);
        view.state = fault ? State::FAULT : State::STOPPED;
    }
};

Worker::Worker(std::shared_ptr<IMouseController> mouse, std::function<bool()> permission)
    : impl_(std::make_unique<Impl>(std::move(mouse), std::move(permission))) {}
Worker::~Worker() { stop(); }
bool Worker::start(const Config& config) noexcept {
    try {
        if (!valid_config(config) || !impl_->mouse || !impl_->mouse->supports_wasd_keyboard() ||
            impl_->thread.joinable() || impl_->debt) return false;
        if (!impl_->mouse->set_movement_report_subscription(true)) return false;
        { std::lock_guard lock(impl_->mutex); impl_->config = config; impl_->view = {}; impl_->view.state = State::IDLE; ++impl_->config_revision; }
        impl_->stopping.store(false); impl_->fault = false; impl_->active = false;
        impl_->cursor = {}; impl_->reset_input();
        Log::register_module("movement", LogLevel::INFO);
        impl_->thread = std::thread([this] { impl_->run(); });
        return true;
    } catch (...) { if (impl_->mouse) impl_->mouse->set_movement_report_subscription(false); return false; }
}
void Worker::configure(const Config& config) noexcept {
    try { std::lock_guard lock(impl_->mutex); impl_->config = config; ++impl_->config_revision; impl_->wake.notify_all(); } catch (...) {}
}
void Worker::cancel() noexcept { impl_->cancel_generation.fetch_add(1); impl_->wake.notify_all(); }
void Worker::stop() noexcept {
    impl_->stopping.store(true); impl_->wake.notify_all();
    try { if (impl_->thread.joinable()) impl_->thread.join(); } catch (...) {}
}
Snapshot Worker::snapshot() const noexcept {
    try { std::lock_guard lock(impl_->mutex); return impl_->view; } catch (...) { Snapshot result; result.state = State::FAULT; return result; }
}
}
