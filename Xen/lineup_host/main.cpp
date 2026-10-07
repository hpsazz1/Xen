#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#ifdef ERROR
#undef ERROR
#endif
#include "capture/capture.h"
#include "lineup/host_capture_internal.h"
#include "log/log.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
constexpr int kCaptureHotkey = 1;
std::atomic<DWORD> main_thread_id{0};
struct Options {
    lineup::detail::HostCapturePaths paths;
    bool help = false, dry_run = false;
};
struct ProcessHandle {
    HANDLE value = nullptr;
    ~ProcessHandle() { if (value) CloseHandle(value); }
};
struct Foreground {
    HWND window = nullptr;
    DWORD process_id = 0;
    HMONITOR monitor = nullptr;
    RECT bounds{};
};
void require(bool condition, const std::string &reason) {
    if (!condition) throw std::runtime_error(reason);
}
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::string utc_now() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}
std::string capture_id() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const auto ticks = (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    static std::atomic<std::uint64_t> sequence{0};
    return "capture-" + std::to_string(ticks) + "-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(++sequence);
}
Options parse(int argc, wchar_t **argv) {
    Options options;
    std::filesystem::path local_library, inbox, output;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--help") { options.help = true; continue; }
        if (argument == L"--dry-run") { options.dry_run = true; continue; }
        require(argument == L"--local-library" || argument == L"--inbox" || argument == L"--output",
                "未知参数，请使用 --help");
        require(++i < argc, "参数缺少路径");
        require(argv[i][0] != L'\0' && !std::wstring_view(argv[i]).starts_with(L"--"), "参数缺少有效路径");
        auto &path = argument == L"--local-library" ? local_library : argument == L"--inbox" ? inbox : output;
        require(path.empty(), "路径参数不能重复指定");
        path = argv[i];
    }
    if (!options.help) {
        std::string error;
        if (!lineup::detail::resolve_host_capture_paths(local_library, inbox, output, options.paths, error))
            throw std::runtime_error(error);
    }
    return options;
}
Foreground foreground() {
    Foreground result;
    result.window = GetForegroundWindow();
    require(result.window && IsWindowVisible(result.window) && !IsIconic(result.window), "CS2 必须位于前台且未最小化");
    require(GetWindowThreadProcessId(result.window, &result.process_id) && result.process_id,
            "无法确认前台进程");
    ProcessHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.process_id)};
    require(process.value != nullptr, "无法只读查询前台进程");
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    require(QueryFullProcessImageNameW(process.value, 0, image.data(), &length) != 0, "无法取得前台程序名称");
    image.resize(length);
    require(_wcsicmp(std::filesystem::path(image).filename().c_str(), L"cs2.exe") == 0,
            "前台不是 cs2.exe，本次不采集");
    result.monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    require(result.monitor && GetMonitorInfoW(result.monitor, &monitor) && (monitor.dwFlags & MONITORINFOF_PRIMARY),
            "无法确认主显示器");
    RECT client{};
    require(GetClientRect(result.window, &client) != 0, "无法读取 CS2 客户区");
    POINT start{client.left, client.top}, end{client.right, client.bottom};
    require(ClientToScreen(result.window, &start) && ClientToScreen(result.window, &end), "无法转换客户区物理坐标");
    result.bounds = {start.x, start.y, end.x, end.y};
    require(EqualRect(&result.bounds, &monitor.rcMonitor) != 0, "CS2 客户区须完整覆盖主显示器，请核对全屏和分辨率");
    DWORD checked_pid = 0;
    require(GetForegroundWindow() == result.window && GetWindowThreadProcessId(result.window, &checked_pid) &&
            checked_pid == result.process_id, "检查期间前台进程已改变");
    return result;
}
CaptureConfig capture_config(const Foreground &selected) {
    ComPtr<IDXGIFactory1> factory;
    require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "无法枚举 DXGI 显示输出");
    for (UINT adapter_index = 0; adapter_index < 64; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto adapter_result = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_result == DXGI_ERROR_NOT_FOUND) break;
        require(SUCCEEDED(adapter_result), "DXGI 显卡枚举失败");
        for (UINT output_index = 0; output_index < 64; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const auto output_result = adapter->EnumOutputs(output_index, &output);
            if (output_result == DXGI_ERROR_NOT_FOUND) break;
            require(SUCCEEDED(output_result), "DXGI 输出枚举失败");
            DXGI_OUTPUT_DESC description{};
            require(SUCCEEDED(output->GetDesc(&description)), "无法读取 DXGI 输出几何");
            if (description.Monitor != selected.monitor) continue;
            require(description.AttachedToDesktop && EqualRect(&description.DesktopCoordinates, &selected.bounds),
                    "DXGI 主显示器与 CS2 客户区几何不一致");
            CaptureConfig config;
            config.backend = CaptureBackend::DESKTOP_DUPLICATION;
            config.adapter_index = static_cast<int>(adapter_index);
            config.output_index = static_cast<int>(output_index);
            config.center_roi = false;
            config.roi_width = selected.bounds.right - selected.bounds.left;
            config.roi_height = selected.bounds.bottom - selected.bounds.top;
            config.roi_x = config.roi_y = 0;
            config.acquire_timeout_ms = 100;
            return config;
        }
    }
    throw std::runtime_error("找不到主显示器对应的 DXGI 输出");
}
void capture_once(const Options &options, std::stop_token stop) {
    nlohmann::json profile;
    std::string error;
    if (!lineup::read_host_capture_profile(options.paths.profile_file, profile, error))
        throw std::runtime_error("无法读取采集设置：" + error);
    const auto before = foreground();
    const auto config = capture_config(before);
    require(profile.at("source_width") == config.roi_width && profile.at("source_height") == config.roi_height,
            "采集设置的源尺寸与主机实际全屏不符，本次不采集");
    require(!stop.stop_requested(), "采集已取消");
    const auto request_timestamp = utc_now();
    const auto requested = std::chrono::steady_clock::now();
    auto capture = create_capture(config);
    require(capture != nullptr, "无法创建 DXGI 采集");
    if (!capture->open()) throw std::runtime_error("无法打开 DXGI 采集：" + capture->last_error());
    CapturedFrame frame;
    bool acquired = false;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() - requested < std::chrono::seconds(2)) {
        const auto status = capture->grab(frame);
        if (status == CaptureStatus::FRAME) { acquired = true; break; }
        require(status == CaptureStatus::NO_FRAME || status == CaptureStatus::READY,
                "DXGI 未取得有效帧：" + capture->last_error());
    }
    const auto capture_timestamp = utc_now();
    capture->close();
    require(acquired && !stop.stop_requested(), "采集超时或已取消，没有发布截图");
    const auto after = foreground();
    require(after.window == before.window && after.process_id == before.process_id &&
            after.monitor == before.monitor && EqualRect(&after.bounds, &before.bounds), "取帧期间 CS2 前台或显示几何改变");
    require(frame.storage == CapturedFrameStorage::CPU_BGR && frame.bgr.type() == CV_8UC3 &&
            frame.width == config.roi_width && frame.height == config.roi_height &&
            frame.bgr.cols == frame.width && frame.bgr.rows == frame.height &&
            frame.source_width == frame.width && frame.source_height == frame.height &&
            frame.encoded_width == frame.width && frame.encoded_height == frame.height &&
            frame.roi_x == 0 && frame.roi_y == 0 && frame.source_pixels_per_pixel_x == 1 && frame.source_pixels_per_pixel_y == 1 &&
            frame.timing.captured_at >= requested, "DXGI 实际帧几何或读取时间不符合全屏合同");
    lineup::HostCaptureBundle bundle;
    require(lineup::make_host_capture_bundle(frame.bgr, profile, capture_id(), capture_timestamp, bundle, error), error);
    // UTC 记录读回完成时刻，不冒充 DXGI 源呈现时刻或跨机控制时钟。
    bundle.metadata["request_timestamp"] = request_timestamp;
    bundle.metadata["timestamp_basis"] = "desktop_readback_completion_utc";
    bundle.metadata["foreground"] = {{"process_id", before.process_id},
        {"window", reinterpret_cast<std::uintptr_t>(before.window)}, {"executable", "cs2.exe"},
        {"client", {{"left", before.bounds.left}, {"top", before.bounds.top},
                    {"right", before.bounds.right}, {"bottom", before.bounds.bottom}}}};
    std::filesystem::path local, delivered;
    if (!lineup::write_host_capture_bundle(options.paths.captures, bundle, local, error))
        throw std::runtime_error("本机截图保存失败：" + error);
    LOG_INFO("LineupHost", "本机证据已保存：{}", utf8(local));
    if (options.paths.local_only) return;
    require(!stop.stop_requested(), "已保留本机证据；退出请求取消本次传送");
    if (!lineup::write_host_capture_bundle(options.paths.delivery, bundle, delivered, error))
        throw std::runtime_error("本机证据已保留，辅机收件失败：" + error);
    LOG_INFO("LineupHost", "F7 截图已完整发布到辅机收件目录：{}", utf8(delivered));
}
BOOL WINAPI console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT && event != CTRL_SHUTDOWN_EVENT)
        return FALSE;
    const auto thread = main_thread_id.load();
    return thread && PostThreadMessageW(thread, WM_QUIT, 0, 0);
}
struct HotkeyRegistration {
    bool active = false;
    ~HotkeyRegistration() { if (active) UnregisterHotKey(nullptr, kCaptureHotkey); }
};
} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        const auto options = parse(argc, argv);
        if (options.help) {
            std::cout << "XenLineupHost 主机只读 F7 截图工具\n"
                "--local-library <本机采集库> [--dry-run]\n"
                "读取采集库/profile.json，仅保存到采集库/captures，无需辅机、NDI 或 Runtime 在线。\n"
                "兼容共享收件模式（不能与 --local-library 同用）：\n"
                "--inbox <辅机收件目录或 UNC> --output <本机证据目录> [--dry-run]\n"
                "读取 inbox/profile.json，保存本机证据后传送到 inbox/captures。\n"
                "只注册 F7，不监听 F8/F9，不连接鼠标设备。\n"
                "CS2 须前台全屏覆盖主显示器；Ctrl+C 退出。--dry-run/--help 不采集、不注册热键。\n";
            return 0;
        }
        if (options.dry_run) {
            nlohmann::json profile; std::string error;
            require(lineup::read_host_capture_profile(options.paths.profile_file, profile, error), error);
            nlohmann::json result{{"mode", "DRY_RUN"}, {"profile_valid", true},
                {"capture_started", false}, {"hotkeys_registered", false},
                {"storage_mode", options.paths.local_only ? "local_library" : "shared_inbox"},
                {"profile", utf8(options.paths.profile_file)}, {"output", utf8(options.paths.captures)}};
            if (options.paths.local_only) result["local_library"] = utf8(options.paths.profile_file.parent_path());
            else result["inbox"] = utf8(options.paths.delivery.parent_path());
            std::cout << result.dump() << '\n';
            return 0;
        }
        LogConfig logging; logging.enable_file = false; logging.global_level = LogLevel::INFO;
        Log::init(logging); Log::register_module("LineupHost");
        require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
                AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2),
                "无法启用物理像素 DPI 感知，本次不注册采集热键");
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        main_thread_id.store(GetCurrentThreadId());
        require(SetConsoleCtrlHandler(console_control, TRUE) != 0, "无法注册退出处理");
        HotkeyRegistration hotkey;
        hotkey.active = RegisterHotKey(nullptr, kCaptureHotkey, MOD_NOREPEAT, VK_F7) != 0;
        require(hotkey.active, "F7 热键注册失败，可能已被其他程序占用；错误码 " + std::to_string(GetLastError()));
        if (options.paths.local_only)
            LOG_INFO("LineupHost", "已注册 F7；使用本机采集设置，仅保存本地截图。回 CS2 前台按一次 F7，Ctrl+C 退出。");
        else
            LOG_INFO("LineupHost", "已注册 F7；先在辅机网页保存采集设置，再回 CS2 前台按一次 F7。Ctrl+C 退出。");
        std::atomic<bool> busy{false};
        std::atomic<DWORD> accept_after{GetTickCount()};
        std::jthread worker;
        int result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (message.message != WM_HOTKEY || message.wParam != kCaptureHotkey) continue;
            if (busy.load() || static_cast<LONG>(message.time - accept_after.load()) <= 0) {
                LOG_WARN("LineupHost", "忙时或过期的 F7 已丢弃，没有排队补采。");
                continue;
            }
            if (worker.joinable()) worker.join();
            busy.store(true);
            worker = std::jthread([&](std::stop_token stop) {
                try { capture_once(options, stop); }
                catch (const std::exception &e) { LOG_ERROR("LineupHost", "F7 未完成：{}", e.what()); }
                catch (...) { LOG_ERROR("LineupHost", "F7 采集发生未知错误"); }
                accept_after.store(GetTickCount());
                busy.store(false);
            });
        }
        UnregisterHotKey(nullptr, kCaptureHotkey); hotkey.active = false;
        worker.request_stop();
        if (worker.joinable()) worker.join();
        SetConsoleCtrlHandler(console_control, FALSE); main_thread_id.store(0);
        require(result >= 0, "读取热键消息失败");
        Log::shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "主机采集工具失败：" << e.what() << '\n';
        SetConsoleCtrlHandler(console_control, FALSE); main_thread_id.store(0);
        Log::shutdown();
        return 1;
    }
}
