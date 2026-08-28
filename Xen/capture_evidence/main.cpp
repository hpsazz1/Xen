#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "capture/capture.h"
#include "capture_evidence/capture_evidence.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct Options {
    std::filesystem::path binding_path;
    std::filesystem::path output_directory;
    std::string ndi_source_name;
    std::string clock_sync_url;
    NetworkFrameLayout frame_layout =
        NetworkFrameLayout::CENTER_CROP_1_TO_1;
    std::uint64_t frames = 480;
    std::uint64_t max_seconds = 15;
    int source_width = 2560;
    int source_height = 1440;
    int roi_width = 320;
    int roi_height = 320;
    int roi_x = 0;
    int roi_y = 0;
    int discovery_timeout_ms = 10000;
    int receive_timeout_ms = 50;
    int disconnect_timeout_ms = 2000;
    int clock_sync_interval_ms = 250;
    int clock_sync_timeout_ms = 200;
    int clock_mapping_max_age_ms = 1000;
    bool roi_x_set = false;
    bool roi_y_set = false;
    bool require_frame_metadata = false;
    bool require_source_timing = false;
    bool show_help = false;
};

std::atomic<bool> g_stop_requested{false};

BOOL WINAPI handle_console_event(DWORD event) noexcept {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
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
bool parse_integer(std::wstring_view input, Integer& output) noexcept {
    std::string utf8;
    if (!wide_to_utf8(input, utf8) || utf8.empty()) return false;
    Integer candidate{};
    const auto [end, result] = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), candidate);
    if (result != std::errc{} || end != utf8.data() + utf8.size()) {
        return false;
    }
    output = candidate;
    return true;
}

void print_usage() {
    std::cout
        << "XenCaptureEvidence 是 output-off NDI 像素证据入口；"
           "physical_output_capability=false。\n"
        << "它不链接 Runtime、Aim、Mouse 或 Keyboard，不能武装或发送输入。\n\n"
        << "用法:\n"
        << "  XenCaptureEvidence --ndi-source <name> --binding <json> "
           "--output <new-dir> [options]\n\n"
        << "选项:\n"
        << "  --frames <1..2400>                 默认 480\n"
        << "  --max-seconds <1..60>              默认 15\n"
        << "  --frame-layout <center_crop_1_to_1|full_frame_1_to_1|full_frame_scaled>\n"
        << "  --source-width <px> --source-height <px>\n"
        << "  --roi-width <px> --roi-height <px>\n"
        << "  --roi-x <px> --roi-y <px>          两项同时提供时关闭中心 ROI\n"
        << "  --discovery-timeout-ms <ms>        默认 10000\n"
        << "  --receive-timeout-ms <ms>          默认 50\n"
        << "  --disconnect-timeout-ms <ms>       默认 2000\n"
        << "  --clock-sync-url <udp://IPv4:port>\n"
        << "  --clock-sync-interval-ms <ms> --clock-sync-timeout-ms <ms>\n"
        << "  --clock-mapping-max-age-ms <ms>\n"
        << "  --require-frame-metadata\n"
        << "  --require-source-timing            仅录制 mapping VALID 帧\n";
}

bool parse_layout(std::wstring_view value,
                  NetworkFrameLayout& output) noexcept {
    if (value == L"center_crop_1_to_1") {
        output = NetworkFrameLayout::CENTER_CROP_1_TO_1;
        return true;
    }
    if (value == L"full_frame_1_to_1") {
        output = NetworkFrameLayout::FULL_FRAME_1_TO_1;
        return true;
    }
    if (value == L"full_frame_scaled") {
        output = NetworkFrameLayout::FULL_FRAME_SCALED;
        return true;
    }
    return false;
}

bool parse_options(int argc, wchar_t* argv[], Options& options,
                   std::string& error) noexcept {
    try {
        auto missing_value = [&](std::wstring_view name) {
            std::string utf8;
            wide_to_utf8(name, utf8);
            error = "参数缺少值：" + utf8;
            return false;
        };
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument = argv[index];
            if (argument == L"--help" || argument == L"-h") {
                options.show_help = true;
                continue;
            }
            if (argument == L"--require-frame-metadata") {
                options.require_frame_metadata = true;
                continue;
            }
            if (argument == L"--require-source-timing") {
                options.require_source_timing = true;
                continue;
            }
            const bool takes_value =
                argument == L"--binding" || argument == L"--output" ||
                argument == L"--ndi-source" ||
                argument == L"--clock-sync-url" ||
                argument == L"--frame-layout" || argument == L"--frames" ||
                argument == L"--max-seconds" ||
                argument == L"--source-width" ||
                argument == L"--source-height" ||
                argument == L"--roi-width" ||
                argument == L"--roi-height" || argument == L"--roi-x" ||
                argument == L"--roi-y" ||
                argument == L"--discovery-timeout-ms" ||
                argument == L"--receive-timeout-ms" ||
                argument == L"--disconnect-timeout-ms" ||
                argument == L"--clock-sync-interval-ms" ||
                argument == L"--clock-sync-timeout-ms" ||
                argument == L"--clock-mapping-max-age-ms";
            if (!takes_value) {
                std::string utf8;
                wide_to_utf8(argument, utf8);
                error = "未知参数：" + utf8;
                return false;
            }
            if (index + 1 >= argc) return missing_value(argument);
            const std::wstring_view value = argv[++index];
            if (argument == L"--binding") {
                options.binding_path = std::filesystem::path(value);
            } else if (argument == L"--output") {
                options.output_directory = std::filesystem::path(value);
            } else if (argument == L"--ndi-source") {
                if (!wide_to_utf8(value, options.ndi_source_name)) {
                    error = "NDI source 名称不是合法 Unicode";
                    return false;
                }
            } else if (argument == L"--clock-sync-url") {
                if (!wide_to_utf8(value, options.clock_sync_url)) {
                    error = "clock sync URL 不是合法 Unicode";
                    return false;
                }
            } else if (argument == L"--frame-layout") {
                if (!parse_layout(value, options.frame_layout)) {
                    error = "frame layout 值无效";
                    return false;
                }
            } else if (argument == L"--frames") {
                if (!parse_integer(value, options.frames)) {
                    error = "frames 必须是整数";
                    return false;
                }
            } else if (argument == L"--max-seconds") {
                if (!parse_integer(value, options.max_seconds)) {
                    error = "max-seconds 必须是整数";
                    return false;
                }
            } else if (argument == L"--source-width") {
                if (!parse_integer(value, options.source_width)) {
                    error = "source-width 必须是整数";
                    return false;
                }
            } else if (argument == L"--source-height") {
                if (!parse_integer(value, options.source_height)) {
                    error = "source-height 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-width") {
                if (!parse_integer(value, options.roi_width)) {
                    error = "roi-width 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-height") {
                if (!parse_integer(value, options.roi_height)) {
                    error = "roi-height 必须是整数";
                    return false;
                }
            } else if (argument == L"--roi-x") {
                if (!parse_integer(value, options.roi_x)) {
                    error = "roi-x 必须是整数";
                    return false;
                }
                options.roi_x_set = true;
            } else if (argument == L"--roi-y") {
                if (!parse_integer(value, options.roi_y)) {
                    error = "roi-y 必须是整数";
                    return false;
                }
                options.roi_y_set = true;
            } else if (argument == L"--discovery-timeout-ms") {
                if (!parse_integer(value, options.discovery_timeout_ms)) {
                    error = "discovery-timeout-ms 必须是整数";
                    return false;
                }
            } else if (argument == L"--receive-timeout-ms") {
                if (!parse_integer(value, options.receive_timeout_ms)) {
                    error = "receive-timeout-ms 必须是整数";
                    return false;
                }
            } else if (argument == L"--disconnect-timeout-ms") {
                if (!parse_integer(value, options.disconnect_timeout_ms)) {
                    error = "disconnect-timeout-ms 必须是整数";
                    return false;
                }
            } else if (argument == L"--clock-sync-interval-ms") {
                if (!parse_integer(value, options.clock_sync_interval_ms)) {
                    error = "clock-sync-interval-ms 必须是整数";
                    return false;
                }
            } else if (argument == L"--clock-sync-timeout-ms") {
                if (!parse_integer(value, options.clock_sync_timeout_ms)) {
                    error = "clock-sync-timeout-ms 必须是整数";
                    return false;
                }
            } else if (argument == L"--clock-mapping-max-age-ms") {
                if (!parse_integer(value, options.clock_mapping_max_age_ms)) {
                    error = "clock-mapping-max-age-ms 必须是整数";
                    return false;
                }
            }
        }
        if (options.show_help) return true;
        if (options.binding_path.empty() || options.output_directory.empty() ||
            options.ndi_source_name.empty()) {
            error = "必须提供 --ndi-source、--binding 和 --output";
            return false;
        }
        if (options.frames == 0 || options.frames > 2400 ||
            options.max_seconds == 0 || options.max_seconds > 60 ||
            options.source_width <= 0 || options.source_height <= 0 ||
            options.roi_width <= 0 || options.roi_height <= 0 ||
            options.discovery_timeout_ms <= 0 ||
            options.receive_timeout_ms <= 0 ||
            options.disconnect_timeout_ms <= 0 ||
            options.clock_sync_interval_ms <= 0 ||
            options.clock_sync_timeout_ms <= 0 ||
            options.clock_mapping_max_age_ms <= 0) {
            error = "帧数、时限、几何或超时参数超出正值边界";
            return false;
        }
        if (options.roi_x_set != options.roi_y_set) {
            error = "--roi-x 与 --roi-y 必须同时提供";
            return false;
        }
        if (options.require_source_timing && options.clock_sync_url.empty()) {
            error = "--require-source-timing 要求显式 --clock-sync-url";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("解析参数异常：") + exception.what();
        return false;
    } catch (...) {
        error = "解析参数发生未知异常";
        return false;
    }
}

CaptureConfig make_capture_config(const Options& options) {
    CaptureConfig config;
    config.backend = CaptureBackend::NDI;
    config.ndi_source_name = options.ndi_source_name;
    config.ndi_discovery_timeout_ms = options.discovery_timeout_ms;
    config.ndi_receive_timeout_ms = options.receive_timeout_ms;
    config.ndi_disconnect_timeout_ms = options.disconnect_timeout_ms;
    config.ndi_clock_sync_url = options.clock_sync_url;
    config.ndi_clock_sync_interval_ms = options.clock_sync_interval_ms;
    config.ndi_clock_sync_timeout_ms = options.clock_sync_timeout_ms;
    config.ndi_clock_mapping_max_age_ms = options.clock_mapping_max_age_ms;
    config.ndi_frame_layout = options.frame_layout;
    config.ndi_source_width = options.source_width;
    config.ndi_source_height = options.source_height;
    config.ndi_require_frame_metadata = options.require_frame_metadata;
    config.roi_width = options.roi_width;
    config.roi_height = options.roi_height;
    config.center_roi = !options.roi_x_set;
    config.roi_x = options.roi_x;
    config.roi_y = options.roi_y;
    return config;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << error << '\n';
        print_usage();
        return 2;
    }
    if (options.show_help) {
        print_usage();
        return 0;
    }

    const CaptureConfig capture_config = make_capture_config(options);
    capture_evidence::CaptureEvidenceConfig evidence_config;
    evidence_config.output_directory = options.output_directory;
    evidence_config.source_binding_path = options.binding_path;
    evidence_config.capture = capture_config;
    evidence_config.require_source_timing = options.require_source_timing;
    evidence_config.requested_frame_count = options.frames;

    capture_evidence::CaptureEvidenceRecorder recorder;
    if (!recorder.start(evidence_config, error)) {
        std::cerr << "证据录制初始化失败：" << error << '\n';
        return 1;
    }
    std::unique_ptr<ICapture> capture = create_capture(capture_config);
    if (!capture) {
        std::cerr << "无法创建 NDI Capture；本入口仍未发布任何最终证据。\n";
        return 1;
    }
    if (!capture->open()) {
        std::cerr << "NDI Capture 打开失败：" << capture->last_error() << '\n';
        capture->close();
        return 1;
    }

    SetConsoleCtrlHandler(handle_console_event, TRUE);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.max_seconds);
    std::uint64_t timing_rejected_frames = 0;
    while (recorder.recorded_frame_count() < options.frames &&
           !g_stop_requested.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        CapturedFrame frame;
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::FRAME) {
            if (options.require_source_timing &&
                (!frame.timing.source_time_timing_valid ||
                 frame.timing.source_clock_status != SourceClockStatus::VALID)) {
                ++timing_rejected_frames;
                continue;
            }
            if (!recorder.record(frame, error)) {
                std::cerr << "记录 NDI 帧失败：" << error << '\n';
                capture->close();
                SetConsoleCtrlHandler(handle_console_event, FALSE);
                return 1;
            }
            continue;
        }
        if (status == CaptureStatus::NO_FRAME ||
            status == CaptureStatus::READY) {
            std::this_thread::yield();
            continue;
        }
        std::cerr << "NDI Capture 失败：status=" << CaptureStatusName(status)
                  << "；error=" << capture->last_error() << '\n';
        capture->close();
        SetConsoleCtrlHandler(handle_console_event, FALSE);
        return 1;
    }
    capture->close();
    SetConsoleCtrlHandler(handle_console_event, FALSE);

    if (g_stop_requested.load(std::memory_order_acquire)) {
        std::cerr << "用户中止 output-off 证据录制；未发布最终目录。\n";
        return 130;
    }
    if (recorder.recorded_frame_count() != options.frames) {
        std::cerr << "证据录制超时：recorded="
                  << recorder.recorded_frame_count()
                  << "；requested=" << options.frames
                  << "；timing_rejected=" << timing_rejected_frames
                  << "。未发布最终目录。\n";
        return 1;
    }
    if (!recorder.finish(error)) {
        std::cerr << "发布证据失败：" << error << '\n';
        return 1;
    }
    std::string output_path;
    if (!wide_to_utf8(
            std::filesystem::absolute(options.output_directory).wstring(),
            output_path)) {
        std::cout << "output-off NDI evidence published; "
                     "path encoding unavailable；frames="
                  << options.frames
                  << "；timing_rejected=" << timing_rejected_frames << '\n';
        return 0;
    }
    std::cout << "output-off NDI 证据已原子发布：" << output_path
              << "；frames=" << options.frames
              << "；timing_rejected=" << timing_rejected_frames << '\n';
    return 0;
}
