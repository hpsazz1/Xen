#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "scr/data_collector.h"
#include "other_tools.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace cvm {
namespace {

namespace fs = std::filesystem;

// 两次收集保存操作之间的最小间隔时间（纳秒），防止频繁写入磁盘
constexpr int64_t kCollectSaveCooldownNs = 500'000'000;

// 数据收集运行时状态结构体
// 跟踪帧计数、样本计数、已保存的图像/标签数量以及上次保存时间
struct CollectRuntimeState
{
    std::uint64_t frame_counter = 0;       // 全局帧计数器
    std::uint64_t sample_counter = 0;      // 已尝试的样本数
    std::uint64_t saved_image_count = 0;   // 已保存的图像数
    std::uint64_t saved_label_count = 0;   // 已保存的标签文件数
    int64_t last_collect_save_ns = 0;      // 上次保存的时间戳（纳秒）
    std::string last_output_dir;           // 上次使用的输出目录
    std::string last_status;               // 上次操作的状态信息
};

// 数据收集配置快照结构体
// 从 Config 对象中捕获当前配置，确保在收集过程中配置不会被修改
struct CollectConfigSnapshot
{
    bool enabled = false;                  // 是否启用数据收集
    bool only_when_aimbot_running = false; // 是否仅在自瞄运行时收集
    bool only_when_targets_present = false;// 是否仅在检测到目标时收集
    int save_every_n_frames = 1;           // 每隔 N 帧保存一次
    int jpeg_quality = 95;                 // JPEG 图像质量 (50-100)
    std::string output_dir;                // 输出目录路径
    bool auto_label_data = false;          // 是否自动生成 YOLO 标签
    float auto_label_min_conf = 0.25f;     // 自动标签的最小置信度阈值
    int auto_label_max_boxes = 20;         // 自动标签的最大框数
    std::string auto_label_record_classes; // 需要记录的类别 ID（逗号分隔）
};

// 收集尝试结构体，包含配置快照和样本 ID
struct CollectAttempt
{
    CollectConfigSnapshot cfg;
    std::uint64_t sample_id = 0;
};

// 全局运行时状态和互斥锁（线程安全）
CollectRuntimeState g_collectRuntimeState;
std::mutex g_collectRuntimeMutex;

// 获取可执行文件所在目录
// 通过 Windows API GetModuleFileNameW 获取 exe 路径并提取父目录
std::string GetExecutableDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return ".";

    return fs::path(exePath).parent_path().string();
}

// 构建收集样本的文件名主干（不含扩展名）
// 格式：YYYYMMDD_HHMMSS_MS_s000000，包含时间戳和样本 ID
std::string BuildCollectSampleStem(std::uint64_t sample_id)
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
    localtime_s(&local_tm, &t);

    char time_buf[80] = {};
    std::snprintf(
        time_buf,
        sizeof(time_buf),
        "%04d%02d%02d_%02d%02d%02d_%03lld_s%06llu",
        local_tm.tm_year + 1900,
        local_tm.tm_mon + 1,
        local_tm.tm_mday,
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_sec,
        static_cast<long long>(ms.count()),
        static_cast<unsigned long long>(sample_id));
    return std::string(time_buf);
}

// 解析自动标签的记录类别字符串
// 输入格式：逗号分隔的整数，如 "0,1,2"，返回对应的整数集合
std::set<int> ParseRecordClasses(const char* s)
{
    std::set<int> ids;
    if (!s || !s[0])
        return ids;

    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item = OtherTools::TrimAscii(item);
        if (item.empty())
            continue;

        try
        {
            ids.insert(std::stoi(item));
        }
        catch (...) {}
    }

    return ids;
}

// 准备保存用的帧图像
// 将各种通道格式统一转换为 BGR 三通道格式：
//   - 4通道 BGRA → 转换 BGR
//   - 3通道 BGR → 克隆
//   - 1通道 灰度 → 转换 BGR
cv::Mat PrepareFrameForSave(const cv::Mat& frame)
{
    if (frame.empty())
        return {};

    cv::Mat bgr;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        break;
    case 3:
        bgr = frame.clone();
        break;
    case 1:
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        break;
    default:
        break;
    }

    return bgr;
}

// 根据模型名称生成对应的输出子文件夹名
// 从模型文件路径中提取主文件名（不含扩展名），若无效则使用 "default"
std::string ModelNameToFolder(const char* model_name)
{
    if (!model_name || model_name[0] == '\0')
        return "default";

    std::string s = OtherTools::TrimAscii(model_name);
    if (s.empty())
        return "default";

    const fs::path p(s);
    const std::string stem = p.filename().stem().string();
    return stem.empty() ? "default" : stem;
}

// 从 Config 中快照当前数据收集配置
// 对关键参数进行范围限制（如 JPEG 质量、置信度阈值等）
CollectConfigSnapshot SnapshotCollectConfig(const Config& cfg)
{
    CollectConfigSnapshot snapshot;
    snapshot.enabled = cfg.collect_data_while_playing;
    snapshot.only_when_aimbot_running = cfg.collect_only_when_aimbot_running;
    snapshot.only_when_targets_present = cfg.collect_only_when_targets_present;
    snapshot.save_every_n_frames = std::max(1, cfg.collect_save_every_n_frames);
    snapshot.jpeg_quality = std::clamp(cfg.collect_jpeg_quality, 50, 100);
    snapshot.output_dir = cfg.collect_output_dir;
    snapshot.auto_label_data = cfg.auto_label_data;
    snapshot.auto_label_min_conf = std::clamp(cfg.auto_label_min_conf, 0.01f, 0.99f);
    snapshot.auto_label_max_boxes = std::max(1, cfg.auto_label_max_boxes);
    snapshot.auto_label_record_classes = cfg.auto_label_record_classes;
    return snapshot;
}

// 更新运行时状态信息（线程安全）
// 设置输出目录和最新状态字符串
void UpdateRuntimeStatus(const std::string& output_dir, const std::string& status)
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    if (!output_dir.empty())
        g_collectRuntimeState.last_output_dir = output_dir;
    g_collectRuntimeState.last_status = status;
}

// 写入 YOLO 格式的标签文件
// 将检测框（boxes）转换为 YOLO 格式：class_id center_x center_y width height
//   - label_path：标签文件路径
//   - boxes/classes/confidences：检测结果
//   - frame_width/height：图像尺寸，用于归一化
//   - min_conf/max_boxes：置信度阈值和最大框数
//   - allowed_classes：可选，只记录指定类别的框
std::string WriteYoloLabelFile(const fs::path& label_path,
                               const std::vector<cv::Rect>& boxes,
                               const std::vector<int>& classes,
                               const std::vector<float>& confidences,
                               int frame_width,
                               int frame_height,
                               float min_conf,
                               int max_boxes,
                               const std::set<int>* allowed_classes)
{
    std::ofstream out(label_path, std::ios::trunc);
    if (!out.is_open())
        return "label open failed";

    const float width = std::max(1.0f, static_cast<float>(frame_width));
    const float height = std::max(1.0f, static_cast<float>(frame_height));
    int written = 0;

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const int cls = (i < classes.size()) ? classes[i] : 0;
        const float conf = (i < confidences.size()) ? confidences[i] : 1.0f;
        if (conf < min_conf)
            continue;

        if (allowed_classes && !allowed_classes->empty() && allowed_classes->count(cls) == 0)
            continue;

        if (written >= std::max(1, max_boxes))
            break;

        const cv::Rect& box = boxes[i];
        const float x1 = std::clamp(static_cast<float>(box.x), 0.0f, width);
        const float y1 = std::clamp(static_cast<float>(box.y), 0.0f, height);
        const float x2 = std::clamp(static_cast<float>(box.x + box.width), 0.0f, width);
        const float y2 = std::clamp(static_cast<float>(box.y + box.height), 0.0f, height);

        const float box_w = std::max(0.0f, x2 - x1) / width;
        const float box_h = std::max(0.0f, y2 - y1) / height;
        if (box_w <= 0.0f || box_h <= 0.0f)
            continue;

        const float cx = std::clamp(((x1 + x2) * 0.5f) / width, 0.0f, 1.0f);
        const float cy = std::clamp(((y1 + y2) * 0.5f) / height, 0.0f, 1.0f);

        out << cls << " " << cx << " " << cy << " " << box_w << " " << box_h << "\n";
        ++written;
    }

    return std::to_string(written) + " label(s)";
}

// 解析模型输出目录路径
// 返回 {images目录, labels目录} 的 pair，按模型名称划分子文件夹
std::pair<fs::path, fs::path> ResolveModelOutputDirs(const std::string& root_dir,
                                                     const char* model_name,
                                                     const CollectConfigSnapshot& cfg)
{
    const fs::path output_root = ResolveCollectOutputDir(root_dir, cfg.output_dir.c_str());
    const fs::path model_root = output_root / ModelNameToFolder(model_name);
    return { model_root / "images", model_root / "labels" };
}

// 构建待保存的帧图像
// 调用 PrepareFrameForSave 转换格式，并检查图像是否有效（非空、尺寸 > 0）
bool BuildSaveFrame(const cv::Mat& frame, cv::Mat& save_frame)
{
    save_frame = PrepareFrameForSave(frame);
    return !save_frame.empty() && save_frame.cols > 0 && save_frame.rows > 0;
}

// 尝试开始一次收集操作
// 检查各种条件（是否启用、自瞄状态、目标存在、帧间隔、冷却时间），符合条件则分配样本 ID
bool TryBeginCollectAttempt(const CollectConfigSnapshot& cfg,
                            const std::vector<cv::Rect>& boxes,
                            bool aimbot_enabled,
                            std::uint64_t& sample_id)
{
    if (!cfg.enabled)
        return false;

    if (cfg.only_when_aimbot_running && !aimbot_enabled)
        return false;

    if (cfg.only_when_targets_present && boxes.empty())
        return false;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    ++g_collectRuntimeState.frame_counter;
    if ((g_collectRuntimeState.frame_counter % static_cast<std::uint64_t>(cfg.save_every_n_frames)) != 0)
        return false;

    if (g_collectRuntimeState.last_collect_save_ns > 0 &&
        (now_ns - g_collectRuntimeState.last_collect_save_ns) < kCollectSaveCooldownNs)
    {
        return false;
    }

    g_collectRuntimeState.last_collect_save_ns = now_ns;
    sample_id = ++g_collectRuntimeState.sample_counter;
    return true;
}

// 保存收集的帧数据（图像 + 标签）
// 创建输出目录结构，写入 JPEG 图像，若启用自动标签则写入 YOLO 标签文件
void SaveCollectedFrame(const std::string& root_dir,
                        const char* model_name,
                        const cv::Mat& frame,
                        const std::vector<cv::Rect>& boxes,
                        const std::vector<int>& classes,
                        const std::vector<float>& confidences,
                        const CollectAttempt& attempt)
{
    cv::Mat save_frame;
    if (!BuildSaveFrame(frame, save_frame))
        return;

    const auto [images_dir, labels_dir] = ResolveModelOutputDirs(root_dir, model_name, attempt.cfg);
    const fs::path model_root = images_dir.parent_path();

    std::error_code ec;
    fs::create_directories(images_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: create images folder.");
        return;
    }

    ec.clear();
    fs::create_directories(labels_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: create labels folder.");
        return;
    }

    const std::string stem = BuildCollectSampleStem(attempt.sample_id);
    const fs::path image_path = images_dir / (stem + ".jpg");
    const fs::path label_path = labels_dir / (stem + ".txt");

    const std::vector<int> imwrite_params = {
        cv::IMWRITE_JPEG_QUALITY,
        attempt.cfg.jpeg_quality
    };

    bool image_ok = false;
    try
    {
        image_ok = cv::imwrite(image_path.string(), save_frame, imwrite_params);
    }
    catch (...)
    {
        image_ok = false;
    }

    if (!image_ok)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: image write.");
        return;
    }

    bool label_ok = true;
    std::string label_result = "auto-label disabled";
    if (attempt.cfg.auto_label_data)
    {
        const std::set<int> allowed = ParseRecordClasses(attempt.cfg.auto_label_record_classes.c_str());
        const std::set<int>* allowed_ptr = allowed.empty() ? nullptr : &allowed;
        label_result = WriteYoloLabelFile(
            label_path,
            boxes,
            classes,
            confidences,
            save_frame.cols,
            save_frame.rows,
            attempt.cfg.auto_label_min_conf,
            attempt.cfg.auto_label_max_boxes,
            allowed_ptr);
        label_ok = (label_result != "label open failed");
    }

    {
        std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
        g_collectRuntimeState.saved_image_count += 1;
        if (attempt.cfg.auto_label_data && label_ok)
            g_collectRuntimeState.saved_label_count += 1;
        g_collectRuntimeState.last_output_dir = model_root.string();
        g_collectRuntimeState.last_status = attempt.cfg.auto_label_data
            ? ("Saved image + " + label_result)
            : "Saved image only";
    }
}

}  // namespace

// 解析数据收集的输出目录路径
// 若 output_dir_raw 为空或相对路径，则在 root_dir 下创建 data_collector/Collected_data 目录
// 若 output_dir_raw 为绝对路径，则直接使用
std::filesystem::path ResolveCollectOutputDir(const std::string& root_dir, const char* output_dir_raw)
{
    const std::string cleaned = OtherTools::TrimAscii(output_dir_raw ? std::string(output_dir_raw) : std::string());
    if (cleaned.empty())
    {
        const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
        return fs::path(base_dir) / "data_collector" / "Collected_data";
    }

    fs::path out(cleaned);
    if (out.is_absolute())
        return out;

    const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
    return fs::path(base_dir) / out;
}

// 检查数据收集功能是否启用
bool IsDataCollectionEnabled(const Config& cfg)
{
    return cfg.collect_data_while_playing;
}

// 获取数据收集的 UI 状态信息
// 包括：启用状态、输出目录路径、帧计数、样本计数、保存统计和状态信息
DataCollectionUiState GetDataCollectionUiState(const std::string& root_dir, const char* model_name, const Config& cfg)
{
    DataCollectionUiState ui;
    ui.enabled = IsDataCollectionEnabled(cfg);

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    const fs::path model_root = ResolveCollectOutputDir(root_dir, snapshot.output_dir.c_str()) / ModelNameToFolder(model_name);
    ui.resolved_output_dir = model_root.string();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    ui.observed_frame_count = g_collectRuntimeState.frame_counter;
    ui.attempted_sample_count = g_collectRuntimeState.sample_counter;
    ui.saved_image_count = g_collectRuntimeState.saved_image_count;
    ui.saved_label_count = g_collectRuntimeState.saved_label_count;
    ui.status = g_collectRuntimeState.last_status;
    return ui;
}

// 重置数据收集的运行时计数器和状态
void ResetDataCollectionRuntime()
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    g_collectRuntimeState = {};
    g_collectRuntimeState.last_status = "Counters reset.";
}

// 主数据收集入口函数（CPU 路径）
// 检查条件后尝试收集一帧数据，包括保存截图和检测标签
//   - root_dir：项目根目录
//   - model_name：当前使用的模型名称
//   - frame：当前帧图像
//   - boxes/classes/confidences：检测结果
//   - aimbot_enabled：自瞄是否运行中
//   - cfg：全局配置
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::Mat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0)
        return;

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    SaveCollectedFrame(
        root_dir,
        model_name,
        frame,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}

#ifdef USE_CUDA
// 主数据收集入口函数（CUDA GPU 路径）
// 与 CPU 版本类似，但先从 GPU 下载帧到 CPU 再保存
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::cuda::GpuMat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty())
        return;

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    cv::Mat downloaded;
    try
    {
        frame.download(downloaded);
    }
    catch (...)
    {
        UpdateRuntimeStatus("", "Collect save failed: GPU download.");
        return;
    }

    SaveCollectedFrame(
        root_dir,
        model_name,
        downloaded,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}
#endif

}  // namespace cvm
