#ifndef MOUSE_INPUT_REPORT_INTERNAL_H
#define MOUSE_INPUT_REPORT_INTERNAL_H

#include "mouse/mouse.h"
#include <algorithm>
#include <span>

namespace mouse::detail {

// 只还原公开 SDK 的 packed 字段布局，不假设坐标是增量或累计量。
inline InputReportEvent parse_input_report(std::span<const std::uint8_t> payload,
        std::int64_t received_ns, bool endpoint_valid) noexcept {
    InputReportEvent event;
    event.received_at_steady_ns = received_ns;
    event.datagram_size = payload.size();
    std::copy_n(payload.begin(), std::min(payload.size(), event.raw_report.size()),
                event.raw_report.begin());
    if (payload.size() != event.raw_report.size() || !endpoint_valid) return event;
    const auto signed_word = [&](std::size_t offset) noexcept {
        const int value = payload[offset] | (static_cast<int>(payload[offset + 1]) << 8);
        return static_cast<std::int16_t>(value >= 32768 ? value - 65536 : value);
    };
    event.mouse_buttons = payload[1];
    event.raw_x = signed_word(2);
    event.raw_y = signed_word(4);
    event.raw_wheel = signed_word(6);
    event.keyboard_modifiers = payload[9];
    event.state_valid = true;
    for (std::size_t i = 0; i < event.keyboard_usages.size(); ++i) {
        const auto usage = payload[i + 10];
        event.keyboard_usages[i] = usage;
        if (usage >= 1 && usage <= 3) event.state_valid = false;
        if (usage == 0x1a) event.wasd_mask |= 1;
        if (usage == 0x04) event.wasd_mask |= 2;
        if (usage == 0x16) event.wasd_mask |= 4;
        if (usage == 0x07) event.wasd_mask |= 8;
    }
    return event;
}

// 外部owner持monitor锁。固定容量覆盖最旧记录，每个reader独立报告精确本机缺口。
class InputReportBuffer {
public:
    static constexpr std::size_t kCapacity = 65536;
    bool subscribed() const noexcept { return subscribed_; }
    bool subscribe(bool enabled) noexcept {
        if (!enabled) { freeze(); return true; }
        if (subscribed_) return true;
        try {
            if (!events_) events_ = std::make_unique<Storage>();
        } catch (...) { return false; }
        subscribed_ = true;
        frozen_ = false;
        ++epoch_;
        sequence_ = 0;
        status_ = InputMonitorStatus::WAITING;
        return true;
    }
    void freeze() noexcept {
        if (!events_) return;
        subscribed_ = false;
        frozen_ = true;
    }
    void publish(InputReportEvent event) noexcept {
        if (!subscribed_) return;
        event.epoch = epoch_;
        event.sequence = ++sequence_;
        (*events_)[(sequence_ - 1) % kCapacity] = event;
        status_ = event.state_valid ? InputMonitorStatus::READY : InputMonitorStatus::UNVERIFIED;
    }
    void fail(std::int64_t received_ns) noexcept {
        if (!subscribed_) return;
        InputReportEvent event;
        event.received_at_steady_ns = received_ns;
        publish(event);
        status_ = InputMonitorStatus::FAILURE;
    }
    void close(std::int64_t received_ns) noexcept {
        if (subscribed_) {
            InputReportEvent event;
            event.received_at_steady_ns = received_ns;
            publish(event);
        }
        freeze();
        status_ = InputMonitorStatus::CLOSED;
    }
    void read(InputReportCursor& cursor, InputReportBatch& batch) const noexcept {
        batch = {};
        batch.subscribed = subscribed_;
        batch.frozen = frozen_;
        batch.status = status_;
        batch.epoch = epoch_;
        batch.final_sequence = sequence_;
        if (!events_) return;
        if (cursor.epoch != epoch_) {
            batch.gap = cursor.epoch != 0;
            cursor = {epoch_, 0};
        }
        if (cursor.sequence > sequence_) {
            batch.gap = true;
            cursor.sequence = 0;
        }
        const auto oldest = sequence_ > kCapacity ? sequence_ - kCapacity + 1 : 1;
        if (cursor.sequence < oldest - 1) {
            batch.gap = true;
            batch.dropped_count = oldest - 1 - cursor.sequence;
            cursor.sequence = oldest - 1;
        }
        while (cursor.sequence < sequence_ && batch.count < batch.events.size()) {
            ++cursor.sequence;
            batch.events[batch.count++] = (*events_)[(cursor.sequence - 1) % kCapacity];
        }
    }
private:
    using Storage = std::array<InputReportEvent, kCapacity>;
    std::unique_ptr<Storage> events_;
    std::uint64_t epoch_ = 0;
    std::uint64_t sequence_ = 0;
    bool subscribed_ = false;
    bool frozen_ = false;
    InputMonitorStatus status_ = InputMonitorStatus::CLOSED;
};
}
#endif
