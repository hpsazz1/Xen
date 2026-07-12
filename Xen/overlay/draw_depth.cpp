#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <commdlg.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <algorithm>
#include <exception>
#include <thread>
#include <atomic>
#include <mutex>

#include "imgui/imgui.h"
#include "Xen.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "capture.h"
#include "draw_settings.h"
#include "include/other_tools.h"
#include "overlay/ui_sections.h"

#ifdef USE_CUDA
#include "overlay/export_progress_panel.h"
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#include "tensorrt/trt_monitor.h"
#endif

#ifdef USE_CUDA
// 深度估计彩色映射方案名称列表（用于可视化深度图的伪彩色方案）
static const char* kDepthColormapNames[] = {
    "秋色",           // 秋色（红橙黄渐变）
    "骨骼",             // 骨骼（灰蓝渐变）
    "喷射",              // 喷射（彩虹蓝-青-黄-红）
    "冬季",           // 冬季（蓝白渐变）
    "彩虹",          // 彩虹
    "海洋",            // 海洋（蓝绿渐变）
    "夏季",           // 夏季（黄绿渐变）
    "春季",           // 春季（粉紫渐变）
    "冷色",             // 冷色（紫蓝渐变）
    "HSV色环",              // HSV色环
    "粉色",             // 粉色
    "热力",              // 热力（黑红黄白）
    "Parula",           // Parula（MATLAB默认色图）
    "岩浆",            // 岩浆
    "地狱火",          // 地狱火
    "等离子",           // 等离子
    "翠绿",          // 翠绿
    "色盲友好",          // 色盲友好
    "黄昏",         // 黄昏
    "偏移黄昏", // 偏移黄昏
    "涡轮",            // 涡轮（彩虹增强）
    "深绿"         // 深绿
};

#endif

// ============================================================
// 主函数：绘制深度设置UI
// 包含以下子章节：深度推理、深度运行时、深度遮罩、深度状态
// ============================================================
void draw_depth()
{
#ifndef USE_CUDA
    // 非CUDA构建时显示不可用提示
    if (OverlayUI::BeginSection("深度", "depth_section_unavailable"))
    {
        ImGui::TextUnformatted("深度功能需要CUDA构建。");
        OverlayUI::EndSection();
    }
    return;
#else
    // 深度运行时状态文本
    static std::string depthStatus = "Depth runtime is managed automatically.";
    // 标记深度引擎导出线程是否正在运行
    static std::atomic<bool> depthExportRunning{ false };
    // 深度引擎导出线程
    static std::thread depthExportThread;
    // 保护导出结果和导出模型路径的互斥锁
    static std::mutex depthExportMutex;
    // 导出结果消息缓存
    static std::string depthExportResult;
    // 导出的模型名称缓存
    static std::string depthExportedModel;

    // 清理已完成的导出线程（导出完成后join）
    if (depthExportThread.joinable() && !depthExportRunning.load())
    {
        depthExportThread.join();
    }
    // 获取导出线程完成后的结果
    std::string completedEngineModel;
    {
        std::lock_guard<std::mutex> lock(depthExportMutex);
        if (!depthExportResult.empty())
        {
            depthStatus = depthExportResult;
            completedEngineModel = depthExportedModel;
            depthExportResult.clear();
            depthExportedModel.clear();
        }
    }
    // 如果导出成功且模型路径未改变，自动更新配置
    if (!completedEngineModel.empty() && config.depth_model_path != completedEngineModel)
    {
        config.depth_model_path = completedEngineModel;
        OverlayConfig_MarkDirty();
    }

    // 获取可用的深度模型列表
    // 缓存深度模型列表（每 60 帧刷新一次，避免每帧扫描文件系统）
    static std::vector<std::string> cachedDepthModels;
    static int depthModelRefreshCounter = 0;
    if (++depthModelRefreshCounter >= 60 || cachedDepthModels.empty())
    {
        cachedDepthModels = getAvailableDepthModels();
        depthModelRefreshCounter = 0;
    }
    const std::vector<std::string>& availableDepthModels = cachedDepthModels;
    std::string selectedModel;
    bool hasModels = !availableDepthModels.empty();

    // ========== 深度推理设置 ==========
    if (OverlayUI::BeginSection("深度推理", "depth_section_inference"))
    {
        // 深度推理开关
        {
            const auto row = OverlayUI::BeginSettingRow("启用深度推理");
            if (ImGui::Checkbox("##enable_depth_inference", &config.depth_inference_enabled))
            {
                OverlayConfig_MarkDirty();
                if (!config.depth_inference_enabled)
                    depthStatus = "Depth inference disabled.";
            }
            OverlayUI::EndSettingRow(row);
        }

        // 没有可用深度模型时显示警告
        if (!hasModels)
        {
            OverlayUI::TextRow("depth_models 文件夹中无可用深度模型。", IM_COL32(255, 108, 108, 255));
        }
        else
        {
            // 查找当前配置的模型在列表中的索引
            int currentModelIndex = 0;
            auto it = std::find(availableDepthModels.begin(), availableDepthModels.end(), config.depth_model_path);
            if (it == availableDepthModels.end())
            {
                std::string configFile = std::filesystem::path(config.depth_model_path).filename().string();
                it = std::find(availableDepthModels.begin(), availableDepthModels.end(), configFile);
            }
            if (it != availableDepthModels.end())
            {
                currentModelIndex = static_cast<int>(std::distance(availableDepthModels.begin(), it));
            }

            // 构建模型名称下拉列表
            std::vector<const char*> modelItems;
            modelItems.reserve(availableDepthModels.size());
            for (const auto& modelName : availableDepthModels)
            {
                modelItems.push_back(modelName.c_str());
            }

            // 深度模型选择下拉框
            const auto row = OverlayUI::BeginSettingRow("深度模型");
            if (ImGui::Combo("##depth_model", &currentModelIndex, modelItems.data(), static_cast<int>(modelItems.size())))
            {
                if (config.depth_model_path != availableDepthModels[currentModelIndex])
                {
                    config.depth_model_path = availableDepthModels[currentModelIndex];
                    OverlayConfig_MarkDirty();
                }
            }
            OverlayUI::EndSettingRow(row);

            selectedModel = availableDepthModels[currentModelIndex];
        }

        // 判断当前模型是否为ONNX格式（需要额外导出为TensorRT引擎）
        const bool selectedIsOnnx = hasModels && OtherTools::HasExtensionCaseInsensitive(selectedModel, ".onnx");
        const bool exportBusy = depthExportRunning.load();

        // "加载模型"按钮：对于ONNX格式或导出进行中时禁用
        if (!hasModels || selectedIsOnnx || exportBusy)
        {
            ImGui::BeginDisabled();
        }
        {
            const auto row = OverlayUI::BeginSettingRow("加载深度模型");
            if (ImGui::Button("加载", ImVec2(row.controlWidth, 0.0f)))
            {
                if (config.depth_model_path != selectedModel)
                {
                    config.depth_model_path = selectedModel;
                    OverlayConfig_MarkDirty();
                    depthStatus = "Depth model path applied. Runtime loader will update automatically.";
                }
                else
                {
                    depthStatus = "Depth model path already selected.";
                }
            }
            OverlayUI::EndSettingRow(row);
        }
        if (!hasModels || selectedIsOnnx || exportBusy)
        {
            ImGui::EndDisabled();
        }

        // "导出引擎"按钮：仅在ONNX模型且未在导出时可用
        if (!hasModels || !selectedIsOnnx || exportBusy)
        {
            ImGui::BeginDisabled();
        }
        {
            const auto row = OverlayUI::BeginSettingRow("导出深度引擎");
            if (ImGui::Button("导出", ImVec2(row.controlWidth, 0.0f)))
            {
                // 检查导出是否已经在运行
                if (!depthExportRunning.load())
                {
                    // 确保选中的模型已应用到配置
                    if (config.depth_model_path != selectedModel)
                    {
                        config.depth_model_path = selectedModel;
                        OverlayConfig_MarkDirty();
                    }

                    std::string exportPath = selectedModel;
                    if (exportPath.empty())
                    {
                        depthStatus = "Set a depth ONNX path to export.";
                    }
                    else if (!OtherTools::HasExtensionCaseInsensitive(exportPath, ".onnx"))
                    {
                        depthStatus = "Export expects an .onnx depth model path.";
                    }
                    else
                    {
                        // 启动导出线程，异步编译TensorRT引擎
                        depthExportRunning.store(true);
                        depthStatus = "Depth engine export started...";
                        depthExportThread = std::thread([exportPath] {
                            depth_anything::DepthAnythingTrt exporter;
                            std::string result;
                            std::string exportedModel;
                            try
                            {
                                std::string enginePath;
                                if (exporter.exportEngine(exportPath, gLogger, &enginePath))
                                {
                                    // 导出成功，提取文件名
                                    exportedModel = std::filesystem::path(enginePath).filename().string();
                                    result = "Depth engine exported: " + exportedModel;
                                }
                                else
                                {
                                    // 检查是否被用户取消
                                    if (gTrtExportCancelRequested.load())
                                    {
                                        result = "Depth export canceled.";
                                    }
                                    else
                                    {
                                        result = exporter.lastError();
                                    }
                                }
                            }
                            catch (const std::exception& e)
                            {
                                result = std::string("Depth export failed: ") + e.what();
                                exportedModel.clear();
                            }
                            catch (...)
                            {
                                result = "Depth export failed with an unknown error.";
                                exportedModel.clear();
                            }
                            // 保存导出结果，主线程将拾取
                            {
                                std::lock_guard<std::mutex> lock(depthExportMutex);
                                depthExportResult = result;
                                depthExportedModel = exportedModel;
                            }
                            depthExportRunning.store(false);
                        });
                    }
                }
            }
            OverlayUI::EndSettingRow(row);
        }
        if (!hasModels || !selectedIsOnnx || exportBusy)
        {
            ImGui::EndDisabled();
        }

        // 导出进行中时显示进度面板
        if (exportBusy)
        {
            OverlayExportUI::DrawTensorRtExportPanel(
                "depth_tensor_rt_export",
                "深度引擎导出",
                "正在编译优化后的深度推理引擎",
                selectedModel.c_str(),
                "取消深度导出");
        }

        OverlayUI::EndSection();
    }

    // ========== 深度运行时设置 ==========
    if (OverlayUI::BeginSection("深度运行时", "depth_section_runtime"))
    {
        // 深度推理帧率上限
        {
            const auto row = OverlayUI::BeginSettingRow("深度推理帧率");
            if (ImGui::SliderInt("##depth_fps", &config.depth_fps, 0, 120))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度遮罩生成帧率上限
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩帧率");
            if (ImGui::SliderInt("##depth_mask_fps", &config.depth_mask_fps, 1, 30))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }
        OverlayUI::EndSection();
    }

    // ========== 深度遮罩配置 ==========
    if (OverlayUI::BeginSection("深度遮罩", "depth_section_mask"))
    {
        // 深度遮罩启用开关
        {
            const auto row = OverlayUI::BeginSettingRow("启用深度遮罩");
            if (ImGui::Checkbox("##enable_depth_mask", &config.depth_mask_enabled))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度遮罩近处百分比（控制近处物体的遮罩范围）
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩近处百分比");
            if (ImGui::SliderInt("##depth_mask_near_percent", &config.depth_mask_near_percent, 1, 100))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度遮罩边缘扩展像素（扩大遮罩边界）
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩扩展(像素)");
            if (ImGui::SliderInt("##depth_mask_expand", &config.depth_mask_expand, 0, 128))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度遮罩帧保持数（遮罩消失前的持续帧数）
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩帧保持数");
            if (ImGui::SliderInt("##depth_mask_hold_frames", &config.depth_mask_hold_frames, 0, 120))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度遮罩透明度（0=完全透明，255=完全不透明）
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩透明度");
            if (ImGui::SliderInt("##depth_mask_alpha", &config.depth_mask_alpha, 0, 255))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }
        // 透明度为0时提示遮罩不可见
        if (config.depth_mask_enabled && config.depth_mask_alpha == 0)
        {
            OverlayUI::TextRow("深度遮罩不可见：透明度为0。", IM_COL32(255, 108, 108, 255));
        }

        // 深度遮罩反转开关
        {
            const auto row = OverlayUI::BeginSettingRow("深度遮罩反转");
            if (ImGui::Checkbox("##depth_mask_invert", &config.depth_mask_invert))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 游戏内深度调试叠加层开关
        {
            const auto row = OverlayUI::BeginSettingRow("深度调试叠加(游戏内)");
            if (ImGui::Checkbox("##depth_debug_overlay_game", &config.depth_debug_overlay_enabled))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        // 深度彩色映射方案选择
        int colormapIndex = config.depth_colormap;
        {
            const auto row = OverlayUI::BeginSettingRow("深度彩色图");
            if (ImGui::Combo("##depth_colormap", &colormapIndex, kDepthColormapNames, IM_ARRAYSIZE(kDepthColormapNames)))
            {
                config.depth_colormap = colormapIndex;
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        OverlayUI::EndSection();
    }

    // ========== 深度状态信息 ==========
    if (OverlayUI::BeginSection("深度状态", "depth_section_status"))
    {
        // 当前状态
        ImGui::Text("状态：%s", depthStatus.c_str());

        // 深度推理和遮罩均启用时显示详细运行时信息
        if (config.depth_inference_enabled && config.depth_mask_enabled)
        {
            auto& depthMask = depth_anything::GetDepthMaskGenerator();
            const auto state = depthMask.debugState();
            const auto lastErr = depthMask.lastError();
            const auto frameSize = depthMask.lastFrameSize();

            ImGui::Separator();
            ImGui::Text("遮罩运行时：%s", state.model_ready ? "就绪" : "未就绪");
            ImGui::Text("遮罩模型路径：%s",
                state.last_model_path.empty() ? "(无)" : state.last_model_path.c_str());
            if (frameSize.first > 0 && frameSize.second > 0)
                ImGui::Text("上次遮罩帧：%dx%d", frameSize.first, frameSize.second);

            // 显示错误信息（如果有）
            if (!lastErr.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "遮罩错误：%s", lastErr.c_str());
        }
        else if (config.depth_inference_enabled)
        {
            ImGui::Separator();
            ImGui::TextUnformatted("深度遮罩已禁用。");
        }
        else
        {
            ImGui::Separator();
            ImGui::TextUnformatted("深度推理已禁用。");
        }

        ImGui::TextUnformatted("启用调试叠加层后，深度预览将显示在游戏叠加层中。");
        OverlayUI::EndSection();
    }
#endif
}
