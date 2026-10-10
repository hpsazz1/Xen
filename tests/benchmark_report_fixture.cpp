#include "debug/debug.h"

#include <iostream>
#include <string>

// 只把合成Runtime数据交给实际DebugReport写入器；不创建Runtime、模型或设备。
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    DebugReportConfig config;
    config.csv_path = std::string(argv[1]) + ".csv";
    config.json_path = std::string(argv[1]) + ".json";
    config.session_id = "synthetic-benchmark-evidence";
    config.provider = "CUDAExecutionProvider";
    config.capture_backend = "XUDP_JPEG";
    config.mouse_backend = "WIN32_SEND_INPUT";
    config.max_samples = 100000;
    config.enable_lock_marker = false;
    config.aim_config = AimConfig{};
    DebugReport report;
    std::string error;
    if (!report.start(config, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    RuntimePipelineSample sample;
    sample.sequence = 2;
    sample.geometry.encoded_width = sample.geometry.encoded_height = 320;
    sample.geometry.source_width = 2560;
    sample.geometry.source_height = 1440;
    sample.geometry.roi_x = 1120;
    sample.geometry.roi_y = 560;
    sample.geometry.roi_width = sample.geometry.roi_height = 320;
    sample.detection_status = DetectionStatus::SUCCESS;
    sample.aim_status = AimStatus::SUCCESS;
    report.ingest(std::span<const RuntimePipelineSample>(&sample, 1));
    RuntimeSnapshot snapshot;
    snapshot.provider = config.provider;
    snapshot.source_width = 2560;
    snapshot.source_height = 1440;
    snapshot.encoded_width = snapshot.encoded_height = 320;
    snapshot.capture_roi_x = 1120;
    snapshot.capture_roi_y = 560;
    snapshot.capture_roi_width = snapshot.capture_roi_height = 320;
    DebugCoverageSummary coverage;
    coverage.available = true;
    coverage.startup.sample_count = 1;
    coverage.startup.first_sequence = coverage.startup.last_sequence = 1;
    coverage.formal.sample_count = 1;
    coverage.formal.first_sequence = coverage.formal.last_sequence = 2;
    coverage.formal.counter_matches_sequence_gaps = true;
    if (!report.finalize(snapshot, error, &coverage)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
