#include "mouse/movement_trace_internal.h"
#include <iostream>
#include <thread>

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; ++failures; }
    };
    mouse::detail::MovementTraceBuffer<4> buffer;
    MovementTraceBatch batch;
    check(!buffer.read(batch), "未开始的缓冲不可读取");
    check(buffer.begin() && !buffer.begin(), "拒绝重复录制");
    MovementTraceEvent event;
    event.kind = MovementTraceEvent::Kind::MASK;
    event.at_ns = 1; event.completed_ns = 2;
    event.wasd = 1; event.masked = true; event.acknowledged = false;
    buffer.publish(event);
    check(buffer.read(batch) && batch.events.size() == 1 && !batch.gap,
          "读取单个屏蔽请求");
    check(batch.events.front().masked && !batch.events.front().acknowledged &&
          batch.events.front().completed_ns == 2, "未知回执与时刻完整保留");
    check(buffer.read(batch) && batch.events.empty(), "读取消费事件");
    for (int i = 0; i < 5; ++i) { event.at_ns = i; buffer.publish(event); }
    buffer.end();
    event.at_ns = 99; buffer.publish(event);
    check(buffer.read(batch) && batch.gap && batch.events.size() == 4 &&
          batch.events.back().at_ns == 3, "溢出报告缺口且停止后保留尾记录");
    check(buffer.read(batch) && batch.gap && batch.events.empty(), "缺口在会话内持续可见");
    check(buffer.begin(), "允许新录制");
    buffer.publish(event); buffer.end(true);
    check(buffer.read(batch) && batch.gap && batch.events.size() == 1,
          "断连结束报告缺口");
    check(buffer.begin(), "允许新录制重置缺口");
    buffer.end();
    check(buffer.read(batch) && !batch.gap, "正常停止不产生缺口");
    mouse::detail::MovementTraceBuffer<1024> concurrent;
    check(concurrent.begin(), "开始并发录制");
    const auto publish = [&] {
        for (int i = 0; i < 400; ++i) concurrent.publish(event);
    };
    std::thread first(publish), second(publish);
    first.join(); second.join(); concurrent.end();
    std::size_t total = 0;
    do {
        check(concurrent.read(batch) && !batch.gap, "并发生产者不丢失记录");
        total += batch.events.size();
    } while (!batch.events.empty());
    check(total == 800, "并发记录计数一致");
    return failures == 0 ? 0 : 1;
}
