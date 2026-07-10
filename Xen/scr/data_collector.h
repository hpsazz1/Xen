#pragma once

#include "config.h"

#include <opencv2/opencv.hpp>
#ifdef USE_CUDA
#include <opencv2/core/cuda.hpp>
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cvm {

/**
 * @brief 数据采集 UI 状态结构
 *
 * 记录数据采集过程中的各项统计信息和运行状态。
 */
struct DataCollectionUiState
{
    bool enabled = false;                          ///< 采集是否已启用
    std::uint64_t observed_frame_count = 0;         ///< 已处理的帧数
    std::uint64_t attempted_sample_count = 0;       ///< 尝试采样的次数
    std::uint64_t saved_image_count = 0;            ///< 已保存的图片数量
    std::uint64_t saved_label_count = 0;            ///< 已保存的标签数量
    std::string resolved_output_dir;                ///< 解析后的输出目录
    std::string status;                             ///< 当前状态描述
};

/** @brief 解析数据采集输出目录路径 */
std::filesystem::path ResolveCollectOutputDir(const std::string& root_dir, const char* output_dir_raw);
/** @brief 检查数据采集是否已启用 */
bool IsDataCollectionEnabled(const Config& cfg);
/** @brief 获取数据采集 UI 状态信息 */
DataCollectionUiState GetDataCollectionUiState(const std::string& root_dir, const char* model_name, const Config& cfg);
/** @brief 重置数据采集运行时状态 */
void ResetDataCollectionRuntime();

/**
 * @brief 可能进行数据采集（根据配置决定是否保存当前帧）
 * @param root_dir 根目录
 * @param model_name 模型名称
 * @param frame 当前视频帧（CPU）
 * @param boxes 检测到的边界框
 * @param classes 对应的类别 ID
 * @param confidences 置信度
 * @param aimbot_enabled 自瞄是否启用
 * @param cfg 程序配置
 */
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::Mat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg);

#ifdef USE_CUDA
/**
 * @brief 可能进行数据采集（GPU 版本）
 * @param frame 当前视频帧（GPU）
 */
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::cuda::GpuMat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg);
#endif

}  // namespace cvm
