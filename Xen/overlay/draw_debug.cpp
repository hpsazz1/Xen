#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <d3d11.h>
#include <iostream>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "scr/data_collector.h"
#include "Xen.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "include/other_tools.h"
#include "capture.h"
#include "overlay/ui_sections.h"

#ifdef USE_CUDA
#include "depth/depth_mask.h"
#endif

// 安全释放COM接口对象的宏，检查非空后Release并置空
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p)       \
    do {                      \
        if ((p) != nullptr) { \
            (p)->Release();   \
            (p) = nullptr;    \
        }                     \
    } while (0)
#endif

// 用于检测screenshot_delay和verbose是否发生变化的旧值快照
int prev_screenshot_delay = config.screenshot_delay;
bool prev_verbose = config.verbose;

// 调试帧预览所需的D3D纹理和着色器资源视图
static ID3D11Texture2D* g_debugTex = nullptr;          // 调试帧纹理
static ID3D11ShaderResourceView* g_debugSRV = nullptr;  // 调试帧着色器资源视图
static int texW = 0, texH = 0;                           // 调试帧纹理宽高

// 深度遮罩预览所需的D3D纹理和着色器资源视图
static ID3D11Texture2D* g_maskTex = nullptr;            // 遮罩纹理
static ID3D11ShaderResourceView* g_maskSRV = nullptr;    // 遮罩着色器资源视图
static int maskTexW = 0, maskTexH = 0;                   // 遮罩纹理宽高

// 调试帧缩放比例（用于预览窗口中的图像缩放）
static float debug_scale = 0.5f;
// 数据收集输出目录的文本缓冲区及其镜像值（用于检测变化）
static char g_collectOutputDirBuffer[512] = {};
static std::string g_collectOutputDirMirror;
// 数据收集类别筛选器文本缓冲区及其镜像值
static char g_collectClassFilterBuffer[256] = {};
static std::string g_collectClassFilterMirror;

// 将字符串同步到静态文本缓冲区（仅当镜像值发生变化时才更新，避免重复刷新）
static void syncDebugTextBuffer(char* buffer, size_t buffer_size, std::string& mirror, const std::string& value)
{
    if (mirror == value)
        return;

    std::snprintf(buffer, buffer_size, "%s", value.c_str());
    buffer[buffer_size - 1] = '\0';
    mirror = value;
}

// 将文本缓冲区内容应用到配置目标，同时更新镜像值，返回是否发生了实际变更
static bool applyDebugTextBuffer(std::string& target, std::string& mirror, const char* buffer)
{
    const std::string value = buffer ? std::string(buffer) : std::string();
    if (target == value && mirror == value)
        return false;

    target = value;
    mirror = value;
    return true;
}

// 根据按键名称查找对应的按键索引（遍历key_names数组）
static int findDebugKeyIndexByName(const std::string& keyName)
{
    for (size_t k = 0; k < key_names.size(); ++k)
    {
        if (key_names[k] == keyName)
            return static_cast<int>(k);
    }
    return 0;
}

// 绘制截图按键配置行（支持添加/删除多个截图快捷键）
static bool drawScreenshotButtonRows()
{
    // 无按键列表时显示禁用提示
    if (key_names_cstrs.empty())
    {
        ImGui::TextDisabled("无可用按键列表。");
        return false;
    }

    bool changed = false;
    // 截图按键列表为空时初始化为"None"
    if (config.screenshot_button.empty())
    {
        config.screenshot_button.push_back("None");
        changed = true;
    }

    // 遍历当前所有截图按键配置行
    for (size_t i = 0; i < config.screenshot_button.size();)
    {
        std::string& currentKeyName = config.screenshot_button[i];
        int currentIndex = findDebugKeyIndexByName(currentKeyName);
        // 多按键时显示行号，单按键时不显示序号
        const std::string rowLabel = (config.screenshot_button.size() > 1)
            ? "截图 " + std::to_string(i + 1)
            : "截图";

        ImGui::PushID(static_cast<int>(i));

        const auto row = OverlayUI::BeginSettingRow(rowLabel.c_str());
        // 计算下拉框宽度（扣除加减按钮的宽度）
        const float actionBtnW = ImGui::GetFrameHeight();
        float comboWidth = row.controlWidth - (actionBtnW * 2.0f + 7.0f);
        if (comboWidth < 1.0f)
            comboWidth = 1.0f;
        ImGui::SetNextItemWidth(comboWidth);

        // 截图快捷键选择下拉框
        if (ImGui::Combo("##value", &currentIndex, key_names_cstrs.data(), static_cast<int>(key_names_cstrs.size())))
        {
            currentKeyName = key_names[currentIndex];
            changed = true;
        }

        // 添加新截图按键行的"+"按钮
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("+", ImVec2(actionBtnW, 0.0f)))
        {
            config.screenshot_button.insert(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i + 1), "None");
            changed = true;
        }

        // 删除当前截图按键行的"-"按钮
        ImGui::SameLine(0.0f, 3.0f);
        bool removedCurrent = false;
        if (ImGui::Button("-", ImVec2(actionBtnW, 0.0f)))
        {
            // 至少保留一行，置为"None"而不删除
            if (config.screenshot_button.size() <= 1)
            {
                config.screenshot_button[0] = "None";
            }
            else
            {
                config.screenshot_button.erase(config.screenshot_button.begin() + static_cast<std::vector<std::string>::difference_type>(i));
                removedCurrent = true;
            }
            changed = true;
        }

        OverlayUI::EndSettingRow(row);
        ImGui::PopID();

        // 如果当前行被删除，不递增索引以处理下一行
        if (removedCurrent)
            continue;

        ++i;
    }

    return changed;
}

// 将OpenCV的BGR帧上传到D3D11调试纹理（用于叠加层预览）
static void uploadDebugFrame(const cv::Mat& bgr)
{
    if (bgr.empty()) return;

    // 纹理尺寸不匹配或未创建时重新创建
    if (!g_debugTex || bgr.cols != texW || bgr.rows != texH)
    {
        // 释放旧的纹理和资源视图
        SAFE_RELEASE(g_debugTex);
        SAFE_RELEASE(g_debugSRV);

        texW = bgr.cols;  texH = bgr.rows;

        // 创建动态D3D纹理，格式为RGBA8
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = texW;
        td.Height = texH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_debugTex);
        if (FAILED(hr) || !g_debugTex) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = g_pd3dDevice->CreateShaderResourceView(g_debugTex, &sd, &g_debugSRV);
        if (FAILED(hr) || !g_debugSRV)
        {
            g_debugTex->Release();
            g_debugTex = nullptr;
            return;
        }
    }

    // BGR转RGBA后拷贝到D3D纹理
    static cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_debugTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < texH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), texW * 4);
        g_pd3dDeviceContext->Unmap(g_debugTex, 0);
    }
}

// 将RGBA遮罩帧上传到D3D11遮罩纹理（用于深度遮罩预览叠加）
static void uploadMaskFrame(const cv::Mat& rgba)
{
    if (rgba.empty()) return;

    // 纹理尺寸不匹配或未创建时重新创建遮罩纹理
    if (!g_maskTex || rgba.cols != maskTexW || rgba.rows != maskTexH)
    {
        SAFE_RELEASE(g_maskTex);
        SAFE_RELEASE(g_maskSRV);

        maskTexW = rgba.cols;
        maskTexH = rgba.rows;

        // 创建动态D3D纹理，格式为RGBA8
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = maskTexW;
        td.Height = maskTexH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        g_pd3dDevice->CreateTexture2D(&td, nullptr, &g_maskTex);

        // 创建对应的着色器资源视图
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        g_pd3dDevice->CreateShaderResourceView(g_maskTex, &sd, &g_maskSRV);
    }

    // 将RGBA数据通过映射拷贝到D3D纹理
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_pd3dDeviceContext->Map(g_maskTex, 0,
        D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        for (int y = 0; y < maskTexH; ++y)
            memcpy((uint8_t*)ms.pData + ms.RowPitch * y,
                rgba.ptr(y), maskTexW * 4);
        g_pd3dDeviceContext->Unmap(g_maskTex, 0);
    }
}

// 绘制数据收集设置UI（包含收集开关、自动标注配置和统计信息）
static bool drawDataCollectionSection()
{
    // 将配置值同步到本地文本缓冲区
    syncDebugTextBuffer(g_collectOutputDirBuffer, sizeof(g_collectOutputDirBuffer), g_collectOutputDirMirror, config.collect_output_dir);
    syncDebugTextBuffer(g_collectClassFilterBuffer, sizeof(g_collectClassFilterBuffer), g_collectClassFilterMirror, config.auto_label_record_classes);

    bool changed = false;
    if (!OverlayUI::BeginSection("数据采集", "debug_section_data_collection"))
        return false;

    // 数据收集条件选项
    changed |= OverlayUI::CheckboxRow("游戏中收集数据", &config.collect_data_while_playing);
    changed |= OverlayUI::CheckboxRow("仅当自瞄激活时", &config.collect_only_when_aimbot_running);
    changed |= OverlayUI::CheckboxRow("仅当存在目标时", &config.collect_only_when_targets_present);

    // 每隔N帧保存一次
    int saveEveryNFrames = config.collect_save_every_n_frames;
    if (OverlayUI::SliderIntRow("每N帧保存一次", &saveEveryNFrames, 1, 120))
    {
        config.collect_save_every_n_frames = saveEveryNFrames;
        changed = true;
    }

    // JPEG保存质量
    int jpegQuality = config.collect_jpeg_quality;
    if (OverlayUI::SliderIntRow("JPEG质量", &jpegQuality, 50, 100))
    {
        config.collect_jpeg_quality = jpegQuality;
        changed = true;
    }

    // 输出文件夹路径
    if (OverlayUI::InputTextRow("输出文件夹", g_collectOutputDirBuffer, sizeof(g_collectOutputDirBuffer)))
        changed |= applyDebugTextBuffer(config.collect_output_dir, g_collectOutputDirMirror, g_collectOutputDirBuffer);

    // ===== 自动标注子章节 =====
    if (OverlayUI::BeginSubsection("YOLO 标注"))
    {
        changed |= OverlayUI::CheckboxRow("写入YOLO标签文件", &config.auto_label_data);

        ImGui::BeginDisabled(!config.auto_label_data);

        // 标注最小置信度阈值
        float minConf = config.auto_label_min_conf;
        if (OverlayUI::SliderFloatRow("最小置信度", &minConf, 0.01f, 0.99f, "%.2f"))
        {
            config.auto_label_min_conf = minConf;
            changed = true;
        }

        // 每张图片最大标注框数
        int maxBoxes = config.auto_label_max_boxes;
        if (OverlayUI::SliderIntRow("每文件最大框数", &maxBoxes, 1, 100))
        {
            config.auto_label_max_boxes = maxBoxes;
            changed = true;
        }

        // 类别筛选（逗号分隔的ID）
        if (OverlayUI::InputTextRow("类别筛选", g_collectClassFilterBuffer, sizeof(g_collectClassFilterBuffer)))
            changed |= applyDebugTextBuffer(config.auto_label_record_classes, g_collectClassFilterMirror, g_collectClassFilterBuffer);

        ImGui::TextDisabled("类别筛选为空时记录所有类别。使用逗号分隔的ID，如 0,1。");
        ImGui::EndDisabled();

        OverlayUI::EndSubsection();
    }

    // 数据收集运行时统计信息显示
    const cvm::DataCollectionUiState ui = cvm::GetDataCollectionUiState("", config.ai_model.c_str(), config);
    ImGui::Separator();
    ImGui::Text("已观测帧数：%llu", static_cast<unsigned long long>(ui.observed_frame_count));
    ImGui::Text("保存尝试：%llu", static_cast<unsigned long long>(ui.attempted_sample_count));
    ImGui::Text("已保存图片：%llu", static_cast<unsigned long long>(ui.saved_image_count));
    ImGui::Text("标签文件：%llu", static_cast<unsigned long long>(ui.saved_label_count));
    ImGui::TextWrapped("解析路径：%s", ui.resolved_output_dir.c_str());
    if (!ui.status.empty())
        ImGui::TextWrapped("状态：%s", ui.status.c_str());
    else
        ImGui::TextDisabled("状态：空闲");

    // 复制路径和重置计数按钮
    if (OverlayUI::ButtonRow("解析路径", "复制路径", "copy_resolved_path"))
        ImGui::SetClipboardText(ui.resolved_output_dir.c_str());

    if (OverlayUI::ButtonRow("收集计数", "重置计数", "reset_collect_counters"))
        cvm::ResetDataCollectionRuntime();

    OverlayUI::EndSection();
    return changed;
}

// 绘制调试帧预览叠加层（显示检测框、深度遮罩和鼠标预测轨迹）
void draw_debug_frame()
{
    // 从全局帧缓冲区获取最新帧的副本（线程安全）
    cv::Mat frameCopy;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
            latestFrame.copyTo(frameCopy);
    }

    // 将帧上传到D3D纹理
    uploadDebugFrame(frameCopy);

    if (!g_debugSRV) return;

    // 调试缩放比例滑块
    {
        const auto row = OverlayUI::BeginSettingRow("调试缩放");
        ImGui::SliderFloat("##value", &debug_scale, 0.1f, 2.0f, "%.1fx");
        OverlayUI::EndSettingRow(row);
    }

    // 显示调试图像
    ImVec2 image_size(texW * debug_scale, texH * debug_scale);
    ImGui::Image((ImTextureID)(intptr_t)g_debugSRV, image_size);

    ImVec2 image_pos = ImGui::GetItemRectMin();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

#ifdef USE_CUDA
    // 如果启用了深度遮罩，在调试帧上叠加显示遮罩
    if (config.depth_mask_enabled)
    {
        auto& depthMask = depth_anything::GetDepthMaskGenerator();
        cv::Mat mask = depthMask.getMask();
        if (!mask.empty() && mask.size() == frameCopy.size())
        {
            // 构建RGBA遮罩：红色通道表示遮罩区域，alpha控制透明度
            cv::Mat alpha(mask.size(), CV_8U, cv::Scalar(0));
            alpha.setTo(config.depth_mask_alpha, mask);

            std::vector<cv::Mat> channels(4);
            channels[0] = cv::Mat(mask.size(), CV_8U, cv::Scalar(255));
            channels[1] = cv::Mat(mask.size(), CV_8U, cv::Scalar(0));
            channels[2] = cv::Mat(mask.size(), CV_8U, cv::Scalar(0));
            channels[3] = alpha;

            cv::Mat rgba;
            cv::merge(channels, rgba);
            uploadMaskFrame(rgba);

            if (g_maskSRV)
            {
                ImVec2 overlay_max(image_pos.x + image_size.x, image_pos.y + image_size.y);
                draw_list->AddImage((ImTextureID)(intptr_t)g_maskSRV, image_pos, overlay_max);
            }
        }
    }
#endif

    // 绘制检测边界框（从检测缓冲区读取，线程安全）
    {
        std::lock_guard<std::mutex> lock(detectionBuffer.mutex);
        for (size_t i = 0; i < detectionBuffer.boxes.size(); ++i)
        {
            const cv::Rect& box = detectionBuffer.boxes[i];

            // 根据缩放比例计算框的屏幕位置
            ImVec2 p1(image_pos.x + box.x * debug_scale,
                image_pos.y + box.y * debug_scale);
            ImVec2 p2(p1.x + box.width * debug_scale,
                p1.y + box.height * debug_scale);

            ImU32 color = IM_COL32(255, 0, 0, 255);

            // 绘制红色矩形框
            draw_list->AddRect(p1, p2, color, 0.0f, 2.0f);

            // 在框上方显示类别标签
            if (i < detectionBuffer.classes.size())
            {
                std::string label = std::to_string(detectionBuffer.classes[i]);
                draw_list->AddText(ImVec2(p1.x, p1.y - 16), IM_COL32(255, 255, 0, 255), label.c_str());
            }
        }
    }

    // 绘制鼠标未来轨迹预测点（从红到蓝渐变）
    if (config.draw_futurePositions && globalMouseThread)
    {
        auto futurePts = globalMouseThread->getFuturePositions();
        if (!futurePts.empty())
        {
            // 将检测分辨率坐标映射到显示尺寸
            float scale_x = static_cast<float>(texW) / config.detection_resolution;
            float scale_y = static_cast<float>(texH) / config.detection_resolution;

            // 裁剪到图像区域
            ImVec2 clip_min = image_pos;
            ImVec2 clip_max = ImVec2(image_pos.x + texW * debug_scale,
                image_pos.y + texH * debug_scale);
            draw_list->PushClipRect(clip_min, clip_max, true);

            int totalPts = static_cast<int>(futurePts.size());
            for (size_t i = 0; i < futurePts.size(); ++i)
            {
                int px = static_cast<int>(futurePts[i].first * scale_x);
                int py = static_cast<int>(futurePts[i].second * scale_y);
                ImVec2 pt(image_pos.x + px * debug_scale,
                    image_pos.y + py * debug_scale);

                // 颜色渐变：早期为蓝色，后期为红色
                int b = static_cast<int>(255 - (i * 255.0 / totalPts));
                int r = static_cast<int>(i * 255.0 / totalPts);
                int g = 50;

                ImU32 fillColor = IM_COL32(r, g, b, 255);
                ImU32 outlineColor = IM_COL32(255, 255, 255, 255);

                // 绘制实心圆点和白色边框
                draw_list->AddCircleFilled(pt, 4.0f * debug_scale, fillColor);
                draw_list->AddCircle(pt, 4.0f * debug_scale, outlineColor, 0, 1.0f);
            }

            draw_list->PopClipRect();
        }
    }
}

// 绘制捕获预览章节（显示预览窗口开关和调试帧）
void draw_capture_preview()
{
    if (OverlayUI::BeginSection("采集预览", "capture_section_preview"))
    {
        // 预览窗口开关
        {
            const auto row = OverlayUI::BeginSettingRow("显示预览窗口");
            if (ImGui::Checkbox("##value", &config.show_window))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 开启预览时显示调试帧
        if (config.show_window)
        {
            draw_debug_frame();
        }

        OverlayUI::EndSection();
    }
}

// 主调试设置UI（包含截图按键、数据收集等配置）
void draw_debug()
{
    bool changed = false;

    // ========== 截图按键配置 ==========
    if (OverlayUI::BeginSection("截图绑定", "debug_section_screenshot_buttons"))
    {
        if (drawScreenshotButtonRows())
            changed = true;

        // 截图延迟（毫秒）
        if (OverlayUI::InputIntRow("截图延迟(毫秒)", &config.screenshot_delay, 50, 500))
            changed = true;
        // 详细控制台输出开关
        if (OverlayUI::CheckboxRow("详细控制台输出", &config.verbose))
            changed = true;

        if (config.screenshot_delay < 0)
            config.screenshot_delay = 0;

        // 打印OpenCV构建信息按钮
        if (OverlayUI::ButtonRow("OpenCV", "打印构建信息", "button_cv2_build_info"))
        {
            std::cout << cv::getBuildInformation() << std::endl;
        }

        OverlayUI::EndSection();
    }

    // 数据收集设置
    changed |= drawDataCollectionSection();

    // 检测配置变更并标记脏
    if (prev_screenshot_delay != config.screenshot_delay ||
        prev_verbose != config.verbose)
    {
        prev_screenshot_delay = config.screenshot_delay;
        prev_verbose = config.verbose;
        changed = true;
    }

    if (changed)
    {
        OverlayConfig_MarkDirty();
    }
}
