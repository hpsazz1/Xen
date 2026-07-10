// postProcess.cpp - AI 检测后处理
// 提供 YOLO 系列模型输出结果的解码、过滤和 NMS（非极大值抑制）功能。
// 支持多种输出格式：xyxy（左上右下坐标）、feature-major（特征主导）和 prediction-major（预测主导）布局。
// 通过自动检测输出格式来兼容不同的模型导出方式。

#include <algorithm>
#include <numeric>
#include <chrono>
#include <limits>
#include <cmath>

#include "postProcess.h"
#include "Xen.h"
#ifdef USE_CUDA
#include "trt_detector.h"
#endif

namespace
{
    /**
     * RunBoundedNms - 带数量限制的 NMS 执行
     * 在 NMS 之前限制候选检测数量（硬上限 1000），NMS 后再按置信度保留前 maxDetections 个。
     * @param detections     检测结果列表（会被原地修改）
     * @param nmsThreshold   NMS 的 IoU 阈值
     * @param maxDetections  最大保留检测数
     * @param nmsTime        [输出] NMS 耗时统计（可选）
     */
    void RunBoundedNms(
        std::vector<Detection>& detections,
        float nmsThreshold,
        int maxDetections,
        std::chrono::duration<double, std::milli>* nmsTime);

    /**
     * TryPositiveInt64ToInt - 安全地将正 int64_t 转换为 int
     * @param value int64_t 值（必须为正数）
     * @param out   [输出] 转换后的 int
     * @return 转换成功返回 true
     */
    bool TryPositiveInt64ToInt(int64_t value, int* out)
    {
        if (!out || value <= 0 ||
            value > static_cast<int64_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        *out = static_cast<int>(value);
        return true;
    }

    /**
     * ExtractRowsCols - 从张量形状中提取行数和列数（最后两个维度）
     * @param shape 张量形状向量
     * @param rows  [输出] 行数
     * @param cols  [输出] 列数
     * @return 提取成功返回 true
     */
    bool ExtractRowsCols(const std::vector<int64_t>& shape, int* rows, int* cols)
    {
        if (shape.size() < 2)
            return false;

        return TryPositiveInt64ToInt(shape[shape.size() - 2], rows) &&
            TryPositiveInt64ToInt(shape[shape.size() - 1], cols);
    }

    /**
     * ConfiguredClassCountHint - 根据配置的类别 ID 推断类别总数
     * @return 类别总数（最大类别 ID + 1），超出有效范围返回 0
     */
    int ConfiguredClassCountHint()
    {
        const int maxClassId = std::max(config.class_player, config.class_head);
        if (maxClassId < 0 || maxClassId > 9999)
            return 0;

        return maxClassId + 1;
    }

    /**
     * TryResolveClassLayout - 尝试解析输出张量的类别布局
     *
     * 根据张量的特征维度（extent）和类别数提示（numClassesHint）来判断：
     * - 是否包含 objectness（置信度）分数
     * - 实际的类别数量
     *
     * 布局规则:
     *   extent = 5 + numClasses: 包含 objectness 分数（[cx, cy, w, h, objectness, class1, class2, ...]）
     *   extent = 4 + numClasses: 无 objectness（[cx, cy, w, h, class1, class2, ...]）
     *
     * @param extent            特征维度大小
     * @param numClassesHint    类别数提示（来自模型配置）
     * @param preferObjectness  是否优先使用 objectness 布局
     * @param numClasses        [输出] 解析出的类别数
     * @param usesObjectness    [输出] 是否包含 objectness
     * @return 解析成功返回 true
     */
    bool TryResolveClassLayout(
        int extent,
        int numClassesHint,
        bool preferObjectness,
        int* numClasses,
        bool* usesObjectness)
    {
        if (!numClasses || !usesObjectness || extent <= 4)
            return false;

        auto tryHint = [&](int hint) -> bool
        {
            if (hint <= 0 || hint > 10000)
                return false;

            if (extent == 5 + hint)
            {
                *numClasses = hint;
                *usesObjectness = true;
                return true;
            }

            if (extent == 4 + hint)
            {
                *numClasses = hint;
                *usesObjectness = false;
                return true;
            }

            return false;
        };

        // 先用模型配置的类别数进行匹配
        if (tryHint(numClassesHint))
            return true;

        // 然后用配置文件中定义的类别数进行匹配
        if (tryHint(ConfiguredClassCountHint()))
            return true;

        // 回退方式：根据 extent 自动推断
        if (preferObjectness && extent > 5)
        {
            *numClasses = extent - 5;
            *usesObjectness = true;
            return *numClasses > 0;
        }

        *numClasses = extent - 4;
        *usesObjectness = false;
        return *numClasses > 0;
    }

    /**
     * LooksLikeXyxyDetections - 判断输出是否看起来像 xyxy 格式的检测结果
     *
     * xyxy 格式的行布局：[x1, y1, x2, y2, confidence, classId]
     * 通过采样检查来验证：
     * - x2 >= x1 且 y2 >= y1（有效的左上右下坐标关系）
     * - classId 为整数（或接近整数）
     *
     * @param output 输出数据指针
     * @param rows   检测行数
     * @param cols   每行列数（应为 6）
     * @return 如果看起来像 xyxy 格式返回 true
     */
    bool LooksLikeXyxyDetections(const float* output, int rows, int cols)
    {
        if (!output || rows <= 0 || cols != 6)
            return false;

        const int maxSamples = std::min(rows, 256);
        const int step = std::max(1, rows / maxSamples);
        int considered = 0;
        int validXyxy = 0;
        int integerClassId = 0;

        for (int i = 0; i < rows && considered < maxSamples; i += step)
        {
            const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
            const float x1 = det[0];
            const float y1 = det[1];
            const float x2 = det[2];
            const float y2 = det[3];
            const float confidence = det[4];
            const float classId = det[5];

            if (!std::isfinite(x1) || !std::isfinite(y1) ||
                !std::isfinite(x2) || !std::isfinite(y2) ||
                !std::isfinite(confidence) || !std::isfinite(classId))
            {
                continue;
            }

            ++considered;

            if (x2 >= x1 && y2 >= y1)
                ++validXyxy;

            const float roundedClassId = std::round(classId);
            if (classId >= -1.0f &&
                classId <= 10000.0f &&
                std::fabs(classId - roundedClassId) <= 1e-3f)
            {
                ++integerClassId;
            }
        }

        if (considered == 0)
            return false;

        // 至少 2/3 的样本满足条件，且 3/4 的 classId 为整数
        return validXyxy * 3 >= considered * 2 &&
            integerClassId * 4 >= considered * 3;
    }

    /**
     * AddXyxyDetection - 添加 xyxy 格式的检测结果到列表
     *
     * 将左上右下坐标转换为 OpenCV Rect 格式，并应用缩放因子。
     * 如果置信度过低或框尺寸无效则跳过。
     *
     * @param detections    检测结果列表
     * @param x1,y1         左上角坐标
     * @param x2,y2         右下角坐标
     * @param confidence    置信度
     * @param classId       类别 ID
     * @param scale         坐标缩放因子（用于从模型输出空间映射到检测分辨率空间）
     * @param confThreshold 置信度阈值
     */
    void AddXyxyDetection(
        std::vector<Detection>& detections,
        float x1,
        float y1,
        float x2,
        float y2,
        float confidence,
        int classId,
        float scale,
        float confThreshold)
    {
        if (confidence <= confThreshold || x2 <= x1 || y2 <= y1)
            return;

        cv::Rect box;
        box.x = static_cast<int>(x1 * scale);
        box.y = static_cast<int>(y1 * scale);
        box.width = static_cast<int>((x2 - x1) * scale);
        box.height = static_cast<int>((y2 - y1) * scale);

        if (box.width <= 0 || box.height <= 0)
            return;

        detections.push_back(Detection{ box, confidence, classId });
    }

    /**
     * AddCxcywhDetection - 添加中心点+宽高格式的检测结果到列表
     *
     * 将 (cx, cy, width, height) 格式转换为 OpenCV Rect 格式，并应用缩放因子。
     *
     * @param detections    检测结果列表
     * @param cx,cy         中心点坐标
     * @param width,height  框的宽高
     * @param confidence    置信度
     * @param classId       类别 ID
     * @param scale         坐标缩放因子
     * @param confThreshold 置信度阈值
     */
    void AddCxcywhDetection(
        std::vector<Detection>& detections,
        float cx,
        float cy,
        float width,
        float height,
        float confidence,
        int classId,
        float scale,
        float confThreshold)
    {
        if (confidence <= confThreshold || width <= 0.0f || height <= 0.0f)
            return;

        const float halfWidth = 0.5f * width;
        const float halfHeight = 0.5f * height;

        cv::Rect box;
        box.x = static_cast<int>((cx - halfWidth) * scale);
        box.y = static_cast<int>((cy - halfHeight) * scale);
        box.width = static_cast<int>(width * scale);
        box.height = static_cast<int>(height * scale);

        if (box.width <= 0 || box.height <= 0)
            return;

        detections.push_back(Detection{ box, confidence, classId });
    }

    /**
     * DecodeXyxyDetections - 解码 xyxy 格式的检测结果
     * 遍历每一行，每行格式为 [x1, y1, x2, y2, confidence, classId]
     * @param output         输出数据指针
     * @param rows           行数
     * @param cols           列数（应为 6）
     * @param confThreshold  置信度阈值
     * @param scale          缩放因子
     * @param detections     [输出] 检测结果列表
     */
    void DecodeXyxyDetections(
        const float* output,
        int rows,
        int cols,
        float confThreshold,
        float scale,
        std::vector<Detection>& detections)
    {
        detections.reserve(detections.size() + static_cast<size_t>(rows));

        for (int i = 0; i < rows; ++i)
        {
            const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
            AddXyxyDetection(
                detections,
                det[0],
                det[1],
                det[2],
                det[3],
                det[4],
                static_cast<int>(std::round(det[5])),
                scale,
                confThreshold);
        }
    }

    /**
     * DecodeFeatureMajorPredictions - 解码特征主导（feature-major）布局的预测
     *
     * 特征主导布局：每个特征是连续存储的一行，跨所有预测位置。
     * 例如 rows=特征数(=4+numClasses), cols=预测位置数。
     * 按列遍历，每列对应一个预测：cx[row0][i], cy[row1][i], w[row2][i], h[row3][i], ...
     *
     * @param output         输出数据指针
     * @param rows           行数（特征维度）
     * @param cols           列数（预测位置数）
     * @param numClasses     类别数
     * @param usesObjectness 是否包含 objectness 分数
     * @param confThreshold  置信度阈值
     * @param scale          缩放因子
     * @param detections     [输出] 检测结果列表
     */
    void DecodeFeatureMajorPredictions(
        const float* output,
        int rows,
        int cols,
        int numClasses,
        bool usesObjectness,
        float confThreshold,
        float scale,
        std::vector<Detection>& detections)
    {
        const int classBase = usesObjectness ? 5 : 4;
        if (!output || rows < classBase + numClasses || cols <= 0 || numClasses <= 0)
            return;

        detections.reserve(detections.size() + 256);

        for (int i = 0; i < cols; ++i)
        {
            const float cx = output[0 * cols + i];
            const float cy = output[1 * cols + i];
            const float ow = output[2 * cols + i];
            const float oh = output[3 * cols + i];
            const float objectness = usesObjectness ? output[4 * cols + i] : 1.0f;

            // 找类别中得分最高的
            float maxClassScore = 0.0f;
            int maxClassId = 0;
            for (int c = 0; c < numClasses; ++c)
            {
                const float score = output[(classBase + c) * cols + i];
                if (score > maxClassScore)
                {
                    maxClassScore = score;
                    maxClassId = c;
                }
            }

            // 最终置信度 = objectness * maxClassScore
            AddCxcywhDetection(
                detections,
                cx,
                cy,
                ow,
                oh,
                objectness * maxClassScore,
                maxClassId,
                scale,
                confThreshold);
        }
    }

    /**
     * DecodePredictionMajorPredictions - 解码预测主导（prediction-major）布局
     *
     * 预测主导布局：每个预测行包含所有特征（cx, cy, w, h, [objectness], class1, class2, ...）。
     * 每行对应一个预测，行遍历即可得到所有检测。
     *
     * @param output         输出数据指针
     * @param rows           行数（预测数）
     * @param cols           列数（每行的特征数）
     * @param numClasses     类别数
     * @param usesObjectness 是否包含 objectness 分数
     * @param confThreshold  置信度阈值
     * @param scale          缩放因子
     * @param detections     [输出] 检测结果列表
     */
    void DecodePredictionMajorPredictions(
        const float* output,
        int rows,
        int cols,
        int numClasses,
        bool usesObjectness,
        float confThreshold,
        float scale,
        std::vector<Detection>& detections)
    {
        const int classBase = usesObjectness ? 5 : 4;
        if (!output || cols < classBase + numClasses || rows <= 0 || numClasses <= 0)
            return;

        detections.reserve(detections.size() + 256);

        for (int i = 0; i < rows; ++i)
        {
            const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
            const float objectness = usesObjectness ? det[4] : 1.0f;

            // 找类别中得分最高的
            float maxClassScore = 0.0f;
            int maxClassId = 0;
            for (int c = 0; c < numClasses; ++c)
            {
                const float score = det[classBase + c];
                if (score > maxClassScore)
                {
                    maxClassScore = score;
                    maxClassId = c;
                }
            }

            AddCxcywhDetection(
                detections,
                det[0],
                det[1],
                det[2],
                det[3],
                objectness * maxClassScore,
                maxClassId,
                scale,
                confThreshold);
        }
    }

    /**
     * DecodeYoloOutput - YOLO 输出解码主函数
     *
     * 自动检测输出格式并选择相应的解码策略：
     * 1. 如果 cols == 6 且 LooksLikeXyxyDetections 为真 -> xyxy 格式解码
     * 2. 如果 rows <= cols（特征较多） -> feature-major 布局解码
     * 3. 如果 rows > cols（预测较多） -> prediction-major 布局解码
     *
     * 解码后执行带数量限制的 NMS。
     *
     * @param output         输出数据指针
     * @param shape          输出张量形状 [rows, cols]
     * @param numClassesHint 类别数提示
     * @param confThreshold  置信度阈值
     * @param nmsThreshold   NMS 的 IoU 阈值
     * @param maxDetections  最大保留检测数
     * @param scale          坐标缩放因子
     * @param nmsTime        [输出] NMS 耗时
     * @return 最终检测结果列表
     */
    std::vector<Detection> DecodeYoloOutput(
        const float* output,
        const std::vector<int64_t>& shape,
        int numClassesHint,
        float confThreshold,
        float nmsThreshold,
        int maxDetections,
        float scale,
        std::chrono::duration<double, std::milli>* nmsTime)
    {
        std::vector<Detection> detections;
        if (!output)
            return detections;

        int rows = 0;
        int cols = 0;
        if (!ExtractRowsCols(shape, &rows, &cols))
            return detections;

        // 自动检测输出格式
        if (cols == 6 && LooksLikeXyxyDetections(output, rows, cols))
        {
            DecodeXyxyDetections(output, rows, cols, confThreshold, scale, detections);
        }
        else if (rows <= cols)
        {
            // feature-major 布局
            int classes = 0;
            bool usesObjectness = false;
            if (TryResolveClassLayout(rows, numClassesHint, false, &classes, &usesObjectness))
            {
                DecodeFeatureMajorPredictions(
                    output,
                    rows,
                    cols,
                    classes,
                    usesObjectness,
                    confThreshold,
                    scale,
                    detections);
            }
        }
        else
        {
            // prediction-major 布局
            int classes = 0;
            bool usesObjectness = false;
            if (TryResolveClassLayout(cols, numClassesHint, true, &classes, &usesObjectness))
            {
                DecodePredictionMajorPredictions(
                    output,
                    rows,
                    cols,
                    classes,
                    usesObjectness,
                    confThreshold,
                    scale,
                    detections);
            }
        }

        RunBoundedNms(detections, nmsThreshold, maxDetections, nmsTime);
        return detections;
    }

    /**
     * SortDetectionsByConfidence - 按置信度降序排序检测结果
     * @param detections 检测结果列表
     */
    void SortDetectionsByConfidence(std::vector<Detection>& detections)
    {
        std::sort(
            detections.begin(),
            detections.end(),
            [](const Detection& a, const Detection& b)
            {
                return a.confidence > b.confidence;
            });
    }

    /**
     * LimitDetectionsByConfidence - 按置信度保留前 limit 个检测结果
     * 使用 std::nth_element 进行部分排序以提高效率。
     * @param detections 检测结果列表（会被原地截断）
     * @param limit      保留的最大数量
     */
    void LimitDetectionsByConfidence(std::vector<Detection>& detections, size_t limit)
    {
        if (limit == 0 || detections.size() <= limit)
            return;

        const auto kth = detections.begin() + static_cast<std::vector<Detection>::difference_type>(limit);
        std::nth_element(
            detections.begin(),
            kth,
            detections.end(),
            [](const Detection& a, const Detection& b)
            {
                return a.confidence > b.confidence;
            });
        detections.resize(limit);
    }

    /**
     * RunBoundedNms - 带数量限制的 NMS 执行
     *
     * 执行流程：
     * 1. 将候选检测数限制在硬上限 1000 以内（防止 NMS 性能退化）
     * 2. 执行标准 NMS
     * 3. 按 maxDetections 截断并排序
     *
     * @param detections     检测结果列表
     * @param nmsThreshold   NMS IoU 阈值
     * @param maxDetections  最大保留数
     * @param nmsTime        [输出] NMS 耗时
     */
    void RunBoundedNms(
        std::vector<Detection>& detections,
        float nmsThreshold,
        int maxDetections,
        std::chrono::duration<double, std::milli>* nmsTime)
    {
        constexpr size_t kPreNmsHardLimit = 1000;

        size_t preNmsLimit = kPreNmsHardLimit;
        if (maxDetections > 0)
        {
            const size_t requested = static_cast<size_t>(maxDetections);
            preNmsLimit = std::min(kPreNmsHardLimit, std::max(requested, requested * 8));
        }

        LimitDetectionsByConfidence(detections, preNmsLimit);
        NMS(detections, nmsThreshold, nmsTime);

        if (maxDetections > 0)
            LimitDetectionsByConfidence(detections, static_cast<size_t>(maxDetections));

        SortDetectionsByConfidence(detections);
    }
} // anonymous namespace

/**
 * NMS - 非极大值抑制
 *
 * 标准的 NMS 算法实现：
 * 1. 按置信度降序排序检测结果
 * 2. 从最高置信度开始，保留当前检测框
 * 3. 计算保留框与其余框的 IoU（交并比）
 * 4. 如果 IoU 超过阈值则抑制（删除）该框
 *
 * 如果 nmsThreshold <= 0，则跳过 NMS 直接返回。
 *
 * @param detections   检测结果列表（会被原地修改，保留 NMS 后的结果）
 * @param nmsThreshold IoU 阈值，范围 [0, 1]，越高越允许重叠
 * @param nmsTime      [输出] NMS 执行耗时（可选）
 */
void NMS(std::vector<Detection>& detections, float nmsThreshold, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (detections.empty()) return;

    if (nmsThreshold <= 0.0f)
    {
        if (nmsTime)
        {
            *nmsTime = std::chrono::duration<double, std::milli>(0);
        }
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    SortDetectionsByConfidence(detections);

    std::vector<bool> suppress(detections.size(), false);
    std::vector<Detection> result;
    result.reserve(detections.size());

    for (size_t i = 0; i < detections.size(); ++i)
    {
        if (suppress[i]) continue;

        result.push_back(detections[i]);

        const cv::Rect& box_i = detections[i].box;
        const float area_i = static_cast<float>(box_i.area());

        for (size_t j = i + 1; j < detections.size(); ++j)
        {
            if (suppress[j]) continue;

            const cv::Rect& box_j = detections[j].box;
            const cv::Rect intersection = box_i & box_j;

            if (intersection.width > 0 && intersection.height > 0)
            {
                // 计算 IoU（交集面积 / 并集面积）
                const float intersection_area = static_cast<float>(intersection.area());
                const float union_area = area_i + static_cast<float>(box_j.area()) - intersection_area;

                if (intersection_area / union_area > nmsThreshold)
                {
                    suppress[j] = true;
                }
            }
        }
    }

    detections = std::move(result);

    auto t1 = std::chrono::steady_clock::now();
    if (nmsTime)
    {
        *nmsTime = t1 - t0;
    }
}

#ifdef USE_CUDA
/**
 * postProcessYolo - TensorRT 路径的 YOLO 后处理入口
 *
 * 包装 DecodeYoloOutput 函数，使用 TrtDetector 的图像缩放因子。
 * 该函数从全局 trt_detector 获取 img_scale，确保检测框坐标
 * 从模型输入空间映射到检测分辨率空间。
 *
 * @param output         模型输出数据指针
 * @param shape          输出张量形状
 * @param numClasses     类别数
 * @param confThreshold  置信度阈值
 * @param nmsThreshold   NMS 的 IoU 阈值
 * @param maxDetections  最大检测数
 * @param nmsTime        [输出] NMS 耗时
 * @return 检测结果列表
 */
std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        trt_detector.img_scale,
        nmsTime);
}
#endif

#ifndef USE_CUDA
/**
 * postProcessYoloDML - DirectML 路径的 YOLO 后处理入口
 *
 * 包装 DecodeYoloOutput 函数，缩放因子固定为 1.0。
 * DirectML 后端在预处理阶段已将图像缩放到检测分辨率，
 * 因此后处理无需额外缩放。
 *
 * @param output         模型输出数据指针
 * @param shape          输出张量形状
 * @param numClasses     类别数
 * @param confThreshold  置信度阈值
 * @param nmsThreshold   NMS 的 IoU 阈值
 * @param maxDetections  最大检测数
 * @param nmsTime        [输出] NMS 耗时
 * @return 检测结果列表
 */
std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        1.0f,
        nmsTime);
}
#endif
