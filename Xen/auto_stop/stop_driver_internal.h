#ifndef XEN_STOP_DRIVER_INTERNAL_H
#define XEN_STOP_DRIVER_INTERNAL_H
#include "auto_stop/auto_stop.h"
#include "auto_stop/hud_stop.h"
#include <variant>

namespace detail {
// 一个worker只拥有一种策略；设备、许可与清理仍由同一个worker负责。
class StopDriver {
public:
    explicit StopDriver(const AutoStopConfig& config) : driver_(AutoStopController(config)) {
        if (config.experimental_hud_model) driver_.emplace<HudStopController>(config);
    }
    AutoStopDecision observe(const WasdMotionIntent& input, std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.observe(input,now);},driver_);
    }
    AutoStopDecision request(std::uint64_t id,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.request(id,now);},driver_);
    }
    AutoStopDecision request_manual_release(std::uint64_t id,std::uint8_t mask,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.request_manual_release(id,mask,now);},driver_);
    }
    AutoStopDecision cancel(std::uint64_t id,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.cancel(id,now);},driver_);
    }
    AutoStopDecision tick(std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.tick(now);},driver_);
    }
    AutoStopDecision acknowledge(std::uint64_t id,std::uint64_t command,std::uint8_t mask,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.acknowledge(id,command,mask,now);},driver_);
    }
    AutoStopDecision decision() const noexcept {
        return std::visit([](const auto& value){return value.decision();},driver_);
    }
    bool restart_after_cleanup(const WasdMotionIntent& input,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.restart_after_cleanup(input,now);},driver_);
    }
    bool resume_after_masked_hold(const WasdMotionIntent& input,std::int64_t now) noexcept {
        return std::visit([&](auto& value){return value.resume_after_masked_hold(input,now);},driver_);
    }
    auto hud_telemetry() const noexcept {
        if (const auto* value=std::get_if<HudStopController>(&driver_)) return value->telemetry();
        return HudStopController{}.telemetry();
    }
private:
    std::variant<AutoStopController,HudStopController> driver_;
};
}
#endif
