#include "movement/movement.h"
#include "movement/movement_internal.h"
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
        config.large_ctrl_delay_ms >= 0 && config.large_ctrl_delay_ms <= 2000 &&
        config.large_ctrl_hold_ms >= 1 && config.large_ctrl_hold_ms <= 2000 &&
        config.trigger_guard_ms >= 0 && config.trigger_guard_ms <= 2000 &&
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
    Impl(std::shared_ptr<IMouseController> device, std::function<bool()> permission,
         std::function<bool(bool)> suspension)
        : mouse(std::move(device)), output_permission(std::move(permission)),
          counter_strafe_suspension(std::move(suspension)) {}
    std::shared_ptr<IMouseController> mouse;
    std::function<bool()> output_permission;
    std::function<bool(bool)> counter_strafe_suspension;
    bool counter_strafe_suspended = false, counter_strafe_ready = true;
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
    std::int64_t trigger_guard_until_ns = 0;
    Direction direction = Direction::LEFT;
    Mode action_mode = Mode::SPIN;
    Config action_config;
    Clock::time_point action_start{}, phase_start{};
    int phase = 0, phase_sent = 0, phase_counts = 0;
    bool phase_key_sent = false;
    int ctrl_phase = 0; // 0待发，1软件持有，2完成。
    Clock::time_point trigger_received{}, ctrl_release_at{};
    std::uint64_t diagnostic_action_id = 0;
    Clock::time_point diagnostic_begin{};
    static std::int64_t stamp(Clock::time_point time = Clock::now()) {
        return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    }
    const char* mode_name() const { return action_mode == Mode::SPIN ? "spin" : "long_jump"; }
    void mark(const char* stage) {
        LOG_INFO("movement", "动作 id={} type={} stage={} steady_us={} since_begin_us={} phase={} initial_side={}",
            diagnostic_action_id, mode_name(), stage, stamp(), stamp()-stamp(diagnostic_begin), phase,
            phase == -1 ? "waiting" : direction == Direction::LEFT ? "A" : "D");
    }
    void keyboard_timing(const char* operation, Clock::time_point started, const KeyboardReceipt& receipt) {
        LOG_INFO("movement", "键盘 id={} op={} start_us={} end_us={} ack_us={} disposition={} sent={}",
            diagnostic_action_id, operation, stamp(started), stamp(),
            receipt.protocol_ack_received_at == Clock::time_point{} ? 0 : stamp(receipt.protocol_ack_received_at),
            static_cast<int>(receipt.disposition), receipt.datagram_sent);
    }
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
        const auto cleanup_started = Clock::now();
        const auto receipt = mouse->cleanup_wasd_keyboard();
        keyboard_timing("cleanup", cleanup_started, receipt);
        if (!acknowledged(receipt)) {
            fail("身法键盘清理未确认，已锁定输出");
            return false;
        }
        debt = false;
        return true;
    }
    void reset_input() {
        baseline = false;
    }
    void finish(bool completed) {
        mark(completed ? "finish_requested" : "cancel_requested");
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
        // 从清理结束开始保护；期内报告仍消费，之后到达的期内旧报告也不允许重放。
        trigger_guard_until_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count() + static_cast<std::int64_t>(action_config.trigger_guard_ms) * 1000000;
        LOG_INFO("movement", "结束 id={} type={} completed={} cleaned={} fault={} steady_us={} guard_until_us={} tail_epoch={} tail_seq={}",
            diagnostic_action_id, mode_name(), completed, cleaned, fault, stamp(), trigger_guard_until_ns/1000,
            tail.epoch, tail.final_sequence);
        // 发布完成意味着尾水位已经消费。
        { std::lock_guard lock(mutex);
          if (completed && cleaned && !fault) ++view.completed; else ++view.canceled;
          view.state = fault ? State::FAULT : (action_config.trigger_guard_ms > 0 ? State::COOLDOWN : State::IDLE); }
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
    bool suspend_counter_strafe() {
        counter_strafe_suspended = true;
        return !counter_strafe_suspension || counter_strafe_suspension(true);
    }
    void resume_counter_strafe() {
        // 清理未确认时不让第二所有者接管共享键盘；Runtime停止仍可销毁两者。
        if (!counter_strafe_suspended || debt) return;
        if (counter_strafe_suspension) counter_strafe_suspension(false);
        counter_strafe_suspended = false;
    }
    bool advance_ctrl() {
        if (action_mode != Mode::LARGE_JUMP || !action_config.large_ctrl_enabled) return true;
        const auto now = Clock::now();
        if (ctrl_phase == 0 && now >= trigger_received + std::chrono::milliseconds(action_config.large_ctrl_delay_ms)) {
            debt = true;
            const auto receipt = mouse->set_left_ctrl_key(true);
            keyboard_timing("ctrl_down", now, receipt);
            if (!command_ok(receipt, "身法Left Ctrl按下未确认")) { finish(false); return false; }
            ctrl_phase = 1;
            ctrl_release_at = Clock::now() + std::chrono::milliseconds(action_config.large_ctrl_hold_ms);
        } else if (ctrl_phase == 1 && now >= ctrl_release_at) {
            const auto receipt = mouse->set_left_ctrl_key(false);
            keyboard_timing("ctrl_up", now, receipt);
            if (!command_ok(receipt, "身法Left Ctrl释放未确认")) { finish(false); return false; }
            ctrl_phase = 2;
        }
        return true;
    }
    void advance() {
        if (!active) return;
        if (!action_allowed()) { finish(false); return; }
        if (phase == -1) {
            // 旧触发不能在很久之后被新方向键激活；等待不产生设备输出。
            if (Clock::now() >= action_start + 1s) { mark("direction_timeout"); finish(false); }
            return;
        }
        if (phase == 0) {
            if (!counter_strafe_ready) {
                if (!suspend_counter_strafe()) return;
                counter_strafe_ready = true;
                mark("owner_ready");
                // 前一所有者归还耗时不能变成一笔大幅补转。
                action_start = Clock::now();
            }
            if (Clock::now() < action_start + std::chrono::milliseconds(action_config.jump_delay_ms)) return;
            // 发送前即承担潜在清理责任；超时并不意味着设备未应用。
            debt = true;
            // 暂管 W/A/D，防止用户持续按住首侧抵消自动反侧；沿用同一 owner 的清理责任。
            for (std::uint8_t key : {1, 2, 8}) {
                if (!action_allowed()) { finish(false); return; }
                const auto started = Clock::now();
                const auto receipt = mouse->set_wasd_mask(key, true);
                keyboard_timing(key == 1 ? "mask_W" : key == 2 ? "mask_A" : "mask_D", started, receipt);
                if (!command_ok(receipt, "身法屏蔽方向键未确认")) { finish(false); return; }
            }
            if (!action_allowed()) { finish(false); return; }
            mark("masks_done");
            phase = 1;
            // 使用同一个触发时刻，ACK 往返不再重新启动转动计时。
            phase_start = action_start + std::chrono::milliseconds(action_config.jump_delay_ms);
            set_state(State::TURNING);
        }
        if (!advance_ctrl()) return;
        if (!action_allowed()) { finish(false); return; }
        if (phase == 3) {
            if (ctrl_phase == 2) finish(true);
            return;
        }
        const bool left = (direction == Direction::LEFT) != (phase == 2);
        if (!phase_key_sent) {
            const auto started = Clock::now();
            const auto receipt = mouse->set_wasd_keyboard(left ? 2 : 8);
            keyboard_timing(left ? "key_A" : "key_D", started, receipt);
            if (!command_ok(receipt, "身法方向键未确认")) { finish(false); return; }
            phase_key_sent = true;
            // Long Jump每侧保留完整转动窗口，旧owner/mask/方向ACK不吞掉有效时长。
            // 旋转跳继续沿用原触发时钟；协议确认不证明游戏已经施加方向键。
            if (action_mode == Mode::LARGE_JUMP) {
                phase_start = receipt.protocol_ack_received_at != Clock::time_point{}
                    ? receipt.protocol_ack_received_at : Clock::now();
                mark("phase_clock_started");
            }
        }
        if (!action_allowed()) { finish(false); return; }
        const int duration = action_mode == Mode::SPIN ? action_config.spin_duration_ms : action_config.large_duration_ms;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - phase_start).count();
        const int target = static_cast<int>(std::llround(phase_counts * std::clamp(elapsed / duration, 0.0, 1.0)));
        const int remaining = target - phase_sent;
        if (remaining) {
            // 后端使用有符号16位位移；慢 ACK 时只补当前累计目标，不溢出协议字段。
            const int step = std::min(remaining, 32767);
            const auto move_started = Clock::now();
            const bool moved = mouse->move({left ? -step : step, 0});
            if (phase_sent == 0 || !moved) {
                LOG_INFO("movement", "首位移 id={} type={} phase={} start_us={} end_us={} dx={} ok={} since_begin_us={}",
                    diagnostic_action_id, mode_name(), phase, stamp(move_started), stamp(), left ? -step : step,
                    moved, stamp(move_started)-stamp(diagnostic_begin));
            }
            if (!moved) {
                fail("身法鼠标发送失败，已锁定输出"); finish(false); return;
            }
            phase_sent += step;
            { std::lock_guard lock(mutex); view.sent_dx += left ? -step : step; }
        }
        if (phase_sent != phase_counts || elapsed < duration) return;
        if (phase == 1 && action_mode == Mode::LARGE_JUMP) {
            mark("phase_1_end");
            phase = 2; phase_sent = 0; phase_key_sent = false;
            phase_start = Clock::now();
        } else if (action_mode == Mode::LARGE_JUMP && action_config.large_ctrl_enabled && ctrl_phase != 2) {
            // 转向结束即松软件A/D，后续只等待单次Ctrl；物理W/A/D及瞬停在全动作清理后归还。
            const auto started = Clock::now();
            const auto receipt = mouse->set_wasd_keyboard(0);
            keyboard_timing("turn_keys_up", started, receipt);
            if (!command_ok(receipt, "身法转向键释放未确认")) { finish(false); return; }
            phase = 3; set_state(State::CTRL_PENDING); mark("ctrl_pending");
        } else finish(true);
    }
    void begin(const Config& current, Mode mode, std::int64_t received_ns) {
        if (!current.enabled || fault || active || !permitted() || stopping.load()) return;
        if (mode == Mode::LARGE_JUMP && current.large_ctrl_enabled && !mouse->supports_left_ctrl_key()) {
            fail("当前后端不支持Left Ctrl，Long Jump未执行"); return;
        }
        trigger_received = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(received_ns)));
        ctrl_phase = 0;
        ++diagnostic_action_id;
        diagnostic_begin = Clock::now();
        action_config = current;
        action_mode = mode;
        action_cancel_generation = cancel_generation.load();
        action_start = Clock::now(); phase = mode == Mode::SPIN ? -1 : 0; phase_sent = 0; phase_key_sent = false;
        const double angle = mode == Mode::SPIN ? current.spin_angle_degrees : current.large_angle_degrees;
        phase_counts = static_cast<int>(std::llround(angle / (current.sensitivity * current.yaw_degrees_per_count)));
        if (mode == Mode::LARGE_JUMP) direction = Direction::LEFT;
        active = true;
        mark("begin");
        LOG_INFO("movement", "参数 id={} duration_ms={} delay_ms={} guard_ms={} counts={} report_mode={} wheel_down_positive={}",
            diagnostic_action_id, mode == Mode::SPIN ? current.spin_duration_ms : current.large_duration_ms,
            current.jump_delay_ms, current.trigger_guard_ms, phase_counts, static_cast<int>(current.report_mode), current.wheel_down_positive);
        LOG_INFO("movement", "Ctrl计划 id={} enabled={} trigger_us={} delay_ms={} hold_ms={}",
            diagnostic_action_id, mode == Mode::LARGE_JUMP && current.large_ctrl_enabled,
            stamp(trigger_received), current.large_ctrl_delay_ms, current.large_ctrl_hold_ms);
        counter_strafe_ready = suspend_counter_strafe();
        mark(counter_strafe_ready ? "owner_ready" : "owner_wait");
        { std::lock_guard lock(mutex); view.state = phase == -1 ? State::WAITING_DIRECTION : State::DELAY; view.direction = direction; view.sent_dx = 0; ++view.started; }
    }
    void observe(const InputReportEvent& event, const Config& current, bool allow_begin) {
        if (baseline && event.epoch == previous.epoch && event.sequence <= previous.sequence) return;
        if (!event.state_valid) { reset_input(); if (active) finish(false); return; }
        if (!baseline || event.epoch != previous.epoch) {
            if (active) finish(false);
            reset_input(); previous = event; baseline = true; return;
        }
        int wheel = 0;
        if (current.report_mode == ReportMode::CUMULATIVE) {
            wheel = wrapped_delta(event.raw_wheel, previous.raw_wheel);
        } else if (event.raw_x != previous.raw_x || event.raw_y != previous.raw_y ||
                   event.raw_wheel != previous.raw_wheel) {
            // 键盘包可能复带鼠标字段；重复字段不当作新的相对运动或滚轮。
            wheel = event.raw_wheel;
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
        const auto previous_wheel = previous.raw_wheel;
        previous = event;
        // 只接受及时到达的物理 monitor 报告，旧队列不能替新动作选侧。
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const bool fresh = event.received_at_steady_ns > 0 && event.received_at_steady_ns <= now_ns &&
            now_ns - event.received_at_steady_ns <= 100000000;
        if (wheel != 0 || spin || large) {
            const char* reason = !fresh ? "stale" : active ? "active" :
                !detail::trigger_guard_finished(event.received_at_steady_ns, now_ns, trigger_guard_until_ns) ? "guard" :
                !allow_begin ? "batch_or_fault" : !current.enabled ? "disabled" : spin == large ? "no_unique_trigger" :
                stopping.load() ? "stopping" : "candidate";
            LOG_INFO("movement", "触发 last_id={} epoch={} seq={} received_us={} observed_us={} raw_prev={} raw={} delta={} spin={} long_jump={} reason={} guard_until_us={} wasd={}",
                diagnostic_action_id, event.epoch, event.sequence, event.received_at_steady_ns/1000, now_ns/1000,
                previous_wheel, event.raw_wheel, wheel, spin, large, reason, trigger_guard_until_ns/1000, event.wasd_mask);
        }
        if (!fresh) { if (active && phase == -1) finish(false); return; }
        if (!active && !detail::trigger_guard_finished(event.received_at_steady_ns, now_ns, trigger_guard_until_ns)) return;
        if (!active && allow_begin && spin != large)
            begin(current, spin ? Mode::SPIN : Mode::LARGE_JUMP, event.received_at_steady_ns);
        if (!active || phase != -1) return;
        if (Clock::now() >= action_start + 1s) { mark("direction_timeout"); finish(false); return; }
        const bool left = key_down(event, 'A'), right = key_down(event, 'D');
        if (left == right) return;
        // 本动作尚未发软件方向键；选侧后整个周期不再读取方向，避免自身输出反馈。
        direction = left ? Direction::LEFT : Direction::RIGHT;
        action_start = Clock::now(); phase = 0;
        LOG_INFO("movement", "选侧 id={} source=monitor_single_AD epoch={} seq={} received_us={} side={}",
            diagnostic_action_id, event.epoch, event.sequence, event.received_at_steady_ns/1000,
            direction == Direction::LEFT ? "A" : "D");
        { std::lock_guard lock(mutex); view.direction = direction; view.state = State::DELAY; }

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
                if (!active && !fault && std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch()).count() >= trigger_guard_until_ns) {
                    resume_counter_strafe();
                    std::lock_guard lock(mutex);
                    if (view.state == State::COOLDOWN) view.state = State::IDLE;
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, 1ms);
            }
            if (active) finish(false);
            else if (debt) cleanup();
            resume_counter_strafe();
        } catch (...) {
            try { fail("身法执行异常"); active = false; cleanup(); resume_counter_strafe(); } catch (...) {}
        }
        mouse->set_movement_report_subscription(false);
        std::lock_guard lock(mutex);
        view.state = fault ? State::FAULT : State::STOPPED;
    }
};

Worker::Worker(std::shared_ptr<IMouseController> mouse, std::function<bool()> permission,
               std::function<bool(bool)> counter_strafe_suspension)
    : impl_(std::make_unique<Impl>(std::move(mouse), std::move(permission),
                                  std::move(counter_strafe_suspension))) {}
Worker::~Worker() { stop(); }
bool Worker::start(const Config& config) noexcept {
    try {
        if (!valid_config(config) || !impl_->mouse || !impl_->mouse->supports_wasd_keyboard() ||
            impl_->thread.joinable() || impl_->debt) return false;
        if (!impl_->mouse->set_movement_report_subscription(true)) return false;
        { std::lock_guard lock(impl_->mutex); impl_->config = config; impl_->view = {}; impl_->view.state = State::IDLE; ++impl_->config_revision; }
        impl_->stopping.store(false); impl_->fault = false; impl_->active = false;
        impl_->cursor = {}; impl_->reset_input(); impl_->trigger_guard_until_ns = 0;
        Log::register_module("movement", LogLevel::INFO);
        LOG_INFO("movement", "诊断会话 steady_us={} revision=r6_long_ctrl", Impl::stamp());
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
