#ifndef RECOIL_WALL_CAPTURE_RUN_H
#define RECOIL_WALL_CAPTURE_RUN_H
#include "capture/capture.h"
#include "mouse/mouse.h"
#include "recoil_tuner/wall_capture_analysis.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <functional>

namespace recoil_tuner {
enum class WallRunMode { CALIBRATE, CAPTURE, TEST };
struct WallRunRequest {
    WallRunMode mode = WallRunMode::CAPTURE;
    bool measurement_required = true;
    CaptureConfig capture;
    std::filesystem::path output_directory;
    std::string weapon_id, environment_fingerprint;
    double sensitivity = 0;
    int duration_ms = 1500, trigger_virtual_key = 0, cancel_virtual_key = 0x23;
    int target_shots = 0;
    cv::Rect registration_roi;
    // 外层持有唯一 Source/GSI 服务，返回焦点及武器上下文是否仍有效。
    std::function<bool()> context_valid;
    std::function<std::string()> context_block_reason;
    // 使用当前实际帧几何复核标定绑定，在任何设备输出之前执行。
    std::function<bool(const nlohmann::json&)> geometry_valid;
    // Worker 在任何 DOWN 前预备；仅本模块拥有软件左键。
    std::function<bool()> on_ready;
    std::function<bool(std::chrono::steady_clock::time_point, const ButtonReceipt&)> on_firing_started;
    std::function<void()> on_firing_stopped;
    // 按GSI弹药减少量停止并在UP后核对；接收计数不代表实际逐发时刻或物理恰好发数。
    // 返回起始弹量减当前弹量（有符号），补满超过起始弹量为负，缺失必须返回nullopt。
    std::function<std::optional<int>()> observed_ammo_delta;
    // 无输出专项测试可注入Capture；生产省略时使用正式工厂。
    std::function<std::unique_ptr<ICapture>(const CaptureConfig&)> capture_factory;
};
struct WallRunResult {
    bool completed = false, cleanup_unknown = false;
    std::string message;
    nlohmann::json report;
    std::vector<WallFrame> frames;
    std::vector<WallCalibrationSample> calibration;
};
// 由已有 Debug 后台线程调用；外层保证 Runtime 已停止并借出唯一设备。
// 调用已由用户前台按键触发，先等测试键/左键/WASD释放，不再等待第二次按键。
WallRunResult run_wall_capture(const WallRunRequest&, std::shared_ptr<IMouseController>,
    const std::atomic<bool>& canceled,
    const std::function<void(const std::string&)>& progress = {}) noexcept;
}
#endif
