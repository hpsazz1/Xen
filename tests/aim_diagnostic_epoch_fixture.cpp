#include "debug/debug.h"
#include "runtime/runtime_internal.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}
RuntimePipelineSample sample(std::uint64_t sequence, std::uint64_t epoch, int dx) {
    RuntimePipelineSample value;
    value.sequence = sequence; value.aim_observation_epoch = epoch;
    value.geometry.encoded_width = value.geometry.encoded_height = 320;
    value.geometry.roi_width = value.geometry.roi_height = 320;
    value.geometry.source_width = value.geometry.source_height = 320;
    value.detection_status = DetectionStatus::SUCCESS;
    value.aim_status = AimStatus::SUCCESS;
    value.aim_has_target = value.aim_has_command = true;
    value.aim_range_locked = value.aim_range_allows_control = true;
    value.aim_base_point_inside_box = value.aim_command_toward_target = true;
    value.aim_control_center_x = value.aim_control_center_y = 160;
    value.aim_acquisition_range_radius = 144; value.aim_active_range_radius = 72;
    value.aim_target.track_id = 7; value.aim_target.state = TrackState::CONFIRMED;
    value.aim_target.x1 = value.aim_target.y1 = 100;
    value.aim_target.x2 = value.aim_target.y2 = 220;
    value.aim_target.matched_observation_valid = true;
    value.aim_target.matched_observation_x1 = value.aim_target.matched_observation_y1 = 100;
    value.aim_target.matched_observation_x2 = value.aim_target.matched_observation_y2 = 220;
    value.aim_target.base_aim_x = value.aim_target.delay_compensated_aim_x = value.aim_target.aim_x = 160.0f + dx * 10.0f;
    value.aim_target.base_aim_y = value.aim_target.delay_compensated_aim_y = value.aim_target.aim_y = 160;
    value.aim_control.evaluated = true; value.aim_control.controller_dt_ms = 4;
    value.aim_control.desired_x_counts = static_cast<float>(dx);
    value.aim_command.sequence = sequence; value.aim_command.dx_counts = dx;
    return value;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::u8path(argv[1]);
    if (!std::filesystem::is_directory(root)) return 2;
    auto ring = std::make_unique<runtime::detail::BoundedSampleRing<RuntimePipelineSample, 4096>>();
    DebugReportConfig config;
    config.csv_path = utf8(root / "ring.csv"); config.json_path = utf8(root / "ring.json");
    config.enable_lock_marker = false; config.session_id = "synthetic-epoch-ring";
    DebugReport report; std::string error; std::vector<RuntimePipelineSample> values;
    if (!report.start(config, error)) { std::cerr << error; return 1; }
    constexpr std::uint64_t old_epoch = 9007199254740993ULL;
    if (!ring->push(sample(1, old_epoch, 2)) || !ring->drain(values)) return 1;
    report.ingest(values);
    auto reset = sample(2, old_epoch + 1, 0);
    reset.aim_has_target = false; reset.aim_has_command = false;
    reset.aim_control.filter_reset_x = true;
    if (!ring->push(reset)) return 1;
    for (std::uint64_t sequence = 3; sequence <= 4098; ++sequence) {
        if (!ring->push(sample(sequence, old_epoch + 1, -2))) return 1;
    }
    if (!ring->drain(values) || ring->dropped() != 1 || values.size() != 4096 || values.front().sequence != 3) return 1;
    for (const auto& value : values) {
        if (value.aim_control.filter_reset_x || value.aim_observation_epoch != old_epoch + 1) return 1;
    }
    report.ingest(values);
    RuntimeSnapshot final_snapshot; final_snapshot.debug_samples_dropped = ring->dropped();
    if (!report.finalize(final_snapshot, error)) { std::cerr << error; return 1; }
    std::cout << "Actual ring dropped reset=1, report samples=4097, persistent epoch retained\n";
    return 0;
}
