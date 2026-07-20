#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "imgui/imgui.h"

#include <algorithm>

#include "Xen.h"
#include "include/other_tools.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "draw_settings.h"
#include "overlay/ui_sections.h"
#ifdef USE_CUDA
#include "overlay/export_progress_panel.h"
#include "trt_monitor.h"
#endif

// ========== 全局跟踪变量 ==========
// 用于在每一帧检测配置是否发生变更，以便触发保存或模型重载
std::string prev_backend = config.backend;                         // 上一次记录的推理后端（TRT / DML）
float prev_confidence_threshold = config.confidence_threshold;     // 上一次记录的置信度阈值
float prev_nms_threshold = config.nms_threshold;                  // 上一次记录的 NMS（非极大值抑制）阈值
int prev_max_detections = config.max_detections;                  // 上一次记录的最大检测目标数

static bool wasExporting = false;         // 标记上一个帧是否正在导出 TensorRT 引擎
static bool ai_state_initialized = false; // 标记 AI 设置面板是否已完成首次初始化

// ========== draw_ai：AI 设置面板 ==========
// 绘制 AI 相关的配置界面，包括：推理后端选择、AI 模型选择、检测参数（置信度/NMS/最大检测数）。
// 同时负责在配置发生变更时标记"脏"状态，以供上层保存或触发模型重载。
void draw_ai()
{
    // 编译时固定后端（仅首次设置，避免每帧覆盖用户选择）
    static bool backendInitialized = false;
    if (!backendInitialized)
    {
#ifdef USE_CUDA
        config.backend = "TRT";
#else
        config.backend = "DML";
#endif
        backendInitialized = true;
    }

    // 首次进入时，将当前配置的快照保存到全局跟踪变量中，作为后续脏检测的基准
    if (!ai_state_initialized)
    {
        prev_backend = config.backend;
        prev_confidence_threshold = config.confidence_threshold;
        prev_nms_threshold = config.nms_threshold;
        prev_max_detections = config.max_detections;
        ai_state_initialized = true;
    }

#ifdef USE_CUDA
    // 如果正在导出 TensorRT 引擎，绘制导出进度面板
    if (gIsTrtExporting)
    {
        OverlayExportUI::DrawTensorRtExportPanel(
            "ai_tensor_rt_export",
            "TensorRT引擎导出",
            "正在编译优化后的AI推理引擎",
            config.ai_model.c_str(),
            "取消导出");
    }
#endif

    // ========== AI 模型选择区域 ==========
    // 从 "models" 文件夹中扫描可用的模型文件，供用户选择
    // 缓存模型列表（仅模型变更时刷新，避免每帧扫描文件系统）
    static std::vector<std::string> cachedAvailableModels;
    static bool modelsCached = false;
    if (!modelsCached || detector_model_changed.load())
    {
        cachedAvailableModels = getAvailableModels();
        modelsCached = true;
    }
    const std::vector<std::string>& availableModels = cachedAvailableModels;
    if (OverlayUI::BeginSection("推理引擎", "ai_section_model"))
    {
        if (availableModels.empty())
        {
            // 无可用模型时显示提示信息
            ImGui::Text("未找到模型文件，请将 .onnx 放入 models 目录。");
        }
        else
        {
            // 查找当前使用的模型在可用模型列表中的索引
            int currentModelIndex = 0;
            auto it = std::find(availableModels.begin(), availableModels.end(), config.ai_model);

            if (it != availableModels.end())
            {
                currentModelIndex = static_cast<int>(std::distance(availableModels.begin(), it));
            }

            // 将 std::string 列表转换为 ImGui 需要的 const char* 列表
            std::vector<const char*> modelsItems;
            modelsItems.reserve(availableModels.size());

            for (const auto& modelName : availableModels)
            {
                modelsItems.push_back(modelName.c_str());
            }

            // 模型下拉选择框
            {
                const auto row = OverlayUI::BeginSettingRow("模型文件");
                if (ImGui::Combo("##model", &currentModelIndex, modelsItems.data(), static_cast<int>(modelsItems.size())))
                {
                    // 如果用户切换了模型，更新配置并标记脏状态，通知检测器重新加载模型
                    if (config.ai_model != availableModels[currentModelIndex])
                    {
                        config.ai_model = availableModels[currentModelIndex];
                        OverlayConfig_MarkDirty();
                        detector_model_changed.store(true);
                    }
                }
                OverlayUI::EndSettingRow(row);
            }

            // 显示当前模型是否使用固定输入尺寸
            OverlayUI::TextRow(config.fixed_input_size ? "固定模型尺寸：已启用" : "固定模型尺寸：已禁用",
                IM_COL32(188, 188, 188, 255));
        }
        OverlayUI::EndSection();
    }

    // ========== 模型类别选择区域 ==========
    // 四类 CS2 模型按阵营成对映射：警方 0/1，匪方 2/3；其他模型可使用自定义 ID。
    if (OverlayUI::BeginSection("目标模型类别", "ai_section_classes"))
    {
        std::vector<std::string> modelClassNames;
        int modelClassCount = 4;
#ifndef USE_CUDA
        if (dml_detector && dml_detector->isReady())
        {
            modelClassCount = std::max(0, dml_detector->getNumberOfClasses());
            modelClassNames = dml_detector->getClassNames();
        }
#endif
        const auto displayClassName = [&modelClassNames](int classId) {
            if (classId < 0 || classId >= static_cast<int>(modelClassNames.size()))
                return std::string("类别 ") + std::to_string(classId);
            const std::string& raw = modelClassNames[classId];
            if (raw == "c") return std::string("警身");
            if (raw == "ch") return std::string("警头");
            if (raw == "t") return std::string("匪身");
            if (raw == "th") return std::string("匪头");
            return raw.empty() ? std::string("类别 ") + std::to_string(classId) : raw;
        };
        const bool hasFactionPresets = modelClassCount >= 4;
        std::vector<std::string> classPresetStorage;
        if (hasFactionPresets)
        {
            classPresetStorage.push_back("警方 · 0 " + displayClassName(0) + " / 1 " + displayClassName(1));
            classPresetStorage.push_back("匪方 · 2 " + displayClassName(2) + " / 3 " + displayClassName(3));
        }
        classPresetStorage.push_back("自定义类别 ID");
        std::vector<const char*> classPresets;
        for (const auto& item : classPresetStorage)
            classPresets.push_back(item.c_str());

        int preset = static_cast<int>(classPresetStorage.size()) - 1;
        if (hasFactionPresets && config.class_player == 0 && config.class_head == 1)
            preset = 0;
        else if (hasFactionPresets && config.class_player == 2 && config.class_head == 3)
            preset = 1;

        {
            const auto row = OverlayUI::BeginSettingRow("目标阵营");
            if (ImGui::Combo("##class_preset", &preset, classPresets.data(), static_cast<int>(classPresets.size())))
            {
                if (hasFactionPresets && preset == 0)
                {
                    config.class_player = 0;
                    config.class_head = 1;
                }
                else if (hasFactionPresets && preset == 1)
                {
                    config.class_player = 2;
                    config.class_head = 3;
                }
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        if (preset == static_cast<int>(classPresetStorage.size()) - 1)
        {
            {
                const auto row = OverlayUI::BeginSettingRow("身体类别 ID");
                if (ImGui::InputInt("##class_player", &config.class_player))
                {
                    config.class_player = std::clamp(config.class_player, 0, 255);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }
            {
                const auto row = OverlayUI::BeginSettingRow("头部类别 ID");
                if (ImGui::InputInt("##class_head", &config.class_head))
                {
                    config.class_head = std::clamp(config.class_head, 0, 255);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }
            if (config.class_player == config.class_head)
                OverlayUI::TextRow("身体类别和头部类别不能相同", IM_COL32(186, 38, 35, 255));
        }
        if (!modelClassNames.empty())
        {
            std::string metadataText = "模型类别：";
            for (size_t i = 0; i < modelClassNames.size(); ++i)
            {
                if (i != 0) metadataText += "、";
                metadataText += std::to_string(i) + "=" + displayClassName(static_cast<int>(i));
            }
            OverlayUI::TextRow(metadataText.c_str(), IM_COL32(96, 101, 109, 255));
        }
        OverlayUI::EndSection();
    }

    // ========== 检测参数设置区域 ==========
    // 包含置信度阈值、NMS 阈值、最大检测目标数三个滑块
    if (OverlayUI::BeginSection("检测阈值", "ai_section_detection"))
    {
        // 置信度阈值滑块：仅保留置信度高于该值的检测结果
        {
            const auto row = OverlayUI::BeginSettingRow("置信度阈值");
            ImGui::SliderFloat("##confidence_threshold", &config.confidence_threshold, 0.01f, 1.00f, "%.2f");
            OverlayUI::EndSettingRow(row);
        }

        // NMS 阈值滑块：控制非极大值抑制的宽容度，值越低重复框越少
        {
            const auto row = OverlayUI::BeginSettingRow("NMS阈值");
            ImGui::SliderFloat("##nms_threshold", &config.nms_threshold, 0.00f, 1.00f, "%.2f");
            OverlayUI::EndSettingRow(row);
        }

        // 最大检测数滑块：限制单帧最多输出的检测目标数量
        {
            const auto row = OverlayUI::BeginSettingRow("最大检测数");
            ImGui::SliderInt("##max_detections", &config.max_detections, 1, 100);
            OverlayUI::EndSettingRow(row);
        }
        OverlayUI::EndSection();
    }

    // ========== 脏检测：检测参数变更 ==========
    // 比较当前值与上一帧保存的快照，若任一检测参数发生改变则标记脏
    if (prev_confidence_threshold != config.confidence_threshold ||
        prev_nms_threshold != config.nms_threshold ||
        prev_max_detections != config.max_detections)
    {
        prev_nms_threshold = config.nms_threshold;
        prev_confidence_threshold = config.confidence_threshold;
        prev_max_detections = config.max_detections;
        OverlayConfig_MarkDirty();
    }

    // ========== 脏检测：推理后端变更 ==========
    // 如果后端（TRT/DML）发生切换，除标记脏外还需通知检测器重新加载模型
    if (prev_backend != config.backend)
    {
        prev_backend = config.backend;
        detector_model_changed.store(true);
        OverlayConfig_MarkDirty();
    }
}
