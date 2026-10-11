#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "xen_exe_runtime_fixture.h"
#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include "recoil/recoil.h"
#include "overlay/overlay.h"
#include "app/report_lifecycle_internal.h"
#include "debug/debug.h"
#include "log/log.h"
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

// 组件组合验收：真实 Overlay 先打开选择器，测试宿主再启动其持有的 Runtime。
// 该组合绕过 App 启动准入；不冒充实际 Xen.exe 自然可达流程。

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
    Runtime* runtime = nullptr;
    xen::tests::RuntimeFixture* fixture = nullptr;
    AppConfig* config = nullptr;
    bool runtime_started = false;
    std::uint64_t running_watermark = 0;
    std::uint64_t closing_watermark = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t successful_samples = 0;
    bool runtime_collected = false;
    Clock::time_point started = Clock::now();
    Clock::time_point close_started{};
    Json events = Json::array();
    HWND main_window = nullptr;
    DWORD main_thread_id = 0;
    HWND picker = nullptr;
    HWND prompt = nullptr;
    HANDLE picker_thread = nullptr;
    DWORD picker_thread_id = 0;
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
        if (picker_thread) CloseHandle(picker_thread);
    }

    void record(const char* event, Json detail = Json::object()) {
        detail["event"] = event;
        detail["elapsed_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        events.push_back(std::move(detail));
        std::ofstream stream(output / "shutdown-witness.json");
        stream << Json{{"scope", "component_integration_not_natural_exe_flow"}, {"mode", error_mode ? "error" : "picker"}, {"failed", failed},
            {"failure", failure}, {"rescue_used", rescued}, {"events", events}}.dump(2);
        require(stream.good(), "关闭见证文件写入失败");
    }

    void fail(const char* message) noexcept {
        if (failed) return;
        failed = true;
        try { failure = message; record("failure", {{"message", failure}}); } catch (...) {}
    }

    static bool owned_thread_window(HWND window, DWORD expected_thread) noexcept {
        if (!window || !expected_thread || !IsWindow(window)) return false;
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        return process == GetCurrentProcessId() && thread == expected_thread;
    }

    bool owned_main_window() const noexcept {
        if (!owned_thread_window(main_window, main_thread_id)) return false;
        wchar_t name[128]{};
        return GetClassNameW(main_window, name, 128) > 0 &&
            std::wstring_view(name) == L"XenCodexOverlay";
    }

    void rescue() noexcept {
        // 救援只处理夹具已经识别的自身窗口；任何救援都会永久判失败。
        if (!rescued) {
            rescued = true;
            try { record("rescue"); } catch (...) {}
        }
        if (owned_thread_window(picker, picker_thread_id)) {
            const auto popup = GetWindow(picker, GW_ENABLEDPOPUP);
            if (popup != picker && owned_thread_window(popup, picker_thread_id) &&
                GetWindow(popup, GW_OWNER) == picker)
                PostMessageW(popup, WM_CLOSE, 0, 0);
            // 发送前重新核对，拒绝对已经销毁或被其他线程复用的 HWND 投递消息。
            if (owned_thread_window(picker, picker_thread_id))
                PostMessageW(picker, WM_COMMAND, IDCANCEL, 0);
        }
        if (owned_main_window()) PostMessageW(main_window, WM_CLOSE, 0, 0);
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
            const bool enumerated = EnumWindows([](HWND candidate, LPARAM opaque) -> BOOL {
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
            }, reinterpret_cast<LPARAM>(this)) != FALSE;
            require(enumerated || picker != nullptr, "首次文件选择器窗口枚举失败");
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
        if (!runtime_started) {
            xen::tests::RuntimeFixture::assert_safe(*config);
            fixture->start_sender();
            require(runtime->start(*config), "真实 Runtime 启动失败");
            runtime_started = true;
            record("runtime_started_with_dialog", {{"test_host_started_runtime", true},
                {"physical_output_allowed", false}});
            return;
        }
        const auto snapshot = runtime->snapshot();
        require(snapshot.state == RuntimeState::RUNNING, "组合场景的真实 Runtime 必须持续 RUNNING");
        require(snapshot.provider == "CPUExecutionProvider" && !snapshot.output_allowed_by_config &&
            !snapshot.output_armed && snapshot.mouse_commands == 0 && snapshot.failed_frames == 0 &&
            snapshot.last_error.empty(), "Runtime 非安全 CPU 状态或出现输出/处理错误");
        require(!fixture->sender_evidence().at("failed").get<bool>(), "合成回环发送失败");
        std::vector<RuntimePipelineSample> samples;
        require(runtime->drain_pipeline_samples(samples), "Runtime 诊断样本回收失败");
        sample_count += samples.size();
        for (const auto& sample : samples) {
            require(!sample.mouse_sent, "Runtime 诊断样本出现物理命令发送");
            if (debug_sample_succeeded(sample) && sample.profile.detector.status == DetectionStatus::SUCCESS)
                ++successful_samples;
        }
        if (snapshot.processed_frames < 3 || successful_samples == 0) return;
        require(snapshot.detection_status == DetectionStatus::SUCCESS,
            "Runtime 已处理帧必须包含实际成功推理结果");
        if (running_watermark == 0) {
            running_watermark = snapshot.processed_frames;
            record("runtime_running_first", {{"processed_frames", running_watermark}, {"sample_count", sample_count},
                {"successful_samples", successful_samples}, {"detection_status", "SUCCESS"}});
            return;
        }
        if (snapshot.processed_frames <= running_watermark) return;
        require(IsWindowVisible(picker) && GetWindow(picker, GW_OWNER) == main_window &&
            GetWindowThreadProcessId(picker, nullptr) == picker_thread_id && !IsWindowEnabled(main_window),
            "关闭前选择器必须仍可见且保持本宿主所有权");
        if (error_mode) require(prompt && IsWindowVisible(prompt) && !IsWindowEnabled(picker) &&
            GetWindow(prompt, GW_OWNER) == picker &&
            GetWindowThreadProcessId(prompt, nullptr) == picker_thread_id,
            "关闭前错误提示必须仍可见且保持本选择器所有权");
        closing_watermark = snapshot.processed_frames;
        prepared = true;
        record("runtime_running_continues", {{"processed_frames", closing_watermark},
            {"sample_count", sample_count}, {"successful_samples", successful_samples},
            {"detection_status", "SUCCESS"}, {"mouse_commands", snapshot.mouse_commands}});
        record("ready_to_close", {{"picker_visible", IsWindowVisible(picker) != FALSE},
            {"error_prompt_visible", prompt != nullptr}});
        // 成功路径只投递主窗关闭；宿主调用生产 Overlay 取消与 ReportLifecycle 停止接缝。
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
        require(prepared && close_requested && runtime_collected, "关闭前必须已观察真实运行并回收 Runtime 停止 future");
        require(main_alive, "回收见证必须发生于真实主窗口销毁之前");
        require(picker_gone && prompt_gone, "主窗口销毁前必须回收文件选择器及错误提示");
        require(thread_state == WAIT_OBJECT_0, "主窗口销毁前必须确认实际文件选择线程退出");
        require(close_elapsed <= std::chrono::seconds(3), "关闭屏障超过3秒回收预算");
        const auto snapshot = runtime->snapshot();
        require(snapshot.state == RuntimeState::STOPPED && snapshot.capture_status == CaptureStatus::CLOSED &&
            snapshot.mouse_status == MouseStatus::CLOSED && snapshot.emergency_stopped &&
            !snapshot.output_armed && !snapshot.output_allowed_by_config && snapshot.mouse_commands == 0 &&
            snapshot.failed_frames == 0 && snapshot.last_error.empty() &&
            snapshot.processed_frames >= closing_watermark,
            "主窗销毁前 Runtime 必须 STOPPED/CLOSED 且无输出或处理错误");
        record("runtime_stopped_before_window", {{"state", "STOPPED"}, {"capture_status", "CLOSED"},
            {"mouse_status", "CLOSED"}, {"processed_frames", snapshot.processed_frames},
            {"mouse_commands", snapshot.mouse_commands}, {"failed_frames", snapshot.failed_frames},
            {"stop_future_collected", runtime_collected}, {"sender", fixture->sender_evidence()}});
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

void drive() noexcept {
    if (!active) return;
    try {
        if (!active->installed && ImGui::GetCurrentContext()) {
            active->main_window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            require(active->main_window && IsWindow(active->main_window), "真实 Overlay 主窗口未建立");
            DWORD process = 0;
            active->main_thread_id = GetWindowThreadProcessId(active->main_window, &process);
            require(active->main_thread_id == GetCurrentThreadId() &&
                process == GetCurrentProcessId(), "帧钩子必须安装在本进程真实主窗口线程");
            require(active->owned_main_window(), "帧钩子宿主必须为本进程的生产 Overlay 类窗口");
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
        if (Clock::now() - active->started > std::chrono::seconds(45)) active->fail("Runtime/选择器组合超过45秒有界预算");
        if (active->close_requested && Clock::now() - active->close_started > std::chrono::seconds(3))
            active->fail("主窗口请求关闭后3秒内未完成后台任务回收");
        if (!active->failed) active->observe_native();
    } catch (const std::exception& error) { active->fail(error.what()); }
    catch (...) { active->fail("组件驱动发生未知异常"); }
    if (active->failed) active->rescue();
}

void prepare(const std::filesystem::path& output, AppConfig& config) {
    require(output.is_absolute() && !std::filesystem::exists(output), "必须使用尚不存在的独立绝对输出目录");
    std::filesystem::create_directories(output / "profiles");
    std::filesystem::create_directories(output / "cache/recoil");
    config.recoil.profile_directory = "profiles";
    config.recoil.sensitivity = 1;
    config.ui.width = 1100;
    config.ui.height = 850;
    config.log.log_dir = "logs";
    RecoilProfile imported;
    imported.id = "runtime-dialog-shutdown-fixture";
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

Json remaining_windows() {
    struct Search { Json values = Json::array(); bool failed = false; } search;
    const bool enumerated = EnumWindows([](HWND window, LPARAM opaque) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(opaque);
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
    }, reinterpret_cast<LPARAM>(&search)) != FALSE;
    require(enumerated && !search.failed, "退出窗口枚举失败");
    return search.values;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    Driver driver;
    try {
        require(argc == 4, "用法：runtime_file_dialog_shutdown_tests <证据绝对根目录> <picker|error> <ONNX模型绝对路径>");
        const std::wstring_view mode(argv[2]);
        require(mode == L"picker" || mode == L"error", "关闭场景必须为picker或error");
        driver.error_mode = mode == L"error";
        const std::filesystem::path output_root(argv[1]);
        require(output_root.is_absolute(), "证据根目录必须为绝对路径");
        driver.output = output_root / (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(Clock::now().time_since_epoch().count()));
        AppConfig config;
        prepare(driver.output, config);
        std::cout << "实际见证路径：" << utf8(driver.output / "shutdown-witness.json") << std::endl;
        const auto previous_directory = std::filesystem::current_path();
        struct RestoreDirectory {
            std::filesystem::path path;
            ~RestoreDirectory() { std::error_code error; std::filesystem::current_path(path, error); }
        } restore{previous_directory};
        std::filesystem::current_path(driver.output);
        xen::tests::RuntimeFixture fixture(driver.output, std::filesystem::path(argv[3]));
        fixture.prepare(config);
        // App 原有模型解析不在本组件宿主中；向公开 Runtime.start 提供本次模型的绝对路径。
        config.detector.model_path = utf8(driver.output / "models/runtime-shutdown-fixture.onnx");
        std::string error;
        const bool saved = save_app_config(utf8(driver.output / "config.ini"), config, error);
        require(saved, error.c_str());
        struct LogLifetime {
            bool closed = false;
            void shutdown() {
                if (closed) return;
                closed = true;
                Log::shutdown();
            }
            ~LogLifetime() { shutdown(); }
        } logging;
        Log::init(config.log);
        require(Log::initialized(), "组合验收日志初始化失败");
        Runtime runtime;
        Overlay overlay;
        driver.runtime = &runtime;
        driver.fixture = &fixture;
        driver.config = &config;
        active = &driver;
        require(overlay.init(config.ui), "真实 Overlay 初始化失败");
        driver.record("component_entry_prepared", {{"physical_output_allowed", false},
            {"scope", "real_overlay_runtime_composition"}, {"natural_exe_reachable", false}});
        bool report_active = false, archive_active = false;
        app::detail::ReportFinalization finalization;
        app::detail::ReportLifecycle lifecycle(report_active, archive_active, finalization);
        bool stop_started = false;
        const auto request_stop = [&](bool retry) {
            if (stop_started) return;
            const auto requested = lifecycle.stop(runtime, [] { return true; }, retry);
            require(requested == app::detail::ReportStopRequest::STARTED,
                "真实 Runtime 停止必须创建生产生命周期任务");
            stop_started = true;
            driver.record("runtime_stop_requested");
        };
        const auto collect_stop = [&](bool succeeded, app::detail::ReportJobKind kind) {
            require(succeeded && kind == app::detail::ReportJobKind::STOP, "Runtime 停止 future 失败");
            driver.runtime_collected = true;
            driver.record("runtime_stop_collected");
        };
        OverlayModelCatalog models;
        OverlayBackendCatalog backends;
        backends.backends = {BackendType::CPU};
        model_workspace::Settings settings;
        model_workspace::Snapshot workspace;
        try {
            for (;;) {
                require(overlay.pump_messages(true), "组合关闭屏障前 Overlay 消息循环提前退出");
                drive();
                require(!driver.failed, driver.failure.c_str());
                lifecycle.poll(overlay.background_busy(), request_stop, collect_stop);
                if (overlay.close_requested()) {
                    overlay.cancel_background();
                    overlay.poll_background();
                    if (!overlay.background_busy()) request_stop(false);
                    if (!overlay.background_busy() && driver.runtime_collected && !lifecycle.busy()) break;
                }
                OverlayActions actions;
                require(overlay.render(runtime.snapshot(), {}, models, backends, config, settings,
                    workspace, "Runtime与文件选择器组合集成验证；物理输出关闭", actions), "真实 Overlay 渲染失败");
                require(actions.debug_action == debug_session::Action::NONE && !actions.start_requested &&
                    !actions.debug_allow_physical_output, "测试导航产生未授权业务动作");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(driver.prepared && driver.close_requested && !driver.rescued && !overlay.background_busy(),
                "必须通过公开关闭接缝完成组合回收且未使用救援");
            fixture.stop_sender();
            const auto sender = fixture.sender_evidence();
            require(sender.at("started").get<bool>() && sender.at("finished").get<bool>() &&
                sender.at("joined").get<bool>() && !sender.at("failed").get<bool>(), "合成发送器未完成线程回收");
            overlay.shutdown();
            require(driver.shutdown_seen && driver.shutdown_passed && !driver.failed,
                "主窗销毁前的组合关闭见证未通过");
            require(Clock::now() - driver.close_started <= std::chrono::seconds(3),
                "Overlay 同步 shutdown 完成超过3秒关闭预算");
            const auto windows = remaining_windows();
            logging.shutdown();
            driver.record("component_returned", {{"main_window_gone", !IsWindow(driver.main_window)},
                {"imgui_context_gone", ImGui::GetCurrentContext() == nullptr},
                {"remaining_process_windows", windows}, {"stop_future_busy", lifecycle.busy()},
                {"close_elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - driver.close_started).count()}});
            require(!IsWindow(driver.main_window) && ImGui::GetCurrentContext() == nullptr &&
                windows.empty() && !lifecycle.busy(), "组合验收结束后仍有窗口、上下文或停止 future");
            require(Clock::now() - driver.close_started <= std::chrono::seconds(3),
                "组件回收及见证写入完成超过3秒关闭预算");
            driver.record("passed");
        } catch (const std::exception& exception) {
            driver.fail(exception.what());
            driver.rescue();
            overlay.cancel_background();
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (overlay.background_busy() && Clock::now() < deadline) {
                overlay.pump_messages(true);
                overlay.poll_background();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            // 失败清理也只触及本宿主持有的 Runtime/窗口/发送器；不能将救援计为通过。
            runtime.stop();
            fixture.stop_sender();
            overlay.shutdown();
            throw;
        }
        active = nullptr;
        std::cout << "真实 Runtime 与选择器组件组合关闭通过：" << (driver.error_mode ? "错误提示" : "文件选择器")
            << "；不代表实际 Xen.exe 可自然进入该组合。\n";
        return 0;
    } catch (const std::exception& error) {
        active = nullptr;
        std::cerr << "Runtime 与选择器组件组合关闭失败：" << error.what() << '\n';
        return 1;
    }
}
