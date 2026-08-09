#include "aim/aim.h"

#include "aim/aim_config_internal.h"

#include "log/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    float aim_ratio_x = 0.5f;
    float aim_ratio_y = 0.5f;
    float confidence = 0.0f;
    bool head_only = false;
    bool aim_from_head = false;
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
    float aim_ratio_x = 0.5f;
    float aim_ratio_y = 0.5f;
    float vx = 0.0f;
    float vy = 0.0f;
    float confidence = 0.0f;
    int hits = 0;
    int lost_frames = 0;
    bool predicted = false;
    bool head_only = false;
    bool aim_from_head = false;
    float prediction_dt = 1.0f / 240.0f;
    std::chrono::steady_clock::time_point state_at{};
};

struct IssuedCommand {
    std::chrono::steady_clock::time_point captured_at{};
    float dx_counts = 0.0f;
    float dy_counts = 0.0f;
};

constexpr float kInvalidAssignmentCost = 1000000.0f;
constexpr float kUnmatchedAssignmentCost = 1.05f;
constexpr float kTrackPositionAlphaHigh = 0.72f;
constexpr float kTrackPositionAlphaLow = 0.45f;
constexpr float kTrackVelocityBetaHigh = 0.10f;
constexpr float kTrackVelocityBetaLow = 0.04f;
// 控制点只立即跟随框两条对边的共同位移；边缘分歧代表宽高/姿态形变，
// 仅低速校正其框内偏移。这样恒速平移仍使用原位置增益，不额外引入滞后。
constexpr float kTrackAimShapeAlphaHigh = 0.12f;
constexpr float kTrackAimShapeAlphaLow = 0.06f;
constexpr float kTrackIncoherentVelocityBetaScale = 0.40f;
constexpr float kMaxTrackSpeedDiagonalsPerSecond = 6.0f;
constexpr float kMaxObservationAgeSeconds = 0.10f;
// 比例控制对恒速目标必然保留与速度成正比的稳态误差。积分项只补偿这部分
// 基础前馈观察器：状态单位为 counts/frame。历史命令补回相机自运动后，
// 屏幕相对速度才代表世界目标运动；低通增益按真实帧间隔计算，避免帧率变化
// 改变收敛速度。独立上限覆盖实测维持命令，但不允许生成无界物理输入。
constexpr float kControllerFeedforwardObserverGainPerSecond = 20.0f;
// 轨迹速度是平滑后的估计值，存在一个采样窗口的幅值损失；有限补偿只作用
// 于观察器测量，不改变比例项或物理命令上限。
constexpr float kControllerFeedforwardVelocityScale = 2.00f;
constexpr float kControllerMovingHoldBiasMaximumCounts = 0.15f;
constexpr float kControllerFeedforwardLeakPerSecond = 1.5f;
// 第八轮真实 KMBOX 命令 P50/P95 为 3/4 counts；基础保持量低于维持速度
// 本身时，比例项必须永久保留数像素误差才能补足差额。上限覆盖实测 P95，
// 方向过零和静止确认仍负责释放历史保持量。
constexpr float kControllerFeedforwardMaximumCounts = 4.0f;
constexpr float kControllerIntegralMinimumErrorPixels = 2.0f;
constexpr std::size_t kControllerCommandHistoryCapacity = 64;
constexpr float kControllerCommandHistoryMaximumAgeSeconds = 0.10f;
// 第十四轮真实序列中，15 ms 窗口内约 15% 的命令位移足以解释基础点
// 随后越过准星的幅度。这里只用于保守预测在途位移，不改变鼠标标定。
constexpr float kControllerPendingCommandResponse = 0.15f;
constexpr float kControllerMovingVelocityThresholdPixelsPerSecond = 20.0f;
// 真实 MoveLeft 序列中，延迟向量方向仅跨越阈值反转 5 次，而原始速度
// 符号出现 459 个短游程。prediction 因此从延迟补偿点沿同一向量再前探
// 半个补偿窗口：P50/P95 约 1.42/2.09 px，既可感知又保留过冲余量。
constexpr float kPredictionDelayHorizonScale = 0.50f;
// 量化残余需要比“保持积分是否泄漏”更低的运动门槛；否则目标已在移动但
// 轨迹估计尚未达到 20 px/s 时，亚整数命令仍会被 floor 截断。
constexpr float kControllerQuantizationMotionThresholdPixelsPerSecond = 10.0f;
// 命令整形使用时间速率而不是固定帧步：240 Hz 下每帧最多变化 1 count，
// 120/60 Hz 下分别为 2/4 counts，保持不同采集刷新率下的加减速时间一致。
constexpr float kControllerMaximumSlewCountsPerSecond = 240.0f;
// 相对速度在准星跟随目标时会自然接近零，不能用单帧低速直接判定静止。
// 连续 60 帧无命令且误差处于保持带内，才允许泄漏历史移动保持量。
constexpr int kControllerStaticSettleConfirmFrames = 60;

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
    return static_cast<float>(std::clamp(value, 1.0 / 1000.0, 0.05));
}

void clamp_vector(float& x, float& y, float maximum) noexcept {
    const float magnitude = std::hypot(x, y);
    if (magnitude <= maximum || magnitude <= 0.0f) return;
    const float scale = maximum / magnitude;
    x *= scale;
    y *= scale;
}

float common_edge_motion(float first_residual,
                         float second_residual) noexcept {
    // 两条对边同向移动的重叠部分才能确定为人物平移；相反方向或仅单边
    // 变化属于框尺度/轮廓形变，不能立即写入控制点和轨迹速度。
    if (first_residual * second_residual <= 0.0f) return 0.0f;
    return std::copysign(
        std::min(std::abs(first_residual), std::abs(second_residual)),
        first_residual);
}

float normalized_position(float value, float minimum,
                          float maximum) noexcept {
    const float size = maximum - minimum;
    if (size <= 0.0f) return 0.5f;
    return std::clamp((value - minimum) / size, 0.0f, 1.0f);
}

std::pair<float, float> point_from_ratio(
        float x1, float y1, float x2, float y2,
        float ratio_x, float ratio_y) noexcept {
    return {
        x1 + (x2 - x1) * std::clamp(ratio_x, 0.0f, 1.0f),
        y1 + (y2 - y1) * std::clamp(ratio_y, 0.0f, 1.0f)};
}

// 目标数通常很小，但贪心边排序仍会在交叉和遮挡场景受输入顺序影响。
// 这里使用经典匈牙利算法求全局最小代价，并通过虚拟行列显式表达未匹配。
std::vector<int> minimum_cost_assignment(
        const std::vector<std::vector<float>>& costs) {
    if (costs.empty()) return {};
    const std::size_t row_count = costs.size();
    const std::size_t column_count = costs.front().size();
    if (column_count == 0) return std::vector<int>(row_count, -1);

    // 每个真实行和真实列都需要独立的虚拟未匹配槽；只补到 max(rows, cols)
    // 会在方阵且所有候选都被门控时被迫接受一条非法边。
    const std::size_t size = row_count + column_count;
    std::vector<std::vector<double>> square(
        size + 1, std::vector<double>(size + 1, 0.0));
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column < size; ++column) {
            if (row < row_count && column < column_count) {
                square[row + 1][column + 1] = costs[row][column];
            } else if (row < row_count) {
                square[row + 1][column + 1] = kUnmatchedAssignmentCost;
            }
        }
    }

    std::vector<double> row_potential(size + 1, 0.0);
    std::vector<double> column_potential(size + 1, 0.0);
    std::vector<std::size_t> matched_row(size + 1, 0);
    std::vector<std::size_t> previous_column(size + 1, 0);
    for (std::size_t row = 1; row <= size; ++row) {
        matched_row[0] = row;
        std::size_t column = 0;
        std::vector<double> minimum(size + 1,
                                    std::numeric_limits<double>::max());
        std::vector<bool> used(size + 1, false);
        do {
            used[column] = true;
            const std::size_t current_row = matched_row[column];
            double delta = std::numeric_limits<double>::max();
            std::size_t next_column = 0;
            for (std::size_t candidate = 1; candidate <= size; ++candidate) {
                if (used[candidate]) continue;
                const double reduced = square[current_row][candidate] -
                    row_potential[current_row] - column_potential[candidate];
                if (reduced < minimum[candidate]) {
                    minimum[candidate] = reduced;
                    previous_column[candidate] = column;
                }
                if (minimum[candidate] < delta) {
                    delta = minimum[candidate];
                    next_column = candidate;
                }
            }
            for (std::size_t candidate = 0; candidate <= size; ++candidate) {
                if (used[candidate]) {
                    row_potential[matched_row[candidate]] += delta;
                    column_potential[candidate] -= delta;
                } else {
                    minimum[candidate] -= delta;
                }
            }
            column = next_column;
        } while (matched_row[column] != 0);

        do {
            const std::size_t next_column = previous_column[column];
            matched_row[column] = matched_row[next_column];
            column = next_column;
        } while (column != 0);
    }

    std::vector<int> assignment(row_count, -1);
    for (std::size_t column = 1; column <= size; ++column) {
        const std::size_t row = matched_row[column];
        if (row == 0 || row > row_count || column > column_count) continue;
        if (costs[row - 1][column - 1] < kUnmatchedAssignmentCost) {
            assignment[row - 1] = static_cast<int>(column - 1);
        }
    }
    return assignment;
}

} // namespace

struct Aim::Impl {
    explicit Impl(const AimConfig& value) : config(value) {}

    AimConfig config;
    std::vector<Track> tracks;
    std::uint64_t next_track_id = 1;
    std::uint64_t last_sequence = 0;
    std::chrono::steady_clock::time_point last_captured_at{};
    std::uint64_t selected_track_id = 0;
    std::uint64_t leading_track_id = 0;
    int leading_frames = 0;
    int switch_cooldown = 0;
    std::uint64_t controller_track_id = 0;
    float filtered_x = 0.0f;
    float filtered_y = 0.0f;
    float shaped_x = 0.0f;
    float shaped_y = 0.0f;
    float residual_x = 0.0f;
    float residual_y = 0.0f;
    float feedforward_x = 0.0f;
    float feedforward_y = 0.0f;
    std::array<IssuedCommand, kControllerCommandHistoryCapacity>
        issued_commands{};
    std::size_t issued_command_next = 0;
    std::size_t issued_command_count = 0;
    float previous_command_x = 0.0f;
    float previous_command_y = 0.0f;
    int static_settle_frames = 0;
    std::chrono::steady_clock::time_point controller_captured_at{};
    bool controller_initialized = false;
    bool shaper_initialized = false;
    std::uint64_t lead_track_id = 0;
    bool lead_active = false;
    bool lead_ever_activated = false;
    bool lead_rearm_ready = true;
    int lead_settle_frames = 0;
    int lead_candidate_frames = 0;
    float lead_direction_x = 0.0f;
    float lead_direction_y = 0.0f;
    float acquisition_range_radius = 0.0f;
    float active_range_radius = 0.0f;
    bool range_locked = false;
    bool range_allows_control = false;

    bool valid_config() const noexcept {
        return aim::detail::valid_aim_config(config);
    }

    bool valid_frame_order(const AimFrame& frame) const noexcept {
        return last_sequence == 0 ||
            (frame.sequence > last_sequence &&
             frame.captured_at > last_captured_at);
    }

    struct LeadProjection {
        float base_x = 0.0f;
        float base_y = 0.0f;
        float delay_compensated_x = 0.0f;
        float delay_compensated_y = 0.0f;
        float delay_x = 0.0f;
        float delay_y = 0.0f;
        float delay_seconds = 0.0f;
        float final_x = 0.0f;
        float final_y = 0.0f;
        float observation_age_seconds = 0.0f;
        bool delay_active = false;
        bool active = false;
    };

    void commit_frame_order(const AimFrame& frame) noexcept {
        last_sequence = frame.sequence;
        last_captured_at = frame.captured_at;
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

        const auto detection_order = [](const Detection* left,
                                        const Detection* right) {
            const float left_x = (left->x1 + left->x2) * 0.5f;
            const float right_x = (right->x1 + right->x2) * 0.5f;
            if (left_x != right_x) return left_x < right_x;
            const float left_y = (left->y1 + left->y2) * 0.5f;
            const float right_y = (right->y1 + right->y2) * 0.5f;
            if (left_y != right_y) return left_y < right_y;
            return left->confidence > right->confidence;
        };
        std::sort(bodies.begin(), bodies.end(), detection_order);
        std::sort(heads.begin(), heads.end(), detection_order);

        std::vector<std::vector<float>> head_costs(
            bodies.size(), std::vector<float>(heads.size(),
                                              kInvalidAssignmentCost));
        for (std::size_t body_index = 0;
             body_index < bodies.size(); ++body_index) {
            const Detection& body = *bodies[body_index];
            const float body_width = body.x2 - body.x1;
            const float body_height = body.y2 - body.y1;
            const float body_area = body_width * body_height;
            const float expected_x = (body.x1 + body.x2) * 0.5f;
            const float expected_y = body.y1 + body_height * 0.20f;
            for (std::size_t head_index = 0;
                 head_index < heads.size(); ++head_index) {
                const Detection& head = *heads[head_index];
                const float head_x = (head.x1 + head.x2) * 0.5f;
                const float head_y = (head.y1 + head.y2) * 0.5f;
                const float head_area = (head.x2 - head.x1) *
                                        (head.y2 - head.y1);
                const float area_ratio = body_area > 0.0f
                    ? head_area / body_area : 1.0f;
                if (head_x < body.x1 || head_x > body.x2 ||
                    head_y < body.y1 ||
                    head_y > body.y1 + body_height * 0.65f ||
                    area_ratio < 0.01f || area_ratio > 0.40f) {
                    continue;
                }
                head_costs[body_index][head_index] = center_distance(
                    head_x, head_y, expected_x, expected_y) /
                    std::max(1.0f, std::hypot(body_width, body_height));
            }
        }
        const std::vector<int> head_assignment =
            minimum_cost_assignment(head_costs);

        std::vector<Observation> observations;
        observations.reserve(bodies.size() + heads.size());
        std::vector<bool> head_used(heads.size(), false);
        for (std::size_t body_index = 0;
             body_index < bodies.size(); ++body_index) {
            const Detection* body = bodies[body_index];
            Observation observation;
            observation.x1 = body->x1;
            observation.y1 = body->y1;
            observation.x2 = body->x2;
            observation.y2 = body->y2;
            observation.confidence = body->confidence;
            observation.aim_ratio_x = 0.5f;
            observation.aim_ratio_y = config.body_aim_height_ratio;
            if (body_index < head_assignment.size() &&
                head_assignment[body_index] >= 0) {
                const std::size_t head_index = static_cast<std::size_t>(
                    head_assignment[body_index]);
                const Detection& head = *heads[head_index];
                head_used[head_index] = true;
                // 头框只参与头身归一化关联，不直接把头中心作为瞄点；基础瞄点的
                // 高度和横向有效范围统一由 AimConfig 控制，避免不同检测框尺度造成跳变。
                observation.aim_ratio_x = 0.5f;
                observation.aim_ratio_y = config.body_aim_height_ratio;
                observation.confidence =
                    std::max(observation.confidence, head.confidence);
                observation.aim_from_head = true;
            }
            const auto [aim_x, aim_y] = point_from_ratio(
                observation.x1, observation.y1,
                observation.x2, observation.y2,
                observation.aim_ratio_x, observation.aim_ratio_y);
            observation.aim_x = aim_x;
            observation.aim_y = aim_y;
            observations.push_back(observation);
        }

        // 没有身体框时，头部观测可以延续既有轨迹；未匹配的低置信度头部
        // 不会创建确认轨迹，避免同一人物在头身框之间产生两个稳定 ID。
        for (std::size_t index = 0; index < heads.size(); ++index) {
            if (head_used[index]) continue;
            const Detection& head = *heads[index];
            Observation observation;
            observation.x1 = head.x1;
            observation.y1 = head.y1;
            observation.x2 = head.x2;
            observation.y2 = head.y2;
            observation.aim_ratio_x = 0.5f;
            observation.aim_ratio_y = config.body_aim_height_ratio;
            observation.aim_x = head.x1 + (head.x2 - head.x1) *
                observation.aim_ratio_x;
            observation.aim_y = head.y1 + (head.y2 - head.y1) *
                observation.aim_ratio_y;
            observation.confidence = head.confidence;
            observation.head_only = true;
            observation.aim_from_head = true;
            observations.push_back(observation);
        }
        return observations;
    }

    void predict_tracks(std::chrono::steady_clock::time_point now,
                        float diagonal) noexcept {
        for (auto& track : tracks) {
            const float dt = clamp_delta_seconds(
                std::chrono::duration<double>(now - track.state_at).count());
            float dx = track.vx * dt;
            float dy = track.vy * dt;
            clamp_vector(dx, dy, diagonal * 0.25f);
            track.x1 += dx;
            track.x2 += dx;
            track.y1 += dy;
            track.y2 += dy;
            track.aim_x += dx;
            track.aim_y += dy;
            track.prediction_dt = dt;
            track.state_at = now;
            track.predicted = true;
        }
    }

    void update_matched_track(Track& track,
                              const Observation& observation,
                              float diagonal) noexcept {
        const bool high = observation.confidence >= config.high_confidence;
        const float alpha = high
            ? kTrackPositionAlphaHigh : kTrackPositionAlphaLow;
        const float beta = high
            ? kTrackVelocityBetaHigh : kTrackVelocityBetaLow;
        const bool box_semantics_changed =
            track.head_only != observation.head_only;
        const float track_center_x = (track.x1 + track.x2) * 0.5f;
        const float track_center_y = (track.y1 + track.y2) * 0.5f;
        const float observation_center_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_center_y =
            (observation.y1 + observation.y2) * 0.5f;
        const float center_motion_residual_x =
            observation_center_x - track_center_x;
        const float center_motion_residual_y =
            observation_center_y - track_center_y;
        float stable_motion_residual_x = center_motion_residual_x;
        float stable_motion_residual_y = center_motion_residual_y;
        float velocity_beta_x = beta;
        float velocity_beta_y = beta;

        if (!box_semantics_changed) {
            const float x1_residual = observation.x1 - track.x1;
            const float x2_residual = observation.x2 - track.x2;
            const float y1_residual = observation.y1 - track.y1;
            const float y2_residual = observation.y2 - track.y2;
            stable_motion_residual_x = common_edge_motion(
                x1_residual, x2_residual);
            stable_motion_residual_y = common_edge_motion(
                y1_residual, y2_residual);
            if (x1_residual * x2_residual <= 0.0f) {
                velocity_beta_x *= kTrackIncoherentVelocityBetaScale;
            }
            if (y1_residual * y2_residual <= 0.0f) {
                velocity_beta_y *= kTrackIncoherentVelocityBetaScale;
            }
        }

        // 身体框短时消失、只剩头框时保留既有身体尺度，只用头部观测平移状态。
        // 否则下一帧身体框恢复会制造一次无意义的尺度突变并破坏多目标关联。
        if (observation.head_only && !track.head_only) {
            stable_motion_residual_x = observation.aim_x - track.aim_x;
            stable_motion_residual_y = observation.aim_y - track.aim_y;
            track.x1 += stable_motion_residual_x * alpha;
            track.x2 += stable_motion_residual_x * alpha;
            track.y1 += stable_motion_residual_y * alpha;
            track.y2 += stable_motion_residual_y * alpha;
        } else {
            track.x1 += (observation.x1 - track.x1) * alpha;
            track.y1 += (observation.y1 - track.y1) * alpha;
            track.x2 += (observation.x2 - track.x2) * alpha;
            track.y2 += (observation.y2 - track.y2) * alpha;
            track.head_only = observation.head_only;
        }

        // 身体框是头身模型的稳定坐标系。头框出现时只平滑更新框内归一化
        // 瞄点；头框连续缺失期间保留既有比例，避免在头中心和身体默认点间跳变。
        if (!observation.head_only) {
            if (observation.aim_from_head) {
                const float ratio_alpha = track.aim_from_head
                    ? alpha : std::min(alpha, 0.25f);
                track.aim_ratio_x +=
                    (observation.aim_ratio_x - track.aim_ratio_x) * ratio_alpha;
                track.aim_ratio_y +=
                    (observation.aim_ratio_y - track.aim_ratio_y) * ratio_alpha;
                track.aim_from_head = true;
            } else if (!track.aim_from_head) {
                track.aim_ratio_x = observation.aim_ratio_x;
                track.aim_ratio_y = observation.aim_ratio_y;
            }
        } else if (track.head_only) {
            track.aim_ratio_x = 0.5f;
            track.aim_ratio_y = 0.5f;
            track.aim_from_head = true;
        } else {
            const float observed_ratio_x = normalized_position(
                observation.aim_x, track.x1, track.x2);
            const float observed_ratio_y = normalized_position(
                observation.aim_y, track.y1, track.y2);
            const float ratio_alpha = std::min(alpha, 0.25f);
            track.aim_ratio_x +=
                (observed_ratio_x - track.aim_ratio_x) * ratio_alpha;
            track.aim_ratio_y +=
                (observed_ratio_y - track.aim_ratio_y) * ratio_alpha;
            track.aim_from_head = true;
        }
        const auto [observed_aim_x, observed_aim_y] = point_from_ratio(
            track.x1, track.y1, track.x2, track.y2,
            track.aim_ratio_x, track.aim_ratio_y);
        if (box_semantics_changed) {
            // 头框和身体框的坐标语义不同，切换时直接采用已归一化的新点，
            // 避免把合法尺度切换长期滞留在旧框内。
            track.aim_x = observed_aim_x;
            track.aim_y = observed_aim_y;
        } else {
            // predict_tracks() 已按速度完成本帧推进。共同边缘位移使用与
            // 关联框相同的增益立即校正；剩余差值只可能来自框内形变或
            // 缓慢尺度变化，因此使用独立低增益，不拖慢真实平移响应。
            track.aim_x += stable_motion_residual_x * alpha;
            track.aim_y += stable_motion_residual_y * alpha;
            const float shape_alpha = high
                ? kTrackAimShapeAlphaHigh : kTrackAimShapeAlphaLow;
            track.aim_x += (observed_aim_x - track.aim_x) * shape_alpha;
            track.aim_y += (observed_aim_y - track.aim_y) * shape_alpha;
        }
        track.aim_x = std::clamp(track.aim_x, track.x1, track.x2);
        track.aim_y = std::clamp(track.aim_y, track.y1, track.y2);

        if (box_semantics_changed) {
            // 头框和身体框的尺度定义不同，切换时不把几何变化解释为速度。
            track.vx *= 0.5f;
            track.vy *= 0.5f;
        } else {
            // 控制锚点使用对边共同残差抑制瞬时形变；速度观察器仍以低
            // beta 消费中心残差。持续平移会跨帧累积，正负交替的步态
            // 形变则相互抵消，避免把真实运动连同轮廓噪声一起归零。
            track.vx += velocity_beta_x * center_motion_residual_x /
                track.prediction_dt;
            track.vy += velocity_beta_y * center_motion_residual_y /
                track.prediction_dt;
        }
        clamp_vector(track.vx, track.vy,
                     diagonal * kMaxTrackSpeedDiagonalsPerSecond);
        track.confidence +=
            (observation.confidence - track.confidence) * alpha;
        track.predicted = false;
        track.lost_frames = 0;
        ++track.hits;
        if (track.hits >= config.min_confirmed_hits) {
            track.state = TrackState::CONFIRMED;
        }
    }

    void associate_stage(const std::vector<Observation>& observations,
                         bool high_stage, float diagonal,
                         std::vector<bool>& track_matched,
                         std::vector<bool>& observation_matched) {
        std::vector<std::size_t> track_indices;
        std::vector<std::size_t> observation_indices;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            if (!track_matched[ti]) track_indices.push_back(ti);
        }
        for (std::size_t oi = 0; oi < observations.size(); ++oi) {
            const bool high = observations[oi].confidence >=
                              config.high_confidence;
            if (!observation_matched[oi] && high == high_stage) {
                observation_indices.push_back(oi);
            }
        }
        if (track_indices.empty() || observation_indices.empty()) return;

        std::vector<std::vector<float>> costs(
            track_indices.size(),
            std::vector<float>(observation_indices.size(),
                               kInvalidAssignmentCost));
        for (std::size_t row = 0; row < track_indices.size(); ++row) {
            const Track& track = tracks[track_indices[row]];
            const float track_x = (track.x1 + track.x2) * 0.5f;
            const float track_y = (track.y1 + track.y2) * 0.5f;
            const float track_width = track.x2 - track.x1;
            const float track_height = track.y2 - track.y1;
            for (std::size_t column = 0;
                 column < observation_indices.size(); ++column) {
                const Observation& observation =
                    observations[observation_indices[column]];
                const float observation_x =
                    (observation.x1 + observation.x2) * 0.5f;
                const float observation_y =
                    (observation.y1 + observation.y2) * 0.5f;
                const float normalized_distance = center_distance(
                    track_x, track_y, observation_x, observation_y) /
                    std::max(1.0f, diagonal);
                const float iou = box_iou(track, observation);
                if (iou < config.min_iou &&
                    normalized_distance > config.max_center_distance) {
                    continue;
                }

                float shape_cost = 0.0f;
                if (track.head_only == observation.head_only) {
                    const float observation_width =
                        observation.x2 - observation.x1;
                    const float observation_height =
                        observation.y2 - observation.y1;
                    const float width_ratio = std::max(track_width,
                        observation_width) /
                        std::max(1.0f, std::min(track_width,
                                               observation_width));
                    const float height_ratio = std::max(track_height,
                        observation_height) /
                        std::max(1.0f, std::min(track_height,
                                               observation_height));
                    shape_cost = std::clamp(
                        (std::log(width_ratio) + std::log(height_ratio)) /
                            (2.0f * std::log(4.0f)),
                        0.0f, 1.0f);
                }
                costs[row][column] = (1.0f - iou) * 0.50f +
                    normalized_distance * 0.35f + shape_cost * 0.15f;
            }
        }

        const std::vector<int> assignment = minimum_cost_assignment(costs);
        for (std::size_t row = 0; row < assignment.size(); ++row) {
            if (assignment[row] < 0) continue;
            const std::size_t track_index = track_indices[row];
            const std::size_t observation_index = observation_indices[
                static_cast<std::size_t>(assignment[row])];
            if (track_matched[track_index] ||
                observation_matched[observation_index]) {
                continue;
            }
            update_matched_track(tracks[track_index],
                                 observations[observation_index], diagonal);
            track_matched[track_index] = true;
            observation_matched[observation_index] = true;
        }
    }

    void update_tracks(const std::vector<Observation>& observations,
                       const AimFrame& frame) {
        const float diagonal = std::hypot(
            static_cast<float>(frame.roi_width),
            static_cast<float>(frame.roi_height));
        predict_tracks(frame.captured_at, diagonal);
        std::vector<bool> track_matched(tracks.size(), false);
        std::vector<bool> observation_matched(observations.size(), false);
        associate_stage(observations, true, diagonal,
                        track_matched, observation_matched);
        associate_stage(observations, false, diagonal,
                        track_matched, observation_matched);

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
                observation.aim_ratio_x, observation.aim_ratio_y,
                0.0f, 0.0f, observation.confidence,
                1, 0, false, observation.head_only,
                observation.aim_from_head, 1.0f / 240.0f,
                frame.captured_at});
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
        acquisition_range_radius = std::max(
            1.0f, std::min(frame.roi_width, frame.roi_height) * 0.5f *
                config.acquisition_range_percent / 100.0f);
        active_range_radius = acquisition_range_radius;
        range_locked = false;
        range_allows_control = false;
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

        const auto range_distance = [&](const Track& track) {
            return std::hypot(
                track.aim_x - center_x, track.aim_y - center_y);
        };

        Track* current = nullptr;
        for (auto& track : tracks) {
            if (track.id == selected_track_id &&
                track.state != TrackState::TENTATIVE) {
                current = &track;
                break;
            }
        }
        if (current && frame.lock_active) {
            constexpr float kMinimumLockedRangeRatio = 0.35f;
            constexpr float kBoxMarginRatio = 0.50f;
            const float current_distance = range_distance(*current);
            const float box_radius = std::hypot(
                current->x2 - current->x1,
                current->y2 - current->y1) * 0.5f;
            const float minimum_radius =
                acquisition_range_radius * kMinimumLockedRangeRatio;
            active_range_radius = std::clamp(
                std::max(minimum_radius,
                         current_distance + box_radius * kBoxMarginRatio),
                minimum_radius, acquisition_range_radius);
            range_locked = true;
        }

        Track* best = nullptr;
        float current_score = current
            ? score(*current) : std::numeric_limits<float>::max();
        float best_score = std::numeric_limits<float>::max();
        for (auto& track : tracks) {
            if (track.state == TrackState::TENTATIVE) continue;
            const float value = score(track);
            // 新挑战者必须由当前帧真实观测支持，不能因为另一条滑行轨迹的
            // 外推分数暂时更优就切换锁定。动态范围仅筛选挑战者和控制，
            // 所有轨迹仍已在前序观测/估计阶段完整更新。
            if (!track.predicted &&
                range_distance(track) <= active_range_radius &&
                value < best_score) {
                best = &track;
                best_score = value;
            }
        }

        if (!current) {
            selected_track_id = best ? best->id : 0;
            leading_track_id = 0;
            leading_frames = 0;
            if (best) {
                range_allows_control =
                    range_distance(*best) <= active_range_radius;
            }
            return best;
        }
        range_allows_control =
            range_distance(*current) <= active_range_radius;
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
            range_allows_control =
                range_distance(*best) <= active_range_radius;
            return best;
        }
        return current;
    }

    void record_issued_command(const AimFrame& frame,
                               float dx_counts,
                               float dy_counts) noexcept {
        // lock_active=false 时 Runtime 不会发送物理命令，历史必须记录零而
        // 不是预计算结果，否则观察器会补偿一段从未发生的相机自运动。
        IssuedCommand& entry = issued_commands[issued_command_next];
        entry.captured_at = frame.captured_at;
        entry.dx_counts = frame.lock_active ? dx_counts : 0.0f;
        entry.dy_counts = frame.lock_active ? dy_counts : 0.0f;
        issued_command_next =
            (issued_command_next + 1U) % issued_commands.size();
        issued_command_count = std::min(
            issued_command_count + 1U, issued_commands.size());
    }

    std::pair<float, float> delayed_issued_command(
            std::chrono::steady_clock::time_point captured_at) const
            noexcept {
        const float delay_seconds = config.enable_delay_compensation
            ? config.control_delay_ms / 1000.0f : 0.0f;
        const auto effective_at = captured_at -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(delay_seconds));
        const IssuedCommand* best = nullptr;
        for (std::size_t offset = 0; offset < issued_command_count; ++offset) {
            const std::size_t index =
                (issued_command_next + issued_commands.size() - 1U - offset) %
                issued_commands.size();
            const IssuedCommand& candidate = issued_commands[index];
            if (candidate.captured_at <= effective_at) {
                best = &candidate;
                break;
            }
        }
        if (!best) return {0.0f, 0.0f};
        const float age_seconds = static_cast<float>(
            std::chrono::duration<double>(effective_at - best->captured_at)
                .count());
        if (age_seconds > kControllerCommandHistoryMaximumAgeSeconds) {
            return {0.0f, 0.0f};
        }
        return {best->dx_counts, best->dy_counts};
    }

    std::pair<float, float> pending_issued_command_sum(
            std::chrono::steady_clock::time_point captured_at) const
            noexcept {
        if (!config.enable_delay_compensation ||
            config.control_delay_ms <= 0.0f) {
            return {0.0f, 0.0f};
        }
        const auto effective_at = captured_at -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(
                    config.control_delay_ms / 1000.0f));
        float pending_x = 0.0f;
        float pending_y = 0.0f;
        for (std::size_t offset = 0; offset < issued_command_count; ++offset) {
            const std::size_t index =
                (issued_command_next + issued_commands.size() - 1U - offset) %
                issued_commands.size();
            const IssuedCommand& candidate = issued_commands[index];
            if (candidate.captured_at <= effective_at) break;
            if (candidate.captured_at <= captured_at) {
                pending_x += candidate.dx_counts;
                pending_y += candidate.dy_counts;
            }
        }
        return {pending_x, pending_y};
    }

    void reset_controller() noexcept {
        controller_track_id = 0;
        filtered_x = 0.0f;
        filtered_y = 0.0f;
        shaped_x = 0.0f;
        shaped_y = 0.0f;
        residual_x = 0.0f;
        residual_y = 0.0f;
        feedforward_x = 0.0f;
        feedforward_y = 0.0f;
        issued_commands = {};
        issued_command_next = 0;
        issued_command_count = 0;
        previous_command_x = 0.0f;
        previous_command_y = 0.0f;
        static_settle_frames = 0;
        controller_captured_at = {};
        controller_initialized = false;
        shaper_initialized = false;
    }

    LeadProjection projected_aim_point(
            const AimFrame& frame, const Track& track,
            std::chrono::steady_clock::time_point control_at) noexcept {
        const float half_range = config.body_aim_range_percent / 200.0f;
        const float range_min_x = track.x1 + (track.x2 - track.x1) *
            (0.5f - half_range);
        const float range_max_x = track.x1 + (track.x2 - track.x1) *
            (0.5f + half_range);
        // 基础点直接取状态估计点在配置内窗中的位置，不按速度逐帧补偿；
        // 这样检测抖动不会把瞄点反复推向内窗两侧。预测层仍独立处理提前量。
        const float base_x = std::clamp(track.aim_x, range_min_x, range_max_x);
        const float base_y = std::clamp(track.aim_y, track.y1, track.y2);
        LeadProjection projection;
        projection.base_x = base_x;
        projection.base_y = base_y;
        projection.delay_compensated_x = base_x;
        projection.delay_compensated_y = base_y;
        projection.final_x = base_x;
        projection.final_y = base_y;
        projection.observation_age_seconds = static_cast<float>(std::clamp(
            std::chrono::duration<double>(control_at - frame.captured_at).count(),
            0.0, static_cast<double>(kMaxObservationAgeSeconds)));
        const float box_diagonal = std::hypot(
            track.x2 - track.x1, track.y2 - track.y1);
        if (config.enable_delay_compensation &&
            track.state == TrackState::CONFIRMED && !track.predicted) {
            const float requested_delay_seconds =
                projection.observation_age_seconds +
                config.control_delay_ms / 1000.0f;
            projection.delay_seconds = std::clamp(
                requested_delay_seconds, 0.0f,
                config.max_delay_compensation_ms / 1000.0f);
            projection.delay_x = track.vx * projection.delay_seconds;
            projection.delay_y = track.vy * projection.delay_seconds;
            if (controller_track_id == track.id) {
                const auto [pending_x, pending_y] =
                    pending_issued_command_sum(frame.captured_at);
                // 当前基础点尚未包含延迟窗内命令的相机位移。提前扣除该
                // 位移可在批量命令生效前减速，避免越过后只能停发反拉。
                projection.delay_x -= pending_x *
                    kControllerPendingCommandResponse /
                    config.counts_per_pixel_x /
                    frame.source_pixels_per_roi_pixel_x;
                projection.delay_y -= pending_y *
                    kControllerPendingCommandResponse /
                    config.counts_per_pixel_y /
                    frame.source_pixels_per_roi_pixel_y;
            }
            // 延迟补偿只投影控制点，不修改原始框和关联状态。距离门禁按当前
            // 目标尺度归一化，速度尖峰不能生成无界物理输入。
            clamp_vector(
                projection.delay_x, projection.delay_y,
                box_diagonal *
                    config.max_delay_compensation_percent / 100.0f);
            projection.delay_active = projection.delay_seconds > 0.0f &&
                std::hypot(projection.delay_x, projection.delay_y) > 0.0f;
            projection.delay_compensated_x = base_x + projection.delay_x;
            projection.delay_compensated_y = base_y + projection.delay_y;
            projection.final_x = projection.delay_compensated_x;
            projection.final_y = projection.delay_compensated_y;
        }
        if (lead_track_id != track.id) {
            lead_track_id = track.id;
            lead_active = false;
            lead_ever_activated = false;
            lead_rearm_ready = true;
            lead_settle_frames = 0;
            lead_candidate_frames = 0;
            lead_direction_x = 0.0f;
            lead_direction_y = 0.0f;
        }
        if (!config.enable_prediction) {
            lead_active = false;
            lead_ever_activated = false;
            lead_rearm_ready = true;
            lead_settle_frames = 0;
            lead_candidate_frames = 0;
            return projection;
        }

        if (projection.delay_active && !track.predicted) {
            // 延迟向量已经同时包含轨迹位移和在途命令扣减，真实序列中的
            // 方向远比逐帧原始速度稳定。prediction 直接以补偿后的点为
            // 原点，沿该有界向量继续前探；不再等待框尺度误差积累后才启动。
            const float delay_magnitude = std::hypot(
                projection.delay_x, projection.delay_y);
            const float activation_distance = std::max(
                0.25f, config.deadzone_pixels * 0.50f);
            if (delay_magnitude <= activation_distance) {
                lead_active = false;
                lead_candidate_frames = 0;
                return projection;
            }
            const bool direction_reversed = lead_active &&
                lead_direction_x * projection.delay_x +
                    lead_direction_y * projection.delay_y <= 0.0f;
            if (direction_reversed) {
                lead_active = false;
                lead_candidate_frames = 0;
                return projection;
            }
            if (!lead_active) {
                ++lead_candidate_frames;
                const int required_frames = lead_ever_activated ? 2 : 1;
                if (lead_candidate_frames < required_frames) return projection;
                lead_active = true;
                lead_ever_activated = true;
                lead_candidate_frames = 0;
            }
            lead_direction_x = projection.delay_x / delay_magnitude;
            lead_direction_y = projection.delay_y / delay_magnitude;
            float lead_x = projection.delay_x *
                kPredictionDelayHorizonScale;
            float lead_y = projection.delay_y *
                kPredictionDelayHorizonScale;
            clamp_vector(
                lead_x, lead_y,
                box_diagonal * config.max_prediction_lead_percent / 100.0f);
            projection.final_x = projection.delay_compensated_x + lead_x;
            projection.final_y = projection.delay_compensated_y + lead_y;
            projection.active = true;
            return projection;
        }

        // 未启用延迟补偿时保留原有的准星闭环迟滞语义。此分支没有可
        // 复用的延迟向量，只能按相对速度和观测年龄做保守预测。
        const float error_x =
            projection.delay_compensated_x - frame.control_center_x;
        const float error_y =
            projection.delay_compensated_y - frame.control_center_y;
        const float error_magnitude = std::hypot(error_x, error_y);
        const float velocity_magnitude = std::hypot(track.vx, track.vy);
        const float alignment = error_x * track.vx + error_y * track.vy;
        const float longitudinal_error = velocity_magnitude > 1.0f
            ? std::fabs(alignment) / velocity_magnitude : 0.0f;
        const float enter_distance = std::max(
            config.deadzone_pixels * 2.0f, box_diagonal * 0.12f);
        const float exit_distance = std::max(
            config.deadzone_pixels, box_diagonal * 0.05f);
        const bool moving_away = velocity_magnitude > 1.0f && alignment > 0.0f;
        const bool velocity_reversed = lead_active &&
            lead_direction_x * track.vx + lead_direction_y * track.vy <= 0.0f;
        const float lead_axis_error =
            std::fabs(error_x * lead_direction_x +
                      error_y * lead_direction_y);

        if (lead_active) {
            if (!moving_away || velocity_reversed ||
                error_magnitude <= exit_distance) {
                lead_active = false;
                lead_rearm_ready = false;
                lead_settle_frames = 0;
                lead_candidate_frames = 0;
            }
        } else {
            // 准星移动会反向改变目标的屏幕速度。预测退出后若立即按该相对
            // 速度重新前探，会形成“越过目标→反拉→归位→再次前探”的极限环。
            // 只有基础点沿原预测方向连续回到中心小范围后才重新武装。
            if (!lead_rearm_ready) {
                // 只判断上一预测方向上的归位；身体默认瞄点在正交方向的
                // 天然偏移不能永久阻塞水平预测恢复。
                if (lead_axis_error <= exit_distance) {
                    ++lead_settle_frames;
                    constexpr int kLeadSettleConfirmFrames = 5;
                    if (lead_settle_frames >= kLeadSettleConfirmFrames) {
                        lead_rearm_ready = true;
                    }
                } else {
                    lead_settle_frames = 0;
                }
            }
            if (lead_rearm_ready && !track.predicted && moving_away &&
                longitudinal_error >= enter_distance) {
                ++lead_candidate_frames;
                constexpr int kLeadReenterConfirmFrames = 2;
                const int required_frames = lead_ever_activated
                    ? kLeadReenterConfirmFrames : 1;
                if (lead_candidate_frames >= required_frames) {
                    lead_active = true;
                    lead_ever_activated = true;
                    lead_settle_frames = 0;
                    lead_candidate_frames = 0;
                }
            } else {
                lead_candidate_frames = 0;
            }
        }
        if (!lead_active) return projection;

        lead_direction_x = track.vx / velocity_magnitude;
        lead_direction_y = track.vy / velocity_magnitude;
        const float lead_gain = track.predicted
            ? config.predicted_gain : 1.0f;
        float lead_x = track.vx * projection.observation_age_seconds * lead_gain;
        float lead_y = track.vy * projection.observation_age_seconds * lead_gain;
        // 用户只需要控制一个与目标尺度归一化的最远提前距离。高速、低速
        // 或加速度变化都不能绕过该向量硬门禁生成大范围提前点。
        clamp_vector(
            lead_x, lead_y,
            box_diagonal * config.max_prediction_lead_percent / 100.0f);
        // 基础追踪点必须锁在模型框内；预测提前点允许越过框边界，否则高速
        // 目标只能追到当前观测位置，失去提前量的意义。
        projection.final_x = projection.delay_compensated_x + lead_x;
        projection.final_y = projection.delay_compensated_y + lead_y;
        projection.active = true;
        return projection;
    }

    bool control(const AimFrame& frame, const Track& track,
                 float base_x, float base_y,
                 float tracking_x, float tracking_y,
                 float aim_x, float aim_y,
                 AimCommand& command) noexcept {
        if (controller_track_id != track.id) {
            reset_controller();
            controller_track_id = track.id;
        }
        if (track.predicted && !config.enable_prediction) {
            // 短时丢框仍禁止发送物理命令，但同一轨迹的基础保持量不能重置。
            // 否则重新观测后会从纯比例控制重新学习恒速偏差，形成一次明显
            // 落后。这里只按真实帧间隔泄漏积分并推进控制时钟；滤波、整形和
            // 亚整数残余保持冻结，恢复帧仍受当前误差方向门禁约束。
            const float controller_dt = controller_captured_at ==
                    std::chrono::steady_clock::time_point{}
                ? track.prediction_dt
                : clamp_delta_seconds(std::chrono::duration<double>(
                      frame.captured_at - controller_captured_at).count());
            const float leak = std::exp(
                -kControllerFeedforwardLeakPerSecond * controller_dt);
            feedforward_x *= leak;
            feedforward_y *= leak;
            previous_command_x = 0.0f;
            previous_command_y = 0.0f;
            static_settle_frames = 0;
            controller_captured_at = frame.captured_at;
            record_issued_command(frame, 0.0f, 0.0f);
            return false;
        }
        const float error_x =
            (aim_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float error_y =
            (aim_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        const float tracking_error_x =
            (tracking_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float tracking_error_y =
            (tracking_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        const float base_error_x =
            (base_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float base_error_y =
            (base_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        // deadzone_pixels 与 counts_per_pixel 始终以主机完整 FOV 像素为单位，
        // 不随 OBS 编码尺寸或辅机显示器分辨率变化。恒速目标进入死区后不能
        // 立即清空已学习的积分，否则会形成“追上、停发、落后、再追”的周期。
        const bool inside_deadzone =
            std::hypot(error_x, error_y) <= config.deadzone_pixels;
        const float gain = track.predicted ? config.predicted_gain : 1.0f;
        const float controller_dt = controller_captured_at ==
                std::chrono::steady_clock::time_point{}
            ? track.prediction_dt
            : clamp_delta_seconds(std::chrono::duration<double>(
                  frame.captured_at - controller_captured_at).count());
        controller_captured_at = frame.captured_at;
        const float proportional_x =
            error_x * config.counts_per_pixel_x * gain;
        const float proportional_y =
            error_y * config.counts_per_pixel_y * gain;
        const float hold_band = std::max(
            kControllerIntegralMinimumErrorPixels,
            config.deadzone_pixels * 1.5f);
        const bool previous_command_zero =
            previous_command_x == 0.0f && previous_command_y == 0.0f;
        const bool low_relative_motion =
            std::hypot(track.vx, track.vy) <=
                kControllerMovingVelocityThresholdPixelsPerSecond;
        const bool within_hold_band =
            std::hypot(base_error_x, base_error_y) <= hold_band;
        if (previous_command_zero && low_relative_motion &&
            within_hold_band) {
            static_settle_frames = std::min(
                static_settle_frames + 1,
                kControllerStaticSettleConfirmFrames);
        } else {
            static_settle_frames = 0;
        }

        const auto [delayed_command_x, delayed_command_y] =
            delayed_issued_command(frame.captured_at);
        // track.v* 是目标相对屏幕的速度，包含历史鼠标命令造成的相机运动。
        // 将预计当前生效的历史命令补回后，measurement 才是世界目标在本帧
        // 需要的维持量。该观察器不依赖基础点过零或相对速度符号猜测反转。
        const auto update_feedforward = [&](float base_error,
                                            float relative_velocity,
                                            float source_scale,
                                            float counts_per_pixel,
                                            float delayed_command,
                                            float& feedforward) {
            if (config.enable_prediction &&
                !config.enable_delay_compensation) {
                // 无延迟向量时 prediction 只能按相对速度生成提前量；若再
                // 叠加基础速度前馈，同一相机运动会被两条路径重复补偿，
                // 静止归位会形成闭环极限环，因此只保留比例与整形。
                // 启用延迟补偿时 prediction 仅延伸其既有向量，基础 tracking
                // 前馈必须继续工作，否则开关 prediction 会直接造成严重滞后。
                feedforward *= std::exp(
                    -kControllerFeedforwardLeakPerSecond * controller_dt);
            } else if (track.predicted) {
                feedforward *= std::exp(
                    -kControllerFeedforwardLeakPerSecond * controller_dt);
            } else if (track.state != TrackState::CONFIRMED) {
                feedforward = 0.0f;
            } else {
                const float measurement = delayed_command +
                    relative_velocity * source_scale * controller_dt *
                        counts_per_pixel *
                        kControllerFeedforwardVelocityScale;
                const float alpha = 1.0f - std::exp(
                    -kControllerFeedforwardObserverGainPerSecond *
                        controller_dt);
                feedforward += (measurement - feedforward) * alpha;
                // 只有相对速度已静止时，基础点反向才可视为真正归位。
                // 动态过冲仍交给前馈观测器判断，避免把相机反馈误当成目标反转。
                if (std::fabs(base_error) > hold_band &&
                    base_error * feedforward < 0.0f &&
                    std::fabs(measurement) < 0.80f) {
                    feedforward = 0.0f;
                }
                // 基础点在保持带内且相对速度朝向准星时，观测主要由相机
                // 执行旧命令造成。加速释放这部分残余，避免静止目标留下
                // 亚整数前馈而出现视觉往返；目标继续离开准星时不触发。
                if (std::fabs(base_error) <= hold_band &&
                    base_error * relative_velocity < 0.0f) {
                    feedforward *= std::exp(
                        -20.0f * controller_dt);
                }
                feedforward = std::clamp(
                    feedforward,
                    -kControllerFeedforwardMaximumCounts,
                    kControllerFeedforwardMaximumCounts);
            }
        };
        update_feedforward(
            base_error_x, track.vx, frame.source_pixels_per_roi_pixel_x,
            config.counts_per_pixel_x, delayed_command_x, feedforward_x);
        update_feedforward(
            base_error_y, track.vy, frame.source_pixels_per_roi_pixel_y,
            config.counts_per_pixel_y, delayed_command_y, feedforward_y);
        float desired_x = proportional_x + feedforward_x;
        float desired_y = proportional_y + feedforward_y;
        // 移动目标在保持带内仍需承担量化后的平均维持量。仅在观察器已
        // 学到前馈且基础误差与其同向时加入很小的偏置，静止目标和真实
        // 反转不继承该偏置，避免重新引入周期性抖动。
        if (std::fabs(feedforward_x) > 0.05f &&
            base_error_x * feedforward_x > 0.0f &&
            std::fabs(base_error_x) <= hold_band) {
            desired_x += std::clamp(
                base_error_x * config.counts_per_pixel_x * 0.25f,
                -kControllerMovingHoldBiasMaximumCounts,
                kControllerMovingHoldBiasMaximumCounts);
        }
        if (std::fabs(feedforward_y) > 0.05f &&
            base_error_y * feedforward_y > 0.0f &&
            std::fabs(base_error_y) <= hold_band) {
            desired_y += std::clamp(
                base_error_y * config.counts_per_pixel_y * 0.25f,
                -kControllerMovingHoldBiasMaximumCounts,
                kControllerMovingHoldBiasMaximumCounts);
        }
        // 只允许一种跨最终点符号的保持命令：延迟投影点位于保持带内，且
        // 命令仍明确朝向尚未过零的基础点。无延迟控制、基础点真实过零或
        // 投影点离开保持带时均恢复逐轴比例方向，避免积分推动闭环远离目标。
        const bool allow_delayed_base_hold =
            config.enable_delay_compensation &&
            config.control_delay_ms > 0.0f;
        if (desired_x * error_x <= 0.0f &&
            (!allow_delayed_base_hold || std::fabs(error_x) > hold_band ||
             desired_x * base_error_x <= 0.0f)) {
            desired_x = proportional_x;
        }
        if (desired_y * error_y <= 0.0f &&
            (!allow_delayed_base_hold || std::fabs(error_y) > hold_band ||
             desired_y * base_error_y <= 0.0f)) {
            desired_y = proportional_y;
        }
        // 逐轴允许基础维持后仍需满足二维生产契约。若正交轴误差使合成向量
        // 在保持带外整体背离最终点，回退纯比例向量；保持带内不触发，避免
        // 再次切断本轮需要保护的恒速前馈。
        if (std::hypot(error_x, error_y) > hold_band &&
            desired_x * error_x + desired_y * error_y <= 0.0f) {
            desired_x = proportional_x;
            desired_y = proportional_y;
        }
        const bool moving_away_x =
            tracking_error_x * track.vx > 0.0f &&
            std::fabs(track.vx) >
                kControllerMovingVelocityThresholdPixelsPerSecond;
        const bool moving_away_y =
            tracking_error_y * track.vy > 0.0f &&
            std::fabs(track.vy) >
                kControllerMovingVelocityThresholdPixelsPerSecond;
        if (inside_deadzone && std::hypot(feedforward_x, feedforward_y) < 0.05f &&
            std::hypot(shaped_x, shaped_y) <= 1.0f &&
            !moving_away_x && !moving_away_y) {
            filtered_x = 0.0f;
            filtered_y = 0.0f;
            shaped_x = 0.0f;
            shaped_y = 0.0f;
            residual_x = 0.0f;
            residual_y = 0.0f;
            previous_command_x = 0.0f;
            previous_command_y = 0.0f;
            controller_captured_at = frame.captured_at;
            record_issued_command(frame, 0.0f, 0.0f);
            return false;
        }
        if (!controller_initialized) {
            filtered_x = desired_x;
            filtered_y = desired_y;
            controller_initialized = true;
        } else {
            filtered_x += (desired_x - filtered_x) * config.smoothing;
            filtered_y += (desired_y - filtered_y) * config.smoothing;
        }
        // 鼠标后端消费二维相对位移，限幅也必须作用于向量模长；逐轴限幅会让
        // 对角线命令达到配置上限的 sqrt(2) 倍。
        clamp_vector(filtered_x, filtered_y, config.max_counts_per_frame);

        const bool smooth_delayed_motion =
            config.enable_delay_compensation &&
            config.control_delay_ms > 0.0f &&
            std::hypot(track.vx, track.vy) >
                kControllerMovingVelocityThresholdPixelsPerSecond;
        if (!shaper_initialized) {
            shaped_x = filtered_x;
            shaped_y = filtered_y;
            shaper_initialized = true;
        } else {
            float delta_x = filtered_x - shaped_x;
            float delta_y = filtered_y - shaped_y;
            // 比例滤波负责低频响应，整形器按真实 dt 限制相邻物理命令的可见阶跃。
            const float maximum_delta = smooth_delayed_motion
                ? std::max(
                    0.25f,
                    kControllerMaximumSlewCountsPerSecond * controller_dt)
                : std::max(
                    1.0f, config.max_counts_per_frame *
                        std::max(0.10f, config.smoothing));
            clamp_vector(delta_x, delta_y, maximum_delta);
            shaped_x += delta_x;
            shaped_y += delta_y;
            clamp_vector(shaped_x, shaped_y, config.max_counts_per_frame);
        }

        // 轨迹整形不能让历史动量继续把准星推向当前控制点的反方向。方向始终对准本帧
        // 基础点或预测点；静态模式限制到当前需求，延迟移动模式保留连续减速幅度。
        const float desired_magnitude = std::hypot(desired_x, desired_y);
        const float shaped_magnitude = std::hypot(shaped_x, shaped_y);
        if (desired_magnitude <= 0.0f || shaped_magnitude <= 0.0f ||
            shaped_x * desired_x + shaped_y * desired_y <= 0.0f) {
            shaped_x = 0.0f;
            shaped_y = 0.0f;
            residual_x = 0.0f;
            residual_y = 0.0f;
        } else {
            // 方向立即对准当前控制点，幅度则保留整形后的连续减速轨迹。后续逐轴量化上限仍按
            // 当前 desired 限制整数命令，因此不会因平滑状态超过当前需求而增加额外物理步长。
            const float safe_magnitude = smooth_delayed_motion
                ? shaped_magnitude
                : std::min(shaped_magnitude, desired_magnitude);
            shaped_x = desired_x / desired_magnitude * safe_magnitude;
            shaped_y = desired_y / desired_magnitude * safe_magnitude;
        }

        float quantized_x = shaped_x + residual_x;
        float quantized_y = shaped_y + residual_y;
        // 量化残余属于上一帧误差方向。目标越过准星或转向时，旧残余不能
        // 把最终整数命令推回当前控制点的反方向；逐轴丢弃反向分量后，二维
        // 点积必然保持朝向当前基础点或预测点。
        if (desired_x == 0.0f || quantized_x * desired_x < 0.0f) {
            quantized_x = 0.0f;
        }
        if (desired_y == 0.0f || quantized_y * desired_y < 0.0f) {
            quantized_y = 0.0f;
        }
        command.sequence = frame.sequence;
        command.captured_at = frame.captured_at;
        command.dx_counts = static_cast<int>(std::lround(quantized_x));
        command.dy_counts = static_cast<int>(std::lround(quantized_y));
        // 亚整数残余只有在后续帧允许发出 1 count 时才能完成时间分摊。
        // 对确认中的移动轴使用 ceil；静止轴仍使用 floor，避免静态目标在
        // 小误差内越过瞄点。方向门禁和二维单帧上限继续在前后两侧生效。
        const auto quantized_axis_limit = [&](float desired,
                float feedforward, float relative_velocity,
                float previous_command) {
            const float magnitude = std::fabs(desired);
            int limit = static_cast<int>(std::floor(magnitude));
            // 屏幕相对速度会被相机执行旧命令反向，不能再据此判断世界
            // 目标运动方向。观察器前馈与当前需求同向时，亚整数维持量
            // 必须允许 ceil 后跨帧分摊，否则会重新形成停发等待窗口。
            const bool observed_world_motion =
                std::fabs(feedforward) > 0.01f &&
                desired * feedforward > 0.0f;
            const bool direct_relative_motion =
                !config.enable_delay_compensation &&
                std::fabs(relative_velocity) >
                    kControllerQuantizationMotionThresholdPixelsPerSecond &&
                desired * relative_velocity > 0.0f;
            if ((observed_world_motion || direct_relative_motion) &&
                magnitude > 0.0f) {
                limit = static_cast<int>(std::ceil(magnitude));
            }
            // 轨迹正在向零点收敛时，速度会与剩余纠偏方向相反。只要剩余纠偏仍与上一帧同向，
            // 整数上限每帧最多下降 1 count，确保减速序列经过 …3、2、1、0 而不是直接停发。
            if (desired * previous_command > 0.0f &&
                std::fabs(previous_command) > 1.0f && magnitude > 0.0f) {
                limit = std::max(
                    limit, static_cast<int>(std::fabs(previous_command)) - 1);
            }
            return limit;
        };
        const int maximum_x = quantized_axis_limit(
            desired_x, feedforward_x, track.vx, previous_command_x);
        const int maximum_y = quantized_axis_limit(
            desired_y, feedforward_y, track.vy, previous_command_y);
        command.dx_counts = std::clamp(
            command.dx_counts, -maximum_x, maximum_x);
        command.dy_counts = std::clamp(
            command.dy_counts, -maximum_y, maximum_y);
        while (std::hypot(static_cast<float>(command.dx_counts),
                          static_cast<float>(command.dy_counts)) >
               config.max_counts_per_frame) {
            if (std::abs(command.dx_counts) >=
                std::abs(command.dy_counts) && command.dx_counts != 0) {
                command.dx_counts += command.dx_counts > 0 ? -1 : 1;
            } else if (command.dy_counts != 0) {
                command.dy_counts += command.dy_counts > 0 ? -1 : 1;
            } else {
                break;
            }
        }
        // 浮点 desired 合法并不保证逐轴整数化后仍满足二维方向契约。
        // 保持带外剔除每个背离最终点的轴分量，避免正交轴四舍五入后让合成
        // 命令整体推离目标；被剔除轴的残余也必须作废，不能延后再次发出。
        if (std::hypot(error_x, error_y) > hold_band) {
            if (command.dx_counts * error_x <= 0.0f) {
                command.dx_counts = 0;
                quantized_x = 0.0f;
            }
            if (command.dy_counts * error_y <= 0.0f) {
                command.dy_counts = 0;
                quantized_y = 0.0f;
            }
        }
        // 二维整形可能在合成向量仍然朝向目标时，让单个轴从旧方向直接跳到
        // 新方向。真实延迟窗口会放大这个跳变，表现为视觉抖动；每个轴必须
        // 先经过一个零命令帧，再允许沿新方向输出。被截断的量化残余同步清除，
        // 避免下一帧把旧方向重新带回来。
        if (previous_command_x != 0.0f && command.dx_counts != 0 &&
            std::signbit(previous_command_x) !=
                std::signbit(static_cast<float>(command.dx_counts))) {
            command.dx_counts = 0;
            quantized_x = 0.0f;
        }
        if (previous_command_y != 0.0f && command.dy_counts != 0 &&
            std::signbit(previous_command_y) !=
                std::signbit(static_cast<float>(command.dy_counts))) {
            command.dy_counts = 0;
            quantized_y = 0.0f;
        }
        residual_x = quantized_x - command.dx_counts;
        residual_y = quantized_y - command.dy_counts;
        previous_command_x = static_cast<float>(command.dx_counts);
        previous_command_y = static_cast<float>(command.dy_counts);
        record_issued_command(
            frame, previous_command_x, previous_command_y);
        return command.dx_counts != 0 || command.dy_counts != 0;
    }

    void reset_all() noexcept {
        tracks.clear();
        next_track_id = 1;
        last_sequence = 0;
        last_captured_at = {};
        selected_track_id = 0;
        leading_track_id = 0;
        leading_frames = 0;
        switch_cooldown = 0;
        lead_track_id = 0;
        lead_active = false;
        lead_ever_activated = false;
        lead_rearm_ready = true;
        lead_settle_frames = 0;
        lead_candidate_frames = 0;
        lead_direction_x = 0.0f;
        lead_direction_y = 0.0f;
        acquisition_range_radius = 0.0f;
        active_range_radius = 0.0f;
        range_locked = false;
        range_allows_control = false;
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
        (frame.control_at != std::chrono::steady_clock::time_point{} &&
         frame.control_at < frame.captured_at) ||
        !std::isfinite(frame.control_center_x) ||
        !std::isfinite(frame.control_center_y) ||
        !std::isfinite(frame.source_pixels_per_roi_pixel_x) ||
        !std::isfinite(frame.source_pixels_per_roi_pixel_y) ||
        frame.source_pixels_per_roi_pixel_x <= 0.0f ||
        frame.source_pixels_per_roi_pixel_y <= 0.0f ||
        !impl_->valid_frame_order(frame)) {
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
        result.acquisition_range_radius = impl_->acquisition_range_radius;
        result.active_range_radius = impl_->active_range_radius;
        result.range_locked = impl_->range_locked;
        result.range_allows_control = impl_->range_allows_control;
        const auto selected = clock::now();

        if (target) {
            const auto control_at = frame.control_at ==
                    std::chrono::steady_clock::time_point{}
                ? clock::now() : frame.control_at;
            const auto projection =
                impl_->projected_aim_point(frame, *target, control_at);
            result.has_target = true;
            result.target.track_id = target->id;
            result.target.state = target->state;
            result.target.x1 = target->x1;
            result.target.y1 = target->y1;
            result.target.x2 = target->x2;
            result.target.y2 = target->y2;
            result.target.base_aim_x = projection.base_x;
            result.target.base_aim_y = projection.base_y;
            result.target.delay_compensated_aim_x =
                projection.delay_compensated_x;
            result.target.delay_compensated_aim_y =
                projection.delay_compensated_y;
            result.target.aim_x = projection.final_x;
            result.target.aim_y = projection.final_y;
            result.target.velocity_x = target->vx;
            result.target.velocity_y = target->vy;
            result.target.lead_x = projection.final_x -
                projection.delay_compensated_x;
            result.target.lead_y = projection.final_y -
                projection.delay_compensated_y;
            result.target.delay_compensation_x = projection.delay_x;
            result.target.delay_compensation_y = projection.delay_y;
            result.target.delay_compensation_ms = projection.delay_active
                ? projection.delay_seconds * 1000.0f : 0.0f;
            result.target.observation_age_ms =
                projection.observation_age_seconds * 1000.0f;
            result.target.confidence = target->confidence;
            result.target.lead_active = projection.active;
            result.target.delay_compensation_active =
                projection.delay_active;
            result.target.predicted = target->predicted;
            if (impl_->range_allows_control) {
                result.has_command = impl_->control(
                    frame, *target,
                    projection.base_x,
                    projection.base_y,
                    projection.delay_compensated_x,
                    projection.delay_compensated_y,
                    projection.final_x, projection.final_y,
                    result.command);
            } else {
                impl_->reset_controller();
            }
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
        impl_->commit_frame_order(frame);
        LOG_TRACE("aim", "seq={} tracks={} target={} command={} total={:.3f}ms",
                  frame.sequence, impl_->tracks.size(), result.has_target,
                  result.has_command, result.profile.total_ms);
        return result;
    } catch (...) {
        impl_->reset_all();
        result.status = AimStatus::TRACKING_FAILED;
        return result;
    }
}

void Aim::reset() noexcept {
    if (impl_) impl_->reset_all();
}
