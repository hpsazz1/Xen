#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include "log/log.h"
#include "runtime/runtime.h"
#include "debug/session_archive.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class WinsockSession final {
public:
    WinsockSession() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockSession() {
        if (ready_) WSACleanup();
    }

    bool ready() const noexcept { return ready_; }

private:
    bool ready_ = false;
};

// 复用 Runtime 的共享设备入口；配置校验仍走生产路径，绝不连接 KMBOX。
class NoOutputMouse final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool poll_input(InputSnapshot& value) noexcept override { value = {}; return false; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override {
        std::unique_lock lock(status_mutex_);
        if (entered_status_calls_ < held_status_calls_) {
            const unsigned ticket = ++entered_status_calls_;
            status_wake_.notify_all();
            if (!status_wake_.wait_for(lock, 30s, [&] {
                    return released_status_calls_ >= ticket;
                })) {
                status_barrier_timed_out.store(true);
            }
        }
        return MouseStatus::DISABLED;
    }
    std::string last_error() const override { return {}; }
    std::atomic<unsigned> moves{0};
    mutable std::atomic<bool> status_barrier_timed_out{false};

    // 启动完成后，status() 只由 Pipeline 在诊断发布前调用；双屏障分别
    // 固定重载前的在途帧和下一帧，不依赖 sleep 碰撞线程调度窗口。
    void hold_status_calls(unsigned count) {
        std::lock_guard lock(status_mutex_);
        held_status_calls_ = count;
    }
    bool wait_for_status(unsigned ticket) {
        std::unique_lock lock(status_mutex_);
        return status_wake_.wait_for(lock, 10s, [&] {
            return entered_status_calls_ >= ticket;
        });
    }
    void release_status(unsigned ticket) {
        std::lock_guard lock(status_mutex_);
        released_status_calls_ = ticket;
        status_wake_.notify_all();
    }

private:
    mutable std::mutex status_mutex_;
    mutable std::condition_variable status_wake_;
    unsigned held_status_calls_ = 0;
    mutable unsigned entered_status_calls_ = 0;
    unsigned released_status_calls_ = 0;
};

unsigned short reserve_loopback_port() noexcept {
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    int address_size = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address),
                    &address_size) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    closesocket(socket_handle);
    return ntohs(address.sin_port);
}

bool send_fragmented_jpeg(SOCKET socket_handle,
                          const sockaddr_in& destination,
                          const std::vector<unsigned char>& jpeg) noexcept {
    if (jpeg.size() < 4) return false;
    const std::size_t middle_size = jpeg.size() - 2;
    const int first = sendto(
        socket_handle, reinterpret_cast<const char*>(jpeg.data()), 1, 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    const int middle = sendto(
        socket_handle, reinterpret_cast<const char*>(jpeg.data() + 1),
        static_cast<int>(middle_size), 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    const int last = sendto(
        socket_handle,
        reinterpret_cast<const char*>(jpeg.data() + jpeg.size() - 1), 1, 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    return first == 1 && middle == static_cast<int>(middle_size) && last == 1;
}

template <typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position =
        quantile * static_cast<double>(values.size() - 1);
    const std::size_t lower =
        static_cast<std::size_t>(std::floor(position));
    const std::size_t upper =
        static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) return values[lower];
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

void check_inflight_reload_generation(AppConfig config, bool probes_enabled) {
    config.runtime.enable_performance_probes = probes_enabled;
    config.runtime.diagnostics_enabled = false;
    auto mouse = std::make_shared<NoOutputMouse>();
    Runtime runtime;
    const bool started = runtime.start(config, mouse);
    expect(started, "代际屏障回归必须启动真实 CPU Detector 与 UDP Capture");
    if (!started) return;
    expect(wait_until([&] { return runtime.snapshot().processed_frames >= 3; }, 10s),
           "代际屏障回归必须先完成初始帧");
    mouse->hold_status_calls(2);
    const bool old_frame_held = mouse->wait_for_status(1);
    expect(old_frame_held, "旧模型在途帧必须停在诊断发布前");
    if (old_frame_held) {
        const auto before_reload = runtime.snapshot();
        expect(runtime.reload_detector(config.detector), "屏障内必须接受真实模型重载");
        const bool reloaded = wait_until([&] {
            const auto value = runtime.snapshot();
            return value.detector_reload_state == DetectorReloadState::SUCCEEDED &&
                value.detector_generation == before_reload.detector_generation + 1;
        }, 20s);
        expect(reloaded, "旧帧尚未发布时新模型必须完成切换");
        expect(runtime.snapshot().processed_frames == before_reload.processed_frames,
               "重载完成前旧帧必须仍由屏障持有");
        runtime.set_diagnostics_enabled(true);
        mouse->release_status(1);
        const bool next_frame_held = mouse->wait_for_status(2);
        expect(next_frame_held, "新模型首帧必须停在诊断发布前");
        const auto after_old_frame = runtime.snapshot();
        expect(after_old_frame.processed_frames == before_reload.processed_frames + 1,
               "第二屏障前只允许旧帧完成，保留基本处理计数");
        std::vector<RuntimePipelineSample> samples;
        expect(runtime.drain_pipeline_samples(samples) && samples.empty(),
               "新模型诊断段不得接收重载前已完成推理的在途旧帧");
        mouse->release_status(2);
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >= after_old_frame.processed_frames + 3;
        }, 5s), "释放屏障后新模型必须继续发布样本");
        runtime.set_diagnostics_enabled(false);
        expect(runtime.drain_pipeline_samples(samples) && !samples.empty() &&
                   std::all_of(samples.begin(), samples.end(), [&](const auto& sample) {
                       return sample.sequence > after_old_frame.last_sequence &&
                           sample.service.valid == probes_enabled;
                   }), "新代模型必须保留同帧样本及对应性能探针完成语义");
    }
    mouse->release_status(2);
    runtime.stop();
    expect(!mouse->status_barrier_timed_out.load(), "代际回归屏障不得靠超时释放");
    expect(mouse->moves.load() == 0, "代际回归不得向假设备输出");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "用法：runtime_model_reload_test <模型路径>\n";
        return 2;
    }

    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_debug_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    if (!Log::initialized()) {
        std::cerr << "Log 初始化失败。\n";
        return 2;
    }

    WinsockSession winsock;
    expect(winsock.ready(), "Winsock 必须可用于 Runtime UDP 回环");
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "Runtime UDP 回环必须取得临时端口");

    cv::Mat source(320, 320, CV_8UC3, cv::Scalar(24, 96, 208));
    std::vector<unsigned char> jpeg;
    expect(cv::imencode(".jpg", source, jpeg) && !jpeg.empty(),
           "Runtime UDP 回环必须生成真实 JPEG 帧");

    std::atomic<bool> sender_running{winsock.ready() && port != 0 &&
                                     !jpeg.empty()};
    std::atomic<bool> sender_failed{false};
    std::thread sender;
    if (sender_running.load(std::memory_order_acquire)) {
        sender = std::thread([&] {
            const SOCKET socket_handle =
                socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_handle == INVALID_SOCKET) {
                sender_failed.store(true, std::memory_order_release);
                return;
            }
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            destination.sin_port = htons(port);
            while (sender_running.load(std::memory_order_acquire)) {
                if (!send_fragmented_jpeg(
                        socket_handle, destination, jpeg)) {
                    sender_failed.store(true, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(4ms);
            }
            closesocket(socket_handle);
        });
    }

    AppConfig config;
    config.detector.model_path = argv[1];
    config.detector.backend = BackendType::CPU;
    config.capture.backend = CaptureBackend::UDP_MJPEG;
    config.capture.udp_url =
        "udp://127.0.0.1:" + std::to_string(port);
    config.capture.udp_read_timeout_ms = 50;
    config.capture.udp_disconnect_timeout_ms = 1000;
    config.capture.udp_frame_layout =
        UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.capture.udp_source_width = 2560;
    config.capture.udp_source_height = 1440;
    config.capture.roi_width = 320;
    config.capture.roi_height = 320;
    config.capture.center_roi = true;
    config.capture.acquire_timeout_ms = 20;
    config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
    config.mouse.allow_send_input = false;
    config.runtime.enable_performance_probes = true;

    Runtime runtime;
    SessionArchive diagnostic_archive;
    const auto diagnostic_root = std::filesystem::temp_directory_path() /
        ("xen-runtime-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const bool started = runtime.start(config);
    expect(started, "Runtime 必须使用真实 CPU 模型和 UDP Capture 启动" +
                        (runtime.snapshot().last_error.empty()
                             ? std::string()
                             : ": " + runtime.snapshot().last_error));

    if (started) {
        const bool initial_frames = wait_until([&] {
            return runtime.snapshot().processed_frames >= 3;
        }, 10s);
        expect(initial_frames, "初始 Detector 必须持续处理 UDP 帧");
        std::vector<RuntimePipelineSample> diagnostics;
        expect(runtime.drain_pipeline_samples(diagnostics) && diagnostics.empty(),
               "默认关闭详细记录时，真实生产链不积累逐帧样本");
        expect(!std::filesystem::exists(diagnostic_root), "默认关闭不会创建诊断文件");

        RuntimeSnapshot snapshot = runtime.snapshot();
        expect(snapshot.state == RuntimeState::RUNNING,
               "初始处理后 Runtime 必须保持 RUNNING");
        expect(snapshot.detector_generation == 1 &&
                   snapshot.active_model_path == config.detector.model_path,
               "初始模型必须记录为第 1 代活动模型");
        expect(snapshot.provider == "CPUExecutionProvider",
               "集成回归必须实际使用 CPUExecutionProvider");
        expect(snapshot.encoded_width == 320 &&
                   snapshot.encoded_height == 320 &&
                   snapshot.source_width == 2560 &&
                   snapshot.source_height == 1440 &&
                   snapshot.capture_roi_width == 320 &&
                   snapshot.capture_roi_height == 320 &&
                   snapshot.capture_roi_x == 1120.0 &&
                   snapshot.capture_roi_y == 560.0 &&
                   snapshot.source_pixels_per_pixel_x == 1.0 &&
                   snapshot.source_pixels_per_pixel_y == 1.0,
               "双机语义必须保持主机 2560x1440 的中心 320x320 ROI");

        DetectorConfig invalid_config = config.detector;
        invalid_config.model_path =
            config.detector.model_path + ".xen-missing";
        expect(!std::filesystem::exists(invalid_config.model_path),
               "失败回滚测试路径必须不存在");
        const std::uint64_t generation_before_failure =
            snapshot.detector_generation;
        expect(runtime.reload_detector(invalid_config),
               "不存在的模型路径应作为异步重载请求被接受");
        const bool failed_reload = wait_until([&] {
            return runtime.snapshot().detector_reload_state ==
                   DetectorReloadState::FAILED;
        }, 10s);
        expect(failed_reload, "不存在的模型必须明确进入 FAILED");
        snapshot = runtime.snapshot();
        const std::uint64_t processed_after_failure =
            snapshot.processed_frames;
        expect(snapshot.state == RuntimeState::RUNNING &&
                   snapshot.detector_generation == generation_before_failure &&
                   snapshot.active_model_path == config.detector.model_path &&
                   !snapshot.detector_reload_error.empty(),
               "重载失败必须保留旧模型、代次和 RUNNING 状态");
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >=
                   processed_after_failure + 3;
        }, 5s), "重载失败后旧 Detector 必须继续处理帧");

        const std::uint64_t processed_before_success =
            runtime.snapshot().processed_frames;
        expect(runtime.reload_detector(config.detector),
               "同一真实模型的第二代加载请求必须被接受");
        const bool successful_reload = wait_until([&] {
            const RuntimeSnapshot current = runtime.snapshot();
            return current.detector_reload_state ==
                       DetectorReloadState::SUCCEEDED &&
                   current.detector_generation == 2;
        }, 20s);
        expect(successful_reload, "真实模型必须成功切换到第 2 代");
        snapshot = runtime.snapshot();
        expect(snapshot.state == RuntimeState::RUNNING &&
                   snapshot.active_model_path == config.detector.model_path &&
                   snapshot.provider == "CPUExecutionProvider" &&
                   snapshot.detector_reload_error.empty() &&
                   !snapshot.output_armed,
               "成功切换必须发布实际模型/Provider 并保持输出解除武装");
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >=
                   processed_before_success + 3;
        }, 5s), "成功切换期间 Pipeline 必须继续前进");
        expect(runtime.drain_pipeline_samples(diagnostics) && diagnostics.empty(),
               "模型热重载不能重新开启已关闭的详细记录");
        runtime.set_diagnostics_enabled(true);
        SessionArchiveConfig archive_config;
        archive_config.directory = diagnostic_root.string();
        archive_config.report_config.session_id = "runtime-recording-toggle";
        std::string archive_error;
        expect(diagnostic_archive.start(archive_config, archive_error), "显式开启生产归档");

        // 先让新 Session 经过与正式样本相同的 16 帧完整链路预热，再用生产
        // 诊断样本建立端到端基线；失败帧不混入分位数。
        const std::uint64_t warmup_started_at =
            runtime.snapshot().processed_frames;
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >=
                   warmup_started_at + 16;
        }, 5s), "热重载后必须完成同链路预热");
        std::vector<RuntimePipelineSample> discarded_samples;
        runtime.drain_pipeline_samples(discarded_samples);
        const std::uint64_t profile_started_at =
            runtime.snapshot().processed_frames;
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >=
                   profile_started_at + 256;
        }, 15s), "热重载后必须取得足够的端到端性能样本");
        std::vector<RuntimePipelineSample> profile_samples;
        expect(runtime.drain_pipeline_samples(profile_samples),
               "Runtime 必须允许取出热重载后的诊断样本");
        expect(wait_until([&] { return diagnostic_archive.submit(profile_samples, runtime.snapshot()); }, 2s),
               "开启后实际逐帧数据提交归档");
        std::vector<double> successful_total_ms;
        bool landmark_contract_valid = true;
        bool processed_timing_valid = true;
        for (const auto& sample : profile_samples) {
            if (sample.detection_status == DetectionStatus::SUCCESS &&
                sample.aim_status == AimStatus::SUCCESS) {
                successful_total_ms.push_back(sample.profile.total_ms);
                const auto& timing = sample.frame_timing;
                processed_timing_valid = processed_timing_valid &&
                    timing.capture_steady_valid && timing.observation_steady_valid &&
                    timing.control_steady_valid && !timing.source_steady_valid &&
                    !timing.source_sequence_valid && !timing.source_timestamp_valid &&
                    timing.observation_steady_ns == timing.capture_steady_ns &&
                    timing.control_steady_ns >= timing.capture_steady_ns &&
                    std::fabs(static_cast<double>(
                        timing.control_steady_ns - timing.capture_steady_ns) /
                            1000000.0 - sample.profile.capture_to_control_ms) < 1e-6;
                landmark_contract_valid = landmark_contract_valid &&
                    sample.aim_landmark.sequence == sample.sequence &&
                    sample.aim_landmark.status !=
                        aim_landmark::Status::INVALID_INPUT &&
                    !sample.aim_landmark.control_eligible;
            }
        }
        expect(landmark_contract_valid,
               "Runtime 成功样本必须发布同 sequence 且不可接控制的 landmark");
        expect(processed_timing_valid,
               "关闭输出的UDP真实Pipeline必须保存同帧Capture/Aim时间，缺少源时钟时保持无效");
        expect(successful_total_ms.size() >= 128,
               "端到端分位数必须排除失败帧并保留至少 128 个成功样本");
        if (!successful_total_ms.empty()) {
            const double mean_ms = std::accumulate(
                successful_total_ms.begin(), successful_total_ms.end(),
                0.0) / static_cast<double>(successful_total_ms.size());
            std::cout << "热重载后端到端样本="
                      << successful_total_ms.size()
                      << ", 失败="
                      << profile_samples.size() - successful_total_ms.size()
                      << ", Mean=" << mean_ms
                      << " ms, P50="
                      << percentile(successful_total_ms, 0.50)
                      << " ms, P95="
                      << percentile(successful_total_ms, 0.95)
                      << " ms, P99="
                      << percentile(successful_total_ms, 0.99)
                      << " ms, Max="
                      << *std::max_element(
                             successful_total_ms.begin(),
                             successful_total_ms.end())
                      << " ms\n";
        }
    }

    runtime.set_diagnostics_enabled(false);
    std::vector<RuntimePipelineSample> closing_samples;
    runtime.drain_pipeline_samples(closing_samples);
    if (started) {
        expect(wait_until([&] { return diagnostic_archive.submit(closing_samples, runtime.snapshot()); }, 2s),
               "关闭只封尾已接收数据");
        diagnostic_archive.stop();
        expect(diagnostic_archive.status().written_samples > 0 &&
                   diagnostic_archive.status().last_error.empty(), "生产CSV与JSON实际写盘成功");
    }
    const auto archive_bytes = [&] {
        std::uintmax_t bytes = 0;
        if (std::filesystem::exists(diagnostic_root))
            for (const auto& entry : std::filesystem::recursive_directory_iterator(diagnostic_root))
                if (entry.is_regular_file()) bytes += entry.file_size();
        return bytes;
    };
    const auto closed_bytes = archive_bytes();
    const auto before_disabled = runtime.snapshot().processed_frames;
    expect(wait_until([&] { return runtime.snapshot().processed_frames >= before_disabled + 8; }, 5s),
           "关闭记录不停止生产链");
    expect(runtime.drain_pipeline_samples(closing_samples) && closing_samples.empty(),
           "动态关闭后诊断队列停止增长");
    expect(archive_bytes() == closed_bytes, "关闭封尾后实际文件字节数停止增长");
    const auto reopen_sequence = runtime.snapshot().last_sequence;
    runtime.set_diagnostics_enabled(true);
    expect(wait_until([&] { return runtime.snapshot().processed_frames >= before_disabled + 16; }, 5s),
           "重新开启后生产链继续运行");
    runtime.set_diagnostics_enabled(false);
    expect(runtime.drain_pipeline_samples(closing_samples) && !closing_samples.empty() &&
               std::all_of(closing_samples.begin(), closing_samples.end(), [&](const auto& sample) {
                   return sample.sequence > reopen_sequence && sample.service.valid;
               }), "性能探针两阶段样本须在关闭前完成，重新开启不补旧段尾样本");
    std::error_code archive_cleanup_error;
    std::filesystem::remove_all(diagnostic_root, archive_cleanup_error);
    runtime.stop();
    const RuntimeSnapshot stopped = runtime.snapshot();
    expect(stopped.state == RuntimeState::STOPPED &&
               stopped.detector_reload_state == DetectorReloadState::IDLE,
           "stop() 必须回收重载线程并恢复 IDLE");
    expect(!runtime.reload_detector(config.detector),
           "Runtime 停止后必须拒绝 Detector 重载");

    check_inflight_reload_generation(config, false);
    check_inflight_reload_generation(config, true);

    // 只移除测试进程继承的凭据，不读取或记录部署秘密；桥接故障不得阻断观测链。
    expect(_putenv_s("XEN_SOURCE_CONTEXT_TOKEN", "") == 0,
           "测试进程必须移除源状态桥接认证环境");
    config.source_context.enabled = true;
    config.source_context.host = "127.0.0.1";
    config.source_context.port = 5012;
    config.source_context.process_name = "game.exe";
    config.source_context.token.clear();
    const bool degraded_started = runtime.start(config);
    expect(degraded_started, "缺少桥接认证时 Runtime 仍必须启动采集检测");
    if (degraded_started) {
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >= 3;
        }, 10s), "桥接启动失败后真实 CPU Detector 必须继续处理 UDP 帧");
        const RuntimeSnapshot degraded = runtime.snapshot();
        expect(degraded.state == RuntimeState::RUNNING &&
                   degraded.last_error.empty(),
               "桥接启动失败不得成为 Runtime 致命错误");
        expect(degraded.source_context_error.find("XEN_SOURCE_CONTEXT_TOKEN") !=
                   std::string::npos,
               "独立桥接告警必须指明缺少认证环境变量");
        expect(!degraded.source_context.available &&
                   !degraded.source_context.focused && !degraded.output_armed,
               "桥接故障不得伪造前台证据或自动武装输出");
    }
    runtime.stop();

    config.source_context.enabled = false;
    const SOCKET occupied_gsi = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in occupied_address{};
    occupied_address.sin_family = AF_INET;
    occupied_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL exclusive = TRUE;
    const bool occupied_bound = occupied_gsi != INVALID_SOCKET &&
        setsockopt(occupied_gsi, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0 &&
        bind(occupied_gsi, reinterpret_cast<const sockaddr*>(&occupied_address), sizeof(occupied_address)) == 0;
    int occupied_size = sizeof(occupied_address);
    const bool occupied_ready = occupied_bound &&
        getsockname(occupied_gsi, reinterpret_cast<sockaddr*>(&occupied_address), &occupied_size) == 0;
    expect(occupied_ready, "GSI 故障夹具必须独占临时 TCP 端口");
    if (occupied_ready) {
        config.gsi.enabled = true;
        config.gsi.bind_address = "127.0.0.1";
        config.gsi.port = ntohs(occupied_address.sin_port);
        config.team_filter.enabled = true;
        config.team_filter.ct_class_ids = {0};
        config.team_filter.t_class_ids = {1};
        const bool gsi_degraded_started = runtime.start(config);
        expect(gsi_degraded_started, "GSI 端口占用不得阻断 Runtime 采集检测");
        if (gsi_degraded_started) {
            expect(wait_until([&] { return runtime.snapshot().processed_frames >= 3; }, 10s),
                   "GSI 失效后真实 CPU Detector 继续处理 UDP 帧");
            const auto degraded = runtime.snapshot();
            expect(degraded.state == RuntimeState::RUNNING && degraded.last_error.empty() &&
                       !degraded.gsi_error.empty(), "GSI 失败须保留独立告警");
            expect(!degraded.weapon_snapshot.valid && !degraded.output_armed &&
                       !degraded.last_aim.has_target, "GSI 故障不得关闭队伍约束或继承有效目标");
        }
        runtime.stop();
    }
    if (occupied_gsi != INVALID_SOCKET) closesocket(occupied_gsi);

    // 无效目录走真实 RecoilStore；同一 Runtime 重启后不得留下半启动压枪调度。
    std::filesystem::create_directories(diagnostic_root);
    const auto invalid_profile_directory = diagnostic_root / "profiles-file";
    { std::ofstream output(invalid_profile_directory); output << "not a directory"; }
    AppConfig recoil_failure_config = config;
    recoil_failure_config.mouse.backend = MouseBackend::KMBOX_NET;
    recoil_failure_config.mouse.kmbox_ip = "127.0.0.1";
    recoil_failure_config.mouse.kmbox_port = 13384;
    recoil_failure_config.mouse.kmbox_uuid = "00000000";
    recoil_failure_config.gsi.enabled = true;
    recoil_failure_config.source_context.enabled = true;
    recoil_failure_config.recoil.enabled = true;
    recoil_failure_config.recoil.sensitivity = 1.0;
    recoil_failure_config.recoil.profile_directory = invalid_profile_directory.string();
    auto no_output_mouse = std::make_shared<NoOutputMouse>();
    const bool recoil_degraded_started = runtime.start(recoil_failure_config, no_output_mouse);
    expect(recoil_degraded_started, "弹道目录损坏不得阻断 Runtime 采集检测");
    if (recoil_degraded_started) {
        expect(wait_until([&] { return runtime.snapshot().processed_frames >= 3; }, 10s),
               "弹道目录读取失败后真实 CPU Detector 继续处理 UDP 帧");
        const auto degraded = runtime.snapshot();
        expect(degraded.last_error.empty() && !degraded.recoil_telemetry_available &&
                   degraded.recoil_profile_status.find("弹道目录读取失败") != std::string::npos,
               "弹道失败状态明确保留且不启动压枪调度");
        expect(no_output_mouse->moves.load() == 0, "故障降级回归不得向假设备输出");
    }
    runtime.stop();
    expect(runtime.snapshot().recoil_profile_status.find("弹道目录读取失败") != std::string::npos,
           "停止不得把弹道加载错误覆盖成无匹配曲线");
    std::filesystem::remove(invalid_profile_directory);
    std::filesystem::remove(diagnostic_root);

    const bool recovered_started = runtime.start(config);
    expect(recovered_started, "禁用桥接后必须能够重新启动 Runtime");
    if (recovered_started) {
        expect(wait_until([&] {
            return runtime.snapshot().processed_frames >= 3;
        }, 10s), "禁用桥接重启后采集检测必须正常处理帧");
        const RuntimeSnapshot recovered = runtime.snapshot();
        expect(recovered.source_context_error.empty() && recovered.gsi_error.empty() &&
                   recovered.recoil_profile_status.empty() &&
                   recovered.last_error.empty(),
               "新的启动周期必须清除旧桥接、GSI 和弹道告警");
    }
    runtime.stop();

    sender_running.store(false, std::memory_order_release);
    if (sender.joinable()) sender.join();
    expect(!sender_failed.load(std::memory_order_acquire),
           "UDP JPEG 连续发送线程不得失败");
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "Runtime 模型热重载测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Runtime 模型热重载测试全部通过。\n";
    return 0;
}
