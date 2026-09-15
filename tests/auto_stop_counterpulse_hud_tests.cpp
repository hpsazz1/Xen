#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "auto_stop_probe/counterpulse_hud.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace auto_stop_probe_detail;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
Json command(const char* kind, int value, int shot) {
    const auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return {{"kind", kind}, {"value", value}, {"shot_index", shot},
        {"disposition", static_cast<int>(KeyboardDisposition::ACKNOWLEDGED)},
        {"planned_ns", time}, {"submit_ns", time}, {"ack_received_ns", time},
        {"backend_completed_ns", time}, {"returned_ns", time}};
}
void wait_state(const CounterpulseHud& hud, const char* expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        const auto status = hud.status();
        require(status["state"] != "FAILED", "HUD初始化或模型线程失败，不能当作无窗口通过");
        if (status["state"] == expected && status["queued_commands"] == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("HUD未在1秒内进入预期可见状态");
}
void wait_snapshot(const CounterpulseHud& hud, unsigned count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        require(hud.status()["state"] != "FAILED", "外部HUD快照不能使UI线程失败");
        if (hud.status()["snapshots_displayed"].get<unsigned>() >= count) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("外部HUD快照未在1秒内显示");
}
Json external_snapshot() {
    Json output{{"source", "KMBOX_MONITOR"}, {"recording", true},
        {"recording_id", "manual-input-20260915-014523-a1b2c3d4"}, {"timings", Json::array()},
        {"shots", Json::array()}, {"current_model", {{"valid", true}, {"speed_ratio", 0.5},
            {"estimated_speed", 0.17}, {"classification", "WITHIN_MODEL_THRESHOLD"}}}};
    for (int i = 1; i <= 40; ++i) {
        const auto ratio = (i % 7) * 0.3;
        output["timings"].push_back({{"ordinal", i}, {"delta_ms", (i % 9 - 4) * 10.0}, {"grade", "EXCELLENT"}});
        output["shots"].push_back({{"ordinal", i}, {"down_model", output["current_model"]},
            {"samples", Json::array({Json{{"kind", "FIRST_MODEL_SAMPLE"}, {"time_ns", 1},
                {"valid", true}, {"speed_ratio", ratio}, {"classification", ratio <= 1 ? "WITHIN_MODEL_THRESHOLD" : "MICRO"}}})}});
    }
    output["shots"].back()["samples"][0]["time_ns"] = INT64_MAX / 2;
    return output;
}
void snapshot(HWND window, const std::filesystem::path& path) {
    RECT bounds{}; require(GetClientRect(window, &bounds), "HUD窗口尺寸不可读");
    const auto dc = GetDC(window); require(dc != nullptr, "HUD窗口DC不可用");
    const auto memory = CreateCompatibleDC(dc);
    const auto bitmap = CreateCompatibleBitmap(dc, bounds.right, bounds.bottom);
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(window, dc); throw std::runtime_error("HUD截图资源不可用");
    }
    const auto previous = SelectObject(memory, bitmap);
    const bool painted = PrintWindow(window, memory, PW_CLIENTONLY) != FALSE;
    SelectObject(memory, previous);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.right; info.bmiHeader.biHeight = -bounds.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(bounds.right) * bounds.bottom * 4);
    const bool read = painted && GetDIBits(dc, bitmap, 0, bounds.bottom, pixels.data(), &info, DIB_RGB_COLORS) != 0;
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
    require(read, "HUD窗口截图失败");
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream file(path, std::ios::binary); file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}
}
int main(int argc, char** argv) {
    try {
        std::filesystem::path snapshot_path;
        if (argc == 3 && std::string(argv[1]) == "--snapshot") snapshot_path = argv[2];
        else require(argc == 1, "仅支持可选--snapshot BMP参数");
        DWORD session = 0;
        require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0,
            "真实HUD测试要求交互式Windows会话，session0不能跳过或判通过");
        require(FindWindowW(L"XenCounterpulseReadOnlyHud", nullptr) == nullptr,
            "存在其他HUD窗口，无法独立验证本次窗口");
        const auto foreground = GetForegroundWindow();
        require(foreground != nullptr, "没有可核对的前台窗口，不能证明HUD未夺焦点");
        HWND window = nullptr;
        {
            CounterpulseHud hud(SamplingSettings{});
            wait_state(hud, "VISIBLE");
            window = FindWindowW(L"XenCounterpulseReadOnlyHud", nullptr);
            require(window && IsWindowVisible(window), "状态VISIBLE必须对应真实可见窗口");
            const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            require((style & WS_EX_NOACTIVATE) && (style & WS_EX_TOPMOST), "HUD须非激活且置顶");
            require(GetForegroundWindow() == foreground, "HUD创建不得改变原前台窗口");
            hud.observe(command("left_button", 0, 0));
            hud.observe(command("wasd", 0, 0));
            hud.observe(command("wasd", 2, 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            hud.observe(command("wasd", 0, 1));
            hud.observe(command("left_button", 1, 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            hud.observe(command("left_button", 0, 1));
            hud.finish(Json{{"success", true}});
            wait_state(hud, "FINISHED_VISIBLE");
            const auto shared_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            std::shared_ptr<const Json> shared_analysis;
            do {
                shared_analysis = hud.latest_analysis();
                if (shared_analysis && shared_analysis->value("shot_count",0) == 1 &&
                    shared_analysis->at("shots")[0].value("complete_hold",false)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < shared_deadline);
            require(shared_analysis && shared_analysis->at("shots").size() == 1 &&
                shared_analysis->at("shots")[0]["down_ack_to_up_submit_ms"].is_number(),
                "页面共享HUD现有分析快照，结束按住与提交时钟不能缺失");
            const auto status = hud.status();
            require(status["success"] && status["dropped_commands"] == 0,
                "完整合成回执应正常排空，HUD不得丢命令");
            require(status["physical_validation_passed"] == false && status["actual_game_speed"].is_null(),
                "窗口验证不能转成真实输入或游戏速度验收");
            require(GetForegroundWindow() == foreground, "HUD刷新与结束不得改变原前台窗口");
            if (!snapshot_path.empty()) snapshot(window, snapshot_path);
        }
        require(!IsWindow(window), "HUD析构必须关闭窗口和UI线程");
        {
            CounterpulseHud hud(SamplingSettings{}, true);
            wait_state(hud, "VISIBLE");
            window = FindWindowW(L"XenCounterpulseReadOnlyHud", nullptr);
            RECT client{}; require(GetClientRect(window, &client) && client.right == 1100 && client.bottom == 430,
                "双面板HUD客户区须容纳完整统计和默认/当前参数");
            require((GetWindowLongPtrW(window, GWL_STYLE) & WS_SYSMENU) != 0 &&
                (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0,
                "常驻HUD必须保留可点击关闭按钮，不能由透传样式吞掉操作");
            auto data = external_snapshot(); hud.publish(data); wait_snapshot(hud, 1);
            require(hud.status()["valid_first_plot_count"] == 32,
                "外部已到期快照的源时钟不能与本机uptime比较后丢掉最后样本");
            std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
            require(title.find(L"manual-input-20260915-014523-a1b2c3d4") == 0,
                "HUD标题必须保留完整录制ID供反馈定位，不能截短或只显示日期");
            require(hud.status()["source"] == "KMBOX_MONITOR" && hud.status()["success"],
                "外部图表必须声明monitor来源，不冒充命令ACK");
            require(GetForegroundWindow() == foreground, "外部快照显示不得夺取前台");
            const auto button = GetDlgItem(window, 1001);
            require(button && IsWindowEnabled(button), "外部录制中必须提供停止录制按钮");
            SendMessageW(button, BM_CLICK, 0, 0);
            require(hud.stop_requested() && !hud.closed() && IsWindowVisible(window),
                "停止录制只发停止请求，不能关闭反馈窗口");
            data["recording"] = false; hud.publish(data); wait_snapshot(hud, 2);
            require(!IsWindowEnabled(button), "录制结束后必须禁用停止按钮");
            hud.finish(Json{{"success", true}}); wait_state(hud, "FINISHED_VISIBLE");
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            require(!hud.closed() && IsWindowVisible(window), "finish后必须持续保留反馈而不是自行关闭");
            const auto stationary = hud.status();
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            const auto later = hud.status();
            std::cerr << "停止快照paint: " << stationary["paint_count"] << " -> " << later["paint_count"] << '\n';
            require(later["paint_count"] == stationary["paint_count"],
                "相同停止快照静置不能持续重画，否则背景和图表可能反复闪烁");
            hud.publish(data); wait_snapshot(hud, 3);
            require(hud.status()["title_updates"] == stationary["title_updates"],
                "相同录制ID不得重复更新标题触发非客户区重绘");
            if (!snapshot_path.empty()) snapshot(window, snapshot_path);
            PostMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!hud.closed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(hud.closed() && !IsWindow(window), "用户关闭路径必须关闭窗口并通知owner结束保留");
        }
        std::cout << "HUD真实窗口专项通过：仅合成ACK、非激活置顶、排空、结束保留与析构关闭\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
