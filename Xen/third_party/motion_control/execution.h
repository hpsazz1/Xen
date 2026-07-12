#pragma once
// ============================================================
//  Execution Layer (统一执行层)
//  把"误差/目标偏移"转化为原始移动量.
//  唯一控制器: Pure Pursuit（纯追踪, 2D）
//    输入 = 延迟补偿后的预测点, 输出 = 本帧应移动 (dx, dy)
//    核心: 误差 = 预测点 − 准星; 输出 = clamp(误差·增益, ±单帧上限)
//    增强: 近端减速 + 死区 + 输出 IIR 低通 + 亚像素余量累计
//           + 速度前馈 + 运动突变保护
//  统一接口: MotionController 抽象基类
// ============================================================
#include "filters.h"
#include <cmath>
#include <algorithm>

namespace execution {

// ---------------- 统一接口(单轴标量) ----------------
class MotionController
{
public:
    virtual ~MotionController() = default;
    // 输入当前误差(目标 - 准星), 返回本帧应执行的移动量
    virtual double computeMovement(double error, double dt, double targetWidth,
                                   double imageSize) = 0;
    virtual void   reset() = 0;
    // 输出限幅: 子类可覆写, 在 computeMovement 内部生效
    virtual void   setOutputLimit(double limit) { (void)limit; }
};


// ============================================================
//  已移除的控制器（统一执行层只保留 Pure Pursuit）：
//    PController（比例追踪）— 未使用，已删除
//    SegmentedCurveController（分段曲线/拟人轨迹）— 未使用，已删除
//    PIDFController（PID+前馈）— 未使用，已删除
// ============================================================


// ============================================================
//  Pure Pursuit Controller (纯追踪, 2D) — 统一执行层唯一控制器
//     "延迟补偿 + 直追" 的执行端:
//       输入 = 延迟补偿后的预测点 (px, py)
//       输出 = 本帧应移动 (dx, dy), 使准星直奔预测点
//     核心: 误差 = 预测点 − 准星; 输出 = clamp(误差·增益, ±单帧上限)
//     增强:
//       + 近端减速 (距离 ≤ 慢速半径 时增益打折, 防过冲)
//       + 死区 (距离 ≤ 死区 时输出 0, 防抖)
//       + 输出 IIR 低通平滑 (每轴一份 EMA)
//       + 亚像素余量累计 (避免 int 截断漂移)
//     继承 MotionController, 提供 2D 接口 computeMovement2D() 和单轴降级接口.
// ============================================================
class PurePursuitController : public MotionController
{
public:
    PurePursuitController(double gain = 0.85, double singleFrameLimit = 60.0,
                          double slowRadius = 25.0, double closeRangeRatio = 0.6,
                          double deadZone = 2.0, double smoothingAlpha = 0.8)
        : gain_(gain), singleFrameLimit_(singleFrameLimit),
          slowRadius_(slowRadius), closeRangeRatio_(closeRangeRatio),
          deadZone_(deadZone)
    {
        smoothX_.setCoefficient(smoothingAlpha);
        smoothY_.setCoefficient(smoothingAlpha);
    }

    void setSmoothingAlpha(double alpha)
    {
        smoothX_.setCoefficient(alpha);
        smoothY_.setCoefficient(alpha);
    }
    void setOutputLimit(double limit) override
    {
        singleFrameLimit_ = std::fabs(limit);
    }
    void setFeedforwardCoeff(double Kf)
    {
        feedforwardCoeff_ = Kf;
        feedforwardOriginal_ = Kf;
    }

    // 挂接突变检测器(可选): 急停时自动停用速度前馈, 3帧后线性恢复
    void setChangeDetector(const filters::MotionChangeDetector* detector)
    {
        if (changeDetector_ && !detector)
        {
            // 检测器被移除时重置回退状态，避免残留状态影响后续
            stopFallback_ = 0;
            lastTriggered_ = false;
        }
        changeDetector_ = detector;
    }

    void reset() override
    {
        carryoverX_ = 0;
        carryoverY_ = 0;
        smoothX_.reset();
        smoothY_.reset();
        currentDistance_ = 0;
        stopFallback_ = 0;
        lastTriggered_ = false;
    }

    // 单轴降级接口(满足 MotionController 基类契约): 仅在 X 轴上追踪
    double computeMovement(double error, double /*dt*/, double /*targetWidth*/,
                           double /*imageSize*/) override
    {
        double sx = 0, sy = 0;
        double dx = 0, dy = 0;
        computeMovement2D(error, 0, sx, sy, dx, dy, false);
        return dx;
    }

    // 输入预测点 (px,py), 准星 (sx,sy), 目标速度 (vx,vy px/帧, 可选前馈)
    // 取整开关: true=输出整数像素并累计余量(贴合鼠标移动), false=输出浮点
    void computeMovement2D(double px, double py, double sx, double sy,
                           double& dx, double& dy, bool integerOutput = true,
                           double targetVx = 0, double targetVy = 0)
    {
        const double ex = px - sx;
        const double ey = py - sy;
        const double distance = std::sqrt(ex * ex + ey * ey);
        currentDistance_ = distance;

        // 死区仅跳位置修正, 速度前馈始终生效(追踪移动目标的近端保持)
        double mx = feedforwardCoeff_ * targetVx;
        double my = feedforwardCoeff_ * targetVy;

        // NOTE: MotionChangeDetector 集成已移除。该检测器原操作控制器输出
        // 而非目标速度，导致检测控制器伪影而非真实运动变化，造成 3 帧额外延迟。
        // PurePursuit 控制器的 IIR 平滑已提供足够的单帧异常保护。

        if (distance > deadZone_)
        {
            double currentGain = gain_;
            if (distance <= slowRadius_) currentGain *= closeRangeRatio_;
            mx += ex * currentGain;
            my += ey * currentGain;
        }
        mx = std::clamp(mx, -singleFrameLimit_, singleFrameLimit_);
        my = std::clamp(my, -singleFrameLimit_, singleFrameLimit_);

        // 输出 IIR 低通平滑
        mx = smoothX_.update(mx);
        my = smoothY_.update(my);

        if (integerOutput)
        {
            mx += carryoverX_;
            my += carryoverY_;
            const int ix = static_cast<int>(mx < 0 ? mx - 0.5 : mx + 0.5);
            const int iy = static_cast<int>(my < 0 ? my - 0.5 : my + 0.5);
            carryoverX_ = mx - static_cast<double>(ix);
            carryoverY_ = my - static_cast<double>(iy);
            dx = static_cast<double>(ix);
            dy = static_cast<double>(iy);
        }
        else
        {
            dx = mx;
            dy = my;
        }
    }

    double currentDistance_ = 0;  // 仅供调试观察

private:
    double gain_, singleFrameLimit_, slowRadius_, closeRangeRatio_, deadZone_;
    double feedforwardCoeff_ = 0.0, feedforwardOriginal_ = 0.0;
    double carryoverX_ = 0.0, carryoverY_ = 0.0;
    filters::ExponentialMovingAverage smoothX_{0.8}, smoothY_{0.8};
    const filters::MotionChangeDetector* changeDetector_ = nullptr;
    int stopFallback_ = 0;
    bool lastTriggered_ = false;
};

} // namespace execution
