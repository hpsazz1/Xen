#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
// Windows 的 ERROR 宏不能污染 LogLevel::ERROR。
#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "log/log.h"
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> stopped{false};
BOOL WINAPI on_console(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        stopped.store(true);
        return TRUE;
    }
    return FALSE;
}
std::int64_t ns(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
struct Options {
    std::filesystem::path output;
    int seconds = 10;
    int fps = 120;
    int x = 0, y = 300, width = 650, height = 310;
    bool dry_run = false;
};
int integer(const std::wstring& value) {
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("参数必须为非负整数");
    const auto number = std::stoll(value);
    if (number > std::numeric_limits<int>::max()) throw std::runtime_error("整数超出范围");
    return static_cast<int>(number);
}
Options parse(int argc, wchar_t** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::wstring key = argv[i];
        if (key == L"--dry-run") { result.dry_run = true; continue; }
        if (i + 1 >= argc) throw std::runtime_error("参数缺少值");
        const std::wstring value = argv[++i];
        if (key == L"--output") result.output = value;
        else if (key == L"--seconds") result.seconds = integer(value);
        else if (key == L"--fps") result.fps = integer(value);
        else if (key == L"--roi-x") result.x = integer(value);
        else if (key == L"--roi-y") result.y = integer(value);
        else if (key == L"--roi-width") result.width = integer(value);
        else if (key == L"--roi-height") result.height = integer(value);
        else throw std::runtime_error("未知参数");
    }
    if (result.output.empty() || result.seconds < 1 || result.seconds > 30 ||
        result.fps < 1 || result.fps > 240 || result.width < 1 || result.height < 1 ||
        result.width > 8192 || result.height > 8192 || result.x > 32768 || result.y > 32768)
        throw std::runtime_error("参数范围无效；需要新输出目录、seconds 1..30、fps 1..240、合法 ROI");
    result.output = std::filesystem::absolute(result.output);
    if (std::filesystem::exists(result.output)) throw std::runtime_error("输出目录已存在，拒绝覆盖");
    return result;
}
struct Row {
    std::int64_t started = 0, returned = 0, write_finished = 0;
    CaptureStatus status = CaptureStatus::FAILURE;
    FrameTiming timing;
    int width = 0, height = 0;
    double roi_x = 0, roi_y = 0;
    std::string file, error;
};
std::string quote(const std::string& value) {
    std::string escaped = "\"";
    for (char ch : value) { if (ch == '"') escaped += '"'; escaped += ch; }
    return escaped + '"';
}
void save_rows(const std::filesystem::path& output, const std::vector<Row>& rows) {
    std::ofstream csv(output / "frames.csv");
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv << "attempt,grab_started_ns,grab_returned_ns,write_finished_ns,status,png,sequence,"
           "captured_at_ns,capture_ms,width,height,roi_x,roi_y,source_dropped_frames,"
           "duplication_recoveries,transport_dropped_frames,transport_invalid_packets,"
           "source_received_frames,source_sequence,source_sequence_valid,source_fps,"
           "source_timecode,source_timecode_valid,source_timestamp,source_timestamp_valid,"
           "source_time_basis,source_clock_status,source_time_timing_valid,source_time_at_ns,error\n";
    csv << std::setprecision(17);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const auto& t = r.timing;
        csv << i << ',' << r.started << ',' << r.returned << ',' << r.write_finished << ','
            << CaptureStatusName(r.status) << ',' << quote(r.file) << ',';
        if (r.status == CaptureStatus::FRAME) {
            csv << t.sequence << ',' << ns(t.captured_at) << ',' << t.capture_ms << ','
                << r.width << ',' << r.height << ',' << r.roi_x << ',' << r.roi_y << ','
                << t.source_dropped_frames << ',' << t.duplication_recoveries << ','
                << t.transport_dropped_frames << ',' << t.transport_invalid_packets << ','
                << t.source_received_frames << ',' << t.source_sequence << ',' << t.source_sequence_valid << ','
                << t.source_fps << ',' << t.source_timecode << ',' << t.source_timecode_valid << ','
                << t.source_timestamp << ',' << t.source_timestamp_valid << ','
                << SourceTimeBasisName(t.source_time_basis) << ',' << SourceClockStatusName(t.source_clock_status)
                << ',' << t.source_time_timing_valid << ',' << ns(t.source_time_at) << ',';
        } else {
            // 非 FRAME 没有有效帧元数据；空列不能被解释成零耗时或静止帧。
            for (int column = 0; column < 23; ++column) csv << ',';
        }
        csv << quote(r.error) << '\n';
    }
}
int capture(const Options& o) {
    if (!std::filesystem::create_directories(o.output)) throw std::runtime_error("新输出目录创建失败");
    LogConfig logging;
    logging.enable_file = false;
    logging.enable_ringbuf = false;
    Log::init(logging);
    struct Shutdown { ~Shutdown() { Log::shutdown(); } } shutdown;
    CaptureConfig config;
    config.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.center_roi = false;
    config.roi_x = o.x; config.roi_y = o.y;
    config.roi_width = o.width; config.roi_height = o.height;
    config.acquire_timeout_ms = 8;
    auto source = create_capture(config);
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(o.seconds * o.fps + 1));
    std::string failure;
    std::size_t saved = 0, no_frame = 0;
    const auto began = Clock::now();
    auto finished = began;
    try {
        if (!source || !source->open()) throw std::runtime_error(source ? source->last_error() : "Capture 创建失败");
        const auto deadline = Clock::now() + std::chrono::seconds(o.seconds);
        const auto period = std::chrono::nanoseconds(1000000000LL / o.fps);
        auto next = Clock::now();
        SetConsoleCtrlHandler(on_console, TRUE);
        while (!stopped.load() && Clock::now() < deadline &&
               rows.size() < static_cast<std::size_t>(o.seconds * o.fps + 1)) {
            std::this_thread::sleep_until(next);
            if (Clock::now() >= deadline) break;
            CapturedFrame frame;
            Row row;
            row.started = ns(Clock::now());
            row.status = source->grab(frame);
            row.returned = ns(Clock::now());
            if (row.status == CaptureStatus::FRAME) {
                row.timing = frame.timing;
                row.width = frame.width; row.height = frame.height;
                row.roi_x = frame.roi_x; row.roi_y = frame.roi_y;
                try {
                    if (frame.bgr.empty() || frame.bgr.cols != o.width || frame.bgr.rows != o.height ||
                        frame.roi_x != o.x || frame.roi_y != o.y)
                        throw std::runtime_error("捕获 ROI 与指定几何不符");
                    // 同步编码/写盘扰动保留在时间线上，不补齐或复制帧伪造目标帧率。
                    row.file = "frame_" + std::to_string(rows.size()) + ".png";
                    if (!cv::imwrite((o.output / row.file).string(), frame.bgr,
                                     {cv::IMWRITE_PNG_COMPRESSION, 1}))
                        throw std::runtime_error("PNG 写入失败");
                    ++saved;
                } catch (const std::exception& error) { row.error = error.what(); failure = row.error; }
            } else if (row.status == CaptureStatus::NO_FRAME) {
                ++no_frame;
                row.error = "本次等待没有新帧";
            } else { row.error = source->last_error(); failure = "Capture 非正常状态：" + row.error; }
            row.write_finished = ns(Clock::now());
            rows.push_back(std::move(row));
            if (!failure.empty()) break;
            next += period;
            if (next < Clock::now()) next = Clock::now();
        }
    } catch (const std::exception& error) { failure = error.what(); }
    finished = Clock::now();
    if (source) source->close();
    SetConsoleCtrlHandler(on_console, FALSE);
    if (failure.empty() && saved == 0) failure = "有界采样结束，未保存任何有效帧";
    save_rows(o.output, rows);
    const double elapsed = std::chrono::duration<double>(finished - began).count();
    std::ofstream summary(o.output / "summary.txt");
    summary.exceptions(std::ios::failbit | std::ios::badbit);
    summary << "status=" << (!failure.empty() ? "FAILED" : stopped.load() ? "INTERRUPTED" : "COMPLETED")
            << "\nphysical_output_capability=false\nbackend=DESKTOP_DUPLICATION\n"
            << "requested_fps=" << o.fps << "\nrequested_seconds=" << o.seconds
            << "\nattempts=" << rows.size() << "\nsaved_frames=" << saved
            << "\nno_frame_attempts=" << no_frame << "\nelapsed_including_open_seconds=" << elapsed
            << "\nobserved_saved_fps_including_open=" << (elapsed > 0 ? saved / elapsed : 0)
            << "\ninterrupted=" << stopped.load() << "\nfailure=" << failure
            << "\n说明=时间戳为本机 steady 域；captured_at 沿用 Capture 语义，非曝光承诺。\n";
    std::cout << "采样结束：保存 " << saved << " 帧；空帧等待 " << no_frame << " 次。\n";
    return failure.empty() && saved > 0 && !stopped.load() ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::cout << "只读 DXGI ROI 采样：--output <新目录> [--seconds 1..30] [--fps 1..240] "
                     "[--roi-x 0 --roi-y 300 --roi-width 650 --roi-height 310] [--dry-run]\n";
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        if (options.dry_run) { std::cout << "参数有效；未打开 Capture，未创建目录。\n"; return 0; }
        return capture(options);
    } catch (const std::exception& error) { std::cerr << "失败：" << error.what() << '\n'; return 1; }
}
