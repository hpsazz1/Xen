#ifndef AUTO_STOP_COUNTERPULSE_HUD_H
#define AUTO_STOP_COUNTERPULSE_HUD_H

#include "sampling_analysis_internal.h"
#include "config/ui_theme.h"
#include <memory>

namespace auto_stop_probe_detail {
// 只读显示：主线程仅有界入队，窗口、模型查询和绘图均归独立UI线程。
class CounterpulseHud {
public:
    explicit CounterpulseHud(const sampling_detail::SamplingSettings& settings, bool external_snapshots = false,
        bool persistent_window = false, UiTheme theme = UiTheme::DARK, bool initially_visible = true);
    ~CounterpulseHud();
    CounterpulseHud(const CounterpulseHud&) = delete;
    CounterpulseHud& operator=(const CounterpulseHud&) = delete;
    void observe(const Json& command) noexcept;
    // 外部模式只发布已分析快照；只保留近32条显示历史，不改变采集或评价。
    void publish(const Json& snapshot) noexcept;
    void finish(const Json& report) noexcept;
    void set_visible(bool visible) noexcept;
    void set_theme(UiTheme theme) noexcept;
    // 仅任务编排后台调用；等待窗口线程重置每组模型，禁止与旧组生产者并发。
    bool begin_session(const sampling_detail::SamplingSettings& settings,
        bool external_snapshots, bool model_enabled = true) noexcept;
    void end_session() noexcept;
    bool visible() const noexcept;
    bool failed() const noexcept;
    bool task_active() const noexcept;
    std::uint64_t close_sequence() const noexcept;
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
