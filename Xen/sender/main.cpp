#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "crash/crash.h"
#include "log/log.h"
#include "sender/sender.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct SenderOptions {
    std::string destination_url;
    int adapter_index = 0;
    int output_index = 0;
    int roi_width = 320;
    int roi_height = 320;
    int roi_x = 0;
    int roi_y = 0;
    int jpeg_quality = 85;
    int max_datagram_bytes = 1400;
    std::uint32_t fps = 240;
    std::uint64_t max_frames = 0;
    bool explicit_roi = false;
    bool show_help = false;
};

std::atomic<bool> stop_requested{false};

BOOL WINAPI console_control_handler(DWORD control_type) noexcept {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_SHUTDOWN_EVENT) {
        stop_requested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    try {
        if (input.empty()) {
            output.clear();
            return true;
        }
        if (input.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max())) {
            return false;
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0) return false;
        output.resize(static_cast<std::size_t>(required));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                   static_cast<int>(input.size()), output.data(), required,
                   nullptr, nullptr) == required;
    } catch (...) {
        return false;
    }
}

template<typename Integer>
bool parse_integer(std::wstring_view input, Integer& value) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8) || utf8.empty()) return false;
    Integer parsed{};
    const auto [end, error] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), parsed);
    if (error != std::errc{} || end != utf8.data() + utf8.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_options(int argc, wchar_t* argv[], SenderOptions& options,
                   std::string& error) noexcept {
    try {
        bool roi_x_set = false;
        bool roi_y_set = false;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") {
                options.show_help = true;
                continue;
            }
            if (index + 1 >= argc) {
                error = "参数缺少值";
                return false;
            }
            const std::wstring_view value(argv[++index]);
            if (argument == L"--destination") {
                if (!wide_to_utf8(value, options.destination_url)) {
                    error = "目的地址不是合法 UTF-16";
                    return false;
                }
            } else if (argument == L"--adapter") {
                if (!parse_integer(value, options.adapter_index)) {
                    error = "--adapter 必须是整数";
                    return false;
                }
            } else if (argument == L"--output") {
                if (!parse_integer(value, options.output_index)) {
                    error = "--output 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-width") {
                if (!parse_integer(value, options.roi_width)) {
                    error = "--roi-width 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-height") {
                if (!parse_integer(value, options.roi_height)) {
                    error = "--roi-height 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-x") {
                if (!parse_integer(value, options.roi_x)) {
                    error = "--roi-x 必须是整数";
                    return false;
                }
                roi_x_set = true;
            } else if (argument == L"--roi-y") {
                if (!parse_integer(value, options.roi_y)) {
                    error = "--roi-y 必须是整数";
                    return false;
                }
                roi_y_set = true;
            } else if (argument == L"--jpeg-quality") {
                if (!parse_integer(value, options.jpeg_quality)) {
                    error = "--jpeg-quality 必须是整数";
                    return false;
                }
            } else if (argument == L"--fps") {
                if (!parse_integer(value, options.fps)) {
                    error = "--fps 必须是正整数";
                    return false;
                }
            } else if (argument == L"--datagram-bytes") {
                if (!parse_integer(value, options.max_datagram_bytes)) {
                    error = "--datagram-bytes 必须是整数";
                    return false;
                }
            } else if (argument == L"--max-frames") {
                if (!parse_integer(value, options.max_frames)) {
                    error = "--max-frames 必须是非负整数";
                    return false;
                }
            } else {
                std::string unknown;
                wide_to_utf8(argument, unknown);
                error = "未知参数: " + unknown;
                return false;
            }
        }

        if (options.show_help) return true;
        if (options.destination_url.empty()) {
            error = "必须提供 --destination udp://辅机IPv4:端口";
            return false;
        }
        if (roi_x_set != roi_y_set) {
            error = "显式 ROI 必须同时提供 --roi-x 和 --roi-y";
            return false;
        }
        options.explicit_roi = roi_x_set;
        if (options.adapter_index < 0 || options.output_index < 0 ||
            options.roi_width <= 0 || options.roi_height <= 0 ||
            options.jpeg_quality < 1 || options.jpeg_quality > 100 ||
            options.fps == 0 || options.fps > 1'000'000U ||
            options.max_datagram_bytes <= 124 ||
            options.max_datagram_bytes > 65507 ||
            (options.explicit_roi &&
             (options.roi_x < 0 || options.roi_y < 0))) {
            error = "命令行参数超出允许范围";
            return false;
        }
        return true;
    } catch (...) {
        error = "解析命令行时发生未知异常";
        return false;
    }
}

void print_help() {
    std::cout
        << "XenSender - Desktop Duplication 到 XUDP JPEG 发送端\n\n"
        << "用法:\n"
        << "  XenSender.exe --destination udp://辅机IPv4:5000 [选项]\n\n"
        << "选项:\n"
        << "  --adapter N          DXGI 适配器索引，默认 0\n"
        << "  --output N           DXGI 输出索引，默认 0\n"
        << "  --roi-width N        主机采集 ROI 宽度，默认 320\n"
        << "  --roi-height N       主机采集 ROI 高度，默认 320\n"
        << "  --roi-x N --roi-y N  显式主机 ROI；省略时使用主机中心\n"
        << "  --jpeg-quality N     JPEG 质量 1..100，默认 85\n"
        << "  --fps N              发送上限及协议声明 FPS，默认 240\n"
        << "  --datagram-bytes N   XUDP 数据报上限，默认 1400\n"
        << "  --max-frames N       成功发送 N 帧后退出，0 表示持续运行\n"
        << "  --help                显示帮助\n";
}

void log_stats(const XudpSenderStats& stats) noexcept {
    LOG_INFO(
        "sender",
        "发送统计: stream={}, frame={}, success={}, failed={}, "
        "datagrams={}, jpeg={}B, wire={}B, largest={}B, "
        "last_ms={{encode:{:.3f}, packetize:{:.3f}, send:{:.3f}, total:{:.3f}}}",
        stats.stream_id, stats.last_frame_id, stats.frames_sent,
        stats.frames_failed, stats.datagrams_sent, stats.jpeg_bytes_sent,
        stats.wire_bytes_sent, stats.largest_datagram_bytes,
        stats.last_encode_ms, stats.last_packetize_ms, stats.last_send_ms,
        stats.last_total_ms);
}

int run_sender(const SenderOptions& options) noexcept {
    CaptureConfig capture_config;
    capture_config.backend = CaptureBackend::DESKTOP_DUPLICATION;
    capture_config.adapter_index = options.adapter_index;
    capture_config.output_index = options.output_index;
    capture_config.roi_width = options.roi_width;
    capture_config.roi_height = options.roi_height;
    capture_config.center_roi = !options.explicit_roi;
    capture_config.roi_x = options.roi_x;
    capture_config.roi_y = options.roi_y;
    capture_config.acquire_timeout_ms = 4;

    auto capture = create_capture(capture_config);
    if (!capture || !capture->open()) {
        LOG_ERROR(
            "sender", "Desktop Duplication 打开失败: {}",
            capture ? capture->last_error() : "Capture 对象创建失败");
        return 2;
    }

    XudpSenderConfig sender_config;
    sender_config.destination_url = options.destination_url;
    sender_config.jpeg_quality = options.jpeg_quality;
    sender_config.max_datagram_bytes = options.max_datagram_bytes;
    sender_config.frame_rate_n = options.fps;
    sender_config.frame_rate_d = 1;
    XudpSender sender;
    if (!sender.open(sender_config)) {
        LOG_ERROR("sender", "XUDP Sender 打开失败: {}", sender.last_error());
        capture->close();
        return 3;
    }

    const auto frame_interval = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / options.fps));
    auto next_send_at = std::chrono::steady_clock::time_point::min();
    auto next_stats_at = std::chrono::steady_clock::now() +
                         std::chrono::seconds(5);
    auto next_warning_at = std::chrono::steady_clock::time_point::min();
    bool geometry_logged = false;
    int exit_code = 0;
    CapturedFrame frame;

    while (!stop_requested.load(std::memory_order_acquire)) {
        const CaptureStatus capture_status = capture->grab(frame);
        if (capture_status == CaptureStatus::NO_FRAME) continue;
        if (capture_status != CaptureStatus::FRAME) {
            LOG_ERROR(
                "sender", "Desktop Duplication 取帧失败: status={}, error={}",
                CaptureStatusName(capture_status), capture->last_error());
            exit_code = 4;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        // 超过目标采样率时直接跳过较早的新帧，不通过 sleep 持有并发送旧画面。
        if (now < next_send_at) continue;
        if (!geometry_logged) {
            LOG_INFO(
                "sender",
                "主机几何: source={}x{}, roi=({:.0f},{:.0f},{}x{}), "
                "encoded={}x{}, scale=({:.3f},{:.3f})",
                frame.source_width, frame.source_height, frame.roi_x,
                frame.roi_y, frame.bgr.cols, frame.bgr.rows, frame.bgr.cols,
                frame.bgr.rows, frame.source_pixels_per_pixel_x,
                frame.source_pixels_per_pixel_y);
            geometry_logged = true;
        }

        if (!sender.send_frame(frame)) {
            const auto failed_at = std::chrono::steady_clock::now();
            if (failed_at >= next_warning_at) {
                LOG_WARN("sender", "XUDP 帧发送失败: {}", sender.last_error());
                next_warning_at = failed_at + std::chrono::seconds(5);
            }
            if (sender.status() != XudpSenderStatus::READY) {
                exit_code = 5;
                break;
            }
        } else {
            next_send_at = now + frame_interval;
            if (options.max_frames != 0 &&
                sender.stats().frames_sent >= options.max_frames) {
                break;
            }
        }

        if (now >= next_stats_at) {
            log_stats(sender.stats());
            next_stats_at = now + std::chrono::seconds(5);
        }
    }

    log_stats(sender.stats());
    sender.close();
    capture->close();
    return exit_code;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SenderOptions options;
    std::string parse_error;
    if (!parse_options(argc, argv, options, parse_error)) {
        std::cerr << "参数错误: " << parse_error << "\n\n";
        print_help();
        return 1;
    }
    if (options.show_help) {
        print_help();
        return 0;
    }

    LogConfig log_config;
    Log::init(log_config);
    Log::register_module("sender", LogLevel::INFO);
    CrashHandler crash_handler;
    if (!crash_handler.install(log_config.log_dir)) {
        LOG_ERROR("sender", "崩溃诊断安装失败");
    }
    if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
        LOG_ERROR("sender", "安装 Ctrl+C 处理器失败");
        crash_handler.uninstall();
        Log::shutdown();
        return 1;
    }

    const int exit_code = run_sender(options);
    SetConsoleCtrlHandler(console_control_handler, FALSE);
    crash_handler.uninstall();
    Log::shutdown();
    return exit_code;
}
