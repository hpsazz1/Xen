#include "aim/aim.h"

#include "aim/aim_config_internal.h"
#include "aim/aim_prediction_internal.h"

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
    int shape_deformation_x_frames = 0;
    int shape_deformation_y_frames = 0;
    int pose_deformation_x_frames = 0;
    int pose_deformation_y_frames = 0;
    float protected_motion_direction_x = 0.0f;
    float protected_motion_direction_y = 0.0f;
    int protected_motion_x_frames = 0;
    int protected_motion_y_frames = 0;
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
// 人物姿态可能让检测框两条对边连续多帧同向摆动，单靠“对边反向即形变”
// 或“中心逐帧反号”都无法识别。有明确尺度变化且中心创新仍属于小尺度时，
// 保护基础锚点；尺度证据消失后按固定保持窗口恢复。真实匀速平移仍由轨迹
// 速度预测连续推进。
constexpr float kTrackCoherentDeformationMaximumTargetDiagonal = 0.04f;
constexpr float kTrackCoherentDeformationMinimumShapeChangeTargetDiagonal =
    0.0005f;
constexpr float kTrackCoherentDeformationMinimumShapeChangePixels = 0.05f;
// 实机姿态同向形变可持续 3～10 帧；保护必须覆盖完整形变窗口，真实平移仍由
// stable_motion_residual 和速度估计独立推进。
constexpr int kTrackCoherentDeformationHoldFrames = 10;
// 姿态段内先用低速度增益并隔离比例点回写；只有同向速度跨过实测最长
// 17 帧形变段后，才在固定窗口内连续恢复原增益。这样不会冻结真实平移，
// 也不会在阈值帧把累计位置误差一次性写回基础锚点。
constexpr float kTrackDeformationVelocityBetaScaleMinimum = 0.08f;
constexpr float kTrackDeformationVelocityBetaScaleMaximum = 0.40f;
constexpr float kTrackProtectedMotionMinimumPixelsPerSecond = 8.0f;
constexpr int kTrackProtectedMotionConfirmFrames = 18;
constexpr int kTrackProtectedMotionRampFrames = 4;
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
// 40 ms 窗口内的 pending 总和会按命令逐帧阶跃；0.12 只平滑隐藏库存
// 投影，使实测 8～10 帧命令反馈周期内逐步制动，不改变公开 prediction 点或
// 用户配置的 0.475 基础控制平滑。
constexpr float kPredictionPendingProjectionResponse = 0.12f;
constexpr float kControllerMovingVelocityThresholdPixelsPerSecond = 20.0f;
// prediction 从已经完成基础延迟补偿的点继续向前推 1.5 个控制延迟。
// 真机 Run 20260809-234450 证明半个时域的最终点虽比基础点向前 2.75 px，
// 但仍不足以覆盖 3.21 px 的比例控制稳态误差；合成闭环中的一个完整时域
// 也只让基础点领先 0.52 px，仍低于可见门槛。1.5 倍只积分独立世界速度，
// 不反读会被自身输出放大的延迟向量。
constexpr float kPredictionAdditionalHorizonScale = 1.50f;
// 控制延迟描述 KMBOX 命令到画面反馈的时域；几何投影只描述当前轨迹在
// 画面中的短时外推，两者不能被同一个配置值同时放大。真实命令响应需要
// 约 40 ms，但已经验证的几何/prediction 基准最多使用 16 ms 基础时域。
constexpr float kGeometricProjectionMaximumSeconds = 0.016f;
// 基础前馈服务于每帧跟随，响应不能为 prediction 稳定方向降速。
// prediction 单独使用慢速世界运动状态：持续运动低通，静止时快速释放。
// 这个状态只读取基础前馈，绝不回写轨迹、基础控制点或基础控制器。
constexpr float kPredictionWorldMotionGainPerSecond = 2.0f;
constexpr float kPredictionWorldMotionReleasePerSecond = 120.0f;
// 世界速度低通仍可能遇到 Provider/NDI 突发交付、几何上限切入或基础前馈
// 量化边沿。最终预测偏移单独按目标对角线/秒限速，保证基础点不动的同时
// 阻止预测点一帧从半程跳到几何上限。1.5 diagonals/s 在 240 Hz、约 100 px
// 人物框下每帧最多约 0.63 px，不改变稳定提前距离。
constexpr float kPredictionOffsetMaximumSlewDiagonalsPerSecond = 1.5f;
// prediction 退出后反拉保持只用于跨越短暂的相机反馈低谷；低运动持续超过
// 300 ms 仍没有真实反向时必须释放，才能让基础点把准星带回配置高度。
constexpr float kPredictionPullbackHoldTimeoutSeconds = 0.30f;
// 实机人物姿态会造成 3～10 帧的同向低运动窗口；五帧确认仍会把窗口
// 中间误判成停止并清零 prediction。延长到 12 帧只改变停止确认，真实
// 停止尾窗仍受快速释放增益限制，基础前馈状态不受影响。
constexpr int kPredictionStaticReleaseConfirmFrames = 12;
// 世界运动只在独立慢速状态形成至少四分之一 count 的稳定维持量后
// 才可用于 prediction；更小残余属于静止收敛和量化噪声，禁止强行前探。
constexpr float kPredictionWorldMotionMinimumCounts = 0.25f;
// 屏幕相对速度包含 KMBOX 命令造成的相机反馈，只能用于维持已经成立的
// 世界运动，不能从静止状态单独启动 prediction。60 counts/s 等价于
// 240 Hz 下每帧 0.25 count，与真实 Run 使用的世界运动门槛保持一致。
constexpr float kPredictionEstablishedWorldVelocityCountsPerSecond = 60.0f;
// 从零建立 prediction 前，命令补偿后的世界运动必须按同一方向持续至少
// 250 ms。真机姿态/归位伪运动只持续数十帧，持续移动则远长于该窗口；
// 已建立后的低谷和停止仍由原有 12 帧释放状态机处理。
constexpr float kPredictionWorldMotionEstablishmentSeconds = 0.25f;
// 正确方向的提前量也不能在观察器刚建立时整段跳入。按真实 dt 在约 33 ms
// 内线性渐入，避免 prediction 状态变化绕过物理命令阶跃门禁。
constexpr float kPredictionLeadRampPerSecond = 30.0f;
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

void move_vector_toward(float target_x, float target_y,
                        float maximum_delta,
                        float& current_x, float& current_y) noexcept {
    const float delta_x = target_x - current_x;
    const float delta_y = target_y - current_y;
    const float distance = std::hypot(delta_x, delta_y);
    if (distance <= maximum_delta || distance <= 0.0f) {
        current_x = target_x;
        current_y = target_y;
        return;
    }
    const float scale = maximum_delta / distance;
    current_x += delta_x * scale;
    current_y += delta_y * scale;
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
    float world_motion_measurement_x = 0.0f;
    float world_motion_measurement_y = 0.0f;
    // 独立 prediction 状态使用 counts/second；基础控制器前馈仍保持
    // counts/frame，二者不能混用，否则瞬时帧间隔会改变预测距离。
    float prediction_world_velocity_x = 0.0f;
    float prediction_world_velocity_y = 0.0f;
    // 符号表示候选世界方向，绝对值表示同向连续测量时长（秒）。
    float prediction_motion_candidate_x_seconds = 0.0f;
    float prediction_motion_candidate_y_seconds = 0.0f;
    bool prediction_external_motion_evidence_x = false;
    bool prediction_external_motion_evidence_y = false;
    float prediction_offset_x = 0.0f;
    float prediction_offset_y = 0.0f;
    float prediction_control_offset_x = 0.0f;
    float prediction_control_offset_y = 0.0f;
    float prediction_pending_projection_x = 0.0f;
    int prediction_low_motion_x_frames = 0;
    int prediction_low_motion_y_frames = 0;
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
    bool lead_axis_active_x = false;
    bool lead_axis_active_y = false;
    bool lead_ever_activated = false;
    bool lead_rearm_ready = true;
    bool prediction_pullback_hold_x = false;
    bool prediction_pullback_hold_y = false;
    float prediction_pullback_direction_x = 0.0f;
    float prediction_pullback_direction_y = 0.0f;
    float prediction_pullback_hold_time_x = 0.0f;
    float prediction_pullback_hold_time_y = 0.0f;
    int lead_settle_frames = 0;
    int lead_candidate_frames = 0;
    float lead_direction_x = 0.0f;
    float lead_direction_y = 0.0f;
    float delay_lead_scale = 0.0f;
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
            const bool x_edges_coherent =
                x1_residual * x2_residual > 0.0f;
            const bool y_edges_coherent =
                y1_residual * y2_residual > 0.0f;
            stable_motion_residual_x = common_edge_motion(
                x1_residual, x2_residual);
            stable_motion_residual_y = common_edge_motion(
                y1_residual, y2_residual);
            const float target_diagonal = std::max(
                1.0f, std::hypot(
                    (track.x2 - track.x1 +
                     observation.x2 - observation.x1) * 0.5f,
                    (track.y2 - track.y1 +
                     observation.y2 - observation.y1) * 0.5f));
            const float coherent_deformation_maximum =
                target_diagonal *
                kTrackCoherentDeformationMaximumTargetDiagonal;
            const float shape_change_minimum = std::max(
                kTrackCoherentDeformationMinimumShapeChangePixels,
                target_diagonal *
                    kTrackCoherentDeformationMinimumShapeChangeTargetDiagonal);
            const bool width_changed =
                std::fabs(x2_residual - x1_residual) >
                    shape_change_minimum;
            const bool height_changed =
                std::fabs(y2_residual - y1_residual) >
                    shape_change_minimum;
            const bool shape_changed = width_changed || height_changed;
            const bool pose_changed = width_changed && height_changed;
            const auto update_deformation = [=](
                    bool shape_evidence, float residual,
                    bool edges_coherent,
                    int& hold_frames) {
                const bool deformation =
                    shape_evidence && edges_coherent &&
                    std::fabs(residual) <= coherent_deformation_maximum;
                if (deformation) {
                    hold_frames = kTrackCoherentDeformationHoldFrames;
                } else if (hold_frames > 0) {
                    --hold_frames;
                }
            };
            update_deformation(
                shape_changed, center_motion_residual_x,
                x_edges_coherent,
                track.shape_deformation_x_frames);
            update_deformation(
                shape_changed, center_motion_residual_y,
                y_edges_coherent,
                track.shape_deformation_y_frames);
            update_deformation(
                pose_changed, center_motion_residual_x,
                x_edges_coherent,
                track.pose_deformation_x_frames);
            update_deformation(
                pose_changed, center_motion_residual_y,
                y_edges_coherent,
                track.pose_deformation_y_frames);
            const auto update_protected_motion = [](
                    bool shape_protected, float velocity,
                    float& direction, int& frames) {
                if (!shape_protected) {
                    direction = 0.0f;
                    frames = 0;
                    return;
                }
                if (frames >= kTrackProtectedMotionConfirmFrames +
                    kTrackProtectedMotionRampFrames) {
                    return;
                }
                if (std::fabs(velocity) <
                    kTrackProtectedMotionMinimumPixelsPerSecond) {
                    direction = 0.0f;
                    frames = 0;
                    return;
                }
                const float current_direction =
                    std::copysign(1.0f, velocity);
                if (current_direction == direction) {
                    frames = std::min(
                        frames + 1,
                        kTrackProtectedMotionConfirmFrames +
                            kTrackProtectedMotionRampFrames);
                } else {
                    direction = current_direction;
                    frames = 1;
                }
            };
            update_protected_motion(
                track.pose_deformation_x_frames > 0, track.vx,
                track.protected_motion_direction_x,
                track.protected_motion_x_frames);
            update_protected_motion(
                track.pose_deformation_y_frames > 0, track.vy,
                track.protected_motion_direction_y,
                track.protected_motion_y_frames);
            const auto protected_motion_blend = [](int frames) {
                return std::clamp(
                    static_cast<float>(
                        frames - kTrackProtectedMotionConfirmFrames) /
                        static_cast<float>(kTrackProtectedMotionRampFrames),
                    0.0f, 1.0f);
            };
            if (track.shape_deformation_x_frames > 0) {
                stable_motion_residual_x = 0.0f;
                if (x_edges_coherent) {
                    float scale =
                        kTrackDeformationVelocityBetaScaleMaximum;
                    if (track.pose_deformation_x_frames > 0) {
                        const float blend = protected_motion_blend(
                            track.protected_motion_x_frames);
                        scale = kTrackDeformationVelocityBetaScaleMinimum +
                            (kTrackDeformationVelocityBetaScaleMaximum -
                             kTrackDeformationVelocityBetaScaleMinimum) *
                                blend;
                    }
                    velocity_beta_x *= scale;
                }
            }
            if (track.shape_deformation_y_frames > 0) {
                stable_motion_residual_y = 0.0f;
                if (y_edges_coherent) {
                    float scale =
                        kTrackDeformationVelocityBetaScaleMaximum;
                    if (track.pose_deformation_y_frames > 0) {
                        const float blend = protected_motion_blend(
                            track.protected_motion_y_frames);
                        scale = kTrackDeformationVelocityBetaScaleMinimum +
                            (kTrackDeformationVelocityBetaScaleMaximum -
                             kTrackDeformationVelocityBetaScaleMinimum) *
                                blend;
                    }
                    velocity_beta_y *= scale;
                }
            }
            if (!x_edges_coherent) {
                velocity_beta_x *=
                    kTrackDeformationVelocityBetaScaleMaximum;
            }
            if (!y_edges_coherent) {
                velocity_beta_y *=
                    kTrackDeformationVelocityBetaScaleMaximum;
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
            const auto protected_shape_alpha = [=](
                    int deformation_frames, int motion_frames) {
                if (deformation_frames <= 0) return shape_alpha;
                return shape_alpha * std::clamp(
                    static_cast<float>(
                        motion_frames - kTrackProtectedMotionConfirmFrames) /
                        static_cast<float>(kTrackProtectedMotionRampFrames),
                    0.0f, 1.0f);
            };
            track.aim_x += (observed_aim_x - track.aim_x) *
                protected_shape_alpha(
                    track.pose_deformation_x_frames,
                    track.protected_motion_x_frames);
            track.aim_y += (observed_aim_y - track.aim_y) *
                protected_shape_alpha(
                    track.pose_deformation_y_frames,
                    track.protected_motion_y_frames);
        }
        track.aim_x = std::clamp(track.aim_x, track.x1, track.x2);
        track.aim_y = std::clamp(track.aim_y, track.y1, track.y2);

        if (box_semantics_changed) {
            // 头框和身体框的尺度定义不同，切换时不把几何变化解释为速度。
            track.vx *= 0.5f;
            track.vy *= 0.5f;
            track.shape_deformation_x_frames = 0;
            track.shape_deformation_y_frames = 0;
            track.pose_deformation_x_frames = 0;
            track.pose_deformation_y_frames = 0;
            track.protected_motion_direction_x = 0.0f;
            track.protected_motion_direction_y = 0.0f;
            track.protected_motion_x_frames = 0;
            track.protected_motion_y_frames = 0;
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
            // 没有相邻观测时不能跨缺帧沿用形变保持；重新匹配后必须从
            // 新的连续观测重新建立宽高与中心创新证据。
            track.shape_deformation_x_frames = 0;
            track.shape_deformation_y_frames = 0;
            track.pose_deformation_x_frames = 0;
            track.pose_deformation_y_frames = 0;
            track.protected_motion_direction_x = 0.0f;
            track.protected_motion_direction_y = 0.0f;
            track.protected_motion_x_frames = 0;
            track.protected_motion_y_frames = 0;
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

    std::pair<float, float> stable_prediction_world_velocity(
            const AimFrame& frame, const Track& track) noexcept {
        if (!frame.lock_active || controller_track_id != track.id) {
            prediction_world_velocity_x = 0.0f;
            prediction_world_velocity_y = 0.0f;
            prediction_motion_candidate_x_seconds = 0.0f;
            prediction_motion_candidate_y_seconds = 0.0f;
            prediction_external_motion_evidence_x = false;
            prediction_external_motion_evidence_y = false;
            prediction_offset_x = 0.0f;
            prediction_offset_y = 0.0f;
            prediction_low_motion_x_frames = 0;
            prediction_low_motion_y_frames = 0;
            return {0.0f, 0.0f};
        }
        // 真机 5288dd9 Run 已把开关次数降到 11 段，但仍会在预测追上、
        // 控制停发五帧后把“已到提前点”误判为人物静止，随后退回基础点并
        // 再次预测。停止只接受逐轴连续五帧同时满足低世界运动测量和低屏幕
        // 相对速度；两者任一仍在运动都说明只是相机反馈抵消，不能释放。
        // 控制是否停发不再作为证据。非零速度起伏统一慢速滤波，避免 lead
        // 每帧跳动。
        const auto stop_measurement = [&](float world_measurement,
                                          float relative_velocity,
                                          float source_scale,
                                          float counts_per_pixel,
                                          float prediction_velocity) {
            const float relative_counts = relative_velocity * source_scale *
                track.prediction_dt * counts_per_pixel *
                kControllerFeedforwardVelocityScale;
            // 命令补偿后的世界测量负责建立运动；只有该轴 prediction 已有
            // 足够强且与屏幕相对速度同向的世界速度时，才允许后者跨越低谷。
            // 否则静止目标的相机归位会在 Y 轴凭空创建 prediction，随后被
            // 反拉保护误当成真实运动方向并长期清零高度修正命令。
            const bool relative_motion_confirms_world_motion =
                std::fabs(prediction_velocity) >=
                    kPredictionEstablishedWorldVelocityCountsPerSecond &&
                relative_counts * prediction_velocity > 0.0f;
            return relative_motion_confirms_world_motion &&
                    std::fabs(relative_counts) > std::fabs(world_measurement)
                ? relative_counts : world_measurement;
        };
        const auto update_prediction_axis = [&](
                float feedforward, float world_measurement,
                float relative_velocity, float source_scale,
                float counts_per_pixel, float& prediction_velocity,
                int& low_motion_frames, float& candidate_seconds,
                bool& external_motion_evidence) {
            if (!external_motion_evidence &&
                std::fabs(candidate_seconds) <
                kPredictionWorldMotionEstablishmentSeconds) {
                if (std::fabs(world_measurement) <=
                    kPredictionWorldMotionMinimumCounts) {
                    candidate_seconds = 0.0f;
                } else {
                    const float signed_dt = std::copysign(
                        track.prediction_dt, world_measurement);
                    if (candidate_seconds * world_measurement <= 0.0f) {
                        candidate_seconds = signed_dt;
                    } else {
                        candidate_seconds += signed_dt;
                    }
                    candidate_seconds = std::clamp(
                        candidate_seconds,
                        -kPredictionWorldMotionEstablishmentSeconds,
                        kPredictionWorldMotionEstablishmentSeconds);
                }
                if (std::fabs(candidate_seconds) <
                    kPredictionWorldMotionEstablishmentSeconds) {
                    prediction_velocity = 0.0f;
                    low_motion_frames = 0;
                    return;
                }
            }
            aim::detail::update_prediction_velocity_axis(
                feedforward,
                stop_measurement(
                    world_measurement, relative_velocity, source_scale,
                    counts_per_pixel, prediction_velocity),
                track.prediction_dt, kPredictionWorldMotionMinimumCounts,
                kPredictionStaticReleaseConfirmFrames,
                kPredictionWorldMotionGainPerSecond,
                kPredictionWorldMotionReleasePerSecond,
                prediction_velocity, low_motion_frames);
            if (low_motion_frames >= kPredictionStaticReleaseConfirmFrames &&
                std::fabs(prediction_velocity) <
                    kPredictionEstablishedWorldVelocityCountsPerSecond) {
                prediction_velocity = 0.0f;
                low_motion_frames = 0;
                candidate_seconds = 0.0f;
                external_motion_evidence = false;
            }
        };
        update_prediction_axis(
            feedforward_x, world_motion_measurement_x, track.vx,
            frame.source_pixels_per_roi_pixel_x, config.counts_per_pixel_x,
            prediction_world_velocity_x, prediction_low_motion_x_frames,
            prediction_motion_candidate_x_seconds,
            prediction_external_motion_evidence_x);
        update_prediction_axis(
            feedforward_y, world_motion_measurement_y, track.vy,
            frame.source_pixels_per_roi_pixel_y, config.counts_per_pixel_y,
            prediction_world_velocity_y, prediction_low_motion_y_frames,
            prediction_motion_candidate_y_seconds,
            prediction_external_motion_evidence_y);
        if (prediction_world_velocity_x == 0.0f &&
            prediction_world_velocity_y == 0.0f) {
            return {0.0f, 0.0f};
        }
        // 独立状态已经是 counts/second；这里只换算为检测 ROI 像素/秒，
        // 调用者直接乘固定预测秒数，不再除以瞬时 prediction_dt。
        return {
            prediction_world_velocity_x /
                (config.counts_per_pixel_x *
                 frame.source_pixels_per_roi_pixel_x),
            prediction_world_velocity_y /
                (config.counts_per_pixel_y *
                 frame.source_pixels_per_roi_pixel_y)};
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
        world_motion_measurement_x = 0.0f;
        world_motion_measurement_y = 0.0f;
        prediction_world_velocity_x = 0.0f;
        prediction_world_velocity_y = 0.0f;
        prediction_motion_candidate_x_seconds = 0.0f;
        prediction_motion_candidate_y_seconds = 0.0f;
        prediction_external_motion_evidence_x = false;
        prediction_external_motion_evidence_y = false;
        prediction_offset_x = 0.0f;
        prediction_offset_y = 0.0f;
        prediction_control_offset_x = 0.0f;
        prediction_control_offset_y = 0.0f;
        prediction_pending_projection_x = 0.0f;
        prediction_low_motion_x_frames = 0;
        prediction_low_motion_y_frames = 0;
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
                std::min(config.max_delay_compensation_ms / 1000.0f,
                         kGeometricProjectionMaximumSeconds));
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
            lead_axis_active_x = false;
            lead_axis_active_y = false;
            lead_ever_activated = false;
            lead_rearm_ready = true;
            prediction_pullback_hold_x = false;
            prediction_pullback_hold_y = false;
            prediction_pullback_direction_x = 0.0f;
            prediction_pullback_direction_y = 0.0f;
            prediction_pullback_hold_time_x = 0.0f;
            prediction_pullback_hold_time_y = 0.0f;
            lead_settle_frames = 0;
            lead_candidate_frames = 0;
            lead_direction_x = 0.0f;
            lead_direction_y = 0.0f;
            delay_lead_scale = 0.0f;
            prediction_world_velocity_x = 0.0f;
            prediction_world_velocity_y = 0.0f;
            prediction_motion_candidate_x_seconds = 0.0f;
            prediction_motion_candidate_y_seconds = 0.0f;
            prediction_external_motion_evidence_x = false;
            prediction_external_motion_evidence_y = false;
            prediction_offset_x = 0.0f;
            prediction_offset_y = 0.0f;
            prediction_low_motion_x_frames = 0;
            prediction_low_motion_y_frames = 0;
        }
        if (!config.enable_prediction) {
            lead_active = false;
            lead_axis_active_x = false;
            lead_axis_active_y = false;
            lead_ever_activated = false;
            lead_rearm_ready = true;
            prediction_pullback_hold_x = false;
            prediction_pullback_hold_y = false;
            prediction_pullback_direction_x = 0.0f;
            prediction_pullback_direction_y = 0.0f;
            prediction_pullback_hold_time_x = 0.0f;
            prediction_pullback_hold_time_y = 0.0f;
            lead_settle_frames = 0;
            lead_candidate_frames = 0;
            delay_lead_scale = 0.0f;
            prediction_world_velocity_x = 0.0f;
            prediction_world_velocity_y = 0.0f;
            prediction_motion_candidate_x_seconds = 0.0f;
            prediction_motion_candidate_y_seconds = 0.0f;
            prediction_external_motion_evidence_x = false;
            prediction_external_motion_evidence_y = false;
            prediction_offset_x = 0.0f;
            prediction_offset_y = 0.0f;
            prediction_low_motion_x_frames = 0;
            prediction_low_motion_y_frames = 0;
            return projection;
        }
        if (config.enable_delay_compensation && !frame.lock_active) {
            // 松开锁定后 Runtime 不发送物理输出；prediction 的逐轴反拉保持
            // 也必须在此清空，避免下一次按住时继承上一次移动方向。
            lead_active = false;
            lead_axis_active_x = false;
            lead_axis_active_y = false;
            lead_ever_activated = false;
            lead_rearm_ready = true;
            prediction_pullback_hold_x = false;
            prediction_pullback_hold_y = false;
            prediction_pullback_direction_x = 0.0f;
            prediction_pullback_direction_y = 0.0f;
            prediction_pullback_hold_time_x = 0.0f;
            prediction_pullback_hold_time_y = 0.0f;
            lead_settle_frames = 0;
            lead_candidate_frames = 0;
            lead_direction_x = 0.0f;
            lead_direction_y = 0.0f;
            delay_lead_scale = 0.0f;
            prediction_world_velocity_x = 0.0f;
            prediction_world_velocity_y = 0.0f;
            prediction_motion_candidate_x_seconds = 0.0f;
            prediction_motion_candidate_y_seconds = 0.0f;
            prediction_external_motion_evidence_x = false;
            prediction_external_motion_evidence_y = false;
            prediction_offset_x = 0.0f;
            prediction_offset_y = 0.0f;
            prediction_low_motion_x_frames = 0;
            prediction_low_motion_y_frames = 0;
            return projection;
        }

        if (config.enable_delay_compensation &&
            config.control_delay_ms > 0.0f && !track.predicted) {
            // 真实失败 Run 证明：延迟向量长度会被 prediction 命令造成的镜头
            // 反馈从 P50 2.878 px 放大到 8.099 px，再乘固定倍率会构成正反馈。
            // 因此延迟点只是叠加起点；额外位移由独立世界运动速度在 1.5 个
            // 控制延迟时域内积分，不会再反读 prediction 放大后的延迟向量。
            // 瞬时延迟向量恰好为零时仍属于同一模式，不能因此清空逐轴反拉
            // 保持并退回无延迟状态机。
            const auto [world_velocity_x, world_velocity_y] =
                stable_prediction_world_velocity(frame, track);
            const float world_velocity_magnitude = std::hypot(
                world_velocity_x, world_velocity_y);
            const float horizon_seconds =
                std::min(projection.delay_seconds,
                         kGeometricProjectionMaximumSeconds) *
                kPredictionAdditionalHorizonScale;
            const float activation_distance_x = std::max(
                {0.25f, config.deadzone_pixels * 0.50f,
                 0.50f /
                     (config.counts_per_pixel_x *
                      frame.source_pixels_per_roi_pixel_x)});
            const float activation_distance_y = std::max(
                {0.25f, config.deadzone_pixels * 0.50f,
                 0.50f /
                     (config.counts_per_pixel_y *
                      frame.source_pixels_per_roi_pixel_y)});
            const float desired_lead_x = world_velocity_x * horizon_seconds;
            const float desired_lead_y = world_velocity_y * horizon_seconds;
            const float forecast_limit_percent = std::min(
                config.max_prediction_lead_percent,
                config.max_delay_compensation_percent *
                    kPredictionAdditionalHorizonScale);
            const float reverse_takeover_distance = std::max(
                config.deadzone_pixels * 2.0f,
                box_diagonal * forecast_limit_percent / 100.0f);
            const auto update_pullback_hold = [&](
                    float world_velocity, float base_error,
                    float hold_direction, bool& hold,
                    float& hold_time, float release_velocity,
                    bool allow_timeout) {
                if (!hold) {
                    hold_time = 0.0f;
                    return;
                }
                hold_time += track.prediction_dt;
                if (hold_direction * world_velocity < 0.0f &&
                    -base_error * hold_direction >=
                        reverse_takeover_distance) {
                    hold = false;
                    hold_time = 0.0f;
                    return;
                }
                // 低运动长期持续表示 prediction 已经失效，而不是一帧相机
                // 反馈低谷。超时后释放逐轴停发，避免 Y 轴永久停在错误高度。
                if (allow_timeout &&
                    hold_time >= kPredictionPullbackHoldTimeoutSeconds &&
                    std::fabs(world_velocity) <= release_velocity) {
                    hold = false;
                    hold_time = 0.0f;
                }
            };
            update_pullback_hold(
                world_velocity_x, base_x - frame.control_center_x,
                prediction_pullback_direction_x,
                prediction_pullback_hold_x,
                prediction_pullback_hold_time_x,
                kPredictionEstablishedWorldVelocityCountsPerSecond /
                    (config.counts_per_pixel_x *
                     frame.source_pixels_per_roi_pixel_x),
                false);
            update_pullback_hold(
                world_velocity_y, base_y - frame.control_center_y,
                prediction_pullback_direction_y,
                prediction_pullback_hold_y,
                prediction_pullback_hold_time_y,
                kPredictionEstablishedWorldVelocityCountsPerSecond /
                    (config.counts_per_pixel_y *
                     frame.source_pixels_per_roi_pixel_y),
                true);
            // 二维 prediction 可能仍因垂直姿态保持 active，但水平世界状态
            // 已先衰减到门槛以下。每轴必须在自己的前探消失时保存方向，
            // 不能等两个轴同时归零，否则垂直 lead 会掩盖水平拉回。
            if (lead_active && lead_axis_active_x &&
                std::fabs(desired_lead_x) <= activation_distance_x &&
                std::fabs(lead_direction_x) > 0.001f) {
                prediction_pullback_hold_x = true;
                prediction_pullback_direction_x =
                    std::copysign(1.0f, lead_direction_x);
                prediction_pullback_hold_time_x = 0.0f;
            }
            if (lead_active && lead_axis_active_y &&
                std::fabs(desired_lead_y) <= activation_distance_y &&
                std::fabs(lead_direction_y) > 0.001f) {
                prediction_pullback_hold_y = true;
                prediction_pullback_direction_y =
                    std::copysign(1.0f, lead_direction_y);
                prediction_pullback_hold_time_y = 0.0f;
            }
            if ((std::fabs(desired_lead_x) <= activation_distance_x &&
                 std::fabs(desired_lead_y) <= activation_distance_y) ||
                world_velocity_magnitude <= 0.0f) {
                // 已经形成提前后，低运动释放不能立刻回到基础点发送反向命令；
                // 保持停发，等待真实反向或锁定结束，切断“提前—拉回—再预测”。
                if (lead_active && std::fabs(lead_direction_x) > 0.001f) {
                    prediction_pullback_hold_x = true;
                    prediction_pullback_direction_x =
                        std::copysign(1.0f, lead_direction_x);
                    prediction_pullback_hold_time_x = 0.0f;
                }
                if (lead_active && lead_axis_active_y &&
                    std::fabs(lead_direction_y) > 0.001f) {
                    prediction_pullback_hold_y = true;
                    prediction_pullback_direction_y =
                        std::copysign(1.0f, lead_direction_y);
                    prediction_pullback_hold_time_y = 0.0f;
                }
                lead_active = false;
                lead_axis_active_x = false;
                lead_axis_active_y = false;
                lead_candidate_frames = 0;
                delay_lead_scale = 0.0f;
                prediction_offset_x = 0.0f;
                prediction_offset_y = 0.0f;
                return projection;
            }
            const bool opposite_world_direction =
                lead_active &&
                lead_direction_x * world_velocity_x +
                    lead_direction_y * world_velocity_y <= 0.0f;
            if (opposite_world_direction) {
                const bool reverse_takeover_x =
                    std::fabs(lead_direction_x) > 0.001f &&
                    lead_direction_x * world_velocity_x < 0.0f &&
                    lead_direction_x * track.vx < 0.0f &&
                    -(base_x - frame.control_center_x) * lead_direction_x >=
                        reverse_takeover_distance;
                const bool reverse_takeover_y =
                    lead_axis_active_y &&
                    std::fabs(lead_direction_y) > 0.001f &&
                    lead_direction_y * world_velocity_y < 0.0f &&
                    lead_direction_y * track.vy < 0.0f &&
                    -(base_y - frame.control_center_y) * lead_direction_y >=
                        reverse_takeover_distance;
                // 闭环相机反馈会让世界速度观察器短时反号。基础点尚未沿
                // 旧方向反侧越过接管距离时，这不是人物真实反向：保留上个
                // 有界预测偏移，并由反拉门禁停发相反命令。否则每次伪反向
                // 都会把偏移瞬时清零，形成约二十至三十帧的周期性抖动。
                if (!reverse_takeover_x && !reverse_takeover_y) {
                    if (std::fabs(lead_direction_x) > 0.001f) {
                        prediction_pullback_hold_x = true;
                        prediction_pullback_direction_x =
                            std::copysign(1.0f, lead_direction_x);
                        prediction_pullback_hold_time_x = 0.0f;
                    }
                    if (lead_axis_active_y &&
                        std::fabs(lead_direction_y) > 0.001f) {
                        prediction_pullback_hold_y = true;
                        prediction_pullback_direction_y =
                            std::copysign(1.0f, lead_direction_y);
                        prediction_pullback_hold_time_y = 0.0f;
                    }
                    float held_lead_x =
                        prediction_offset_x - projection.delay_x;
                    float held_lead_y =
                        prediction_offset_y - projection.delay_y;
                    clamp_vector(
                        held_lead_x, held_lead_y,
                        box_diagonal *
                            config.max_prediction_lead_percent / 100.0f);
                    prediction_offset_x = projection.delay_x + held_lead_x;
                    prediction_offset_y = projection.delay_y + held_lead_y;
                    projection.final_x = base_x + prediction_offset_x;
                    projection.final_y = base_y + prediction_offset_y;
                    projection.active = true;
                    return projection;
                }
                // 确认某一轴真实接管后，其他轴仍保留旧方向的反拉保护，
                // 下一帧再由新世界方向建立新的预测偏移。
                if (std::fabs(lead_direction_x) > 0.001f &&
                    !reverse_takeover_x) {
                    prediction_pullback_hold_x = true;
                    prediction_pullback_direction_x =
                        std::copysign(1.0f, lead_direction_x);
                    prediction_pullback_hold_time_x = 0.0f;
                }
                if (lead_axis_active_y &&
                    std::fabs(lead_direction_y) > 0.001f &&
                    !reverse_takeover_y) {
                    prediction_pullback_hold_y = true;
                    prediction_pullback_direction_y =
                        std::copysign(1.0f, lead_direction_y);
                    prediction_pullback_hold_time_y = 0.0f;
                }
                lead_active = false;
                lead_axis_active_x = false;
                lead_axis_active_y = false;
                lead_candidate_frames = 0;
                delay_lead_scale = 0.0f;
                prediction_offset_x = 0.0f;
                prediction_offset_y = 0.0f;
                return projection;
            }
            if (!lead_active) {
                ++lead_candidate_frames;
                // 退出过 prediction 后要求连续 4 帧运动确认，避免低谷噪声
                // 让最终点在基础点和 prediction 点之间快速来回切换。
                const int required_frames = lead_ever_activated ? 4 : 1;
                if (lead_candidate_frames < required_frames) return projection;
                lead_active = true;
                lead_ever_activated = true;
                lead_candidate_frames = 0;
            }
            lead_direction_x = world_velocity_x / world_velocity_magnitude;
            lead_direction_y = world_velocity_y / world_velocity_magnitude;
            delay_lead_scale = std::min(
                1.0f, delay_lead_scale +
                    kPredictionLeadRampPerSecond * track.prediction_dt);
            float forecast_x = world_velocity_x * horizon_seconds *
                delay_lead_scale;
            float forecast_y = world_velocity_y * horizon_seconds *
                delay_lead_scale;
            // 额外预测时域为基础延迟时域的 1.5 倍，因此先单独限制真正的
            // 世界运动前探。后续若延迟补偿沿世界运动反方向，只允许 prediction
            // 抵消该轴向反向分量；不能把这部分抵消误算成额外预测时域。
            clamp_vector(
                forecast_x, forecast_y,
                box_diagonal * forecast_limit_percent / 100.0f);
            // 渐入初期的亚量化提前量既不会形成可感知控制效果，又可能在准星
            // 附近把最终点推过零点。内部渐入状态继续累积，但只有实际向量
            // 达到同一激活门槛后才公开为最终点，避免停止阶段的微小再次前探。
            if (std::fabs(forecast_x) < activation_distance_x) {
                forecast_x = 0.0f;
            }
            if (std::fabs(forecast_y) < activation_distance_y) {
                forecast_y = 0.0f;
            }
            if (forecast_x == 0.0f && forecast_y == 0.0f) {
                lead_axis_active_x = false;
                lead_axis_active_y = false;
                return projection;
            }
            lead_axis_active_x = std::fabs(forecast_x) > activation_distance_x;
            lead_axis_active_y = std::fabs(forecast_y) > activation_distance_y;
            // 真机 MoveLeft Run 20260809-225041 中，世界前探 P50 为向左
            // 3.11 px，但屏幕相对速度生成的延迟补偿 P50 为向右 2.45 px；
            // 99.32% 的有效帧发生抵消，最终点 P50 只剩 0.37 count，肉眼等同
            // 没有预测。prediction 仍从延迟点叠加，但必须先吃掉延迟向量在
            // 世界运动反方向上的投影，再把独立世界运动前探完整留在基础点前方。
            // 同向和正交延迟分量保持原样，基础点、轨迹与基础控制器均不回写。
            // 鼠标按轴量化，水平追踪中少量垂直姿态噪声不能把水平抵消量旋转到
            // 另一轴。只在该轴的延迟分量与世界前探确实反向时逐轴抵消；同向轴
            // 和没有有效前探的轴完全不动。
            const float opposing_delay_x =
                projection.delay_x * forecast_x < 0.0f
                ? -projection.delay_x : 0.0f;
            const float opposing_delay_y =
                projection.delay_y * forecast_y < 0.0f
                ? -projection.delay_y : 0.0f;
            float lead_x = forecast_x + opposing_delay_x;
            float lead_y = forecast_y + opposing_delay_y;
            // 公有 lead 表示“延迟点到最终点”的完整位移，因此抵消量加入后仍须
            // 服从用户配置的 prediction 总几何上限。默认 35% 足以容纳最多
            // 15% 的反向延迟抵消和 7.5% 的额外前探，但异常配置也不能越界。
            clamp_vector(
                lead_x, lead_y,
                box_diagonal * config.max_prediction_lead_percent / 100.0f);
            // 延迟抵消后才得到真正送入控制器的最终预测点。把它转换为相对
            // 基础点的独立偏移并按秒限速：基础点仍严格锁在身体框内，预测偏移
            // 可以越过框边界；NDI 突发交付、延迟向量变化和几何上限切入都不能
            // 再让最终点一帧跳到另一位置。
            const float target_offset_x = projection.delay_x + lead_x;
            const float target_offset_y = projection.delay_y + lead_y;
            move_vector_toward(
                target_offset_x, target_offset_y,
                box_diagonal *
                    kPredictionOffsetMaximumSlewDiagonalsPerSecond *
                    track.prediction_dt,
                prediction_offset_x, prediction_offset_y);
            // 公有 lead 的硬上限以“延迟点到最终点”为定义。当前延迟向量
            // 变化后，历史平滑状态可能暂时落在新上限之外，因此在发布前再次
            // 收敛，并同步保存状态，保证运行时契约始终成立。
            float smoothed_lead_x = prediction_offset_x - projection.delay_x;
            float smoothed_lead_y = prediction_offset_y - projection.delay_y;
            clamp_vector(
                smoothed_lead_x, smoothed_lead_y,
                box_diagonal * config.max_prediction_lead_percent / 100.0f);
            prediction_offset_x = projection.delay_x + smoothed_lead_x;
            prediction_offset_y = projection.delay_y + smoothed_lead_y;
            projection.final_x = base_x + prediction_offset_x;
            projection.final_y = base_y + prediction_offset_y;
            projection.active = true;
            return projection;
        }

        // 未启用延迟补偿时保留原有的准星闭环迟滞语义。此分支没有可
        // 复用的延迟向量，只能按相对速度和观测年龄做保守预测。
        delay_lead_scale = 0.0f;
        prediction_pullback_hold_x = false;
        prediction_pullback_hold_y = false;
        lead_axis_active_x = false;
        lead_axis_active_y = false;
        prediction_pullback_direction_x = 0.0f;
        prediction_pullback_direction_y = 0.0f;
        prediction_pullback_hold_time_x = 0.0f;
        prediction_pullback_hold_time_y = 0.0f;
        prediction_offset_x = 0.0f;
        prediction_offset_y = 0.0f;
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
                lead_axis_active_x = false;
                lead_axis_active_y = false;
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
                constexpr int kLeadReenterConfirmFrames = 4;
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
        // prediction 最终点可能通过反向 lead 抵消延迟点中的在途命令投影。
        // 若延迟点即时进入比例项、lead 再单独低通，同一抵消向量会形成快慢
        // 两条控制路径；真实 40 ms 闭环中公开最终点虽稳定，内部控制目标仍会
        // 往返。该轴有 prediction 时改以基础点为锚，统一处理最终点相对基础点
        // 的总投影偏移，并把超过稳定世界维持预算的在途库存投影到隐藏控制锚；
        // 其他轴和关闭 prediction 时继续控制延迟点。
        const bool use_coherent_prediction_projection_x =
            config.enable_prediction && config.enable_delay_compensation &&
            lead_active && lead_axis_active_x;
        const bool use_coherent_prediction_projection_y =
            config.enable_prediction && config.enable_delay_compensation &&
            lead_active && lead_axis_active_y;
        float pending_control_projection_target_x = 0.0f;
        if (use_coherent_prediction_projection_x) {
            const auto [pending_x, pending_y] =
                pending_issued_command_sum(frame.captured_at);
            (void)pending_y;
            // 稳定世界速度本来就需要在反馈窗内保留一份命令库存，不能把
            // 全部 pending 都当成过冲。只投影超过稳定预算的部分，连续小
            // 命令不受影响，-4~-6 counts 的脉冲库存则会提前触发制动。
            const float expected_pending_x = prediction_world_velocity_x *
                config.control_delay_ms / 1000.0f;
            const float excess_pending_x = pending_x - expected_pending_x;
            pending_control_projection_target_x = -excess_pending_x *
                kControllerPendingCommandResponse /
                config.counts_per_pixel_x /
                frame.source_pixels_per_roi_pixel_x;
            const float maximum_pending_projection = std::hypot(
                track.x2 - track.x1, track.y2 - track.y1) *
                config.max_delay_compensation_percent / 100.0f;
            pending_control_projection_target_x = std::clamp(
                pending_control_projection_target_x,
                -maximum_pending_projection, maximum_pending_projection);
            prediction_pending_projection_x +=
                (pending_control_projection_target_x -
                 prediction_pending_projection_x) *
                kPredictionPendingProjectionResponse;
        } else {
            prediction_pending_projection_x = 0.0f;
        }
        const float control_anchor_x = use_coherent_prediction_projection_x
            ? base_x + prediction_pending_projection_x : tracking_x;
        const float control_anchor_y = use_coherent_prediction_projection_y
            ? base_y : tracking_y;
        const float prediction_target_x =
            (aim_x - (use_coherent_prediction_projection_x
                ? base_x : control_anchor_x)) *
            frame.source_pixels_per_roi_pixel_x;
        const float prediction_target_y =
            (aim_y - control_anchor_y) *
            frame.source_pixels_per_roi_pixel_y;
        // 基础 tracking 的 smoothing 为 0.475；总投影偏移采用更慢的独立
        // 响应，避免姿态形变和在途命令窗口变化直接转成鼠标命令。
        const float prediction_alpha = lead_active ? 0.35f : 0.12f;
        if (!controller_initialized && use_coherent_prediction_projection_x) {
            prediction_control_offset_x = prediction_target_x;
        } else {
            prediction_control_offset_x +=
                (prediction_target_x - prediction_control_offset_x) *
                prediction_alpha;
        }
        if (!controller_initialized && use_coherent_prediction_projection_y) {
            prediction_control_offset_y = prediction_target_y;
        } else {
            prediction_control_offset_y +=
                (prediction_target_y - prediction_control_offset_y) *
                prediction_alpha;
        }
        const float control_anchor_error_x =
            (control_anchor_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float control_anchor_error_y =
            (control_anchor_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        const float error_x =
            control_anchor_error_x + prediction_control_offset_x;
        const float error_y =
            control_anchor_error_y + prediction_control_offset_y;
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
        // 比例闭环复用同一总投影偏移状态；prediction 关闭时锚点仍是原延迟点，
        // 开启时则不会把延迟点与其反向抵消量拆成不同响应速度。
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
        // “上一帧停发且基础点位于保持带”也是预测已经追上的正常状态，不能
        // 再据此释放 prediction；真实停止由逐轴世界运动低测量连续五帧确认。
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
                                            float& feedforward,
                                            float& world_motion_measurement,
                                            bool& external_motion_evidence) {
            world_motion_measurement = 0.0f;
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
                const float relative_motion_counts =
                    relative_velocity * source_scale * controller_dt *
                    counts_per_pixel * kControllerFeedforwardVelocityScale;
                const float measurement =
                    delayed_command + relative_motion_counts;
                world_motion_measurement = measurement;
                // 自身相机反馈必然与到期命令反向；相对运动仍与命令同向
                // 说明外部目标运动压过了反馈，可作为快速建立的因果证据。
                // 零命令后的滤波残余不满足此条件，不能误触发 Y prediction。
                if (std::fabs(delayed_command) > 0.001f &&
                    std::fabs(relative_motion_counts) >
                        kPredictionWorldMotionMinimumCounts &&
                    delayed_command * relative_motion_counts > 0.0f) {
                    external_motion_evidence = true;
                }
                const float alpha = 1.0f - std::exp(
                    -kControllerFeedforwardObserverGainPerSecond *
                        controller_dt);
                feedforward += (measurement - feedforward) * alpha;
                // prediction 已经把准星推到人物前方后，目标停止帧不能继续
                // 保留旧的同向基础维持量，否则它会与返回基础点的比例项争抢
                // 方向，形成“经零反转”微抖。只在当前轴的世界运动测量已
                // 低于 prediction 噪声门槛时快速释放，tracking 配置的原路径不变。
                if (config.enable_prediction &&
                    config.enable_delay_compensation &&
                    std::fabs(measurement) <=
                        kPredictionWorldMotionMinimumCounts) {
                    feedforward *= std::exp(
                        -kPredictionWorldMotionReleasePerSecond *
                            controller_dt);
                }
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
            config.counts_per_pixel_x, delayed_command_x, feedforward_x,
            world_motion_measurement_x,
            prediction_external_motion_evidence_x);
        update_feedforward(
            base_error_y, track.vy, frame.source_pixels_per_roi_pixel_y,
            config.counts_per_pixel_y, delayed_command_y, feedforward_y,
            world_motion_measurement_y,
            prediction_external_motion_evidence_y);
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
        // prediction 活动时，最终点可能已经越过基础点，但仍暂时位于准星另一侧。
        // 此时沿世界运动反方向纠偏只会把准星拉回旧位置；真实延迟闭环会将
        // 这种“追上后反拉”放大为经零反转抖动。对确有世界运动分量的轴选择
        // 停发等待目标进入预测点，不影响 prediction 退出后的基础归位，也不
        // 改写 tracking 配置的控制路径。
        if (config.enable_prediction && config.enable_delay_compensation) {
            // X 轴反拉门禁只允许被同一帧的因果世界运动证据放行：命令必须
            // 朝当前最终点，且命令补偿后的世界测量仍与历史 prediction 方向
            // 同向并超过噪声门槛。这样相机反馈低谷、停止和真实反向继续停发，
            // 只有真实目标仍在沿原方向运动时才切断约束造成的长停发。
            const bool allow_x_lead_release =
                aim::detail::prediction_pullback_command_allowed(
                    desired_x, error_x, world_motion_measurement_x,
                    lead_direction_x,
                    kPredictionWorldMotionMinimumCounts);
            const bool allow_x_pullback_release =
                aim::detail::prediction_pullback_command_allowed(
                    desired_x, error_x, world_motion_measurement_x,
                    prediction_pullback_direction_x,
                    kPredictionWorldMotionMinimumCounts);
            // 公有命令逐轴量化；只要该轴存在有效世界方向就必须阻止反拉，
            // 不能因正交轴幅值更大而用归一化 0.1 门槛丢掉水平保护。
            if (lead_active && std::fabs(lead_direction_x) > 0.001f &&
                desired_x * lead_direction_x < 0.0f &&
                !allow_x_lead_release) {
                desired_x = 0.0f;
            }
            if (lead_active && lead_axis_active_y &&
                std::fabs(lead_direction_y) > 0.001f &&
                desired_y * lead_direction_y < 0.0f) {
                desired_y = 0.0f;
            }
            if (prediction_pullback_hold_x &&
                desired_x * prediction_pullback_direction_x < 0.0f &&
                !allow_x_pullback_release) {
                desired_x = 0.0f;
            }
            if (prediction_pullback_hold_y &&
                desired_y * prediction_pullback_direction_y < 0.0f) {
                desired_y = 0.0f;
            }
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
        lead_axis_active_x = false;
        lead_axis_active_y = false;
        lead_ever_activated = false;
        lead_rearm_ready = true;
        prediction_pullback_hold_x = false;
        prediction_pullback_hold_y = false;
        prediction_pullback_direction_x = 0.0f;
        prediction_pullback_direction_y = 0.0f;
        prediction_pullback_hold_time_x = 0.0f;
        prediction_pullback_hold_time_y = 0.0f;
        lead_settle_frames = 0;
        lead_candidate_frames = 0;
        lead_direction_x = 0.0f;
        lead_direction_y = 0.0f;
        prediction_world_velocity_x = 0.0f;
        prediction_world_velocity_y = 0.0f;
        prediction_motion_candidate_x_seconds = 0.0f;
        prediction_motion_candidate_y_seconds = 0.0f;
        prediction_external_motion_evidence_x = false;
        prediction_external_motion_evidence_y = false;
        prediction_offset_x = 0.0f;
        prediction_offset_y = 0.0f;
        prediction_control_offset_x = 0.0f;
        prediction_control_offset_y = 0.0f;
        prediction_pending_projection_x = 0.0f;
        prediction_low_motion_x_frames = 0;
        prediction_low_motion_y_frames = 0;
        world_motion_measurement_x = 0.0f;
        world_motion_measurement_y = 0.0f;
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
            // prediction 点与最终控制点保持同一坐标契约；基础点始终独立保留。
            result.target.prediction_aim_x = projection.final_x;
            result.target.prediction_aim_y = projection.final_y;
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
