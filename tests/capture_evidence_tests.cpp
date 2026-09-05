#include "capture_evidence/capture_evidence.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#if XEN_HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

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

std::filesystem::path find_unique_pending_directory(
        const std::filesystem::path& parent,
        const std::string& final_name) {
    const std::string prefix = "." + final_name + ".incoming-";
    std::filesystem::path result;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (!entry.is_directory() ||
            !entry.path().filename().string().starts_with(prefix)) {
            continue;
        }
        result = entry.path();
        ++count;
    }
    return count == 1 ? result : std::filesystem::path{};
}

class DirectoryRenameLock {
public:
    explicit DirectoryRenameLock(const std::filesystem::path& directory)
        : handle_(CreateFileW(
              directory.c_str(), FILE_LIST_DIRECTORY,
              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
              FILE_FLAG_BACKUP_SEMANTICS, nullptr)) {}

    ~DirectoryRenameLock() { release(); }

    DirectoryRenameLock(const DirectoryRenameLock&) = delete;
    DirectoryRenameLock& operator=(const DirectoryRenameLock&) = delete;

    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

    void release() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        CloseHandle(handle);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
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
    expect(find_unique_pending_directory(
               invalid_config.output_directory.parent_path(),
               invalid_config.output_directory.filename().string()).empty(),
           "没有完整 manifest 的非法录制仍必须清理 incoming");

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
    expect(find_unique_pending_directory(
               incomplete_config.output_directory.parent_path(),
               incomplete_config.output_directory.filename().string()).empty(),
           "没有完整 manifest 的未录满录制仍必须清理 incoming");
}

void test_required_source_timing_rejects_invalid_frames() {
    {
        TemporaryDirectory temporary;
        expect(temporary.valid(), "必须创建无效源时钟测试临时目录");
        const CaptureEvidenceConfig config = test_config(temporary, 1);
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        expect(recorder.start(config, error),
               "无效源时钟测试必须先正常启动：" + error);

        CapturedFrame frame = test_frame(1, 1);
        frame.timing.source_time_timing_valid = false;
        expect(!recorder.record(frame, error) && !error.empty() &&
                   recorder.recorded_frame_count() == 0,
               "require_source_timing 必须拒绝 timing_valid=false 的帧");
        expect(!recorder.finish(error) &&
                   !std::filesystem::exists(config.output_directory),
               "无效源时钟帧不得形成正式证据目录");
    }

    {
        TemporaryDirectory temporary;
        expect(temporary.valid(), "必须创建非 VALID 源时钟测试临时目录");
        const CaptureEvidenceConfig config = test_config(temporary, 1);
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        expect(recorder.start(config, error),
               "非 VALID 源时钟测试必须先正常启动：" + error);

        CapturedFrame frame = test_frame(1, 1);
        frame.timing.source_clock_status = SourceClockStatus::STALE;
        expect(!recorder.record(frame, error) && !error.empty() &&
                   recorder.recorded_frame_count() == 0,
               "require_source_timing 必须拒绝 source clock 非 VALID 的帧");
        expect(!recorder.finish(error) &&
                   !std::filesystem::exists(config.output_directory),
               "非 VALID 源时钟帧不得形成正式证据目录");
    }
}

void test_required_source_timing_rejects_missing_timestamp_fact() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建源时间戳事实测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);
    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "源时间戳事实测试必须先正常启动：" + error);

    CapturedFrame frame = test_frame(1, 1);
    frame.timing.source_timestamp_valid = false;
    expect(!recorder.record(frame, error) && !error.empty() &&
               recorder.recorded_frame_count() == 0,
           "require_source_timing 必须拒绝 timestamp_valid=false 的帧");
    expect(!recorder.finish(error) &&
               !std::filesystem::exists(config.output_directory),
           "缺少时间戳事实的帧不得形成正式证据目录");
}

void expect_required_timing_rejection(const CapturedFrame& frame,
                                      const std::string& case_name) {
    TemporaryDirectory temporary;
    expect(temporary.valid(), case_name + " 必须创建临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);
    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           case_name + " 必须先正常启动：" + error);
    expect(!recorder.record(frame, error) && !error.empty() &&
               recorder.recorded_frame_count() == 0,
           "require_source_timing 必须拒绝 " + case_name);
    expect(!recorder.finish(error) &&
               !std::filesystem::exists(config.output_directory),
           case_name + " 不得形成正式证据目录");
}

void test_required_source_timing_rejects_unsupported_basis() {
    CapturedFrame unavailable = test_frame(1, 1);
    unavailable.timing.source_time_basis = SourceTimeBasis::UNAVAILABLE;
    expect_required_timing_rejection(unavailable, "basis=UNAVAILABLE 的帧");

    CapturedFrame unknown = test_frame(1, 1);
    unknown.timing.source_time_basis = static_cast<SourceTimeBasis>(999);
    expect_required_timing_rejection(unknown, "未知 basis 的帧");
}

void test_required_source_timing_rejects_unmappable_timestamp() {
    constexpr std::int64_t kUndefinedTimestamp =
        std::numeric_limits<std::int64_t>::max();
#if XEN_HAS_NDI
    static_assert(kUndefinedTimestamp == NDIlib_recv_timestamp_undefined);
#endif
    const struct {
        std::int64_t timestamp;
        const char* name;
    } cases[] = {
        {0, "timestamp=0 的帧"},
        {-1, "timestamp<0 的帧"},
        {kUndefinedTimestamp, "NDI undefined timestamp 的帧"},
        {kUndefinedTimestamp / 100 + 1, "timestamp 乘 100 溢出的帧"},
    };
    for (const auto& test : cases) {
        CapturedFrame frame = test_frame(1, 1);
        frame.timing.source_timestamp = test.timestamp;
        expect_required_timing_rejection(frame, test.name);
    }
}

void expect_required_timing_publication(const CapturedFrame& frame,
                                        const std::string& case_name) {
    TemporaryDirectory temporary;
    expect(temporary.valid(), case_name + " 必须创建临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);
    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           case_name + " 必须先正常启动：" + error);
    expect(recorder.record(frame, error) &&
               recorder.recorded_frame_count() == 1,
           "require_source_timing 必须接受 " + case_name + "：" + error);
    expect(recorder.finish(error) &&
               std::filesystem::is_regular_file(
                   config.output_directory / "manifest.json"),
           case_name + " 必须形成正式证据目录：" + error);
}

void test_required_source_timing_accepts_timestamp_range_edges() {
    CapturedFrame first = test_frame(1, 1);
    first.timing.source_timestamp = 1;
    expect_required_timing_publication(first, "可表示 timestamp 下界的帧");

    CapturedFrame last = test_frame(1, 1);
    last.timing.source_timestamp =
        std::numeric_limits<std::int64_t>::max() / 100;
    expect_required_timing_publication(last, "可表示 timestamp 上界的帧");
}

void test_required_source_timing_rejects_invalid_mapped_time() {
    CapturedFrame missing = test_frame(1, 1);
    missing.timing.source_time_at = {};
    expect_required_timing_rejection(missing, "缺少 source_time_at 的帧");

    CapturedFrame negative = test_frame(1, 1);
    negative.timing.source_time_at = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(-1));
    expect_required_timing_rejection(negative, "source_time_at 为负的帧");

    CapturedFrame future = test_frame(1, 1);
    future.timing.source_time_at = future.timing.captured_at +
        std::chrono::nanoseconds(1);
    expect_required_timing_rejection(future, "source_time_at 晚于采集的帧");
}

void test_required_source_timing_accepts_capture_time_boundary() {
    CapturedFrame frame = test_frame(1, 1);
    frame.timing.source_time_at = frame.timing.captured_at;
    expect_required_timing_publication(
        frame, "source_time_at 等于 captured_at 的帧");
}

void test_required_source_timing_rejects_missing_clock_session() {
    CapturedFrame frame = test_frame(1, 1);
    frame.timing.source_clock_session_id = 0;
    expect_required_timing_rejection(frame, "缺少 source clock session 的帧");
}

void test_required_source_timing_rejects_missing_clock_sample() {
    CapturedFrame frame = test_frame(1, 1);
    frame.timing.source_clock_sample_count = 0;
    expect_required_timing_rejection(frame, "缺少 source clock sample 的帧");
}

void test_required_source_timing_does_not_duplicate_sample_policy() {
    CapturedFrame frame = test_frame(1, 1);
    frame.timing.source_clock_sample_count = 1;
    expect_required_timing_publication(frame, "具有一个 source clock sample 的帧");
}

void test_optional_source_timing_preserves_missing_and_stale_facts() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建 optional timing 测试临时目录");
    CaptureEvidenceConfig config = test_config(temporary, 2);
    config.require_source_timing = false;
    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "optional timing 录制必须启动：" + error);

    CapturedFrame missing = test_frame(1, 1);
    const auto captured_at = missing.timing.captured_at;
    missing.timing = {};
    missing.timing.sequence = 1;
    missing.timing.captured_at = captured_at;
    expect(recorder.record(missing, error),
           "optional timing 必须允许保存缺失源时钟事实：" + error);

    CapturedFrame stale = test_frame(2, 2);
    stale.timing.source_clock_status = SourceClockStatus::STALE;
    stale.timing.source_time_timing_valid = false;
    stale.timing.source_time_at = {};
    expect(recorder.record(stale, error) &&
               recorder.recorded_frame_count() == 2,
           "optional timing 必须允许保存 STALE 状态：" + error);
    expect(recorder.finish(error),
           "optional timing 录满后必须发布：" + error);

    cv::FileStorage manifest(
        (config.output_directory / "manifest.json").string(),
        cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    expect(manifest.isOpened(), "optional timing 必须发布可读 manifest");
    if (!manifest.isOpened()) return;
    const cv::FileNode frames = manifest["frames"];
    expect(static_cast<int>(
               manifest["capture_config"]["require_source_timing"]) == 0 &&
               frames.isSeq() && frames.size() == 2 &&
               static_cast<int>(frames[0]["source_timestamp_valid"]) == 0 &&
               static_cast<std::string>(frames[0]["source_time_basis"]) ==
                   "UNAVAILABLE" &&
               static_cast<std::string>(frames[0]["source_clock_status"]) ==
                   "UNSYNCHRONIZED" &&
               static_cast<int>(frames[0]["source_time_timing_valid"]) == 0 &&
               static_cast<std::string>(frames[1]["source_clock_status"]) ==
                   "STALE" &&
               static_cast<int>(frames[1]["source_time_timing_valid"]) == 0,
           "optional manifest 必须原样保留缺失和 STALE，不能伪装为有效时钟");
}

void test_required_source_timing_recovers_after_rejected_frame() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建 timing 拒绝恢复测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);
    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "timing 拒绝恢复测试必须启动：" + error);

    CapturedFrame invalid = test_frame(1, 1);
    invalid.timing.source_timestamp_valid = false;
    expect(!recorder.record(invalid, error) &&
               recorder.recorded_frame_count() == 0,
           "恢复测试中的坏 timing 不得计入录制数量");
    expect(!recorder.finish(error) &&
               !std::filesystem::exists(config.output_directory),
           "拒绝坏帧后未录满不能发布");
    expect(recorder.record(test_frame(2, 2), error) &&
               recorder.recorded_frame_count() == 1,
           "失败 finish 后必须仍可补入完整 timing 帧：" + error);
    expect(recorder.finish(error),
           "补入合法帧后必须可正常发布：" + error);

    cv::FileStorage manifest(
        (config.output_directory / "manifest.json").string(),
        cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    expect(manifest.isOpened(), "恢复后必须发布可读 manifest");
    if (!manifest.isOpened()) return;
    const cv::FileNode frames = manifest["frames"];
    expect(frames.isSeq() && frames.size() == 1 &&
               static_cast<int>(frames[0]["sequence"]) == 2,
           "恢复后的正式证据只能包含后来接收的合法帧");
}

void test_multi_second_directory_rename_lock_is_retried() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建 transient rename 测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);

    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "transient rename 测试必须启动：" + error);
    expect(recorder.record(test_frame(1, 7), error),
           "transient rename 测试必须录入一帧：" + error);

    const auto pending = find_unique_pending_directory(
        config.output_directory.parent_path(),
        config.output_directory.filename().string());
    expect(!pending.empty(), "必须找到唯一 Recorder incoming 目录");
    DirectoryRenameLock rename_lock(pending);
    expect(rename_lock.valid(),
           "必须建立不共享 FILE_SHARE_DELETE 的目录锁");

    bool manifest_observed = false;
    std::thread releaser([&] {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        std::error_code filesystem_error;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::is_regular_file(
                    pending / "manifest.json", filesystem_error) &&
                !filesystem_error) {
                manifest_observed = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                rename_lock.release();
                return;
            }
            filesystem_error.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rename_lock.release();
    });

    const bool published = recorder.finish(error);
    releaser.join();
    expect(manifest_observed,
           "transient rename 测试必须观察到完整 incoming manifest");
    expect(published,
           "多秒目录 rename 冲突释放后必须在同一次 finish 原子发布：" +
               error);
    expect(std::filesystem::is_regular_file(
               config.output_directory / "manifest.json"),
           "重试成功后最终 manifest 必须存在");
    expect(!std::filesystem::exists(pending),
           "重试成功后 incoming 目录必须消失");
}

void test_directory_publish_retry_never_overwrites_appearing_final() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建 final 竞争测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);

    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(recorder.start(config, error),
           "final 竞争测试必须启动：" + error);
    expect(recorder.record(test_frame(1, 9), error),
           "final 竞争测试必须录入一帧：" + error);

    const auto pending = find_unique_pending_directory(
        config.output_directory.parent_path(),
        config.output_directory.filename().string());
    expect(!pending.empty(), "final 竞争测试必须找到唯一 incoming 目录");
    DirectoryRenameLock rename_lock(pending);
    expect(rename_lock.valid(), "final 竞争测试必须建立目录锁");

    const auto sentinel = config.output_directory / "sentinel.txt";
    bool manifest_observed = false;
    std::thread competitor([&] {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        std::error_code filesystem_error;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::is_regular_file(
                    pending / "manifest.json", filesystem_error) &&
                !filesystem_error) {
                manifest_observed = true;
                std::filesystem::create_directories(
                    config.output_directory, filesystem_error);
                if (!filesystem_error) {
                    write_file(sentinel, "do-not-overwrite");
                }
                rename_lock.release();
                return;
            }
            filesystem_error.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rename_lock.release();
    });

    const bool published = recorder.finish(error);
    competitor.join();
    expect(manifest_observed,
           "final 竞争测试必须观察到完整 incoming manifest");
    expect(!published &&
               error.find("最终证据目录在发布期间出现，拒绝覆盖") !=
                   std::string::npos,
           "重试期间出现 final 时必须明确拒绝覆盖：" + error);
    expect(std::filesystem::is_regular_file(sentinel) &&
               read_file(sentinel) == "do-not-overwrite",
           "重试期间出现的 final sentinel 必须逐字节保持不变");
}

void test_completed_incoming_survives_publish_failure() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建完整 incoming 保留测试临时目录");
    const CaptureEvidenceConfig config = test_config(temporary, 1);
    std::filesystem::path pending;

    {
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        expect(recorder.start(config, error),
               "完整 incoming 保留测试必须启动：" + error);
        expect(recorder.record(test_frame(1, 13), error),
               "完整 incoming 保留测试必须录入一帧：" + error);

        pending = find_unique_pending_directory(
            config.output_directory.parent_path(),
            config.output_directory.filename().string());
        expect(!pending.empty(),
               "完整 incoming 保留测试必须找到唯一 incoming 目录");
        DirectoryRenameLock rename_lock(pending);
        expect(rename_lock.valid(),
               "完整 incoming 保留测试必须建立目录锁");

        bool manifest_observed = false;
        std::thread competitor([&] {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            std::error_code filesystem_error;
            while (std::chrono::steady_clock::now() < deadline) {
                if (std::filesystem::is_regular_file(
                        pending / "manifest.json", filesystem_error) &&
                    !filesystem_error) {
                    manifest_observed = true;
                    std::filesystem::create_directories(
                        config.output_directory, filesystem_error);
                    rename_lock.release();
                    return;
                }
                filesystem_error.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            rename_lock.release();
        });

        const bool published = recorder.finish(error);
        competitor.join();
        expect(manifest_observed,
               "完整 incoming 保留测试必须观察到原子 manifest");
        expect(!published,
               "最终目录竞争出现时发布必须失败且不得覆盖");
    }

    expect(std::filesystem::is_regular_file(pending / "manifest.json") &&
               std::filesystem::is_regular_file(
                   pending / "source-binding.json") &&
               std::filesystem::is_regular_file(
                   pending / "frames" / "000000.png"),
           "完整 manifest 已落盘后发布失败，Recorder 析构不得删除可恢复 incoming");
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

void test_impossible_frame_byte_budget_is_rejected_at_start() {
    TemporaryDirectory temporary;
    expect(temporary.valid(), "必须创建帧字节预算测试临时目录");
    constexpr std::uint64_t kRequestedFrames = 656;
    CaptureEvidenceConfig config = test_config(temporary, kRequestedFrames);
    config.capture.roi_width = 640;
    config.capture.roi_height = 640;

    capture_evidence::CaptureEvidenceRecorder recorder;
    std::string error;
    expect(!recorder.start(config, error) && !error.empty() &&
               !std::filesystem::exists(config.output_directory),
           "640x640x656 BGR 超过 768 MiB 的任务必须在 start 前置拒绝");
}

} // namespace

int main() {
    test_lossless_atomic_publication_and_identity();
    test_invalid_or_incomplete_capture_never_publishes();
    test_required_source_timing_rejects_invalid_frames();
    test_required_source_timing_rejects_missing_timestamp_fact();
    test_required_source_timing_rejects_unsupported_basis();
    test_required_source_timing_rejects_unmappable_timestamp();
    test_required_source_timing_accepts_timestamp_range_edges();
    test_required_source_timing_rejects_invalid_mapped_time();
    test_required_source_timing_accepts_capture_time_boundary();
    test_required_source_timing_rejects_missing_clock_session();
    test_required_source_timing_rejects_missing_clock_sample();
    test_required_source_timing_does_not_duplicate_sample_policy();
    test_optional_source_timing_preserves_missing_and_stale_facts();
    test_required_source_timing_recovers_after_rejected_frame();
    test_multi_second_directory_rename_lock_is_retried();
    test_directory_publish_retry_never_overwrites_appearing_final();
    test_completed_incoming_survives_publish_failure();
    test_impossible_frame_byte_budget_is_rejected_at_start();
    test_advertised_maximum_standard_roi_frames_are_recordable();
    if (failures != 0) {
        std::cerr << failures << " 项 Capture evidence 测试失败。\n";
        return 1;
    }
    std::cout << "Capture evidence 原子发布、身份和 output-off 安全测试通过。\n";
    return 0;
}
