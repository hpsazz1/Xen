#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace VideoReplay
{
    struct TrajectoryPoint
    {
        double timeSeconds = 0.0;
        double globalX = 0.0;
        double globalY = 0.0;
        bool detected = false;
    };

    inline double ObservedCoordinate(
        double globalCoordinate, double cameraOffset, double cropOrigin)
    {
        return globalCoordinate - cameraOffset - cropOrigin;
    }

    inline bool IsNewTrajectorySegment(
        const TrajectoryPoint& previous,
        const TrajectoryPoint& current,
        double maxGapSeconds = 0.10,
        double maxJumpPixels = 64.0)
    {
        return current.timeSeconds - previous.timeSeconds > maxGapSeconds ||
            std::hypot(
                current.globalX - previous.globalX,
                current.globalY - previous.globalY) > maxJumpPixels;
    }

    inline double InterpolateCoordinate(
        const std::vector<TrajectoryPoint>& points,
        size_t index,
        double queryTime,
        bool horizontal)
    {
        if (points.empty())
            return 0.0;

        index = std::min(index, points.size() - 1);
        size_t before = index;
        for (size_t next = index + 1; next < points.size(); ++next)
        {
            if (!points[next].detected)
                continue;
            if (points[next].timeSeconds <= queryTime)
            {
                before = next;
                continue;
            }

            const auto& first = points[before];
            const auto& second = points[next];
            const double span = second.timeSeconds - first.timeSeconds;
            if (!std::isfinite(span) || span <= 0.0)
                return horizontal ? first.globalX : first.globalY;

            const double ratio = std::clamp((queryTime - first.timeSeconds) / span, 0.0, 1.0);
            const double firstValue = horizontal ? first.globalX : first.globalY;
            const double secondValue = horizontal ? second.globalX : second.globalY;
            return firstValue + (secondValue - firstValue) * ratio;
        }

        return horizontal ? points[before].globalX : points[before].globalY;
    }
}
