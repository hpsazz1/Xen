#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "keyboard/keyboard.h"
#include "keyboard/keyboard_internal.h"

#include "log/log.h"

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <memory>
#include <utility>

struct KeyboardListener::Impl {
    explicit Impl(const KeyboardConfig& value,
                  std::shared_ptr<IMouseController> device_value = {})
        : config(value), device(std::move(device_value)) {}

    KeyboardConfig config;
    std::shared_ptr<IMouseController> device;
    KeyboardStatus status = KeyboardStatus::CLOSED;
    keyboard::detail::KeyboardEventState event_state;
    InputMonitorStatus monitor_status = InputMonitorStatus::CLOSED;
    bool input_healthy = false;
    bool have_input_fact = false;
    std::uint64_t last_input_sequence = 0;
};

KeyboardListener::KeyboardListener(const KeyboardConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

KeyboardListener::KeyboardListener(
        const KeyboardConfig& config,
        std::shared_ptr<IMouseController> device)
    : impl_(std::make_unique<Impl>(config, std::move(device))) {}

KeyboardListener::~KeyboardListener() = default;

KeyboardListener::KeyboardListener(KeyboardListener&& other) noexcept
    = default;

KeyboardListener& KeyboardListener::operator=(KeyboardListener&& other) noexcept = default;

bool KeyboardListener::open() noexcept {
    if (!impl_ || !valid_keyboard_config(impl_->config)) {
        if (impl_) impl_->status = KeyboardStatus::FAILURE;
        return false;
    }
    Log::register_module("keyboard", LogLevel::INFO);
    impl_->event_state = {};
    impl_->monitor_status = impl_->device
        ? InputMonitorStatus::WAITING
        : InputMonitorStatus::UNVERIFIED;
    impl_->input_healthy = false;
    impl_->have_input_fact = false;
    impl_->last_input_sequence = 0;
    impl_->status = KeyboardStatus::READY;
    LOG_INFO("keyboard", "全局按键轮询已启用: hold_keys={}, emergency_keys={}, toggle_keys={}",
             impl_->config.aim_hold_virtual_keys.size(),
             impl_->config.emergency_virtual_keys.size(),
             impl_->config.runtime_toggle_virtual_keys.size());
    return true;
}

KeyboardPollResult KeyboardListener::poll() noexcept {
    KeyboardPollResult result;
    if (!impl_) {
        result.monitor_status = InputMonitorStatus::FAILURE;
        return result;
    }
    const auto publish_state = [&]() noexcept {
        result.monitor_status = impl_->monitor_status;
        result.input_healthy = impl_->input_healthy;
        result.sequence = impl_->have_input_fact
            ? impl_->last_input_sequence : 0;
    };
    publish_state();
    if (impl_->status != KeyboardStatus::READY) return result;
    try {
        if (!impl_->device) {
            // GetAsyncKeyState 的全零结果无法区分释放与查询失败；无 owned
            // input source 时只报告 UNVERIFIED，不推进任何语义按键事实。
            impl_->monitor_status = InputMonitorStatus::UNVERIFIED;
            impl_->input_healthy = false;
            publish_state();
            return result;
        }

        InputSnapshot snapshot;
        if (!impl_->device->poll_input(snapshot)) {
            impl_->monitor_status = InputMonitorStatus::FAILURE;
            impl_->input_healthy = false;
            impl_->status = KeyboardStatus::FAILURE;
            publish_state();
            return result;
        }

        impl_->monitor_status = snapshot.status;
        bool new_input_fact = false;
        if (snapshot.state_valid &&
            (!impl_->have_input_fact ||
             snapshot.sequence > impl_->last_input_sequence)) {
            new_input_fact = true;
            impl_->have_input_fact = true;
            impl_->last_input_sequence = snapshot.sequence;

            const auto polled = keyboard::detail::update_keyboard_events(
                impl_->event_state, impl_->config, snapshot.virtual_keys);
            result.events.reserve(polled.count);
            for (std::size_t index = 0; index < polled.count; ++index) {
                result.events.push_back(polled.events[index]);
            }
        }

        // READY 的同序号可维持 change-only monitor 的健康；发生过故障后，
        // 同一缓存序号不能恢复健康，必须等新的有效输入事实。
        const bool current_sequence = snapshot.state_valid &&
            impl_->have_input_fact &&
            snapshot.sequence == impl_->last_input_sequence;
        if (snapshot.status == InputMonitorStatus::READY &&
            current_sequence &&
            (new_input_fact || impl_->input_healthy)) {
            impl_->input_healthy = true;
        } else {
            impl_->input_healthy = false;
        }
        result.new_input_fact = new_input_fact;
        publish_state();
    } catch (...) {
        impl_->status = KeyboardStatus::FAILURE;
        impl_->monitor_status = InputMonitorStatus::FAILURE;
        impl_->input_healthy = false;
        result.events.clear();
        result.new_input_fact = false;
        publish_state();
    }
    return result;
}

void KeyboardListener::close() noexcept {
    if (!impl_) return;
    impl_->event_state = {};
    impl_->monitor_status = InputMonitorStatus::CLOSED;
    impl_->input_healthy = false;
    impl_->have_input_fact = false;
    impl_->last_input_sequence = 0;
    impl_->status = KeyboardStatus::CLOSED;
}

KeyboardStatus KeyboardListener::status() const noexcept {
    return impl_ ? impl_->status : KeyboardStatus::FAILURE;
}
