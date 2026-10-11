#ifndef XEN_EXE_RUNTIME_FIXTURE_H
#define XEN_EXE_RUNTIME_FIXTURE_H

// 本头先于 Windows.h 引入，避免旧 Winsock 声明与 Winsock2 冲突。
#include <WinSock2.h>
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace xen::tests {

// 真实应用的进程外输入夹具：只在回环发送合成图像，通过既有报告读取生产状态。
// 不注入 Runtime/Capture，不调用任何键鼠设备接口，也不替代应用的停止路径。
class RuntimeFixture {
public:
    using Json = nlohmann::json;

    RuntimeFixture(std::filesystem::path output, std::filesystem::path model)
        : output_(std::move(output)), model_(std::move(model)) {}

    ~RuntimeFixture() {
        stop_sender();
        if (winsock_ready_) WSACleanup();
    }

    RuntimeFixture(const RuntimeFixture&) = delete;
    RuntimeFixture& operator=(const RuntimeFixture&) = delete;

    void prepare(AppConfig& config) {
        check(!prepared_, "Runtime 输入夹具不能重复准备");
        check(output_.is_absolute() && std::filesystem::is_directory(output_),
            "Runtime 输入夹具需要已创建的独立绝对目录");
        check(model_.is_absolute() && std::filesystem::is_regular_file(model_),
            "Runtime 输入夹具需要本地 ONNX 模型绝对路径");
        WSADATA data{};
        check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Runtime 回环 Winsock 初始化失败");
        winsock_ready_ = true;
        port_ = reserve_port();
        const auto models = output_ / "models";
        std::filesystem::create_directory(models);
        copied_model_ = models / "runtime-shutdown-fixture.onnx";
        check(std::filesystem::copy_file(model_, copied_model_), "Runtime 测试模型复制失败");

        config.detector.backend = BackendType::CPU;
        config.detector.model_path = "runtime-shutdown-fixture.onnx";
        config.detector.enable_fp16 = false;
        config.capture.backend = CaptureBackend::UDP_MJPEG;
        config.capture.udp_url = "udp://127.0.0.1:" + std::to_string(port_);
        config.capture.udp_read_timeout_ms = 50;
        config.capture.udp_disconnect_timeout_ms = 2000;
        config.capture.udp_frame_layout = UdpFrameLayout::FULL_FRAME_1_TO_1;
        // 完整帧 1:1 从 JPEG 自身取得尺寸，生产配置契约要求源尺寸留零。
        config.capture.udp_source_width = config.capture.udp_source_height = 0;
        config.capture.roi_width = config.capture.roi_height = 320;
        config.capture.center_roi = true;
        config.capture.acquire_timeout_ms = 20;
        config.capture.enable_d3d11_cuda_interop = false;
        config.capture.enable_d3d11_directml_interop = false;
        config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
        config.mouse.allow_send_input = false;
        config.auto_stop.enabled = config.auto_stop.cycle_enabled = false;
        config.trigger.enabled = config.trigger.fire_enabled = false;
        config.recoil.enabled = config.movement.enabled = false;
        config.gsi.enabled = config.source_context.enabled = false;
        config.keyboard.aim_hold_virtual_keys.clear();
        config.keyboard.runtime_toggle_virtual_keys.clear();
        config.keyboard.emergency_virtual_keys.clear();
        config.keyboard.anomaly_mark_virtual_keys.clear();
        config.keyboard.debug_test_virtual_keys.clear();
        config.keyboard.debug_test_enabled = false;
        config.runtime.diagnostics_enabled = true;
        config.runtime.enable_performance_probes = false;
        assert_safe(config);
        std::string error;
        const bool valid_config = validate_app_config(config, error);
        check(valid_config, "Runtime 测试配置无效：" + error);
        const cv::Mat frame(320, 320, CV_8UC3, cv::Scalar(24, 96, 208));
        check(cv::imencode(".jpg", frame, jpeg_) && !jpeg_.empty() && jpeg_.size() <= 65507,
            "Runtime 合成 JPEG 生成失败或超出单个 UDP 数据报容量");
        prepared_ = true;
    }

    static void assert_safe(const AppConfig& config) {
        check(config.mouse.backend == MouseBackend::WIN32_SEND_INPUT && !config.mouse.allow_send_input &&
            !config.auto_stop.enabled && !config.auto_stop.cycle_enabled && !config.trigger.enabled &&
            !config.trigger.fire_enabled && !config.recoil.enabled && !config.movement.enabled &&
            !config.gsi.enabled && !config.source_context.enabled && !config.keyboard.debug_test_enabled &&
            config.keyboard.aim_hold_virtual_keys.empty() && config.keyboard.runtime_toggle_virtual_keys.empty() &&
            config.keyboard.emergency_virtual_keys.empty() && config.keyboard.anomaly_mark_virtual_keys.empty() &&
            config.keyboard.debug_test_virtual_keys.empty(), "Runtime 测试拒绝非隔离或允许物理输出的配置");
        check(config.detector.backend == BackendType::CPU && config.capture.backend == CaptureBackend::UDP_MJPEG &&
            config.capture.udp_url.starts_with("udp://127.0.0.1:") &&
            !config.capture.enable_d3d11_cuda_interop && !config.capture.enable_d3d11_directml_interop &&
            config.runtime.diagnostics_enabled, "Runtime 测试只接受有诊断的 CPU 回环输入");
    }

    void start_sender() {
        check(prepared_ && !sender_.joinable() && !sender_started_, "Runtime 合成发送器准备状态无效");
        sender_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        check(sender_socket_ != INVALID_SOCKET, "Runtime 合成发送器 socket 创建失败");
        const DWORD timeout = 100;
        if (setsockopt(sender_socket_, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
            closesocket(sender_socket_); sender_socket_ = INVALID_SOCKET;
            throw std::runtime_error("Runtime 合成发送器超时配置失败");
        }
        sender_stop_.store(false);
        try {
            sender_ = std::thread([this] {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                destination.sin_port = htons(port_);
                while (!sender_stop_.load()) {
                    const int sent = sendto(sender_socket_, reinterpret_cast<const char*>(jpeg_.data()),
                        static_cast<int>(jpeg_.size()), 0, reinterpret_cast<const sockaddr*>(&destination),
                        sizeof(destination));
                    if (sent != static_cast<int>(jpeg_.size())) {
                        sender_error_.store(WSAGetLastError());
                        sender_failed_.store(true);
                        break;
                    }
                    sent_frames_.fetch_add(1);
                    std::unique_lock lock(sender_mutex_);
                    sender_wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return sender_stop_.load(); });
                }
                sender_finished_.store(true);
            });
            sender_started_ = true;
        } catch (...) {
            closesocket(sender_socket_); sender_socket_ = INVALID_SOCKET;
            throw;
        }
    }

    void stop_sender() noexcept {
        sender_stop_.store(true);
        sender_wake_.notify_all();
        if (sender_.joinable()) sender_.join();
        if (sender_socket_ != INVALID_SOCKET) {
            closesocket(sender_socket_);
            sender_socket_ = INVALID_SOCKET;
        }
        sender_joined_ = sender_started_;
    }

    Json sender_evidence() const {
        return {{"input_kind", "synthetic_jpeg"}, {"destination", "127.0.0.1"}, {"port", port_},
            {"period_ms", 20}, {"sent_frames", sent_frames_.load()}, {"started", sender_started_},
            {"finished", sender_finished_.load()}, {"joined", sender_joined_},
            {"failed", sender_failed_.load()}, {"winsock_error", sender_error_.load()},
            {"physical_output_allowed", false}};
    }

    Json poll_running(DWORD process_id) {
        check(!sender_failed_.load(), "Runtime 合成回环发送失败");
        // 即使没有新的归档段，也不能把已封尾会话的旧 RUNNING 样本当成当前活跃。
        check(session_id_.empty() || !std::filesystem::exists(output_ / "cache/runtime" / (session_id_ + ".json")),
            "主窗口关闭前 Runtime 会话已经封尾");
        const auto now = std::chrono::steady_clock::now();
        if (last_poll_ != std::chrono::steady_clock::time_point{} &&
            now - last_poll_ < std::chrono::milliseconds(200)) return running_evidence();
        last_poll_ = now;
        const auto root = output_ / "cache/runtime";
        if (!std::filesystem::exists(root)) return running_evidence();
        std::vector<std::pair<std::uint64_t, std::filesystem::path>> segments;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory() || !entry.path().filename().string().ends_with("-archive")) continue;
            for (const auto& segment : std::filesystem::directory_iterator(entry.path())) {
                const auto name = segment.path().filename().string();
                if (!segment.is_regular_file() || !name.starts_with("segment-") || !name.ends_with(".meta.json")) continue;
                const auto meta = read_json(segment.path());
                check(meta.at("published").get<bool>(), "Runtime 分段元数据未确认发布");
                const auto path = entry.path() / meta.at("json").get<std::string>();
                check(path.parent_path() == entry.path(), "Runtime 分段路径超出本次归档");
                if (!seen_.contains(path)) segments.emplace_back(meta.at("segment").get<std::uint64_t>(), path);
            }
        }
        std::sort(segments.begin(), segments.end());
        for (const auto& [number, path] : segments) {
            const auto report = read_json(path);
            validate_common(report, process_id);
            const auto& snapshot = report.at("final_snapshot");
            check(snapshot.at("runtime_state") == "RUNNING", "关闭请求前 Runtime 分段状态不是 RUNNING");
            const auto processed = snapshot.at("processed_frames").get<std::uint64_t>();
            const auto sequence = snapshot.at("last_sequence").get<std::uint64_t>();
            check(processed > last_processed_ && sequence > last_sequence_ && report.at("sample_count").get<std::uint64_t>() > 0,
                "Runtime 已发布分段没有持续处理新帧");
            if (session_id_.empty()) session_id_ = report.at("session_id").get<std::string>();
            check(report.at("session_id") == session_id_, "Runtime 测试发生未请求的会话重启");
            last_processed_ = processed;
            last_sequence_ = sequence;
            observations_.push_back({{"report", path.string()}, {"segment", number},
                {"processed_frames", processed}, {"last_sequence", sequence},
                {"runtime_state", "RUNNING"}, {"sample_count", report.at("sample_count")}});
            seen_.insert(path);
        }
        return running_evidence();
    }

    Json validate_stopped(DWORD process_id) const {
        check(observations_.size() >= 2 && !session_id_.empty(), "缺少关闭前 Runtime 持续运行见证");
        check(!sender_failed_.load(), "Runtime 合成回环发送失败");
        const auto root = output_ / "cache/runtime";
        const auto report_path = root / (session_id_ + ".json");
        const auto report = read_json(report_path);
        validate_common(report, process_id);
        check(report.at("session_id") == session_id_, "Runtime 终态报告会话身份不一致");
        const auto& snapshot = report.at("final_snapshot");
        check(snapshot.at("runtime_state") == "STOPPED" && snapshot.at("capture_status") == "CLOSED" &&
            snapshot.at("mouse_status") == "CLOSED" && snapshot.at("emergency_stopped").get<bool>(),
            "Runtime 终态未完成停止和模块回收");
        check(snapshot.at("processed_frames").get<std::uint64_t>() >= last_processed_ &&
            snapshot.at("last_sequence").get<std::uint64_t>() >= last_sequence_, "Runtime 终态帧水位回退");
        check(snapshot.at("captured_frames").get<std::uint64_t>() >=
            snapshot.at("processed_frames").get<std::uint64_t>(), "Runtime 终态处理帧数超出已采集帧数");
        check(std::filesystem::is_regular_file(root / (session_id_ + ".csv")), "Runtime 最终 CSV 未发布");
        const auto manifest_path = root / (session_id_ + "-archive") / "manifest.json";
        const auto manifest = read_json(manifest_path);
        check(manifest.at("session_id") == session_id_ && manifest.at("closed").get<bool>() &&
            manifest.at("complete").get<bool>(), "Runtime 全程归档未完整封尾");
        check(manifest.at("accepted_samples").get<std::uint64_t>() > 0 &&
            manifest.at("accepted_samples") == manifest.at("written_samples") &&
            manifest.at("last_error") == "", "Runtime 全程归档未写完已接收样本");
        for (const auto* name : {"dropped_samples", "dropped_batches", "trigger_events_dropped", "runtime_samples_dropped"})
            check(manifest.at(name).get<std::uint64_t>() == 0, std::string("Runtime 归档存在丢失：") + name);
        return {{"report", report_path.string()}, {"manifest", manifest_path.string()},
            {"session_id", session_id_}, {"final_snapshot", snapshot},
            {"sample_count", report.at("sample_count")}, {"successful_samples", report.at("successful_samples")},
            {"archive", manifest}, {"sender", sender_evidence()}};
    }

private:
    static void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    static Json read_json(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        check(input.good(), "无法读取已发布 Runtime 报告：" + path.string());
        return Json::parse(input);
    }

    void validate_common(const Json& report, DWORD process_id) const {
        check(report.at("schema") == 20 && report.at("provider") == "CPUExecutionProvider" &&
            report.at("capture_backend") == "UDP_MJPEG" && report.at("mouse_backend") == "win32_send_input",
            "Runtime 报告未使用声明的 CPU/回环/Win32 后端");
        check(report.at("session_id").get<std::string>().starts_with(std::to_string(process_id) + "-"),
            "Runtime 报告不属于本次 Xen.exe 进程");
        check(std::filesystem::equivalent(std::filesystem::u8path(report.at("model_path").get<std::string>()), copied_model_),
            "Runtime 报告模型不是本次复制的独立 fixture");
        const auto& snapshot = report.at("final_snapshot");
        const auto samples = report.at("sample_count").get<std::uint64_t>();
        const auto successful = report.at("successful_samples").get<std::uint64_t>();
        const auto processed = snapshot.at("processed_frames").get<std::uint64_t>();
        check(snapshot.at("detection_status") == "SUCCESS" && samples > 0 &&
            successful > 0 && successful == samples && processed > 0 &&
            snapshot.at("captured_frames").get<std::uint64_t>() > 0 &&
            snapshot.at("last_sequence").get<std::uint64_t>() >= processed,
            "Runtime 报告没有成功推理或帧计数无效");
        check(!snapshot.at("output_allowed_by_config").get<bool>() && !snapshot.at("output_armed").get<bool>() &&
            snapshot.at("mouse_commands").get<std::uint64_t>() == 0, "Runtime 报告存在物理输出许可或已发送命令");
        check(snapshot.at("failed_frames").get<std::uint64_t>() == 0 && snapshot.at("last_error") == "" &&
            report.at("failed_samples").get<std::uint64_t>() == 0 &&
            report.at("runtime_samples_dropped").get<std::uint64_t>() == 0,
            "Runtime 处理存在失败或诊断样本丢失");
    }

    Json running_evidence() const {
        return {{"ready", observations_.size() >= 2}, {"session_id", session_id_},
            {"observations", observations_}, {"last_processed_frames", last_processed_},
            {"last_sequence", last_sequence_}, {"sender", sender_evidence()}};
    }

    static unsigned short reserve_port() {
        const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        check(handle != INVALID_SOCKET, "Runtime 回环端口 socket 创建失败");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        const bool ok = bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR &&
            getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != SOCKET_ERROR;
        closesocket(handle);
        check(ok && address.sin_port != 0, "Runtime 临时回环端口取得失败");
        // 与现有 Runtime UDP 集成测试一致：接收端实际绑定失败仍由应用报告并使验证失败。
        return ntohs(address.sin_port);
    }

    std::filesystem::path output_, model_, copied_model_;
    bool prepared_ = false, winsock_ready_ = false, sender_started_ = false, sender_joined_ = false;
    unsigned short port_ = 0;
    SOCKET sender_socket_ = INVALID_SOCKET;
    std::vector<unsigned char> jpeg_;
    std::thread sender_;
    std::atomic<bool> sender_stop_{false}, sender_finished_{false}, sender_failed_{false};
    std::atomic<int> sender_error_{0};
    std::atomic<std::uint64_t> sent_frames_{0};
    std::mutex sender_mutex_;
    std::condition_variable sender_wake_;
    std::chrono::steady_clock::time_point last_poll_{};
    std::set<std::filesystem::path> seen_;
    std::string session_id_;
    std::uint64_t last_processed_ = 0, last_sequence_ = 0;
    Json observations_ = Json::array();
};

} // namespace xen::tests

#endif
