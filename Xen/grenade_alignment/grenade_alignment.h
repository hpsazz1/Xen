#ifndef GRENADE_ALIGNMENT_H
#define GRENADE_ALIGNMENT_H

#include "config/config.h"
#include "source_context/source_context.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

namespace grenade_alignment {
struct Geometry {
    int source_width = 0, source_height = 0;
    double roi_x = 0, roi_y = 0;
    int roi_width = 320, roi_height = 320;
    double scale_x = 1, scale_y = 1;
};

struct Reference {
    std::filesystem::path directory;
    // manifest及已有原图的SHA-256，调用方可据此撤销旧的已对齐状态。
    std::string revision;
    nlohmann::json plan;
    Geometry geometry;
    // 参考ROI内像素坐标；全屏视轴通过实际帧几何换算，不假定ROI居中。
    cv::Point2d target{160, 160};
    cv::Mat standing_image, aim_image;
};

// 只读固定的 ../references/<方案文件stem>/reference.json；资源或方案绑定
// 不合法时抛出中文异常，由Session冷路径统一显示，不补造图片或人工通过事实。
// 找点只需当前方案的瞄点图；allow_partial供展示单图或保留旧方案关联的原图。
Reference load_reference(const std::filesystem::path& plan_path,
                         const nlohmann::json& expected_plan, bool allow_partial = false);

struct Result {
    bool aligned = false;
    bool canceled = false;
    std::string error;
    nlohmann::json evidence = nlohmann::json::object();
    // 最后验证通过的实际源帧时刻；采集关闭或函数返回不刷新其年龄。
    std::chrono::steady_clock::time_point verified_at{};
    // 全局急停须由Session撤销测试模板；与只取消本次对齐的方向左键不同。
    bool emergency_stop = false;
};

// Runtime运行时借用原始ROI和同源焦点；空值沿用停止态独立采集。
// 借用的生命周期由Session持有，不创建第二个NDI接收器。
struct SharedStream {
    std::function<CaptureStatus(CapturedFrame&)> grab;
    std::function<source_context::SourceContextSnapshot()> source;
    bool settle_before_alignment = false; // 蹲姿起步只观察，画面稳定后再细调。
};

// 同步有界任务，由Session独占调度；复用已有Mouse，不创建或关闭设备。
// verify_only只取新帧验证，不输出。permitted须可并发读取，每次输出前复核。
Result run(const Reference& reference, const AppConfig& config,
           std::shared_ptr<IMouseController> device, std::atomic_bool& canceled,
           bool verify_only = false, std::function<bool()> permitted = {},
           SharedStream stream = {}) noexcept;
}

#endif
