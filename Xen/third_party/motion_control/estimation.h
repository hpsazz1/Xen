#pragma once
// ============================================================
//  Estimation Layer
//  多目标卡尔曼滤波 + 匈牙利匹配
//    - 单目标卡尔曼: 7 状态 [类别, cx, cy, w, h, vx, vy] 恒速模型
//    - 多目标管理:   IoU / 归一化距离 / 形状 加权代价 + 匈牙利最优分配
//    - 单目标追踪器 (SOT): 锁定/惯性滑行/重捕获, 内置 EMA 速度平滑
//  整理自: KalmanFilter.h / DetectionTypes.h
// ============================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "filters.h"
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <cstring>

namespace estimation {

// ---------------- 基础数据类型 ----------------
struct BoundingBox { float x = 0, y = 0, w = 0, h = 0; };

struct DetectedObject
{
    BoundingBox box;
    int    classId = 0;
    float  confidence = 0;
    int    trackId = -1;
    float  velocityX = 0, velocityY = 0;  // 卡尔曼估计速度(px/帧), 供前馈/外推
};

namespace detail {

// 测量向量 z = [类别, cx, cy, w, h]
inline void objectToMeasurement(const DetectedObject& obj, float z[5])
{
    z[0] = static_cast<float>(obj.classId);
    z[1] = obj.box.x + obj.box.w * 0.5f;
    z[2] = obj.box.y + obj.box.h * 0.5f;
    z[3] = obj.box.w;
    z[4] = obj.box.h;
}

inline void measurementToBox(const float z[5],
                             float& x0, float& y0, float& x1, float& y1)
{
    x0 = z[1] - z[3] * 0.5f;
    y0 = z[2] - z[4] * 0.5f;
    x1 = z[1] + z[3] * 0.5f;
    y1 = z[2] + z[4] * 0.5f;
}

inline float measurementIoU(const float a[5], const float b[5])
{
    float ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    measurementToBox(a, ax0, ay0, ax1, ay1);
    measurementToBox(b, bx0, by0, bx1, by1);
    const float iw = std::max(std::min(ax1, bx1) - std::max(ax0, bx0) + 1.f, 0.f);
    const float ih = std::max(std::min(ay1, by1) - std::max(ay0, by0) + 1.f, 0.f);
    const float inter = iw * ih;
    if (inter <= 0.f) return 0.f;
    const float ua = (ax1 - ax0) * (ay1 - ay0)
                   + (bx1 - bx0) * (by1 - by0) - inter;
    return ua > 0.f ? inter / ua : 0.f;
}

// 5x5 解析求逆 (高斯-若尔当)
inline bool invert5x5(const float S[5][5], float Sinv[5][5])
{
    double m[5][10];
    for (int i = 0; i < 5; ++i)
    {
        for (int j = 0; j < 5; ++j) m[i][j] = S[i][j];
        for (int j = 5; j < 10; ++j) m[i][j] = (i == (j - 5)) ? 1.0 : 0.0;
    }
    for (int col = 0; col < 5; ++col)
    {
        int pivot = col;
        double best = std::fabs(m[pivot][col]);
        for (int r = col + 1; r < 5; ++r)
        {
            const double v = std::fabs(m[r][col]);
            if (v > best) { best = v; pivot = r; }
        }
        if (best < 1e-12) return false;
        if (pivot != col)
            for (int c = 0; c < 10; ++c) std::swap(m[pivot][c], m[col][c]);
        const double div = m[col][col];
        for (int c = 0; c < 10; ++c) m[col][c] /= div;
        for (int r = 0; r < 5; ++r) if (r != col)
        {
            const double f = m[r][col];
            for (int c = 0; c < 10; ++c) m[r][c] -= f * m[col][c];
        }
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j)
            Sinv[i][j] = static_cast<float>(m[i][j + 5]);
    return true;
}

// 匈牙利算法 (Jonker-Volgenant, O(n^3)), 返回 行->列 分配 (-1 = 未匹配)
// cost 为展平的一维向量, 索引为 i * m + j
inline std::vector<int> hungarianAssignment(
    const std::vector<float>& cost, size_t n, size_t m)
{
    const int nn = static_cast<int>(n);
    const int mm = static_cast<int>(m);
    const int dim = std::max(nn, mm);
    constexpr double INF = 1e18;
    // MAXD=32 硬限制。超出时降级为贪心最近分配而非返回全部未匹配。
    constexpr int MAXD = 32;
    if (dim > MAXD)
    {
        // 超出 32 个 track+detection 时降级：对每个 track 分配最近检测
        std::vector<int> greedy(nn, -1);
        std::vector<bool> used(mm, false);
        for (int i = 0; i < nn; ++i)
        {
            float bestCost = 1.0f;
            int bestJ = -1;
            for (int j = 0; j < mm; ++j)
            {
                if (!used[j] && cost[i * mm + j] < bestCost)
                {
                    bestCost = cost[i * mm + j];
                    bestJ = j;
                }
            }
            if (bestJ >= 0)
            {
                greedy[i] = bestJ;
                used[bestJ] = true;
            }
        }
        return greedy;
    }

    double a[MAXD + 1][MAXD + 1] = {};
    for (int i = 1; i <= nn; ++i)
    {
        for (int j = 1; j <= mm; ++j) a[i][j] = cost[(i - 1) * mm + (j - 1)];
        for (int j = mm + 1; j <= dim; ++j) a[i][j] = 1.0;
    }

    double u[MAXD + 1] = {}, v[MAXD + 1] = {};
    int p[MAXD + 1] = {}, way[MAXD + 1] = {};
    for (int i = 1; i <= dim; ++i)
    {
        p[0] = i;
        int j0 = 0;
        double minv[MAXD + 1];
        char used[MAXD + 1] = {};
        for (int j = 0; j <= dim; ++j) minv[j] = INF;
        do
        {
            used[j0] = true;
            const int i0 = p[j0];
            int j1 = 0;
            double delta = INF;
            for (int j = 1; j <= dim; ++j)
            {
                if (used[j]) continue;
                const double cur = a[i0][j] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j = 0; j <= dim; ++j)
            {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);

        do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0 != 0);
    }

    std::vector<int> assignment(n, -1);
    for (int j = 1; j <= mm; ++j)
        if (p[j] >= 1 && p[j] <= n) assignment[p[j] - 1] = j - 1;
    return assignment;
}

} // namespace detail


// ============================================================
//  单目标卡尔曼滤波器 (恒速模型)
//    状态 X = [类别, cx, cy, w, h, vx, vy]  (7)
//    测量 z = [类别, cx, cy, w, h]          (5)
// ============================================================
class SingleTargetKalman
{
public:
    SingleTargetKalman(int terminationFrames, int confirmThreshold,
                       float noiseVx, float noiseVy,
                       float noiseW, float noiseH, float measurementStdDev)
        : terminationCount_(terminationFrames),
          terminationInit_(terminationFrames),
          confirmThreshold_(confirmThreshold),
          consecutiveHits_(1),
          id_(nextId_++),
          noiseVx_(noiseVx), noiseVy_(noiseVy),
          noiseW_(noiseW), noiseH_(noiseH),
          measurementVariance_(measurementStdDev * measurementStdDev)
    {
        for (int i = 0; i < 7; ++i)
        {
            posterior_[i] = 0;
            prior_[i] = 0;
            posteriorCov_[i][i] = 1.f;
        }
    }

    static void resetId(int v = 0) { nextId_ = v; }
    void   setConfidence(float p) { confidence_ = p; }
    float  confidence() const { return confidence_; }
    bool   isConfirmed() const { return consecutiveHits_ >= confirmThreshold_; }
    int    id()   const { return id_; }
    bool   hasMeasurement() const { return hasMeasurement_; }
    const float* priorState5() const { return prior_; }
    const float* latestState5() const { return latest_; }
    float  velocityX() const { return posterior_[5]; }
    float  velocityY() const { return posterior_[6]; }

    void initializeWithMeasurement(const float z[5])
    {
        for (int i = 0; i < 5; ++i) posterior_[i] = z[i];
        posterior_[5] = 0;
        posterior_[6] = 0;
    }

    // 先验: X' = A·X,  P' = A·P·Aᵀ + Q;  A 把 cx+=vx·dt, cy+=vy·dt
    void predict(float dt = 1.f)
    {
        float A[7][7] = {};
        for (int i = 0; i < 7; ++i) A[i][i] = 1.f;
        A[1][5] = dt;
        A[2][6] = dt;

        for (int i = 0; i < 7; ++i)
        {
            float s = 0;
            for (int j = 0; j < 7; ++j) s += A[i][j] * posterior_[j];
            prior_[i] = s;
        }

        float AP[7][7];
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
            {
                float s = 0;
                for (int k = 0; k < 7; ++k) s += A[i][k] * posteriorCov_[k][j];
                AP[i][j] = s;
            }

        // NOTE: velocity process noise kept unscaled by dt to preserve existing
        // parameter tuning. For dt≈1/N_fps, consider scaling noiseVx_/noiseVy_ values
        // proportionally (e.g. multiply by 60/fps) if prediction drifts at low FPS.
        const float q[7] = { 0, 0, 0, noiseW_, noiseH_, noiseVx_, noiseVy_ };
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
            {
                float s = 0;
                for (int k = 0; k < 7; ++k) s += AP[i][k] * A[j][k];
                priorCov_[i][j] = s + ((i == j) ? q[i] : 0.f);
            }
    }

    // 用真实测量更新; 返回 false 表示轨迹应终止
    bool update(const float z[5])
    {
        consecutiveHits_++;
        float y[5];
        for (int i = 0; i < 5; ++i) y[i] = z[i] - prior_[i];

        float S[5][5];
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                // classId (i=0) 是分类变量，跳过测量噪声叠加
                S[i][j] = priorCov_[i][j] + ((i == j && i > 0) ? measurementVariance_ : 0.f);

        float Sinv[5][5];
        if (!detail::invert5x5(S, Sinv)) return updateMissed();

        float K[7][5];
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
            {
                double acc = 0;
                for (int k = 0; k < 5; ++k)
                    acc += static_cast<double>(priorCov_[r][k]) * Sinv[k][c];
                K[r][c] = static_cast<float>(acc);
            }

        for (int r = 0; r < 7; ++r)
        {
            double delta = 0;
            for (int c = 0; c < 5; ++c)
                delta += static_cast<double>(K[r][c]) * y[c];
            posterior_[r] = prior_[r] + static_cast<float>(delta);
        }

        float HP[5][7];
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 7; ++c)
                HP[r][c] = priorCov_[r][c];

        float KHP[7][7];
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
            {
                double acc = 0;
                for (int k = 0; k < 5; ++k)
                    acc += static_cast<double>(K[i][k]) * HP[k][j];
                KHP[i][j] = static_cast<float>(acc);
            }

        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
                posteriorCov_[i][j] = priorCov_[i][j] - KHP[i][j];

        for (int i = 0; i < 5; ++i) latest_[i] = posterior_[i];
        latest_[5] = static_cast<float>(id_);
        hasMeasurement_ = true;
        terminationCount_ = terminationInit_;
        return true;
    }

    // 无匹配测量: 仅靠先验惯性推演; 超过终止帧数则返回 false
    bool updateMissed()
    {
        if (terminationCount_ == 1) return false;
        --terminationCount_;
        for (int i = 0; i < 7; ++i) posterior_[i] = prior_[i];
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j)
                posteriorCov_[i][j] = priorCov_[i][j];
        for (int k = 0; k < 5; ++k) latest_[k] = posterior_[k];
        latest_[5] = static_cast<float>(id_);
        hasMeasurement_ = true;
        return true;
    }

private:
    int terminationCount_, terminationInit_, confirmThreshold_,
        consecutiveHits_, id_;
    bool hasMeasurement_ = false;
    float posterior_[7], prior_[7];
    float posteriorCov_[7][7], priorCov_[7][7];
    float latest_[6];
    float confidence_ = 0.f;
    float noiseVx_, noiseVy_, noiseW_, noiseH_, measurementVariance_;
    inline static int nextId_ = 0;
};


// ============================================================
//  多目标追踪器
//    每帧: 先验 → 代价矩阵 → 匈牙利分配 → 更新/惯性/新建/删除
// ============================================================
class MultiTargetTracker
{
public:
    void initialize(int confirm = 2, int termination = 5,
                    float noiseVx = 1.f, float noiseVy = 1.f,
                    float noiseW = 0.01f, float noiseH = 0.01f,
                    float measurementStdDev = 5.f)
    {
        confirmThreshold_ = confirm;
        terminationFrames_ = termination;
        measurementStdDev_ = measurementStdDev;
        noiseVx_ = noiseVx;
        noiseVy_ = noiseVy;
        noiseW_ = noiseW;
        noiseH_ = noiseH;
    }

    void reset(bool resetIdFlag = true)
    {
        tracks_.clear();
        toDelete_.clear();
        if (resetIdFlag) SingleTargetKalman::resetId(0);
    }

    // 输入本帧检测, 输出已确认的稳定轨迹 (带持久轨迹号)
    // dt = 本帧与上一帧之间的时间间隔（秒），用于正确缩放卡尔曼状态转移
    void predictAndUpdate(const std::vector<DetectedObject>& detections,
                          std::vector<DetectedObject>& output,
                          float dt = 1.f)
    {
        // 1. 所有轨迹先验预测
        for (auto& t : tracks_) t->predict(dt);

        // 2. 收集测量
        measurements_.clear();
        confidences_.clear();
        for (const auto& d : detections)
        {
            float z[5];
            detail::objectToMeasurement(d, z);
            measurements_.push_back({ z[0], z[1], z[2], z[3], z[4] });
            confidences_.push_back(d.confidence);
        }

        // 3. 收集轨迹先验状态
        states_.clear();
        for (const auto& t : tracks_)
        {
            const float* s = t->priorState5();
            states_.push_back({ s[0], s[1], s[2], s[3], s[4] });
        }

        // 4. 代价矩阵 = 0.1·(1−IoU) + 0.7·归一化距离 + 0.2·形状差
        const size_t n = states_.size(), m = measurements_.size();
        costMatrix_.assign(n * m, 1.0f);
        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = 0; j < m; ++j)
            {
                const float iou = detail::measurementIoU(
                    states_[i].data(), measurements_[j].data());
                const float c_iou = 1.f - iou;
                const float dx = states_[i][1] - measurements_[j][1];
                const float dy = states_[i][2] - measurements_[j][2];
                const float wa = (states_[i][3] + measurements_[j][3]) * 0.5f;
                const float ha = (states_[i][4] + measurements_[j][4]) * 0.5f;
                const float c_dist = std::min(
                    (std::fabs(dx) / (wa * 3.f) + std::fabs(dy) / (ha * 3.f)) * 0.5f, 1.f);
                const float ap = states_[i][3] * states_[i][4];
                const float am = measurements_[j][3] * measurements_[j][4];
                const float c_shape = std::fabs(ap - am) / std::max(ap, am);
                const float c_confidence = 1.f - std::clamp(confidences_[j], 0.f, 1.f);
                costMatrix_[i * m + j] =
                    0.10f * c_iou +
                    0.60f * c_dist +
                    0.15f * c_shape +
                    0.15f * c_confidence;
            }
        }

        // 5. 匈牙利分配 + 更新
        assignment_ = detail::hungarianAssignment(costMatrix_, n, m);
        measurementUsed_.assign(measurements_.size(), false);
        toDelete_.clear();
        for (size_t i = 0; i < tracks_.size(); ++i)
        {
            const int j = (i < assignment_.size()) ? assignment_[i] : -1;
            const bool matched = (j >= 0 && j < static_cast<int>(measurements_.size()));
            if (matched && costMatrix_[i * m + j] < 1.f)
            {
                float z[5];
                for (int k = 0; k < 5; ++k) z[k] = measurements_[j][k];
                tracks_[i]->setConfidence(confidences_[j]);
                if (!tracks_[i]->update(z))
                    toDelete_.push_back(static_cast<int>(i));
                measurementUsed_[j] = true;
                continue;
            }
            if (!tracks_[i]->updateMissed())
                toDelete_.push_back(static_cast<int>(i));
        }

        // 6. 删除终止轨迹
        if (!toDelete_.empty())
        {
            keep_.clear();
            keep_.reserve(tracks_.size());
            for (size_t i = 0; i < tracks_.size(); ++i)
                if (std::find(toDelete_.begin(), toDelete_.end(),
                              static_cast<int>(i)) == toDelete_.end())
                    keep_.emplace_back(std::move(tracks_[i]));
            tracks_.swap(keep_);
            toDelete_.clear();
        }

        // 7. 未匹配测量 → 新建轨迹
        for (size_t j = 0; j < measurementUsed_.size(); ++j)
        {
            if (!measurementUsed_[j])
            {
                auto t = std::make_unique<SingleTargetKalman>(
                    terminationFrames_, confirmThreshold_,
                    noiseVx_, noiseVy_, noiseW_, noiseH_, measurementStdDev_);
                float z[5];
                for (int k = 0; k < 5; ++k) z[k] = measurements_[j][k];
                t->initializeWithMeasurement(z);
                t->setConfidence(confidences_[j]);
                tracks_.emplace_back(std::move(t));
            }
        }

        // 8. 输出已确认轨迹
        output.clear();
        for (const auto& t : tracks_)
        {
            if (!t->hasMeasurement() || !t->isConfirmed()) continue;
            const float* z = t->latestState5();
            float x0, y0, x1, y1;
            detail::measurementToBox(z, x0, y0, x1, y1);
            DetectedObject obj;
            obj.classId = static_cast<int>(z[0]);
            obj.box.x = x0;
            obj.box.y = y0;
            obj.box.w = x1 - x0;
            obj.box.h = y1 - y0;
            obj.confidence = t->confidence();
            obj.trackId = static_cast<int>(z[5]);
            obj.velocityX = t->velocityX();
            obj.velocityY = t->velocityY();
            output.push_back(obj);
        }
    }

private:
    int confirmThreshold_, terminationFrames_;
    float noiseVx_, noiseVy_, noiseW_, noiseH_, measurementStdDev_;
    std::vector<std::unique_ptr<SingleTargetKalman>> tracks_;
    std::vector<std::unique_ptr<SingleTargetKalman>> keep_;
    std::vector<int> toDelete_;
    std::vector<std::array<float, 5>> measurements_, states_;
    std::vector<float> confidences_;
    std::vector<float> costMatrix_;  // 展平代价矩阵 (n*m)
    std::vector<int> assignment_;
    std::vector<char> measurementUsed_;
};


// ============================================================
//  单目标追踪器 (Single Object Tracker, SOT)
//    在多候选中"选定一个并死死咬住", 提供锁定/惯性滑行/重捕获.
//    与 MultiTargetTracker (MOT) 的区别:
//      MOT: 维护 N 条并行轨迹, 输出全部稳定目标.
//      SOT: 只关心"我正在瞄的那一个", 候选丢失时靠速度滑行不掉锁,
//           目标再现时验证同一性后无缝续接.
//
//  典型管线: MOT 输出 → SOT 选定 → 延迟补偿 → 执行层
//
//  状态机:
//    未锁定 ──选定──► 已锁定(有测量) ──候选缺失──► 滑行中(惯性推演)
//                                            ▲                 │
//                                            └──重捕获通过──────┤
//                                                              │
//                                              超过滑行帧数上限 └──► 未锁定
// ============================================================
class SingleTargetTracker
{
public:
    // 选择策略: 在多个候选中如何挑"那一个"
    enum class SelectionStrategy { HighestConfidence, NearestDistance, SpecifiedTrackId };

    // 配置
    void initialize(SelectionStrategy strategy = SelectionStrategy::HighestConfidence,
                    int    coastFramesLimit = 15,
                    int    specifiedTrackId   = -1,
                    float  recaptureIoUThreshold = 0.3f,
                    float  recaptureDistanceMultiplier = 2.5f,
                    float  coastVelocityDecay = 1.0f)
    {
        strategy_ = strategy;
        coastLimit_ = coastFramesLimit;
        specifiedId_ = specifiedTrackId;
        recaptureIoU_ = recaptureIoUThreshold;
        recaptureMultiplier_ = recaptureDistanceMultiplier;
        coastDecay_ = coastVelocityDecay;
    }

    // 锁定指定轨迹号(强制切换目标), 下一帧更新立即生效
    void lockTrackId(int id)
    {
        if (lockedId_ != id) { state_ = kUnlocked; lockedId_ = id; }
        strategy_ = SelectionStrategy::SpecifiedTrackId;
        specifiedId_ = id;
    }

    // 注入外部速度(如卡尔曼估计, px/帧): 优先级高于内部差分估速.
    // 在调用 update() 前设置; 设为 (0,0) 且启用=false 可回到差分模式.
    void setExternalVelocity(float vx, float vy, bool enable = true)
    {
        externalVx_ = vx;
        externalVy_ = vy;
        useExternalVelocity_ = enable;
    }

    bool isLocked()     const { return state_ == kLocked; }
    bool isCoasting()   const { return state_ == kCoasting; }
    bool hasTarget()    const { return state_ != kUnlocked; }
    int  trackId()      const { return lockedId_; }
    int  coastCount()   const { return coastFrames_; }

    // 当前锁定目标的位置/速度/尺寸 (无目标时返回 false)
    bool getState(float& cx, float& cy, float& w, float& h,
                  float& vx, float& vy) const
    {
        if (state_ == kUnlocked) return false;
        cx = centerX_;
        cy = centerY_;
        w = width_;
        h = height_;
        vx = velocityX_;
        vy = velocityY_;
        return true;
    }

    float confidence() const { return confidence_; }
    int   classId()    const { return classId_; }

    void reset()
    {
        state_ = kUnlocked;
        lockedId_ = -1;
        coastFrames_ = 0;
        centerX_ = centerY_ = width_ = height_ = 0;
        velocityX_ = velocityY_ = 0;
        velocityFilterX_.reset();
        velocityFilterY_.reset();
    }

    // 设置 NearestDistance 策略的参考中心（仅在解锁状态下调用），
    // 确保初始目标获取时选择离准星最近的目标而非 (0,0)
    void setReferenceCenter(float cx, float cy)
    {
        if (state_ == kUnlocked)
        {
            centerX_ = cx;
            centerY_ = cy;
        }
    }

    // 输入本帧候选(通常是 MOT 的稳定输出), 内部完成 选定/锁定/滑行/重捕获.
    // 返回当前是否有有效目标.
    bool update(const std::vector<DetectedObject>& candidates)
    {
        const DetectedObject* hit = nullptr;

        if (state_ == kUnlocked)
        {
            // ---- 未锁定: 从候选里选定一个 ----
            hit = selectTarget(candidates);
            if (hit) bindTarget(*hit);
        }
        else
        {
            // ---- 已锁定或滑行中: 找同一目标的再现测量 ----
            hit = findRecapture(candidates);
            if (hit)
            {
                refreshLock(*hit);
            }
            else
            {
                doCoast();
            }
        }
        return state_ != kUnlocked;
    }

private:
    // ---- 状态枚举 ----
    static constexpr int kUnlocked  = 0;
    static constexpr int kLocked    = 1;
    static constexpr int kCoasting  = 2;

    // ---- 选择 ----
    const DetectedObject* selectTarget(const std::vector<DetectedObject>& candidates) const
    {
        const DetectedObject* best = nullptr;
        if (strategy_ == SelectionStrategy::SpecifiedTrackId)
        {
            for (const auto& c : candidates)
                if (c.trackId == specifiedId_) { best = &c; break; }
            return best;
        }

        if (strategy_ == SelectionStrategy::NearestDistance)
        {
            float bestScore = 1e30f;
            for (const auto& c : candidates)
            {
                const float cx = c.box.x + c.box.w * 0.5f;
                const float cy = c.box.y + c.box.h * 0.5f;
                const float dx = cx - centerX_;
                const float dy = cy - centerY_;
                const float d = dx * dx + dy * dy;
                if (d < bestScore) { bestScore = d; best = &c; }
            }
            return best;
        }

        // 最高置信度
        float bestP = -1.f;
        for (const auto& c : candidates)
            if (c.confidence > bestP) { bestP = c.confidence; best = &c; }
        return best;
    }

    // ---- 重捕获: 在候选里找与锁定目标同一性的那个 ----
    const DetectedObject* findRecapture(const std::vector<DetectedObject>& candidates) const
    {
        const DetectedObject* best = nullptr;
        float bestScore = -1e30f;
        const float refW = width_ > 0 ? width_ : 1.f;
        const float refH = height_ > 0 ? height_ : 1.f;
        const float radiusMultiplier = recaptureMultiplier_
            * (1.f + 0.5f * static_cast<float>(coastFrames_));
        const float radiusX = refW * radiusMultiplier;
        const float radiusY = refH * radiusMultiplier;

        for (const auto& c : candidates)
        {
            if (strategy_ == SelectionStrategy::SpecifiedTrackId
                && c.trackId != lockedId_) continue;
            if (classId_ >= 0 && c.classId >= 0 && c.classId != classId_) continue;

            const float cx = c.box.x + c.box.w * 0.5f;
            const float cy = c.box.y + c.box.h * 0.5f;
            const float dx = std::fabs(cx - centerX_);
            const float dy = std::fabs(cy - centerY_);
            if (dx > radiusX || dy > radiusY) continue;

            float a[5], b[5];
            a[0] = static_cast<float>(classId_);
            a[1] = centerX_;
            a[2] = centerY_;
            a[3] = width_;
            a[4] = height_;
            b[0] = static_cast<float>(c.classId);
            b[1] = cx;
            b[2] = cy;
            b[3] = c.box.w;
            b[4] = c.box.h;
            const float iou = detail::measurementIoU(a, b);

            const float distanceScore =
                1.f - 0.5f * (dx / radiusX + dy / radiusY);
            const float score =
                0.5f * iou + 0.4f * distanceScore + 0.1f * c.confidence;
            if (score > bestScore) { bestScore = score; best = &c; }
        }

        if (bestScore < 0.05f) return nullptr;
        return best;
    }

    // ---- 绑定新目标(初次锁定) ----
    void bindTarget(const DetectedObject& obj)
    {
        state_ = kLocked;
        lockedId_ = obj.trackId;
        classId_ = obj.classId;
        confidence_ = obj.confidence;
        centerX_ = obj.box.x + obj.box.w * 0.5f;
        centerY_ = obj.box.y + obj.box.h * 0.5f;
        width_ = obj.box.w;
        height_ = obj.box.h;
        velocityX_ = 0;
        velocityY_ = 0;
        coastFrames_ = 0;
    }

    // ---- 用新测量刷新锁定状态(差分估速, 外部速度优先) ----
    void refreshLock(const DetectedObject& obj)
    {
        const float newCx = obj.box.x + obj.box.w * 0.5f;
        const float newCy = obj.box.y + obj.box.h * 0.5f;
        if (useExternalVelocity_)
        {
            velocityX_ = externalVx_;
            velocityY_ = externalVy_;
            useExternalVelocity_ = false;
        }
        else
        {
            const float newVx = newCx - centerX_;
            const float newVy = newCy - centerY_;
            velocityX_ = static_cast<float>(velocityFilterX_.update(newVx));
            velocityY_ = static_cast<float>(velocityFilterY_.update(newVy));
        }
        centerX_ = newCx;
        centerY_ = newCy;
        width_ = obj.box.w;
        height_ = obj.box.h;
        classId_ = obj.classId;
        confidence_ = obj.confidence;
        state_ = kLocked;
        coastFrames_ = 0;
    }

    // ---- 惯性滑行: 用速度推演位置, 超时则掉锁 ----
    void doCoast()
    {
        if (coastFrames_ >= coastLimit_)
        {
            state_ = kUnlocked;
            return;
        }
        centerX_ += velocityX_;
        centerY_ += velocityY_;
        velocityX_ *= coastDecay_;
        velocityY_ *= coastDecay_;
        ++coastFrames_;
        state_ = kCoasting;
    }

    // ---- 配置 ----
    SelectionStrategy strategy_ = SelectionStrategy::HighestConfidence;
    int       coastLimit_   = 15;
    int       specifiedId_  = -1;
    float     recaptureIoU_ = 0.3f;
    float     recaptureMultiplier_ = 2.5f;
    float     coastDecay_   = 1.0f;

    // ---- 运行时状态 ----
    int     state_       = kUnlocked;
    int     lockedId_    = -1;
    int     coastFrames_ = 0;
    int     classId_     = -1;
    float   confidence_  = 0;
    float   centerX_ = 0, centerY_ = 0, width_ = 0, height_ = 0;
    float   velocityX_ = 0, velocityY_ = 0;
    // 速度 EMA 平滑(抑制差分估速抖动, α=0.4 → 中度平滑)
    filters::ExponentialMovingAverage velocityFilterX_{0.4f},
                                      velocityFilterY_{0.4f};
    // 外部速度注入(卡尔曼等)
    float externalVx_ = 0, externalVy_ = 0;
    bool  useExternalVelocity_ = false;
};

} // namespace estimation
