#ifndef AUTO_STOP_COUNTERPULSE_HUD_H
#define AUTO_STOP_COUNTERPULSE_HUD_H

#include "sampling_analysis_internal.h"
#include <memory>

namespace auto_stop_probe_detail {
// 只读显示：主线程仅有界入队，窗口、模型查询和绘图均归独立UI线程。
class CounterpulseHud {
public:
    explicit CounterpulseHud(const sampling_detail::SamplingSettings& settings);
    ~CounterpulseHud();
    CounterpulseHud(const CounterpulseHud&) = delete;
    CounterpulseHud& operator=(const CounterpulseHud&) = delete;
    void observe(const Json& command) noexcept;
    void finish(const Json& report) noexcept;
    Json status() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
