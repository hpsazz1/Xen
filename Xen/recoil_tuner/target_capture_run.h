#ifndef RECOIL_TARGET_CAPTURE_RUN_H
#define RECOIL_TARGET_CAPTURE_RUN_H
#include "capture/capture.h"
#include "mouse/mouse.h"
#include "recoil/recoil.h"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>

namespace recoil_tuner {
// 一个线程持有唯一派发通道；前馈和反馈都只是分量，不创建 RecoilWorker。
struct TargetRunRequest {
    nlohmann::json plan;
    CaptureConfig capture;
    std::filesystem::path output_directory;
    std::function<bool()> context_valid;
    std::function<nlohmann::json()> context_facts;
    std::function<std::optional<int>()> ammo;
    std::function<std::unique_ptr<ICapture>(const CaptureConfig&)> capture_factory;
};
struct TargetRunResult {
    bool completed = false, cleanup_unknown = false;
    std::string message;
    nlohmann::json report;
};
// 有界量化器将舍入归入各分量；总数严格守恒，未知回执后不可继续。
class TargetCommandLedger {
public:
    bool prepare(std::array<double,2> ff, std::array<double,2> fb,
        int command_limit, int total_limit, nlohmann::json& record, std::string& reason);
    void complete(bool known) noexcept;
private:
    std::array<double,2> ff_remainder_{}, fb_remainder_{};
    int spent_ = 0;
    bool terminal_ = false, pending_ = false;
};
TargetRunResult run_target_capture(const TargetRunRequest&, const std::shared_ptr<IMouseController>&,
    const std::atomic<bool>& canceled,
    const std::function<void(const std::string&)>& progress = {}) noexcept;
}
#endif
