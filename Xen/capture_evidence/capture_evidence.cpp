#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#include "capture_evidence/capture_evidence.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace capture_evidence {
namespace {

constexpr std::uint64_t kMaximumEvidenceFrames = 2400;
constexpr std::uint64_t kMaximumBufferedBgrBytes =
    768ULL * 1024ULL * 1024ULL;
constexpr std::size_t kHashBufferBytes = 64U * 1024U;
std::atomic<std::uint64_t> g_pending_sequence{0};

void set_error(std::string& output, const std::string& value) noexcept {
    try {
        output = value;
    } catch (...) {
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

bool nt_succeeded(NTSTATUS status) noexcept { return status >= 0; }

class Sha256Context {
public:
    Sha256Context() = default;
    ~Sha256Context() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    Sha256Context(const Sha256Context&) = delete;
    Sha256Context& operator=(const Sha256Context&) = delete;

    bool initialize(std::string& error) {
        DWORD object_bytes = 0;
        DWORD hash_bytes = 0;
        DWORD copied = 0;
        if (!nt_succeeded(BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !nt_succeeded(BCryptGetProperty(
                algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes),
                sizeof(object_bytes), &copied, 0)) ||
            copied != sizeof(object_bytes) || object_bytes == 0 ||
            !nt_succeeded(BCryptGetProperty(
                algorithm_, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_bytes),
                sizeof(hash_bytes), &copied, 0)) ||
            copied != sizeof(hash_bytes) || hash_bytes != 32U) {
            set_error(error, "无法初始化 Windows CNG SHA-256");
            return false;
        }
        object_.resize(object_bytes);
        digest_.resize(hash_bytes);
        if (!nt_succeeded(BCryptCreateHash(
                algorithm_, &hash_, object_.data(),
                static_cast<ULONG>(object_.size()), nullptr, 0, 0))) {
            set_error(error, "无法创建 Windows CNG SHA-256 句柄");
            return false;
        }
        return true;
    }

    bool update(std::span<const unsigned char> bytes,
                std::string& error) noexcept {
        if (bytes.empty()) return true;
        if (bytes.size() > static_cast<std::size_t>(
                std::numeric_limits<ULONG>::max()) ||
            !nt_succeeded(BCryptHashData(
                hash_, const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), 0))) {
            set_error(error, "Windows CNG SHA-256 读取数据失败");
            return false;
        }
        return true;
    }

    bool finish(std::string& output, std::string& error) {
        if (!nt_succeeded(BCryptFinishHash(
                hash_, digest_.data(),
                static_cast<ULONG>(digest_.size()), 0))) {
            set_error(error, "Windows CNG SHA-256 完成计算失败");
            return false;
        }
        constexpr char kHex[] = "0123456789abcdef";
        output.resize(digest_.size() * 2U);
        for (std::size_t index = 0; index < digest_.size(); ++index) {
            output[index * 2U] = kHex[digest_[index] >> 4U];
            output[index * 2U + 1U] = kHex[digest_[index] & 0x0FU];
        }
        return true;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
    std::vector<unsigned char> digest_;
};

bool compute_file_sha256(const std::filesystem::path& path,
                         std::string& output,
                         std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "无法打开 SHA-256 输入文件：" + path_to_utf8(path));
        return false;
    }
    Sha256Context context;
    if (!context.initialize(error)) return false;
    std::array<unsigned char, kHashBufferBytes> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && !context.update(
                std::span<const unsigned char>(
                    buffer.data(), static_cast<std::size_t>(count)), error)) {
            return false;
        }
    }
    if (!input.eof()) {
        set_error(error, "读取 SHA-256 输入文件失败：" + path_to_utf8(path));
        return false;
    }
    return context.finish(output, error);
}

bool compute_bgr_sha256(const cv::Mat& bgr,
                        std::string& output,
                        std::string& error) {
    Sha256Context context;
    if (!context.initialize(error)) return false;
    const std::size_t row_bytes = static_cast<std::size_t>(bgr.cols) *
        bgr.elemSize();
    for (int row = 0; row < bgr.rows; ++row) {
        if (!context.update(std::span<const unsigned char>(
                bgr.ptr<unsigned char>(row), row_bytes), error)) {
            return false;
        }
    }
    return context.finish(output, error);
}

std::int64_t steady_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count();
}

std::string frame_file_name(std::uint64_t index) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(6) << index << ".png";
    return output.str();
}

nlohmann::json capture_stages_json(const CaptureStageTiming& timing) {
    return {
        {"ndi_valid", timing.ndi_valid},
        {"runtime_handoff_valid", timing.runtime_handoff_valid},
        {"receive_call_ms", timing.receive_call_ms},
        {"metadata_ms", timing.metadata_ms},
        {"geometry_ms", timing.geometry_ms},
        {"pool_acquire_ms", timing.pool_acquire_ms},
        {"color_convert_ms", timing.color_convert_ms},
        {"performance_query_sampled", timing.performance_query_sampled},
        {"performance_query_ms", timing.performance_query_ms},
        {"queue_depth_sampled", timing.queue_depth_sampled},
        {"queue_query_ms", timing.queue_query_ms},
        {"queued_video_frames", timing.queued_video_frames},
        {"queued_audio_frames", timing.queued_audio_frames},
        {"queued_metadata_frames", timing.queued_metadata_frames},
        {"pool_publish_ms", timing.pool_publish_ms},
        {"runtime_capture_grab_ms", timing.runtime_capture_grab_ms},
        {"runtime_queue_publish_ms", timing.runtime_queue_publish_ms},
    };
}

bool write_text_file(const std::filesystem::path& path,
                     const std::string& content,
                     std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        set_error(error, "无法创建证据文件：" + path_to_utf8(path));
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
        set_error(error, "无法完整写入证据文件：" + path_to_utf8(path));
        return false;
    }
    return true;
}

} // namespace

class CaptureEvidenceRecorder::Impl {
public:
    enum class State { IDLE, RECORDING, PUBLISHED };

    struct BufferedFrame {
        cv::Mat bgr;
        nlohmann::json descriptor;
    };

    ~Impl() { cleanup_pending(); }

    void cleanup_pending() noexcept {
        if (pending_directory.empty()) return;
        std::error_code error;
        std::filesystem::remove_all(pending_directory, error);
        pending_directory.clear();
    }

    CaptureEvidenceConfig config;
    std::filesystem::path final_directory;
    std::filesystem::path pending_directory;
    std::string binding_sha256;
    std::vector<BufferedFrame> frames;
    std::uint64_t buffered_bgr_bytes = 0;
    State state = State::IDLE;
};

CaptureEvidenceRecorder::CaptureEvidenceRecorder() noexcept
    : impl_(new (std::nothrow) Impl) {}

CaptureEvidenceRecorder::~CaptureEvidenceRecorder() = default;

bool CaptureEvidenceRecorder::start(
        const CaptureEvidenceConfig& config, std::string& error) noexcept {
    try {
        error.clear();
        if (!impl_) {
            set_error(error, "无法分配 Capture evidence recorder 状态");
            return false;
        }
        if (impl_->state != Impl::State::IDLE) {
            set_error(error, "Capture evidence recorder 只能启动一次");
            return false;
        }
        if (config.output_directory.empty() ||
            config.source_binding_path.empty() ||
            (config.capture.backend == CaptureBackend::NDI &&
             config.capture.ndi_source_name.empty()) ||
            config.requested_frame_count == 0 ||
            config.requested_frame_count > kMaximumEvidenceFrames) {
            set_error(error, "Capture evidence 配置为空或超出 2400 帧短证据上限");
            return false;
        }

        std::error_code filesystem_error;
        if (!std::filesystem::is_regular_file(
                config.source_binding_path, filesystem_error) ||
            filesystem_error) {
            set_error(error, "source binding 不是可读普通文件：" +
                path_to_utf8(config.source_binding_path));
            return false;
        }
        impl_->final_directory = std::filesystem::absolute(
            config.output_directory, filesystem_error);
        if (filesystem_error || impl_->final_directory.filename().empty() ||
            impl_->final_directory == impl_->final_directory.root_path()) {
            set_error(error, "最终证据目录非法");
            return false;
        }
        if (std::filesystem::exists(
                impl_->final_directory, filesystem_error) || filesystem_error) {
            set_error(error, "最终证据目录已存在，拒绝覆盖：" +
                path_to_utf8(impl_->final_directory));
            return false;
        }
        const auto parent = impl_->final_directory.parent_path();
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            set_error(error, "无法创建证据父目录：" +
                path_to_utf8(parent));
            return false;
        }
        const std::uint64_t sequence = g_pending_sequence.fetch_add(
            1, std::memory_order_relaxed);
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        impl_->pending_directory = parent /
            (L"." + impl_->final_directory.filename().wstring() +
             L".incoming-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(suffix) + L"-" + std::to_wstring(sequence));
        std::filesystem::create_directories(
            impl_->pending_directory / "frames", filesystem_error);
        if (filesystem_error) {
            set_error(error, "无法创建证据 incoming 目录");
            impl_->cleanup_pending();
            return false;
        }

        const auto binding_destination =
            impl_->pending_directory / "source-binding.json";
        if (!std::filesystem::copy_file(
                config.source_binding_path, binding_destination,
                std::filesystem::copy_options::none, filesystem_error) ||
            filesystem_error ||
            !compute_file_sha256(
                binding_destination, impl_->binding_sha256, error)) {
            if (error.empty()) {
                set_error(error, "无法复制 source binding 到证据目录");
            }
            impl_->cleanup_pending();
            return false;
        }
        impl_->config = config;
        impl_->frames.clear();
        impl_->frames.reserve(
            static_cast<std::size_t>(config.requested_frame_count));
        impl_->buffered_bgr_bytes = 0;
        impl_->state = Impl::State::RECORDING;
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("启动 Capture evidence 异常：") +
            exception.what());
        if (impl_) impl_->cleanup_pending();
        return false;
    } catch (...) {
        set_error(error, "启动 Capture evidence 发生未知异常");
        if (impl_) impl_->cleanup_pending();
        return false;
    }
}

bool CaptureEvidenceRecorder::record(
        const CapturedFrame& frame, std::string& error) noexcept {
    try {
        error.clear();
        if (!impl_ || impl_->state != Impl::State::RECORDING) {
            set_error(error, "Capture evidence recorder 尚未启动");
            return false;
        }
        if (impl_->frames.size() >= impl_->config.requested_frame_count) {
            set_error(error, "Capture evidence 已录满声明帧数");
            return false;
        }
        if (frame.storage != CapturedFrameStorage::CPU_BGR ||
            frame.bgr.empty() || frame.bgr.type() != CV_8UC3 ||
            frame.width <= 0 || frame.height <= 0 ||
            frame.bgr.cols != frame.width || frame.bgr.rows != frame.height ||
            frame.source_width <= 0 || frame.source_height <= 0 ||
            frame.encoded_width <= 0 || frame.encoded_height <= 0 ||
            !std::isfinite(frame.roi_x) || !std::isfinite(frame.roi_y) ||
            !std::isfinite(frame.source_pixels_per_pixel_x) ||
            !std::isfinite(frame.source_pixels_per_pixel_y) ||
            frame.source_pixels_per_pixel_x <= 0.0 ||
            frame.source_pixels_per_pixel_y <= 0.0) {
            set_error(error, "只接受几何完整的 CPU_BGR Capture 帧");
            return false;
        }

        const auto frame_bytes = static_cast<std::uint64_t>(
            frame.bgr.total() * frame.bgr.elemSize());
        if (frame_bytes == 0 || frame_bytes > kMaximumBufferedBgrBytes ||
            impl_->buffered_bgr_bytes >
                kMaximumBufferedBgrBytes - frame_bytes) {
            set_error(error,
                "Capture evidence 超出 768 MiB 有界 BGR 缓冲上限");
            return false;
        }

        const std::uint64_t index = impl_->frames.size();
        const FrameTiming& timing = frame.timing;
        Impl::BufferedFrame buffered;
        buffered.bgr = frame.bgr.clone();
        if (buffered.bgr.empty()) {
            set_error(error, "无法复制 Capture evidence BGR 帧");
            return false;
        }
        buffered.descriptor = {
            {"sequence", timing.sequence},
            {"captured_at_steady_ns", steady_nanoseconds(timing.captured_at)},
            {"capture_ms", timing.capture_ms},
            {"capture_stages", capture_stages_json(timing.capture_stages)},
            {"source_dropped_frames", timing.source_dropped_frames},
            {"duplication_recoveries", timing.duplication_recoveries},
            {"transport_dropped_frames", timing.transport_dropped_frames},
            {"transport_invalid_packets", timing.transport_invalid_packets},
            {"source_received_frames", timing.source_received_frames},
            {"source_sequence", timing.source_sequence},
            {"source_sequence_valid", timing.source_sequence_valid},
            {"source_fps", timing.source_fps},
            {"source_timecode", timing.source_timecode},
            {"source_timecode_valid", timing.source_timecode_valid},
            {"source_timestamp", timing.source_timestamp},
            {"source_timestamp_valid", timing.source_timestamp_valid},
            {"source_time_basis", SourceTimeBasisName(timing.source_time_basis)},
            {"source_clock_status", SourceClockStatusName(timing.source_clock_status)},
            {"source_time_timing_valid", timing.source_time_timing_valid},
            {"source_time_at_steady_ns", steady_nanoseconds(timing.source_time_at)},
            {"source_clock_uncertainty_ms", timing.source_clock_uncertainty_ms},
            {"source_clock_round_trip_ms", timing.source_clock_round_trip_ms},
            {"source_clock_rate", timing.source_clock_rate},
            {"source_clock_mapping_age_ms", timing.source_clock_mapping_age_ms},
            {"source_clock_sample_count", timing.source_clock_sample_count},
            {"source_clock_session_id", timing.source_clock_session_id},
            {"storage", "CPU_BGR"},
            {"width", frame.width},
            {"height", frame.height},
            {"roi_x", frame.roi_x},
            {"roi_y", frame.roi_y},
            {"source_width", frame.source_width},
            {"source_height", frame.source_height},
            {"encoded_width", frame.encoded_width},
            {"encoded_height", frame.encoded_height},
            {"source_pixels_per_pixel_x", frame.source_pixels_per_pixel_x},
            {"source_pixels_per_pixel_y", frame.source_pixels_per_pixel_y},
        };
        impl_->frames.push_back(std::move(buffered));
        impl_->buffered_bgr_bytes += frame_bytes;
        return true;
    } catch (const cv::Exception& exception) {
        set_error(error, std::string("缓冲 Capture evidence BGR 失败：") +
            exception.what());
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("记录 Capture evidence 异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "记录 Capture evidence 发生未知异常");
        return false;
    }
}

bool CaptureEvidenceRecorder::finish(std::string& error) noexcept {
    try {
        error.clear();
        if (!impl_ || impl_->state != Impl::State::RECORDING) {
            set_error(error, "Capture evidence recorder 尚未启动");
            return false;
        }
        if (impl_->frames.size() != impl_->config.requested_frame_count) {
            set_error(error, "实际帧数未达到声明帧数，拒绝发布不完整证据");
            return false;
        }
        std::vector<nlohmann::json> manifest_frames;
        manifest_frames.reserve(impl_->frames.size());
        const std::vector<int> png_parameters{
            cv::IMWRITE_PNG_COMPRESSION, 3,
        };
        for (std::size_t index = 0; index < impl_->frames.size(); ++index) {
            auto& buffered = impl_->frames[index];
            const std::string file_name = frame_file_name(index);
            const auto path = impl_->pending_directory / "frames" / file_name;
            if (!cv::imwrite(path.string(), buffered.bgr, png_parameters)) {
                set_error(error, "无法写入无损 PNG：" + path_to_utf8(path));
                return false;
            }
            std::string png_sha256;
            std::string bgr_sha256;
            if (!compute_file_sha256(path, png_sha256, error) ||
                !compute_bgr_sha256(buffered.bgr, bgr_sha256, error)) {
                return false;
            }
            buffered.descriptor["index"] = index;
            buffered.descriptor["file"] = "frames/" + file_name;
            buffered.descriptor["png_sha256"] = std::move(png_sha256);
            buffered.descriptor["bgr_sha256"] = std::move(bgr_sha256);
            manifest_frames.push_back(buffered.descriptor);
        }
        nlohmann::json manifest{
            {"schema_version", 1},
            {"evidence_type", "output_off_capture"},
            {"physical_output_capability", false},
            {"capture_backend", CaptureBackendName(impl_->config.capture.backend)},
            {"capture_source_name", impl_->config.capture.ndi_source_name},
            {"capture_config", {
                {"frame_layout", NetworkFrameLayoutName(
                    impl_->config.capture.ndi_frame_layout)},
                {"source_width", impl_->config.capture.ndi_source_width},
                {"source_height", impl_->config.capture.ndi_source_height},
                {"roi_width", impl_->config.capture.roi_width},
                {"roi_height", impl_->config.capture.roi_height},
                {"center_roi", impl_->config.capture.center_roi},
                {"roi_x", impl_->config.capture.roi_x},
                {"roi_y", impl_->config.capture.roi_y},
                {"discovery_timeout_ms",
                    impl_->config.capture.ndi_discovery_timeout_ms},
                {"receive_timeout_ms",
                    impl_->config.capture.ndi_receive_timeout_ms},
                {"disconnect_timeout_ms",
                    impl_->config.capture.ndi_disconnect_timeout_ms},
                {"require_frame_metadata",
                    impl_->config.capture.ndi_require_frame_metadata},
                {"clock_sync_url",
                    impl_->config.capture.ndi_clock_sync_url},
                {"clock_sync_interval_ms",
                    impl_->config.capture.ndi_clock_sync_interval_ms},
                {"clock_sync_timeout_ms",
                    impl_->config.capture.ndi_clock_sync_timeout_ms},
                {"clock_mapping_max_age_ms",
                    impl_->config.capture.ndi_clock_mapping_max_age_ms},
                {"require_source_timing",
                    impl_->config.require_source_timing},
            }},
            {"requested_frame_count", impl_->config.requested_frame_count},
            {"recorded_frame_count", impl_->frames.size()},
            {"source_binding", {
                {"file", "source-binding.json"},
                {"sha256", impl_->binding_sha256},
            }},
            {"frames", std::move(manifest_frames)},
        };
        const auto pending_manifest =
            impl_->pending_directory / "manifest.json.pending";
        const auto manifest_path = impl_->pending_directory / "manifest.json";
        if (!write_text_file(
                pending_manifest, manifest.dump(2) + "\n", error)) {
            return false;
        }
        std::error_code filesystem_error;
        std::filesystem::rename(
            pending_manifest, manifest_path, filesystem_error);
        if (filesystem_error) {
            set_error(error, "无法完成 manifest 原子发布：" +
                filesystem_error.message());
            return false;
        }
        std::filesystem::rename(
            impl_->pending_directory, impl_->final_directory,
            filesystem_error);
        if (filesystem_error) {
            set_error(error, "无法完成证据目录原子发布：" +
                filesystem_error.message());
            return false;
        }
        impl_->pending_directory.clear();
        impl_->state = Impl::State::PUBLISHED;
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("发布 Capture evidence 异常：") +
            exception.what());
        return false;
    } catch (...) {
        set_error(error, "发布 Capture evidence 发生未知异常");
        return false;
    }
}

std::uint64_t CaptureEvidenceRecorder::recorded_frame_count() const noexcept {
    return impl_ ? static_cast<std::uint64_t>(impl_->frames.size()) : 0;
}

} // namespace capture_evidence
