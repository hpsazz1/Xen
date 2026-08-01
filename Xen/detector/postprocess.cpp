#include "detector/postprocess.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace detector::detail {
namespace {

constexpr int64_t kMinChannelFirstFeatures = 5;       // 4 个框坐标 + 至少 1 类
constexpr int64_t kMinAnchorFirstFeatures = 6;        // 4 个框坐标 + obj + 至少 1 类
constexpr int64_t kEndToEndFeatures = 6;              // xyxy + score + class

bool valid_shape(const std::vector<int64_t>& shape) noexcept {
    return shape.size() == 3 && shape[0] == 1 &&
           shape[1] > 0 && shape[2] > 0;
}

bool finite_box(float x1, float y1, float x2, float y2) noexcept {
    return std::isfinite(x1) && std::isfinite(y1) &&
           std::isfinite(x2) && std::isfinite(y2);
}

bool make_xywh_detection(float cx, float cy, float width, float height,
                         float confidence, int class_id,
                         Detection& detection) noexcept {
    if (!std::isfinite(confidence) || confidence < 0.0f ||
        !finite_box(cx, cy, width, height) ||
        width <= 0.0f || height <= 0.0f) {
        return false;
    }

    detection.x1 = cx - width * 0.5f;
    detection.y1 = cy - height * 0.5f;
    detection.x2 = cx + width * 0.5f;
    detection.y2 = cy + height * 0.5f;
    detection.confidence = confidence;
    detection.class_id = class_id;
    return true;
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
    Detection detection;
    if (make_xywh_detection(cx, cy, width, height,
                            confidence, class_id, detection)) {
        dets.push_back(detection);
    }
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

void nms_into(std::vector<Detection>& candidates,
              float nms_threshold,
              int top_k,
              std::vector<Detection>& output,
              std::vector<unsigned char>& suppressed) {
    output.clear();
    std::sort(candidates.begin(), candidates.end(),
        [](const Detection& a, const Detection& b) {
            return a.confidence > b.confidence;
        });

    suppressed.resize(candidates.size());
    std::fill(suppressed.begin(), suppressed.end(), 0U);
    output.reserve(std::min(
        candidates.size(), static_cast<size_t>(top_k)));
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i] != 0U) continue;
        output.push_back(candidates[i]);
        if (static_cast<int>(output.size()) >= top_k) break;

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j] == 0U &&
                candidates[j].class_id == candidates[i].class_id &&
                intersection_over_union(candidates[i], candidates[j]) >
                    nms_threshold) {
                suppressed[j] = 1U;
            }
        }
    }
}

void nms_segmentations_into(
        std::vector<SegmentationCandidate>& candidates,
        float nms_threshold,
        int top_k,
        std::vector<SegmentationCandidate>& output,
        std::vector<unsigned char>& suppressed) {
    output.clear();
    std::sort(candidates.begin(), candidates.end(),
        [](const SegmentationCandidate& a,
           const SegmentationCandidate& b) {
            return a.detection.confidence > b.detection.confidence;
        });

    suppressed.resize(candidates.size());
    std::fill(suppressed.begin(), suppressed.end(), 0U);
    output.reserve(std::min(
        candidates.size(), static_cast<size_t>(top_k)));
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i] != 0U) continue;
        output.push_back(candidates[i]);
        if (static_cast<int>(output.size()) >= top_k) break;

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j] == 0U &&
                candidates[j].detection.class_id ==
                    candidates[i].detection.class_id &&
                intersection_over_union(
                    candidates[i].detection,
                    candidates[j].detection) > nms_threshold) {
                suppressed[j] = 1U;
            }
        }
    }
}

void nms_poses_into(
        std::vector<PoseCandidate>& candidates,
        float nms_threshold,
        int top_k,
        std::vector<PoseCandidate>& output,
        std::vector<unsigned char>& suppressed) {
    output.clear();
    std::sort(candidates.begin(), candidates.end(),
        [](const PoseCandidate& a, const PoseCandidate& b) {
            return a.detection.confidence > b.detection.confidence;
        });

    suppressed.resize(candidates.size());
    std::fill(suppressed.begin(), suppressed.end(), 0U);
    output.reserve(std::min(
        candidates.size(), static_cast<size_t>(top_k)));
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i] != 0U) continue;
        output.push_back(candidates[i]);
        if (static_cast<int>(output.size()) >= top_k) break;

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (suppressed[j] == 0U &&
                candidates[j].detection.class_id ==
                    candidates[i].detection.class_id &&
                intersection_over_union(
                    candidates[i].detection,
                    candidates[j].detection) > nms_threshold) {
                suppressed[j] = 1U;
            }
        }
    }
}

void nms_obbs_into(
        std::vector<ObbCandidate>& candidates,
        float nms_threshold,
        int top_k,
        std::vector<ObbCandidate>& output,
        std::vector<unsigned char>& suppressed) {
    output.clear();
    std::sort(candidates.begin(), candidates.end(),
        [](const ObbCandidate& a, const ObbCandidate& b) {
            return a.detection.confidence > b.detection.confidence;
        });

    // Ultralytics OBB 使用 Fast-NMS：只要任一更高分同类框达到阈值即
    // 抑制当前框，即使那个更高分框自身随后也被抑制。不能改成贪心 NMS。
    suppressed.resize(candidates.size());
    std::fill(suppressed.begin(), suppressed.end(), 0U);
    output.reserve(std::min(
        candidates.size(), static_cast<size_t>(top_k)));
    for (size_t current = 0; current < candidates.size(); ++current) {
        for (size_t higher = 0; higher < current; ++higher) {
            if (candidates[higher].detection.class_id ==
                    candidates[current].detection.class_id &&
                probabilistic_iou(
                    candidates[higher].detection,
                    candidates[current].detection) >= nms_threshold) {
                suppressed[current] = 1U;
                break;
            }
        }
        if (suppressed[current] == 0U) {
            output.push_back(candidates[current]);
            if (static_cast<int>(output.size()) >= top_k) break;
        }
    }
}

bool valid_letterbox(const LetterBoxInfo& info) noexcept {
    return info.scale > 0.0f && std::isfinite(info.scale) &&
           info.orig_w > 0 && info.orig_h > 0;
}

bool scale_detection(const Detection& input,
                     const LetterBoxInfo& info,
                     Detection& output) noexcept {
    if (!valid_letterbox(info)) return false;

    const float max_x = static_cast<float>(info.orig_w);
    const float max_y = static_cast<float>(info.orig_h);
    const float inverse_scale = 1.0f / info.scale;
    output = input;
    output.x1 = std::clamp(
        (input.x1 - info.pad_x) * inverse_scale, 0.0f, max_x);
    output.y1 = std::clamp(
        (input.y1 - info.pad_y) * inverse_scale, 0.0f, max_y);
    output.x2 = std::clamp(
        (input.x2 - info.pad_x) * inverse_scale, 0.0f, max_x);
    output.y2 = std::clamp(
        (input.y2 - info.pad_y) * inverse_scale, 0.0f, max_y);
    return finite_box(output.x1, output.y1, output.x2, output.y2) &&
           output.x2 > output.x1 && output.y2 > output.y1;
}

bool scale_oriented_detection(
        const OrientedDetection& input,
        const LetterBoxInfo& info,
        OrientedDetection& scaled,
        Detection& envelope) noexcept {
    if (!valid_letterbox(info) ||
        !std::isfinite(input.center_x) ||
        !std::isfinite(input.center_y) ||
        !std::isfinite(input.width) ||
        !std::isfinite(input.height) ||
        !std::isfinite(input.angle_radians) ||
        input.width <= 0.0f || input.height <= 0.0f) {
        return false;
    }

    scaled = input;
    scaled.center_x = (input.center_x - info.pad_x) / info.scale;
    scaled.center_y = (input.center_y - info.pad_y) / info.scale;
    scaled.width = input.width / info.scale;
    scaled.height = input.height / info.scale;
    if (!std::isfinite(scaled.center_x) ||
        !std::isfinite(scaled.center_y) ||
        !std::isfinite(scaled.width) ||
        !std::isfinite(scaled.height) ||
        scaled.width <= 0.0f || scaled.height <= 0.0f) {
        return false;
    }

    const double cosine = std::cos(
        static_cast<double>(scaled.angle_radians));
    const double sine = std::sin(
        static_cast<double>(scaled.angle_radians));
    const double half_width =
        static_cast<double>(scaled.width) * 0.5;
    const double half_height =
        static_cast<double>(scaled.height) * 0.5;
    const double vector1_x = half_width * cosine;
    const double vector1_y = half_width * sine;
    const double vector2_x = -half_height * sine;
    const double vector2_y = half_height * cosine;
    const double center_x = scaled.center_x;
    const double center_y = scaled.center_y;
    const double x_values[4] = {
        center_x + vector1_x + vector2_x,
        center_x + vector1_x - vector2_x,
        center_x - vector1_x - vector2_x,
        center_x - vector1_x + vector2_x,
    };
    const double y_values[4] = {
        center_y + vector1_y + vector2_y,
        center_y + vector1_y - vector2_y,
        center_y - vector1_y - vector2_y,
        center_y - vector1_y + vector2_y,
    };
    const auto [min_x, max_x] = std::minmax_element(
        std::begin(x_values), std::end(x_values));
    const auto [min_y, max_y] = std::minmax_element(
        std::begin(y_values), std::end(y_values));
    envelope.x1 = std::clamp(
        static_cast<float>(*min_x), 0.0f,
        static_cast<float>(info.orig_w));
    envelope.y1 = std::clamp(
        static_cast<float>(*min_y), 0.0f,
        static_cast<float>(info.orig_h));
    envelope.x2 = std::clamp(
        static_cast<float>(*max_x), 0.0f,
        static_cast<float>(info.orig_w));
    envelope.y2 = std::clamp(
        static_cast<float>(*max_y), 0.0f,
        static_cast<float>(info.orig_h));
    envelope.confidence = scaled.confidence;
    envelope.class_id = scaled.class_id;
    return finite_box(
               envelope.x1, envelope.y1, envelope.x2, envelope.y2) &&
           envelope.x2 > envelope.x1 && envelope.y2 > envelope.y1;
}

float bilinear_sample(const std::vector<float>& values,
                      int width, int height,
                      float x, float y) noexcept {
    x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float wx = x - static_cast<float>(x0);
    const float wy = y - static_cast<float>(y0);
    const float top = values[static_cast<size_t>(y0) * width + x0] *
            (1.0f - wx) +
        values[static_cast<size_t>(y0) * width + x1] * wx;
    const float bottom = values[static_cast<size_t>(y1) * width + x0] *
            (1.0f - wx) +
        values[static_cast<size_t>(y1) * width + x1] * wx;
    return top * (1.0f - wy) + bottom * wy;
}

float bilinear_sample_binary_roi(
        const std::vector<std::uint8_t>& values,
        int full_width,
        int left,
        int top,
        int width,
        int height,
        float x,
        float y) noexcept {
    x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float wx = x - static_cast<float>(x0);
    const float wy = y - static_cast<float>(y0);
    const auto value = [&](int sample_x, int sample_y) {
        return static_cast<float>(
            values[static_cast<size_t>(top + sample_y) * full_width +
                   left + sample_x]);
    };
    const float top_value = value(x0, y0) * (1.0f - wx) +
        value(x1, y0) * wx;
    const float bottom_value = value(x0, y1) * (1.0f - wx) +
        value(x1, y1) * wx;
    return top_value * (1.0f - wy) + bottom_value * wy;
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
                return rows >= kMinChannelFirstFeatures;
            case OutputFormat::ANCHOR_FIRST_OBJECTNESS:
                return columns >= kMinAnchorFirstFeatures;
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

    // [B,N,6] 与单类别 raw head 在 shape 上完全相同，候选数量不是可靠
    // 契约。没有 metadata 或显式配置时必须失败关闭，禁止按 N 猜测。
    if (columns == kEndToEndFeatures) {
        return false;
    }
    if (rows < columns && matches(OutputFormat::CHANNEL_FIRST)) {
        resolved = OutputFormat::CHANNEL_FIRST;
        return true;
    }
    if (rows > columns && matches(OutputFormat::ANCHOR_FIRST_OBJECTNESS)) {
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

bool resolve_segmentation_contract(
        const std::vector<int64_t>& prediction_shape,
        const std::vector<int64_t>& prototype_shape,
        OutputFormat requested,
        SegmentationContract& contract) noexcept {
    contract = {};
    if (requested != OutputFormat::AUTO &&
        requested != OutputFormat::CHANNEL_FIRST) {
        return false;
    }
    if (prediction_shape.size() != 3 || prediction_shape[0] != 1 ||
        prediction_shape[1] <= 0 || prediction_shape[2] <= 0 ||
        prototype_shape.size() != 4 || prototype_shape[0] != 1 ||
        prototype_shape[1] <= 0 || prototype_shape[2] <= 0 ||
        prototype_shape[3] <= 0) {
        return false;
    }

    const int64_t mask_channels = prototype_shape[1];
    const int64_t features = prediction_shape[1];
    if (mask_channels > std::numeric_limits<int64_t>::max() - 4 ||
        features <= 4 + mask_channels) {
        return false;
    }

    contract.class_count = features - 4 - mask_channels;
    contract.mask_channels = mask_channels;
    contract.anchors = prediction_shape[2];
    contract.prototype_height = prototype_shape[2];
    contract.prototype_width = prototype_shape[3];
    return contract.class_count > 0;
}

bool decode_segmentation_output(
        const float* prediction_data,
        const std::vector<int64_t>& prediction_shape,
        const SegmentationContract& contract,
        float conf_threshold,
        std::vector<SegmentationCandidate>& candidates) noexcept {
    if (!prediction_data || prediction_shape.size() != 3 ||
        prediction_shape[0] != 1 ||
        prediction_shape[1] !=
            4 + contract.class_count + contract.mask_channels ||
        prediction_shape[2] != contract.anchors ||
        contract.class_count <= 0 || contract.mask_channels <= 0 ||
        contract.anchors <= 0 || !std::isfinite(conf_threshold) ||
        conf_threshold < 0.0f || conf_threshold > 1.0f) {
        return false;
    }

    try {
        candidates.reserve(
            candidates.size() + static_cast<size_t>(contract.anchors));
        for (int64_t anchor = 0; anchor < contract.anchors; ++anchor) {
            float best_confidence =
                -std::numeric_limits<float>::infinity();
            int best_class = -1;
            for (int64_t class_index = 0;
                 class_index < contract.class_count; ++class_index) {
                const float confidence = prediction_data[
                    (4 + class_index) * contract.anchors + anchor];
                if (std::isfinite(confidence) &&
                    confidence > best_confidence) {
                    best_confidence = confidence;
                    best_class = static_cast<int>(class_index);
                }
            }
            if (best_class < 0 || best_confidence < conf_threshold) continue;

            SegmentationCandidate candidate;
            if (!make_xywh_detection(
                    prediction_data[anchor],
                    prediction_data[contract.anchors + anchor],
                    prediction_data[2 * contract.anchors + anchor],
                    prediction_data[3 * contract.anchors + anchor],
                    best_confidence, best_class,
                    candidate.detection)) {
                continue;
            }
            candidate.anchor_index = anchor;
            candidates.push_back(candidate);
        }
        return true;
    } catch (...) {
        candidates.clear();
        return false;
    }
}

bool resolve_pose_contract(
        const std::vector<int64_t>& prediction_shape,
        OutputFormat requested,
        int64_t keypoint_count,
        int64_t keypoint_dimensions,
        PoseContract& contract) noexcept {
    contract = {};
    if ((requested != OutputFormat::AUTO &&
         requested != OutputFormat::CHANNEL_FIRST) ||
        prediction_shape.size() != 3 || prediction_shape[0] != 1 ||
        prediction_shape[1] <= 0 || prediction_shape[2] <= 0 ||
        keypoint_count <= 0 ||
        (keypoint_dimensions != 2 && keypoint_dimensions != 3) ||
        keypoint_count > std::numeric_limits<int64_t>::max() /
            keypoint_dimensions) {
        return false;
    }

    const int64_t keypoint_features =
        keypoint_count * keypoint_dimensions;
    if (keypoint_features > std::numeric_limits<int64_t>::max() - 4 ||
        prediction_shape[1] <= 4 + keypoint_features) {
        return false;
    }

    contract.class_count =
        prediction_shape[1] - 4 - keypoint_features;
    contract.keypoint_count = keypoint_count;
    contract.keypoint_dimensions = keypoint_dimensions;
    contract.anchors = prediction_shape[2];
    return contract.class_count > 0;
}

bool decode_pose_output(
        const float* prediction_data,
        const std::vector<int64_t>& prediction_shape,
        const PoseContract& contract,
        float conf_threshold,
        std::vector<PoseCandidate>& candidates) noexcept {
    if (!prediction_data || prediction_shape.size() != 3 ||
        prediction_shape[0] != 1 ||
        contract.keypoint_count <= 0 ||
        (contract.keypoint_dimensions != 2 &&
         contract.keypoint_dimensions != 3) ||
        contract.keypoint_count > std::numeric_limits<int64_t>::max() /
            contract.keypoint_dimensions ||
        prediction_shape[1] !=
            4 + contract.class_count +
                contract.keypoint_count * contract.keypoint_dimensions ||
        prediction_shape[2] != contract.anchors ||
        contract.class_count <= 0 || contract.anchors <= 0 ||
        !std::isfinite(conf_threshold) || conf_threshold < 0.0f ||
        conf_threshold > 1.0f) {
        return false;
    }

    try {
        candidates.reserve(
            candidates.size() + static_cast<size_t>(contract.anchors));
        for (int64_t anchor = 0; anchor < contract.anchors; ++anchor) {
            float best_confidence =
                -std::numeric_limits<float>::infinity();
            int best_class = -1;
            for (int64_t class_index = 0;
                 class_index < contract.class_count; ++class_index) {
                const float confidence = prediction_data[
                    (4 + class_index) * contract.anchors + anchor];
                if (std::isfinite(confidence) &&
                    confidence > best_confidence) {
                    best_confidence = confidence;
                    best_class = static_cast<int>(class_index);
                }
            }
            if (best_class < 0 || best_confidence < conf_threshold) continue;

            PoseCandidate candidate;
            if (!make_xywh_detection(
                    prediction_data[anchor],
                    prediction_data[contract.anchors + anchor],
                    prediction_data[2 * contract.anchors + anchor],
                    prediction_data[3 * contract.anchors + anchor],
                    best_confidence, best_class,
                    candidate.detection)) {
                continue;
            }
            candidate.anchor_index = anchor;
            candidates.push_back(candidate);
        }
        return true;
    } catch (...) {
        candidates.clear();
        return false;
    }
}

bool resolve_obb_contract(
        const std::vector<int64_t>& prediction_shape,
        OutputFormat requested,
        ObbContract& contract) noexcept {
    contract = {};
    if ((requested != OutputFormat::AUTO &&
         requested != OutputFormat::CHANNEL_FIRST) ||
        prediction_shape.size() != 3 || prediction_shape[0] != 1 ||
        prediction_shape[1] <= 5 || prediction_shape[2] <= 0) {
        return false;
    }
    contract.class_count = prediction_shape[1] - 5;
    contract.anchors = prediction_shape[2];
    return contract.class_count > 0;
}

bool decode_obb_output(
        const float* prediction_data,
        const std::vector<int64_t>& prediction_shape,
        const ObbContract& contract,
        float conf_threshold,
        std::vector<ObbCandidate>& candidates) noexcept {
    if (!prediction_data || prediction_shape.size() != 3 ||
        prediction_shape[0] != 1 ||
        prediction_shape[1] != 5 + contract.class_count ||
        prediction_shape[2] != contract.anchors ||
        contract.class_count <= 0 || contract.anchors <= 0 ||
        !std::isfinite(conf_threshold) || conf_threshold < 0.0f ||
        conf_threshold > 1.0f) {
        return false;
    }

    try {
        candidates.reserve(
            candidates.size() + static_cast<size_t>(contract.anchors));
        const int64_t angle_feature = 4 + contract.class_count;
        for (int64_t anchor = 0; anchor < contract.anchors; ++anchor) {
            float best_confidence =
                -std::numeric_limits<float>::infinity();
            int best_class = -1;
            for (int64_t class_index = 0;
                 class_index < contract.class_count; ++class_index) {
                const float confidence = prediction_data[
                    (4 + class_index) * contract.anchors + anchor];
                if (std::isfinite(confidence) &&
                    confidence > best_confidence) {
                    best_confidence = confidence;
                    best_class = static_cast<int>(class_index);
                }
            }
            if (best_class < 0 || best_confidence < conf_threshold) continue;

            const float center_x = prediction_data[anchor];
            const float center_y =
                prediction_data[contract.anchors + anchor];
            const float width =
                prediction_data[2 * contract.anchors + anchor];
            const float height =
                prediction_data[3 * contract.anchors + anchor];
            const float angle = prediction_data[
                angle_feature * contract.anchors + anchor];
            if (!std::isfinite(center_x) || !std::isfinite(center_y) ||
                !std::isfinite(width) || !std::isfinite(height) ||
                !std::isfinite(angle) || width <= 0.0f ||
                height <= 0.0f) {
                continue;
            }

            ObbCandidate candidate;
            candidate.detection.center_x = center_x;
            candidate.detection.center_y = center_y;
            candidate.detection.width = width;
            candidate.detection.height = height;
            candidate.detection.angle_radians = angle;
            candidate.detection.confidence = best_confidence;
            candidate.detection.class_id = best_class;
            candidates.push_back(candidate);
        }
        return true;
    } catch (...) {
        candidates.clear();
        return false;
    }
}

float probabilistic_iou(const OrientedDetection& left,
                        const OrientedDetection& right) noexcept {
    constexpr double kEpsilon = 1e-7;
    const auto valid = [](const OrientedDetection& box) {
        return std::isfinite(box.center_x) &&
               std::isfinite(box.center_y) &&
               std::isfinite(box.width) &&
               std::isfinite(box.height) &&
               std::isfinite(box.angle_radians) &&
               box.width > 0.0f && box.height > 0.0f;
    };
    if (!valid(left) || !valid(right)) return 0.0f;

    const auto covariance = [](const OrientedDetection& box) {
        const double width_variance =
            static_cast<double>(box.width) * box.width / 12.0;
        const double height_variance =
            static_cast<double>(box.height) * box.height / 12.0;
        const double cosine = std::cos(box.angle_radians);
        const double sine = std::sin(box.angle_radians);
        const double cosine_squared = cosine * cosine;
        const double sine_squared = sine * sine;
        return std::array<double, 3>{
            width_variance * cosine_squared +
                height_variance * sine_squared,
            width_variance * sine_squared +
                height_variance * cosine_squared,
            (width_variance - height_variance) * cosine * sine,
        };
    };
    const auto left_covariance = covariance(left);
    const auto right_covariance = covariance(right);
    const double a1 = left_covariance[0];
    const double b1 = left_covariance[1];
    const double c1 = left_covariance[2];
    const double a2 = right_covariance[0];
    const double b2 = right_covariance[1];
    const double c2 = right_covariance[2];
    const double delta_x =
        static_cast<double>(left.center_x) - right.center_x;
    const double delta_y =
        static_cast<double>(left.center_y) - right.center_y;
    const double covariance_determinant =
        (a1 + a2) * (b1 + b2) - (c1 + c2) * (c1 + c2);
    const double denominator = covariance_determinant + kEpsilon;
    if (!std::isfinite(denominator) || denominator <= 0.0) return 0.0f;

    const double t1 = 0.25 *
        ((a1 + a2) * delta_y * delta_y +
         (b1 + b2) * delta_x * delta_x) /
        denominator;
    const double t2 = 0.5 *
        ((c1 + c2) * (-delta_x) * delta_y) / denominator;
    const double determinant1 = std::max(0.0, a1 * b1 - c1 * c1);
    const double determinant2 = std::max(0.0, a2 * b2 - c2 * c2);
    const double logarithm_argument = covariance_determinant /
            (4.0 * std::sqrt(determinant1 * determinant2) + kEpsilon) +
        kEpsilon;
    if (!std::isfinite(logarithm_argument) ||
        logarithm_argument <= 0.0) {
        return 0.0f;
    }
    const double t3 = 0.5 * std::log(logarithm_argument);
    const double bhattacharyya_distance = std::clamp(
        t1 + t2 + t3, kEpsilon, 100.0);
    const double hellinger_distance = std::sqrt(std::max(
        0.0, 1.0 - std::exp(-bhattacharyya_distance) + kEpsilon));
    const double similarity = 1.0 - hellinger_distance;
    return std::isfinite(similarity)
        ? static_cast<float>(similarity) : 0.0f;
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
        std::vector<Detection> output;
        std::vector<unsigned char> suppressed;
        nms_into(dets, nms_threshold, top_k, output, suppressed);
        dets = std::move(output);
    } catch (...) {
        dets.clear();
    }
}

bool finalize_detections(std::vector<Detection>& candidates,
                         OutputFormat format,
                         float nms_threshold,
                         int top_k,
                         const LetterBoxInfo& info,
                         std::vector<Detection>& output,
                         std::vector<unsigned char>& suppressed) noexcept {
    output.clear();
    if (top_k <= 0 || !std::isfinite(nms_threshold) ||
        nms_threshold < 0.0f || nms_threshold > 1.0f) {
        return false;
    }

    try {
        if (format == OutputFormat::END_TO_END) {
            const size_t count = std::min(
                candidates.size(), static_cast<size_t>(top_k));
            if (candidates.size() > count) {
                std::partial_sort(
                    candidates.begin(), candidates.begin() + count,
                    candidates.end(),
                    [](const Detection& a, const Detection& b) {
                        return a.confidence > b.confidence;
                    });
            }
            output.assign(candidates.begin(), candidates.begin() + count);
        } else {
            nms_into(candidates, nms_threshold, top_k, output, suppressed);
        }

        return scale_detections(output, info);
    } catch (...) {
        output.clear();
        return false;
    }
}

bool finalize_segmentations(
        std::vector<SegmentationCandidate>& candidates,
        float nms_threshold,
        int top_k,
        const LetterBoxInfo& info,
        const float* prediction_data,
        const std::vector<int64_t>& prediction_shape,
        const float* prototype_data,
        const std::vector<int64_t>& prototype_shape,
        const SegmentationContract& contract,
        bool generate_masks,
        SegmentationResult& output,
        std::vector<SegmentationCandidate>& selected,
        std::vector<unsigned char>& suppressed,
        std::vector<float>& mask_logits,
        std::vector<std::uint8_t>& mask_input) noexcept {
    output = {};
    if (top_k <= 0 || !std::isfinite(nms_threshold) ||
        nms_threshold < 0.0f || nms_threshold > 1.0f ||
        !valid_letterbox(info) || prediction_shape.size() != 3 ||
        prediction_shape[0] != 1 ||
        prediction_shape[1] !=
            4 + contract.class_count + contract.mask_channels ||
        prediction_shape[2] != contract.anchors ||
        prototype_shape.size() != 4 || prototype_shape[0] != 1 ||
        prototype_shape[1] != contract.mask_channels ||
        prototype_shape[2] != contract.prototype_height ||
        prototype_shape[3] != contract.prototype_width ||
        contract.class_count <= 0 || contract.mask_channels <= 0 ||
        contract.anchors <= 0 || contract.prototype_height <= 0 ||
        contract.prototype_width <= 0) {
        return false;
    }
    if (generate_masks && (!prediction_data || !prototype_data ||
                           info.target_w <= 0 || info.target_h <= 0)) {
        return false;
    }

    try {
        nms_segmentations_into(
            candidates, nms_threshold, top_k, selected, suppressed);

        output.detections.reserve(selected.size());
        size_t valid_count = 0;
        for (const SegmentationCandidate& candidate : selected) {
            Detection scaled;
            if (!scale_detection(candidate.detection, info, scaled)) continue;
            output.detections.push_back(scaled);
            selected[valid_count++] = candidate;
        }
        selected.resize(valid_count);
        if (!generate_masks || selected.empty()) return true;

        if (contract.prototype_height >
                static_cast<int64_t>(std::numeric_limits<int>::max()) ||
            contract.prototype_width >
                static_cast<int64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        const int prototype_height =
            static_cast<int>(contract.prototype_height);
        const int prototype_width =
            static_cast<int>(contract.prototype_width);
        const size_t prototype_area =
            static_cast<size_t>(prototype_height) *
            static_cast<size_t>(prototype_width);
        if (prototype_width <= 0 || prototype_height <= 0 ||
            prototype_area >
                std::numeric_limits<size_t>::max() /
                    static_cast<size_t>(contract.mask_channels)) {
            return false;
        }
        const size_t prototype_elements = prototype_area *
            static_cast<size_t>(contract.mask_channels);
        for (size_t index = 0; index < prototype_elements; ++index) {
            if (!std::isfinite(prototype_data[index])) return false;
        }

        output.masks.reserve(output.detections.size());
        size_t total_mask_bytes = 0;
        for (const Detection& detection : output.detections) {
            const int left = std::clamp(
                static_cast<int>(std::floor(detection.x1)),
                0, info.orig_w);
            const int top = std::clamp(
                static_cast<int>(std::floor(detection.y1)),
                0, info.orig_h);
            const int right = std::clamp(
                static_cast<int>(std::ceil(detection.x2)),
                0, info.orig_w);
            const int bottom = std::clamp(
                static_cast<int>(std::ceil(detection.y2)),
                0, info.orig_h);
            if (right <= left || bottom <= top) return false;

            const size_t width = static_cast<size_t>(right - left);
            const size_t height = static_cast<size_t>(bottom - top);
            if (height > std::numeric_limits<size_t>::max() / width) {
                return false;
            }
            const size_t mask_bytes = width * height;
            if (mask_bytes >
                std::numeric_limits<size_t>::max() - total_mask_bytes) {
                return false;
            }

            InstanceMask mask;
            mask.x = left;
            mask.y = top;
            mask.width = right - left;
            mask.height = bottom - top;
            mask.data_offset = total_mask_bytes;
            mask.row_stride = width;
            output.masks.push_back(mask);
            total_mask_bytes += mask_bytes;
        }

        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            total_mask_bytes, 0U);
        mask_logits.resize(prototype_area);
        const size_t input_area = static_cast<size_t>(info.target_w) *
            static_cast<size_t>(info.target_h);
        mask_input.resize(input_area);
        const float prototype_scale_x =
            static_cast<float>(prototype_width) /
            static_cast<float>(info.target_w);
        const float prototype_scale_y =
            static_cast<float>(prototype_height) /
            static_cast<float>(info.target_h);
        const int resized_width = std::clamp(
            static_cast<int>(std::round(info.orig_w * info.scale)),
            1, info.target_w);
        const int resized_height = std::clamp(
            static_cast<int>(std::round(info.orig_h * info.scale)),
            1, info.target_h);
        const int input_left = std::clamp(
            static_cast<int>(std::round(info.pad_x)),
            0, info.target_w - resized_width);
        const int input_top = std::clamp(
            static_cast<int>(std::round(info.pad_y)),
            0, info.target_h - resized_height);

        for (size_t instance = 0; instance < selected.size(); ++instance) {
            const SegmentationCandidate& candidate = selected[instance];
            if (candidate.anchor_index < 0 ||
                candidate.anchor_index >= contract.anchors) {
                return false;
            }
            for (int64_t channel = 0;
                 channel < contract.mask_channels; ++channel) {
                const float coefficient = prediction_data[
                    (4 + contract.class_count + channel) *
                        contract.anchors + candidate.anchor_index];
                if (!std::isfinite(coefficient)) return false;
                const float* prototype = prototype_data +
                    static_cast<size_t>(channel) * prototype_area;
                if (channel == 0) {
                    for (size_t pixel = 0; pixel < prototype_area; ++pixel) {
                        mask_logits[pixel] = coefficient * prototype[pixel];
                    }
                } else {
                    for (size_t pixel = 0; pixel < prototype_area; ++pixel) {
                        mask_logits[pixel] += coefficient * prototype[pixel];
                    }
                }
            }
            if (std::any_of(mask_logits.begin(), mask_logits.end(),
                    [](float value) { return !std::isfinite(value); })) {
                return false;
            }

            // Ultralytics process_mask() 先在原型分辨率按模型框裁剪，再双线性
            // 上采样并按 logit>0 二值化；该阈值与 sigmoid(logit)>0.5 等价。
            const int crop_left = std::clamp(
                static_cast<int>(std::round(
                    candidate.detection.x1 * prototype_scale_x)),
                0, prototype_width);
            const int crop_top = std::clamp(
                static_cast<int>(std::round(
                    candidate.detection.y1 * prototype_scale_y)),
                0, prototype_height);
            const int crop_right = std::clamp(
                static_cast<int>(std::round(
                    candidate.detection.x2 * prototype_scale_x)),
                0, prototype_width);
            const int crop_bottom = std::clamp(
                static_cast<int>(std::round(
                    candidate.detection.y2 * prototype_scale_y)),
                0, prototype_height);
            for (int y = 0; y < prototype_height; ++y) {
                float* row = mask_logits.data() +
                    static_cast<size_t>(y) * prototype_width;
                if (y < crop_top || y >= crop_bottom) {
                    std::fill_n(row, prototype_width, 0.0f);
                    continue;
                }
                std::fill(row, row + crop_left, 0.0f);
                std::fill(row + crop_right,
                          row + prototype_width, 0.0f);
            }

            // 固定输入 scratch 跨实例、跨帧复用。这里显式保留“先上采样再
            // 二值化”的阶段边界，不能把两次插值合并，否则小目标边缘会与
            // Ultralytics 参考实现产生可观测偏差。
            for (int y = 0; y < info.target_h; ++y) {
                const float prototype_y =
                    ((static_cast<float>(y) + 0.5f) *
                         prototype_scale_y) - 0.5f;
                std::uint8_t* row = mask_input.data() +
                    static_cast<size_t>(y) * info.target_w;
                for (int x = 0; x < info.target_w; ++x) {
                    const float prototype_x =
                        ((static_cast<float>(x) + 0.5f) *
                             prototype_scale_x) - 0.5f;
                    row[x] = bilinear_sample(
                        mask_logits, prototype_width, prototype_height,
                        prototype_x, prototype_y) > 0.0f ? 1U : 0U;
                }
            }

            // scale_masks() 会先移除 LetterBox，再把二值模型掩码缩放到原图。
            // 只计算实例 ROI，最终输出不保留每实例的整图缓冲。
            const InstanceMask& mask = output.masks[instance];
            for (int row = 0; row < mask.height; ++row) {
                const int original_y = mask.y + row;
                const float center_y =
                    static_cast<float>(original_y) + 0.5f;
                const float input_y = center_y *
                        static_cast<float>(resized_height) /
                        static_cast<float>(info.orig_h) -
                    0.5f;
                std::uint8_t* destination = pixels->data() +
                    mask.data_offset +
                    static_cast<size_t>(row) * mask.row_stride;
                for (int column = 0; column < mask.width; ++column) {
                    const int original_x = mask.x + column;
                    const float center_x =
                        static_cast<float>(original_x) + 0.5f;
                    const float input_x = center_x *
                            static_cast<float>(resized_width) /
                            static_cast<float>(info.orig_w) -
                        0.5f;
                    destination[column] = bilinear_sample_binary_roi(
                        mask_input, info.target_w, input_left, input_top,
                        resized_width, resized_height,
                        input_x, input_y) > 0.5f ? 1U : 0U;
                }
            }
        }
        output.mask_pixels = std::move(pixels);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool finalize_poses(
        std::vector<PoseCandidate>& candidates,
        float nms_threshold,
        int top_k,
        const LetterBoxInfo& info,
        const float* prediction_data,
        const std::vector<int64_t>& prediction_shape,
        const PoseContract& contract,
        bool generate_keypoints,
        PoseResult& output,
        std::vector<PoseCandidate>& selected,
        std::vector<unsigned char>& suppressed) noexcept {
    output = {};
    if (top_k <= 0 || !std::isfinite(nms_threshold) ||
        nms_threshold < 0.0f || nms_threshold > 1.0f ||
        !valid_letterbox(info) || prediction_shape.size() != 3 ||
        prediction_shape[0] != 1 || contract.class_count <= 0 ||
        contract.keypoint_count <= 0 ||
        (contract.keypoint_dimensions != 2 &&
         contract.keypoint_dimensions != 3) ||
        contract.keypoint_count > std::numeric_limits<int64_t>::max() /
            contract.keypoint_dimensions ||
        prediction_shape[1] !=
            4 + contract.class_count +
                contract.keypoint_count * contract.keypoint_dimensions ||
        prediction_shape[2] != contract.anchors ||
        contract.anchors <= 0) {
        return false;
    }
    if (generate_keypoints && !prediction_data) return false;

    try {
        nms_poses_into(
            candidates, nms_threshold, top_k, selected, suppressed);

        output.detections.reserve(selected.size());
        size_t valid_count = 0;
        for (const PoseCandidate& candidate : selected) {
            Detection scaled;
            if (!scale_detection(candidate.detection, info, scaled)) continue;
            output.detections.push_back(scaled);
            selected[valid_count++] = candidate;
        }
        selected.resize(valid_count);
        if (!generate_keypoints) return true;

        output.keypoints_per_detection =
            static_cast<size_t>(contract.keypoint_count);
        output.keypoint_dimensions =
            static_cast<int>(contract.keypoint_dimensions);
        if (selected.empty()) return true;
        if (selected.size() > std::numeric_limits<size_t>::max() /
                output.keypoints_per_detection) {
            return false;
        }
        output.keypoints.resize(
            selected.size() * output.keypoints_per_detection);

        const size_t keypoint_feature_offset =
            static_cast<size_t>(4 + contract.class_count);
        for (size_t instance = 0; instance < selected.size(); ++instance) {
            const PoseCandidate& candidate = selected[instance];
            if (candidate.anchor_index < 0 ||
                candidate.anchor_index >= contract.anchors) {
                return false;
            }
            for (int64_t keypoint_index = 0;
                 keypoint_index < contract.keypoint_count;
                 ++keypoint_index) {
                const size_t feature = keypoint_feature_offset +
                    static_cast<size_t>(keypoint_index) *
                        static_cast<size_t>(contract.keypoint_dimensions);
                const float raw_x = prediction_data[
                    feature * static_cast<size_t>(contract.anchors) +
                    static_cast<size_t>(candidate.anchor_index)];
                const float raw_y = prediction_data[
                    (feature + 1) *
                        static_cast<size_t>(contract.anchors) +
                    static_cast<size_t>(candidate.anchor_index)];
                const float confidence = contract.keypoint_dimensions == 3
                    ? prediction_data[
                        (feature + 2) *
                            static_cast<size_t>(contract.anchors) +
                        static_cast<size_t>(candidate.anchor_index)]
                    : 1.0f;
                if (!std::isfinite(raw_x) || !std::isfinite(raw_y) ||
                    !std::isfinite(confidence)) {
                    return false;
                }

                PoseKeypoint& keypoint = output.keypoints[
                    instance * output.keypoints_per_detection +
                    static_cast<size_t>(keypoint_index)];
                keypoint.x = std::clamp(
                    (raw_x - info.pad_x) / info.scale,
                    0.0f, static_cast<float>(info.orig_w));
                keypoint.y = std::clamp(
                    (raw_y - info.pad_y) / info.scale,
                    0.0f, static_cast<float>(info.orig_h));
                keypoint.confidence = confidence;
            }
        }
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

bool finalize_obbs(
        std::vector<ObbCandidate>& candidates,
        float nms_threshold,
        int top_k,
        const LetterBoxInfo& info,
        bool generate_oriented,
        ObbResult& output,
        std::vector<ObbCandidate>& selected,
        std::vector<unsigned char>& suppressed) noexcept {
    output = {};
    if (top_k <= 0 || !std::isfinite(nms_threshold) ||
        nms_threshold < 0.0f || nms_threshold > 1.0f ||
        !valid_letterbox(info)) {
        return false;
    }

    try {
        nms_obbs_into(
            candidates, nms_threshold, top_k, selected, suppressed);
        output.detections.reserve(selected.size());
        if (generate_oriented) {
            output.oriented_detections.reserve(selected.size());
        }
        for (const ObbCandidate& candidate : selected) {
            OrientedDetection scaled;
            Detection envelope;
            if (!scale_oriented_detection(
                    candidate.detection, info, scaled, envelope)) {
                continue;
            }
            output.detections.push_back(envelope);
            if (generate_oriented) {
                output.oriented_detections.push_back(scaled);
            }
        }
        return !generate_oriented ||
               output.detections.size() ==
                   output.oriented_detections.size();
    } catch (...) {
        output = {};
        return false;
    }
}

bool scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info) noexcept {
    if (!valid_letterbox(info)) {
        dets.clear();
        return false;
    }

    size_t valid_count = 0;
    for (const Detection& detection : dets) {
        Detection scaled;
        if (scale_detection(detection, info, scaled)) {
            dets[valid_count++] = scaled;
        }
    }
    dets.resize(valid_count);
    return true;
}

} // namespace detector::detail
