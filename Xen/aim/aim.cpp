#include "aim/aim.h"

#include "aim/aim_config_internal.h"

#include "log/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {

struct Observation {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    float confidence = 0.0f;
    bool head_only = false;
};

struct Track {
    std::uint64_t id = 0;
    TrackState state = TrackState::TENTATIVE;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float confidence = 0.0f;
    int hits = 0;
    int lost_frames = 0;
    bool predicted = false;
    std::chrono::steady_clock::time_point updated_at{};
};

bool finite_box(const Detection& detection) noexcept {
    return std::isfinite(detection.x1) && std::isfinite(detection.y1) &&
           std::isfinite(detection.x2) && std::isfinite(detection.y2) &&
           std::isfinite(detection.confidence) &&
           detection.x2 > detection.x1 && detection.y2 > detection.y1 &&
           detection.confidence >= 0.0f && detection.confidence <= 1.0f;
}

bool contains_class(const std::vector<int>& classes, int class_id) {
    return std::find(classes.begin(), classes.end(), class_id) != classes.end();
}

float box_iou(const Track& track, const Observation& observation) noexcept {
    const float left = std::max(track.x1, observation.x1);
    const float top = std::max(track.y1, observation.y1);
    const float right = std::min(track.x2, observation.x2);
    const float bottom = std::min(track.y2, observation.y2);
    const float intersection = std::max(0.0f, right - left) *
                               std::max(0.0f, bottom - top);
    const float track_area = (track.x2 - track.x1) * (track.y2 - track.y1);
    const float observation_area =
        (observation.x2 - observation.x1) *
        (observation.y2 - observation.y1);
    const float denominator = track_area + observation_area - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

float center_distance(float x1, float y1, float x2, float y2) noexcept {
    return std::hypot(x1 - x2, y1 - y2);
}

float clamp_delta_seconds(double value) noexcept {
    return static_cast<float>(std::clamp(value, 1.0 / 1000.0, 0.1));
}

} // namespace

struct Aim::Impl {
    explicit Impl(const AimConfig& value) : config(value) {}

    AimConfig config;
    std::vector<Track> tracks;
    std::uint64_t next_track_id = 1;
    std::uint64_t selected_track_id = 0;
    std::uint64_t leading_track_id = 0;
    int leading_frames = 0;
    int switch_cooldown = 0;
    std::uint64_t controller_track_id = 0;
    float filtered_x = 0.0f;
    float filtered_y = 0.0f;
    float residual_x = 0.0f;
    float residual_y = 0.0f;
    bool controller_initialized = false;

    bool valid_config() const noexcept {
        return aim::detail::valid_aim_config(config);
    }

    std::vector<Observation> build_observations(
            const AimFrame& frame) const {
        std::vector<const Detection*> bodies;
        std::vector<const Detection*> heads;
        for (const auto& detection : frame.detections) {
            if (!finite_box(detection) ||
                detection.confidence < config.low_confidence) {
                continue;
            }
            if (contains_class(config.head_class_ids, detection.class_id)) {
                heads.push_back(&detection);
            } else if (config.person_class_ids.empty() ||
                       contains_class(config.person_class_ids,
                                      detection.class_id)) {
                bodies.push_back(&detection);
            }
        }

        std::vector<Observation> observations;
        observations.reserve(bodies.size() + heads.size());
        std::vector<bool> head_used(heads.size(), false);
        for (const Detection* body : bodies) {
            Observation observation;
            observation.x1 = body->x1;
            observation.y1 = body->y1;
            observation.x2 = body->x2;
            observation.y2 = body->y2;
            observation.confidence = body->confidence;
            observation.aim_x = (body->x1 + body->x2) * 0.5f;
            observation.aim_y = body->y1 +
                (body->y2 - body->y1) * config.body_aim_height_ratio;

            float best_cost = std::numeric_limits<float>::max();
            std::optional<std::size_t> best_head;
            const float body_width = body->x2 - body->x1;
            const float body_height = body->y2 - body->y1;
            const float body_area = body_width * body_height;
            for (std::size_t index = 0; index < heads.size(); ++index) {
                if (head_used[index]) continue;
                const Detection& head = *heads[index];
                const float head_x = (head.x1 + head.x2) * 0.5f;
                const float head_y = (head.y1 + head.y2) * 0.5f;
                const float head_area = (head.x2 - head.x1) *
                                        (head.y2 - head.y1);
                const float area_ratio = body_area > 0.0f
                    ? head_area / body_area : 1.0f;
                if (head_x < body->x1 || head_x > body->x2 ||
                    head_y < body->y1 ||
                    head_y > body->y1 + body_height * 0.65f ||
                    area_ratio < 0.01f || area_ratio > 0.40f) {
                    continue;
                }
                const float cost = center_distance(
                    head_x, head_y,
                    (body->x1 + body->x2) * 0.5f,
                    body->y1 + body_height * 0.20f) /
                    std::max(1.0f, std::hypot(body_width, body_height));
                if (cost < best_cost) {
                    best_cost = cost;
                    best_head = index;
                }
            }
            if (best_head.has_value()) {
                const Detection& head = *heads[*best_head];
                head_used[*best_head] = true;
                observation.aim_x = (head.x1 + head.x2) * 0.5f;
                observation.aim_y = (head.y1 + head.y2) * 0.5f;
                observation.confidence =
                    std::max(observation.confidence, head.confidence);
            }
            observations.push_back(observation);
        }

        // 没有身体框时，头部观测可以延续既有轨迹；未匹配的低置信度头部
        // 不会创建确认轨迹，避免同一人物在头身框之间产生两个稳定 ID。
        for (std::size_t index = 0; index < heads.size(); ++index) {
            if (head_used[index]) continue;
            const Detection& head = *heads[index];
            observations.push_back({
                head.x1, head.y1, head.x2, head.y2,
                (head.x1 + head.x2) * 0.5f,
                (head.y1 + head.y2) * 0.5f,
                head.confidence, true});
        }
        return observations;
    }

    void predict_tracks(std::chrono::steady_clock::time_point now) noexcept {
        for (auto& track : tracks) {
            const float dt = clamp_delta_seconds(
                std::chrono::duration<double>(now - track.updated_at).count());
            const float dx = track.vx * dt;
            const float dy = track.vy * dt;
            track.x1 += dx;
            track.x2 += dx;
            track.y1 += dy;
            track.y2 += dy;
            track.aim_x += dx;
            track.aim_y += dy;
            track.predicted = true;
        }
    }

    void associate_stage(const std::vector<Observation>& observations,
                         bool high_stage, float diagonal,
                         std::vector<bool>& track_matched,
                         std::vector<bool>& observation_matched,
                         std::chrono::steady_clock::time_point now) {
        struct Edge {
            float cost = 0.0f;
            std::size_t track_index = 0;
            std::size_t observation_index = 0;
        };
        std::vector<Edge> edges;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            if (track_matched[ti]) continue;
            for (std::size_t oi = 0; oi < observations.size(); ++oi) {
                if (observation_matched[oi]) continue;
                const bool high = observations[oi].confidence >=
                                  config.high_confidence;
                if (high != high_stage) continue;
                const float track_x = (tracks[ti].x1 + tracks[ti].x2) * 0.5f;
                const float track_y = (tracks[ti].y1 + tracks[ti].y2) * 0.5f;
                const float observation_x =
                    (observations[oi].x1 + observations[oi].x2) * 0.5f;
                const float observation_y =
                    (observations[oi].y1 + observations[oi].y2) * 0.5f;
                const float normalized_distance = center_distance(
                    track_x, track_y, observation_x, observation_y) /
                    std::max(1.0f, diagonal);
                const float iou = box_iou(tracks[ti], observations[oi]);
                if (iou < config.min_iou &&
                    normalized_distance > config.max_center_distance) {
                    continue;
                }
                edges.push_back({(1.0f - iou) * 0.55f +
                                     normalized_distance * 0.45f,
                                 ti, oi});
            }
        }
        std::sort(edges.begin(), edges.end(),
                  [](const Edge& lhs, const Edge& rhs) {
                      return lhs.cost < rhs.cost;
                  });
        for (const auto& edge : edges) {
            if (track_matched[edge.track_index] ||
                observation_matched[edge.observation_index]) {
                continue;
            }
            Track& track = tracks[edge.track_index];
            const Observation& observation =
                observations[edge.observation_index];
            const float old_x = (track.x1 + track.x2) * 0.5f;
            const float old_y = (track.y1 + track.y2) * 0.5f;
            const float new_x = (observation.x1 + observation.x2) * 0.5f;
            const float new_y = (observation.y1 + observation.y2) * 0.5f;
            const float dt = clamp_delta_seconds(
                std::chrono::duration<double>(now - track.updated_at).count());
            track.vx = track.vx * 0.65f + (new_x - old_x) / dt * 0.35f;
            track.vy = track.vy * 0.65f + (new_y - old_y) / dt * 0.35f;
            track.x1 = observation.x1;
            track.y1 = observation.y1;
            track.x2 = observation.x2;
            track.y2 = observation.y2;
            track.aim_x = observation.aim_x;
            track.aim_y = observation.aim_y;
            track.confidence = observation.confidence;
            track.updated_at = now;
            track.predicted = false;
            track.lost_frames = 0;
            ++track.hits;
            if (track.hits >= config.min_confirmed_hits) {
                track.state = TrackState::CONFIRMED;
            }
            track_matched[edge.track_index] = true;
            observation_matched[edge.observation_index] = true;
        }
    }

    void update_tracks(const std::vector<Observation>& observations,
                       const AimFrame& frame) {
        predict_tracks(frame.captured_at);
        std::vector<bool> track_matched(tracks.size(), false);
        std::vector<bool> observation_matched(observations.size(), false);
        const float diagonal = std::hypot(
            static_cast<float>(frame.roi_width),
            static_cast<float>(frame.roi_height));
        associate_stage(observations, true, diagonal,
                        track_matched, observation_matched,
                        frame.captured_at);
        associate_stage(observations, false, diagonal,
                        track_matched, observation_matched,
                        frame.captured_at);

        for (std::size_t index = 0; index < tracks.size(); ++index) {
            if (track_matched[index]) continue;
            Track& track = tracks[index];
            ++track.lost_frames;
            if (track.state == TrackState::CONFIRMED ||
                track.state == TrackState::LOST) {
                track.state = TrackState::LOST;
                track.confidence *= 0.85f;
            }
        }

        for (std::size_t index = 0; index < observations.size(); ++index) {
            if (observation_matched[index] ||
                observations[index].confidence < config.high_confidence) {
                continue;
            }
            const Observation& observation = observations[index];
            tracks.push_back({
                next_track_id_advance(),
                config.min_confirmed_hits <= 1
                    ? TrackState::CONFIRMED
                    : TrackState::TENTATIVE,
                observation.x1, observation.y1,
                observation.x2, observation.y2,
                observation.aim_x, observation.aim_y,
                0.0f, 0.0f, observation.confidence,
                1, 0, false, frame.captured_at});
        }

        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
            [&](const Track& track) {
                const int limit = track.state == TrackState::TENTATIVE
                    ? 1 : config.max_lost_frames;
                return track.lost_frames > limit;
            }), tracks.end());
    }

    std::uint64_t next_track_id_advance() noexcept {
        const std::uint64_t value = next_track_id++;
        if (next_track_id == 0) next_track_id = 1;
        return value;
    }

    Track* select_target(const AimFrame& frame) noexcept {
        if (switch_cooldown > 0) --switch_cooldown;
        const float center_x = frame.control_center_x;
        const float center_y = frame.control_center_y;
        const float diagonal = std::max(
            1.0f,
            std::hypot(
                frame.roi_width *
                    frame.source_pixels_per_roi_pixel_x * 0.5f,
                frame.roi_height *
                    frame.source_pixels_per_roi_pixel_y * 0.5f));
        auto score = [&](const Track& track) {
            float value = std::hypot(
                (track.aim_x - center_x) *
                    frame.source_pixels_per_roi_pixel_x,
                (track.aim_y - center_y) *
                    frame.source_pixels_per_roi_pixel_y) / diagonal;
            value += (1.0f - track.confidence) * 0.25f;
            if (track.predicted) value += 0.30f;
            if (track.id == selected_track_id) value -= 0.15f;
            return value;
        };

        Track* current = nullptr;
        Track* best = nullptr;
        float current_score = std::numeric_limits<float>::max();
        float best_score = std::numeric_limits<float>::max();
        for (auto& track : tracks) {
            if (track.state == TrackState::TENTATIVE) continue;
            const float value = score(track);
            if (track.id == selected_track_id) {
                current = &track;
                current_score = value;
            }
            if (value < best_score) {
                best = &track;
                best_score = value;
            }
        }

        if (!current) {
            selected_track_id = best ? best->id : 0;
            leading_track_id = 0;
            leading_frames = 0;
            return best;
        }
        if (!best || best->id == current->id || switch_cooldown > 0 ||
            best_score >= current_score * (1.0f - config.switch_margin)) {
            leading_track_id = 0;
            leading_frames = 0;
            return current;
        }
        if (leading_track_id == best->id) {
            ++leading_frames;
        } else {
            leading_track_id = best->id;
            leading_frames = 1;
        }
        if (leading_frames >= config.switch_confirm_frames) {
            selected_track_id = best->id;
            leading_track_id = 0;
            leading_frames = 0;
            switch_cooldown = config.switch_cooldown_frames;
            return best;
        }
        return current;
    }

    void reset_controller() noexcept {
        controller_track_id = 0;
        filtered_x = 0.0f;
        filtered_y = 0.0f;
        residual_x = 0.0f;
        residual_y = 0.0f;
        controller_initialized = false;
    }

    bool control(const AimFrame& frame, const Track& track,
                 AimCommand& command) noexcept {
        if (controller_track_id != track.id) {
            reset_controller();
            controller_track_id = track.id;
        }
        const float error_x =
            (track.aim_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float error_y =
            (track.aim_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        // deadzone_pixels 与 counts_per_pixel 始终以主机完整 FOV 像素为单位，
        // 不随 OBS 编码尺寸或辅机显示器分辨率变化。
        if (std::hypot(error_x, error_y) <= config.deadzone_pixels) {
            filtered_x = 0.0f;
            filtered_y = 0.0f;
            residual_x = 0.0f;
            residual_y = 0.0f;
            return false;
        }

        const float gain = track.predicted ? config.predicted_gain : 1.0f;
        const float desired_x = error_x * config.counts_per_pixel_x * gain;
        const float desired_y = error_y * config.counts_per_pixel_y * gain;
        if (!controller_initialized) {
            filtered_x = desired_x;
            filtered_y = desired_y;
            controller_initialized = true;
        } else {
            filtered_x += (desired_x - filtered_x) * config.smoothing;
            filtered_y += (desired_y - filtered_y) * config.smoothing;
        }
        filtered_x = std::clamp(filtered_x,
            -config.max_counts_per_frame, config.max_counts_per_frame);
        filtered_y = std::clamp(filtered_y,
            -config.max_counts_per_frame, config.max_counts_per_frame);

        const float quantized_x = filtered_x + residual_x;
        const float quantized_y = filtered_y + residual_y;
        command.sequence = frame.sequence;
        command.captured_at = frame.captured_at;
        command.dx_counts = static_cast<int>(std::lround(quantized_x));
        command.dy_counts = static_cast<int>(std::lround(quantized_y));
        residual_x = quantized_x - command.dx_counts;
        residual_y = quantized_y - command.dy_counts;
        return command.dx_counts != 0 || command.dy_counts != 0;
    }

    void reset_all() noexcept {
        tracks.clear();
        next_track_id = 1;
        selected_track_id = 0;
        leading_track_id = 0;
        leading_frames = 0;
        switch_cooldown = 0;
        reset_controller();
    }
};

const char* AimStatusName(AimStatus status) noexcept {
    switch (status) {
        case AimStatus::NOT_RUN: return "NOT_RUN";
        case AimStatus::SUCCESS: return "SUCCESS";
        case AimStatus::INVALID_INPUT: return "INVALID_INPUT";
        case AimStatus::TRACKING_FAILED: return "TRACKING_FAILED";
        case AimStatus::CONTROL_FAILED: return "CONTROL_FAILED";
    }
    return "UNKNOWN";
}

Aim::Aim(const AimConfig& config) : impl_(std::make_unique<Impl>(config)) {
    Log::register_module("aim", LogLevel::INFO);
}

Aim::~Aim() = default;
Aim::Aim(Aim&&) noexcept = default;
Aim& Aim::operator=(Aim&&) noexcept = default;

AimResult Aim::process(const AimFrame& frame) noexcept {
    AimResult result;
    if (!impl_ || !impl_->valid_config() || frame.roi_width <= 0 ||
        frame.roi_height <= 0 || frame.sequence == 0 ||
        frame.captured_at == std::chrono::steady_clock::time_point{} ||
        !std::isfinite(frame.control_center_x) ||
        !std::isfinite(frame.control_center_y) ||
        !std::isfinite(frame.source_pixels_per_roi_pixel_x) ||
        !std::isfinite(frame.source_pixels_per_roi_pixel_y) ||
        frame.source_pixels_per_roi_pixel_x <= 0.0f ||
        frame.source_pixels_per_roi_pixel_y <= 0.0f) {
        result.status = AimStatus::INVALID_INPUT;
        return result;
    }

    using clock = std::chrono::steady_clock;
    using milliseconds = std::chrono::duration<double, std::milli>;
    const auto started = clock::now();
    try {
        const auto observations = impl_->build_observations(frame);
        const auto observed = clock::now();
        impl_->update_tracks(observations, frame);
        const auto tracked = clock::now();
        Track* target = impl_->select_target(frame);
        const auto selected = clock::now();

        if (target) {
            result.has_target = true;
            result.target = {
                target->id, target->state,
                target->x1, target->y1, target->x2, target->y2,
                target->aim_x, target->aim_y,
                target->confidence, target->predicted};
            result.has_command = impl_->control(
                frame, *target, result.command);
        } else {
            impl_->reset_controller();
        }
        const auto finished = clock::now();
        result.profile.observation_ms =
            std::chrono::duration_cast<milliseconds>(observed - started).count();
        result.profile.tracking_ms =
            std::chrono::duration_cast<milliseconds>(tracked - observed).count();
        result.profile.selection_ms =
            std::chrono::duration_cast<milliseconds>(selected - tracked).count();
        result.profile.control_ms =
            std::chrono::duration_cast<milliseconds>(finished - selected).count();
        result.profile.total_ms =
            std::chrono::duration_cast<milliseconds>(finished - started).count();
        result.status = AimStatus::SUCCESS;
        LOG_TRACE("aim", "seq={} tracks={} target={} command={} total={:.3f}ms",
                  frame.sequence, impl_->tracks.size(), result.has_target,
                  result.has_command, result.profile.total_ms);
        return result;
    } catch (...) {
        result.status = AimStatus::TRACKING_FAILED;
        return result;
    }
}

void Aim::reset() noexcept {
    if (impl_) impl_->reset_all();
}
