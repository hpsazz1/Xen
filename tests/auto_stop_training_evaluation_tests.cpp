#include "auto_stop_probe/training_evaluation_internal.h"
#include <iostream>

namespace {
using namespace auto_stop_probe_detail;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json command(const char* kind, int value, std::int64_t milliseconds, int disposition = 2) {
    const auto time = milliseconds * 1000000;
    return {{"kind", kind}, {"value", value}, {"submit_ns", time - 100}, {"ack_received_ns", time},
        {"backend_completed_ns", time + 10}, {"returned_ns", time + 20}, {"disposition", disposition}};
}
Json initial() { return {{"commands", Json::array({command("wasd", 0, 1), command("left_button", 0, 2)})}}; }
void run(const std::filesystem::path& root) {
    auto report = initial();
    for (auto entry : {command("wasd", 2, 10), command("wasd", 0, 20), command("wasd", 8, 23),
        command("left_button", 1, 24), command("left_button", 0, 40), command("wasd", 0, 50)})
        report["commands"].push_back(entry);
    const auto result = evaluate_counterpulse_training(report, root / "gap");
    check(result["success"] == true && result["source"] == "COMMAND_ACK", "命令域归档完成");
    check(result["received_events"] == 8 && result["total_timings"] == 1, "初始全松命令保留");
    check(result["timings_summary_complete"] == true && result["holds_summary_complete"] == true,
        "本组摘要完整性显式声明");
    check(result["timings"][0]["delta_ns"] == 3000000, "正向间隙复用生产配对");
    check(result["holds"][0]["complete_received_stream"] == true && result["holds"][0]["motion_available"] == false,
        "完整命令按住段不冒充位移");
    check(result["game_shot_stability"].is_null() && result["physical_validation_passed"] == false &&
        result["timings"][0]["timing_uncertainty_known"] == false, "未知时钟误差与游戏效果保持未知");
    input_training::Session replay;
    check(replay.load(root / "gap"), "归档可由生产Session回放");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (replay.snapshot()->status == input_training::Status::REPLAYING && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto replayed = training_snapshot_json(*replay.snapshot(), "COMMAND_ACK");
    check(replayed["timings"] == result["timings"] && replayed["holds"] == result["holds"], "回放评价一致");
    replay.stop();

    report = initial();
    for (auto entry : {command("wasd", 2, 10), command("wasd", 10, 20), command("wasd", 8, 24)})
        report["commands"].push_back(entry);
    const auto overlap = evaluate_counterpulse_training(report, root / "overlap");
    check(overlap["timings"].size() == 1 && overlap["timings"][0]["delta_ns"] == -4000000,
        "双位WASD保留重叠");

    report = initial();
    report["commands"].push_back(command("left_button", 1, 10));
    report["commands"].push_back(command("left_button", 0, 1010));
    const auto held_second = evaluate_counterpulse_training(report, root / "held-second");
    check(held_second["holds"][0]["observed_duration_ns"] == 1000000000 &&
        held_second["source"] == "COMMAND_ACK" && held_second["physical_validation_passed"] == false,
        "一秒ACK按住观察跨度不能称真实开枪时长");

    report = initial();
    for (auto entry : {command("left_button", 1, 10), command("wasd", 2, 12), command("wasd", 0, 20, 3),
        command("wasd", 8, 23), command("left_button", 0, 30)}) report["commands"].push_back(entry);
    const auto unknown = evaluate_counterpulse_training(report, root / "unknown");
    check(unknown["invalid_receipts"] == 1 && unknown["total_timings"] == 0 &&
        unknown["holds"][0]["complete_received_stream"] == false, "未知ACK断开配对和完整按住段");
    report["commands"][4]["disposition"] = 2;
    report["commands"][4]["ack_received_ns"] = 0;
    const auto bad_time = evaluate_counterpulse_training(report, root / "bad-time");
    check(bad_time["invalid_receipts"] == 1, "非法ACK时间不能伪造有效状态");
    const auto empty = evaluate_counterpulse_training({{"commands", Json::array()}}, root / "empty");
    check(empty["success"] == true && empty["samples_present"] == false && empty["physical_validation_passed"] == false,
        "空档案不称实物验证成功");
    report = initial();
    for (int index = 0; index < 101; ++index) {
        report["commands"].push_back(command("left_button", 1, 10 + index * 2));
        report["commands"].push_back(command("left_button", 0, 11 + index * 2));
    }
    const auto limited = evaluate_counterpulse_training(report, root / "retained");
    check(limited["success"] == true && limited["total_holds"] == 101 && limited["holds_retained"] == 100 &&
        limited["holds_summary_complete"] == false && limited["received_events"] == 204,
        "超保留预算只截摘要并明确标记，原始事件全部归档");
    bool refused = false;
    try { evaluate_counterpulse_training(report, root / "gap"); } catch (...) { refused = true; }
    check(refused, "拒绝覆盖已有原始档案");
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("xen-command-training-" +
        std::to_string(Clock::now().time_since_epoch().count()));
    try { run(root); std::filesystem::remove_all(root); std::cout << "命令训练评价测试通过\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
