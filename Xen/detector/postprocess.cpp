#include "detector/postprocess.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace detector::detail {

// ── 辅助 IoU ──
static inline float iou(const Detection& a, const Detection& b) {
    float ix = std::max(a.x1, b.x1), iy = std::max(a.y1, b.y1);
    float ia = std::min(a.x2, b.x2), ib = std::min(a.y2, b.y2);
    float iw = std::max(0.0f, ia - ix), ih = std::max(0.0f, ib - iy);
    float inter = iw * ih;
    float aa = (a.x2 - a.x1) * (a.y2 - a.y1);
    float ab = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (aa + ab - inter + 1e-6f);
}

// ── NMS ──
void nms(std::vector<Detection>& dets, float nms_thresh, int top_k) {
    if (dets.empty()) return;
    std::sort(dets.begin(), dets.end(),
        [](const Detection& a, const Detection& b) {
            return a.confidence > b.confidence; });

    std::vector<bool> keep(dets.size(), true);
    std::vector<Detection> out;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (!keep[i]) continue;
        out.push_back(dets[i]);
        if ((int)out.size() >= top_k) break;
        for (size_t j = i + 1; j < dets.size(); ++j)
            if (keep[j] && dets[j].class_id == dets[i].class_id
                && iou(dets[i], dets[j]) > nms_thresh)
                keep[j] = false;
    }
    dets = std::move(out);
}

// ── YOLOv8/v11 解码 ──
void decode_yolov8(const float* data, int anchors, int channels,
                   std::vector<Detection>& dets) {
    int nc = channels - 4;
    dets.reserve(dets.size() + anchors);
    for (int i = 0; i < anchors; ++i) {
        float best_conf = 0; int best_id = -1;
        for (int c = 0; c < nc; ++c) {
            float conf = data[4 + c];
            if (conf > best_conf) { best_conf = conf; best_id = c; }
        }
        if (best_id >= 0) {
            Detection d;
            float cx = data[0], cy = data[1], w = data[2], h = data[3];
            d.x1 = cx - w / 2; d.y1 = cy - h / 2;
            d.x2 = cx + w / 2; d.y2 = cy + h / 2;
            d.confidence = best_conf; d.class_id = best_id;
            dets.push_back(d);
        }
        data += channels;
    }
}

// ── YOLOv10 解码 ──
void decode_yolov10(const float* data, int num_dets,
                    std::vector<Detection>& dets) {
    dets.reserve(dets.size() + num_dets);
    for (int i = 0; i < num_dets; ++i) {
        Detection d;
        d.x1 = data[0]; d.y1 = data[1]; d.x2 = data[2]; d.y2 = data[3];
        d.confidence = data[4]; d.class_id = (int)data[5];
        dets.push_back(d);
        data += 6;
    }
}

// ── YOLOv5 解码（带 objectness） ──
void decode_yolov5(const float* data, int anchors, int channels,
                   std::vector<Detection>& dets) {
    int nc = channels - 5;
    dets.reserve(dets.size() + anchors);
    for (int i = 0; i < anchors; ++i) {
        float obj = data[4];
        if (obj > 0) {
            float best_conf = 0; int best_id = -1;
            for (int c = 0; c < nc; ++c) {
                float conf = data[5 + c];
                if (conf > best_conf) { best_conf = conf; best_id = c; }
            }
            if (best_id >= 0) {
                Detection d;
                float cx = data[0], cy = data[1], w = data[2], h = data[3];
                d.x1 = cx - w / 2; d.y1 = cy - h / 2;
                d.x2 = cx + w / 2; d.y2 = cy + h / 2;
                d.confidence = obj * best_conf; d.class_id = best_id;
                dets.push_back(d);
            }
        }
        data += channels;
    }
}

// ── 坐标还原 ──
void scale_detections(std::vector<Detection>& dets,
                      const LetterBoxInfo& info) {
    for (auto& d : dets) {
        d.x1 = (d.x1 * info.scale_x * info.orig_w - info.pad_x * info.scale_x);
        d.y1 = (d.y1 * info.scale_y * info.orig_h - info.pad_y * info.scale_y);
        d.x2 = (d.x2 * info.scale_x * info.orig_w - info.pad_x * info.scale_x);
        d.y2 = (d.y2 * info.scale_y * info.orig_h - info.pad_y * info.scale_y);
    }
}

} // namespace detector::detail
