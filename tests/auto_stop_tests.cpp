#include "auto_stop/auto_stop.h"

#include <array>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
}

int main() {
    constexpr std::array<unsigned, 4> keys{1, 2, 4, 8};
    for (const auto first : keys) {
        for (const auto second : keys) {
            if ((first | second) == 5 || (first | second) == 10) continue;
            WasdInputHistory history;
            history.observe(0, 1, 1, 100);
            const auto one = history.observe(first, 1, 2, 200);
            expect(one.history_valid && !one.conflicting, "合法单键应保留可信输入历史");
            const auto two = history.observe(first | second, 1, 3, 300);
            expect(two.history_valid && !two.conflicting, "所有相邻双键进入顺序应可表示");
            for (unsigned index = 0; index < 4; ++index) {
                if ((1U << index) == first)
                    expect(two.held_since_ns[index] == 200, "追加另一个轴不得重置先按轴的时间");
                else if ((1U << index) == second)
                    expect(two.held_since_ns[index] == 300, "后按轴保留独立边沿时间");
            }
            const auto released = history.observe(second, 1, 4, 400);
            expect(released.history_valid, "斜向改单向应保持剩余轴历史");
            const auto stopped = history.observe(0, 1, 5, 500);
            expect(stopped.history_valid && stopped.held_mask == 0,
                   "明确全松开应归零输入意图，而非输出物理停稳结论");
        }
    }
    WasdInputHistory history;
    expect(!history.observe(1, 1, 1, 100).history_valid,
           "启动时已经按住不能伪造按下时间");
    history.observe(0, 1, 2, 200);
    history.observe(1, 1, 3, 300);
    const auto conflict = history.observe(5, 1, 4, 400);
    expect(conflict.conflicting && !conflict.history_valid,
           "相反键冲突必须使制动历史失去资格");
    expect(!history.observe(4, 1, 5, 500).history_valid,
           "冲突解除不能凭空补出先前运动历史");
    history.observe(0, 1, 6, 600);
    history.observe(3, 1, 7, 700);
    expect(!history.observe(0, 1, 8, 800, false).history_valid,
           "错误报告不能冒充物理全释放");
    history.observe(0, 1, 9, 900);
    expect(!history.observe(1, 1, 10, 1000, true, true).history_valid,
           "输入缺口必须撤销历史资格");
    history.observe(0, 2, 1, 1100);
    expect(!history.observe(1, 1, 50, 1200).history_valid,
           "旧代际迟到不能重建当前历史");
    history.observe(0, 2, 2, 1300);
    history.observe(2, 2, 3, 1400);
    expect(history.observe(2, 2, 3, 1400).held_since_ns[1] == 1400,
           "重复同一事实不能重置持键时间");
    expect(!history.observe(0, 2, 3, 1400).history_valid,
           "同序号不同键态不得被接受为释放");
    history.observe(0, 3, 1, 1500);
    history.observe(1, 3, 2, 1600);
    history.observe(0, 0, 1, 1700);
    expect(!history.observe(0, 2, 10, 1800).history_valid,
           "零代际错误报告不能清除已见代际水位并接纳旧代际释放");
    history.observe(0, 3, 3, 1900);
    history.observe(1, 3, 4, 2000);
    history.observe(1, 3, 6, 2200, false);
    expect(!history.observe(0, 3, 5, 2100).history_valid,
           "无效新报告的序号和时间水位不能被较早释放跨过");
    expect(history.observe(0, 3, 7, 2300).history_valid,
           "错误之后仅新的合法全释放能够重新同步");
    for (const bool enabled : {false, true}) {
        for (const bool supported : {false, true}) {
            for (const bool paused : {false, true}) {
                const auto state = assess_auto_stop_availability(
                    {enabled, 0x05}, supported, true, paused);
                expect(!state.fire_permitted && !state.stop_evidence_available,
                       "所有配置/暂停组合均不得把首批协议能力变成停稳或开火许可");
            }
        }
    }
    if (failures == 0) std::cout << "自动急停输入历史与未验证输出边界通过\n";
    return failures == 0 ? 0 : 1;
}
