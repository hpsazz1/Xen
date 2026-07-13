#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <string.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <vector>

#include <imgui/imgui.h>
#include "imgui/imgui_internal.h"

#include "config.h"
#include "Xen.h"
#include "capture.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include "ndi_capture.h"
#include "draw_settings.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

// 标记WinRT异步未来功能是否可用（基于Windows版本检查）
bool disable_winrt_futures = checkwin1903();
// 当前活动的显示器数量
int monitors = get_active_monitors();

// 可用虚拟摄像头列表缓存
static std::vector<std::string> virtual_cameras;
// 可捕获窗口列表缓存
static std::vector<CaptureWindowInfo> capture_windows;
// 虚拟摄像头筛选输入缓冲区
static char virtual_camera_filter_buf[128] = "";
// 窗口筛选输入缓冲区
static char capture_window_filter_buf[128] = "";
// UDP IP地址输入缓冲区
static char udp_ip_buf[64] = "";
// UDP端口号输入缓冲区
static int udp_port_buf = 2333;
// NDI源列表缓存
static std::vector<std::string> ndi_sources;
// NDI源筛选输入缓冲区
static char ndi_source_filter_buf[128] = "";
// 标记NDI源列表是否已加载
static bool ndi_sources_loaded = false;
// NDI 首轮 mDNS 发现可能需要约 1 秒，必须放到后台任务，避免阻塞 ImGui 渲染和捕获预览。
static bool ndi_source_refreshing = false;
static std::future<std::vector<std::string>> ndi_source_refresh_future;
// 标记窗口列表是否已加载
static bool capture_windows_loaded = false;
// 标记UDP设置是否已初始化
static bool udp_settings_init = false;

// 请求重启WinRT捕获（标记捕获方法和窗口已更改，由后台线程重新初始化）
static void requestWinrtCaptureRestart()
{
    capture_method_changed.store(true);
    capture_window_changed.store(true);
}

// 刷新可捕获窗口列表，调用系统API枚举当前所有窗口
static void refreshCaptureWindowList()
{
    capture_windows = EnumerateCaptureWindows();
    capture_windows_loaded = true;
}

// 启动一次 NDI 源发现。正在刷新时忽略重复点击，防止并发创建多个 finder 和重复占用网络。
static void startNdiSourceRefresh()
{
    if (ndi_source_refreshing)
        return;

    ndi_source_refreshing = true;
    ndi_source_refresh_future = std::async(std::launch::async, []()
    {
        return NDICapture::GetAvailableSources(1500);
    });
}

// 每个 UI 帧非阻塞检查发现结果；空结果也标记为已完成，避免旧逻辑每帧重启发现。
static void pollNdiSourceRefresh()
{
    if (!ndi_source_refreshing || !ndi_source_refresh_future.valid() ||
        ndi_source_refresh_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;

    try
    {
        ndi_sources = ndi_source_refresh_future.get();
    }
    catch (const std::exception& e)
    {
        ndi_sources.clear();
        std::cerr << "[NDI] Source refresh failed: " << e.what() << std::endl;
    }
    ndi_sources_loaded = true;
    ndi_source_refreshing = false;
}

// 判断指定窗口是否匹配给定的标题字符串（支持精确匹配、子串匹配和不区分大小写匹配）
static bool captureWindowMatchesTitle(const CaptureWindowInfo& window, const std::string& title)
{
    const std::string needle = OtherTools::TrimAscii(title);
    if (needle.empty())
        return false;

    return window.title == needle ||
        window.title.find(needle) != std::string::npos ||
        OtherTools::ContainsCaseInsensitive(window.title, needle);
}

// 检查当前配置的捕获窗口标题是否存在于已枚举的窗口列表中
static bool currentWindowTitleIsInList()
{
    for (const auto& window : capture_windows)
        if (captureWindowMatchesTitle(window, config.capture_window_title))
            return true;
    return false;
}

// 应用选中的窗口标题作为WinRT捕获目标，更新配置并重启捕获
static void applyWinrtWindowTarget(const std::string& title)
{
    if (config.capture_window_title != title)
    {
        config.capture_window_title = title;
        OverlayConfig_MarkDirty();
    }

    requestWinrtCaptureRestart();
}

// 确保虚拟摄像头列表已加载，供虚拟摄像头捕获模式使用
void ensureVirtualCamerasLoaded()
{
    if (virtual_cameras.empty())
    {
        virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras();
    }
}

// ============================================================
// 主函数：绘制捕获设置UI
// 包含以下子章节：通用捕获、WinRT、显示器捕获、虚拟摄像头、UDP捕获、NDI
// ============================================================
void draw_capture_settings()
{
    // 允许的检测分辨率列表（越小性能越好）
    static const int allowed_resolutions[] = { 160, 320, 640 };
    // 当前选中的分辨率索引（默认320）
    static int current_resolution_idx = 1;

    // 根据配置同步当前分辨率索引
    for (int i = 0; i < 3; ++i)
        if (config.detection_resolution == allowed_resolutions[i])
            current_resolution_idx = i;

    // ========== 通用捕获设置 ==========
    if (OverlayUI::BeginSection("采集参数", "capture_section_general"))
    {
        // 检测分辨率下拉框
        {
            const auto row = OverlayUI::BeginSettingRow("检测分辨率");
            if (ImGui::Combo("##value", &current_resolution_idx, "160\0""320\0""640\0"))
            {
                config.detection_resolution = allowed_resolutions[current_resolution_idx];
                detection_resolution_changed.store(true);
                detector_model_changed.store(true);

                globalMouseThread->updateConfig(
                    config.detection_resolution,
                    config.fovX,
                    config.fovY,
                    config.auto_shoot,
                    config.bScope_multiplier);
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 捕获帧率滑块（0表示不限帧率，范围0-360）
        {
            const auto row = OverlayUI::BeginSettingRow("捕获帧率");
            if (ImGui::SliderInt("##value", &config.capture_fps, 0, 360))
            {
                capture_fps_changed.store(true);
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 当FPS为0时提示已禁用帧率上限
        if (config.capture_fps == 0)
        {
            OverlayUI::TextRow("捕获帧率上限已禁用。", IM_COL32(255, 188, 108, 255));
        }
        // 当FPS≥61时警告高帧率可能降低性能
        else if (config.capture_fps >= 61)
        {
            OverlayUI::TextRow("警告：高帧率可能降低性能。");
        }

        // 圆形视野（FOV）开关
        {
            const auto row = OverlayUI::BeginSettingRow("圆形视野");
            if (ImGui::Checkbox("##value", &config.circle_fov_enabled))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 圆形视野大小百分比（仅开启时显示）
        if (config.circle_fov_enabled)
        {
            const auto row = OverlayUI::BeginSettingRow("圆形视野大小");
            if (ImGui::SliderInt("##value", &config.circle_fov_radius_percent, 1, 100, "%d%%"))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

#ifdef USE_CUDA
        // CUDA直接捕获仅在使用duplication_api时可用
        if (config.backend == "TRT")
        {
            const bool cudaCaptureAvailable = (config.capture_method == "duplication_api");
            if (!cudaCaptureAvailable)
            {
                ImGui::BeginDisabled();
            }

            {
                const auto row = OverlayUI::BeginSettingRow("使用CUDA直接捕获");
                if (ImGui::Checkbox("##value", &config.capture_use_cuda))
                {
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            if (!cudaCaptureAvailable)
            {
                ImGui::EndDisabled();
                OverlayUI::TextRow("仅支持 duplication_api 方式。", IM_COL32(188, 188, 188, 255));
            }
        }
#endif

        // 可选的捕获方式列表
        std::vector<std::string> captureMethodOptions = { "duplication_api", "winrt", "virtual_camera", "udp_capture", "ndi" };
        std::vector<const char*> captureMethodItems;

        for (const auto& option : captureMethodOptions)
        {
            captureMethodItems.push_back(option.c_str());
        }

        // 查找当前配置对应的捕获方式索引
        int currentcaptureMethodIndex = 0;
        for (size_t i = 0; i < captureMethodOptions.size(); ++i)
        {
            if (captureMethodOptions[i] == config.capture_method)
            {
                currentcaptureMethodIndex = static_cast<int>(i);
                break;
            }
        }

        // 捕获方式选择下拉框
        {
            const auto row = OverlayUI::BeginSettingRow("捕获方式");
            if (ImGui::Combo("##value", &currentcaptureMethodIndex, captureMethodItems.data(), static_cast<int>(captureMethodItems.size())))
            {
                config.capture_method = captureMethodOptions[currentcaptureMethodIndex];
                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }
            OverlayUI::EndSettingRow(row);
        }

        OverlayUI::EndSection();
    }

    draw_capture_preview();

    // ========== WinRT捕获设置 ==========
    if (config.capture_method == "winrt")
    {
        if (OverlayUI::BeginSection("WinRT", "capture_section_winrt"))
        {
            // WinRT捕获目标选择：显示器或窗口
            {
                std::vector<std::string> targetOptions = { "monitor", "window" };
                int currentTargetIndex = (config.capture_target == "window") ? 1 : 0;
                {
                    const auto row = OverlayUI::BeginSettingRow("捕获目标(WinRT)");
                    if (ImGui::Combo("##value", &currentTargetIndex,
                        [](void* data, int idx) -> const char* {
                            const auto* v = static_cast<const std::vector<std::string>*>(data);
                            if (idx < 0 || idx >= (int)v->size()) return nullptr;
                            return v->at(idx).c_str();
                        }, (void*)&targetOptions, (int)targetOptions.size()))
                    {
                        config.capture_target = targetOptions[currentTargetIndex];
                        OverlayConfig_MarkDirty();
                        requestWinrtCaptureRestart();
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }

            // 窗口模式：显示窗口筛选和选择UI
            if (config.capture_target == "window")
            {
                if (!capture_windows_loaded)
                    refreshCaptureWindowList();

                {
                    const auto row = OverlayUI::BeginSettingRow("窗口筛选");
                    ImGui::InputText("##value", capture_window_filter_buf, IM_ARRAYSIZE(capture_window_filter_buf));
                    OverlayUI::EndSettingRow(row);
                }

                const std::string filterLower = OtherTools::ToLowerAscii(capture_window_filter_buf);
                std::vector<int> filteredWindowIndices;
                filteredWindowIndices.reserve(capture_windows.size());
                for (int i = 0; i < static_cast<int>(capture_windows.size()); ++i)
                {
                    const std::string displayLower = OtherTools::ToLowerAscii(capture_windows[i].displayName);
                    if (filterLower.empty() || displayLower.find(filterLower) != std::string::npos)
                        filteredWindowIndices.push_back(i);
                }

                if (!filteredWindowIndices.empty())
                {
                    const std::string preview = config.capture_window_title.empty()
                        ? std::string("选择窗口")
                        : config.capture_window_title;

                    const auto row = OverlayUI::BeginSettingRow("窗口");
                    if (ImGui::BeginCombo("##value", preview.c_str()))
                    {
                        for (int index : filteredWindowIndices)
                        {
                            const CaptureWindowInfo& window = capture_windows[index];
                            const bool selected = captureWindowMatchesTitle(window, config.capture_window_title);

                            ImGui::PushID(window.hwnd);
                            if (ImGui::Selectable(window.displayName.c_str(), selected))
                            {
                                applyWinrtWindowTarget(window.title);
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    OverlayUI::EndSettingRow(row);
                }
                else
                {
                    OverlayUI::TextRow("无匹配窗口", IM_COL32(188, 188, 188, 255));
                }

                {
                    const auto row = OverlayUI::BeginSettingRow("窗口列表");
                    if (ImGui::Button("刷新", ImVec2(row.controlWidth, 0.0f)))
                    {
                        refreshCaptureWindowList();
                        if (currentWindowTitleIsInList())
                            requestWinrtCaptureRestart();
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }

            // WinRT未来功能是否被禁用（Windows版本过低时禁用边框和光标捕获）
            if (disable_winrt_futures)
            {
                ImGui::BeginDisabled();
            }

            // WinRT捕获时是否包含窗口边框
            {
                const auto row = OverlayUI::BeginSettingRow("捕获边框");
                if (ImGui::Checkbox("##value", &config.capture_borders))
                {
                    capture_borders_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            // WinRT捕获时是否包含鼠标光标
            {
                const auto row = OverlayUI::BeginSettingRow("捕获光标");
                if (ImGui::Checkbox("##value", &config.capture_cursor))
                {
                    capture_cursor_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            if (disable_winrt_futures)
            {
                ImGui::EndDisabled();
            }

            OverlayUI::EndSection();
        }
    }

    // ========== 显示器捕获设置（duplication_api或WinRT显示器模式） ==========
    if (config.capture_method == "duplication_api" || (config.capture_method == "winrt" && config.capture_target != "window"))
    {
        if (OverlayUI::BeginSection("显示器捕获", "capture_section_monitor"))
        {
            // 构建显示器名称列表
            std::vector<std::string> monitorNames;
            int monitorCount = monitors;
            // 未检测到显示器时显示默认名称
            if (monitorCount <= 0)
            {
                monitorNames.push_back("显示器 1");
                monitorCount = 1;
            }
            else
            {
                for (int i = 0; i < monitorCount; ++i)
                {
                    monitorNames.push_back("显示器 " + std::to_string(i + 1));
                }
            }

            std::vector<const char*> monitorItems;
            for (const auto& name : monitorNames)
            {
                monitorItems.push_back(name.c_str());
            }

            // 目标显示器选择（限制在有效范围内）
            int selectedMonitor = std::clamp(config.monitor_idx, 0, monitorCount - 1);
            {
                const auto row = OverlayUI::BeginSettingRow("捕获显示器");
                if (ImGui::Combo("##value", &selectedMonitor, monitorItems.data(), static_cast<int>(monitorItems.size())))
                {
                    config.monitor_idx = selectedMonitor;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }

    // ========== 虚拟摄像头捕获设置 ==========
    if (config.capture_method == "virtual_camera")
    {
        if (OverlayUI::BeginSection("虚拟摄像头", "capture_section_virtual_camera"))
        {
            // 确保虚拟摄像头列表已加载
            ensureVirtualCamerasLoaded();

            // 摄像头名称筛选输入框
            {
                const auto row = OverlayUI::BeginSettingRow("筛选");
                ImGui::InputText("##value", virtual_camera_filter_buf, IM_ARRAYSIZE(virtual_camera_filter_buf));
                OverlayUI::EndSettingRow(row);
            }

            // 根据筛选文本过滤摄像头列表（不区分大小写）
            std::string filter_lower = OtherTools::ToLowerAscii(virtual_camera_filter_buf);

            std::vector<int> filtered_indices;
            for (int i = 0; i < static_cast<int>(virtual_cameras.size()); ++i)
            {
                std::string name_lower = OtherTools::ToLowerAscii(virtual_cameras[i]);
                if (filter_lower.empty() || name_lower.find(filter_lower) != std::string::npos)
                {
                    filtered_indices.push_back(i);
                }
            }

            // 有匹配结果时显示选择下拉框
            if (!filtered_indices.empty())
            {
                // 查找当前配置的虚拟摄像头在筛选结果中的索引
                int currentIndex = 0;
                for (int fi = 0; fi < static_cast<int>(filtered_indices.size()); ++fi)
                {
                    if (virtual_cameras[filtered_indices[fi]] == config.virtual_camera_name)
                    {
                        currentIndex = fi;
                        break;
                    }
                }

                std::vector<const char*> items;
                items.reserve(filtered_indices.size());
                for (int idx : filtered_indices)
                {
                    items.push_back(virtual_cameras[idx].c_str());
                }

                // 虚拟摄像头选择下拉框
                {
                    const auto row = OverlayUI::BeginSettingRow("虚拟摄像头");
                    if (ImGui::Combo("##value", &currentIndex, items.data(), static_cast<int>(items.size())))
                    {
                        config.virtual_camera_name = virtual_cameras[filtered_indices[currentIndex]];
                        OverlayConfig_MarkDirty();
                        capture_method_changed.store(true);
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }
            else
            {
                OverlayUI::TextRow("无匹配虚拟摄像头", IM_COL32(188, 188, 188, 255));
            }

            // 刷新摄像头列表按钮
            {
                const auto row = OverlayUI::BeginSettingRow("摄像头列表");
                if (ImGui::Button("刷新", ImVec2(row.controlWidth, 0.0f)))
                {
                    VirtualCameraCapture::ClearCachedCameraList();
                    virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras(true);
                    virtual_camera_filter_buf[0] = '\0';
                }
                OverlayUI::EndSettingRow(row);
            }

            // 虚拟摄像头输出宽度
            {
                const auto row = OverlayUI::BeginSettingRow("虚拟摄像头宽度");
                if (ImGui::SliderInt("##value", &config.virtual_camera_width, 128, 3840))
                {
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            // 虚拟摄像头输出高度
            {
                const auto row = OverlayUI::BeginSettingRow("虚拟摄像头高度");
                if (ImGui::SliderInt("##value", &config.virtual_camera_height, 128, 2160))
                {
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }

    // ========== UDP网络捕获设置 ==========
    if (config.capture_method == "udp_capture")
    {
        if (OverlayUI::BeginSection("UDP网络捕获", "capture_section_udp"))
        {
            // 首次打开时将配置值同步到本地缓冲区
            if (!udp_settings_init)
            {
                memset(udp_ip_buf, 0, sizeof(udp_ip_buf));
                std::string ip = config.udp_ip;
                if (ip.size() >= sizeof(udp_ip_buf))
                    ip = ip.substr(0, sizeof(udp_ip_buf) - 1);
                memcpy(udp_ip_buf, ip.c_str(), ip.size());
                udp_port_buf = config.udp_port;
                udp_settings_init = true;
            }

            // UDP IP地址输入框
            {
                const auto row = OverlayUI::BeginSettingRow("UDP地址");
                ImGui::InputText("##value", udp_ip_buf, IM_ARRAYSIZE(udp_ip_buf));
                OverlayUI::EndSettingRow(row);
            }
            // UDP端口号输入框
            {
                const auto row = OverlayUI::BeginSettingRow("UDP端口");
                ImGui::InputInt("##value", &udp_port_buf);
                OverlayUI::EndSettingRow(row);
            }
            // 发送端只传输中心 ROI 时，必须声明该 ROI 所属的完整游戏坐标空间；否则 320 宽
            // 会被误当作完整 FOV，鼠标 counts/pixel 被放大约 8 倍。
            {
                const auto row = OverlayUI::BeginSettingRow("完整FOV宽度");
                if (ImGui::InputInt("##value", &config.udp_source_width))
                {
                    config.udp_source_width = std::clamp(config.udp_source_width, 0, 16384);
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }
            {
                const auto row = OverlayUI::BeginSettingRow("完整FOV高度");
                if (ImGui::InputInt("##value", &config.udp_source_height))
                {
                    config.udp_source_height = std::clamp(config.udp_source_height, 0, 16384);
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }
            // 应用UDP设置按钮
            {
                const auto row = OverlayUI::BeginSettingRow("UDP设置");
                if (ImGui::Button("应用UDP设置", ImVec2(row.controlWidth, 0.0f)))
                {
                    udp_port_buf = std::clamp(udp_port_buf, 1, 65535);
                    config.udp_ip = udp_ip_buf;
                    config.udp_port = udp_port_buf;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }

    // ========== NDI网络视频源捕获设置 ==========
    if (config.capture_method == "ndi")
    {
        if (OverlayUI::BeginSection("NDI", "capture_section_ndi"))
        {
            // 首次打开或手动点击都走异步发现，捕获预览在 mDNS 等待期间继续刷新。
            if (!ndi_sources_loaded && !ndi_source_refreshing)
                startNdiSourceRefresh();
            pollNdiSourceRefresh();

            // NDI源选择下拉框
            {
                const auto row = OverlayUI::BeginSettingRow("NDI源");
                if (ndi_sources.empty() && ndi_source_refreshing)
                {
                    ImGui::TextColored(ImColor(180, 180, 180), "正在搜索NDI源...");
                }
                else if (ndi_sources.empty())
                {
                    ImGui::TextColored(ImColor(180, 180, 180), "未找到NDI源");
                }
                else
                {
                    // 构建源名称列表
                    std::vector<const char*> items;
                    items.reserve(ndi_sources.size());
                    for (const auto& src : ndi_sources)
                        items.push_back(src.c_str());

                    // 查找当前配置对应的源索引
                    int currentIndex = -1;
                    for (size_t i = 0; i < ndi_sources.size(); ++i)
                    {
                        if (ndi_sources[i] == config.ndi_source_name)
                        {
                            currentIndex = static_cast<int>(i);
                            break;
                        }
                    }

                    ImGui::Combo("##value", &currentIndex, items.data(), static_cast<int>(items.size()));
                    // 当选中的源发生改变时更新配置
                    if (currentIndex >= 0 && currentIndex < static_cast<int>(ndi_sources.size()))
                    {
                        std::string newSource = ndi_sources[currentIndex];
                        if (newSource != config.ndi_source_name)
                        {
                            config.ndi_source_name = newSource;
                            OverlayConfig_MarkDirty();
                            capture_method_changed.store(true);
                        }
                    }
                }
                OverlayUI::EndSettingRow(row);
            }

            // 手动刷新NDI源按钮
            {
                const auto row = OverlayUI::BeginSettingRow("");
                ImGui::BeginDisabled(ndi_source_refreshing);
                if (ImGui::Button(
                        ndi_source_refreshing ? "正在刷新..." : "刷新NDI源",
                        ImVec2(row.controlWidth, 0.0f)))
                    startNdiSourceRefresh();
                ImGui::EndDisabled();
                OverlayUI::EndSettingRow(row);
            }

            // OBS 发送 1:1 中心预裁剪 ROI 时，NDI 视频帧本身只有 ROI 尺寸。
            // 这里声明游戏完整 FOV 尺寸，供鼠标角度换算使用；0/0 表示优先使用帧元数据或帧尺寸。
            {
                const auto row = OverlayUI::BeginSettingRow("完整FOV宽度");
                if (ImGui::InputInt("##value", &config.ndi_source_width))
                {
                    config.ndi_source_width = std::clamp(config.ndi_source_width, 0, 16384);
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }
            {
                const auto row = OverlayUI::BeginSettingRow("完整FOV高度");
                if (ImGui::InputInt("##value", &config.ndi_source_height))
                {
                    config.ndi_source_height = std::clamp(config.ndi_source_height, 0, 16384);
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }
}
