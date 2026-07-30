#include "detector/postprocess.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace detector::detail {
namespace {

constexpr int64_t kMinChannelFirstFeatures = 5;       // 4 个框坐标 + 至少 1 类
constexpr int64_t kMinAnchorFirstFeatures = 6;        // 4 个框坐标 + obj + 至少 1 类
constexpr int64_t kEndToEndFeatures = 6;              // xyxy + score + class
constexpr int64_t kTypicalEndToEndMaxDetections = 1024;

bool valid_shape(const std::vector<int64_t>& shape) noexcept {
    return shape.size() == 3 && shape[0] == 1 &&
           shape[1] > 0 && shape[2] > 0;
}

bool finite_box(float x1, float y1, float x2, float y2) noexcept {
    return std::isfinite(x1) && std::isfinite(y1) &&
           std::isfinite(x2) && std::isfinite(y2);
}

float intersection_over_union(const Detection& a,
                              const Detection& b) noexcept {
    const float left = std::max(a.x1, b.x1);
    const float top = std::max(a.y1, b.y1);
    const float right = std::min(a.x2, b.x2);
    const float bottom = std::min(a.y2, b.y2);
    const float width = std::max(0.0f, right - left);
    const float height = std::max(0.0f, bottom - top);
    const float intersection = width * height;
    const float area_a = std::max(0.0f, a.x2 - a.x1) *
                         std::max(0.0f, a.y2 - a.y1);
    const float area_b = std::max(0.0f, b.x2 - b.x1) *
                         std::max(0.0f, b.y2 - b.y1);
    const float denominator = area_a + area_b - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

void append_xywh(float cx, float cy, float width, float height,
                 float confidence, int class_id,
                 std::vector<Detection>& dets) {
    if (!std::isfinite(confidence) || confidence < 0.0f ||
        !finite_box(cx, cy, width, height) || width <= 0.0f || height <= 0.0f) {
        return;
    }

    Detection detection;
    detection.x1 = cx - width * 0.5f;
    detection.y1 = cy - height * 0.5f;
    detection.x2 = cx + width * 0.5f;
    detection.y2 = cy + height * 0.5f;
    detection.confidence = confidence;
    detection.class_id = class_id;
    dets.push_back(detection);
}

bool decode_channel_first(const float* data,
                          int64_t features,
                          int64_t anchors,
                          float conf_threshold,
                          std::vector<Detection>& dets) {
    const int64_t class_count = features - 4;
    if (class_count <= 0) return false;

    dets.reserve(dets.size() + static_cast<size_t>(anchors));
    for (int64_t anchor = 0; anchor < anchors; ++anchor) {
        float best_confidence = -std::numeric_limits<float>::infinity();
        int best_class = -1;
        for (int64_t class_index = 0; class_index < class_count; ++class_index) {
            // [B, 4+C, A] 是按 feature 平面连续存储，不能把单个 anchor
            // 误当成连续的 4+C 个 float。
            const float confidence = data[(4 + class_index) * anchors + anchor];
            if (std::isfinite(confidence) && confidence > best_confidence) {
                best_confidence = confidence;
                best_class = static_cast<int>(class_index);
            }
        }
        if (best_class < 0 || best_confidence < conf_threshold) continue;

        append_xywh(data[anchor],
                    data[anchors + anchor],
                    data[2 * anchors + anchor],
                    data[3 * anchors + anchor],
                    best_confidence,
                    best_class,
                    dets);
    }
    return true;
}

bool decode_anchor_first_objectness(const float* data,
                                    int64_t anchors,
                                    int64_t features,
                                    float conf_threshold,
                                    std::vector<Detection>& dets) {
    const int64_t class_count = features - 5;
    if (class_count <= 0) return false;

    dets.reserve(dets.size() + static_cast<size_t>(anchors));
    for (int64_t anchor = 0; anchor < anchors; ++anchor) {
        const float* row = data + anchor * features;
        const float objectness = row[4];
        if (!std::isfinite(objectness) || objectness <= 0.0f) continue;

        float best_probability = -std::numeric_limits<float>::infinity();
        int best_class = -1;
        for (int64_t class_index = 0; class_index < class_count; ++class_index) {
            const float probability = row[5 + class_index];
            if (std::isfinite(probability) && probability > best_probability) {
                best_probability = probability;
                best_class = static_cast<int>(class_index);
            }
        }
        if (best_class < 0) continue;

        const float confidence = objectness * best_probability;
        if (!std::isfinite(confidence) || confidence < conf_threshold) continue;
        append_xywh(row[0], row[1], row[2], row[3],
                    confidence, best_class, dets);
    }
    return true;
}

bool decode_end_to_end(const float* data,
                       int64_t detections,
                       float conf_threshold,
                       std::vector<Detection>& dets) {
    dets.reserve(dets.size() + static_cast<size_t>(detections));
    for (int64_t index = 0; index < detections; ++index) {
        const float* row = data + index * kEndToEndFeatures;
        const float confidence = row[4];
        const float class_value = row[5];
        if (!std::isfinite(confidence) || confidence < conf_threshold ||
            !std::isfinite(class_value) || class_value < 0.0f ||
            class_value > static_cast<float>(std::numeric_limits<int>::max()) ||
            !finite_box(row[0], row[1], row[2], row[3]) ||
            row[2] <= row[0] || row[3] <= row[1]) {
            continue;
        }

        Detection detection;
        detection.x1 = row[0];
        detection.y1 = row[1];
        detection.x2 = row[2];
        detection.y2 = row[3];
        detection.confidence = confidence;
        detection.class_id = static_cast<int>(class_value);
        dets.push_back(detection);
    }
    return true;
}

} // namespace

bool resolve_output_format(const std::vector<int64_t>& shape,
                           OutputFormat requested,
                           OutputFormat& resolved) noexcept {
    if (!valid_shape(shape)) return false;

    const int64_t rows = shape[1];
    const int64_t columns = shape[2];
    const auto matches = [&](OutputFormat format) {
        switch (format) {
            case OutputFormat::CHANNEL_FIRST:
                return rows >= kMinChannelFirstFeatures && columns > rows;
            case OutputFormat::ANCHOR_FIRST_OBJECTNESS:
                return columns >= kMinAnchorFirstFeatures && rows > columns;
            case OutputFormat::END_TO_END:
                return columns == kEndToEndFeatures;
            case OutputFormat::AUTO:
                return false;
        }
        return false;
    };

    if (requested != OutputFormat::AUTO) {
        if (!matches(requested)) return false;
        resolved = requested;
        return true;
    }

    // [B,N,6] 与单类别 YOLOv5 的 [B,A,6] 形状相同。官方端到端头
    // 默认最多数百条结果，而 raw head 通常有数千 anchors；超过保守上限时
    // 识别为 objectness 契约，边界情况要求调用方显式指定。
    if (columns == kEndToEndFeatures) {
        resolved = rows <= kTypicalEndToEndMaxDetections
            ? OutputFormat::END_TO_END
            : OutputFormat::ANCHOR_FIRST_OBJECTNESS;
        return true;
    }
    if (matches(OutputFormat::CHANNEL_FIRST)) {
        resolved = OutputFormat::CHANNEL_FIRST;
        return true;
    }
    if (matches(OutputFormat::ANCHOR_FIRST_OBJECTNESS)) {
        resolved = OutputFormat::ANCHOR_FIRST_OBJECTNESS;
        return true;
    }
    return false;
}

bool decode_output(const float* data,
                   const std::vector<int64_t>& shape,
                   OutputFormat format,
                   float conf_threshold,
                   std::vector<Detection>& dets) noexcept {
    if (!data || !valid_shape(shape) ||
        !std::isfinite(conf_threshold) || conf_threshold < 0.0f ||
        conf_threshold > 1.0f) {
        return false;
    }

    try {
        switch (format) {
            case OutputFormat::CHANNEL_FIRST:
                return decode_channel_first(data, shape[1], shape[2],
                                            conf_threshold, dets);
            case OutputFormat::ANCHOR_FIRST_OBJECTNESS:
                return decode_anchor_first_objectness(data, shape[1], shape[2],
                                                      conf_threshold, dets);
            case OutputFormat::END_TO_END:
                if (shape[2] != kEndToEndFeatures) return false;
                return decode_end_to_end(data, shape[1], conf_threshold, dets);
            case OutputFormat::AUTO:
                return false;
        }
    } catch (...) {
        dets.clear();
        return false;
    }
    return false;
}

void nms(std::vector<Detection>& dets,
         float nms_threshold,
         int top_k) noexcept {
    if (dets.empty()) return;
    if (!std::isfinite(nms_threshold) || nms_threshold < 0.0f ||
        nms_threshold > 1.0f || top_k <= 0) {
        dets.clear();
        return;
    }

    try {
        std::sort(dets.begin(), dets.end(),
            [](const Detection& a, const Detection& b) {
                return a.confidence > b.confidence;
            });

        std::vector<bool> suppressed(dets.size(), false);
        std::vector<Detection> output;
        output.reserve(std::min(dets.size(), static_cast<size_t>(top_k)));
        for (size_t i = 0; i < dets.size(); ++i) {
            if (suppressed[i]) continue;
            output.push_back(dets[i]);
            if (static_cast<int>(output.size()) >= top_k) break;

            for (size_t j = i + 1; j < dets.size(); ++j) {
                if (!suppressed[j] && dets[j].class_id == dets[i].class_id &&
                    intersection_over_union(dets[i], dets[j]) > nms_threshold) {
                    suppressed[j] = true;
                }
            }
        }
        dets = std::move(output);
    } catch (...) {
        dets.clear();
    }
}

void scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info) noexcept {
    if (!(info.scale > 0.0f) || !std::isfinite(info.scale) ||
        info.orig_w <= 0 || info.orig_h <= 0) {
        dets.clear();
        return;
    }

    const float max_x = static_cast<float>(info.orig_w);
    const float max_y = static_cast<float>(info.orig_h);
    const float inverse_scale = 1.0f / info.scale;
    for (auto& detection : dets) {
        detection.x1 = std::clamp(
            (detection.x1 - info.pad_x) * inverse_scale, 0.0f, max_x);
        detection.y1 = std::clamp(
            (detection.y1 - info.pad_y) * inverse_scale, 0.0f, max_y);
        detection.x2 = std::clamp(
            (detection.x2 - info.pad_x) * inverse_scale, 0.0f, max_x);
        detection.y2 = std::clamp(
            (detection.y2 - info.pad_y) * inverse_scale, 0.0f, max_y);
    }

    dets.erase(std::remove_if(dets.begin(), dets.end(),
        [](const Detection& detection) {
            return !finite_box(detection.x1, detection.y1,
                               detection.x2, detection.y2) ||
                   detection.x2 <= detection.x1 ||
                   detection.y2 <= detection.y1;
        }), dets.end());
}

} // namespace detector::detail
