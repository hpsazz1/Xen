#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
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

// 此程序不构造 Runtime、InputRouter、Keyboard、Mouse 或 Workspace。
// 仅使用合成快照渲染生产 Overlay，并保存本进程窗口；动作不被执行。
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "用法：model_workspace_ui_preview.exe <截图目录> [--minimum] [--dark]");
        const auto output = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(output);
        LogConfig logs;
        logs.enable_file = false;
        logs.enable_debug_file = false;
        Log::init(logs);
        Overlay overlay;
        AppConfig config;
        config.ui.width = 1000;
        config.ui.height = 820;
        config.ui.open_detached_preview_on_start = false;
        config.mouse.allow_send_input = false;
        config.mouse.backend = MouseBackend::KMBOX_NET;
        config.mouse.kmbox_ip = "127.0.0.1";
        config.mouse.kmbox_uuid = "00000000";
        config.trigger.require_stop = true;
        config.recoil.sensitivity = 1.4;
        bool target_only = false;
        for (int index = 2; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--minimum") {
                config.ui.width = kMinimumUiWidth; config.ui.height = kMinimumUiHeight;
            } else if (argument == L"--dark") config.ui.theme = UiTheme::DARK;
            else if (argument == L"--target-only") target_only = true;
            else throw std::runtime_error("未知预览参数");
        }
        const auto fixture_directory = std::filesystem::temp_directory_path() /
            ("xen-ui-preview-" + std::to_string(GetCurrentProcessId()) + '-' +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(fixture_directory), "独立UI夹具目录创建失败");
        RecoilProfile fixture;
        fixture.id = "ui_only_candidate"; fixture.weapon_id = "ui_only_weapon";
        fixture.state = RecoilProfileState::SCHEMA_VALID;
        fixture.points = {{0,0,0},{20,0,0},{60,1,4},{100,-1,8},{160,2,12}};
        const auto fixture_text = serialize_recoil_profile(fixture);
        const auto fixture_path = fixture_directory / "ui-only-candidate.json";
        { std::ofstream file(fixture_path, std::ios::binary); file << fixture_text; require(file.good(), "UI夹具写入失败"); }
        const auto directory_utf8 = fixture_directory.u8string();
        config.recoil.profile_directory.assign(reinterpret_cast<const char*>(directory_utf8.data()), directory_utf8.size());
        require(overlay.init(config.ui), "Overlay 初始化失败");
        RuntimeSnapshot runtime;
        runtime.state = RuntimeState::STOPPED;
        model_workspace::Settings settings;
        settings.root_directory = "E:/示例素材/person-dataset";
        settings.python_executable = "C:/Python/python.exe";
        settings.base_python_executable = "C:/Python/python.exe";
        settings.environment_root = "E:/示例训练环境";
        settings.trusted_weights = false;
        settings.script_path = "E:/Xen/tools/model-data/model_data_pipeline.py";
        settings.class_names = "class_0,class_1,class_2,class_3";
        settings.weights_path = "E:/示例模型/teacher.pt";
        settings.model_path = "E:/示例模型/teacher.onnx";
        model_workspace::Snapshot workspace;
        workspace.message = "无设备窗口验收：仅使用合成快照，所有动作均不执行。";
        workspace.job_state = "NOT_STARTED";
        workspace.environment_ready = false;
        workspace.weights_ready = false;
        workspace.environment_message = "示例状态：尚未检查环境，未安装任何依赖。";
        workspace.weights_message = "示例状态：尚未确认权重来源，未读取 PT。";
        OverlayActions actions;
        KeyboardPollResult capture_input;
        capture_input.capture_state_valid = true;
        UiInput input;
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePre;
        hook.Callback = inject_input;
        hook.UserData = &input;
        const auto hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
        ImGuiContextHook frame_hook;
        frame_hook.Type = ImGuiContextHookType_NewFramePost;
        frame_hook.Callback = begin_preview_frame;
        frame_hook.UserData = &input;
        const auto frame_hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &frame_hook);
        RenderCapture capture;
        CaptureFailure failure{capture, output};
        ImGuiContextHook capture_hook;
        capture_hook.Type = ImGuiContextHookType_EndFramePre;
        capture_hook.Callback = append_capture;
        capture_hook.UserData = &capture;
        const auto capture_hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &capture_hook);
        // 防止后端光标采样与模拟事件被分散到后续帧；不会设置系统光标。
        ImGui::GetIO().ConfigInputTrickleEventQueue = false;
        auto frame = [&] {
            require(overlay.pump_messages(), "窗口意外关闭");
            require(overlay.render(runtime, {}, {}, {}, config, settings, workspace,
                                   "无设备 UI 验收", actions, &capture_input), "Overlay 渲染失败");
            require(!actions.start_requested && actions.runtime_intents.empty() &&
                    !actions.stop_requested && !actions.reload_detector_requested && !actions.refresh_models_requested &&
                    !actions.save_config_requested && !actions.log_level_changed && !actions.preview_enabled &&
                    actions.workspace_action == model_workspace::Action::NONE && !config.mouse.allow_send_input &&
                    !config.auto_stop.enabled && !config.trigger.enabled && !config.recoil.enabled,
                    "验收输入误触业务动作，停止执行");
        };
        for (int index = 0; index < 3; ++index) frame();
        auto select_page = [&](int index) {
            // 生产导航：标题 36，sidebar padding 12，每项高42＋ItemSpacing.y。
            input.position = ImVec2(70.0f, 36.0f + 12.0f + 21.0f +
                static_cast<float>(index) * (42.0f + ImGui::GetStyle().ItemSpacing.y));
            input.down = false; frame();
            input.down = true; frame();
            input.down = false; frame();
            input.position = {400,40}; frame(); frame();
        };
        if (target_only) {
            select_page(6);
            auto* content=preview_window("content");
            const auto id=ImHashStr("弹道工具",0,content->GetID("debug_tabs"));
            input.focus_window=content; input.focus_id=id; frame();
            require(ImGui::GetCurrentContext()->NavId==id && ImGui::GetCurrentContext()->NavIdIsAlive,
                "未找到生产弹道工具页签");
            auto rect=ImGui::WindowRectRelToAbs(content,content->NavRectRel[ImGuiNavLayer_Main]);
            input.position=rect.GetCenter(); frame(); frame();
            input.down=true; frame(); input.down=false; frame(); frame();
            require(capture.text.find("固定目标迭代")!=std::string::npos,"固定目标实验未成为默认弹道工具");
            require(capture.text.find("RGB 255/0/255")==std::string::npos,"默认页面仍要求已放弃的洋红准星");
            require(actions.debug_action==debug_session::Action::NONE,"只浏览目标面板不得准备或启动实验");
            save_window(capture,output/"target-top.png");
            ImGui::SetScrollY(content,content->ScrollMax.y*0.5f);frame();frame();
            save_window(capture,output/"target-middle.png");
            ImGui::SetScrollY(content,content->ScrollMax.y);frame();frame();
            save_window(capture,output/"target-bottom.png");
            const auto analysis_id=ImHashStr("固定目标：审核与多轮分析",0,id);
            input.focus_window=content;input.focus_id=analysis_id;frame();
            require(ImGui::GetCurrentContext()->NavId==analysis_id && ImGui::GetCurrentContext()->NavIdIsAlive,
                "未找到固定目标离线分析入口");
            rect=ImGui::WindowRectRelToAbs(content,content->NavRectRel[ImGuiNavLayer_Main]);
            ImGui::ScrollToRect(content,rect,ImGuiScrollFlags_AlwaysCenterY);frame();frame();
            rect=ImGui::WindowRectRelToAbs(content,content->NavRectRel[ImGuiNavLayer_Main]);
            input.position=rect.GetCenter();frame();frame();
            input.down=true;frame();input.down=false;frame();frame();
            ImGui::SetScrollY(content,content->ScrollMax.y);frame();frame();
            require(capture.text.find("合并五轮残差分析")!=std::string::npos,"离线分析表单未展开");
            require(actions.debug_action==debug_session::Action::NONE,"浏览离线表单不得触发实验");
            input.position={400,40};frame();frame();
            save_window(capture,output/"target-analysis.png");
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),capture_hook_id);
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),frame_hook_id);
            ImGui::RemoveContextHook(ImGui::GetCurrentContext(),hook_id);
            overlay.shutdown();Log::shutdown();
            std::cout<<"固定目标默认界面已截图；业务动作0，设备输出0\n";
            return 0;
        }
        select_page(4);
        require_page_table("collection_settings");
        save_window(capture, output / "collection.png");
        select_page(5);
        require_page_table("training_environment");
        save_window(capture, output / "training-top.png");
        input.position = ImVec2(static_cast<float>(config.ui.width - 150),
            static_cast<float>(config.ui.height - 100));
        input.wheel = -20.0f;
        frame(); frame(); frame();
        save_window(capture, output / "training-bottom.png");
        select_page(3);
        auto* content = preview_window("content");
        ImGui::SetScrollY(content, 0); frame(); frame();
        require_page_table("auto_stop_form");
        save_window(capture, output / "auxiliary-top.png");
        // 运行中阻断仍仅为合成快照，不构造 Runtime 或执行任何输出意图。
        const auto original_runtime = runtime;
        runtime.state = RuntimeState::RUNNING;
        runtime.auto_stop.status = AutoStopStatus::READY;
        runtime.auto_stop.telemetry_available = true;
        runtime.auto_stop.independent_trigger_enabled = true;
        runtime.auto_stop.block_reason = AutoStopBlockReason::SOURCE_TIMING_INVALID;
        runtime.auto_stop.source_focused = true;
        runtime.auto_stop.target_available = false;
        auto* stop_panel = preview_window("auto_stop_panel");
        ImGui::SetScrollY(content, 0);
        ImGui::SetScrollY(stop_panel, 0);
        frame(); frame();
        require(capture.text.find("阻断原因：源帧时钟映射无效") != std::string::npos &&
                capture.text.find("源机时钟服务不可用或映射尚未建立") != std::string::npos,
                "运行中急停阻断及排查提示必须进入生产界面");
        require(stop_panel->Scroll.y == 0 && content->Scroll.y == 0,
                "急停阻断首屏证据不得滚动后截取");
        save_window(capture, output / "auxiliary-blocked.png");
        runtime = original_runtime;
        frame(); frame();
        auto focus_item = [&](const char* label, ImGuiWindow* window, const char* table = nullptr) {
            const auto id = table ? ImHashStr(label, 0, window->GetID(table)) : window->GetID(label);
            input.down = false; input.focus_window = window; input.focus_id = id; frame();
            auto* context = ImGui::GetCurrentContext();
            require(context->NavId == id && context->NavIdIsAlive,
                (std::string("未找到当帧生产控件：") + label).c_str());
            auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
            ImGui::ScrollToRect(window, rect, ImGuiScrollFlags_AlwaysCenterY);
            frame(); frame();
            require(context->NavId == id && context->NavIdIsAlive, "滚动后生产控件身份失效");
            rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
            if (!(rect.GetWidth() > 0 && rect.GetHeight() > 0 && window->ClipRect.Contains(rect.GetCenter()))) {
                std::ostringstream detail;
                detail << "导航目标未处于可见内容区域：" << label << " rect=("
                       << rect.Min.x << ',' << rect.Min.y << ',' << rect.Max.x << ',' << rect.Max.y
                       << ") clip=(" << window->ClipRect.Min.x << ',' << window->ClipRect.Min.y << ','
                       << window->ClipRect.Max.x << ',' << window->ClipRect.Max.y << ")";
                throw std::runtime_error(detail.str());
            }
            input.position = rect.GetCenter(); frame(); frame();
            return rect;
        };
        auto activate_view_item = [&](const char* label) {
            const std::string_view name(label);
            require(name == "高级：曲线编辑与数据集优化" || name == "加载文件" || name == "独立弹道自动优化器",
                "预览只允许展开界面和读取临时曲线");
            focus_item(label, content);
            input.down = true; frame(); input.down = false; frame(); frame();
        };
        // 只注入后端快照与本进程点击，不调用Windows或真实设备输入。
        for (int key : {5, 6}) {
            focus_item("##auto_stop_activation_key", preview_window("auto_stop_panel"), "auto_stop_form");
            input.down = true; frame(); input.down = false; frame();
            capture_input.capture_virtual_keys[key] = true; frame();
            require(config.auto_stop.activation_virtual_key == key,
                "真实Overlay捕获按钮必须接收后端侧键快照");
            capture_input.capture_virtual_keys[key] = false; frame();
        }
        const auto default_release_keys = config.auto_stop.release_virtual_keys;
        for (int key : {'6', 'E'}) {
            focus_item("##auto_stop_release_keys", preview_window("auto_stop_panel"), "auto_stop_form");
            input.down = true; frame(); input.down = false; frame();
            capture_input.capture_virtual_keys[key] = true; frame();
            require(std::find(config.auto_stop.release_virtual_keys.begin(),
                    config.auto_stop.release_virtual_keys.end(), key) != config.auto_stop.release_virtual_keys.end(),
                "真实Overlay必须支持追加多个急停释放键");
            capture_input.capture_virtual_keys[key] = false; frame();
        }
        require(config.auto_stop.release_virtual_keys.size() == default_release_keys.size() + 2,
            "追加释放键不能覆盖之前的绑定");
        config.auto_stop.release_virtual_keys = default_release_keys;
        ImGui::SetScrollY(content, 0); frame(); frame();
        save_window(capture, output / "auxiliary-side-button.png");
        focus_item("暂停本次会话", preview_window("auto_stop_panel"));
        require_tooltip(capture, "预计完成不是观察停稳");
        save_window(capture, output / "auxiliary-stop-help.png");
        auto* trigger_panel = preview_window("trigger_panel");
        ImGui::ScrollToRect(content, trigger_panel->Rect(), ImGuiScrollFlags_AlwaysCenterY);
        frame(); frame();
        require_page_table("trigger_form");
        save_window(capture, output / "auxiliary-trigger.png");
        ImGui::SetScrollY(trigger_panel, trigger_panel->ScrollMax.y); frame(); frame();
        save_window(capture, output / "auxiliary-trigger-bottom.png");
        // 弹道配置与编辑统一在调试页，已移除固定版本覆盖入口。
        select_page(6);
        content = preview_window("content");
        ImGui::SetScrollY(content, 0); frame(); frame();
        focus_item("弹道工具", content, "debug_tabs");
        input.down = true; frame(); input.down = false; frame(); frame();
        focus_item("高级：曲线编辑与数据集优化", content);
        require_tooltip(capture, "不会自动加载、激活或执行曲线");
        save_window(capture, output / "recoil-editor-help.png");
        activate_view_item("高级：曲线编辑与数据集优化");
        require(capture.text.find("先加载已有曲线") != std::string::npos, "编辑器没有按要求展开");
        focus_item("文件名", content);
        input.down = true; frame(); input.down = false; frame();
        ImGui::GetIO().AddInputCharactersUTF8("ui-only-candidate.json"); frame();
        activate_view_item("加载文件");
        focus_item("还原草稿", content);
        require_page_table("recoil_tuning");
        require(capture.text.find("ui_only_candidate") != std::string::npos, "临时曲线未载入生产编辑预览");
        require_tooltip(capture, "将内存草稿恢复到本次加载的基线");
        save_window(capture, output / "recoil-editor.png");
        focus_item("撤销", content);
        require_tooltip(capture, "没有可撤销步骤时禁用");
        save_window(capture, output / "recoil-undo-help.png");
        focus_item("另存版本号", content);
        input.position = {400,40}; ImGui::SetScrollY(content, std::max(0.0f, content->Scroll.y - 100)); frame(); frame();
        require(capture.text.find("X / counts") != std::string::npos && capture.text.find("Y / counts") != std::string::npos,
            "编辑预览缺少双轴曲线说明");
        save_window(capture, output / "recoil-curves.png");
        activate_view_item("独立弹道自动优化器");
        focus_item("分析并生成独立候选", content);
        require(ImGui::GetCurrentContext()->NavIdItemFlags & ImGuiItemFlags_Disabled, "未导入数据时优化按钮应禁用");
        require_tooltip(capture, "通过仍不代表物理验收");
        save_window(capture, output / "recoil-optimizer-help.png");
        std::size_t fixture_files = 0;
        for (const auto& file : std::filesystem::directory_iterator(fixture_directory)) {
            require(file.path() == fixture_path, "UI预览创建了非夹具文件或活动索引"); ++fixture_files;
        }
        require(fixture_files == 1, "UI预览改变了夹具目录文件集合");
        { std::ifstream file(fixture_path, std::ios::binary); const std::string after{
              std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
          require(after == fixture_text, "UI预览修改了磁盘曲线"); }
        select_page(7);
        content = preview_window("content");
        ImGui::SetScrollY(content, 0); frame(); frame();
        require_page_table("mouse_form");
        require_page_table("keyboard_form");
        save_window(capture, output / "settings-input.png");
        ImGui::SetScrollY(content, content->ScrollMax.y); frame(); frame();
        save_window(capture, output / "settings-preferences.png");
        std::ofstream result(output / "ui-preview-result.txt", std::ios::binary);
        result << "状态：STOPPED；业务动作：0；真实输入：0\n"
               << "窗口：" << config.ui.width << 'x' << config.ui.height
               << "；主题：" << (config.ui.theme == UiTheme::DARK ? "深色" : "浅色") << '\n'
               << "临时曲线：" << reinterpret_cast<const char*>(fixture_path.u8string().c_str()) << '\n'
               << "曲线字节及目录集合保持不变；未创建配置、活动索引或 Prepare 产物。\n";
        require(result.good(), "UI预览结果写入失败");
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), capture_hook_id);
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), frame_hook_id);
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), hook_id);
        overlay.shutdown();
        Log::shutdown();
        std::cout << "无设备 UI 窗口截图已保存；仍需人工检查布局及文字。\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "窗口验收失败：" << error.what() << '\n';
        Log::shutdown();
        return 1;
    }
}
