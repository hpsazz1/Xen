#ifndef GRENADE_ALIGNMENT_INTERNAL_H
#define GRENADE_ALIGNMENT_INTERNAL_H

#include "grenade_alignment/grenade_alignment.h"
#include "source_context/source_context.h"
#include <chrono>

namespace grenade_alignment::detail {
using Clock = std::chrono::steady_clock;
struct Dependencies {
    std::function<std::unique_ptr<ICapture>(const CaptureConfig&)> create_capture;
    std::function<Clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
    std::function<source_context::SourceContextSnapshot()> source;
    bool settle_before_alignment = false;
};

// 生产runner与测试共用；替换实际采集、时钟及焦点来源，算法和设备许可不旁路。
Result run_with_dependencies(const Reference&, const AppConfig&,
    std::shared_ptr<IMouseController>, std::atomic_bool&, bool verify_only,
    std::function<bool()> permitted, const Dependencies&) noexcept;
}

#endif
