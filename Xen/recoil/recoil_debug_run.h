#ifndef RECOIL_DEBUG_RUN_H
#define RECOIL_DEBUG_RUN_H
#include "config/config.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <atomic>

// 准备仅冻结候选、软件预算与键绑定，不连接设备、不修改原曲线或校准等级。
nlohmann::json prepare_recoil_debug_plan(const std::filesystem::path&, const AppConfig&,
    double x_strength = 1, double y_strength = 1);
nlohmann::json prepare_wall_debug_plan(bool calibrate, const std::string& weapon_id,
    int duration_ms, const std::filesystem::path& calibration_path, const AppConfig&);
bool recoil_debug_geometry_matches(const nlohmann::json& plan, const nlohmann::json& actual) noexcept;
// 借用App独占设备；一次用户热键许可的有界扫射，左键仅归采集runner，不close设备。
// hook在后台周期调用；采集方可通过目录与时间关联原始批次，不能把ACK当成命中观测。
using RecoilDebugCaptureHook = std::function<void(const std::filesystem::path&, RecoilTime)>;
nlohmann::json run_recoil_debug(const nlohmann::json&, const AppConfig&,
    const std::shared_ptr<IMouseController>&, const std::filesystem::path&,
    const std::atomic<bool>& canceled, RecoilDebugCaptureHook capture = {});
#endif
