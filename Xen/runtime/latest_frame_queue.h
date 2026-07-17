#pragma once

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <queue>
#include <utility>

/**
 * @brief 向有界逐帧队列追加一帧，返回因容量不足而丢弃的最旧帧数
 *
 * 该模式仅供需要保留时间序列的诊断工具使用。实时推理仍应使用
 * ReplaceWithLatestFrame，避免处理过期画面。
 */
template<typename T>
uint64_t AppendBoundedFrame(std::queue<T>& queue, T frame, size_t capacity)
{
    const size_t safeCapacity = std::max<size_t>(1, capacity);
    uint64_t dropped = 0;
    while (queue.size() >= safeCapacity)
    {
        queue.pop();
        ++dropped;
    }
    queue.push(std::move(frame));
    return dropped;
}

/** @brief 从逐帧诊断队列取出最旧帧，保持接收顺序。 */
template<typename T>
bool TakeOldestFrame(std::queue<T>& queue, T& oldest)
{
    if (queue.empty())
        return false;
    oldest = std::move(queue.front());
    queue.pop();
    return true;
}

/**
 * @brief 用最新帧替换队列中所有尚未消费的旧帧
 *
 * 实时采集的目标是最小观测延迟，而不是逐帧离线处理。生产速度超过消费速度时，继续排队只会让
 * 检测器处理过期画面，因此保留最新帧并返回被替换数量，供诊断统计真实反压情况。
 */
template<typename T>
uint64_t ReplaceWithLatestFrame(std::queue<T>& queue, T frame)
{
    const uint64_t superseded = static_cast<uint64_t>(queue.size());
    while (!queue.empty())
        queue.pop();
    queue.push(std::move(frame));
    return superseded;
}

/**
 * @brief 取出队列中的最新帧，并返回因异常积压而同时清理的旧帧数
 */
template<typename T>
bool TakeLatestFrame(std::queue<T>& queue, T& latest, uint64_t& skipped)
{
    if (queue.empty())
    {
        skipped = 0;
        return false;
    }

    latest = std::move(queue.back());
    skipped = queue.size() > 1 ? static_cast<uint64_t>(queue.size() - 1) : 0;
    while (!queue.empty())
        queue.pop();
    return true;
}
