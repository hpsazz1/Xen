#ifndef AIM_LANDMARK_H
#define AIM_LANDMARK_H

#include "aim/aim.h"

#include <cstdint>
#include <span>

namespace aim_landmark {

enum class Status {
    INVALID_INPUT,
    NO_TARGET,
    NO_CURRENT_OBSERVATION,
    OCCLUDED,
    ASSOCIATION_MISMATCH,
    AMBIGUOUS,
    VALID,
};

const char* status_name(Status status) noexcept;

enum class SemanticKind {
    NONE,
    HEAD_BOX_CENTER,
};

const char* semantic_kind_name(SemanticKind kind) noexcept;

struct Diagnostic {
    Status status = Status::INVALID_INPUT;
    SemanticKind semantic_kind = SemanticKind::NONE;
    std::uint64_t sequence = 0;
    std::uint64_t track_id = 0;
    std::uint32_t candidate_count = 0;
    bool valid = false;
    bool fresh = false;
    // 只表示当前帧没有可唯一关联的 head；不声称已证明真实遮挡。
    bool occluded = false;
    // landmark v1 契约不允许该点直接成为控制输入；该字段没有配置开关。
    bool control_eligible = false;
    float x = 0.0f;
    float y = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float confidence = 0.0f;
    int class_id = -1;
};

// 该纯函数只检查同帧 Detector/Aim 结果。它不缓存 last-good、不修改 Aim，
// 也不创建 Provider；身份由 (track_id, HEAD_BOX_CENTER) 显式表达。
Diagnostic inspect_head_landmark(
    std::uint64_t sequence,
    std::span<const Detection> detections,
    const AimConfig& aim_config,
    const AimResult& aim_result) noexcept;

} // namespace aim_landmark

#endif // AIM_LANDMARK_H
