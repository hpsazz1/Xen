#ifndef AUTO_STOP_COUNTERPULSE_HUD_H
#define AUTO_STOP_COUNTERPULSE_HUD_H

#include "sampling_analysis_internal.h"
#include <memory>

namespace auto_stop_probe_detail {
// 只读显示：主线程仅有界入队，窗口、模型查询和绘图均归独立UI线程。
class CounterpulseHud {
public:
    explicit CounterpulseHud(const sampling_detail::SamplingSettings& settings, bool external_snapshots = false);
    ~CounterpulseHud();
    CounterpulseHud(const CounterpulseHud&) = delete;
    CounterpulseHud& operator=(const CounterpulseHud&) = delete;
    void observe(const Json& command) noexcept;
    // 外部模式只发布已分析快照；只保留近32条显示历史，不改变采集或评价。
    void publish(const Json& snapshot) noexcept;
    void finish(const Json& report) noexcept;
    void set_visible(bool visible) noexcept;
    bool closed() const noexcept;
    bool stop_requested() const noexcept;
    Json status() const;
    // 与HUD共用同一分析结果；窗口线程最多10Hz发布，读取方不运行模型。
    std::shared_ptr<const Json> latest_analysis() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
