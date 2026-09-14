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
            const auto status = hud.status();
            require(status["success"] && status["dropped_commands"] == 0,
                "完整合成回执应正常排空，HUD不得丢命令");
            require(status["physical_validation_passed"] == false && status["actual_game_speed"].is_null(),
                "窗口验证不能转成真实输入或游戏速度验收");
            require(GetForegroundWindow() == foreground, "HUD刷新与结束不得改变原前台窗口");
            if (!snapshot_path.empty()) snapshot(window, snapshot_path);
        }
        require(!IsWindow(window), "HUD析构必须关闭窗口和UI线程");
        std::cout << "HUD真实窗口专项通过：仅合成ACK、非激活置顶、排空、结束保留与析构关闭\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
