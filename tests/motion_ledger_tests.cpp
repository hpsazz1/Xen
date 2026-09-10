#include "recoil/motion_ledger.h"
#include <iostream>
#include <stdexcept>
using namespace std::chrono;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        MotionLedger ledger;
        const auto t = steady_clock::time_point{} + seconds(1);
        ledger.reset(14, 16, t);
        check(ledger.permits({6, 8}, t), "首笔额度");
        MouseMoveReceipt receipt; receipt.succeeded = true; receipt.backend_completed_at = t;
        ledger.record({6, 8}, receipt, false);
        check(!ledger.permits({0, 5}, t + milliseconds(2)), "worker不能重复取得每帧额度");
        check(ledger.permits({0, 4}, t + milliseconds(2)), "共享剩余额度");
        receipt.backend_completed_at = t + milliseconds(2);
        ledger.record({0, 4}, receipt, true);
        auto window = ledger.snapshot(t + milliseconds(3));
        check(window.complete && window.revision == 1 && window.events.size() == 1 &&
            window.events[0].dy_counts == 4, "只把外部已完成位移交给Aim");
        check(!ledger.permits({1, 0}, t + milliseconds(3)), "总额度耗尽");
        check(ledger.permits({10, 0}, t + milliseconds(16)), "按真实时间释放额度");
        ledger.record({1, 0}, {}, true);
        check(!ledger.snapshot(t + milliseconds(20)).complete &&
            !ledger.permits({1, 0}, t + seconds(10)), "未知不能当零或下一窗口恢复");
        ledger.reset(14, 16, t);
        receipt.backend_completed_at = t - milliseconds(1);
        check(!ledger.record({1, 0}, receipt, true) && !ledger.snapshot(t).complete,
            "完成时间倒序不能污染有序额度或外部账本");
        std::cout << "motion_ledger_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
