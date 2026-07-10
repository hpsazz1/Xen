#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "imgui/imgui.h"

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
#ifdef USE_CUDA
    // 如果编译时启用了 CUDA，强制后端为 TensorRT
    config.backend = "TRT";
#else
    // 否则使用 DirectML
    config.backend = "DML";
#endif

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
    std::vector<std::string> availableModels = getAvailableModels();
    if (OverlayUI::BeginSection("模型", "ai_section_model"))
    {
        if (availableModels.empty())
        {
            // 无可用模型时显示提示信息
            ImGui::Text("models 文件夹中无可用模型。");
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
                const auto row = OverlayUI::BeginSettingRow("模型");
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

    // ========== 检测参数设置区域 ==========
    // 包含置信度阈值、NMS 阈值、最大检测目标数三个滑块
    if (OverlayUI::BeginSection("检测", "ai_section_detection"))
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
