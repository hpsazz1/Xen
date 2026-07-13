#pragma once

#include <cstdint>
#include <queue>
#include <utility>

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
