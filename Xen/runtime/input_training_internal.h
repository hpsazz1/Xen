#ifndef RUNTIME_INPUT_TRAINING_INTERNAL_H
#define RUNTIME_INPUT_TRAINING_INTERNAL_H

#include "input_training/input_training.h"
#include "mouse/mouse.h"
#include <memory>

namespace runtime::detail {
// 只读取已有 monitor；没有 open/move/按键输出权限，析构冻结订阅。
class InputTrainingSource final {
public:
    explicit InputTrainingSource(std::shared_ptr<IMouseController> device)
        : device_(std::move(device)) {}
    ~InputTrainingSource() { device_->set_input_report_subscription(false); }
    input_training::ReadBatch read() {
        input_training::ReadBatch result;
        result.events.reserve(256);
        // 正常轮次最多256条；冻结后读尽65536容量的尾水位，停止不丢最后up。
        std::size_t maximum_batches = 1;
        for (std::size_t index = 0; index < maximum_batches; ++index) {
            InputReportBatch batch;
            if (!device_->read_input_reports(cursor_, batch)) {
                result.trailing_gap = !failed_;
                failed_ = true;
                break;
            }
            failed_ = false;
            if (batch.frozen) maximum_batches = 256;
            if (batch.gap && batch.count == 0) result.trailing_gap = true;
            result.dropped_events += batch.dropped_count;
            for (std::size_t offset = 0; offset < batch.count; ++offset) {
                const auto& report = batch.events[offset];
                input_training::Event event;
                event.epoch = report.epoch;
                event.sequence = report.sequence;
                event.received_at_ns = report.received_at_steady_ns;
                event.held_mask = report.wasd_mask;
                event.left_down = (report.mouse_buttons & 1) != 0;
                event.state_valid = report.state_valid;
                event.motion_valid = report.motion_semantics == InputMotionSemantics::RELATIVE_COUNTS;
                // 未验证字段仍由原始报告完整保存，绝不默认当每包delta。
                if (event.motion_valid) { event.dx = report.raw_x; event.dy = report.raw_y; }
                event.physical_motion_verified = report.physical_source_verified;
                event.source_loss_verifiable = batch.source_loss_verifiable;
                event.raw_report = report.raw_report;
                event.datagram_size = report.datagram_size;
                event.raw_report_valid = report.datagram_size == event.raw_report.size();
                event.gap = batch.gap && offset == 0;
                result.events.push_back(event);
            }
            if (batch.count == 0 || (batch.frozen && cursor_.sequence >= batch.final_sequence)) break;
        }
        return result;
    }
private:
    std::shared_ptr<IMouseController> device_;
    InputReportCursor cursor_;
    bool failed_ = false;
};
}
#endif
