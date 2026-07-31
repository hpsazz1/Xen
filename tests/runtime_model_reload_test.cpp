#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include "log/log.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
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

    Runtime runtime;
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
        std::vector<double> successful_total_ms;
        for (const auto& sample : profile_samples) {
            if (sample.detection_status == DetectionStatus::SUCCESS &&
                sample.aim_status == AimStatus::SUCCESS) {
                successful_total_ms.push_back(sample.profile.total_ms);
            }
        }
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

    runtime.stop();
    const RuntimeSnapshot stopped = runtime.snapshot();
    expect(stopped.state == RuntimeState::STOPPED &&
               stopped.detector_reload_state == DetectorReloadState::IDLE,
           "stop() 必须回收重载线程并恢复 IDLE");
    expect(!runtime.reload_detector(config.detector),
           "Runtime 停止后必须拒绝 Detector 重载");

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
