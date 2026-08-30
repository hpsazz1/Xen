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
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace {

// 固定 240 Hz 下 51 个连续观测约覆盖 0.21 秒。窗口长度用于分离短周期
// 人体轮廓形变与轨迹平移，不代表人物速度档位；拟合仍使用真实采集时间。
constexpr std::size_t kTrackHorizontalTrendSampleCount = 51;
constexpr std::size_t kTrackHorizontalTrendMinimumSampleCount = 5;
constexpr std::size_t kTrackHorizontalRawMotionSampleCount = 3;
// 最近三次原始共同边的任一次仍达到旧预测的 25% 时，不把低屏幕运动
// 解释为预测失效。该比例只比较同一轨迹的观测/预测几何，不是速度档位。
constexpr float kTrackHorizontalRecentMotionMaximumPredictionRatio = 0.25f;
// 低观测运动只在旧方向历史 reference 已走过用户水平半内窗的 60% 时撤销预测，
// 避免正常匀速跟随在框中心附近被误当成停稳。
constexpr float kTrackHorizontalUnsupportedMotionMinimumAimRangeRatio = 0.60f;
// OLS 有效后，当前中心与趋势中心的 ROI 横向比例连续三次相差超过 2%
// 才切段。它是框中心相对稳定 ROI 的空间创新，不读取身体框宽高，也不
// 换算成像素/秒或人物速度档位；候选确认前与主趋势隔离。
constexpr float kTrackHorizontalTrendChangePointMaximumRoiWidthRatio = 0.02f;
constexpr std::size_t kTrackHorizontalTrendChangePointConfirmSampleCount = 3;

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

struct HorizontalTrendHistory {
    std::array<std::chrono::steady_clock::time_point,
               kTrackHorizontalTrendSampleCount> captured_at{};
    std::array<float, kTrackHorizontalTrendSampleCount> center_x_ratio{};
    std::size_t next = 0;
    std::size_t count = 0;
};

struct HorizontalTrendChangePoint {
    std::array<std::chrono::steady_clock::time_point,
               kTrackHorizontalTrendChangePointConfirmSampleCount>
        captured_at{};
    std::array<float,
               kTrackHorizontalTrendChangePointConfirmSampleCount>
        center_x_ratio{};
    float side = 0.0f;
    std::size_t count = 0;
};

struct HorizontalRawMotionHistory {
    std::array<float, kTrackHorizontalRawMotionSampleCount>
        velocity_x{};
    std::size_t next = 0;
    std::size_t count = 0;
};

struct HorizontalDirectionHistory {
    std::array<float, kTrackHorizontalTrendSampleCount> center_x_ratio{};
    std::array<float, kTrackHorizontalTrendSampleCount> center_y_ratio{};
    std::size_t next = 0;
    std::size_t count = 0;
    double travelled_distance_x = 0.0;
    double travelled_distance_y = 0.0;
};

struct Track {
    std::uint64_t id = 0;
    TrackState state = TrackState::TENTATIVE;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    // 历史水平估计器的内部 reference。它只服务速度、遮挡历史和
    // prediction 库存，不再冒充当前帧可观测的基础瞄点。
    float horizontal_reference_x = 0.0f;
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
    int horizontal_center_trend_frames = 0;
    int partial_visibility_x_frames = 0;
    float partial_visibility_x_side = 0.0f;
    float accepted_partial_visibility_x_width = 0.0f;
    float accepted_partial_visibility_x_side = 0.0f;
    float partial_visibility_x_reference_width = 0.0f;
    int horizontal_velocity_isolation_frames = 0;
    int horizontal_trend_observation_isolation_frames = 0;
    int horizontal_maneuver_frames = 0;
    float horizontal_maneuver_direction = 0.0f;
    // 当前相邻观测的带符号两边共同平移一致性，范围 [-1, 1]。轨迹变点
    // 继续消费这份新鲜原始几何，不读取人物像素速度或当前框宽。
    float horizontal_translation_evidence_x = 0.0f;
    // tracking X 世界运动观察器消费的三观测鲁棒共同边一致性。当前帧
    // 原始共同边必须与三点中值同向，因此静止、符号反转和语义切换会
    // 自然归零；中值只修复单帧边界幅值噪声，不能凭历史伪造平移。
    float horizontal_control_translation_evidence_x = 0.0f;
    // 保留相邻原始框边位移，供 Runtime 报告精确复盘，并为 tracking X
    // closing 提供当前帧对的因果共同平移；语义切换/缺帧时立即归零。
    float horizontal_raw_left_motion_x = 0.0f;
    float horizontal_raw_right_motion_x = 0.0f;
    float last_observation_x1 = 0.0f;
    float last_observation_x2 = 0.0f;
    float last_observation_aim_x = 0.0f;
    std::chrono::steady_clock::time_point last_observation_at{};
    bool horizontal_observation_initialized = false;
    bool last_observation_head_only = false;
    bool horizontal_trend_rebuilding_from_partial = false;
    bool partial_visibility_x_recovery_pending = false;
    int pose_deformation_y_frames = 0;
    HorizontalTrendHistory horizontal_trend{};
    HorizontalTrendChangePoint horizontal_trend_change{};
    HorizontalRawMotionHistory horizontal_raw_motion{};
    HorizontalDirectionHistory horizontal_direction{};
    // predict_tracks() 实际经过二维限幅后写入历史 reference 的 X 位移。反向快通道
    // 需要用它计算旧预测与新观测的精确差额，不能用 vx*dt 近似限幅结果。
    float predicted_motion_x = 0.0f;
    // 连续低观测运动撤销旧 X 预测的渐入时长；只在同一匹配轨迹内跨帧
    // 保留，语义切换、重新获得运动支持或丢帧时立即清零。
    float horizontal_prediction_unsupported_seconds = 0.0f;
    // 只读保存当前帧关联后的原始 Observation；每帧预测前清空，避免把
    // 丢框时的状态外推误报成新检测。
    bool matched_observation_valid = false;
    float matched_observation_x1 = 0.0f;
    float matched_observation_y1 = 0.0f;
    float matched_observation_x2 = 0.0f;
    float matched_observation_y2 = 0.0f;
    bool matched_observation_head_only = false;
    bool matched_observation_aim_from_head = false;
};

struct IssuedCommand {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point issued_at{};
    std::chrono::steady_clock::time_point backend_completed_at{};
    float dx_counts = 0.0f;
    float dy_counts = 0.0f;
    bool backend_completed = false;
};

struct PendingIssuedCommandInventory {
    float net_x = 0.0f;
    float net_y = 0.0f;
    float absolute_x = 0.0f;
    float absolute_y = 0.0f;
    float backend_completed_weighted_x = 0.0f;
    bool has_positive_x = false;
    bool has_negative_x = false;
};

constexpr float kInvalidAssignmentCost = 1000000.0f;
constexpr float kUnmatchedAssignmentCost = 1.05f;
constexpr float kSelectedTrackIdentityBias = 0.15f;
// 关联矩阵的未匹配竞争比选择器多一层：多个已确认轨迹会争夺同一个
// 观测。当前锁定获得五分之一归一化代价的软优先权；仍远低于无匹配
// 代价，且明显更优的真实目标可以胜出，不做 selected-first 硬绑定。
constexpr float kSelectedTrackAssociationBias = 0.20f;
constexpr float kTrackPositionAlphaHigh = 0.72f;
constexpr float kTrackPositionAlphaLow = 0.45f;
constexpr float kTrackVelocityBetaHigh = 0.10f;
constexpr float kTrackVelocityBetaLow = 0.04f;
// 控制点只立即跟随框两条对边的共同位移；边缘分歧代表宽高/姿态形变，
// 框内偏移使用独立低增益校正。这样真实平移仍使用原位置增益，不额外引入滞后。
constexpr float kTrackAimShapeAlphaHigh = 0.12f;
constexpr float kTrackAimShapeAlphaLow = 0.06f;
// 超级跳平移+交替姿态夹具中，0.50 把跟随误差 P95/误差二阶 P95 收敛到
// 4.62/4.28 px，同时仍通过静止轮廓形变抑振；更低值增加真实平移滞后，
// 更高值开始把交替轮廓写回基础点。它是连续增益，不产生状态边界。
constexpr float kTrackAimVerticalShapeAlpha = 0.50f;
// 人物姿态可能让检测框两条对边连续多帧同向摆动，单靠“对边反向即形变”
// 或“中心逐帧反号”都无法识别。有明确尺度变化且中心创新仍属于小尺度时，
// 保护基础锚点；尺度证据消失后按固定保持窗口恢复。X 在宽高共同形变时
// 使用原始身体中心的时间趋势，避免依赖 head 是否闪断或人物速度档位。
constexpr float kTrackCoherentDeformationMaximumTargetDiagonal = 0.04f;
constexpr float kTrackCoherentDeformationMinimumShapeChangeTargetDiagonal =
    0.0005f;
constexpr float kTrackCoherentDeformationMinimumShapeChangePixels = 0.05f;
// 左/右半身裁切通常表现为一条边基本稳定、另一条边明显内缩。只比较两边
// 残差比例，不使用像素速度；对称缩放不会误入该分支。
constexpr float kTrackPartialVisibilityStableEdgeMaximumResidualRatio = 0.25f;
constexpr float kTrackPartialVisibilityMaximumWidthRatio = 0.75f;
// 单侧截断连续三帧才接受为新的可见框几何。前两帧约 8.3 ms（240 Hz）
// 保留既有水平框；该计数只看几何连续性，不按人物像素速度分档。
constexpr int kTrackPartialVisibilityConfirmFrames = 3;
// 实机姿态同向形变可持续 3～10 帧；保护必须覆盖完整形变窗口，真实平移仍由
// stable_motion_residual 和速度估计独立推进。
constexpr int kTrackCoherentDeformationHoldFrames = 10;
// X 轴姿态段由固定时间窗几何趋势更新；首四个样本沿用共同边缘位移。
// Y 不引入长窗，继续沿用本轮前的跳跃保护状态机，避免扩大改动范围。
constexpr float kTrackDeformationVelocityBetaScaleMinimum = 0.08f;
constexpr float kTrackDeformationVelocityBetaScaleMaximum = 0.40f;
// X 的共同边创新比长窗斜率更及时，但姿态占主导时需要更低底噪增益。
// 该插值只读取无量纲边缘一致性，不是人物速度阈值。
constexpr float kTrackHorizontalVelocityBetaScaleMinimum = 0.02f;
constexpr float kTrackHorizontalVelocityBetaScaleMaximum = 1.00f;
// 共同平移占相邻两边变化至少 70% 时，原始观测才算提供了可信物理方向。
// 该无量纲置信度同时服务于机动切段和控制器位置兜底：姿态同时改变框宽时
// 会自然降权，固定框/真实整体平移保持为 1，不使用框宽或像素速度。
constexpr float kHorizontalTranslationEvidenceConsistencyMinimum = 0.70f;
// 三个同侧候选确认后，以候选首尾中心的真实时间斜率小幅播种 vx，补回
// 候选隔离造成的两帧响应空窗；其余响应仍由共同边快通道连续完成，避免
// 一次切段把完整斜率作为速度阶跃写入延迟投影。
constexpr float kTrackHorizontalManeuverVelocitySeedGain = 0.15f;
// 长窗斜率以 25 ms 时间常数只作低频锚；同帧共同边创新提供额外快通道，
// 变点候选期间两者都被隔离，避免旧趋势或候选回弹直接进入 vx。
constexpr float kTrackHorizontalVelocityAnchorGainPerSecond = 40.0f;
// 变点/半身候选撤回后，关联框仍需数帧从异常几何回到主轨迹。隔离期只
// 保留既有 vx，不把回弹残差除以 dt；固定帧数描述观测语义，不判断速度。
constexpr int kTrackHorizontalVelocityIsolationFrames = 5;
// 三帧中值共同边位移与当前观测相差超过 2% ROI 时，当前及后续两个观测
// 不进入趋势位置窗。固定三次观测覆盖异常本体、持续帧与恢复边沿；判断量
// 是 ROI 中心比例，不按人物像素速度或框宽划档。
constexpr int kTrackHorizontalTrendObservationIsolationFrames = 3;
// ROI 中心创新连续三帧同向后，固定 10 帧开放共同边快通道。它描述已确认
// 运动段的时间证据；单/双帧异常不会触发。快通道按两边共同平移的一致性
// 连续降权，不锁死首次方向，因此窗口内的真实二次换向仍能及时进入状态。
constexpr int kTrackHorizontalManeuverFrames = 10;
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
// tracking 的命令和图像误差来自同一个闭环，不能把二者相关性直接当作
// 鼠标 plant。prediction 关闭时改由图像误差上的泄漏 PI 产生维持量：
// 12/s 的积分建立约覆盖真实 15 ms 延迟后的数个采样，6/s 泄漏在静止后
// 约 116 ms 衰减一半。Y 保留既有 4 counts 状态边界；X 不再沿用旧场景
// P95 推导出的次级天花板，而由既有每帧物理向量上限、Y 优先分配与
// back-calculation 共同约束。所有常量均使用真实 dt，不读取目标速度、
// 框尺度或游戏类型。
constexpr float kTrackingIntegralGainPerSecond = 12.0f;
constexpr float kTrackingIntegralLeakPerSecond = 6.0f;
constexpr float kTrackingAntiWindupGainPerSecond = 30.0f;
constexpr float kTrackingVerticalIntegralMaximumCounts = 4.0f;
// closing-slope 只作为 X 请求的带限阻尼。20 ms 一阶滤波覆盖已观测的
// 4～5 帧反馈时间尺度；2 ms 导数增益把高频误差等效增益限制为比例项的
// 10%，最终仍由同号预算保证最多减到零而不会自行反向。
constexpr float kTrackingErrorDerivativeFilterTimeSeconds = 0.020f;
constexpr float kTrackingErrorDerivativeGainSeconds = 0.002f;
// 40 ms 窗口内的 pending 总和会按命令逐帧阶跃；0.12 只平滑隐藏库存
// 投影，使实测 8～10 帧命令反馈周期内逐步制动，不改变公开 prediction 点或
// 用户配置的 0.475 基础控制平滑。
constexpr float kPredictionPendingProjectionResponse = 0.12f;
// 高频闭环已有足够多的延迟窗口样本供基础观察器工作；真实 Run 的控制
// 节奏会在 68～119 Hz 间变化，40 ms 窗口仅剩 3～5 个离散命令。低于
// 125 Hz 时改用同源世界速度维持量，避免刚好跨过 100 Hz 后再次退回持续
// 位置误差；240 Hz 高频路径保持原观察器。
constexpr float kPredictionDirectFeedforwardMinimumDeltaSeconds = 0.008f;
// 119 Hz 真实 Run 中平均命令仍约等于公开误差的 0.4 倍比例量，
// 说明离散世界速度维持量仍有小幅损失。1.10 只校正低频同源前馈，
// 不放大公开 prediction 点、Y 轴或 240 Hz 基础观察器路径。
constexpr float kPredictionDirectFeedforwardScale = 1.10f;
// 最新真机 Run 仍把“最多两帧”预算用满，8 帧 +1 count 全部背离
// 当前公开 prediction 点。超额在途命令已由隐藏控制锚提前制动，公开点
// 保持带外不再允许额外反向物理命令，只停发等待已在途库存反馈。
constexpr int kPredictionOppositePublicBrakeMaximumFrames = 0;
// prediction 从已经完成基础延迟补偿的点继续向前推 1.5 个控制延迟。
// 真机 Run 20260809-234450 证明半个时域的最终点虽比基础点向前 2.75 px，
// 但仍不足以覆盖 3.21 px 的比例控制稳态误差；合成闭环中的一个完整时域
// 也只让基础点领先 0.52 px，仍低于可见门槛。1.5 倍只积分独立世界速度，
// 不反读会被自身输出放大的延迟向量。
constexpr float kPredictionAdditionalHorizonScale = 1.50f;
// 控制延迟描述 KMBOX 命令到画面反馈的时域；几何投影只描述当前轨迹在
// 画面中的短时外推，两者不能被同一个配置值同时放大。prediction 保持
// 已验证的双轴 16 ms 基准。
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
// prediction 退出后反拉保持只用于跨越短暂的相机反馈低谷；Y 轴单次保持
// 持续超过 300 ms 时必须释放，才能让基础点把准星带回配置高度。
constexpr float kPredictionPullbackHoldTimeoutSeconds = 0.30f;
// 实机人物姿态会造成 3～10 帧的同向低运动窗口；五帧确认仍会把窗口
// 中间误判成停止并清零 prediction。延长到 12 帧只改变停止确认，真实
// 停止尾窗仍受快速释放增益限制，基础前馈状态不受影响。
constexpr int kPredictionStaticReleaseConfirmFrames = 12;
// 世界运动只在独立慢速状态形成至少四分之一 count 的稳定维持量后
// 才可用于 prediction；更小残余属于静止收敛和量化噪声，禁止强行前探。
constexpr float kPredictionWorldMotionMinimumCounts = 0.25f;
// 最新 MoveLeft 真机 Run 中，首次水平 prediction 在锁定后通常仍等待完整
// 250 ms 运动确认，一个测量断续段甚至延长到约 600 ms。120 ms 反事实会
// 让既有闭环在停止边界新增经零反转，最终 X 轴取最小通过值 150 ms：它仍
// 覆盖 40/44 ms 命令反馈窗三倍以上和实测 3～10 帧姿态段，但不会把首次
// 可见提前推迟到目标已接近准星之后。Y 轴保留 250 ms，继续隔离已经复现过的
// 归位相机反馈伪运动；已建立后的低谷和停止仍由原有 12 帧释放状态机处理。
constexpr float kPredictionHorizontalMotionEstablishmentSeconds = 0.15f;
constexpr float kPredictionVerticalMotionEstablishmentSeconds = 0.25f;
// 首次确认只容忍一个低于 0.25 count 的采样空洞。四帧容错的
// 真机反事实未缩短首次进入，因此撤回；连续第二个低谷仍清空累计，
// 不能把断续相机反馈拼成真实运动。
constexpr int kPredictionEstablishmentLowMotionGraceFrames = 1;
// 正确方向的提前量也不能在观察器刚建立时整段跳入。按真实 dt 在约 33 ms
// 内线性渐入，避免 prediction 状态变化绕过物理命令阶跃门禁。
constexpr float kPredictionLeadRampPerSecond = 30.0f;
// 命令整形使用时间速率而不是固定帧步：240 Hz 下每帧最多变化 1 count，
// 120/60 Hz 下分别为 2/4 counts，保持不同采集刷新率下的加减速时间一致。
constexpr float kControllerMaximumSlewCountsPerSecond = 240.0f;
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

void clamp_tracking_vector_preserving_y(
        float& x, float& y, float maximum) noexcept {
    const double limit = std::max(0.0, static_cast<double>(maximum));
    const float float_limit = static_cast<float>(limit);
    y = std::clamp(y, -float_limit, float_limit);
    const double y_value = static_cast<double>(y);
    const double x_budget = std::sqrt(std::max(
        0.0, limit * limit - y_value * y_value));
    float x_limit = static_cast<float>(x_budget);
    // double 预算转回控制状态的 float 后可能向圆盘外舍入；此时只把
    // 实验轴 X 向零收一个 ULP，Y candidate 继续保持原值。
    if (x_limit > 0.0f &&
        std::hypot(static_cast<double>(x_limit), y_value) > limit) {
        x_limit = std::nextafter(x_limit, 0.0f);
    }
    x = std::clamp(x, -x_limit, x_limit);
}

void clamp_tracking_command_preserving_y(
        int& x, int& y, float maximum) noexcept {
    const double limit = std::max(0.0, static_cast<double>(maximum));
    const double integer_maximum =
        static_cast<double>(std::numeric_limits<int>::max());
    const auto floor_to_int = [&](double value) {
        return value >= integer_maximum
            ? std::numeric_limits<int>::max()
            : static_cast<int>(std::floor(value));
    };
    const int y_limit = floor_to_int(limit);
    y = std::clamp(y, -y_limit, y_limit);
    const double y_value = static_cast<double>(y);
    const double x_budget = std::sqrt(std::max(
        0.0, limit * limit - y_value * y_value));
    int x_limit = floor_to_int(x_budget);
    // sqrt 紧邻整数格点时可能向上舍入；最终以整数欧氏合同再校正一次，
    // 只收缩实验轴 X，不触碰已经冻结的 Y。
    if (x_limit > 0 &&
        std::hypot(static_cast<double>(x_limit), y_value) > limit) {
        --x_limit;
    }
    x = std::clamp(x, -x_limit, x_limit);
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

struct HorizontalTrendEstimate {
    bool valid = false;
    float position_ratio = 0.0f;
    float velocity_ratio = 0.0f;
};

void clear_horizontal_trend_change_point(Track& track) noexcept {
    track.horizontal_trend_change.side = 0.0f;
    track.horizontal_trend_change.count = 0;
}

void reset_horizontal_trend(Track& track) noexcept {
    track.horizontal_trend.next = 0;
    track.horizontal_trend.count = 0;
    track.horizontal_trend_rebuilding_from_partial = false;
    clear_horizontal_trend_change_point(track);
}

void append_horizontal_trend(
        Track& track, std::chrono::steady_clock::time_point captured_at,
        float center_x_ratio) noexcept {
    HorizontalTrendHistory& history = track.horizontal_trend;
    history.captured_at[history.next] = captured_at;
    history.center_x_ratio[history.next] = center_x_ratio;
    history.next = (history.next + 1) % kTrackHorizontalTrendSampleCount;
    history.count = std::min(
        history.count + 1, kTrackHorizontalTrendSampleCount);
}

bool latest_horizontal_trend_center_x_ratio(
        const Track& track, float& center_x_ratio) noexcept {
    const HorizontalTrendHistory& history = track.horizontal_trend;
    if (history.count == 0) return false;
    const std::size_t newest =
        (history.next + kTrackHorizontalTrendSampleCount - 1) %
        kTrackHorizontalTrendSampleCount;
    center_x_ratio = history.center_x_ratio[newest];
    return true;
}

HorizontalTrendEstimate estimate_horizontal_trend(
        const Track& track,
        std::chrono::steady_clock::time_point estimated_at) noexcept {
    const HorizontalTrendHistory& history = track.horizontal_trend;
    if (history.count < kTrackHorizontalTrendMinimumSampleCount) return {};

    // 端点局部线性回归相当于因果的一阶 Savitzky-Golay 趋势估计：
    // 前五帧后使用增长窗口及时跟随，满 51 帧后衰减窗口内的短周期轮廓
    // 摆动；匀速平移不论快慢都保留原斜率。时间先减去最新采集时刻，
    // 避免 steady_clock 大数损失精度。
    const std::size_t oldest =
        (history.next + kTrackHorizontalTrendSampleCount - history.count) %
        kTrackHorizontalTrendSampleCount;
    const std::size_t newest =
        (history.next + kTrackHorizontalTrendSampleCount - 1) %
        kTrackHorizontalTrendSampleCount;
    const auto reference_time = history.captured_at[newest];
    double mean_time = 0.0;
    double mean_position = 0.0;
    for (std::size_t offset = 0;
         offset < history.count; ++offset) {
        const std::size_t index =
            (oldest + offset) % kTrackHorizontalTrendSampleCount;
        mean_time += std::chrono::duration<double>(
            history.captured_at[index] - reference_time).count();
        mean_position += history.center_x_ratio[index];
    }
    mean_time /= static_cast<double>(history.count);
    mean_position /= static_cast<double>(history.count);

    double covariance = 0.0;
    double time_variance = 0.0;
    for (std::size_t offset = 0;
         offset < history.count; ++offset) {
        const std::size_t index =
            (oldest + offset) % kTrackHorizontalTrendSampleCount;
        const double time = std::chrono::duration<double>(
            history.captured_at[index] - reference_time).count();
        const double centered_time = time - mean_time;
        covariance += centered_time *
            (static_cast<double>(history.center_x_ratio[index]) -
             mean_position);
        time_variance += centered_time * centered_time;
    }
    if (time_variance <= std::numeric_limits<double>::epsilon()) return {};

    const double velocity = covariance / time_variance;
    const double latest_position = mean_position - velocity * mean_time;
    const double projection_seconds = std::chrono::duration<double>(
        estimated_at - reference_time).count();
    const double position = latest_position + velocity * projection_seconds;
    if (!std::isfinite(position) || !std::isfinite(velocity)) return {};
    return {true, static_cast<float>(position),
            static_cast<float>(velocity)};
}

void reset_horizontal_direction_history(Track& track) noexcept {
    track.horizontal_direction.next = 0;
    track.horizontal_direction.count = 0;
    track.horizontal_direction.travelled_distance_x = 0.0;
    track.horizontal_direction.travelled_distance_y = 0.0;
}

void append_horizontal_direction_history(
        Track& track, float center_x_ratio, float center_y_ratio) noexcept {
    HorizontalDirectionHistory& history = track.horizontal_direction;
    if (history.count != 0) {
        const std::size_t newest =
            (history.next + kTrackHorizontalTrendSampleCount - 1) %
            kTrackHorizontalTrendSampleCount;
        if (history.count == kTrackHorizontalTrendSampleCount) {
            const std::size_t second_oldest =
                (history.next + 1) % kTrackHorizontalTrendSampleCount;
            history.travelled_distance_x -= std::fabs(
                static_cast<double>(
                    history.center_x_ratio[second_oldest]) -
                static_cast<double>(
                    history.center_x_ratio[history.next]));
            history.travelled_distance_y -= std::fabs(
                static_cast<double>(
                    history.center_y_ratio[second_oldest]) -
                static_cast<double>(
                    history.center_y_ratio[history.next]));
        }
        history.travelled_distance_x += std::fabs(
            static_cast<double>(center_x_ratio) -
            static_cast<double>(history.center_x_ratio[newest]));
        history.travelled_distance_y += std::fabs(
            static_cast<double>(center_y_ratio) -
            static_cast<double>(history.center_y_ratio[newest]));
    }
    history.center_x_ratio[history.next] = center_x_ratio;
    history.center_y_ratio[history.next] = center_y_ratio;
    history.next =
        (history.next + 1) % kTrackHorizontalTrendSampleCount;
    history.count = std::min(
        history.count + 1, kTrackHorizontalTrendSampleCount);
}

float horizontal_direction_efficiency(const Track& track) noexcept {
    const HorizontalDirectionHistory& history = track.horizontal_direction;
    if (history.count < kTrackHorizontalTrendMinimumSampleCount) return 1.0f;
    const std::size_t oldest =
        (history.next + kTrackHorizontalTrendSampleCount - history.count) %
        kTrackHorizontalTrendSampleCount;
    const std::size_t newest =
        (history.next + kTrackHorizontalTrendSampleCount - 1) %
        kTrackHorizontalTrendSampleCount;
    const double travelled_distance =
        std::max(0.0, history.travelled_distance_x);
    if (travelled_distance <= std::numeric_limits<double>::epsilon()) {
        return 0.0f;
    }
    const double net_distance = std::fabs(
        static_cast<double>(history.center_x_ratio[newest]) -
        static_cast<double>(history.center_x_ratio[oldest]));
    return static_cast<float>(std::clamp(
        net_distance / travelled_distance, 0.0, 1.0));
}

float horizontal_direction_path_share(const Track& track) noexcept {
    const HorizontalDirectionHistory& history = track.horizontal_direction;
    const double travelled_x = std::max(0.0, history.travelled_distance_x);
    const double travelled_y = std::max(0.0, history.travelled_distance_y);
    const double travelled = travelled_x + travelled_y;
    if (travelled <= std::numeric_limits<double>::epsilon()) return 1.0f;
    return static_cast<float>(std::clamp(
        travelled_x / travelled, 0.0, 1.0));
}

void append_horizontal_trend_change_candidate(
        Track& track, float side,
        std::chrono::steady_clock::time_point captured_at,
        float center_x_ratio) noexcept {
    HorizontalTrendChangePoint& change = track.horizontal_trend_change;
    if (side != change.side) {
        clear_horizontal_trend_change_point(track);
        change.side = side;
    }
    if (change.count >=
        kTrackHorizontalTrendChangePointConfirmSampleCount) {
        return;
    }
    change.captured_at[change.count] = captured_at;
    change.center_x_ratio[change.count] = center_x_ratio;
    ++change.count;
}

void commit_horizontal_trend_change_point(Track& track) noexcept {
    const HorizontalTrendChangePoint change =
        track.horizontal_trend_change;
    reset_horizontal_trend(track);
    for (std::size_t index = 0; index < change.count; ++index) {
        append_horizontal_trend(
            track, change.captured_at[index],
            change.center_x_ratio[index]);
    }
}

void reset_horizontal_raw_motion(Track& track) noexcept {
    track.horizontal_raw_motion.next = 0;
    track.horizontal_raw_motion.count = 0;
}

void append_horizontal_raw_motion(Track& track,
                                  float velocity_x) noexcept {
    HorizontalRawMotionHistory& history = track.horizontal_raw_motion;
    history.velocity_x[history.next] = velocity_x;
    history.next =
        (history.next + 1) % kTrackHorizontalRawMotionSampleCount;
    history.count = std::min(
        history.count + 1, kTrackHorizontalRawMotionSampleCount);
}

float median_horizontal_raw_velocity_x(const Track& track) noexcept {
    const HorizontalRawMotionHistory& history = track.horizontal_raw_motion;
    if (history.count < kTrackHorizontalRawMotionSampleCount) {
        return track.vx;
    }
    const float first = history.velocity_x[0];
    const float second = history.velocity_x[1];
    const float third = history.velocity_x[2];
    return first + second + third -
        std::min({first, second, third}) -
        std::max({first, second, third});
}

bool latest_two_horizontal_raw_motion_same_direction(
    const Track& track, float direction) noexcept {
    const HorizontalRawMotionHistory& history = track.horizontal_raw_motion;
    if (history.count < 2 || direction == 0.0f) return false;
    const std::size_t latest =
        (history.next + kTrackHorizontalRawMotionSampleCount - 1) %
        kTrackHorizontalRawMotionSampleCount;
    const std::size_t previous =
        (history.next + kTrackHorizontalRawMotionSampleCount - 2) %
        kTrackHorizontalRawMotionSampleCount;
    return history.velocity_x[latest] * direction > 0.0f &&
        history.velocity_x[previous] * direction > 0.0f;
}

float horizontal_prediction_unsupported_weight(
    const Track& track, float predicted_velocity_x) noexcept {
    const HorizontalRawMotionHistory& history = track.horizontal_raw_motion;
    if (history.count < kTrackHorizontalRawMotionSampleCount ||
        predicted_velocity_x == 0.0f) {
        return 0.0f;
    }
    float maximum_motion_ratio = 0.0f;
    for (const float observed_velocity_x : history.velocity_x) {
        maximum_motion_ratio = std::max(
            maximum_motion_ratio,
            std::fabs(observed_velocity_x / predicted_velocity_x));
    }
    return 1.0f - std::clamp(
        maximum_motion_ratio /
            kTrackHorizontalRecentMotionMaximumPredictionRatio,
        0.0f, 1.0f);
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

float current_horizontal_aim_x(const Track& track) noexcept {
    return track.x1 + (track.x2 - track.x1) *
        std::clamp(track.aim_ratio_x, 0.0f, 1.0f);
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
    std::chrono::steady_clock::time_point last_control_at{};
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
    float tracking_previous_error_x = 0.0f;
    float tracking_error_derivative_x = 0.0f;
    bool tracking_error_derivative_initialized = false;
    float world_motion_measurement_x = 0.0f;
    float world_motion_measurement_y = 0.0f;
    // 独立 prediction 状态使用 counts/second；基础控制器前馈仍保持
    // counts/frame，二者不能混用，否则瞬时帧间隔会改变预测距离。
    float prediction_world_velocity_x = 0.0f;
    float prediction_world_velocity_y = 0.0f;
    // 符号表示候选世界方向，绝对值表示同向有效测量累计时长（秒）。
    float prediction_motion_candidate_x_seconds = 0.0f;
    float prediction_motion_candidate_y_seconds = 0.0f;
    int prediction_candidate_low_motion_x_frames = 0;
    int prediction_candidate_low_motion_y_frames = 0;
    bool prediction_external_motion_evidence_x = false;
    bool prediction_external_motion_evidence_y = false;
    float prediction_offset_x = 0.0f;
    float prediction_offset_y = 0.0f;
    float prediction_control_offset_x = 0.0f;
    float prediction_control_offset_y = 0.0f;
    float prediction_pending_projection_x = 0.0f;
    int prediction_opposite_public_brake_x_frames = 0;
    int prediction_low_motion_x_frames = 0;
    int prediction_low_motion_y_frames = 0;
    std::array<IssuedCommand, kControllerCommandHistoryCapacity>
        issued_commands{};
    std::size_t issued_command_next = 0;
    std::size_t issued_command_count = 0;
    float previous_command_x = 0.0f;
    float previous_command_y = 0.0f;
    std::chrono::steady_clock::time_point controller_at{};
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
    AimStatus logged_status = AimStatus::NOT_RUN;

    void log_status_transition(AimStatus status,
                               std::uint64_t sequence,
                               const char* reason) noexcept {
        if (status == logged_status) return;
        const AimStatus previous = logged_status;
        logged_status = status;
        switch (status) {
            case AimStatus::SUCCESS:
                LOG_INFO("aim", "Aim 状态变化: {} -> {}, seq={}, reason={}",
                         AimStatusName(previous), AimStatusName(status),
                         sequence, reason);
                break;
            case AimStatus::INVALID_INPUT:
                LOG_WARN("aim", "Aim 状态变化: {} -> {}, seq={}, reason={}",
                         AimStatusName(previous), AimStatusName(status),
                         sequence, reason);
                break;
            case AimStatus::TRACKING_FAILED:
            case AimStatus::CONTROL_FAILED:
                LOG_ERROR("aim", "Aim 状态变化: {} -> {}, seq={}, reason={}",
                          AimStatusName(previous), AimStatusName(status),
                          sequence, reason);
                break;
            case AimStatus::NOT_RUN:
                LOG_INFO("aim", "Aim 状态变化: {} -> {}, seq={}, reason={}",
                         AimStatusName(previous), AimStatusName(status),
                         sequence, reason);
                break;
        }
    }

    bool valid_config() const noexcept {
        return aim::detail::valid_aim_config(config);
    }

    bool valid_frame_order(
            const AimFrame& frame,
            std::chrono::steady_clock::time_point control_at) const noexcept {
        return last_sequence == 0 ||
            (frame.sequence > last_sequence &&
             frame.captured_at > last_captured_at &&
             control_at > last_control_at);
    }

    struct LeadProjection {
        float base_x = 0.0f;
        float base_y = 0.0f;
        float delay_compensated_x = 0.0f;
        float delay_compensated_y = 0.0f;
        float delay_x = 0.0f;
        float delay_y = 0.0f;
        float delay_seconds_x = 0.0f;
        float delay_seconds_y = 0.0f;
        float modelled_response_x_counts = 0.0f;
        float final_x = 0.0f;
        float final_y = 0.0f;
        float observation_age_seconds = 0.0f;
        bool delay_active = false;
        bool active = false;
    };

    void commit_frame_order(
            const AimFrame& frame,
            std::chrono::steady_clock::time_point control_at) noexcept {
        last_sequence = frame.sequence;
        last_captured_at = frame.captured_at;
        last_control_at = control_at;
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
            track.matched_observation_valid = false;
            track.matched_observation_x1 = 0.0f;
            track.matched_observation_y1 = 0.0f;
            track.matched_observation_x2 = 0.0f;
            track.matched_observation_y2 = 0.0f;
            track.matched_observation_head_only = false;
            track.matched_observation_aim_from_head = false;
            const float dt = clamp_delta_seconds(
                std::chrono::duration<double>(now - track.state_at).count());
            float dx = track.vx * dt;
            float dy = track.vy * dt;
            clamp_vector(dx, dy, diagonal * 0.25f);
            track.x1 += dx;
            track.x2 += dx;
            track.y1 += dy;
            track.y2 += dy;
            track.horizontal_reference_x += dx;
            track.aim_y += dy;
            track.predicted_motion_x = dx;
            track.prediction_dt = dt;
            track.state_at = now;
            track.predicted = true;
        }
    }

    void update_matched_track(Track& track,
                              const Observation& observation,
                              float diagonal,
                              float roi_width) noexcept {
        const bool high = observation.confidence >= config.high_confidence;
        const float alpha = high
            ? kTrackPositionAlphaHigh : kTrackPositionAlphaLow;
        const float beta = high
            ? kTrackVelocityBetaHigh : kTrackVelocityBetaLow;
        // `track.head_only` 表示当前身份框采用的坐标语义。body 轨迹短时
        // 只剩 head 时会故意保持 false，以保留身体尺度；因此真正的原始
        // 观测语义切换必须比较相邻观测，不能把每个连续 head 帧都当成
        // body→head 并反复把 vx 减半。
        const bool box_semantics_changed =
            track.horizontal_observation_initialized
            ? track.last_observation_head_only != observation.head_only
            : track.head_only != observation.head_only;
        const float track_center_x = (track.x1 + track.x2) * 0.5f;
        const float track_center_y = (track.y1 + track.y2) * 0.5f;
        const float observation_center_x =
            (observation.x1 + observation.x2) * 0.5f;
        const float observation_center_x_ratio =
            observation_center_x / roi_width;
        const float roi_height = std::sqrt(std::max(
            1.0f, diagonal * diagonal - roi_width * roi_width));
        // 单侧裁切会改变“可见框中心”的坐标语义，却不会改变人物的物理
        // 中心。趋势窗口统一消费由稳定边和裁切前宽度重建的中心；普通框
        // 下它与原始中心完全相同，不增加候选、驻留或放行状态。
        float horizontal_trend_observation_center_x_ratio =
            observation_center_x_ratio;
        const float observation_center_y =
            (observation.y1 + observation.y2) * 0.5f;
        const float observation_center_y_ratio =
            observation_center_y / roi_height;
        const float previous_horizontal_reference_x =
            track.horizontal_reference_x - track.predicted_motion_x;
        const float center_motion_residual_x =
            observation_center_x - track_center_x;
        const float center_motion_residual_y =
            observation_center_y - track_center_y;
        float stable_motion_residual_x = center_motion_residual_x;
        float stable_motion_residual_y = center_motion_residual_y;
        float vertical_translation_consistency = 1.0f;
        float robust_horizontal_velocity_x = track.vx;
        float robust_horizontal_translation_consistency = 0.0f;
        float robust_horizontal_trend_center_x_ratio =
            observation_center_x_ratio;
        bool robust_horizontal_velocity_valid = false;
        bool robust_horizontal_trend_center_valid = false;
        bool horizontal_raw_motion_outlier = false;
        float horizontal_prediction_correction_x = 0.0f;
        float horizontal_prediction_unsupported_blend = 0.0f;
        float horizontal_trend_unsupported_blend = 0.0f;
        bool horizontal_reversal_prediction_corrected = false;
        float horizontal_box_motion_residual_x = center_motion_residual_x;
        float velocity_beta_x = beta;
        float velocity_beta_y = beta;
        bool preserve_horizontal_box = false;
        bool isolate_horizontal_center_observation = false;
        bool isolate_fast_velocity_x =
            track.horizontal_velocity_isolation_frames > 0;
        bool isolate_horizontal_trend_observation =
            track.horizontal_trend_observation_isolation_frames > 0;
        bool horizontal_maneuver_active =
            track.horizontal_maneuver_frames > 0;
        bool suppress_center_velocity_x =
            track.horizontal_trend_rebuilding_from_partial;
        if (track.horizontal_velocity_isolation_frames > 0) {
            --track.horizontal_velocity_isolation_frames;
        }
        if (track.horizontal_trend_observation_isolation_frames > 0) {
            --track.horizontal_trend_observation_isolation_frames;
        }
        if (track.horizontal_maneuver_frames > 0) {
            --track.horizontal_maneuver_frames;
        } else {
            track.horizontal_maneuver_direction = 0.0f;
        }
        bool horizontal_trend_active = false;
        HorizontalTrendEstimate horizontal_trend;
        track.horizontal_translation_evidence_x = 0.0f;
        track.horizontal_control_translation_evidence_x = 0.0f;
        track.horizontal_raw_left_motion_x = 0.0f;
        track.horizontal_raw_right_motion_x = 0.0f;

        // 趋势窗口只接收连续的身体框中心。原始 head 框是否出现不会改变
        // 身体坐标系；真正切到 head-only 或轨迹丢帧才重建窗口，避免把两种
        // 框尺度的切换写成水平运动。
        if (observation.head_only || box_semantics_changed) {
            track.partial_visibility_x_recovery_pending = false;
            track.accepted_partial_visibility_x_width = 0.0f;
            track.accepted_partial_visibility_x_side = 0.0f;
            track.partial_visibility_x_reference_width = 0.0f;
            track.horizontal_velocity_isolation_frames = 0;
            track.horizontal_trend_observation_isolation_frames = 0;
            track.horizontal_maneuver_frames = 0;
            track.horizontal_maneuver_direction = 0.0f;
            track.horizontal_translation_evidence_x = 0.0f;
            track.horizontal_control_translation_evidence_x = 0.0f;
            reset_horizontal_trend(track);
            reset_horizontal_direction_history(track);
        }
        if (!observation.head_only && box_semantics_changed) {
            append_horizontal_trend(
                track, track.state_at, observation_center_x_ratio);
        }

        if (track.horizontal_observation_initialized &&
            track.last_observation_head_only != observation.head_only) {
            reset_horizontal_raw_motion(track);
        }
        if (track.horizontal_observation_initialized &&
            track.last_observation_head_only == observation.head_only) {
            // 世界运动观察器读取相邻原始框的两边共同位移，而不是已被 vx
            // 预测抵消后的残差。这里显式使用“上一原始观测”的语义，不能
            // 使用保留身体坐标系的 track.head_only：连续只剩 head 框时，
            // 后者会始终保持 false，但第二个 head 观测起仍应提供平移证据。
            const float raw_x1_motion =
                observation.x1 - track.last_observation_x1;
            const float raw_x2_motion =
                observation.x2 - track.last_observation_x2;
            track.horizontal_raw_left_motion_x = raw_x1_motion;
            track.horizontal_raw_right_motion_x = raw_x2_motion;
            const float raw_common_motion =
                common_edge_motion(raw_x1_motion, raw_x2_motion);
            const float raw_shape_motion =
                std::fabs(raw_x2_motion - raw_x1_motion);
            const float raw_evidence =
                std::fabs(raw_common_motion) + raw_shape_motion;
            const float raw_translation_consistency = raw_evidence > 0.0f
                ? std::fabs(raw_common_motion) / raw_evidence : 0.0f;
            track.horizontal_translation_evidence_x =
                raw_common_motion == 0.0f ? 0.0f : std::copysign(
                    raw_translation_consistency, raw_common_motion);
            track.horizontal_control_translation_evidence_x =
                track.horizontal_translation_evidence_x;
            const double raw_delta_seconds =
                std::chrono::duration<double>(
                    track.state_at - track.last_observation_at).count();
            if (raw_delta_seconds > 0.0) {
                append_horizontal_raw_motion(
                    track,
                    raw_common_motion /
                        clamp_delta_seconds(raw_delta_seconds));
                if (track.horizontal_raw_motion.count >=
                    kTrackHorizontalRawMotionSampleCount) {
                    robust_horizontal_velocity_x =
                        median_horizontal_raw_velocity_x(track);
                    robust_horizontal_velocity_valid = true;
                    const float raw_delta =
                        clamp_delta_seconds(raw_delta_seconds);
                    const float robust_common_motion =
                        robust_horizontal_velocity_x * raw_delta;
                    const float robust_translation_evidence =
                        std::fabs(robust_common_motion) + raw_shape_motion;
                    robust_horizontal_translation_consistency =
                        robust_translation_evidence > 0.0f
                            ? std::fabs(robust_common_motion) /
                                  robust_translation_evidence
                            : 0.0f;
                    if (raw_common_motion * robust_horizontal_velocity_x >
                        0.0f) {
                        // 当前原始共同边提供新鲜方向，三观测中值只补回因
                        // 单帧轮廓幅值噪声损失的一致性。两者不同向、当前
                        // 静止或语义重建均不借用历史，连续驻留语义不变。
                        track.horizontal_control_translation_evidence_x =
                            std::copysign(
                                std::max(
                                    raw_translation_consistency,
                                    robust_horizontal_translation_consistency),
                                raw_common_motion);
                    }
                    const float previous_raw_center_x =
                        (track.last_observation_x1 +
                         track.last_observation_x2) * 0.5f;
                    const float raw_center_motion =
                        observation_center_x - previous_raw_center_x;
                    const float non_common_center_motion =
                        raw_center_motion - raw_common_motion;
                    float previous_trend_center_x_ratio = 0.0f;
                    if (!latest_horizontal_trend_center_x_ratio(
                            track, previous_trend_center_x_ratio)) {
                        previous_trend_center_x_ratio =
                            previous_raw_center_x / roi_width;
                    }
                    robust_horizontal_trend_center_x_ratio =
                        previous_trend_center_x_ratio +
                        (robust_common_motion +
                         non_common_center_motion) / roi_width;
                    robust_horizontal_trend_center_valid = true;
                    const float raw_motion_innovation_ratio =
                        std::fabs(
                            raw_common_motion - robust_common_motion) /
                        roi_width;
                    if (raw_motion_innovation_ratio >
                        kTrackHorizontalTrendChangePointMaximumRoiWidthRatio) {
                        // 中值只为把三个真实时间间隔的共同边测量对齐；门限
                        // 在换回本帧位移并除以 ROI 后判断，不比较 px/s。
                        horizontal_raw_motion_outlier = true;
                        isolate_horizontal_trend_observation = true;
                        track.horizontal_trend_observation_isolation_frames =
                            kTrackHorizontalTrendObservationIsolationFrames - 1;
                    }
                }
            }
        }

        if (!box_semantics_changed) {
            const float x1_residual = observation.x1 - track.x1;
            const float x2_residual = observation.x2 - track.x2;
            const float y1_residual = observation.y1 - track.y1;
            const float y2_residual = observation.y2 - track.y2;
            const bool x_edges_coherent = x1_residual * x2_residual > 0.0f;
            const bool y_edges_coherent = y1_residual * y2_residual > 0.0f;
            stable_motion_residual_x =
                common_edge_motion(x1_residual, x2_residual);
            horizontal_box_motion_residual_x = stable_motion_residual_x;
            stable_motion_residual_y =
                common_edge_motion(y1_residual, y2_residual);
            const float target_diagonal = std::max(
                1.0f,
                std::hypot(
                    (track.x2 - track.x1 + observation.x2 - observation.x1) *
                        0.5f,
                    (track.y2 - track.y1 + observation.y2 - observation.y1) *
                        0.5f));
            const float coherent_deformation_maximum =
                target_diagonal *
                kTrackCoherentDeformationMaximumTargetDiagonal;
            const float shape_change_minimum = std::max(
                kTrackCoherentDeformationMinimumShapeChangePixels,
                target_diagonal *
                    kTrackCoherentDeformationMinimumShapeChangeTargetDiagonal);
            const bool width_changed =
                std::fabs(x2_residual - x1_residual) > shape_change_minimum;
            const bool height_changed =
                std::fabs(y2_residual - y1_residual) > shape_change_minimum;
            const bool shape_changed = width_changed || height_changed;
            const bool pose_changed = width_changed && height_changed;
            const float horizontal_deformation_residual_x =
                isolate_horizontal_trend_observation &&
                        robust_horizontal_trend_center_valid
                    ? robust_horizontal_trend_center_x_ratio * roi_width -
                          track_center_x
                    : center_motion_residual_x;
            const auto update_deformation = [=](bool shape_evidence,
                                                float residual,
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
            // 分轴形变保护只能读取本轴尺度。旧实现把 width||height 同时
            // 喂给 X/Y，单纯高度轮廓噪声也会冻结 X 位置后验，而 vx 继续
            // 推进历史 reference，曾是实机 X 追边、回摆和反向停发的跨轴根因。
            update_deformation(width_changed,
                               horizontal_deformation_residual_x,
                               x_edges_coherent,
                               track.shape_deformation_x_frames);
            update_deformation(height_changed,
                               center_motion_residual_y,
                               y_edges_coherent,
                               track.shape_deformation_y_frames);
            const float maximum_x_edge_residual =
                std::max(std::fabs(x1_residual), std::fabs(x2_residual));
            const float minimum_x_edge_residual =
                std::min(std::fabs(x1_residual), std::fabs(x2_residual));
            const float track_width = track.x2 - track.x1;
            const float observation_width = observation.x2 - observation.x1;
            const bool partial_visibility_x =
                !observation.head_only && width_changed &&
                maximum_x_edge_residual > 0.0f &&
                observation_width <=
                    track_width * kTrackPartialVisibilityMaximumWidthRatio &&
                minimum_x_edge_residual / maximum_x_edge_residual <=
                    kTrackPartialVisibilityStableEdgeMaximumResidualRatio;
            const bool pose_center_trend_x =
                pose_changed && x_edges_coherent &&
                std::fabs(horizontal_deformation_residual_x) <=
                    coherent_deformation_maximum;
            // 原宽高共同形变继续使用中心趋势；另补“一边稳定、另一边
            // 内缩”的半身可见性证据。对称缩放和普通单轴动画保持旧路径。
            if (pose_center_trend_x || partial_visibility_x) {
                track.horizontal_center_trend_frames =
                    kTrackCoherentDeformationHoldFrames;
            } else if (track.horizontal_center_trend_frames > 0) {
                --track.horizontal_center_trend_frames;
            }
            if (partial_visibility_x) {
                track.horizontal_trend_observation_isolation_frames = 0;
                track.horizontal_maneuver_frames = 0;
                track.horizontal_maneuver_direction = 0.0f;
                horizontal_maneuver_active = false;
                // 残差较大的边就是疑似被裁掉的一侧。仅同一侧连续出现才
                // 累计；左右交替更像轮廓抖动，不能提前接受成新框。
                const float partial_side =
                    std::fabs(x1_residual) > std::fabs(x2_residual) ? -1.0f
                                                                    : 1.0f;
                const bool same_partial_side =
                    partial_side == track.partial_visibility_x_side;
                const int previous_partial_frames =
                    same_partial_side ? track.partial_visibility_x_frames : 0;
                if (same_partial_side) {
                    track.partial_visibility_x_frames =
                        std::min(track.partial_visibility_x_frames + 1,
                                 kTrackPartialVisibilityConfirmFrames);
                } else {
                    track.partial_visibility_x_side = partial_side;
                    track.partial_visibility_x_frames = 1;
                    track.partial_visibility_x_reference_width = track_width;
                }
                const float reference_width = std::max(
                    track.partial_visibility_x_reference_width,
                    observation_width);
                const float canonical_center_x = partial_side < 0.0f
                    ? observation.x2 - reference_width * 0.5f
                    : observation.x1 + reference_width * 0.5f;
                horizontal_trend_observation_center_x_ratio =
                    canonical_center_x / roi_width;
                preserve_horizontal_box = track.partial_visibility_x_frames <
                                          kTrackPartialVisibilityConfirmFrames;
                const bool confirmed_now =
                    previous_partial_frames ==
                        kTrackPartialVisibilityConfirmFrames - 1 &&
                    track.partial_visibility_x_frames ==
                        kTrackPartialVisibilityConfirmFrames;
                if (preserve_horizontal_box || confirmed_now) {
                    // 小目标单侧裁切的中心偏移可能不足 ROI 的 2%。几何证据
                    // 已成立时，前两点仍必须无条件与主 OLS 隔离；第三个同侧
                    // 点才确认新的可见构图，并按原时间戳重建趋势。
                    if (previous_partial_frames == 0) {
                        // 不能把此前由普通 ROI 创新留下的候选与单侧裁切
                        // 拼接；几何序列必须从自己的第一帧独立计数。
                        clear_horizontal_trend_change_point(track);
                    }
                    append_horizontal_trend_change_candidate(
                        track,
                        partial_side,
                        track.state_at,
                        horizontal_trend_observation_center_x_ratio);
                    isolate_horizontal_center_observation = true;
                    suppress_center_velocity_x = true;
                    isolate_fast_velocity_x = true;
                    track.horizontal_velocity_isolation_frames =
                        confirmed_now
                            ? kTrackHorizontalVelocityIsolationFrames
                            : kTrackHorizontalVelocityIsolationFrames;
                    if (confirmed_now) {
                        commit_horizontal_trend_change_point(track);
                        reset_horizontal_direction_history(track);
                        // 第三帧只确认新的可见框语义；已有平移速度保持连续，
                        // 候选中心不能再通过清零制造延迟投影阶跃。
                        track.horizontal_trend_rebuilding_from_partial = true;
                        track.partial_visibility_x_recovery_pending = true;
                        track.accepted_partial_visibility_x_width =
                            observation_width;
                        track.accepted_partial_visibility_x_side =
                            partial_side;
                        suppress_center_velocity_x = true;
                    }
                }
            } else {
                const bool abandoned_partial_candidate =
                    track.partial_visibility_x_frames > 0 &&
                    track.partial_visibility_x_frames <
                        kTrackPartialVisibilityConfirmFrames;
                track.partial_visibility_x_frames = 0;
                track.partial_visibility_x_side = 0.0f;
                if (abandoned_partial_candidate) {
                    clear_horizontal_trend_change_point(track);
                    track.partial_visibility_x_reference_width = 0.0f;
                }
            }
            const bool accepted_partial_visibility_x =
                track.partial_visibility_x_recovery_pending &&
                track.accepted_partial_visibility_x_width > 0.0f &&
                track.partial_visibility_x_reference_width > 0.0f &&
                observation_width <=
                    track.accepted_partial_visibility_x_width * 1.10f;
            if (accepted_partial_visibility_x) {
                const float canonical_center_x =
                    track.accepted_partial_visibility_x_side < 0.0f
                        ? observation.x2 -
                            track.partial_visibility_x_reference_width * 0.5f
                        : observation.x1 +
                            track.partial_visibility_x_reference_width * 0.5f;
                horizontal_trend_observation_center_x_ratio =
                    canonical_center_x / roi_width;
            }
            const bool full_visibility_recovery_x =
                !observation.head_only &&
                track.partial_visibility_x_recovery_pending && width_changed &&
                track.accepted_partial_visibility_x_width > 0.0f &&
                maximum_x_edge_residual > 0.0f &&
                observation_width >=
                    track.accepted_partial_visibility_x_width /
                        kTrackPartialVisibilityMaximumWidthRatio &&
                minimum_x_edge_residual / maximum_x_edge_residual <=
                    kTrackPartialVisibilityStableEdgeMaximumResidualRatio;
            if (full_visibility_recovery_x) {
                // 已接受的单侧半框恢复为完整框时，两种中心语义不能混进
                // 同一 OLS。以首个完整框重新播种；满五点前既不消费原始
                // 中心残差，也不把扩边误当成人物速度。
                reset_horizontal_trend(track);
                reset_horizontal_direction_history(track);
                track.partial_visibility_x_recovery_pending = false;
                track.accepted_partial_visibility_x_width = 0.0f;
                track.accepted_partial_visibility_x_side = 0.0f;
                track.partial_visibility_x_reference_width = 0.0f;
                append_horizontal_trend(
                    track, track.state_at, observation_center_x_ratio);
                track.horizontal_trend_rebuilding_from_partial = true;
                track.horizontal_center_trend_frames =
                    kTrackCoherentDeformationHoldFrames;
                isolate_horizontal_center_observation = true;
                suppress_center_velocity_x = true;
                isolate_fast_velocity_x = true;
                track.horizontal_velocity_isolation_frames =
                    kTrackHorizontalVelocityIsolationFrames;
                track.horizontal_trend_observation_isolation_frames = 0;
                track.horizontal_maneuver_frames = 0;
                track.horizontal_maneuver_direction = 0.0f;
                horizontal_maneuver_active = false;
            }
            if (!observation.head_only) {
                float direction_center_y_ratio = observation_center_y_ratio;
                if (track.horizontal_direction.count != 0) {
                    const std::size_t newest =
                        (track.horizontal_direction.next +
                         kTrackHorizontalTrendSampleCount - 1) %
                        kTrackHorizontalTrendSampleCount;
                    const float previous_direction_center_y_ratio =
                        track.horizontal_direction.center_y_ratio[newest];
                    const float vertical_common_motion = std::fabs(
                        common_edge_motion(y1_residual, y2_residual));
                    const float vertical_shape_motion =
                        std::fabs(y2_residual - y1_residual);
                    const float vertical_motion_total =
                        vertical_common_motion + vertical_shape_motion;
                    const float vertical_path_translation_consistency =
                        vertical_motion_total > 0.0f
                            ? vertical_common_motion /
                                  vertical_motion_total
                            : 0.0f;
                    direction_center_y_ratio =
                        previous_direction_center_y_ratio +
                        (direction_center_y_ratio -
                         previous_direction_center_y_ratio) *
                            vertical_path_translation_consistency *
                            vertical_path_translation_consistency;
                }
                append_horizontal_direction_history(
                    track, horizontal_trend_observation_center_x_ratio,
                    direction_center_y_ratio);
            }
            update_deformation(pose_changed,
                               center_motion_residual_y,
                               y_edges_coherent,
                               track.pose_deformation_y_frames);
            const float raw_translation_consistency =
                std::fabs(track.horizontal_translation_evidence_x);
            const float raw_motion_side =
                raw_translation_consistency <
                        kHorizontalTranslationEvidenceConsistencyMinimum
                    ? 0.0f
                    : std::copysign(
                          1.0f, track.horizontal_translation_evidence_x);
            const auto stage_horizontal_change_point =
                [&](float outside_side) {
                    // 候选前两帧继续使用不含异常点的主趋势。首次机动同时
                    // 隔离位置和速度；已确认机动已有三帧中值速度，可继续
                    // 消费鲁棒测量，只隔离候选位置，避免“保护”本身制造滞后。
                    suppress_center_velocity_x = true;
                    const bool isolate_candidate_velocity =
                        !horizontal_maneuver_active ||
                        !robust_horizontal_velocity_valid;
                    isolate_fast_velocity_x = isolate_candidate_velocity;
                    track.horizontal_velocity_isolation_frames =
                        isolate_candidate_velocity
                            ? kTrackHorizontalVelocityIsolationFrames : 0;
                    // 未确认候选不能把 raw 中心写入主窗，也不能让 240 Hz
                    // 时间轴缺样。用三帧中值共同位移积分出的替代中心占位；
                    // 若第三点确认真实新段，commit 会原子丢弃这些旧窗占位
                    // 并以候选原时间戳重建。
                    if (robust_horizontal_trend_center_valid) {
                        append_horizontal_trend(
                            track,
                            track.state_at,
                            robust_horizontal_trend_center_x_ratio);
                        horizontal_trend = estimate_horizontal_trend(
                            track, track.state_at);
                    }
                    append_horizontal_trend_change_candidate(
                        track,
                        outside_side,
                        track.state_at,
                        horizontal_trend_observation_center_x_ratio);
                    if (track.horizontal_trend_change.count <
                        kTrackHorizontalTrendChangePointConfirmSampleCount) {
                        return;
                    }
                    if (horizontal_maneuver_active) {
                        const HorizontalTrendChangePoint& active_change =
                            track.horizontal_trend_change;
                        const float directed_center_displacement_ratio =
                            (active_change.center_x_ratio[
                                 active_change.count - 1] -
                             active_change.center_x_ratio[0]) * outside_side;
                        if (outside_side ==
                                track.horizontal_maneuver_direction ||
                            directed_center_displacement_ratio <
                            kTrackHorizontalTrendChangePointMaximumRoiWidthRatio) {
                            // 快通道中的相机反馈与姿态三角波可能让相邻两边
                            // 连续同向，却只在 ROI 内往返很短距离。二次机动
                            // 必须同时反向并具备至少 2% ROI 的中心净位移；
                            // 同向加速本就属于当前机动，两类情况都不能重播种
                            // 速度。主 OLS 已逐帧接收鲁棒替代中心，无需再把
                            // 这三个 raw 候选重复写回或制造相同时间戳样本。
                            clear_horizontal_trend_change_point(track);
                            horizontal_trend = estimate_horizontal_trend(
                                track, track.state_at);
                            return;
                        }
                    }
                    const HorizontalTrendChangePoint confirmed_change =
                        track.horizontal_trend_change;
                    float confirmed_velocity_x =
                        robust_horizontal_velocity_valid
                            ? robust_horizontal_velocity_x : track.vx;
                    const double confirmed_duration =
                        std::chrono::duration<double>(
                            confirmed_change.captured_at[
                                confirmed_change.count - 1] -
                            confirmed_change.captured_at[0]).count();
                    if (!robust_horizontal_velocity_valid &&
                        confirmed_duration > 0.0) {
                        confirmed_velocity_x =
                            (confirmed_change.center_x_ratio[
                                 confirmed_change.count - 1] -
                             confirmed_change.center_x_ratio[0]) *
                            roi_width /
                            static_cast<float>(confirmed_duration);
                    }
                    commit_horizontal_trend_change_point(track);
                    horizontal_trend = {};
                    track.horizontal_maneuver_frames =
                        kTrackHorizontalManeuverFrames - 1;
                    track.horizontal_maneuver_direction = outside_side;
                    horizontal_maneuver_active = true;
                    track.horizontal_velocity_isolation_frames = 0;
                    // 当前确认帧只消费三点候选斜率的有界播种；从下一帧起
                    // 再开放 residual/dt 快通道，避免同一观测被重复计入。
                    const float raw_consistency_squared =
                        raw_translation_consistency *
                        raw_translation_consistency;
                    const float velocity_seed_gain =
                        kTrackHorizontalManeuverVelocitySeedGain *
                        raw_consistency_squared * raw_consistency_squared;
                    track.vx += (confirmed_velocity_x - track.vx) *
                                velocity_seed_gain;
                    isolate_fast_velocity_x = true;
                    suppress_center_velocity_x = false;
                };
            if (!observation.head_only) {
                if (isolate_horizontal_center_observation) {
                    // 候选未确认时继续按不含候选的主趋势推进；第三点提交后
                    // 新窗只有三个样本，会自然走下面的几何预热 fallback。
                    horizontal_trend =
                        estimate_horizontal_trend(track, track.state_at);
                } else if (horizontal_maneuver_active &&
                           track.horizontal_trend.count <
                               kTrackHorizontalTrendMinimumSampleCount) {
                    float candidate_side = 0.0f;
                    if (raw_motion_side != 0.0f) {
                        const float center_residual_ratio =
                            center_motion_residual_x / roi_width;
                        if (raw_motion_side !=
                                track.horizontal_maneuver_direction &&
                            std::fabs(center_residual_ratio) >
                                kTrackHorizontalTrendChangePointMaximumRoiWidthRatio &&
                            std::copysign(1.0f, center_residual_ratio) ==
                                raw_motion_side) {
                            candidate_side = raw_motion_side;
                        }
                    } else if (std::fabs(center_motion_residual_x) /
                                           roi_width >
                                       kTrackHorizontalTrendChangePointMaximumRoiWidthRatio) {
                        const float center_residual_side = std::copysign(
                            1.0f, center_motion_residual_x);
                        if (center_residual_side !=
                            track.horizontal_maneuver_direction) {
                            candidate_side = center_residual_side;
                        }
                    }
                    if (candidate_side != 0.0f) {
                        // 新段三点 OLS 尚未有效时，用相对预测中心的 ROI
                        // 空间创新补齐同一候选规则；仍不读取 px/s。
                        stage_horizontal_change_point(candidate_side);
                    } else {
                        clear_horizontal_trend_change_point(track);
                        append_horizontal_trend(
                            track, track.state_at,
                            isolate_horizontal_trend_observation &&
                                    robust_horizontal_trend_center_valid
                                ? robust_horizontal_trend_center_x_ratio
                                : horizontal_trend_observation_center_x_ratio);
                        horizontal_trend =
                            estimate_horizontal_trend(track, track.state_at);
                    }
                } else if ((horizontal_maneuver_active ||
                            track.horizontal_center_trend_frames > 0) &&
                           track.horizontal_trend.count >=
                               kTrackHorizontalTrendMinimumSampleCount) {
                    // 先用未包含当前观测的稳定窗口外推到当前采集时刻。
                    // 越界候选在确认前不进入主窗，避免单/双帧异常在恢复后
                    // 继续污染回归并形成延迟误重置。
                    horizontal_trend =
                        estimate_horizontal_trend(track, track.state_at);
                    if (horizontal_trend.valid) {
                        const float innovation =
                            horizontal_trend_observation_center_x_ratio -
                            horizontal_trend.position_ratio;
                        float outside_side = 0.0f;
                        if (innovation <
                            -kTrackHorizontalTrendChangePointMaximumRoiWidthRatio) {
                            outside_side = -1.0f;
                        } else if (
                            innovation >
                            kTrackHorizontalTrendChangePointMaximumRoiWidthRatio) {
                            outside_side = 1.0f;
                        }
                        if (horizontal_maneuver_active &&
                            track.horizontal_maneuver_direction != 0.0f) {
                            if (raw_motion_side != 0.0f) {
                                // 快通道内二次真实换向的当前位置可能仍落在旧
                                // 趋势同侧。相邻共同边只提供物理方向，仍须与
                                // 2% ROI 的趋势位置创新同侧才建立候选；否则
                                // 可能只是闭环相机反馈压过了本帧框宽变化。
                                outside_side =
                                    raw_motion_side !=
                                            track.horizontal_maneuver_direction &&
                                    outside_side == raw_motion_side
                                        ? raw_motion_side : 0.0f;
                            } else if (outside_side ==
                                       track.horizontal_maneuver_direction) {
                                outside_side = 0.0f;
                            }
                        }
                        if (outside_side != 0.0f) {
                            stage_horizontal_change_point(outside_side);
                        } else {
                            clear_horizontal_trend_change_point(track);
                            append_horizontal_trend(
                                track,
                                track.state_at,
                                isolate_horizontal_trend_observation &&
                                        robust_horizontal_trend_center_valid
                                    ? robust_horizontal_trend_center_x_ratio
                                    : horizontal_trend_observation_center_x_ratio);
                            horizontal_trend =
                                estimate_horizontal_trend(
                                    track, track.state_at);
                        }
                    } else {
                        clear_horizontal_trend_change_point(track);
                        append_horizontal_trend(
                            track, track.state_at,
                            isolate_horizontal_trend_observation &&
                                    robust_horizontal_trend_center_valid
                                ? robust_horizontal_trend_center_x_ratio
                                : horizontal_trend_observation_center_x_ratio);
                    }
                } else {
                    // OLS 尚未有效时只能继续预热；变点候选不能跨中心趋势
                    // 保护段或无效拟合累计。
                    clear_horizontal_trend_change_point(track);
                    append_horizontal_trend(
                        track, track.state_at,
                        isolate_horizontal_trend_observation &&
                                robust_horizontal_trend_center_valid
                            ? robust_horizontal_trend_center_x_ratio
                            : horizontal_trend_observation_center_x_ratio);
                    if (track.horizontal_center_trend_frames > 0) {
                        horizontal_trend =
                            estimate_horizontal_trend(track, track.state_at);
                    }
                }
            }
            const float horizontal_track_width = track.x2 - track.x1;
            const float horizontal_track_anchor_x = track.x1 +
                horizontal_track_width * track.aim_ratio_x;
            // 历史 reference 继续服务已验证的换向/普通 prediction 库存。
            // OLS 姿态段另看同帧观测框：predict_tracks() 会让旧框和 reference
            // 一起前移，只比较两者会漏掉当前观测拉回框后显出的趋势库存。
            const float horizontal_observation_anchor_x = observation.x1 +
                observation_width * track.aim_ratio_x;
            const float horizontal_half_range =
                config.body_aim_range_percent / 200.0f;
            const bool horizontal_old_prediction_inventory_visible =
                horizontal_track_width > 0.0f &&
                (track.horizontal_reference_x - horizontal_track_anchor_x) *
                    track.vx > 0.0f &&
                std::fabs(
                    track.horizontal_reference_x - horizontal_track_anchor_x) /
                        horizontal_track_width >=
                    horizontal_half_range *
                        kTrackHorizontalUnsupportedMotionMinimumAimRangeRatio;
            const bool horizontal_trend_prediction_inventory_same_side =
                track.horizontal_center_trend_frames > 0 &&
                observation_width > 0.0f &&
                (track.horizontal_reference_x -
                 horizontal_observation_anchor_x) * track.vx > 0.0f;
            const bool horizontal_trend_prediction_inventory_visible =
                horizontal_trend_prediction_inventory_same_side &&
                std::fabs(
                    track.horizontal_reference_x -
                    horizontal_observation_anchor_x) /
                        observation_width >=
                    horizontal_half_range *
                        kTrackHorizontalUnsupportedMotionMinimumAimRangeRatio;
            const bool horizontal_trend_prediction_inventory_unwinding =
                track.horizontal_center_trend_frames > 0 &&
                observation_width > 0.0f &&
                track.horizontal_prediction_unsupported_seconds > 0.0f;
            const bool horizontal_recent_raw_direction_confirmed =
                latest_two_horizontal_raw_motion_same_direction(
                    track, robust_horizontal_velocity_x);
            const bool horizontal_reversal_candidate_or_maneuver =
                horizontal_maneuver_active ||
                (!isolate_horizontal_center_observation &&
                 horizontal_recent_raw_direction_confirmed &&
                 (track.horizontal_trend_change.count > 0 ||
                  horizontal_old_prediction_inventory_visible));
            if (horizontal_reversal_candidate_or_maneuver &&
                robust_horizontal_velocity_valid &&
                robust_horizontal_velocity_x * track.vx < 0.0f &&
                std::fabs(
                    track.horizontal_control_translation_evidence_x) >=
                    kHorizontalTranslationEvidenceConsistencyMinimum) {
                // 三点共同边已经确认与旧 vx 反向，且 OLS 位置创新已进入
                // 独立候选/正式机动段，或旧方向 reference 库存已清晰可见。
                // 候选期不提前接纳中心；提交后继续同一差额直到 vx 过零，
                // 避免候选→机动交接反向折返一帧。
                const float consistency_squared =
                    robust_horizontal_translation_consistency *
                    robust_horizontal_translation_consistency;
                horizontal_prediction_correction_x =
                    (robust_horizontal_velocity_x * track.prediction_dt -
                     track.predicted_motion_x) *
                    consistency_squared;
                horizontal_reversal_prediction_corrected = true;
            }
            // 反向分支已用“新观测位移－旧预测位移”给出完整差额；低支持
            // 分支只处理停顿/减速，二者互斥以免重复撤销同一预测。
            if (!horizontal_reversal_prediction_corrected &&
                !config.enable_prediction &&
                (horizontal_old_prediction_inventory_visible ||
                 horizontal_trend_prediction_inventory_visible ||
                 horizontal_trend_prediction_inventory_unwinding) &&
                !observation.head_only && !box_semantics_changed &&
                !isolate_horizontal_center_observation &&
                !track.horizontal_trend_rebuilding_from_partial) {
                const float unsupported_weight =
                    horizontal_prediction_unsupported_weight(
                        track,
                        horizontal_trend_prediction_inventory_unwinding &&
                                horizontal_trend.valid
                            ? horizontal_trend.velocity_ratio * roi_width
                            : track.vx);
                if (unsupported_weight > 0.0f) {
                    const float ramp_seconds = std::max(
                        track.prediction_dt,
                        config.control_delay_ms / 1000.0f);
                    track.horizontal_prediction_unsupported_seconds =
                        std::min(
                            ramp_seconds,
                            track.horizontal_prediction_unsupported_seconds +
                                track.prediction_dt);
                    const float ramp =
                        track.horizontal_prediction_unsupported_seconds /
                        ramp_seconds;
                    horizontal_prediction_unsupported_blend =
                        unsupported_weight * ramp;
                    horizontal_trend_unsupported_blend =
                        (horizontal_trend_prediction_inventory_visible ||
                         horizontal_trend_prediction_inventory_unwinding)
                            ? horizontal_prediction_unsupported_blend
                            : 0.0f;
                    // 连续三次共同边都远小于旧预测时，渐入释放本帧旧 X
                    // 预测库存。该权重还会在下方把仍有效的 OLS 位置/速度
                    // 外推切回当前共同边；框、Y 和 OLS 历史本身不变，vx
                    // 仅经原速度锚连续收敛到新鲜共同边。观测重新支持趋势
                    // 后权重立即归零。
                    horizontal_prediction_correction_x -=
                        track.predicted_motion_x *
                        horizontal_prediction_unsupported_blend;
                } else {
                    track.horizontal_prediction_unsupported_seconds = 0.0f;
                }
            } else {
                track.horizontal_prediction_unsupported_seconds = 0.0f;
            }
            if (horizontal_maneuver_active &&
                robust_horizontal_velocity_valid) {
                // predict_tracks() 已先按旧 vx 推进控制点。三帧中值共同边
                // 速度给出本帧鲁棒位移，只校正“测量速度－预测速度”的
                // 差额；单/双帧中心跳变及其恢复脉冲不会直接移动 reference。
                stable_motion_residual_x =
                    (robust_horizontal_velocity_x - track.vx) *
                    track.prediction_dt;
                // 一致性只约束本帧共同边快通道。OLS 已由多帧可信中心构成，
                // 不能再让当前一帧的框形变置信度缩放完整趋势位置残差。
                const float consistency_squared =
                    robust_horizontal_translation_consistency *
                    robust_horizontal_translation_consistency;
                stable_motion_residual_x *=
                    consistency_squared * consistency_squared;
            }
            if (track.horizontal_center_trend_frames > 0) {
                if (horizontal_trend.valid) {
                    // 增长窗口相邻帧只增加一个样本，拟合端点仍经既有
                    // alpha 合入；不会在第 51 帧形成一步状态切换。
                    const float trend_position_residual_x =
                        horizontal_trend.position_ratio * roi_width -
                        track.horizontal_reference_x;
                    if (horizontal_maneuver_active) {
                        // 新窗第五点首次满足 OLS 有效条件，不能把快通道与
                        // OLS 端点的相位差一次写入历史 reference。仅在 5/6/7/8 点
                        // 按 1/4、1/2、3/4、1 连续交接；权重只由有效样本
                        // 数决定，不读取人物速度。
                        const float trend_blend = std::clamp(
                            static_cast<float>(
                                track.horizontal_trend.count -
                                kTrackHorizontalTrendMinimumSampleCount + 1) /
                                4.0f,
                            0.0f, 1.0f);
                        stable_motion_residual_x +=
                            (trend_position_residual_x -
                             stable_motion_residual_x) * trend_blend;
                    } else {
                        stable_motion_residual_x =
                            trend_position_residual_x;
                    }
                    if (!horizontal_maneuver_active &&
                        horizontal_trend_unsupported_blend > 0.0f) {
                        // predict_tracks() 已把旧 vx 同时写入身份框和 reference，
                        // 所以共同边残差是“当前观测位移－旧预测位移”。这里
                        // 补回同帧 predicted_motion_x，得到预测前坐标系下的新鲜
                        // 观测位移；再与上方等权撤销旧预测，避免重复反拉。
                        const float observed_motion_residual_x =
                            horizontal_box_motion_residual_x +
                            track.predicted_motion_x;
                        stable_motion_residual_x +=
                            (observed_motion_residual_x -
                             stable_motion_residual_x) *
                            horizontal_trend_unsupported_blend;
                    }
                    horizontal_trend_active = true;
                } else {
                    // 首四个观测尚不足以拟合斜率，沿用两侧共同位移；
                    // 该几何 fallback 不判断快慢，且只持续约 17 ms。
                    velocity_beta_x *=
                        kTrackDeformationVelocityBetaScaleMaximum;
                }
            } else if (track.shape_deformation_x_frames > 0) {
                stable_motion_residual_x = 0.0f;
                velocity_beta_x *= kTrackDeformationVelocityBetaScaleMaximum;
            }
            if (track.horizontal_trend_rebuilding_from_partial &&
                track.horizontal_trend.count >=
                    kTrackHorizontalTrendMinimumSampleCount) {
                track.horizontal_trend_rebuilding_from_partial = false;
            }
            // Y 与 X 使用同一种几何分解：两条边的共同位移是平移，边缘
            // 分歧是高度/姿态形变。旧路径用 |vy|>=8 px/s、18+4 帧状态机
            // 冻结后验，再让绝对 aim_y 撞击配置高度 ±5% 硬边界；真机中
            // 该边界占 31.38% 帧并发生 1781 次切换。这里改为无量纲连续
            // 一致性，不再按速度或驻留帧切换语义。
            const float vertical_common_motion = std::fabs(
                common_edge_motion(y1_residual, y2_residual));
            const float vertical_shape_motion =
                std::fabs(y2_residual - y1_residual);
            const float vertical_motion_total =
                vertical_common_motion + vertical_shape_motion;
            vertical_translation_consistency =
                vertical_motion_total > 0.0f
                    ? vertical_common_motion / vertical_motion_total
                    : 0.0f;
            if (track.shape_deformation_y_frames > 0) {
                const float translation_weight = y_edges_coherent
                    ? vertical_translation_consistency *
                          vertical_translation_consistency
                    : 0.0f;
                stable_motion_residual_y *= translation_weight;
                velocity_beta_y *=
                    kTrackDeformationVelocityBetaScaleMinimum +
                    (kTrackDeformationVelocityBetaScaleMaximum -
                     kTrackDeformationVelocityBetaScaleMinimum) *
                        translation_weight;
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
            stable_motion_residual_x =
                observation.aim_x - current_horizontal_aim_x(track);
            stable_motion_residual_y = observation.aim_y - track.aim_y;
            track.x1 += stable_motion_residual_x * alpha;
            track.x2 += stable_motion_residual_x * alpha;
            track.y1 += stable_motion_residual_y * alpha;
            track.y2 += stable_motion_residual_y * alpha;
        } else {
            if (preserve_horizontal_box) {
                // 单侧截断尚未确认时只消费两边都支持的平移，不把移动的
                // 可见边界写入身份框。持续第三帧会退出该分支并接受新框。
                const float dx = horizontal_box_motion_residual_x * alpha;
                track.x1 += dx;
                track.x2 += dx;
            } else if (horizontal_raw_motion_outlier &&
                       horizontal_maneuver_active &&
                       robust_horizontal_trend_center_valid) {
                // 已确认机动中的单步共同边脉冲不能再以 high alpha 平移身份
                // 框，否则即使趋势点已替换，公开内窗仍会随 raw 框跳动。
                // 中心改用 ROI 鲁棒替代点；宽度仍消费本帧观测，避免把正常
                // 姿态尺度变化误当成需要冻结的中心异常。
                const float center_residual =
                    robust_horizontal_trend_center_x_ratio * roi_width -
                    track_center_x;
                const float current_width = track.x2 - track.x1;
                const float observation_width = observation.x2 - observation.x1;
                const float half_width_delta =
                    (observation_width - current_width) * alpha * 0.5f;
                const float center_delta = center_residual * alpha;
                track.x1 += center_delta - half_width_delta;
                track.x2 += center_delta + half_width_delta;
            } else {
                track.x1 += (observation.x1 - track.x1) * alpha;
                track.x2 += (observation.x2 - track.x2) * alpha;
            }
            track.y1 += (observation.y1 - track.y1) * alpha;
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
            track.horizontal_reference_x = observed_aim_x;
            track.aim_y = observed_aim_y;
        } else {
            // predict_tracks() 已按速度完成本帧推进。常规段的共同边缘位移
            // 使用与关联框相同的增益；X 宽高共同形变段则使用时间趋势端点。
            // 其余框内形变或缓慢尺度变化使用独立低增益。
            track.horizontal_reference_x += stable_motion_residual_x * alpha;
            track.horizontal_reference_x += horizontal_prediction_correction_x;
            track.aim_y += stable_motion_residual_y * alpha;
            const float shape_alpha = high
                ? kTrackAimShapeAlphaHigh : kTrackAimShapeAlphaLow;
            const float horizontal_shape_alpha =
                track.horizontal_center_trend_frames > 0
                ? 0.0f : shape_alpha;
            track.horizontal_reference_x +=
                (observed_aim_x - track.horizontal_reference_x) *
                horizontal_shape_alpha;
            // 语义高度始终连续回归；姿态占主导时只按同一无量纲共同边
            // 一致性降低形状校正，不再冻结若干帧后阶跃恢复。
            const float vertical_shape_alpha =
                kTrackAimVerticalShapeAlpha;
            track.aim_y += (observed_aim_y - track.aim_y) *
                vertical_shape_alpha;
        }
        // 历史 reference 只服务速度/prediction 库存，仍限制在用户配置内窗，
        // 防止不可消费状态无限累积；该投影不再决定公开 base X。
        const float horizontal_width = track.x2 - track.x1;
        const float horizontal_half_range =
            config.body_aim_range_percent / 200.0f;
        const float horizontal_range_min_x = track.x1 +
            horizontal_width * (0.5f - horizontal_half_range);
        const float horizontal_range_max_x = track.x1 +
            horizontal_width * (0.5f + horizontal_half_range);
        track.horizontal_reference_x = std::clamp(
            track.horizontal_reference_x,
            horizontal_range_min_x,
            horizontal_range_max_x);
        track.horizontal_reference_x = std::clamp(
            track.horizontal_reference_x, track.x1, track.x2);
        if (horizontal_trend_active &&
            track.horizontal_observation_initialized &&
            !box_semantics_changed) {
            // body 关联中 head 是否被检出只改变置信度来源；aim_ratio_x/y
            // 始终仍由同一 body 配置定义。短暂 head 缺失不是框语义切换，
            // 不能因此绕过 X 往返约束并暴露 OLS/预测放大量。
            const float reference_motion_x =
                track.horizontal_reference_x -
                previous_horizontal_reference_x;
            const float observation_motion_x =
                observation.aim_x - track.last_observation_aim_x;
            const float reference_magnitude = std::fabs(reference_motion_x);
            const float observation_consistency = std::fabs(
                track.horizontal_control_translation_evidence_x);
            const float observation_magnitude =
                std::fabs(observation_motion_x) * observation_consistency *
                observation_consistency;
            if (reference_magnitude > 0.0f) {
                // 水平净位移/总路程继续区分单向平移与往返轮廓；再用同窗
                // X 路程占 X/Y 有效共同平移路程的比例，区分横移与垂直姿态。
                // 两者只连续调度本帧历史 reference 变化，不读取速度、频率或场景。
                // 精确单向横移的方向效率为 1，纯水平往返的路径占比为 1，
                // 均保留 Observation 支持的普通横向运动；低效率且 Y 主导时
                // 才降低 X 轮廓摆动，超出 Observation 的趋势放大量仍只由
                // 既有方向持久度约束。
                const float direction_efficiency =
                    horizontal_direction_efficiency(track);
                const float persistence_weight =
                    direction_efficiency * direction_efficiency;
                const float horizontal_path_share =
                    horizontal_direction_path_share(track);
                const float observation_path_weight =
                    persistence_weight +
                    (1.0f - persistence_weight) * horizontal_path_share *
                        horizontal_path_share;
                const float observation_supported_magnitude =
                    std::min(reference_magnitude, observation_magnitude) *
                    observation_path_weight;
                const float excess_magnitude = std::max(
                    0.0f, reference_magnitude - observation_magnitude);
                const float supported_magnitude =
                    observation_supported_magnitude +
                    excess_magnitude * persistence_weight;
                track.horizontal_reference_x =
                    previous_horizontal_reference_x +
                    std::copysign(supported_magnitude, reference_motion_x);
                track.horizontal_reference_x = std::clamp(
                    track.horizontal_reference_x,
                    horizontal_range_min_x,
                    horizontal_range_max_x);
                track.horizontal_reference_x = std::clamp(
                    track.horizontal_reference_x, track.x1, track.x2);
            }
        }
        // Y 配置高度仍通过连续回归定义，并投影到完整目标框；X 当前 base
        // 已由 Track 归一化锚点直接导出。
        track.aim_y = std::clamp(track.aim_y, track.y1, track.y2);
        if (box_semantics_changed) {
            // 头框和身体框的尺度定义不同，切换时不把几何变化解释为速度。
            track.vx *= 0.5f;
            track.vy *= 0.5f;
            track.shape_deformation_x_frames = 0;
            track.shape_deformation_y_frames = 0;
            track.horizontal_center_trend_frames = 0;
            track.partial_visibility_x_frames = 0;
            track.partial_visibility_x_side = 0.0f;
            track.pose_deformation_y_frames = 0;
        } else {
            // 常规段仍以低 beta 消费中心残差；X 姿态段只消费两条边共同
            // 支持的预测创新。共同量相对边缘分歧越高，平移证据越强；这个
            // 无量纲一致性只调连续增益，不读取人物绝对速度或游戏速度档位。
            if (horizontal_trend_active &&
                !suppress_center_velocity_x &&
                !horizontal_maneuver_active) {
                // fast isolation 只禁止 raw residual÷dt；候选本帧另由
                // suppress_center_velocity_x 阻断。候选撤回后的稳定 OLS
                // anchor 可继续低频收敛，避免固定五帧把真实趋势也冻结。
                float anchor_velocity_x =
                    horizontal_trend.velocity_ratio * roi_width;
                if (horizontal_trend_unsupported_blend > 0.0f &&
                    robust_horizontal_velocity_valid) {
                    // 位置回到当前共同边时，速度锚也必须使用同一证据连续
                    // 交接；只冻结锚会让旧 vx 永久保留，稍后又把位置推出。
                    anchor_velocity_x +=
                        (robust_horizontal_velocity_x - anchor_velocity_x) *
                        horizontal_trend_unsupported_blend;
                }
                const float anchor_correction_x =
                    anchor_velocity_x - track.vx;
                const float anchor_gain = 1.0f - std::exp(
                    -track.prediction_dt *
                    kTrackHorizontalVelocityAnchorGainPerSecond);
                track.vx += anchor_correction_x * anchor_gain;
            }
            if (horizontal_maneuver_active &&
                !isolate_fast_velocity_x &&
                robust_horizontal_velocity_valid) {
                const float consistency_squared =
                    robust_horizontal_translation_consistency *
                    robust_horizontal_translation_consistency;
                const float consistency_fourth =
                    consistency_squared * consistency_squared;
                const float velocity_scale =
                    kTrackHorizontalVelocityBetaScaleMinimum +
                    (kTrackHorizontalVelocityBetaScaleMaximum -
                     kTrackHorizontalVelocityBetaScaleMinimum) *
                        consistency_fourth;
                track.vx += beta * velocity_scale *
                            (robust_horizontal_velocity_x - track.vx);
            } else if (!horizontal_trend_active &&
                       !suppress_center_velocity_x &&
                       !isolate_fast_velocity_x) {
                track.vx += velocity_beta_x * center_motion_residual_x /
                            track.prediction_dt;
            }
            track.vy += velocity_beta_y * center_motion_residual_y /
                        track.prediction_dt;
        }
        clamp_vector(track.vx, track.vy,
                     diagonal * kMaxTrackSpeedDiagonalsPerSecond);
        track.confidence +=
            (observation.confidence - track.confidence) * alpha;
        track.last_observation_x1 = observation.x1;
        track.last_observation_x2 = observation.x2;
        track.last_observation_aim_x = observation.aim_x;
        track.last_observation_at = track.state_at;
        track.horizontal_observation_initialized = true;
        track.last_observation_head_only = observation.head_only;
        track.matched_observation_valid = true;
        track.matched_observation_x1 = observation.x1;
        track.matched_observation_y1 = observation.y1;
        track.matched_observation_x2 = observation.x2;
        track.matched_observation_y2 = observation.y2;
        track.matched_observation_head_only = observation.head_only;
        track.matched_observation_aim_from_head = observation.aim_from_head;
        track.predicted = false;
        track.lost_frames = 0;
        ++track.hits;
        if (track.hits >= config.min_confirmed_hits) {
            track.state = TrackState::CONFIRMED;
        }
    }

    void associate_stage(const std::vector<Observation>& observations,
                         bool high_stage, bool tentative_stage,
                         float diagonal, float roi_width,
                         std::vector<bool>& track_matched,
                         std::vector<bool>& observation_matched) {
        std::vector<std::size_t> track_indices;
        std::vector<std::size_t> observation_indices;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            // 确认/丢失轨迹先消费本置信度层的观测，暂定轨迹只能匹配
            // 剩余观测。短暂重复人体框因此不能在下一帧抢走当前人物，
            // 多个既有确认目标仍在同一层使用全局最优分配。
            const bool tentative =
                tracks[ti].state == TrackState::TENTATIVE;
            if (!track_matched[ti] && tentative == tentative_stage) {
                track_indices.push_back(ti);
            }
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
                float association_cost = (1.0f - iou) * 0.50f +
                    normalized_distance * 0.35f + shape_cost * 0.15f;
                if (track.id == selected_track_id &&
                    track.state == TrackState::CONFIRMED) {
                    association_cost = std::max(
                        0.0f,
                        association_cost - kSelectedTrackAssociationBias);
                }
                costs[row][column] = association_cost;
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
                                 observations[observation_index], diagonal,
                                 roi_width);
            track_matched[track_index] = true;
            observation_matched[observation_index] = true;
        }
    }

    void update_tracks(const std::vector<Observation>& observations,
                       const AimFrame& frame) {
        const float diagonal = std::hypot(
            static_cast<float>(frame.roi_width),
            static_cast<float>(frame.roi_height));
        const float roi_width = std::max(
            1.0f, static_cast<float>(frame.roi_width));
        predict_tracks(frame.captured_at, diagonal);
        std::vector<bool> track_matched(tracks.size(), false);
        std::vector<bool> observation_matched(observations.size(), false);
        associate_stage(observations, true, false, diagonal, roi_width,
                         track_matched, observation_matched);
        associate_stage(observations, true, true, diagonal, roi_width,
                         track_matched, observation_matched);
        associate_stage(observations,
                        false,
                        false,
                        diagonal,
                        roi_width,
                        track_matched,
                        observation_matched);
        associate_stage(observations,
                        false,
                        true,
                        diagonal,
                        roi_width,
                        track_matched,
                        observation_matched);

        for (std::size_t index = 0; index < tracks.size(); ++index) {
            if (track_matched[index]) continue;
            Track& track = tracks[index];
            // 没有相邻观测时不能跨缺帧沿用形变保持；重新匹配后必须从
            // 新的连续观测重新建立宽高与中心创新证据。
            track.shape_deformation_x_frames = 0;
            track.shape_deformation_y_frames = 0;
            track.horizontal_center_trend_frames = 0;
            track.partial_visibility_x_frames = 0;
            track.partial_visibility_x_side = 0.0f;
            track.pose_deformation_y_frames = 0;
            track.partial_visibility_x_recovery_pending = false;
            track.accepted_partial_visibility_x_width = 0.0f;
            track.accepted_partial_visibility_x_side = 0.0f;
            track.partial_visibility_x_reference_width = 0.0f;
            track.horizontal_velocity_isolation_frames = 0;
            track.horizontal_trend_observation_isolation_frames = 0;
            track.horizontal_maneuver_frames = 0;
            track.horizontal_maneuver_direction = 0.0f;
            track.horizontal_translation_evidence_x = 0.0f;
            track.horizontal_control_translation_evidence_x = 0.0f;
            track.horizontal_raw_left_motion_x = 0.0f;
            track.horizontal_raw_right_motion_x = 0.0f;
            track.horizontal_prediction_unsupported_seconds = 0.0f;
            track.horizontal_observation_initialized = false;
            track.last_observation_at = {};
            reset_horizontal_raw_motion(track);
            reset_horizontal_trend(track);
            reset_horizontal_direction_history(track);
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
            Track& created_track = tracks.back();
            if (!observation.head_only) {
                append_horizontal_trend(
                    created_track, frame.captured_at,
                    (observation.x1 + observation.x2) * 0.5f / roi_width);
                append_horizontal_direction_history(
                    created_track,
                    (observation.x1 + observation.x2) * 0.5f / roi_width,
                    (observation.y1 + observation.y2) * 0.5f /
                        std::max(1.0f, static_cast<float>(frame.roi_height)));
            }
            created_track.last_observation_x1 = observation.x1;
            created_track.last_observation_x2 = observation.x2;
            created_track.last_observation_aim_x = observation.aim_x;
            created_track.last_observation_at = frame.captured_at;
            created_track.horizontal_observation_initialized = true;
            created_track.last_observation_head_only = observation.head_only;
            created_track.matched_observation_valid = true;
            created_track.matched_observation_x1 = observation.x1;
            created_track.matched_observation_y1 = observation.y1;
            created_track.matched_observation_x2 = observation.x2;
            created_track.matched_observation_y2 = observation.y2;
            created_track.matched_observation_head_only = observation.head_only;
            created_track.matched_observation_aim_from_head =
                observation.aim_from_head;
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
                (current_horizontal_aim_x(track) - center_x) *
                    frame.source_pixels_per_roi_pixel_x,
                (track.aim_y - center_y) *
                    frame.source_pixels_per_roi_pixel_y) / diagonal;
            value += (1.0f - track.confidence) * 0.25f;
            if (track.predicted) value += 0.30f;
            if (track.id == selected_track_id) {
                value -= kSelectedTrackIdentityBias;
            }
            return value;
        };

        const auto range_distance = [&](const Track& track) {
            return std::hypot(
                current_horizontal_aim_x(track) - center_x,
                track.aim_y - center_y);
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
                               std::chrono::steady_clock::time_point issued_at,
                               float dx_counts,
                               float dy_counts) noexcept {
        // lock_active=false 时 Runtime 不会发送物理命令，历史必须记录零而
        // 不是预计算结果，否则延迟模型会补偿一段从未发生的相机响应。
        IssuedCommand& entry = issued_commands[issued_command_next];
        entry.sequence = frame.sequence;
        entry.issued_at = issued_at;
        entry.backend_completed_at = {};
        entry.dx_counts = frame.lock_active ? dx_counts : 0.0f;
        entry.dy_counts = frame.lock_active ? dy_counts : 0.0f;
        entry.backend_completed = false;
        issued_command_next =
            (issued_command_next + 1U) % issued_commands.size();
        issued_command_count = std::min(
            issued_command_count + 1U, issued_commands.size());
    }

    bool record_backend_completed_command(
            std::uint64_t sequence,
            std::chrono::steady_clock::time_point backend_completed_at,
            int dx_counts,
            int dy_counts) noexcept {
        if (sequence == 0 ||
            backend_completed_at ==
                std::chrono::steady_clock::time_point{}) {
            return false;
        }
        for (std::size_t offset = 0;
             offset < issued_command_count; ++offset) {
            const std::size_t index =
                (issued_command_next + issued_commands.size() - 1U - offset) %
                issued_commands.size();
            IssuedCommand& candidate = issued_commands[index];
            if (candidate.sequence != sequence) continue;
            if (candidate.backend_completed ||
                backend_completed_at < candidate.issued_at) {
                return false;
            }
            const bool confirms_requested_command =
                candidate.dx_counts == static_cast<float>(dx_counts) &&
                candidate.dy_counts == static_cast<float>(dy_counts);
            const bool confirms_no_command_applied =
                dx_counts == 0 && dy_counts == 0;
            if (!confirms_requested_command &&
                !confirms_no_command_applied) {
                return false;
            }
            candidate.dx_counts = static_cast<float>(dx_counts);
            candidate.dy_counts = static_cast<float>(dy_counts);
            candidate.backend_completed_at = backend_completed_at;
            candidate.backend_completed = true;
            return true;
        }
        return false;
    }

    std::pair<float, float> issued_command_at_delay(
            std::chrono::steady_clock::time_point query_at,
            float delay_seconds) const noexcept {
        const auto effective_at = query_at -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(delay_seconds));
        const IssuedCommand* best = nullptr;
        for (std::size_t offset = 0; offset < issued_command_count; ++offset) {
            const std::size_t index =
                (issued_command_next + issued_commands.size() - 1U - offset) %
                issued_commands.size();
            const IssuedCommand& candidate = issued_commands[index];
            const auto command_effective_at = candidate.backend_completed
                ? candidate.backend_completed_at : candidate.issued_at;
            if (command_effective_at <= effective_at) {
                best = &candidate;
                break;
            }
        }
        if (!best) return {0.0f, 0.0f};
        const float age_seconds = static_cast<float>(
            std::chrono::duration<double>(
                effective_at -
                (best->backend_completed
                     ? best->backend_completed_at : best->issued_at))
                .count());
        if (age_seconds > kControllerCommandHistoryMaximumAgeSeconds) {
            return {0.0f, 0.0f};
        }
        return {best->dx_counts, best->dy_counts};
    }

    std::pair<float, float> delayed_issued_command(
            std::chrono::steady_clock::time_point query_at) const
            noexcept {
        const float delay_seconds = config.enable_delay_compensation
            ? config.control_delay_ms / 1000.0f : 0.0f;
        return issued_command_at_delay(query_at, delay_seconds);
    }

    PendingIssuedCommandInventory pending_issued_command_inventory(
            std::chrono::steady_clock::time_point query_at) const
            noexcept {
        if (!config.enable_delay_compensation ||
            config.control_delay_ms <= 0.0f) {
            return {};
        }
        const auto effective_at = query_at -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(
                    config.control_delay_ms / 1000.0f));
        const float delay_seconds = config.control_delay_ms / 1000.0f;
        PendingIssuedCommandInventory inventory;
        for (std::size_t offset = 0; offset < issued_command_count; ++offset) {
            const std::size_t index =
                (issued_command_next + issued_commands.size() - 1U - offset) %
                issued_commands.size();
            const IssuedCommand& candidate = issued_commands[index];
            const auto command_effective_at = candidate.backend_completed
                ? candidate.backend_completed_at : candidate.issued_at;
            if (command_effective_at <= effective_at) break;
            if (command_effective_at <= query_at) {
                if (candidate.backend_completed) {
                    const float age_seconds = static_cast<float>(
                        std::chrono::duration<double>(
                            query_at - candidate.backend_completed_at)
                            .count());
                    const float remaining_age_weight = std::clamp(
                        1.0f - age_seconds / delay_seconds, 0.0f, 1.0f);
                    inventory.backend_completed_weighted_x +=
                        candidate.dx_counts * remaining_age_weight;
                }
                inventory.net_x += candidate.dx_counts;
                inventory.net_y += candidate.dy_counts;
                inventory.absolute_x += std::fabs(candidate.dx_counts);
                inventory.absolute_y += std::fabs(candidate.dy_counts);
                inventory.has_positive_x = inventory.has_positive_x ||
                    candidate.dx_counts > 0.0f;
                inventory.has_negative_x = inventory.has_negative_x ||
                    candidate.dx_counts < 0.0f;
            }
        }
        return inventory;
    }

    std::pair<float, float> stable_prediction_world_velocity(
            const AimFrame& frame, const Track& track) noexcept {
        if (!frame.lock_active || controller_track_id != track.id) {
            prediction_world_velocity_x = 0.0f;
            prediction_world_velocity_y = 0.0f;
            prediction_motion_candidate_x_seconds = 0.0f;
            prediction_motion_candidate_y_seconds = 0.0f;
            prediction_candidate_low_motion_x_frames = 0;
            prediction_candidate_low_motion_y_frames = 0;
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
            // 命令补偿后的世界测量仍负责前置候选建立；进入这里后，只在
            // 屏幕相对运动与已确认 prediction 同向且幅值更大时允许它跨越
            // 补偿低谷。静止目标的相机归位不能从零建立 prediction，也就
            // 不会被 Y 轴反拉保护误当成真实运动方向。
            const bool relative_motion_confirms_world_motion =
                relative_counts * prediction_velocity > 0.0f;
            return relative_motion_confirms_world_motion &&
                    std::fabs(relative_counts) > std::fabs(world_measurement)
                ? relative_counts : world_measurement;
        };
        const auto update_prediction_axis = [&](
                float feedforward, float world_measurement,
                float relative_velocity, float source_scale,
                float counts_per_pixel, float establishment_seconds,
                float& prediction_velocity,
                int& low_motion_frames, float& candidate_seconds,
                int& candidate_low_motion_frames,
                bool& external_motion_evidence) {
            if (!external_motion_evidence &&
                std::fabs(candidate_seconds) < establishment_seconds) {
                if (!aim::detail::update_prediction_motion_candidate(
                        world_measurement,
                        kPredictionWorldMotionMinimumCounts,
                        track.prediction_dt, establishment_seconds,
                        kPredictionEstablishmentLowMotionGraceFrames,
                        candidate_seconds, candidate_low_motion_frames)) {
                    prediction_velocity = 0.0f;
                    low_motion_frames = 0;
                    return;
                }
            }
            candidate_low_motion_frames = 0;
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
            if (low_motion_frames >= kPredictionStaticReleaseConfirmFrames) {
                prediction_velocity = 0.0f;
                low_motion_frames = 0;
                candidate_seconds = 0.0f;
                candidate_low_motion_frames = 0;
                external_motion_evidence = false;
            }
        };
        update_prediction_axis(
            feedforward_x, world_motion_measurement_x, track.vx,
            frame.source_pixels_per_roi_pixel_x, config.counts_per_pixel_x,
            kPredictionHorizontalMotionEstablishmentSeconds,
            prediction_world_velocity_x, prediction_low_motion_x_frames,
            prediction_motion_candidate_x_seconds,
            prediction_candidate_low_motion_x_frames,
            prediction_external_motion_evidence_x);
        update_prediction_axis(
            feedforward_y, world_motion_measurement_y, track.vy,
            frame.source_pixels_per_roi_pixel_y, config.counts_per_pixel_y,
            kPredictionVerticalMotionEstablishmentSeconds,
            prediction_world_velocity_y, prediction_low_motion_y_frames,
            prediction_motion_candidate_y_seconds,
            prediction_candidate_low_motion_y_frames,
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
        tracking_previous_error_x = 0.0f;
        tracking_error_derivative_x = 0.0f;
        tracking_error_derivative_initialized = false;
        world_motion_measurement_x = 0.0f;
        world_motion_measurement_y = 0.0f;
        prediction_world_velocity_x = 0.0f;
        prediction_world_velocity_y = 0.0f;
        prediction_motion_candidate_x_seconds = 0.0f;
        prediction_motion_candidate_y_seconds = 0.0f;
        prediction_candidate_low_motion_x_frames = 0;
        prediction_candidate_low_motion_y_frames = 0;
        prediction_external_motion_evidence_x = false;
        prediction_external_motion_evidence_y = false;
        prediction_offset_x = 0.0f;
        prediction_offset_y = 0.0f;
        prediction_control_offset_x = 0.0f;
        prediction_control_offset_y = 0.0f;
        prediction_pending_projection_x = 0.0f;
        prediction_opposite_public_brake_x_frames = 0;
        prediction_low_motion_x_frames = 0;
        prediction_low_motion_y_frames = 0;
        issued_commands = {};
        issued_command_next = 0;
        issued_command_count = 0;
        previous_command_x = 0.0f;
        previous_command_y = 0.0f;
        controller_at = {};
        controller_initialized = false;
        shaper_initialized = false;
    }

    LeadProjection projected_aim_point(
        const AimFrame& frame,
        const Track& track,
        std::chrono::steady_clock::time_point control_at) noexcept {
        // 当前基础 X 只由同帧 Track 框与配置比例导出。历史 OLS reference
        // 仍可服务速度/prediction，但不得在这里冒充 current feature。
        const float half_range = config.body_aim_range_percent / 200.0f;
        const float range_min_x = track.x1 + (track.x2 - track.x1) *
            (0.5f - half_range);
        const float range_max_x = track.x1 + (track.x2 - track.x1) *
            (0.5f + half_range);
        // 配置范围只作几何安全投影；prediction 层仍独立处理提前量。
        const float base_x = std::clamp(
            current_horizontal_aim_x(track),
            range_min_x,
            range_max_x);
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
        // prediction 的进入、延迟向量变化和退出都复用同一偏移限速器。
        // 退出时目标不是基础点，而是当前延迟补偿点；这样停止或相机反馈
        // 低谷不会把已经公开的提前量一帧清零，最终点只会按已验证的最大
        // 目标对角线/秒回收。该状态只改变最终点，不回写基础轨迹。
        const auto slew_prediction_offset = [&](float target_offset_x,
                                                  float target_offset_y) {
            aim::detail::slew_prediction_offset(
                target_offset_x, target_offset_y,
                projection.delay_x, projection.delay_y,
                box_diagonal, config.max_prediction_lead_percent,
                kPredictionOffsetMaximumSlewDiagonalsPerSecond,
                track.prediction_dt,
                prediction_offset_x, prediction_offset_y);
            projection.final_x = base_x + prediction_offset_x;
            projection.final_y = base_y + prediction_offset_y;
        };
        if (config.enable_prediction &&
            config.enable_delay_compensation &&
            track.state == TrackState::CONFIRMED && !track.predicted) {
            const float requested_delay_seconds =
                projection.observation_age_seconds +
                config.control_delay_ms / 1000.0f;
            const float horizontal_maximum_seconds =
                kGeometricProjectionMaximumSeconds;
            const float vertical_maximum_seconds =
                kGeometricProjectionMaximumSeconds;
            projection.delay_seconds_x = std::clamp(
                requested_delay_seconds, 0.0f,
                std::min(config.max_delay_compensation_ms / 1000.0f,
                         horizontal_maximum_seconds));
            projection.delay_seconds_y = std::clamp(
                requested_delay_seconds, 0.0f,
                std::min(config.max_delay_compensation_ms / 1000.0f,
                         vertical_maximum_seconds));
            projection.delay_x = track.vx * projection.delay_seconds_x;
            projection.delay_y = track.vy * projection.delay_seconds_y;
            if (controller_track_id == track.id) {
                const auto pending =
                    pending_issued_command_inventory(control_at);
                projection.delay_x -= pending.net_x *
                    kControllerPendingCommandResponse /
                    config.counts_per_pixel_x /
                    frame.source_pixels_per_roi_pixel_x;
                projection.delay_y -= pending.net_y *
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
            projection.delay_active =
                (projection.delay_seconds_x > 0.0f ||
                 projection.delay_seconds_y > 0.0f) &&
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
            prediction_candidate_low_motion_x_frames = 0;
            prediction_candidate_low_motion_y_frames = 0;
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
            prediction_candidate_low_motion_x_frames = 0;
            prediction_candidate_low_motion_y_frames = 0;
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
            prediction_candidate_low_motion_x_frames = 0;
            prediction_candidate_low_motion_y_frames = 0;
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
                std::min(projection.delay_seconds_y,
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
                    float& hold_time, bool allow_timeout) {
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
                // Y 轴保持只覆盖有界相机反馈低谷；同一状态达到真实时间上限
                // 后释放逐轴停发，避免固定速度分类让正确高度纠偏永久饿死。
                if (allow_timeout &&
                    hold_time >= kPredictionPullbackHoldTimeoutSeconds) {
                    hold = false;
                    hold_time = 0.0f;
                }
            };
            update_pullback_hold(
                world_velocity_x, base_x - frame.control_center_x,
                prediction_pullback_direction_x,
                prediction_pullback_hold_x,
                prediction_pullback_hold_time_x,
                false);
            update_pullback_hold(
                world_velocity_y, base_y - frame.control_center_y,
                prediction_pullback_direction_y,
                prediction_pullback_hold_y,
                prediction_pullback_hold_time_y,
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
                // 先把公开偏移平滑回收到当前延迟点，等待真实反向或锁定结束，
                // 切断“提前—复位—再预测”的可见跳变。X 轴保持仍由后续反拉
                // 门禁保护，Y 轴允许原有超时释放高度误差。
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
                if (lead_active) {
                    slew_prediction_offset(
                        projection.delay_x, projection.delay_y);
                    const float remaining_lead_x =
                        prediction_offset_x - projection.delay_x;
                    const float remaining_lead_y =
                        prediction_offset_y - projection.delay_y;
                    if (std::hypot(
                            remaining_lead_x, remaining_lead_y) > 0.001f) {
                        lead_axis_active_x =
                            std::fabs(remaining_lead_x) > activation_distance_x;
                        lead_axis_active_y =
                            std::fabs(remaining_lead_y) > activation_distance_y;
                        projection.active = true;
                        lead_candidate_frames = 0;
                        delay_lead_scale = 0.0f;
                        return projection;
                    }
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
            slew_prediction_offset(target_offset_x, target_offset_y);
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

    bool control_tracking(const AimFrame& frame, const Track& track,
                          float base_x, float base_y,
                          std::chrono::steady_clock::time_point
                              current_controller_at,
                          AimControlDiagnostics& diagnostics,
                          AimCommand& command) noexcept {
        diagnostics.evaluated = true;
        const float controller_dt = controller_at ==
                std::chrono::steady_clock::time_point{}
            ? track.prediction_dt
            : clamp_delta_seconds(std::chrono::duration<double>(
                  current_controller_at - controller_at).count());
        diagnostics.controller_dt_ms = controller_dt * 1000.0f;
        controller_at = current_controller_at;

        // 短时丢框不发送物理命令。PI 状态只按真实时间连续泄漏，不读取
        // 丢失帧数，也不在重获时重新注入旧滤波方向。
        if (track.predicted) {
            const float leak = std::exp(
                -kTrackingIntegralLeakPerSecond * controller_dt);
            feedforward_x *= leak;
            feedforward_y *= leak;
            filtered_x *= leak;
            filtered_y *= leak;
            shaped_x = filtered_x;
            shaped_y = filtered_y;
            residual_x = 0.0f;
            residual_y = 0.0f;
            previous_command_x = 0.0f;
            previous_command_y = 0.0f;
            tracking_previous_error_x = 0.0f;
            tracking_error_derivative_x = 0.0f;
            tracking_error_derivative_initialized = false;
            diagnostics.feedforward_x_counts = feedforward_x;
            diagnostics.filtered_x_counts = filtered_x;
            diagnostics.shaped_x_counts = shaped_x;
            record_issued_command(
                frame, current_controller_at, 0.0f, 0.0f);
            return false;
        }

        const float error_x =
            (base_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float error_y =
            (base_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        // 当前控制误差与 closing 导数使用同一帧 Track 锚点。51 点历史
        // reference 仍可服务速度/prediction，但不能再决定“当前”控制特征
        // 或是否消费本帧共同平移，否则会把 endpoint 的相位重新带回控制。
        const float tracking_center_x = current_horizontal_aim_x(track);
        const float track_center_error_x =
            (tracking_center_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float current_common_motion_x = common_edge_motion(
                track.horizontal_raw_left_motion_x,
                track.horizontal_raw_right_motion_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float current_common_consistency = std::clamp(
            std::fabs(track.horizontal_control_translation_evidence_x),
            0.0f, 1.0f);
        const bool use_current_common_motion_x =
            current_common_motion_x != 0.0f &&
            current_common_consistency > 0.0f;
        if (tracking_error_derivative_initialized) {
            const float track_center_motion_x =
                track_center_error_x - tracking_previous_error_x;
            const float derivative_motion_x = use_current_common_motion_x
                ? current_common_motion_x * current_common_consistency *
                      current_common_consistency
                : track_center_motion_x;
            tracking_error_derivative_x =
                (kTrackingErrorDerivativeFilterTimeSeconds *
                     tracking_error_derivative_x +
                 derivative_motion_x) /
                (kTrackingErrorDerivativeFilterTimeSeconds + controller_dt);
        } else {
            tracking_error_derivative_x = 0.0f;
            tracking_error_derivative_initialized = true;
        }
        tracking_previous_error_x = track_center_error_x;
        // Y 保留 fdf6b00 已经实机通过的径向 deadzone 输入。公开/current
        // base X 的语义变化不得借共享模长改写 Y；内部历史 reference 在此
        // 只作为冻结的 Y 尺度输入，不回到 X base、选择或 closing 路径。
        const float vertical_reference_error_x =
            (track.horizontal_reference_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float vertical_error_magnitude =
            std::hypot(vertical_reference_error_x, error_y);
        const float active_error_scale = vertical_error_magnitude > 0.0f
            ? std::max(
                0.0f,
                1.0f - config.deadzone_pixels / vertical_error_magnitude)
            : 0.0f;
        const float regularized_error_scale =
            active_error_scale * active_error_scale * active_error_scale;
        // X 是本轮唯一实验轴：它的死区比例不再被起跳/腾空的 Y 误差
        // 放大；Y 仍逐位复用既有径向 scale 和后续全部控制状态。
        const float x_error_magnitude = std::fabs(error_x);
        const float active_x_error_scale = x_error_magnitude > 0.0f
            ? std::max(
                0.0f,
                1.0f - config.deadzone_pixels / x_error_magnitude)
            : 0.0f;
        const float regularized_x_error_scale = active_x_error_scale *
            active_x_error_scale * active_x_error_scale;
        const float proportional_x = error_x * regularized_x_error_scale *
            config.counts_per_pixel_x;
        const float proportional_y = error_y * regularized_error_scale *
            config.counts_per_pixel_y;

        // 积分只消费当前图像特征误差。误差在死区外真实换边时，旧积分
        // 表示上一方向的扰动而不再有效，直接从零重建；这不是等待证据的
        // 换向门，当前比例项同一帧即可输出新方向。
        if (feedforward_x != 0.0f &&
            error_x * feedforward_x <= 0.0f) {
            feedforward_x = 0.0f;
        }
        if (feedforward_y != 0.0f &&
            error_y * feedforward_y <= 0.0f) {
            feedforward_y = 0.0f;
        }
        const float integral_leak = std::exp(
            -kTrackingIntegralLeakPerSecond * controller_dt);
        feedforward_x = feedforward_x * integral_leak +
            proportional_x * kTrackingIntegralGainPerSecond * controller_dt;
        feedforward_y = feedforward_y * integral_leak +
            proportional_y * kTrackingIntegralGainPerSecond * controller_dt;
        feedforward_x = std::clamp(
            feedforward_x,
            -config.max_counts_per_frame,
            config.max_counts_per_frame);
        feedforward_y = std::clamp(
            feedforward_y,
            -kTrackingVerticalIntegralMaximumCounts,
            kTrackingVerticalIntegralMaximumCounts);

        const float unconstrained_x = proportional_x + feedforward_x;
        const float unconstrained_y = proportional_y + feedforward_y;
        float desired_x = unconstrained_x;
        float desired_y = unconstrained_y;
        clamp_tracking_vector_preserving_y(
            desired_x, desired_y, config.max_counts_per_frame);
        // Åström/Rundqwist 的 tracking anti-windup：执行器实际可接受向量与
        // 线性 PI 请求之差连续回写状态。这里唯一硬上限就是既有物理向量
        // 上限，不再另建 pending、probe、相位或速度门。
        const float anti_windup_alpha = 1.0f - std::exp(
            -kTrackingAntiWindupGainPerSecond * controller_dt);
        feedforward_x += (desired_x - unconstrained_x) * anti_windup_alpha;
        feedforward_y += (desired_y - unconstrained_y) * anti_windup_alpha;
        feedforward_x = std::clamp(
            feedforward_x,
            -config.max_counts_per_frame,
            config.max_counts_per_frame);
        feedforward_y = std::clamp(
            feedforward_y,
            -kTrackingVerticalIntegralMaximumCounts,
            kTrackingVerticalIntegralMaximumCounts);

        const auto [delayed_command_x, delayed_command_y] =
            delayed_issued_command(current_controller_at);
        const auto pending =
            pending_issued_command_inventory(current_controller_at);
        // backend completion 只证明整数命令已完成后端接口，不代表已经观察到
        // 物理效果。这里不把 counts 投影成图像位移，只用完成时间在既有
        // 15 ms 控制窗中的剩余比例形成连续有符号库存；旧向库存越多，当前
        // 新向请求越平滑地收缩。它没有候选、驻留或锁存状态，也不会反向
        // 生成旧方向命令，因此不是 pending/reverse 门或 plant observer。
        if (desired_x != 0.0f) {
            const float request_direction = desired_x > 0.0f ? 1.0f : -1.0f;
            const float opposed_completed_x = std::max(
                0.0f,
                -request_direction * pending.backend_completed_weighted_x);
            if (opposed_completed_x > 0.0f) {
                const float requested_magnitude = std::fabs(desired_x);
                const float history_adjusted_x = desired_x *
                    requested_magnitude /
                    (requested_magnitude + opposed_completed_x);
                // 复用现有 30/s tracking time constant，把历史整形造成的
                // 请求差连续回写 X 积分；上方饱和 back-calculation 保持原样。
                feedforward_x +=
                    (history_adjusted_x - desired_x) * anti_windup_alpha;
                feedforward_x = std::clamp(
                    feedforward_x,
                    -config.max_counts_per_frame,
                    config.max_counts_per_frame);
                desired_x = history_adjusted_x;
            }
        }
        diagnostics.proportional_x_counts = proportional_x;
        diagnostics.feedforward_x_counts = feedforward_x;
        diagnostics.desired_before_reverse_x_counts = unconstrained_x;
        diagnostics.delayed_command_x_counts = delayed_command_x;
        diagnostics.pending_net_x_counts = pending.net_x;
        diagnostics.pending_absolute_x_counts = pending.absolute_x;
        diagnostics.pending_positive_x = pending.has_positive_x;
        diagnostics.pending_negative_x = pending.has_negative_x;

        // 分轴一阶滤波保留用户 smoothing。旧二维方向重排会把既有 Y 模长
        // 瞬时搬到 X；这里每轴独立按自身零点连续通过，不再共享模长。
        const auto filter_axis = [&](float desired, float error,
                                     float& filtered) {
            // 原始图像误差已经过零时，即使还落在软死区、比例项为零，也
            // 必须清掉上一侧的滤波输出；否则它会继续把准星推过目标，直至
            // 反侧走出死区才释放，形成可见的小幅往返。
            if (filtered != 0.0f && filtered * error <= 0.0f) {
                filtered = 0.0f;
                return;
            }
            if (!controller_initialized) {
                filtered = desired;
                return;
            }
            const float candidate =
                filtered + (desired - filtered) * config.smoothing;
            filtered = candidate * desired < 0.0f ? 0.0f : candidate;
        };
        filter_axis(desired_x, error_x, filtered_x);
        filter_axis(desired_y, error_y, filtered_y);
        controller_initialized = true;
        clamp_tracking_vector_preserving_y(
            filtered_x, filtered_y, config.max_counts_per_frame);
        // 导数状态不回写 PI、anti-windup 或既有 smoothing。只消费朝零
        // closing slope，并从当前误差同向的 X 请求中连续扣减；扣减预算
        // 不超过该同向余量，因此滤波残留不能自行产生反向命令。
        const float error_direction_x = error_x > 0.0f
            ? 1.0f : (error_x < 0.0f ? -1.0f : 0.0f);
        const float same_direction_request_x = std::max(
            0.0f, error_direction_x * filtered_x);
        const bool derivative_closing_evidence_x =
            !use_current_common_motion_x ||
            error_x * current_common_motion_x < 0.0f;
        const float closing_slope_x = derivative_closing_evidence_x
            ? std::max(
                  0.0f, -error_direction_x * tracking_error_derivative_x)
            : 0.0f;
        const float derivative_damping_x = std::min(
            same_direction_request_x,
            kTrackingErrorDerivativeGainSeconds * closing_slope_x *
                config.counts_per_pixel_x);
        // backend completion 不是物理位移；只有当前 Observation 已确认误差
        // 朝零闭合时，才把同向完成库存作为“响应仍在控制窗内”的连续预算。
        // 用真实 closing slope 在既有控制延迟内可覆盖当前误差的比例调度该
        // 预算，不新增 counts→pixels plant 换算、固定速度/频率阈值或状态
        // 门，并且最多把剩余请求收缩到零。
        const float same_direction_completed_x = std::max(
            0.0f,
            error_direction_x * pending.backend_completed_weighted_x);
        const float closing_horizon_x = closing_slope_x *
            config.control_delay_ms / 1000.0f;
        const float closing_response_share =
            std::fabs(error_x) + closing_horizon_x > 0.0f
            ? closing_horizon_x /
                (std::fabs(error_x) + closing_horizon_x)
            : 0.0f;
        const float remaining_same_direction_request_x =
            same_direction_request_x - derivative_damping_x;
        const float closing_response_budget_x =
            remaining_same_direction_request_x +
            same_direction_completed_x;
        const float closing_response_taper_x =
            closing_response_budget_x > 0.0f
            ? remaining_same_direction_request_x *
                same_direction_completed_x /
                closing_response_budget_x * closing_response_share
            : 0.0f;
        diagnostics.closing_response_tapered_x =
            closing_response_taper_x > 0.0f;
        shaped_x = filtered_x - error_direction_x *
            (derivative_damping_x + closing_response_taper_x);
        shaped_y = filtered_y;
        shaper_initialized = true;
        residual_x = 0.0f;
        residual_y = 0.0f;
        diagnostics.desired_x_counts = shaped_x;
        diagnostics.filtered_x_counts = filtered_x;
        diagnostics.shaped_x_counts = shaped_x;
        diagnostics.residual_before_quantization_x_counts = 0.0f;

        command.sequence = frame.sequence;
        command.captured_at = frame.captured_at;
        command.dx_counts = static_cast<int>(std::lround(shaped_x));
        command.dy_counts = static_cast<int>(std::lround(shaped_y));
        clamp_tracking_command_preserving_y(
            command.dx_counts, command.dy_counts,
            config.max_counts_per_frame);
        diagnostics.quantization_zero_x =
            command.dx_counts == 0 && std::fabs(desired_x) > 0.001f;
        diagnostics.deadzone_quiet =
            x_error_magnitude <= config.deadzone_pixels &&
            vertical_error_magnitude <= config.deadzone_pixels &&
            command.dx_counts == 0 && command.dy_counts == 0;
        previous_command_x = static_cast<float>(command.dx_counts);
        previous_command_y = static_cast<float>(command.dy_counts);
        record_issued_command(
            frame, current_controller_at,
            previous_command_x, previous_command_y);
        return command.dx_counts != 0 || command.dy_counts != 0;
    }

    bool control(const AimFrame& frame, const Track& track,
                 float base_x, float base_y,
                 float tracking_x, float tracking_y,
                 float aim_x, float aim_y,
                 float modelled_response_x_counts,
                 std::chrono::steady_clock::time_point current_controller_at,
                 AimControlDiagnostics& diagnostics,
                 AimCommand& command) noexcept {
        if (controller_track_id != track.id) {
            reset_controller();
            controller_track_id = track.id;
        }
        diagnostics = {};
        diagnostics.evaluated = true;
        diagnostics.reverse_translation_raw_left_x_roi_pixels =
            track.horizontal_raw_left_motion_x;
        diagnostics.reverse_translation_raw_right_x_roi_pixels =
            track.horizontal_raw_right_motion_x;
        diagnostics.reverse_translation_raw_common_x_roi_pixels =
            common_edge_motion(
                track.horizontal_raw_left_motion_x,
                track.horizontal_raw_right_motion_x);
        diagnostics.reverse_translation_control_evidence_x =
            track.horizontal_control_translation_evidence_x;
        diagnostics.modelled_response_x_counts =
            modelled_response_x_counts;
        if (config.enable_delay_compensation &&
            !config.enable_prediction) {
            return control_tracking(
                frame, track, base_x, base_y,
                current_controller_at, diagnostics, command);
        }
        // 轨迹估计继续严格使用 captured_at；控制滤波、泄漏、slew 与
        // 命令库存统一使用 process() 解析出的同一控制时刻。
        if (track.predicted && !config.enable_prediction) {
            // 短时丢框仍禁止发送物理命令，但同一轨迹的基础保持量不能重置。
            // 否则重新观测后会从纯比例控制重新学习恒速偏差，形成一次明显
            // 落后。这里只按真实帧间隔泄漏积分并推进控制时钟；滤波、整形和
            // 亚整数残余保持冻结，恢复帧仍受当前误差方向门禁约束。
            const float controller_dt = controller_at ==
                    std::chrono::steady_clock::time_point{}
                ? track.prediction_dt
                : clamp_delta_seconds(std::chrono::duration<double>(
                      current_controller_at - controller_at).count());
            const float leak = std::exp(
                -kControllerFeedforwardLeakPerSecond * controller_dt);
            feedforward_x *= leak;
            feedforward_y *= leak;
            diagnostics.controller_dt_ms = controller_dt * 1000.0f;
            diagnostics.feedforward_x_counts = feedforward_x;
            previous_command_x = 0.0f;
            previous_command_y = 0.0f;
            controller_at = current_controller_at;
            record_issued_command(
                frame, current_controller_at, 0.0f, 0.0f);
            return false;
        }
        const float base_error_x =
            (base_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float base_error_y =
            (base_y - frame.control_center_y) *
            frame.source_pixels_per_roi_pixel_y;
        const float public_error_x =
            (aim_x - frame.control_center_x) *
            frame.source_pixels_per_roi_pixel_x;
        const float public_error_y =
            (aim_y - frame.control_center_y) *
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
        const float controller_dt = controller_at ==
                std::chrono::steady_clock::time_point{}
            ? track.prediction_dt
            : clamp_delta_seconds(std::chrono::duration<double>(
                  current_controller_at - controller_at).count());
        diagnostics.controller_dt_ms = controller_dt * 1000.0f;
        float pending_control_projection_target_x = 0.0f;
        if (use_coherent_prediction_projection_x) {
            const auto pending =
                pending_issued_command_inventory(current_controller_at);
            // 稳定世界速度本来就需要在反馈窗内保留一份命令库存，不能把
            // 全部 pending 都当成过冲。只投影超过稳定预算的部分，连续小
            // 命令不受影响，-4~-6 counts 的脉冲库存则会提前触发制动。
            const float expected_pending_x = prediction_world_velocity_x *
                config.control_delay_ms / 1000.0f;
            const float excess_pending_x =
                pending.net_x - expected_pending_x;
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
        controller_at = current_controller_at;
        // prediction 路径继续复用同一总投影偏移状态；prediction 关闭的
        // tracking 已在上方进入独立图像特征 PI，不会执行本段观察器。
        const float proportional_x =
            error_x * config.counts_per_pixel_x * gain;
        const float proportional_y =
            error_y * config.counts_per_pixel_y * gain;
        const float hold_band = std::max(
            kControllerIntegralMinimumErrorPixels,
            config.deadzone_pixels * 1.5f);
        const bool previous_command_zero =
            previous_command_x == 0.0f && previous_command_y == 0.0f;

        const auto [delayed_command_x, delayed_command_y] =
            delayed_issued_command(current_controller_at);
        const auto pending_inventory =
            pending_issued_command_inventory(current_controller_at);
        diagnostics.delayed_command_x_counts = delayed_command_x;
        diagnostics.pending_net_x_counts = pending_inventory.net_x;
        diagnostics.pending_absolute_x_counts = pending_inventory.absolute_x;
        diagnostics.pending_positive_x = pending_inventory.has_positive_x;
        diagnostics.pending_negative_x = pending_inventory.has_negative_x;
        // track.v* 是目标相对屏幕的速度，包含历史鼠标命令造成的相机运动。
        // 将预计当前生效的历史命令补回后，measurement 才是世界目标在本帧
        // 需要的维持量。该观察器不依赖基础点过零或相对速度符号猜测反转。
        const auto update_feedforward = [&](float base_error,
                                            float relative_velocity,
                                            float source_scale,
                                            float counts_per_pixel,
                                            float delayed_command,
                                            float observer_gain_per_second,
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
                    -observer_gain_per_second *
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
            base_error_x, track.vx,
            frame.source_pixels_per_roi_pixel_x,
            config.counts_per_pixel_x, delayed_command_x,
            kControllerFeedforwardObserverGainPerSecond,
            feedforward_x, world_motion_measurement_x,
            prediction_external_motion_evidence_x);
        update_feedforward(
            base_error_y, track.vy, frame.source_pixels_per_roi_pixel_y,
            config.counts_per_pixel_y, delayed_command_y,
            kControllerFeedforwardObserverGainPerSecond, feedforward_y,
            world_motion_measurement_y,
            prediction_external_motion_evidence_y);
        float control_feedforward_x = feedforward_x;
        if (use_coherent_prediction_projection_x &&
            controller_dt >=
                kPredictionDirectFeedforwardMinimumDeltaSeconds) {
            // prediction 点与控制器必须消费同一份已确认世界速度。否则公开
            // 点按稳定速度前探，物理控制却在相机反馈低谷释放基础前馈，最终
            // 只能长期保留比例误差来维持移动，表现为准星贴不到预测标记。
            // 幅度按当前真实 dt 积分，有限校正只补偿离散命令与实际镜头
            // 反馈之间的小幅损失，不改变 coherent/时间前提或单帧上限。
            control_feedforward_x = std::clamp(
                prediction_world_velocity_x * controller_dt *
                    kPredictionDirectFeedforwardScale,
                -kControllerFeedforwardMaximumCounts,
                kControllerFeedforwardMaximumCounts);
        }
        float desired_x = proportional_x + control_feedforward_x;
        float desired_y = proportional_y + feedforward_y;
        diagnostics.proportional_x_counts = proportional_x;
        diagnostics.feedforward_x_counts = control_feedforward_x;
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
        diagnostics.desired_before_reverse_x_counts = desired_x;
        diagnostics.desired_x_counts = desired_x;
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
            // Y 轴必须使用与 X 相同的因果释放条件。人物姿态变化和相机反馈
            // 可能短暂建立垂直 prediction；若历史方向无条件覆盖当前高度
            // 纠偏，基础点已经偏离几十像素时仍会长期停发，随后形成大幅
            // 反向脉冲。只有世界测量仍支持历史方向且命令朝当前最终点时
            // 放行，停止、反馈低谷和真实反向继续受保护。
            const bool allow_y_lead_release =
                aim::detail::prediction_pullback_command_allowed(
                    desired_y, error_y, world_motion_measurement_y,
                    lead_direction_y,
                    kPredictionWorldMotionMinimumCounts);
            const bool allow_y_pullback_release =
                aim::detail::prediction_pullback_command_allowed(
                    desired_y, error_y, world_motion_measurement_y,
                    prediction_pullback_direction_y,
                    kPredictionWorldMotionMinimumCounts);
            // 配置高度已经偏离 8 px 以上时，历史垂直 prediction 不能继续
            // 享有无限优先级。只放行同时朝当前最终点和基础高度的纠偏；
            // 方向整形、二维上限和后续整数方向门禁仍会限制物理命令。
            const bool allow_y_height_recovery =
                std::fabs(base_error_y) > 8.0f &&
                desired_y * error_y > 0.0f &&
                desired_y * base_error_y > 0.0f;
            // 公有命令逐轴量化；只要该轴存在有效世界方向就必须阻止反拉，
            // 不能因正交轴幅值更大而用归一化 0.1 门槛丢掉水平保护。
            if (lead_active && std::fabs(lead_direction_x) > 0.001f &&
                desired_x * lead_direction_x < 0.0f &&
                !allow_x_lead_release) {
                desired_x = 0.0f;
            }
            if (lead_active && lead_axis_active_y &&
                std::fabs(lead_direction_y) > 0.001f &&
                desired_y * lead_direction_y < 0.0f &&
                !allow_y_lead_release && !allow_y_height_recovery) {
                desired_y = 0.0f;
            }
            if (prediction_pullback_hold_x &&
                desired_x * prediction_pullback_direction_x < 0.0f &&
                !allow_x_pullback_release) {
                desired_x = 0.0f;
            }
            if (prediction_pullback_hold_y &&
                desired_y * prediction_pullback_direction_y < 0.0f &&
                !allow_y_pullback_release && !allow_y_height_recovery) {
                desired_y = 0.0f;
            }
        }
        diagnostics.desired_x_counts = desired_x;
        // 进入死区也不能按某个像素速度阈值硬清状态；只有上一命令、到期
        // 命令和整个在途窗都归零，才可证明执行器库存已经安静。
        const bool control_inventory_quiet =
            previous_command_zero &&
            std::fabs(delayed_command_x) <= 0.001f &&
            std::fabs(delayed_command_y) <= 0.001f &&
            pending_inventory.absolute_x <= 0.001f &&
            pending_inventory.absolute_y <= 0.001f;
        if (inside_deadzone && std::hypot(feedforward_x, feedforward_y) < 0.05f &&
            std::hypot(shaped_x, shaped_y) <= 1.0f &&
            control_inventory_quiet) {
            filtered_x = 0.0f;
            filtered_y = 0.0f;
            shaped_x = 0.0f;
            shaped_y = 0.0f;
            residual_x = 0.0f;
            residual_y = 0.0f;
            previous_command_x = 0.0f;
            previous_command_y = 0.0f;
            controller_at = current_controller_at;
            diagnostics.deadzone_quiet = true;
            diagnostics.filtered_x_counts = filtered_x;
            diagnostics.shaped_x_counts = shaped_x;
            record_issued_command(
                frame, current_controller_at, 0.0f, 0.0f);
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
        diagnostics.filtered_x_counts = filtered_x;

        // 到期命令与屏幕相对运动同向，说明外部目标运动压过了相机反馈；
        // 库存安静时则从同一个真实 dt 速率起步。两者都是延迟/因果证据，
        // 不再按人物的 ROI 像素速度在两套整形策略间切档。
        const bool causal_external_motion =
            (std::fabs(delayed_command_x) > 0.001f &&
             delayed_command_x *
                     track.horizontal_control_translation_evidence_x >
                 0.0f) ||
            (std::fabs(delayed_command_y) > 0.001f &&
             delayed_command_y * track.vy > 0.0f);
        const bool smooth_delayed_motion =
            config.enable_delay_compensation &&
            config.control_delay_ms > 0.0f &&
            (!config.enable_prediction || control_inventory_quiet ||
             causal_external_motion);
        const bool delayed_tracking_x_guard =
            config.enable_delay_compensation &&
            config.control_delay_ms > 0.0f &&
            !config.enable_prediction;
        const bool shaper_was_initialized = shaper_initialized;
        const float previous_shaped_x = shaped_x;
        const float maximum_delta = smooth_delayed_motion
            ? std::max(
                0.25f,
                kControllerMaximumSlewCountsPerSecond * controller_dt)
            : std::max(
                1.0f, config.max_counts_per_frame *
                    std::max(0.10f, config.smoothing));
        if (!shaper_initialized) {
            shaped_x = filtered_x;
            shaped_y = filtered_y;
            shaper_initialized = true;
        } else {
            float delta_x = filtered_x - shaped_x;
            float delta_y = filtered_y - shaped_y;
            // 比例滤波负责低频响应，整形器按真实 dt 限制相邻物理命令的可见阶跃。
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
            diagnostics.shaper_direction_reset_x =
                std::fabs(shaped_x) > 0.001f;
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
        if (shaper_was_initialized && delayed_tracking_x_guard) {
            // 上面的方向对齐会保持向量模长，但若 desired 从近竖直旋到近
            // 水平，它也可能把既有 Y 模长一帧搬到 X，绕过刚执行的向量
            // slew。真实 Run 已出现 (0,-6)->(8,-5) 一类阶跃。反向轴仍可
            // 立即降到零以满足安全方向契约；tracking X 的任何同向新增
            // 幅度则继续受本帧 maximum_delta 约束，保证零后重新增长不能
            // 绕过 240 counts/s 门禁。Y 与 prediction 保持既有路径，避免
            // 用本轮 X 实验改写尚未出现同类证据的轴/profile。
            if (shaped_x != 0.0f &&
                previous_shaped_x * shaped_x < 0.0f) {
                shaped_x = 0.0f;
                residual_x = 0.0f;
                diagnostics.post_alignment_sign_change_blocked_x = true;
            } else if (std::fabs(shaped_x) >
                       std::fabs(previous_shaped_x)) {
                const float unconstrained_growth_x =
                    shaped_x - previous_shaped_x;
                const float growth_x = std::clamp(
                    unconstrained_growth_x,
                    -maximum_delta, maximum_delta);
                shaped_x = previous_shaped_x + growth_x;
                diagnostics.post_alignment_growth_limited_x =
                    std::fabs(unconstrained_growth_x - growth_x) > 0.001f;
                // 旧残余属于上一轴向模长。即使本帧增长没有触及浮点
                // maximum_delta，它也不能跨入新的增长阶跃，否则 0.8 的
                // 旧余量会把允许增加 1 count 的命令量化成增加 2 counts。
                // 非增长帧仍保留正常误差扩散，低速亚像素响应不会被关闭。
                residual_x = 0.0f;
            }
            clamp_vector(shaped_x, shaped_y, config.max_counts_per_frame);
        }
        diagnostics.shaped_x_counts = shaped_x;
        diagnostics.residual_before_quantization_x_counts = residual_x;
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
            // 基础追踪只消费已滤波运动方向与当前需求的一致性，不再按
            // ROI 像素/秒划分快慢档。幅度由连续 desired 和残余决定；
            // 因而不同游戏、视场或目标尺度不会在某个速度值处切换量化策略。
            const bool direct_motion_direction_supported =
                !config.enable_delay_compensation &&
                desired * relative_velocity > 0.0f;
            if ((observed_world_motion ||
                 direct_motion_direction_supported) &&
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
                diagnostics.integer_direction_blocked_x = true;
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
            diagnostics.command_sign_change_blocked_x = true;
        }
        if (previous_command_y != 0.0f && command.dy_counts != 0 &&
            std::signbit(previous_command_y) !=
                std::signbit(static_cast<float>(command.dy_counts))) {
            command.dy_counts = 0;
            quantized_y = 0.0f;
        }
        const float public_error_magnitude =
            std::hypot(public_error_x, public_error_y);
        const bool opposite_public_brake_x =
            use_coherent_prediction_projection_x &&
            command.dx_counts != 0 &&
            public_error_magnitude > hold_band &&
            command.dx_counts * public_error_x <= 0.0f;
        if (opposite_public_brake_x) {
            if (aim::detail::prediction_inventory_brake_allowed(
                    command.dx_counts,
                    prediction_opposite_public_brake_x_frames,
                    kPredictionOppositePublicBrakeMaximumFrames)) {
                ++prediction_opposite_public_brake_x_frames;
            } else {
                command.dx_counts = 0;
                quantized_x = 0.0f;
                prediction_opposite_public_brake_x_frames =
                    kPredictionOppositePublicBrakeMaximumFrames;
            }
        } else {
            prediction_opposite_public_brake_x_frames = 0;
        }
        residual_x = quantized_x - command.dx_counts;
        residual_y = quantized_y - command.dy_counts;
        diagnostics.quantization_zero_x =
            command.dx_counts == 0 && std::fabs(desired_x) > 0.001f;
        previous_command_x = static_cast<float>(command.dx_counts);
        previous_command_y = static_cast<float>(command.dy_counts);
        record_issued_command(
            frame, current_controller_at,
            previous_command_x, previous_command_y);
        return command.dx_counts != 0 || command.dy_counts != 0;
    }

    void reset_all() noexcept {
        tracks.clear();
        next_track_id = 1;
        last_sequence = 0;
        last_captured_at = {};
        last_control_at = {};
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
        prediction_candidate_low_motion_x_frames = 0;
        prediction_candidate_low_motion_y_frames = 0;
        prediction_external_motion_evidence_x = false;
        prediction_external_motion_evidence_y = false;
        prediction_offset_x = 0.0f;
        prediction_offset_y = 0.0f;
        prediction_control_offset_x = 0.0f;
        prediction_control_offset_y = 0.0f;
        prediction_pending_projection_x = 0.0f;
        prediction_opposite_public_brake_x_frames = 0;
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

const char* AimReverseTranslationResetReasonName(
        AimReverseTranslationResetReason reason) noexcept {
    switch (reason) {
        case AimReverseTranslationResetReason::NONE: return "NONE";
        case AimReverseTranslationResetReason::CANDIDATE_INACTIVE:
            return "CANDIDATE_INACTIVE";
        case AimReverseTranslationResetReason::BASE_NOT_ALIGNED:
            return "BASE_NOT_ALIGNED";
        case AimReverseTranslationResetReason::PARTIAL_SEMANTICS:
            return "PARTIAL_SEMANTICS";
        case AimReverseTranslationResetReason::PREVIOUS_DIRECTION_PENDING:
            return "PREVIOUS_DIRECTION_PENDING";
        case AimReverseTranslationResetReason::ZERO_TRANSLATION:
            return "ZERO_TRANSLATION";
        case AimReverseTranslationResetReason::OPPOSING_TRANSLATION:
            return "OPPOSING_TRANSLATION";
        case AimReverseTranslationResetReason::WEAK_BUDGET_EXHAUSTED:
            return "WEAK_BUDGET_EXHAUSTED";
        case AimReverseTranslationResetReason::WEAK_WITHOUT_STRONG_HISTORY:
            return "WEAK_WITHOUT_STRONG_HISTORY";
    }
    return "UNKNOWN";
}

Aim::Aim(const AimConfig& config) : impl_(std::make_unique<Impl>(config)) {
    Log::register_module("aim", LogLevel::INFO);
    LOG_INFO(
        "aim",
        "Aim 已初始化: config_valid={}, prediction={}, delay_compensation={}, person_classes={}, head_classes={}",
        impl_->valid_config(), config.enable_prediction,
        config.enable_delay_compensation, config.person_class_ids.size(),
        config.head_class_ids.size());
}

Aim::~Aim() = default;
Aim::Aim(Aim&&) noexcept = default;
Aim& Aim::operator=(Aim&&) noexcept = default;

AimResult Aim::process(const AimFrame& frame) noexcept {
    AimResult result;
    using clock = std::chrono::steady_clock;
    using milliseconds = std::chrono::duration<double, std::milli>;
    const auto invoked_at = clock::now();
    // 零值按公有契约取本次调用时刻；离线合成帧可能声明未来采集时刻，
    // 此时至少钳到 captured_at，保证控制时刻从不早于其观测。
    const auto control_at = frame.control_at == clock::time_point{}
        ? std::max(invoked_at, frame.captured_at) : frame.control_at;
    const char* invalid_reason = nullptr;
    if (!impl_) {
        invalid_reason = "Aim 实例无效";
    } else if (!impl_->valid_config()) {
        invalid_reason = "AimConfig 非法";
    } else if (frame.roi_width <= 0 || frame.roi_height <= 0) {
        invalid_reason = "ROI 尺寸非法";
    } else if (frame.sequence == 0) {
        invalid_reason = "sequence 为 0";
    } else if (frame.captured_at ==
               std::chrono::steady_clock::time_point{}) {
        invalid_reason = "captured_at 未设置";
    } else if (control_at < frame.captured_at) {
        invalid_reason = "control_at 早于 captured_at";
    } else if (!std::isfinite(frame.control_center_x) ||
               !std::isfinite(frame.control_center_y)) {
        invalid_reason = "控制中心不是有限值";
    } else if (!std::isfinite(frame.source_pixels_per_roi_pixel_x) ||
               !std::isfinite(frame.source_pixels_per_roi_pixel_y) ||
               frame.source_pixels_per_roi_pixel_x <= 0.0f ||
               frame.source_pixels_per_roi_pixel_y <= 0.0f) {
        invalid_reason = "ROI 到 source 的像素比例非法";
    } else if (!impl_->valid_frame_order(frame, control_at)) {
        invalid_reason = "帧序号或时间未严格递增";
    }
    if (invalid_reason) {
        result.status = AimStatus::INVALID_INPUT;
        if (impl_) {
            impl_->log_status_transition(
                result.status, frame.sequence, invalid_reason);
        }
        return result;
    }

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
            const auto projection =
                impl_->projected_aim_point(frame, *target, control_at);
            result.has_target = true;
            result.target.track_id = target->id;
            result.target.state = target->state;
            result.target.x1 = target->x1;
            result.target.y1 = target->y1;
            result.target.x2 = target->x2;
            result.target.y2 = target->y2;
            result.target.matched_observation_valid =
                target->matched_observation_valid;
            result.target.matched_observation_x1 =
                target->matched_observation_x1;
            result.target.matched_observation_y1 =
                target->matched_observation_y1;
            result.target.matched_observation_x2 =
                target->matched_observation_x2;
            result.target.matched_observation_y2 =
                target->matched_observation_y2;
            result.target.matched_observation_head_only =
                target->matched_observation_head_only;
            result.target.matched_observation_aim_from_head =
                target->matched_observation_aim_from_head;
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
            result.target.delay_compensation_ms_x = projection.delay_active
                ? projection.delay_seconds_x * 1000.0f : 0.0f;
            result.target.delay_compensation_ms_y = projection.delay_active
                ? projection.delay_seconds_y * 1000.0f : 0.0f;
            result.target.delay_compensation_ms = projection.delay_active
                ? std::max(projection.delay_seconds_x,
                           projection.delay_seconds_y) * 1000.0f
                : 0.0f;
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
                    projection.modelled_response_x_counts,
                    control_at,
                    result.control,
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
        impl_->commit_frame_order(frame, control_at);
        impl_->log_status_transition(
            result.status, frame.sequence, "处理成功");
        LOG_TRACE("aim", "seq={} tracks={} target={} command={} total={:.3f}ms",
                  frame.sequence, impl_->tracks.size(), result.has_target,
                  result.has_command, result.profile.total_ms);
        return result;
    } catch (const std::exception& error) {
        impl_->reset_all();
        result.status = AimStatus::TRACKING_FAILED;
        impl_->log_status_transition(
            result.status, frame.sequence, error.what());
        return result;
    } catch (...) {
        impl_->reset_all();
        result.status = AimStatus::TRACKING_FAILED;
        impl_->log_status_transition(
            result.status, frame.sequence, "未知异常");
        return result;
    }
}

bool Aim::record_backend_completed_command(
        std::uint64_t sequence,
        std::chrono::steady_clock::time_point backend_completed_at,
        int dx_counts,
        int dy_counts) noexcept {
    return impl_ && impl_->record_backend_completed_command(
        sequence, backend_completed_at, dx_counts, dy_counts);
}

void Aim::reset() noexcept {
    if (impl_) impl_->reset_all();
}
