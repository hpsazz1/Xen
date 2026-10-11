#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include "recoil/recoil.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int);

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

struct Driver {
    enum class Stage { START, DEBUG_PAGE, RECOIL_PAGE, TREE_SCROLL,
        TREE_CLICK, FILE_FOCUS, FILE_SCROLL, FILE_CLICK, NATIVE, CLOSING };
    std::filesystem::path output;
    bool error_mode = false;
    Clock::time_point started = Clock::now();
    Clock::time_point close_started{};
    Json events = Json::array();
    HWND main_window = nullptr;
    HWND picker = nullptr;
    HWND prompt = nullptr;
    HANDLE picker_thread = nullptr;
    DWORD picker_thread_id = 0;
    UINT_PTR timer = 0;
    bool installed = false;
    bool failed = false;
    bool rescued = false;
    bool prepared = false;
    bool close_requested = false;
    bool shutdown_seen = false;
    bool shutdown_passed = false;
    bool missing_submitted = false;
    std::string failure;
    Stage stage = Stage::START;
    Stage after_click = Stage::START;
    int wait_frames = 5;
    int click_phase = -1;
    ImVec2 position{400, 40};
    ImGuiWindow* focus_window = nullptr;
    ImGuiID focus_request = 0;
    ImGuiID target = 0;
    ImGuiID recoil_tab = 0;
    std::string frame_text;

    ~Driver() {
        if (timer) KillTimer(nullptr, timer);
        if (picker_thread) CloseHandle(picker_thread);
    }

    void record(const char* event, Json detail = Json::object()) {
        detail["event"] = event;
        detail["elapsed_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        events.push_back(std::move(detail));
        std::ofstream stream(output / "shutdown-witness.json");
        stream << Json{{"mode", error_mode ? "error" : "picker"}, {"failed", failed},
            {"failure", failure}, {"rescue_used", rescued}, {"events", events}}.dump(2);
        require(stream.good(), "关闭见证文件写入失败");
    }

    void fail(const char* message) noexcept {
        if (failed) return;
        failed = true;
        try { failure = message; record("failure", {{"message", failure}}); } catch (...) {}
    }

    void rescue() noexcept {
        // 救援只处理夹具已经识别的自身窗口；任何救援都会永久判失败。
        if (!rescued) {
            rescued = true;
            try { record("rescue"); } catch (...) {}
        }
        if (picker && IsWindow(picker)) {
            const auto popup = GetWindow(picker, GW_ENABLEDPOPUP);
            if (popup && popup != picker && GetWindow(popup, GW_OWNER) == picker &&
                GetWindowThreadProcessId(popup, nullptr) == picker_thread_id)
                PostMessageW(popup, WM_CLOSE, 0, 0);
            PostMessageW(picker, WM_COMMAND, IDCANCEL, 0);
        }
        if (main_window && IsWindow(main_window)) PostMessageW(main_window, WM_CLOSE, 0, 0);
    }

    ImGuiWindow* window(const char* name) {
        const std::string prefix = std::string(name) + '_';
        auto* context = ImGui::GetCurrentContext();
        for (auto* candidate : context->Windows) {
            std::string_view leaf(candidate->Name);
            if (const auto slash = leaf.rfind('/'); slash != std::string_view::npos) leaf.remove_prefix(slash + 1);
            if (leaf.starts_with(prefix) && candidate->LastFrameActive == context->FrameCount) return candidate;
        }
        return nullptr;
    }

    void click(ImVec2 point, Stage next) {
        position = point;
        click_phase = 0;
        after_click = next;
    }

    void focus(ImGuiID id, Stage next) {
        focus_window = window("content");
        require(focus_window != nullptr, "真实内容窗口未出现");
        target = focus_request = id;
        stage = next;
        wait_frames = 1;
    }

    ImRect target_rectangle() {
        auto* context = ImGui::GetCurrentContext();
        require(context->NavId == target && context->NavIdIsAlive,
            "生产目标控件未提交有效导航矩形");
        return ImGui::WindowRectRelToAbs(focus_window, focus_window->NavRectRel[ImGuiNavLayer_Main]);
    }

    void advance_ui() {
        auto* context = ImGui::GetCurrentContext();
        frame_text = context->LogBuffer.c_str();
        ImGui::LogFinish();
        if (failed || stage == Stage::NATIVE || stage == Stage::CLOSING) return;
        if (click_phase >= 0) {
            if (++click_phase == 3) { click_phase = -1; stage = after_click; wait_frames = 6; }
            return;
        }
        if (wait_frames > 0) { --wait_frames; return; }
        auto* content = window("content");
        switch (stage) {
        case Stage::START: {
            auto* sidebar = window("sidebar");
            require(sidebar != nullptr, "真实应用侧栏未出现");
            const ImVec2 point{sidebar->Pos.x + sidebar->Size.x * .5f,
                sidebar->Pos.y + sidebar->WindowPadding.y + 21.f + 6.f * (42.f + ImGui::GetStyle().ItemSpacing.y)};
            require(sidebar->ClipRect.Contains(point), "调试导航不在可点击区域");
            click(point, Stage::DEBUG_PAGE);
            record("click_debug_page");
            break;
        }
        case Stage::DEBUG_PAGE: {
            require(content != nullptr, "调试页内容窗口缺失");
            auto* bar = context->TabBars.GetByKey(content->GetID("debug_tabs"));
            require(bar != nullptr, "真实调试页标签栏未出现");
            for (auto& tab : bar->Tabs) {
                if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) != "弹道工具") continue;
                recoil_tab = tab.ID;
                const ImVec2 point{bar->BarRect.Min.x + tab.Offset + tab.Width * .5f, bar->BarRect.GetCenter().y};
                require(content->ClipRect.Contains(point), "弹道工具标签不在可点击区域");
                click(point, Stage::RECOIL_PAGE);
                record("click_recoil_tab");
                return;
            }
            throw std::runtime_error("真实弹道工具标签不存在");
        }
        case Stage::RECOIL_PAGE:
            if (frame_text.find("正在后台处理文件或分析") != std::string::npos) return;
            require(frame_text.find("弹道工具与射击归档") != std::string::npos, "点击未进入真实弹道工具页");
            focus(ImHashStr("导入已有曲线 JSON", 0, recoil_tab), Stage::TREE_SCROLL);
            break;
        case Stage::TREE_SCROLL:
        case Stage::FILE_SCROLL: {
            ImGui::ScrollToRect(focus_window, target_rectangle(), ImGuiScrollFlags_KeepVisibleEdgeY);
            focus_request = target;
            stage = stage == Stage::TREE_SCROLL ? Stage::TREE_CLICK : Stage::FILE_CLICK;
            wait_frames = 2;
            break;
        }
        case Stage::TREE_CLICK:
        case Stage::FILE_CLICK: {
            const auto rectangle = target_rectangle();
            require(rectangle.GetWidth() > 0 && rectangle.GetHeight() > 0 &&
                focus_window->ClipRect.Contains(rectangle.GetCenter()), "真实文件选择控件未滚入可点击区域");
            const bool tree = stage == Stage::TREE_CLICK;
            click(rectangle.GetCenter(), tree ? Stage::FILE_FOCUS : Stage::NATIVE);
            record(tree ? "click_import_tree" : "click_file_picker");
            break;
        }
        case Stage::FILE_FOCUS:
            focus(ImHashStr("选择曲线文件", 0, ImHashStr("导入已有曲线 JSON", 0, recoil_tab)), Stage::FILE_SCROLL);
            break;
        default: break;
        }
    }

    void observe_native() {
        if (stage != Stage::NATIVE || prepared) return;
        if (!picker) {
            EnumWindows([](HWND candidate, LPARAM opaque) -> BOOL {
                auto& driver = *reinterpret_cast<Driver*>(opaque);
                DWORD process = 0;
                const auto thread = GetWindowThreadProcessId(candidate, &process);
                if (process != GetCurrentProcessId() || thread == GetCurrentThreadId() ||
                    !IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER) != driver.main_window) return TRUE;
                wchar_t title[128]{};
                GetWindowTextW(candidate, title, 128);
                if (std::wstring_view(title) != L"选择弹道曲线 JSON") return TRUE;
                driver.picker = candidate;
                driver.picker_thread_id = thread;
                return FALSE;
            }, reinterpret_cast<LPARAM>(this));
            if (!picker) return;
            picker_thread = OpenThread(SYNCHRONIZE, FALSE, picker_thread_id);
            require(picker_thread != nullptr, "无法持有实际文件选择线程句柄");
            require(!IsWindowEnabled(main_window), "可见文件选择器必须实际禁用主窗口");
            record("picker_visible", {{"thread_id", picker_thread_id}, {"main_disabled", true}});
        }
        if (error_mode && !prompt) {
            if (!missing_submitted) {
                HWND edit = nullptr;
                EnumChildWindows(picker, [](HWND candidate, LPARAM opaque) -> BOOL {
                    wchar_t name[64]{};
                    GetClassNameW(candidate, name, 64);
                    if (IsWindowVisible(candidate) && std::wstring_view(name) == L"Edit") {
                        *reinterpret_cast<HWND*>(opaque) = candidate;
                        return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&edit));
                require(edit != nullptr, "真实文件选择器的文件名输入框缺失");
                const auto missing = output / L"missing-profile.json";
                require(!std::filesystem::exists(missing), "错误提示夹具的文件必须不存在");
                DWORD_PTR result = 0;
                require(SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(missing.c_str()),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0, "不存在文件名输入失败");
                require(PostMessageW(picker, WM_COMMAND, IDOK, 0) != 0, "无法提交不存在文件名");
                missing_submitted = true;
                record("missing_file_submitted");
                return;
            }
            const auto popup = GetWindow(picker, GW_ENABLEDPOPUP);
            if (!popup || popup == picker || !IsWindowVisible(popup) || IsWindowEnabled(picker)) return;
            require(GetWindow(popup, GW_OWNER) == picker &&
                GetWindowThreadProcessId(popup, nullptr) == picker_thread_id,
                "错误提示必须属于已固定的文件选择器和线程");
            prompt = popup;
            record("error_prompt_visible", {{"picker_disabled", true}});
        }
        require(WaitForSingleObject(picker_thread, 0) == WAIT_TIMEOUT,
            "关闭前必须确认实际文件选择线程仍在运行");
        prepared = true;
        record("ready_to_close", {{"picker_visible", IsWindowVisible(picker) != FALSE},
            {"error_prompt_visible", prompt != nullptr}});
        // 唯一成功路径只关闭真实主窗口，由生产 App 负责取消、poll 和 shutdown。
        require(PostMessageW(main_window, WM_CLOSE, 0, 0) != 0, "主窗口关闭请求投递失败");
        require(PostMessageW(main_window, WM_CLOSE, 0, 0) != 0, "重复主窗口关闭请求投递失败");
        close_requested = true;
        close_started = Clock::now();
        stage = Stage::CLOSING;
        record("main_close_requested", {{"repeat_count", 2}, {"deadline_ms", 3000}});
    }

    void observe_shutdown() {
        shutdown_seen = true;
        const bool main_alive = main_window && IsWindow(main_window);
        const bool picker_gone = picker && !IsWindow(picker);
        const bool prompt_gone = !prompt || !IsWindow(prompt);
        const DWORD thread_state = picker_thread ? WaitForSingleObject(picker_thread, 0) : WAIT_FAILED;
        const auto close_elapsed = Clock::now() - close_started;
        record("imgui_shutdown", {{"main_window_alive", main_alive}, {"picker_gone", picker_gone},
            {"prompt_gone", prompt_gone}, {"picker_thread_signaled", thread_state == WAIT_OBJECT_0},
            {"thread_wait_result", thread_state}, {"close_requested", close_requested},
            {"close_elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(close_elapsed).count()}});
        require(prepared && close_requested, "完整应用必须在文件选择场景准备完成后关闭");
        require(main_alive, "回收见证必须发生于真实主窗口销毁之前");
        require(picker_gone && prompt_gone, "主窗口销毁前必须回收文件选择器及错误提示");
        require(thread_state == WAIT_OBJECT_0, "主窗口销毁前必须确认实际文件选择线程退出");
        require(close_elapsed <= std::chrono::seconds(3), "关闭屏障超过3秒回收预算");
        shutdown_passed = true;
    }
};

Driver* active = nullptr;

void input_hook(ImGuiContext* context, ImGuiContextHook*) noexcept {
    try {
        if (!active || active->failed) return;
        context->IO.AddMousePosEvent(active->position.x, active->position.y);
        context->IO.AddMouseButtonEvent(0, active->click_phase == 1);
    } catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("输入钩子发生未知异常"); }
}

void frame_begin_hook(ImGuiContext*, ImGuiContextHook*) noexcept {
    try {
        if (active->focus_request) {
            ImGui::SetFocusID(active->focus_request, active->focus_window);
            active->focus_request = 0;
        }
        ImGui::LogToBuffer(0);
    } catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("帧开始钩子发生未知异常"); }
}

void frame_end_hook(ImGuiContext*, ImGuiContextHook*) noexcept {
    try { active->advance_ui(); }
    catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("帧结束钩子发生未知异常"); }
}

void shutdown_hook(ImGuiContext*, ImGuiContextHook*) noexcept {
    try { active->observe_shutdown(); }
    catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("关闭见证钩子发生未知异常"); }
}

void CALLBACK drive_timer(HWND, UINT, UINT_PTR, DWORD) noexcept {
    if (!active) return;
    try {
        if (!active->installed && ImGui::GetCurrentContext()) {
            active->main_window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            require(active->main_window && IsWindow(active->main_window), "真实 Overlay 主窗口未建立");
            DWORD process = 0;
            require(GetWindowThreadProcessId(active->main_window, &process) == GetCurrentThreadId() &&
                process == GetCurrentProcessId(), "帧钩子必须安装在本进程真实主窗口线程");
            const auto add = [](ImGuiContextHookType type, ImGuiContextHookCallback callback) {
                ImGuiContextHook hook;
                hook.Type = type;
                hook.Callback = callback;
                ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
            };
            add(ImGuiContextHookType_NewFramePre, input_hook);
            add(ImGuiContextHookType_NewFramePost, frame_begin_hook);
            add(ImGuiContextHookType_EndFramePre, frame_end_hook);
            add(ImGuiContextHookType_Shutdown, shutdown_hook);
            ImGui::GetIO().ConfigInputTrickleEventQueue = false;
            active->installed = true;
            active->record("production_overlay_ready");
        }
        if (Clock::now() - active->started > std::chrono::seconds(30)) active->fail("完整应用关闭超过30秒有界预算");
        if (active->close_requested && Clock::now() - active->close_started > std::chrono::seconds(3))
            active->fail("主窗口请求关闭后3秒内未完成后台任务回收");
        if (!active->failed) active->observe_native();
    } catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("应用驱动定时器发生未知异常"); }
    if (active->failed) active->rescue();
}

void prepare(const std::filesystem::path& output) {
    require(output.is_absolute() && !std::filesystem::exists(output), "必须使用尚不存在的独立绝对输出目录");
    std::filesystem::create_directories(output / "profiles");
    std::filesystem::create_directories(output / "cache/recoil");
    AppConfig config;
    config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
    config.mouse.allow_send_input = false;
    config.keyboard.aim_hold_virtual_keys.clear();
    config.keyboard.emergency_virtual_keys.clear();
    config.keyboard.runtime_toggle_virtual_keys.clear();
    config.keyboard.anomaly_mark_virtual_keys.clear();
    config.keyboard.debug_test_virtual_keys.clear();
    config.keyboard.debug_test_enabled = false;
    config.auto_stop.enabled = config.auto_stop.cycle_enabled = false;
    config.trigger.enabled = config.trigger.fire_enabled = false;
    config.recoil.enabled = config.movement.enabled = config.gsi.enabled = config.source_context.enabled = false;
    config.recoil.profile_directory = "profiles";
    config.recoil.sensitivity = 1;
    config.runtime.diagnostics_enabled = false;
    config.ui.width = 1100;
    config.ui.height = 850;
    config.log.log_dir = "logs";
    std::string error;
    const bool saved = save_app_config(utf8(output / "config.ini"), config, error);
    require(saved, error.c_str());
    RecoilProfile imported;
    imported.id = "app-shutdown-fixture";
    imported.weapon_id = "ak47";
    imported.state = RecoilProfileState::IMPORTED;
    imported.points = {{0, 0, 0}, {100, 2, 4}};
    { std::ofstream stream(output / "profiles/imported.json"); stream << serialize_recoil_profile(imported); }
    { std::ofstream stream(output / "cache/recoil/workflow-settings.json"); stream << R"({"ak47":{"selected_file":"imported.json"}})"; }
    require(SetEnvironmentVariableW(L"XEN_DATA_ROOT", output.c_str()) != 0, "测试数据根设置失败");
    require(SetEnvironmentVariableW(L"XEN_RELEASE_ROOT", nullptr) != 0 &&
        SetEnvironmentVariableW(L"XEN_RUNTIME_ID", nullptr) != 0 &&
        SetEnvironmentVariableW(L"XEN_RELEASE_BACKENDS", nullptr) != 0, "测试发布环境隔离失败");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 3, "用法：app_file_dialog_shutdown_tests <证据根目录> <picker|error>");
        const std::wstring_view mode(argv[2]);
        require(mode == L"picker" || mode == L"error", "关闭场景必须为picker或error");
        Driver driver;
        driver.error_mode = mode == L"error";
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        driver.output = std::filesystem::absolute(argv[1]) /
            (std::wstring(driver.error_mode ? L"error-" : L"picker-") +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(stamp));
        prepare(driver.output);
        std::cout << "实际证据目录：" << utf8(driver.output) << std::endl;
        active = &driver;
        driver.record("entry_prepared", {{"physical_output_allowed", false}, {"production_entry", "wWinMain"}});
        driver.timer = SetTimer(nullptr, 0, 20, drive_timer);
        require(driver.timer != 0, "应用集成驱动定时器创建失败");
        const int result = wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWNORMAL);
        KillTimer(nullptr, driver.timer);
        driver.timer = 0;
        struct RemainingWindows { Json values = Json::array(); bool failed = false; } windows;
        const bool enumerated = EnumWindows([](HWND window, LPARAM opaque) -> BOOL {
            auto& found = *reinterpret_cast<RemainingWindows*>(opaque);
            try {
                DWORD process = 0;
                GetWindowThreadProcessId(window, &process);
                if (process == GetCurrentProcessId()) {
                    wchar_t name[128]{};
                    GetClassNameW(window, name, 128);
                    found.values.push_back({{"class", utf8(std::filesystem::path(name))},
                        {"visible", IsWindowVisible(window) != FALSE}});
                }
                return TRUE;
            } catch (...) { found.failed = true; return FALSE; }
        }, reinterpret_cast<LPARAM>(&windows)) != FALSE;
        windows.failed = windows.failed || !enumerated;
        const bool no_context = ImGui::GetCurrentContext() == nullptr;
        const bool main_gone = driver.main_window && !IsWindow(driver.main_window);
        driver.record("production_entry_returned", {{"exit_code", result}, {"main_window_gone", main_gone},
            {"imgui_context_gone", no_context}, {"remaining_process_windows", windows.values},
            {"window_enumeration_failed", windows.failed}});
        if (!(result == 0 && driver.shutdown_seen && driver.shutdown_passed && !driver.rescued))
            driver.fail("生产入口必须在完整关闭见证通过后正常返回0且未使用救援");
        if (!(main_gone && no_context && !windows.failed && windows.values.empty()))
            driver.fail("生产入口返回后仍遗留本进程窗口或ImGui上下文");
        if (driver.close_requested && Clock::now() - driver.close_started > std::chrono::seconds(3))
            driver.fail("生产入口返回超过3秒关闭预算");
        require(!driver.failed, driver.failure.c_str());
        driver.record("passed");
        active = nullptr;
        std::cout << "完整应用文件选择关闭集成验证通过：" << (driver.error_mode ? "错误提示" : "文件选择器") << '\n';
        return 0;
    } catch (const std::exception& error) {
        active = nullptr;
        std::cerr << "完整应用关闭集成验证失败：" << error.what() << '\n';
        return 1;
    }
}
