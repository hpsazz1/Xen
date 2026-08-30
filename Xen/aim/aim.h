#ifndef AIM_H
#define AIM_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "detector/detector.h"

enum class AimStatus {
    NOT_RUN,
    SUCCESS,
    INVALID_INPUT,
    TRACKING_FAILED,
    CONTROL_FAILED,
};

const char* AimStatusName(AimStatus status) noexcept;

// X 反向共同平移驻留在本控制帧没有继续累计或被清零的直接原因。
// 该枚举只进入诊断报告，不参与控制分支；字符串名称是 Runtime schema
// 的稳定取值，便于正式脚本按原因计数而不复刻控制状态机。
enum class AimReverseTranslationResetReason {
    NONE,
    CANDIDATE_INACTIVE,
    BASE_NOT_ALIGNED,
    PARTIAL_SEMANTICS,
    PREVIOUS_DIRECTION_PENDING,
    ZERO_TRANSLATION,
    OPPOSING_TRANSLATION,
    WEAK_BUDGET_EXHAUSTED,
    WEAK_WITHOUT_STRONG_HISTORY,
};

const char* AimReverseTranslationResetReasonName(
    AimReverseTranslationResetReason reason) noexcept;

enum class TrackState {
    TENTATIVE,
    CONFIRMED,
    LOST,
};

struct AimConfig {
    std::vector<int> person_class_ids{0};
    std::vector<int> head_class_ids{1};
    float high_confidence = 0.25f;
    float low_confidence = 0.10f;
    int min_confirmed_hits = 2;
    int max_lost_frames = 8;
    float min_iou = 0.10f;
    float max_center_distance = 0.25f;
    float switch_margin = 0.20f;
    int switch_confirm_frames = 3;
    int switch_cooldown_frames = 5;
    // 相对 ROI 短边半径的百分比；100 表示半个短边。只约束目标获取、
    // 挑战者切换和鼠标命令，不裁剪 Detector 输入或停止轨迹状态更新。
    float acquisition_range_percent = 90.0f;
    float body_aim_height_ratio = 0.35f;
    // 身体框内基础瞄点的横向安全范围百分比；50 表示身体框中间 50%。
    float body_aim_range_percent = 50.0f;
    float deadzone_pixels = 1.5f;
    float smoothing = 0.35f;
    float counts_per_pixel_x = 0.50f;
    float counts_per_pixel_y = 0.50f;
    float max_counts_per_frame = 50.0f;
    // 基础 tracking 的延迟补偿可与 prediction 同时开启；它只补偿从截图到输入完成的
    // 已测控制延迟，不改变检测框、轨迹关联或丢失状态。处理顺序固定为基础瞄点、
    // 延迟补偿点、prediction 提前点。
    bool enable_delay_compensation = false;
    // Aim 控制时刻之后仍未包含在 observation age 中的固定控制延迟，单位 ms。
    float control_delay_ms = 0.0f;
    // observation age 与固定控制延迟之和的硬上限，单位 ms。
    float max_delay_compensation_ms = 16.0f;
    // 延迟补偿向量相对当前目标框对角线的最大百分比。
    float max_delay_compensation_percent = 15.0f;
    bool enable_prediction = false;
    // 预测提前向量相对当前目标框对角线的最大百分比。该值只限制预测层，
    // 不改变基础移动、观测或轨迹状态更新。
    float max_prediction_lead_percent = 35.0f;
    float predicted_gain = 0.50f;
};

struct AimFrame {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    // 默认零值表示由 Aim 在处理时读取当前时刻（离线未来帧至少钳到
    // captured_at）。显式值必须不早于 captured_at，且随帧严格递增；
    // 轨迹 dt 仍按视频时间推进，控制整形和命令库存只使用该控制时间线。
    std::chrono::steady_clock::time_point control_at{};
    int roi_width = 0;
    int roi_height = 0;
    // 主机准星中心在当前检测 ROI 内的位置；允许位于 ROI 外。
    float control_center_x = 0.0f;
    float control_center_y = 0.0f;
    // 一个检测 ROI 像素对应的主机完整 FOV 像素数。
    float source_pixels_per_roi_pixel_x = 1.0f;
    float source_pixels_per_roi_pixel_y = 1.0f;
    // Runtime 在按住键且安全门允许物理控制时置 true。未按键仍持续完成
    // 观测、跟踪、预选和命令计算，只是不启用锁定后的动态收缩范围。
    bool lock_active = false;
    std::vector<Detection> detections;
};

struct AimCommand {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at{};
    int dx_counts = 0;
    int dy_counts = 0;
};

// 控制诊断只包含固定大小标量，用来解释“有目标但某轴停发”的具体阶段。
// 单位统一为 counts、秒或毫秒；该结构不参与控制决策，也不持有动态资源。
struct AimControlDiagnostics {
    bool evaluated = false;
    float controller_dt_ms = 0.0f;
    float proportional_x_counts = 0.0f;
    float feedforward_x_counts = 0.0f;
    float desired_before_reverse_x_counts = 0.0f;
    float desired_x_counts = 0.0f;
    float filtered_x_counts = 0.0f;
    float shaped_x_counts = 0.0f;
    float residual_before_quantization_x_counts = 0.0f;
    float delayed_command_x_counts = 0.0f;
    float pending_net_x_counts = 0.0f;
    float pending_absolute_x_counts = 0.0f;
    // tracking X 连续延迟模型的三项可观测量：从当前观测到预计控制生效
    // 期间尚会显现的历史命令响应、与 51 点速度同相位的已执行命令，及
    // 共同边刚体一致性的连续权重。它们只用于诊断，不参与二次决策。
    float modelled_response_x_counts = 0.0f;
    float observer_phase_command_x_counts = 0.0f;
    float observer_consistency_weight_x = 0.0f;
    // 以下 reverse_* 字段为报告 schema 兼容保留。反向门已由连续延迟
    // 模型替代，相关状态/布尔值恒为零；原始边运动和一致性字段仍用于
    // 解释观察器权重。
    float reverse_output_direction_x = 0.0f;
    float reverse_evidence_ratio_seconds_x = 0.0f;
    float reverse_position_ratio_seconds_x = 0.0f;
    float reverse_position_peak_error_x = 0.0f;
    float reverse_translation_seconds_x = 0.0f;
    // 当前相邻原始检测框左右边位移及其共同部分，单位为检测 ROI 像素；
    // 正负号沿 X 方向。control_evidence 是观察器消费的带符号无量纲
    // 一致性 [-1, 1]；gap 是兼容旧报告的恒零字段。
    float reverse_translation_raw_left_x_roi_pixels = 0.0f;
    float reverse_translation_raw_right_x_roi_pixels = 0.0f;
    float reverse_translation_raw_common_x_roi_pixels = 0.0f;
    float reverse_translation_control_evidence_x = 0.0f;
    float reverse_translation_gap_seconds_x = 0.0f;
    float reverse_deformation_seconds_x = 0.0f;
    float reverse_required_evidence_ratio_seconds_x = 0.0f;
    float reverse_required_position_ratio_seconds_x = 0.0f;
    float reverse_probe_direction_x = 0.0f;
    float reverse_probe_age_ms_x = 0.0f;
    AimReverseTranslationResetReason reverse_translation_reset_reason_x =
        AimReverseTranslationResetReason::NONE;
    bool pending_positive_x = false;
    bool pending_negative_x = false;
    bool reverse_candidate_x = false;
    bool reverse_previous_direction_pending_x = false;
    bool reverse_partial_semantics_transition_x = false;
    bool reverse_deformation_active_x = false;
    bool reverse_evidence_ready_x = false;
    bool reverse_translation_fresh_evidence_x = false;
    bool reverse_translation_ready_x = false;
    bool reverse_position_ready_x = false;
    bool reverse_position_improvement_reset_x = false;
    bool reverse_gate_blocked_x = false;
    bool reverse_probe_active_x = false;
    bool reverse_probe_limited_x = false;
    bool pending_inventory_hold_blocked_x = false;
    bool deadzone_quiet = false;
    bool shaper_direction_reset_x = false;
    bool post_alignment_sign_change_blocked_x = false;
    bool post_alignment_growth_limited_x = false;
    bool closing_response_tapered_x = false;
    bool integer_direction_blocked_x = false;
    bool command_sign_change_blocked_x = false;
    bool quantization_zero_x = false;
};

struct AimTargetSnapshot {
    std::uint64_t track_id = 0;
    TrackState state = TrackState::TENTATIVE;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    // 当前帧实际匹配到该轨迹的 Observation。它保留关联后的检测框与
    // head/body 语义，不参与轨迹、基础点或控制计算；预测续帧必须为 invalid。
    bool matched_observation_valid = false;
    float matched_observation_x1 = 0.0f;
    float matched_observation_y1 = 0.0f;
    float matched_observation_x2 = 0.0f;
    float matched_observation_y2 = 0.0f;
    bool matched_observation_head_only = false;
    bool matched_observation_aim_from_head = false;
    // base_aim_x 是当前 Track 框内的归一化锚点，不消费历史 X reference；
    // base_aim_y 保留现有状态估计并始终位于目标框内。延迟补偿点只用于
    // 基础 tracking 的已测控制延迟；aim_* 是再应用 prediction 后的最终点。
    float base_aim_x = 0.0f;
    float base_aim_y = 0.0f;
    float delay_compensated_aim_x = 0.0f;
    float delay_compensated_aim_y = 0.0f;
    // 独立 prediction 瞄点：只表示 prediction 层生成的点，不回写基础 tracking 瞄点。
    float prediction_aim_x = 0.0f;
    float prediction_aim_y = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    // 速度单位为检测 ROI 像素/秒；实际提前向量已经包含帧龄、降权和距离限幅。
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
    float lead_x = 0.0f;
    float lead_y = 0.0f;
    float delay_compensation_x = 0.0f;
    float delay_compensation_y = 0.0f;
    // 分轴记录实际使用的几何投影时域，单位 ms。旧字段保留为两轴最大值，
    // 供既有报告读取方兼容；新诊断必须优先读取 x/y 字段。
    float delay_compensation_ms_x = 0.0f;
    float delay_compensation_ms_y = 0.0f;
    float delay_compensation_ms = 0.0f;
    float observation_age_ms = 0.0f;
    float confidence = 0.0f;
    bool lead_active = false;
    bool delay_compensation_active = false;
    bool predicted = false;
};

struct AimProfile {
    double observation_ms = 0.0;
    double tracking_ms = 0.0;
    double selection_ms = 0.0;
    double control_ms = 0.0;
    double total_ms = 0.0;
};

struct AimResult {
    AimStatus status = AimStatus::NOT_RUN;
    bool has_target = false;
    bool has_command = false;
    float acquisition_range_radius = 0.0f;
    float active_range_radius = 0.0f;
    bool range_locked = false;
    bool range_allows_control = false;
    AimTargetSnapshot target;
    AimControlDiagnostics control;
    AimCommand command;
    AimProfile profile;
};

class Aim {
public:
    explicit Aim(const AimConfig& config);
    ~Aim();

    Aim(const Aim&) = delete;
    Aim& operator=(const Aim&) = delete;
    Aim(Aim&&) noexcept;
    Aim& operator=(Aim&&) noexcept;

    AimResult process(const AimFrame& frame) noexcept;
    // Runtime 在 Mouse 后端返回后确认本帧的后端结果。成功发送传回原命令，
    // 安全门拒绝或后端失败传回 (0,0)。该时刻不是 protocol ACK
    // 或物理效果；issued_at 继续保留，延迟库存只用 backend completion 生效。
    bool record_backend_completed_command(
        std::uint64_t sequence,
        std::chrono::steady_clock::time_point backend_completed_at,
        int dx_counts,
        int dy_counts) noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // AIM_H
