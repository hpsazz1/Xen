#pragma once

#include "aim_coordinate_space.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

/**
 * @brief 使用真实设备发送counts与后续目标画面位移，被动估算当前机器的瞄准坐标profile。
 *
 * 标定模式不会主动发送鼠标，也不会修改运行配置。调用方只在底层设备确认发送成功后记录命令，
 * 并以检测帧的观测时间记录raw pivot。模块在多个候选command-to-frame延迟下拟合：
 *
 *   画面位移 = counts响应系数 × 区间counts + 目标恒定漂移速度 × dt
 *
 * counts响应系数应为负值，因为相机向一个方向旋转时，目标画面投影向相反方向移动。
 * 恒定漂移项用于容忍轻微目标移动；加速、换向、角色平移和手动鼠标仍会降低可信度，
 * 所以正式标定应保持目标与角色静止，并让控制输出包含足够的正反向激励。
 */
class PassiveProfileCalibrator
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct AxisEstimate
    {
        bool valid = false;
        double pixelsPerCount = 0.0;
        double degreesPerCount = 0.0;
        double delayMs = 0.0;
        double driftPixelsPerSecond = 0.0;
        double rmsePixels = 0.0;
        double correlation = 0.0;
        double confidence = 0.0;
        size_t sampleCount = 0;
        size_t activeSampleCount = 0;
    };

    struct Snapshot
    {
        bool enabled = false;
        bool valid = false;
        AxisEstimate x;
        AxisEstimate y;
        double overallConfidence = 0.0;
        size_t observationCount = 0;
        size_t commandCount = 0;
    };

    void setEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ == enabled)
            return;
        enabled_ = enabled;
        clearLocked();
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clearLocked();
    }

    void recordCommand(int countsX, int countsY, TimePoint sendTime)
    {
        if ((countsX == 0 && countsY == 0) || sendTime.time_since_epoch().count() == 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_)
            return;

        cumulativeCountsX_ += static_cast<double>(countsX);
        cumulativeCountsY_ += static_cast<double>(countsY);
        commands_.push_back({
            static_cast<double>(countsX), static_cast<double>(countsY),
            cumulativeCountsX_, cumulativeCountsY_, sendTime
        });
        pruneLocked(sendTime);
    }

    void addObservation(double pivotX, double pivotY, TimePoint observationTime,
                        double sourceWidth, double sourceHeight,
                        double fovXDegrees, double fovYDegrees)
    {
        if (!std::isfinite(pivotX) || !std::isfinite(pivotY) ||
            observationTime.time_since_epoch().count() == 0)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_)
            return;

        if (!observations_.empty())
        {
            const double dt = std::chrono::duration<double>(
                observationTime - observations_.back().time).count();
            // 非单调时间或长时间断流表示目标/帧序列已经切换。保留启用状态，但重新收集样本，
            // 防止跨目标的大跳变被回归误认为设备响应。
            if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.25)
            {
                observations_.clear();
                cached_ = Snapshot{};
                cached_.enabled = true;
            }
        }

        observations_.push_back({ pivotX, pivotY, observationTime });
        sourceWidth_ = std::max(1.0, sourceWidth);
        sourceHeight_ = std::max(1.0, sourceHeight);
        fovXDegrees_ = fovXDegrees;
        fovYDegrees_ = fovYDegrees;
        pruneLocked(observationTime);

        // 约100 FPS时每8个观测更新一次，避免每帧重复搜索全部延迟候选；
        // UI和CSV读取的始终是最近一次完整快照。
        if (observations_.size() >= kMinimumObservations &&
            observations_.size() - lastComputedObservationCount_ >= 8)
        {
            computeLocked();
            lastComputedObservationCount_ = observations_.size();
        }
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result = cached_;
        result.enabled = enabled_;
        result.observationCount = observations_.size();
        result.commandCount = commands_.size();
        return result;
    }

private:
    struct CommandSample
    {
        double x = 0.0;
        double y = 0.0;
        double cumulativeX = 0.0;
        double cumulativeY = 0.0;
        TimePoint time{};
    };

    struct ObservationSample
    {
        double x = 0.0;
        double y = 0.0;
        TimePoint time{};
    };

    struct RegressionRow
    {
        double command = 0.0;
        double dt = 0.0;
        double displacement = 0.0;
        double weight = 1.0;
    };

    struct RegressionResult
    {
        bool valid = false;
        double commandSlope = 0.0;
        double drift = 0.0;
        double rmse = 0.0;
        double correlation = 0.0;
        double normalizedRmse = std::numeric_limits<double>::infinity();
        size_t sampleCount = 0;
        size_t activeSampleCount = 0;
    };

    static constexpr size_t kMinimumObservations = 24;
    static constexpr size_t kMinimumActiveSamples = 8;
    static constexpr int kMaximumDelayMs = 250;
    static constexpr int kDelayStepMs = 2;

    void clearLocked()
    {
        commands_.clear();
        observations_.clear();
        cumulativeCountsX_ = 0.0;
        cumulativeCountsY_ = 0.0;
        sourceWidth_ = sourceHeight_ = 1.0;
        fovXDegrees_ = fovYDegrees_ = 90.0;
        lastComputedObservationCount_ = 0;
        cached_ = Snapshot{};
        cached_.enabled = enabled_;
    }

    void pruneLocked(TimePoint now)
    {
        constexpr auto lifetime = std::chrono::seconds(24);
        while (observations_.size() > kMinimumObservations &&
               now - observations_.front().time > lifetime)
        {
            observations_.pop_front();
        }
        while (observations_.size() > 2400)
            observations_.pop_front();
        while (commands_.size() > 4800)
            commands_.pop_front();
    }

    std::pair<double, double> cumulativeCountsAtLocked(TimePoint time) const
    {
        if (commands_.empty())
            return { cumulativeCountsX_, cumulativeCountsY_ };

        auto after = std::upper_bound(
            commands_.begin(), commands_.end(), time,
            [](const TimePoint& value, const CommandSample& sample) {
                return value < sample.time;
            });
        if (after == commands_.begin())
        {
            return {
                commands_.front().cumulativeX - commands_.front().x,
                commands_.front().cumulativeY - commands_.front().y
            };
        }
        if (after == commands_.end())
            return { cumulativeCountsX_, cumulativeCountsY_ };
        --after;
        return { after->cumulativeX, after->cumulativeY };
    }

    double commandBetweenLocked(TimePoint start, TimePoint end, bool horizontal) const
    {
        const auto startCounts = cumulativeCountsAtLocked(start);
        const auto endCounts = cumulativeCountsAtLocked(end);
        return horizontal
            ? endCounts.first - startCounts.first
            : endCounts.second - startCounts.second;
    }

    static double median(std::vector<double> values)
    {
        if (values.empty())
            return 0.0;
        const size_t middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + middle, values.end());
        double result = values[middle];
        if (values.size() % 2 == 0)
        {
            const auto lower = std::max_element(values.begin(), values.begin() + middle);
            result = (*lower + result) * 0.5;
        }
        return result;
    }

    static bool solveWeightedRegression(std::vector<RegressionRow>& rows,
                                        double& commandSlope, double& drift)
    {
        double commandSquared = 0.0;
        double commandDt = 0.0;
        double dtSquared = 0.0;
        double commandY = 0.0;
        double dtY = 0.0;
        for (const RegressionRow& row : rows)
        {
            commandSquared += row.weight * row.command * row.command;
            commandDt += row.weight * row.command * row.dt;
            dtSquared += row.weight * row.dt * row.dt;
            commandY += row.weight * row.command * row.displacement;
            dtY += row.weight * row.dt * row.displacement;
        }
        const double determinant = commandSquared * dtSquared - commandDt * commandDt;
        if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-12)
            return false;
        commandSlope = (commandY * dtSquared - dtY * commandDt) / determinant;
        drift = (dtY * commandSquared - commandY * commandDt) / determinant;
        return std::isfinite(commandSlope) && std::isfinite(drift);
    }

    static RegressionResult fitRows(std::vector<RegressionRow> rows)
    {
        RegressionResult result;
        result.sampleCount = rows.size();
        result.activeSampleCount = static_cast<size_t>(std::count_if(
            rows.begin(), rows.end(), [](const RegressionRow& row) {
                return std::abs(row.command) >= 0.5;
            }));
        if (rows.size() < kMinimumObservations - 1 ||
            result.activeSampleCount < kMinimumActiveSamples)
        {
            return result;
        }

        double commandSlope = 0.0;
        double drift = 0.0;
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            if (!solveWeightedRegression(rows, commandSlope, drift))
                return result;

            std::vector<double> absoluteResiduals;
            absoluteResiduals.reserve(rows.size());
            for (const RegressionRow& row : rows)
            {
                absoluteResiduals.push_back(std::abs(
                    row.displacement - commandSlope * row.command - drift * row.dt));
            }
            const double robustScale = std::max(0.05, median(std::move(absoluteResiduals)) * 1.4826);
            const double huberCutoff = robustScale * 1.5;
            for (RegressionRow& row : rows)
            {
                const double residual = std::abs(
                    row.displacement - commandSlope * row.command - drift * row.dt);
                row.weight = residual <= huberCutoff ? 1.0 : huberCutoff / residual;
            }
        }

        double weightedSquaredError = 0.0;
        double weightSum = 0.0;
        double commandMean = 0.0;
        double adjustedMean = 0.0;
        for (const RegressionRow& row : rows)
        {
            const double adjusted = row.displacement - drift * row.dt;
            commandMean += row.weight * row.command;
            adjustedMean += row.weight * adjusted;
            weightSum += row.weight;
            const double residual = adjusted - commandSlope * row.command;
            weightedSquaredError += row.weight * residual * residual;
        }
        if (weightSum <= 0.0)
            return result;
        commandMean /= weightSum;
        adjustedMean /= weightSum;

        double covariance = 0.0;
        double commandVariance = 0.0;
        double adjustedVariance = 0.0;
        for (const RegressionRow& row : rows)
        {
            const double commandCentered = row.command - commandMean;
            const double adjustedCentered =
                row.displacement - drift * row.dt - adjustedMean;
            covariance += row.weight * commandCentered * adjustedCentered;
            commandVariance += row.weight * commandCentered * commandCentered;
            adjustedVariance += row.weight * adjustedCentered * adjustedCentered;
        }

        result.commandSlope = commandSlope;
        result.drift = drift;
        result.rmse = std::sqrt(weightedSquaredError / weightSum);
        const double correlationDenominator = std::sqrt(commandVariance * adjustedVariance);
        result.correlation = correlationDenominator > 1e-12
            ? covariance / correlationDenominator : 0.0;
        const double signalRms = std::sqrt(std::max(0.0, adjustedVariance / weightSum));
        result.normalizedRmse = result.rmse / std::max(0.1, signalRms);
        result.valid = commandSlope < -1e-4 && std::isfinite(result.normalizedRmse);
        return result;
    }

    RegressionResult estimateForDelayLocked(bool horizontal, int delayMs) const
    {
        std::vector<RegressionRow> rows;
        rows.reserve(observations_.size() - 1);
        const auto delay = std::chrono::milliseconds(delayMs);
        for (size_t index = 1; index < observations_.size(); ++index)
        {
            const ObservationSample& previous = observations_[index - 1];
            const ObservationSample& current = observations_[index];
            const double dt = std::chrono::duration<double>(current.time - previous.time).count();
            if (!std::isfinite(dt) || dt < 0.001 || dt > 0.100)
                continue;
            rows.push_back({
                commandBetweenLocked(previous.time - delay, current.time - delay, horizontal),
                dt,
                horizontal ? current.x - previous.x : current.y - previous.y,
                1.0
            });
        }
        return fitRows(std::move(rows));
    }

    AxisEstimate estimateAxisLocked(bool horizontal) const
    {
        RegressionResult best;
        int bestDelayMs = 0;
        double bestScore = std::numeric_limits<double>::infinity();
        for (int delayMs = 0; delayMs <= kMaximumDelayMs; delayMs += kDelayStepMs)
        {
            const RegressionResult candidate = estimateForDelayLocked(horizontal, delayMs);
            if (!candidate.valid)
                continue;
            const double score = candidate.normalizedRmse +
                (1.0 - std::min(1.0, std::abs(candidate.correlation))) * 0.20;
            if (score < bestScore)
            {
                best = candidate;
                bestDelayMs = delayMs;
                bestScore = score;
            }
        }

        AxisEstimate estimate;
        if (!best.valid)
            return estimate;

        estimate.pixelsPerCount = -best.commandSlope;
        estimate.delayMs = static_cast<double>(bestDelayMs);
        estimate.driftPixelsPerSecond = best.drift;
        estimate.rmsePixels = best.rmse;
        estimate.correlation = best.correlation;
        estimate.sampleCount = best.sampleCount;
        estimate.activeSampleCount = best.activeSampleCount;

        const double fov = horizontal ? fovXDegrees_ : fovYDegrees_;
        const double sourceSpan = horizontal ? sourceWidth_ : sourceHeight_;
        estimate.degreesPerCount = std::abs(
            AimCoordinateSpace::angleDegreesForSourcePixelDelta(
                estimate.pixelsPerCount, fov, sourceSpan));

        const double correlationQuality = std::clamp(
            (std::abs(best.correlation) - 0.45) / 0.50, 0.0, 1.0);
        const double fitQuality = std::clamp(1.0 - best.normalizedRmse, 0.0, 1.0);
        const double sampleQuality = std::min(
            1.0, best.activeSampleCount / 40.0);
        estimate.confidence = correlationQuality *
            std::sqrt(fitQuality * sampleQuality);
        estimate.valid = estimate.pixelsPerCount >= 0.01 &&
            estimate.pixelsPerCount <= 20.0 &&
            best.correlation <= -0.45 &&
            estimate.confidence >= 0.10;
        return estimate;
    }

    void computeLocked()
    {
        Snapshot result;
        result.enabled = enabled_;
        result.observationCount = observations_.size();
        result.commandCount = commands_.size();
        result.x = estimateAxisLocked(true);
        result.y = estimateAxisLocked(false);
        result.valid = result.x.valid || result.y.valid;
        if (result.x.valid && result.y.valid)
            result.overallConfidence = (result.x.confidence + result.y.confidence) * 0.5;
        else
            result.overallConfidence = std::max(result.x.confidence, result.y.confidence);
        cached_ = result;
    }

    mutable std::mutex mutex_;
    bool enabled_ = false;
    std::deque<CommandSample> commands_;
    std::deque<ObservationSample> observations_;
    double cumulativeCountsX_ = 0.0;
    double cumulativeCountsY_ = 0.0;
    double sourceWidth_ = 1.0;
    double sourceHeight_ = 1.0;
    double fovXDegrees_ = 90.0;
    double fovYDegrees_ = 90.0;
    size_t lastComputedObservationCount_ = 0;
    Snapshot cached_{};
};
