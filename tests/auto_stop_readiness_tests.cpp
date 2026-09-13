#include "auto_stop_probe/readiness_internal.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace auto_stop_probe_detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
source_context::SourceContextSnapshot focus_ready() { return {true, true, 1, 1, 10}; }
InputSnapshot input_ready() {
    InputSnapshot input; input.status = InputMonitorStatus::READY; input.state_valid = true; input.sequence = 1; return input;
}
void gate_matrix() {
    const auto focus = focus_ready(); const auto input = input_ready();
    require(evaluate_counterpulse_readiness(focus, input, true, false)["ready"], "完整许可应即刻有效");
    for (const auto status : {InputMonitorStatus::CLOSED, InputMonitorStatus::UNVERIFIED,
        InputMonitorStatus::WAITING, InputMonitorStatus::STALE, InputMonitorStatus::FAILURE}) {
        auto bad = input; bad.status = status;
        require(!evaluate_counterpulse_readiness(focus, bad, true, false)["ready"].get<bool>(), "非READY不得放行");
    }
    auto first = InputSnapshot{}; first.status = InputMonitorStatus::WAITING;
    require(evaluate_counterpulse_readiness(focus, first, true, false)["reason"] == "MONITOR_WAITING", "首包未知不得视为全松");
    for (int field = 0; field < 4; ++field) {
        auto bad = focus;
        if (field == 0) bad.available = false;
        if (field == 1) bad.focused = false;
        if (field == 2) bad.session_id = 0;
        if (field == 3) bad.sequence = 0;
        require(!evaluate_counterpulse_readiness(bad, input, true, false)["ready"].get<bool>(), "焦点许可不完整不得放行");
    }
    auto invalid = input; invalid.state_valid = false;
    require(!evaluate_counterpulse_readiness(focus, invalid, true, false)["ready"].get<bool>(), "无效键态不得放行");
    invalid = input; invalid.sequence = 0;
    require(!evaluate_counterpulse_readiness(focus, invalid, true, false)["ready"].get<bool>(), "无序号不得放行");
    require(evaluate_counterpulse_readiness(focus, input, false, false)["reason"] == "MONITOR_POLL_FAILED", "读取失败独立分类");
    constexpr int keys[]{0x57, 0x41, 0x53, 0x44, 1, 2, 4, 5, 6};
    for (unsigned i = 0; i < 9; ++i) {
        auto held = input; held.virtual_keys[keys[i]] = true;
        const auto result = evaluate_counterpulse_readiness(focus, held, true, false);
        require(result["reason"] == "PHYSICAL_KEYS_HELD" && result["blocked_keys"] == (1U << i), "九个阻塞键逐位准确记录");
    }
    auto other = input; other.virtual_keys['B'] = true;
    require(evaluate_counterpulse_readiness(focus, other, true, false)["blocked_keys"] == 0, "非白名单按键不得记录");
    auto end = input; end.virtual_keys[0x23] = true;
    require(evaluate_counterpulse_readiness({}, end, true, false)["reason"] == "USER_CANCELLED", "End优先于焦点失败");
    const auto concurrent = evaluate_counterpulse_readiness({}, first, true, true);
    require(concurrent["flags"]["cancelled"] && concurrent["flags"]["source_unavailable"] &&
        concurrent["flags"]["monitor_waiting"], "并列失败保留且取消优先");
}
void stability_matrix() {
    auto focus = focus_ready(); auto input = input_ready(); CounterpulseReadinessAccumulator gate;
    require(!gate.update(focus, input, true, false, 0)["ready"].get<bool>(), "首个就绪快照必须等待");
    require(!gate.update(focus, input, true, false, 299999999)["ready"].get<bool>(), "不足300ms不得放行");
    require(gate.update(focus, input, true, false, 300000000)["ready"], "完整300ms连续许可放行");
    input.virtual_keys['A'] = true;
    require(gate.update(focus, input, true, false, 310000000)["continuous_ready_ns"] == 0, "持键重置窗口");
    input.virtual_keys['A'] = false;
    require(!gate.update(focus, input, true, false, 320000000)["ready"].get<bool>(), "释放后重建窗口");
    require(gate.update(focus, input, true, false, 620000000)["ready"], "新窗口独立完成");
    focus.session_id = 2;
    require(!gate.update(focus, input, true, false, 630000000)["ready"].get<bool>(), "源会话变更重置窗口");
    require(gate.update(focus, input, true, false, 629000000)["reason"] == "CLOCK_REGRESSION", "倒退时钟拒绝放行");
    require(gate.update(focus, input, true, true, 640000000)["reason"] == "USER_CANCELLED", "显式取消优先");
    require(gate.update(focus, input, true, false, 1000000000)["reason"] == "USER_CANCELLED", "取消后同一事务不得恢复");
    CounterpulseReadinessAccumulator sequence_gate;
    focus.sequence = 2; input.sequence = 2;
    sequence_gate.update(focus, input, true, false, 0);
    input.sequence = 1;
    require(sequence_gate.update(focus, input, true, false, 300000000)["reason"] == "READINESS_SEQUENCE_REGRESSION", "输入序号倒退重置窗口");
}
}
int main() {
    try { gate_matrix(); stability_matrix(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "就绪纯状态专项通过：首包、焦点、全松、取消、连续许可及诊断白名单\n";
}
