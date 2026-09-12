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

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct UiInput {
    ImVec2 position{70.0f, 65.0f};
    bool down = false;
    float wheel = 0.0f;
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
    int frame = -1;
};

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

void append_capture(ImGuiContext*, ImGuiContextHook* hook) {
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
// 仅使用 STOPPED 快照渲染生产 Overlay，并保存本进程窗口；动作不被执行。
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "用法：model_workspace_ui_preview.exe <截图目录>");
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
        require(overlay.init(config.ui), "Overlay 初始化失败");
        RuntimeSnapshot runtime;
        runtime.state = RuntimeState::STOPPED;
        model_workspace::Settings settings;
        settings.root_directory = "E:/示例素材/person-dataset";
        settings.python_executable = "C:/Python/python.exe";
        settings.script_path = "E:/Xen/scripts/model_data_pipeline.py";
        settings.class_names = "class_0,class_1,class_2,class_3";
        settings.weights_path = "E:/示例模型/teacher.pt";
        settings.model_path = "E:/示例模型/teacher.onnx";
        model_workspace::Snapshot workspace;
        workspace.message = "无设备窗口验收：仅使用停止快照，所有动作均不执行。";
        workspace.job_state = "NOT_STARTED";
        OverlayActions actions;
        UiInput input;
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePre;
        hook.Callback = inject_input;
        hook.UserData = &input;
        const auto hook_id = ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
        RenderCapture capture;
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
                                   "无设备 UI 验收", actions), "Overlay 渲染失败");
            require(!actions.start_requested && actions.runtime_intents.empty() &&
                    actions.workspace_action == model_workspace::Action::NONE,
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
            frame();
        };
        select_page(2);
        require_page_table("collection_settings");
        save_window(capture, output / "collection.png");
        select_page(3);
        require_page_table("training_tools");
        save_window(capture, output / "training-top.png");
        input.position = ImVec2(700.0f, 650.0f);
        input.wheel = -20.0f;
        frame(); frame(); frame();
        save_window(capture, output / "training-bottom.png");
        ImGui::RemoveContextHook(ImGui::GetCurrentContext(), capture_hook_id);
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
