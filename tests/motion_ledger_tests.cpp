#include "recoil/motion_ledger.h"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace std::chrono;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            std::ifstream stream(argv[1]);
            const auto report = nlohmann::json::parse(stream);
            const auto& samples = report.at("samples");
            auto time = [](const auto& value) {
                return steady_clock::time_point(nanoseconds(std::stoll(value.template get<std::string>())));
            };
            MotionLedger replay;
            replay.reset(report.at("aim_config").at("max_counts_per_frame").get<double>(),
                report.at("recoil").at("config").at("budget_window_ms").get<int>(),
                time(samples.front().at("control_steady_ns")));
            int commands = 0, blocked = 0;
            for (const auto& sample : samples) {
                const auto now = time(sample.at("control_steady_ns"));
                const MouseMoveCommand command{sample.at("aim_command")[0].get<int>(), sample.at("aim_command")[1].get<int>()};
                if (sample.at("aim_lock_active").get<bool>() && sample.at("aim_has_command").get<bool>()) {
                    ++commands;
                    if (!replay.permits(command, now)) ++blocked;
                }
                if (sample.at("mouse_sent").get<bool>()) {
                    MouseMoveReceipt receipt; receipt.succeeded = true;
                    receipt.backend_completed_at = now + duration_cast<steady_clock::duration>(
                        duration<double, std::milli>(sample.at("control_to_mouse_backend_completion_ms").get<double>()));
                    check(replay.record(command, receipt, false), "真实已发送命令账本记录失败");
                }
            }
            std::cout << "真实Run软件回放：commands=" << commands << ", blocked=" << blocked << '\n';
            check(commands > 0 && blocked == 0, "合法逐帧命令不应被重复滚动额度挡住");
            return 0;
        }
        MotionLedger ledger;
        const auto t = steady_clock::time_point{} + seconds(1);
        ledger.reset(14, 16, t);
        check(ledger.permits({6, 8}, t), "首笔额度");
        MouseMoveReceipt receipt; receipt.succeeded = true; receipt.backend_completed_at = t;
        ledger.record({6, 8}, receipt, false);
        check(ledger.permits({0, 5}, t + milliseconds(2)), "下一帧合法输出不累计节流");
        check(!ledger.permits({14, 14}, t + milliseconds(2)), "保留Aim单帧二维步长上限");
        receipt.backend_completed_at = t + milliseconds(2);
        ledger.record({0, 4}, receipt, true);
        auto window = ledger.snapshot(t + milliseconds(3));
        check(window.complete && window.revision == 1 && window.events.size() == 1 &&
            window.events[0].dy_counts == 4, "只把外部已完成位移交给Aim");
        check(ledger.permits({14, 0}, t + milliseconds(3)), "Recoil外部位移不消费Aim单帧上限");
        check(!ledger.permits({15, 0}, t + milliseconds(3)) && !ledger.permits({0, -15}, t + milliseconds(3)),
            "Aim正负单次上限仍有效");
        receipt.backend_completed_at = t + milliseconds(4);
        check(ledger.record({14, 0}, receipt, false) && ledger.permits({14, 0}, t + milliseconds(8)),
            "连续240Hz满幅命令不被16ms窗口降速");
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
