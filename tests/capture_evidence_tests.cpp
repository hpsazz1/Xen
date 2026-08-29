#include "capture_evidence/capture_evidence.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using capture_evidence::CaptureEvidenceConfig;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("xen-capture-evidence-tests-" + std::to_string(suffix));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        valid_ = !error;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }
    bool valid() const noexcept { return valid_; }

private:
    std::filesystem::path path_;
    bool valid_ = false;
};

void write_file(const std::filesystem::path& path,
                const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::size_t pending_png_count(const std::filesystem::path& parent,
                              std::size_t& pending_directory_count) {
    pending_directory_count = 0;
    std::size_t png_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (!entry.is_directory() ||
            !entry.path().filename().string().starts_with(
                ".evidence.incoming-")) {
            continue;
        }
        ++pending_directory_count;
        const auto frames = entry.path() / "frames";
        if (!std::filesystem::is_directory(frames)) continue;
        for (const auto& frame : std::filesystem::directory_iterator(frames)) {
            if (frame.is_regular_file() && frame.path().extension() == ".png") {
                ++png_count;
            }
        }
    }
    return png_count;
}

CapturedFrame test_frame(std::uint64_t sequence, unsigned char seed) {
    CapturedFrame frame;
    frame.storage = CapturedFrameStorage::CPU_BGR;
    frame.bgr = cv::Mat(2, 3, CV_8UC3);
    for (int row = 0; row < frame.bgr.rows; ++row) {
        for (int column = 0; column < frame.bgr.cols; ++column) {
            frame.bgr.at<cv::Vec3b>(row, column) = cv::Vec3b(
                static_cast<unsigned char>(seed + row),
                static_cast<unsigned char>(seed + column + 10),
                static_cast<unsigned char>(seed + row + column + 20));
        }
    }
    frame.width = frame.bgr.cols;
    frame.height = frame.bgr.rows;
    frame.timing.sequence = sequence;
    frame.timing.captured_at = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(1000000 + sequence));
    frame.timing.capture_ms = 1.25;
    frame.timing.source_sequence = sequence + 100;
    frame.timing.source_sequence_valid = true;
    frame.timing.source_timecode = static_cast<std::int64_t>(sequence + 200);
    frame.timing.source_timecode_valid = true;
    frame.timing.source_timestamp = static_cast<std::int64_t>(sequence + 300);
    frame.timing.source_timestamp_valid = true;
    frame.timing.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
    frame.timing.source_clock_status = SourceClockStatus::VALID;
    frame.timing.source_time_timing_valid = true;
    frame.timing.source_time_at = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(900000 + sequence));
    frame.timing.source_clock_uncertainty_ms = 0.2;
    frame.timing.source_clock_round_trip_ms = 0.3;
    frame.timing.source_clock_rate = 1.00001;
    frame.timing.source_clock_mapping_age_ms = 2.0;
    frame.timing.source_clock_sample_count = 12;
    frame.timing.source_clock_session_id = 34;
    frame.roi_x = 1120.0;
    frame.roi_y = 560.0;
    frame.source_width = 2560;
    frame.source_height = 1440;
    frame.encoded_width = 320;
    frame.encoded_height = 320;
    frame.source_pixels_per_pixel_x = 1.0;
    frame.source_pixels_per_pixel_y = 1.0;
    return frame;
}

CaptureEvidenceConfig test_config(const TemporaryDirectory& temporary,
                                  std::uint64_t frames) {
    const auto binding = temporary.path() / "source-binding.json";
    write_file(binding,
        "{\"schema_version\":1,\"selected_source\":\"原地跳跃\"}");
    CaptureEvidenceConfig config;
    config.output_directory = temporary.path() / "evidence";
    config.source_binding_path = binding;
    config.capture.backend = CaptureBackend::NDI;
    config.capture.ndi_source_name = "HPSAZZ (Xen-ROI-320)";
    config.capture.ndi_frame_layout =
        NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.capture.ndi_source_width = 2560;
    config.capture.ndi_source_height = 1440;
    config.capture.roi_width = 320;
    config.capture.roi_height = 320;
    config.require_source_timing = true;
    config.requested_frame_count = frames;
    return config;
}

void test_lossless_atomic_publication_and_identity() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建录制测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 2);

    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error), "合法证据录制必须启动：" + error);
    const CapturedFrame first = test_frame(7, 11);
    const CapturedFrame second = test_frame(8, 21);
    expect(recorder.record(first, error), "第一帧必须写入：" + error);
    expect(recorder.record(second, error), "第二帧必须写入：" + error);
    expect(recorder.recorded_frame_count() == 2,
           "Recorder 必须公开实际写入帧数");
    std::size_t pending_directory_count = 0;
    expect(pending_png_count(
               config.output_directory.parent_path(),
               pending_directory_count) == 0 &&
               pending_directory_count == 1,
           "采集阶段只能在有界内存接收帧，不得同步编码 PNG 阻塞 NDI");
    expect(recorder.finish(error), "录满后必须原子发布：" + error);

    const auto manifest_path = config.output_directory / "manifest.json";
    const auto binding_path = config.output_directory / "source-binding.json";
    const auto first_path = config.output_directory / "frames" / "000000.png";
    const auto second_path = config.output_directory / "frames" / "000001.png";
    expect(std::filesystem::is_regular_file(manifest_path) &&
               std::filesystem::is_regular_file(binding_path) &&
               std::filesystem::is_regular_file(first_path) &&
               std::filesystem::is_regular_file(second_path),
           "最终目录必须同时包含 manifest、binding 和全部 PNG");
    expect(read_file(binding_path) == read_file(config.source_binding_path),
           "发布的 binding 必须与启动时输入逐字节一致");

    const cv::Mat decoded_first = cv::imread(
        first_path.string(), cv::IMREAD_COLOR);
    const cv::Mat decoded_second = cv::imread(
        second_path.string(), cv::IMREAD_COLOR);
    expect(!decoded_first.empty() &&
               cv::norm(decoded_first, first.bgr, cv::NORM_INF) == 0.0 &&
               !decoded_second.empty() &&
               cv::norm(decoded_second, second.bgr, cv::NORM_INF) == 0.0,
           "PNG 必须无损保留 Capture 发布的 BGR 像素");

    cv::FileStorage manifest(
        manifest_path.string(), cv::FileStorage::READ |
            cv::FileStorage::FORMAT_JSON);
    expect(manifest.isOpened(), "manifest 必须是可解析 JSON");
    const cv::FileNode root = manifest.root();
    expect(static_cast<int>(root["schema_version"]) == 1 &&
               static_cast<std::string>(root["evidence_type"]) ==
                   "output_off_capture" &&
               static_cast<int>(root["physical_output_capability"]) == 0 &&
               static_cast<std::string>(root["capture_backend"]) == "NDI" &&
               static_cast<std::string>(root["capture_source_name"]) ==
                   "HPSAZZ (Xen-ROI-320)" &&
               static_cast<int>(root["requested_frame_count"]) == 2 &&
               static_cast<int>(root["recorded_frame_count"]) == 2,
           "manifest 必须固化 output-off 安全声明和 Capture 身份");
    const cv::FileNode capture_config = root["capture_config"];
    expect(static_cast<std::string>(capture_config["frame_layout"]) ==
               "CENTER_CROP_1_TO_1" &&
               static_cast<int>(capture_config["source_width"]) == 2560 &&
               static_cast<int>(capture_config["source_height"]) == 1440 &&
               static_cast<int>(capture_config["roi_width"]) == 320 &&
               static_cast<int>(capture_config["roi_height"]) == 320 &&
               static_cast<int>(capture_config["require_source_timing"]) == 1,
           "manifest 必须固化完整 NDI 几何和 source timing 要求");
    const cv::FileNode binding = root["source_binding"];
    expect(static_cast<std::string>(binding["file"]) ==
               "source-binding.json" &&
               static_cast<std::string>(binding["sha256"]).size() == 64,
           "manifest 必须绑定复制后的 source binding SHA-256");
    const cv::FileNode frames = root["frames"];
    expect(frames.isSeq() && frames.size() == 2 &&
               static_cast<int>(frames[0]["sequence"]) == 7 &&
               static_cast<int>(frames[1]["sequence"]) == 8 &&
               static_cast<std::string>(frames[0]["png_sha256"]).size() == 64 &&
               static_cast<std::string>(frames[0]["bgr_sha256"]).size() == 64 &&
               static_cast<int>(frames[0]["source_clock_sample_count"]) == 12 &&
               static_cast<int>(frames[0]["source_clock_session_id"]) == 34 &&
               static_cast<std::string>(frames[0]["source_time_basis"]) ==
                   "NDI_SDK_SUBMISSION" &&
               static_cast<std::string>(frames[0]["source_clock_status"]) ==
                   "VALID",
           "逐帧 manifest 必须固化序号、像素哈希、几何和 source timing");

    const std::string original_manifest = read_file(manifest_path);
    capture_evidence::CaptureEvidenceRecorder duplicate;
    error.clear();
    expect(!duplicate.start(config, error) && !error.empty(),
           "最终目录已存在时必须拒绝覆盖");
    expect(read_file(manifest_path) == original_manifest,
           "拒绝覆盖后既有证据必须逐字节不变");
}

void test_invalid_or_incomplete_capture_never_publishes() {
    TemporaryDirectory invalid_temporary;
    expect(invalid_temporary.valid(), "必须创建非法帧测试临时目录");
    const CaptureEvidenceConfig invalid_config = test_config(
        invalid_temporary, 1);
    {
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        expect(recorder.start(invalid_config, error),
               "非法帧测试必须先正常启动：" + error);
        CapturedFrame invalid = test_frame(1, 1);
        invalid.storage = CapturedFrameStorage::D3D11_BGRA8;
        expect(!recorder.record(invalid, error) && !error.empty(),
               "非 CPU_BGR 帧必须失败封闭");
    }
    expect(!std::filesystem::exists(invalid_config.output_directory),
           "非法帧不得发布最终证据目录");

    TemporaryDirectory incomplete_temporary;
    expect(incomplete_temporary.valid(), "必须创建未录满测试临时目录");
    const CaptureEvidenceConfig incomplete_config = test_config(
        incomplete_temporary, 2);
    {
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        expect(recorder.start(incomplete_config, error),
               "未录满测试必须先正常启动：" + error);
        expect(recorder.record(test_frame(1, 1), error),
               "未录满测试首帧必须写入：" + error);
        expect(!recorder.finish(error) && !error.empty(),
               "帧数不足时 finish 必须失败");
    }
    expect(!std::filesystem::exists(incomplete_config.output_directory),
           "帧数不足不得发布最终证据目录");
}

void test_advertised_maximum_standard_roi_frames_are_recordable() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建最大帧数测试临时目录");
    constexpr std::uint64_t kRequestedFrames = 2400;
    const CaptureEvidenceConfig config = test_config(
        temporary, kRequestedFrames);

    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "公开允许的 2400 帧任务必须启动：" + error);
    CapturedFrame frame = test_frame(1, 1);
    frame.bgr = cv::Mat(320, 320, CV_8UC3, cv::Scalar(1, 2, 3));
    frame.width = frame.bgr.cols;
    frame.height = frame.bgr.rows;
    for (std::uint64_t index = 0; index < kRequestedFrames; ++index) {
        frame.timing.sequence = index + 1;
        if (!recorder.record(frame, error)) break;
    }
    expect(recorder.recorded_frame_count() == kRequestedFrames,
           "公开允许的 2400 张 320x320 BGR 帧必须全部被 Recorder 接收：" +
               error);
}

} // namespace

int main() {
    test_lossless_atomic_publication_and_identity();
    test_invalid_or_incomplete_capture_never_publishes();
    test_advertised_maximum_standard_roi_frames_are_recordable();
    if (failures != 0) {
        std::cerr << failures << " 项 Capture evidence 测试失败。\n";
        return 1;
    }
    std::cout << "Capture evidence 原子发布、身份和 output-off 安全测试通过。\n";
    return 0;
}
