#include "aim_landmark/aim_landmark.h"

#include <algorithm>
#include <cmath>

namespace aim_landmark {
namespace {

bool configured_class(std::span<const int> classes, int class_id) noexcept {
    return std::find(classes.begin(), classes.end(), class_id) !=
           classes.end();
}

bool finite_box(const Detection& detection) noexcept {
    return std::isfinite(detection.x1) && std::isfinite(detection.y1) &&
           std::isfinite(detection.x2) && std::isfinite(detection.y2) &&
           std::isfinite(detection.confidence) &&
           detection.x2 > detection.x1 && detection.y2 > detection.y1;
}

bool same_box(const Detection& detection,
              const AimTargetSnapshot& target) noexcept {
    return detection.x1 == target.matched_observation_x1 &&
           detection.y1 == target.matched_observation_y1 &&
           detection.x2 == target.matched_observation_x2 &&
           detection.y2 == target.matched_observation_y2;
}

Diagnostic make_diagnostic(std::uint64_t sequence,
                           const AimTargetSnapshot* target,
                           Status status) noexcept {
    Diagnostic result;
    result.status = status;
    result.sequence = sequence;
    if (target) {
        result.semantic_kind = SemanticKind::HEAD_BOX_CENTER;
        result.track_id = target->track_id;
    }
    result.occluded = status == Status::OCCLUDED;
    return result;
}

} // namespace

Diagnostic inspect_head_landmark(
        std::uint64_t sequence,
        std::span<const Detection> detections,
        const AimConfig& aim_config,
        const AimResult& aim_result) noexcept {
    if (sequence == 0 || aim_result.status != AimStatus::SUCCESS) {
        return make_diagnostic(sequence, nullptr, Status::INVALID_INPUT);
    }
    if (!aim_result.has_target || aim_result.target.track_id == 0) {
        return make_diagnostic(sequence, nullptr, Status::NO_TARGET);
    }
    const AimTargetSnapshot& target = aim_result.target;
    if (!target.matched_observation_valid || target.predicted) {
        return make_diagnostic(
            sequence, &target, Status::NO_CURRENT_OBSERVATION);
    }

    const float observation_x1 = target.matched_observation_x1;
    const float observation_y1 = target.matched_observation_y1;
    const float observation_x2 = target.matched_observation_x2;
    const float observation_y2 = target.matched_observation_y2;
    const float observation_width = observation_x2 - observation_x1;
    const float observation_height = observation_y2 - observation_y1;
    const float observation_area = observation_width * observation_height;
    if (!std::isfinite(observation_x1) ||
        !std::isfinite(observation_y1) ||
        !std::isfinite(observation_x2) ||
        !std::isfinite(observation_y2) ||
        observation_width <= 0.0f || observation_height <= 0.0f ||
        aim_config.head_class_ids.empty() ||
        !std::isfinite(aim_config.low_confidence)) {
        return make_diagnostic(sequence, &target, Status::INVALID_INPUT);
    }
    const Detection* candidate = nullptr;
    std::uint32_t candidate_count = 0;
    for (const Detection& detection : detections) {
        if (!configured_class(
                aim_config.head_class_ids, detection.class_id)) {
            continue;
        }
        if (!finite_box(detection)) {
            return make_diagnostic(
                sequence, &target, Status::INVALID_INPUT);
        }
        if (detection.confidence < aim_config.low_confidence) continue;
        bool associated = false;
        if (target.matched_observation_head_only) {
            associated = same_box(detection, target);
        } else {
            const float head_x = (detection.x1 + detection.x2) * 0.5f;
            const float head_y = (detection.y1 + detection.y2) * 0.5f;
            const float head_area =
                (detection.x2 - detection.x1) *
                (detection.y2 - detection.y1);
            const float area_ratio = observation_area > 0.0f
                ? head_area / observation_area : 1.0f;
            associated = head_x >= observation_x1 &&
                head_x <= observation_x2 &&
                head_y >= observation_y1 &&
                head_y <= observation_y1 + observation_height * 0.65f &&
                area_ratio >= 0.01f && area_ratio <= 0.40f;
        }
        if (!associated) continue;
        ++candidate_count;
        candidate = &detection;
    }
    if (!target.matched_observation_head_only &&
        !target.matched_observation_aim_from_head) {
        Diagnostic result = make_diagnostic(
            sequence, &target,
            candidate_count == 0 ? Status::OCCLUDED
                                 : Status::AMBIGUOUS);
        result.candidate_count = candidate_count;
        return result;
    }
    if (candidate_count == 0) {
        Diagnostic result = make_diagnostic(
            sequence, &target, Status::ASSOCIATION_MISMATCH);
        result.candidate_count = 0;
        return result;
    }
    if (candidate_count != 1 || !candidate) {
        Diagnostic result = make_diagnostic(
            sequence, &target, Status::AMBIGUOUS);
        result.candidate_count = candidate_count;
        return result;
    }

    Diagnostic result = make_diagnostic(sequence, &target, Status::VALID);
    result.semantic_kind = SemanticKind::HEAD_BOX_CENTER;
    result.candidate_count = 1;
    result.valid = true;
    result.fresh = true;
    result.x1 = candidate->x1;
    result.y1 = candidate->y1;
    result.x2 = candidate->x2;
    result.y2 = candidate->y2;
    result.x = (candidate->x1 + candidate->x2) * 0.5f;
    result.y = (candidate->y1 + candidate->y2) * 0.5f;
    result.confidence = candidate->confidence;
    result.class_id = candidate->class_id;
    return result;
}

const char* status_name(Status status) noexcept {
    switch (status) {
        case Status::INVALID_INPUT: return "INVALID_INPUT";
        case Status::NO_TARGET: return "NO_TARGET";
        case Status::NO_CURRENT_OBSERVATION:
            return "NO_CURRENT_OBSERVATION";
        case Status::OCCLUDED: return "OCCLUDED";
        case Status::ASSOCIATION_MISMATCH:
            return "ASSOCIATION_MISMATCH";
        case Status::AMBIGUOUS: return "AMBIGUOUS";
        case Status::VALID: return "VALID";
    }
    return "UNKNOWN";
}

const char* semantic_kind_name(SemanticKind kind) noexcept {
    switch (kind) {
        case SemanticKind::NONE: return "NONE";
        case SemanticKind::HEAD_BOX_CENTER: return "HEAD_BOX_CENTER";
    }
    return "UNKNOWN";
}

} // namespace aim_landmark
