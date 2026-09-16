#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <wrl/client.h>
#ifdef ERROR
#undef ERROR
#endif

#include "overlay/overlay.h"
#include "log/log.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <chrono>
#include <iterator>
#include <exception>
#include <sstream>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct UiInput {
    ImVec2 position{400.0f, 40.0f};
    bool down = false;
    float wheel = 0.0f;
    ImGuiWindow* focus_window = nullptr;
    ImGuiID focus_id = 0;
};

// 仅向本进程 ImGui 队列注入指针事件，绝不移动系统光标或发送设备输入。
void inject_input(ImGuiContext* context, ImGuiContextHook* hook) {
    auto& input = *static_cast<UiInput*>(hook->UserData);
    auto& io = context->IO;
    io.AddMousePosEvent(input.position.x, input.position.y);
    io.AddMouseButtonEvent(0, input.down);
    if (input.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, input.wheel);
    input.wheel = 0.0f;
}

struct RenderCapture {
    cv::Mat image;
    std::string error;
    std::string text;
    int frame = -1;
};

void begin_preview_frame(ImGuiContext*, ImGuiContextHook* hook) {
    auto& input = *static_cast<UiInput*>(hook->UserData);
    if (input.focus_id) {
        // 仅设置本进程导航焦点，不激活业务按钮；由下一次ItemAdd给出真实矩形。
        ImGui::SetFocusID(input.focus_id, input.focus_window);
        input.focus_id = 0;
    }
    ImGui::LogToBuffer(0);
}

// 只读取当前 Overlay draw callback 暴露的渲染目标，不查找桌面或其他窗口。
void capture_render_target(const ImDrawList*, const ImDrawCmd* command) {
    auto& capture = *static_cast<RenderCapture*>(command->UserCallbackData);
    try {
        using Microsoft::WRL::ComPtr;
        auto* state = static_cast<ImGui_ImplDX11_RenderState*>(
            ImGui::GetPlatformIO().Renderer_RenderState);
        require(state && state->Device && state->DeviceContext, "当前回调没有 DX11 渲染状态");
        ComPtr<ID3D11RenderTargetView> target;
        state->DeviceContext->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        require(target != nullptr, "当前 Overlay 没有渲染目标");
        ComPtr<ID3D11Resource> resource;
        target->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(resource.As(&texture)), "Overlay 渲染目标不是二维纹理");
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        require((rgba || bgra) && description.SampleDesc.Count == 1 &&
                description.MipLevels == 1 && description.ArraySize == 1,
                "窗口截图不支持当前纹理格式或采样布局");
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(state->Device->CreateTexture2D(&description, nullptr,
                                                       staging.GetAddressOf())), "创建截图 staging 失败");
        state->DeviceContext->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(state->DeviceContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "读取 Overlay 渲染像素失败");
        struct Unmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~Unmap() { context->Unmap(texture, 0); }
        } unmap{state->DeviceContext, staging.Get()};
        cv::Mat result = cv::Mat(static_cast<int>(description.Height),
            static_cast<int>(description.Width), CV_8UC4, mapped.pData, mapped.RowPitch).clone();
        if (rgba) {
            for (int y = 0; y < result.rows; ++y) {
                auto* row = result.ptr<cv::Vec4b>(y);
                for (int x = 0; x < result.cols; ++x) std::swap(row[x][0], row[x][2]);
            }
        }
        capture.image = std::move(result);
        capture.error.clear();
        capture.frame = ImGui::GetFrameCount();
    } catch (const std::exception& error) {
        capture.image.release();
        capture.error = error.what();
    }
}

void append_capture(ImGuiContext* context, ImGuiContextHook* hook) {
    auto& capture = *static_cast<RenderCapture*>(hook->UserData);
    capture.text = context->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::GetForegroundDrawList()->AddCallback(capture_render_target, hook->UserData);
}

void save_window(const RenderCapture& capture, const std::filesystem::path& path) {
    require(capture.error.empty(), capture.error.c_str());
    require(!capture.image.empty() && capture.frame == ImGui::GetFrameCount(), "当前帧没有渲染截图");
    const auto& copy = capture.image;
    // 排除窗口边缘后检查正文区域，避免白底加边框被误认为有效渲染。
    const cv::Rect center(copy.cols / 4, copy.rows / 5, copy.cols / 2, copy.rows * 3 / 5);
    cv::Scalar mean, deviation;
    cv::meanStdDev(copy(center), mean, deviation);
    require(deviation[0] + deviation[1] + deviation[2] > 12.0,
            "窗口正文区域近乎纯色，拒绝作为 UI 验收证据");
    std::vector<unsigned char> encoded;
    require(cv::imencode(".png", copy, encoded), "PNG 编码失败");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    require(output.good(), "PNG 写入失败");
    auto text_path = path;
    text_path.replace_extension(".txt");
    std::ofstream text_output(text_path, std::ios::binary);
    text_output << capture.text;
    require(text_output.good(), "当帧文字写入失败");
}

struct CaptureFailure {
    const RenderCapture& capture;
    std::filesystem::path output;
    ~CaptureFailure() {
        if (std::uncaught_exceptions() == 0) return;
        try { save_window(capture, output / "failure.png"); } catch (...) {}
    }
};

ImGuiWindow* preview_window(const char* name) {
    auto* context = ImGui::GetCurrentContext();
    const std::string prefix = std::string(name) + '_';
    for (auto* window : context->Windows) {
        std::string_view leaf(window->Name);
        if (const auto slash = leaf.rfind('/'); slash != std::string_view::npos) leaf.remove_prefix(slash + 1);
        if (leaf.starts_with(prefix) && window->LastFrameActive == context->FrameCount) return window;
    }
    throw std::runtime_error(std::string("生产预览子窗口未出现：") + name);
}

void require_tooltip(const RenderCapture& capture, const char* expected) {
    require(capture.text.find(expected) != std::string::npos, "当帧未出现预期帮助文字");
    auto* context = ImGui::GetCurrentContext();
    for (auto* window : context->Windows) {
        if (!(window->Flags & ImGuiWindowFlags_Tooltip) || window->Hidden ||
            window->LastFrameActive != context->FrameCount) continue;
        require(window->Pos.x >= -1 && window->Pos.y >= -1 &&
            window->Pos.x + window->Size.x <= context->IO.DisplaySize.x + 1 &&
            window->Pos.y + window->Size.y <= context->IO.DisplaySize.y + 1, "帮助浮层超出预览窗口");
        return;
    }
    throw std::runtime_error("没有当帧可见的生产帮助浮层");
}

void require_page_table(const char* table_name) {
    auto* context = ImGui::GetCurrentContext();
    for (auto* window : context->Windows) {
        auto* table = context->Tables.GetByKey(window->GetID(table_name));
        if (table && table->LastFrameActive == context->FrameCount) return;
    }
    throw std::runtime_error("导航未到达预期生产页面，拒绝保存误导截图");
}
}

// 只构造显示层与合成快照。无 Runtime 实例、Session、设备、用户配置读取或业务动作执行。
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "用法：debug_workspace_ui_preview.exe <独立截图目录> [--dark] [--auxiliary]");
        const auto output = std::filesystem::absolute(argv[1]);
        require(!std::filesystem::exists(output), "截图目录必须独立且尚不存在");
        require(std::filesystem::create_directories(output), "无法创建独立截图目录");
        LogConfig logs; logs.enable_file = false; logs.enable_debug_file = false; Log::init(logs);
        Overlay overlay; AppConfig config;
        config.ui.width = kMinimumUiWidth; config.ui.height = kMinimumUiHeight;
        config.ui.open_detached_preview_on_start = false;
        config.mouse.allow_send_input = false;
        config.auto_stop.enabled = false; config.trigger.enabled = false; config.recoil.enabled = false;
        // 即使未来新增自动浏览逻辑，也只能命中本测试独立空目录。
        config.recoil.profile_directory = (output / "unused-fixture").string();
        config.weapon_timing_file = (output / "unused-timing.json").string();
        bool auxiliary = false;
        for (int i = 2; i < argc; ++i) {
            const std::wstring_view option(argv[i]);
            if (option == L"--dark") config.ui.theme = UiTheme::DARK;
            else if (option == L"--auxiliary") auxiliary = true;
            else require(false, "未知参数");
        }
        require(overlay.init(config.ui), "Overlay 初始化失败");
        RuntimeSnapshot runtime; runtime.state = RuntimeState::STOPPED;
        if (auxiliary) {
            config.mouse.backend = MouseBackend::KMBOX_NET;
            config.auto_stop.enabled = config.auto_stop.cycle_enabled = config.gsi.enabled = true;
            config.auto_stop.activation_virtual_key = 5;
            runtime.weapon_snapshot.canonical_id = "m4a1_s";
            runtime.weapon_snapshot.status = weapon::Status::READY;
            runtime.weapon_snapshot.valid = true;
            runtime.auto_stop.status = AutoStopStatus::READY;
            runtime.auto_stop.cycle_moving = true; runtime.auto_stop.cycle_count = 3;
            runtime.auto_stop.independent_trigger_enabled = runtime.auto_stop.source_focused = true;
            runtime.auto_stop.target_available = runtime.auto_stop.telemetry_available = true;
            runtime.auto_stop.use_counterpulse_timing = true;
            runtime.auto_stop.counter_hold_ms = 40; runtime.auto_stop.shot_after_release_ms = 18;
        }
        model_workspace::Settings settings; model_workspace::Snapshot workspace; OverlayActions actions;
        debug_session::Snapshot debug;
        debug.state = debug_session::State::COMPLETED;
        debug.message = "合成 UI 快照：无设备、无真实验收";
        debug.plan = {{"schema_version",2},{"baseline","counter"},{"shots",4},{"capture_enabled",false},
            {"move_ms",120},{"counter_hold_ms",40},{"counter_delay_ms",0},{"shot_after_release_ms",18},
            {"shot_hold_ms",60},{"fire_interval_ms",700},{"direction",2}};
        debug.sampling = {{"default_baseline",{{"说明","默认参考并未重跑模型"},{"default_ratio",0.1}}},
            {"actual_parameters",{{"active_ratio",0.2},{"说明","本次合成实际参数"}}}};
        auto analysis = std::make_shared<debug_session::Json>();
        (*analysis)["shots"] = debug_session::Json::array({
            {{"previous_down_submit_interval_ms",nullptr},{"down_ack_to_up_submit_ms",60},{"observed_hold_ms",62},{"valid_command_hold",true},
                {"down_model",{{"valid",true},{"within_model_threshold",true}}}},
            {{"previous_down_submit_interval_ms",0},{"down_ack_to_up_submit_ms",0},{"observed_hold_ms",0},{"valid_command_hold",true},
                {"down_model",{{"valid",true},{"within_model_threshold",false}}}},
            {{"previous_down_submit_interval_ms",700},{"down_ack_to_up_submit_ms",nullptr},{"observed_hold_ms",nullptr},{"valid_command_hold",false},
                {"down_model",{{"valid",false},{"within_model_threshold",nullptr}}}},
            {{"previous_down_submit_interval_ms",705},{"down_ack_to_up_submit_ms",60},{"observed_hold_ms",63},{"valid_command_hold",true},
                {"down_model",{{"valid",true},{"within_model_threshold",true}}}}});
        debug.result = analysis;
        debug.timing_catalog = weapon::default_timing_catalog(); debug.timing_catalog_valid = true;
        UiInput input; RenderCapture capture; CaptureFailure failure{capture,output};
        ImGuiContextHook hook; hook.Type = ImGuiContextHookType_NewFramePre; hook.Callback = inject_input; hook.UserData = &input;
        const auto hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
        ImGuiContextHook frame_hook; frame_hook.Type = ImGuiContextHookType_NewFramePost; frame_hook.Callback = begin_preview_frame; frame_hook.UserData = &input;
        const auto frame_hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &frame_hook);
        ImGuiContextHook capture_hook; capture_hook.Type = ImGuiContextHookType_EndFramePre; capture_hook.Callback = append_capture; capture_hook.UserData = &capture;
        const auto capture_hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &capture_hook);
        ImGui::GetIO().ConfigInputTrickleEventQueue = false;
        int frames = 0;
        std::vector<double> frame_ms;
        frame_ms.reserve(180);
        auto frame = [&] {
            const auto started = std::chrono::steady_clock::now();
            require(++frames < 180, "预览超过有界帧预算");
            require(overlay.pump_messages(true), "窗口消息处理失败");
            overlay.poll_background();
            require(overlay.render(runtime, {}, {}, {}, config, settings, workspace, "无设备调试 UI 预览", actions, nullptr, &debug), "Overlay 渲染失败");
            require(actions.debug_action == debug_session::Action::NONE && !actions.debug_allow_physical_output &&
                actions.debug_confirmation.empty() && !actions.start_requested && !actions.stop_requested &&
                !actions.training_start_requested && !actions.training_stop_requested && !actions.training_load_requested &&
                !actions.reload_detector_requested && !actions.save_config_requested && !actions.refresh_models_requested &&
                actions.runtime_intents.empty() && actions.workspace_action == model_workspace::Action::NONE &&
                !config.mouse.allow_send_input && (auxiliary || !overlay.background_busy()), "预览触发了业务动作或后台作业");
            frame_ms.push_back(std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now() - started).count());
        };
        frame(); frame(); frame();
        // 导航点击只进入本进程 ImGui 队列，不调用系统鼠标API。
        input.position = {70, 36.f + 12.f + 21.f + (auxiliary ? 3.f : 6.f) * (42.f + ImGui::GetStyle().ItemSpacing.y)};
        frame(); input.down = true; frame(); input.down = false; frame(); frame();
        auto* content = preview_window("content");
        if (auxiliary) {
            input.position = {400,40}; frame();
            require_page_table("auto_stop_form");
            require(capture.text.find("GSI武器：M4A1-S") != std::string::npos, "辅助页未显示统一GSI名称");
            require(capture.text.find("已归还移动 3 次") != std::string::npos, "辅助页未显示合成循环状态");
            save_window(capture, output / "auxiliary-top.png");
            auto* panel = preview_window("auto_stop_panel");
            // 向当前子窗口注入滚轮，验证与真实浏览相同的滚动路径。
            input.position = panel->ClipRect.GetCenter(); input.wheel = -5.f; frame(); frame();
            auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(panel->GetID("auto_stop_form"));
            require(table != nullptr, "辅助设置表缺失");
            require(panel->Scroll.y > 0, "辅助循环设置未滚动");
            save_window(capture, output / "auxiliary-cycle.png");
            const auto check_panel = [&](const char* name, const char* table_name,
                                         const char* expected, const char* file_name) {
                auto* card = preview_window(name);
                ImGui::ScrollToRectEx(content, card->Rect(), ImGuiScrollFlags_AlwaysCenterY);
                input.position = {400,40}; frame(); frame();
                card = preview_window(name);
                require_page_table(table_name);
                require(card->ClipRect.GetWidth() > 100 && card->ClipRect.GetHeight() > 100,
                    "辅助卡片未在滚动后显示有效正文");
                require(card->Rect().Min.x >= content->ClipRect.Min.x - 1 &&
                    card->Rect().Max.x <= content->ClipRect.Max.x + 1, "辅助卡片超出正文水平边界");
                require(capture.text.find(expected) != std::string::npos, "辅助卡片缺少预期文字");
                require(capture.text.find("连接与上下文设置") == std::string::npos &&
                    capture.text.find("额外许可键") == std::string::npos &&
                    capture.text.find("再次按下间隔") == std::string::npos &&
                    capture.text.find("曲线草稿强度") == std::string::npos,
                    "辅助卡片仍显示已移除的重复设置");
                save_window(capture, output / file_name);
            };
            check_panel("trigger_panel", "trigger_form", "触发范围 / %", "auxiliary-trigger.png");
            check_panel("recoil_panel", "recoil_settings", "配置缺项", "auxiliary-recoil.png");
            auto* recoil_card = preview_window("recoil_panel");
            ImGui::SetScrollY(recoil_card, recoil_card->ScrollMax.y); frame(); frame();
            require(capture.text.find("游戏版本") != std::string::npos &&
                capture.text.find("适用条件") != std::string::npos &&
                capture.text.find("游戏灵敏度") != std::string::npos,
                "主压枪页必须呈现可填写的校准匹配条件");
            save_window(capture, output / "auxiliary-recoil-settings.png");
            // 只进入本次变更的两个调试标签，不运行旧版完整导航流程。
            input.position = {70, 36.f + 12.f + 21.f + 6.f * (42.f + ImGui::GetStyle().ItemSpacing.y)};
            frame(); input.down = true; frame(); input.down = false; frame(); frame();
            content = preview_window("content");
            const auto select_debug_tab = [&](const char* label, const char* expected, const char* file_name) {
                ImGui::SetScrollY(content, 0); frame(); frame();
                auto* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(content->GetID("debug_tabs"));
                require(tabs && tabs->Tabs.Size == 6, "调试导航没有显示六个标签");
                const ImGuiTabItem* target = nullptr;
                for (auto& tab : tabs->Tabs)
                    if (std::string_view(ImGui::TabBarGetTabName(tabs, &tab)) == label) target = &tab;
                require(target != nullptr, "本次变更的调试标签缺失");
                const auto id = target->ID;
                input.position = {tabs->BarRect.Min.x + target->Offset + target->Width * .5f,
                                  tabs->BarRect.GetCenter().y};
                require(content->ClipRect.Contains(input.position), "调试标签在最小窗口不可点击");
                frame(); input.down = true; frame(); input.down = false; frame(); frame();
                require(tabs->SelectedTabId == id, "未进入预期调试标签");
                for (int i = 0; i < 30 && overlay.background_busy(); ++i) frame();
                require(!overlay.background_busy(), "只读资料预览未在帧预算内结束");
                input.position = {400,40}; frame();
                require(capture.text.find(expected) != std::string::npos, "新调试页未呈现预期内容");
                require(tabs->BarRect.Max.x <= ImGui::GetIO().DisplaySize.x, "调试标签栏超出窗口");
                save_window(capture, output / file_name);
            };
            select_debug_tab("扳机调试", "允许开枪", "trigger-debug.png");
            auto* trigger_tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(content->GetID("debug_tabs"));
            auto* trigger_table = ImGui::GetCurrentContext()->Tables.GetByKey(
                ImHashStr("trigger_debug_form", 0, trigger_tabs->SelectedTabId));
            require(trigger_table && trigger_table->LastFrameActive == ImGui::GetFrameCount(),
                "扳机调试表未在当前标签真实绘制");
            ImGui::SetScrollY(content, content->ScrollMax.y); frame(); frame();
            require(capture.text.find("图像有效期 / ms") != std::string::npos &&
                capture.text.find("使用估计完成联动") != std::string::npos,
                "扳机调试缺少有效性与联动控制");
            save_window(capture, output / "trigger-debug-bottom.png");
            select_debug_tab("弹道工具", "武器点射资料", "recoil-tools.png");
            ImGui::SetScrollY(content, content->ScrollMax.y); frame(); frame();
            require(capture.text.find("辅助 / 自动压枪") != std::string::npos,
                "弹道工具必须指向主压枪页的匹配配置");
            save_window(capture, output / "recoil-tools-bottom.png");
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),capture_hook_id);
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),frame_hook_id);
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),hook_id);
            overlay.shutdown(); Log::shutdown();
            std::ofstream result(output / "ui-preview-result.txt");
            result << "辅助页合成快照；Runtime实例=0；设备=0；业务动作=0；窗口=" << config.ui.width << 'x' << config.ui.height
                   << "；帧=" << frames << "\n已检查统一M4A1-S名称、循环状态、三张辅助卡片及两个调试页面的文案与水平边界；只读独立资料预览，不证明真实设备行为。\n";
            require(result.good(), "辅助预览结果写入失败");
            return 0;
        }
        auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(content->GetID("debug_tabs"));
        require(bar && bar->Tabs.Size == 6, "调试导航没有显示六个标签");
        require(bar->BarRect.Min.x >= 0 && bar->BarRect.Max.x <= ImGui::GetIO().DisplaySize.x,
            "最小窗口下标签栏超出右边界");
        const char* file_names[]{"counterpulse.png","manual.png","fire.png","trigger-debug.png","recoil.png","diagnostics.png"};
        const char* expected[]{"实验草稿独立于生产急停", "原生人工模型录制", "独立原地测试", "允许开枪", "弹道工具与射击归档", "请求"};
        for (int tab_index = 0; tab_index < 6; ++tab_index) {
            debug.plan["baseline"] = tab_index == 2 ? "stationary" : "counter";
            ImGui::SetScrollY(content,0); frame(); frame();
            bar = ImGui::GetCurrentContext()->TabBars.GetByKey(content->GetID("debug_tabs"));
            const auto tab = bar->Tabs[tab_index];
            if (tab_index == 4) {
                // 旧版全导航模式不需要资料读取；专项辅助模式另外覆盖只读编辑器。
                content->StateStorage.SetInt(ImHashStr("武器点射资料", 0, tab.ID), 0);
                content->StateStorage.SetInt(content->GetID("武器点射资料"), 0);
            }
            input.position = {bar->BarRect.Min.x + tab.Offset + tab.Width * .5f, bar->BarRect.GetCenter().y};
            require(content->ClipRect.Contains(input.position), "最小窗口中调试标签不可点击");
            input.down = false; frame(); input.down = true; frame(); input.down = false; frame(); frame();
            require(bar->SelectedTabId == tab.ID, "未进入目标调试标签");
            require(capture.text.find(expected[tab_index]) != std::string::npos, "调试页没有呈现预期内容");
            input.position = {400,40}; frame();
            save_window(capture,output / file_names[tab_index]);
            if (tab_index == 0 || tab_index == 2) {
                // 未准备的START控件禁用，不参与导航矩形更新；定位同一区域可用的准备控件。
                input.focus_window = content; input.focus_id = ImHashStr("准备",0,tab.ID); frame();
                const auto target = ImGui::WindowRectRelToAbs(content,content->NavRectRel[ImGuiNavLayer_Main]);
                ImGui::ScrollToRectEx(content,target,ImGuiScrollFlags_AlwaysCenterY); frame(); frame();
                input.focus_window = content; input.focus_id = ImHashStr("准备",0,tab.ID); frame();
                const auto visible = ImGui::WindowRectRelToAbs(content,content->NavRectRel[ImGuiNavLayer_Main]);
                require(content->ClipRect.Contains(visible.GetCenter()),"最小窗口滚动后准备控件仍不可见");
                save_window(capture,output / (std::string("controls-") + file_names[tab_index]));
            }
            if (tab_index == 0) {
                content->StateStorage.SetInt(content->GetID("默认基准与实际模型参数"),1);
                content->StateStorage.SetInt(content->GetID("default_baseline"),1);
                content->StateStorage.SetInt(content->GetID("actual_parameters"),1);
            }
            if (tab_index == 0 || tab_index == 2 || tab_index == 5) {
                ImGui::SetScrollY(content,content->ScrollMax.y); frame(); frame();
                require(capture.text.find("DOWN ACK至UP提交") != std::string::npos, "报告图缺少协议ACK至UP提交时间域");
                save_window(capture,output / (std::string("report-") + file_names[tab_index]));
            }
        }
        debug.busy = true; debug.state = debug_session::State::RUNNING;
        debug.live = analysis; debug.message = "合成后台繁忙：浏览不得提交动作";
        ImGui::SetScrollY(content,0); frame(); frame(); save_window(capture,output / "busy.png");
        // 关闭只给本窗口投递消息；deferred保持可绘制，兼容默认行为仍返回false。
        const auto window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        require(window && PostMessageW(window,WM_CLOSE,0,0), "无法向本预览窗口请求关闭");
        frame(); require(overlay.close_requested(), "关闭请求未锁存");
        save_window(capture,output / "closing.png");
        require(!overlay.pump_messages(), "默认消息泵未保留旧关闭语义");
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(),capture_hook_id);
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(),frame_hook_id);
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(),hook_id);
        // 进程峰值工作集包含截图缓存及编码；动态查现有Kernel32导出，不增加链接依赖。
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        using MemoryQuery = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        const auto memory_query = kernel ? reinterpret_cast<MemoryQuery>(GetProcAddress(kernel,"K32GetProcessMemoryInfo")) : nullptr;
        const bool memory_available = memory_query && memory_query(GetCurrentProcess(),&memory,sizeof(memory));
        auto sorted = frame_ms;
        std::sort(sorted.begin(),sorted.end());
        const auto percentile = [&](std::size_t percent) {
            const auto rank = (sorted.size() * percent + 99) / 100;
            return sorted.empty() ? 0.0 : sorted[std::max<std::size_t>(1,rank) - 1];
        };
        overlay.shutdown(); Log::shutdown();
        std::ofstream result(output / "ui-preview-result.txt");
        result << "合成快照；Runtime实例=0；设备=0；业务动作=0；窗口=" << config.ui.width << 'x' << config.ui.height
               << "；帧=" << frames << "\n六个调试标签及默认关闭/deferred关闭契约已检查。截图仍需人工视觉核对。\n";
        result << "UI线程每帧墙钟/ms：samples=" << frame_ms.size()
               << "；P95=" << percentile(95) << "；P99=" << percentile(99)
               << "；max=" << (sorted.empty() ? 0.0 : sorted.back()) << "；分位数=nearest-rank；含全部预览帧，不剔除启动帧。\n"
               << "测量范围：消息泵、后台poll、render及动作检查；包含Present等待与DX11截图回读，非CPU使用时间。\n";
        if (memory_available) result << "进程峰值工作集/bytes=" << memory.PeakWorkingSetSize << "（Win32近似RSS，包含截图缓存及编码）\n";
        else result << "进程峰值工作集=不可用（Win32查询失败，未以0代替）\n";
        result << "仅为当前版本合成浏览的软件测量；无旧版对照，无迟到ACK或报告生成压力，不构成性能改善或真实设备验收结论。\n";
        require(result.good(), "结果摘要写入失败");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "调试预览失败：" << error.what() << '\n'; Log::shutdown(); return 1;
    }
}
