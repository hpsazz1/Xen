#include "debug/debug.h"

#include "log/log.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

RuntimePipelineSample make_sample(
        std::uint64_t sequence,
        double total_ms,
        bool success) {
    RuntimePipelineSample sample;
    sample.sequence = sequence;
    sample.profile.capture_ms = 0.2;
    sample.profile.queue_ms = 0.3;
    sample.profile.detector.preprocess_ms = 0.4;
    sample.profile.detector.inference_ms = 0.5;
    sample.profile.detector.h2d_ms = 0.1;
    sample.profile.detector.gpu_preprocess_ms = 0.05;
    sample.profile.detector.gpu_preprocess = true;
    sample.profile.detector.input_upload_bytes = 307200;
    sample.profile.detector.execution_ms = 0.2;
    sample.profile.detector.d2h_ms = 0.1;
    sample.profile.detector.postprocess_ms = 0.3;
    sample.profile.aim.total_ms = 0.6;
    sample.profile.mouse_ms = 0.7;
    sample.profile.total_ms = total_ms;
    sample.detection_status = success
        ? DetectionStatus::SUCCESS : DetectionStatus::INFERENCE_FAILED;
    sample.aim_status = success ? AimStatus::SUCCESS : AimStatus::NOT_RUN;
    sample.mouse_status = success ? MouseStatus::READY : MouseStatus::CLOSED;
    sample.mouse_sent = success;
    return sample;
}

void test_report_summary_and_atomic_files() {
    const auto root = std::filesystem::temp_directory_path() /
                      "xen_debug_report_test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    DebugReport report;
    DebugReportConfig config;
    config.csv_path = (root / "nested" / "runtime.csv").string();
    config.json_path = (root / "nested" / "runtime.json").string();
    config.session_id = "test-session";
    config.model_path = "models/test.onnx";
    config.provider = "CPUExecutionProvider";
    config.capture_backend = "UDP_MJPEG";
    config.mouse_backend = "kmbox_net";
    config.max_samples = 3;
    std::string error;
    expect(report.start(config, error), "Debug 报告合法配置应成功启动");
    if (!report.active()) return;

    const std::vector<RuntimePipelineSample> samples{
        make_sample(1, 1.0, true),
        make_sample(2, 100.0, false),
        make_sample(3, 3.0, true),
        make_sample(4, 5.0, true)};
    report.ingest(samples);
    RuntimeSnapshot final_snapshot;
    final_snapshot.debug_samples_dropped = 7;
    expect(report.finalize(final_snapshot, error),
           "Debug 报告应原子发布 CSV 和 JSON: " + error);
    const auto& summary = report.summary();
    expect(summary.sample_count == 3 && summary.successful_samples == 2 &&
               summary.failed_samples == 1 &&
               summary.report_samples_dropped == 1 &&
               summary.runtime_samples_dropped == 7,
           "报告必须区分保留样本、失败样本和两级丢弃统计");
    expect(summary.total.sample_count == 2 &&
               summary.total.mean_ms == 4.0 &&
               summary.total.p50_ms == 4.0 &&
               summary.total.max_ms == 5.0 &&
               summary.total.p95_ms > 4.8 &&
               summary.total.p95_ms < 5.0,
           "失败样本不得进入成功耗时的均值和分位数");

    std::ifstream csv(root / "nested" / "runtime.csv");
    std::ifstream json(root / "nested" / "runtime.json");
    const std::string csv_text(
        (std::istreambuf_iterator<char>(csv)),
        std::istreambuf_iterator<char>());
    const std::string json_text(
        (std::istreambuf_iterator<char>(json)),
        std::istreambuf_iterator<char>());
    expect(csv_text.find("Xen Runtime Debug Report v2") != std::string::npos &&
               csv_text.find("sequence,capture_ms") != std::string::npos &&
               csv_text.find("gpu_preprocess_ms") != std::string::npos &&
               csv_text.find("INFERENCE_FAILED") != std::string::npos,
           "CSV 必须包含 schema、列头和失败状态");
    expect(json_text.find("\"schema\": 2") != std::string::npos &&
               json_text.find("\"timing\"") != std::string::npos &&
               json_text.find("\"gpu_preprocess\"") != std::string::npos &&
               json_text.find("\"runtime_samples_dropped\": 7") !=
                   std::string::npos,
           "JSON 必须包含 schema、分段统计和 Runtime 丢弃数");
    bool has_temp = false;
    if (std::filesystem::exists(root / "nested")) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 root / "nested")) {
            if (entry.path().string().find(".tmp.") != std::string::npos) {
                has_temp = true;
            }
        }
    }
    expect(!has_temp, "成功发布后不得遗留临时报告文件");
    std::filesystem::remove_all(root, ignored);
}

void test_report_rejects_invalid_capacity() {
    DebugReport report;
    DebugReportConfig config;
    config.max_samples = 0;
    std::string error;
    expect(!report.start(config, error) && !error.empty() &&
               !report.active(),
           "零容量 Debug 报告必须拒绝启动");
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_report_summary_and_atomic_files();
    test_report_rejects_invalid_capacity();
    Log::shutdown();
    if (failures != 0) {
        std::cerr << "Debug 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Debug 测试全部通过。\n";
    return 0;
}
