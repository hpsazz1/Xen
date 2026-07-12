#pragma once
// ============================================================
//  Digital Filters
//  各层共用的基础数字滤波:
//    - 指数移动平均 (EMA / 一阶 IIR 低通)
//    - 双指数移动平均 (无相位滞后, 适合离线/缓冲平滑)
//    - 限幅低通 (低通 + 输出变化率限幅, 抗冲击)
//    - 滑动中值 (小窗中值, 抗离群点)
//  设计要点:
//    - 所有滤波器都是 header-only、无外部依赖
//    - 单通道 (标量), 如需多通道各开一份即可
//    - α 越大越"灵敏"(跟随快), 越小越"平滑"(滤得狠)
// ============================================================
#include <vector>
#include <algorithm>
#include <cmath>

namespace filters {

// ============================================================
//  Exponential Moving Average (EMA / 一阶 IIR 低通)
//    y[n] = α·x[n] + (1-α)·y[n-1]
//    两种 α 给法:
//      (1) 直接给 α            : setCoefficient(α)
//      (2) 给截止频率 fc + dt   : setCutoff(fc, dt)
//          α = 1 − exp(−2π·fc·dt)   (时间离散化一阶 RC 低通)
// ============================================================
class ExponentialMovingAverage
{
public:
    ExponentialMovingAverage() = default;
    explicit ExponentialMovingAverage(double alpha, double initialValue = 0.0)
    {
        setCoefficient(alpha, initialValue);
    }

    // α ∈ [0,1]; 0 = 完全保持(不更新), 1 = 不滤波(直通)
    // 注意: 此方法会重置滤波器状态 (initialized_=false)，
    // 运行时动态调参请使用 setAlpha() 以避免丢失历史值
    void setCoefficient(double alpha, double initialValue = 0.0)
    {
        alpha_ = std::clamp(alpha, 0.0, 1.0);
        initialized_ = false;
        initialValue_ = initialValue;
    }

    // 仅修改 α 不重置状态 — 用于每帧动态调参 (如时间校正 EMA)
    void setAlpha(double alpha)
    {
        alpha_ = std::clamp(alpha, 0.0, 1.0);
    }

    // 由截止频率 fc(Hz) 与采样间隔 dt(s) 自动算 α
    void setCutoff(double cutoffFreq, double dt, double initialValue = 0.0)
    {
        const double a = 1.0 - std::exp(-6.28318530717958647692 * cutoffFreq * dt);
        setCoefficient(a, initialValue);
    }

    // 输入新样本, 返回滤波后值
    double update(double sample)
    {
        // NaN/Inf 防护：异常值不回传，保持当前状态不变
        if (!std::isfinite(sample))
            return state_;

        if (!initialized_)
        {
            state_ = sample;
            initialized_ = true;
            return state_;
        }
        state_ = alpha_ * sample + (1.0 - alpha_) * state_;
        return state_;
    }

    double currentValue() const { return state_; }
    void   reset(double initialValue = 0.0)
    {
        state_ = initialValue;
        initialized_ = false;
    }
    double coefficient() const { return alpha_; }

    // 自适应系数: 误差大 → 临时升高 α (快速追踪), 误差小 → α = 基础值
    void adaptiveCoefficient(double baseAlpha, double currentError, double errorThreshold)
    {
        const double ratio = std::clamp(std::fabs(currentError) / errorThreshold, 0.0, 1.0);
        alpha_ = baseAlpha + (1.0 - baseAlpha) * ratio;
    }

    // 重置自适应状态(恢复基础 α)
    void restoreBaseCoefficient(double baseAlpha) { alpha_ = baseAlpha; }

private:
    double alpha_      = 1.0;
    double state_      = 0.0;
    double initialValue_ = 0.0;
    bool   initialized_ = false;
};


// ============================================================
//  Double Exponential Moving Average (Double EMA / Holt-Linear)
//    前向 EMA + 反向 EMA 取平均 → 零相位滞后, 平滑曲线不右移.
//    需要缓冲一段历史样本后整段处理, 适合离线/低频(非逐帧实时).
// ============================================================
class DoubleExponentialMovingAverage
{
public:
    explicit DoubleExponentialMovingAverage(double alpha = 0.3)
        : alpha_(std::clamp(alpha, 0.0, 1.0))
    {}

    // 对整段输入做零相位平滑, 输出到 result (长度与 input 相同)
    void smooth(const std::vector<double>& input, std::vector<double>& result)
    {
        const int n = static_cast<int>(input.size());
        result.assign(n, 0.0);
        if (n == 0) return;
        if (n == 1) { result[0] = input[0]; return; }

        // 前向 EMA
        std::vector<double> forward(n);
        forward[0] = input[0];
        for (int i = 1; i < n; ++i)
            forward[i] = alpha_ * input[i] + (1.0 - alpha_) * forward[i - 1];

        // 反向 EMA
        std::vector<double> backward(n);
        backward[n - 1] = input[n - 1];
        for (int i = n - 2; i >= 0; --i)
            backward[i] = alpha_ * input[i] + (1.0 - alpha_) * backward[i + 1];

        // 平均 → 零相位
        for (int i = 0; i < n; ++i)
            result[i] = 0.5 * (forward[i] + backward[i]);
    }

    void setCoefficient(double alpha) { alpha_ = std::clamp(alpha, 0.0, 1.0); }
    double coefficient() const { return alpha_; }

private:
    double alpha_;
};


// ============================================================
//  Rate Limited Low-Pass (低通 + 一阶差分限幅)
//    先 EMA 平滑, 再限制"本帧输出相对上一帧输出的最大变化量".
//    作用: 既滤高频噪声, 又防止单帧尖刺(如检测跳变)造成准星猛甩.
// ============================================================
class RateLimitedLowPass
{
public:
    RateLimitedLowPass() = default;
    RateLimitedLowPass(double alpha, double maxSingleFrameChange)
        : lowPass_(alpha), limit_(maxSingleFrameChange)
    {}

    void set(double alpha, double maxSingleFrameChange)
    {
        lowPass_.setCoefficient(alpha);
        limit_ = maxSingleFrameChange;
    }

    double update(double sample)
    {
        const double smoothed = lowPass_.update(sample);
        if (!initialized_)
        {
            lastOutput_ = smoothed;
            initialized_ = true;
            return smoothed;
        }
        double diff = smoothed - lastOutput_;
        diff = std::clamp(diff, -limit_, limit_);
        lastOutput_ += diff;
        return lastOutput_;
    }

    double currentValue() const { return lastOutput_; }
    void   reset()
    {
        lowPass_.reset();
        lastOutput_ = 0.0;
        initialized_ = false;
    }

private:
    ExponentialMovingAverage lowPass_;
    double limit_      = 1e9;
    double lastOutput_ = 0.0;
    bool   initialized_ = false;
};


// ============================================================
//  Sliding Median Filter (小窗中值)
//    对离群点(单帧检测飞点)鲁棒, 但有 (窗长/2) 的群延迟.
//    窗长建议奇数; 仅对缓变量(如目标宽高/置信度)使用, 位置类慎用.
// ============================================================
class SlidingMedian
{
public:
    explicit SlidingMedian(int windowSize = 5) { setWindowSize(windowSize); }

    void setWindowSize(int windowSize)
    {
        windowSize_ = std::max(1, windowSize);
        buffer_.clear();
        buffer_.reserve(windowSize_);
    }

    double update(double sample)
    {
        buffer_.push_back(sample);
        if (static_cast<int>(buffer_.size()) > windowSize_)
            buffer_.erase(buffer_.begin());
        std::vector<double> sorted = buffer_;
        std::sort(sorted.begin(), sorted.end());
        const int n = static_cast<int>(sorted.size());
        return (n % 2) ? sorted[n / 2]
                       : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
    }

    void reset() { buffer_.clear(); }

private:
    int windowSize_;
    std::vector<double> buffer_;
};


// ============================================================
//  Motion Change Detector (运动突变检测器)
//    监视目标速度流, 检测急停 / 急加速 / 反向折返.
//    典型用法: 每帧喂入最新速度, 查询突变类型 → 控制器据此调整策略.
// ============================================================
class MotionChangeDetector
{
public:
    enum class ChangeType { None = 0, SuddenStop, SuddenAccel, Reversal };

    MotionChangeDetector() = default;

    // 急停: |v| < 阈值倍率×|v_smooth|. 加速: |dv| > dv阈值.
    void set(double stopThresholdRatio = 0.25,
             double accelThreshold    = 5.0,
             int    confirmFrames     = 2,
             double internalEmaAlpha  = 0.6)
    {
        stopRatio_ = stopThresholdRatio;
        accelThreshold_ = accelThreshold;
        confirmFrames_ = confirmFrames;
        emaAlpha_ = internalEmaAlpha;
        reset();
    }

    // 每帧输入当前速度 (px/帧). 返回当前突变类型.
    ChangeType detect(double currentVelocity)
    {
        // 先更新 EMA 再分类，避免 1 帧滞后
        velocityEma_.update(currentVelocity);
        const ChangeType current = classify(currentVelocity);

        // 折返是瞬间事件(符号翻转只在1帧出现), 检测到立即触发并锁存3帧
        if (current == ChangeType::Reversal)
        {
            confirmed_ = ChangeType::Reversal;
            lockout_ = 3;
        }
        else if (lockout_ > 0)
        {
            --lockout_;
            return confirmed_;
        }

        // 急停/急加速需要连续确认
        if (current == lastCandidate_ && current != ChangeType::None)
            count_++;
        else
        {
            lastCandidate_ = current;
            count_ = (current != ChangeType::None) ? 1 : 0;
        }
        confirmed_ = (count_ >= confirmFrames_) ? current : ChangeType::None;

        lastVelocity_ = currentVelocity;
        return confirmed_;
    }

    ChangeType type()      const { return confirmed_; }
    bool       isStopped() const { return confirmed_ == ChangeType::SuddenStop; }
    bool       isAccelerating() const { return confirmed_ == ChangeType::SuddenAccel; }
    bool       isReversing() const { return confirmed_ == ChangeType::Reversal; }
    double     smoothVelocity() const { return velocityEma_.currentValue(); }

    // 急停回退持续时间(帧): 检测到急停后, 应维持保护多少帧
    int stopFallbackFrames() const
    {
        return confirmed_ == ChangeType::SuddenStop ? 4 : 0;
    }

    // 根据突变程度建议平滑系数: 无突变→0.3, 急停→1.0, 加速→0.8, 折返→0.9
    double suggestedSmoothingAlpha() const
    {
        switch (confirmed_)
        {
            case ChangeType::SuddenStop:   return 1.0;
            case ChangeType::SuddenAccel:  return 0.8;
            case ChangeType::Reversal:     return 0.9;
            default:                       return 0.3;
        }
    }

    void reset()
    {
        velocityEma_.reset(0.0);  // 明确重置状态值和初始化标志
        lastVelocity_ = 0;
        confirmed_ = ChangeType::None;
        count_ = 0;
        lastCandidate_ = ChangeType::None;
        lockout_ = 0;
    }

private:
    ChangeType classify(double v) const
    {
        const double vs = velocityEma_.currentValue();
        const double vp = lastVelocity_;
        if (vs == 0 && vp == 0) return ChangeType::None;

        // 反向折返: 速度符号翻转且幅值显著
        if (vp * v < 0 && std::fabs(v) > 0.5 && std::fabs(vp) > 0.5)
            return ChangeType::Reversal;
        // 急停: 速度骤降至接近0
        if (std::fabs(vs) > 1e-6 && std::fabs(v) < stopRatio_ * std::fabs(vs))
            return ChangeType::SuddenStop;
        // 急加速: 速度跳跃
        if (std::fabs(v - vp) > accelThreshold_)
            return ChangeType::SuddenAccel;
        return ChangeType::None;
    }

    ExponentialMovingAverage velocityEma_{0.6};
    double lastVelocity_ = 0;
    double stopRatio_    = 0.25;
    double accelThreshold_ = 5.0;
    int    confirmFrames_  = 2;
    double emaAlpha_     = 0.6;
    ChangeType confirmed_    = ChangeType::None;
    ChangeType lastCandidate_= ChangeType::None;
    int    count_   = 0;
    int    lockout_ = 0;
};

// ============================================================
//  Perlin-style smooth noise (shared utility)
//    Hash-based continuous noise with smoothstep interpolation.
//    Used by bezier trajectory and overshoot simulation.
// ============================================================
inline double perlinNoise(int frame, double freq)
{
    double t = static_cast<double>(frame) * freq * 0.1;
    int i = static_cast<int>(std::floor(t));
    double f = t - i;
    f = f * f * (3.0 - 2.0 * f); // smoothstep
    // 使用 int64_t 避免中间乘积溢出（n³ * 15731 可能超过 2³¹）
    auto hash = [](int n) -> double {
        int64_t m = static_cast<int64_t>(n);
        m = (m << 13) ^ m;
        int64_t h = m * (m * m * 15731 + 789221) + 1376312589;
        return (1.0 - static_cast<double>(
            static_cast<int32_t>(h & 0x7fffffff)) / 1073741824.0);
    };
    return hash(i) * (1.0 - f) + hash(i + 1) * f;
}

} // namespace filters
