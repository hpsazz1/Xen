#include "overlay/overlay_internal.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool nearly_equal(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.0001f;
}

void test_metric_history_order_and_overwrite() {
    overlay::detail::MetricHistory<3> history;
    expect(history.empty() && history.size() == 0,
           "新历史环必须为空");

    history.push(1.0f);
    history.push(2.0f);
    history.push(3.0f);
    expect(history.size() == 3 &&
               nearly_equal(history.at(0), 1.0f) &&
               nearly_equal(history.at(2), 3.0f),
           "未满容量时必须按时间顺序读取样本");

    history.push(4.0f);
    expect(history.size() == 3 &&
               nearly_equal(history.at(0), 2.0f) &&
               nearly_equal(history.at(1), 3.0f) &&
               nearly_equal(history.at(2), 4.0f),
           "满载后必须覆盖最旧样本并保持时间顺序");
    expect(nearly_equal(history.latest(), 4.0f) &&
               nearly_equal(history.maximum(0.5f), 4.0f),
           "最新值和动态上界必须反映保留样本");
}

void test_metric_history_clear_and_floor() {
    overlay::detail::MetricHistory<2> history;
    history.push(0.25f);
    expect(nearly_equal(history.maximum(1.0f), 1.0f),
           "动态上界不得低于调用方提供的显示下限");
    history.clear();
    expect(history.empty() && history.size() == 0 &&
               nearly_equal(history.latest(), 0.0f) &&
               nearly_equal(history.at(0), 0.0f),
           "清空后不得暴露上一轮运行的历史值");
}

} // namespace

int main() {
    test_metric_history_order_and_overwrite();
    test_metric_history_clear_and_floor();
    if (failures != 0) {
        std::cerr << "Overlay 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Overlay 测试全部通过。\n";
    return 0;
}
