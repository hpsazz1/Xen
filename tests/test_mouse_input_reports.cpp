#include "mouse/input_report_internal.h"
#include <iostream>
#include <cstdlib>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}
int main() {
    std::array<std::uint8_t, 20> raw{};
    raw[1] = 1;
    raw[2] = 0xff; raw[3] = 0xff;
    raw[4] = 0; raw[5] = 0x80;
    raw[6] = 0xff; raw[7] = 0x7f;
    raw[10] = 0x04; raw[11] = 0x07;
    const auto event = mouse::detail::parse_input_report(raw, 123, true);
    require(event.state_valid && event.mouse_buttons == 1 && event.wasd_mask == 10,
            "同包按钮与反向双键原样保留");
    require(event.raw_x == -1 && event.raw_y == -32768 && event.raw_wheel == 32767,
            "有符号小端字段错误");
    require(event.motion_semantics == InputMotionSemantics::UNVERIFIED &&
            !event.physical_source_verified && event.raw_report == raw,
            "未验证字段不可升级为位移或物理来源");
    raw[12] = 1;
    require(!mouse::detail::parse_input_report(raw, 124, true).state_valid, "HID错误必须无效");
    require(!mouse::detail::parse_input_report(std::span(raw).first(19), 125, true).state_valid,
            "截断包必须无效");
    require(!mouse::detail::parse_input_report(raw, 126, false).state_valid, "无效端点必须无效");
    mouse::detail::InputReportBuffer buffer;
    InputReportCursor a, b;
    InputReportBatch batch;
    buffer.publish(event);
    buffer.read(a, batch);
    require(batch.count == 0 && !batch.subscribed, "未订阅不得记录");
    require(buffer.subscribe(true), "订阅分配失败");
    buffer.publish(event); buffer.publish(event);
    buffer.read(a, batch);
    require(batch.count == 2 && !batch.gap && batch.events[1].sequence == 2,
            "相同位移原包不得去重");
    buffer.read(b, batch);
    require(batch.count == 2, "独立游标不得破坏读取");
    buffer.freeze(); buffer.publish(event);
    InputReportCursor tail;
    buffer.read(tail, batch);
    require(batch.count == 2 && batch.frozen && !batch.subscribed && batch.final_sequence == 2,
            "冻结必须保留尾部并停止发布");
    buffer.read(tail, batch);
    require(batch.count == 0 && batch.final_sequence == 2, "冻结后的合法空批错误");
    require(buffer.subscribe(true), "重新订阅失败");
    for (std::size_t i = 0; i < mouse::detail::InputReportBuffer::kCapacity + 3; ++i)
        buffer.publish(event);
    InputReportCursor fresh;
    buffer.read(fresh, batch);
    require(batch.gap && batch.dropped_count == 3 && batch.events[0].sequence == 4,
            "覆盖缺口必须精确报告");
    buffer.read(a, batch);
    require(batch.gap && batch.epoch != 1, "代际切换必须暴露缺口");
    InputReportCursor end{batch.epoch, batch.final_sequence};
    buffer.fail(999);
    buffer.read(end, batch);
    require(batch.count == 1 && !batch.events[0].state_valid &&
            batch.status == InputMonitorStatus::FAILURE, "链路失败不能伪造松开");
    buffer.close(1000);
    buffer.read(end, batch);
    require(batch.count == 1 && !batch.events[0].state_valid && batch.frozen &&
            batch.status == InputMonitorStatus::CLOSED, "关闭必须保留无效终止事实");
    std::cout << "输入报告解析、冻结、缺口与代际契约通过\n";
}
