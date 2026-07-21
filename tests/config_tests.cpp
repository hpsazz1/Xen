#include "config/config.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>

// config.cpp 在主程序中使用全局配置锁；测试目标单独提供同名锁，复现真实线程保护边界。
std::mutex configMutex;

namespace
{
int failures = 0;

void expectNear(double actual, double expected, double tolerance, const char* name)
{
    if (std::abs(actual - expected) <= tolerance)
        return;

    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void expectString(const std::string& actual, const std::string& expected, const char* name)
{
    if (actual == expected)
        return;

    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void expectTrue(bool condition, const char* name)
{
    if (condition)
        return;

    std::cerr << name << ": condition was false\n";
    ++failures;
}
}

int main()
{
    Config config{};
    config.auto_derive_tracker_params = true;
    config.move_response_ms = 73.0f;
    config.move_max_speed_cps = 1337.0f;
    config.move_catch_up_max_speed_cps = 3555.0f;
    config.move_integral_time_ms = 333.0f;

    config.applyAutoDerivedTrackerParams(320, 240);
    expectNear(config.move_response_ms, 73.0, 0.0,
               "auto derive preserves configured response time");
    expectNear(config.move_max_speed_cps, 1337.0, 0.0,
               "auto derive preserves configured maximum speed");
    expectNear(config.move_catch_up_max_speed_cps, 3555.0, 0.0,
               "auto derive preserves configured conditional catch-up speed");
    expectNear(config.move_integral_time_ms, 333.0, 0.0,
               "auto derive preserves configured moving integral time");
    expectNear(config.ml_termination_frames, 30.0, 0.0,
               "auto derive still updates fps-dependent tracker lifetime");
    expectNear(config.ml_coast_frames, 60.0, 0.0,
               "auto derive still updates fps-dependent tracker coasting");

    // 模拟运行中捕获 FPS 和检测分辨率变化，确保重复自动推导仍不会覆盖用户移动标定。
    config.applyAutoDerivedTrackerParams(640, 500);
    expectNear(config.move_response_ms, 73.0, 0.0,
               "runtime re-derive preserves configured response time");
    expectNear(config.move_max_speed_cps, 1337.0, 0.0,
               "runtime re-derive preserves configured maximum speed");
    expectNear(config.move_catch_up_max_speed_cps, 3555.0, 0.0,
               "runtime re-derive preserves configured conditional catch-up speed");
    expectNear(config.move_integral_time_ms, 333.0, 0.0,
               "runtime re-derive preserves configured moving integral time");
    expectNear(config.ml_termination_frames, 62.0, 0.0,
               "runtime re-derive applies updated fps to tracker");
    expectNear(config.ml_measurement_stddev, 5.0, 1e-6,
               "runtime re-derive applies updated resolution to tracker");

    // 新配置文件必须直接生成当前真实链路参数，避免每次部署后再手动替换地址和坐标空间。
    const std::filesystem::path defaultsPath = "xen_config_defaults_test.ini";
    std::error_code removeError;
    std::filesystem::remove(defaultsPath, removeError);
    Config defaults{};
    if (!defaults.loadConfig(defaultsPath.string()))
    {
        std::cerr << "default config creation failed\n";
        ++failures;
    }
    else
    {
        expectString(defaults.udp_ip, "192.168.3.10", "default udp source ip");
        expectNear(defaults.udp_port, 2333.0, 0.0, "default udp port");
        expectNear(defaults.udp_source_width, 2560.0, 0.0, "default udp full fov width");
        expectNear(defaults.udp_source_height, 1440.0, 0.0, "default udp full fov height");
        expectString(defaults.ndi_source_name, "HPSAZZ (main)", "default ndi source name");
        expectNear(defaults.ndi_source_width, 2560.0, 0.0, "default ndi full fov width");
        expectNear(defaults.ndi_source_height, 1440.0, 0.0, "default ndi full fov height");
        expectNear(defaults.detection_resolution, 320.0, 0.0, "default detection resolution");
        expectNear(defaults.move_max_speed_cps, 3200.0, 0.0,
                   "default maximum speed follows moving-target standard");
        expectNear(defaults.move_catch_up_max_speed_cps, 4000.0, 0.0,
                   "default conditional catch-up speed uses the measured device ceiling");
        expectNear(defaults.move_integral_time_ms, 500.0, 0.0,
                   "moving integral uses the anti-oscillation accumulation window");
        expectTrue(!defaults.manual_control_enabled,
                   "manual control arbitration defaults to opt-in");
        expectNear(defaults.manual_control_enter_dps, 0.60, 1e-6,
                   "manual arbitration uses the documented entry angular speed");
        expectNear(defaults.manual_control_full_dps, 3.00, 1e-6,
                   "manual arbitration uses the documented full takeover angular speed");
        expectNear(defaults.manual_control_same_weight, 0.50, 1e-6,
                   "manual same-direction weight uses the conservative baseline");
        expectNear(defaults.manual_control_cross_weight, 0.20, 1e-6,
                   "manual cross-direction weight uses the conservative baseline");
        expectNear(defaults.aim_motion_compensation_delay_ms, 12.0, 0.0,
                   "production motion compensation uses the measured NDI response delay");
        expectNear(defaults.aim_motion_compensation_response_ms, 0.0, 0.0,
                   "production motion compensation keeps the validated fixed-delay response");
        expectString(defaults.aim_pipeline_mode, "legacy",
                     "new pipeline defaults to legacy mode");
        expectNear(defaults.aim_shadow_command_to_frame_delay_ms, 60.0, 0.0,
                    "shadow applied-view model keeps the compatible response center");
        expectNear(defaults.aim_shadow_command_response_ms, 0.0, 0.0,
                    "shadow applied-view model defaults to the compatible step response");
        expectNear(defaults.aim_shadow_response_ms, 80.0, 0.0,
                   "shadow angle controller defaults to the P-only response baseline");
        expectNear(defaults.aim_shadow_feedforward_gain, 0.0, 0.0,
                   "shadow velocity feedforward remains disabled by default");
        expectNear(defaults.aim_shadow_settle_error_deg, 0.08, 1e-6,
                   "shadow settle uses the replay-derived angular error threshold");
        expectNear(defaults.aim_shadow_settle_rate_dps, 1.2, 1e-6,
                   "shadow settle uses the replay-derived relative LOS rate threshold");
        expectNear(defaults.aim_shadow_reverse_confirm_ms, 80.0, 0.0,
                   "shadow low-speed reverse uses the established time-domain confirmation");
        expectNear(defaults.aim_shadow_vertical_catch_up_deg, 0.8, 1e-6,
                   "shadow vertical catch-up uses the cross-domain angle threshold");
        expectNear(defaults.aim_shadow_integral_time_ms, 0.0, 0.0,
                   "shadow angle integral remains disabled by default");
        expectNear(defaults.aim_shadow_lead_horizon_ms, 0.0, 0.0,
                   "shadow experience lead horizon remains disabled by default");
        expectNear(defaults.aim_shadow_lead_strength, 0.0, 0.0,
                   "shadow experience lead strength remains disabled by default");
        expectString(defaults.aim_shadow_estimator_mode, "kalman",
                     "shadow maneuver estimator remains opt-in by default");
        expectNear(defaults.aim_shadow_ca_jerk_std_dps3, 8000.0, 0.0,
                   "shadow acceleration estimator uses the frozen offline jerk noise");
        expectNear(defaults.aim_shadow_maneuver_rate_threshold_dps, 12.0, 0.0,
                   "shadow maneuver gate uses the zero-regression offline threshold");
        expectNear(defaults.aim_shadow_maneuver_hold_ms, 120.0, 0.0,
                   "shadow maneuver gate uses the frozen hold window");
        expectString(defaults.trajectory_shaper_mode, "off",
                     "trajectory shaper defaults to exact pass-through mode");
        expectNear(defaults.trajectory_output_hz, 240.0, 0.0,
                   "trajectory scheduler uses the documented fixed rate");
        expectNear(defaults.trajectory_max_velocity_cps, 1440.0, 0.0,
                   "trajectory speed defaults to the controller device budget");
        expectNear(defaults.prediction_lead_ms, 50.0, 0.0,
                   "default prediction uses kinematic replay lead");
        expectNear(defaults.prediction_velocity_tau_ms, 50.0, 0.0,
                   "default prediction uses robust velocity regression window");
        expectNear(defaults.prediction_strength, 1.0, 0.0,
                   "default prediction uses bounded constant-velocity strength");
        expectTrue(!defaults.profile_calibration_enabled,
                   "passive profile calibration defaults to disabled");
        expectTrue(!defaults.machine_profile_cache_enabled,
                   "machine profile cache defaults to disabled");
        expectString(defaults.machine_profile_cache_path, "",
                     "machine profile cache has no implicit path");
        expectString(defaults.machine_profile_aim_mode, "hipfire",
                     "machine profile cache defaults to hipfire key");
        expectString(defaults.kmbox_net_ip, "192.168.2.188", "default kmbox net ip");
        expectString(defaults.kmbox_net_port, "13384", "default kmbox net port");
        expectString(defaults.kmbox_net_uuid, "7679E04E", "default kmbox net uuid");
        expectString(defaults.active_game, "CS", "default active game profile");
        expectNear(defaults.game_profiles.at("CS").sens, 1.4, 1e-9,
                   "default cs sensitivity");
        expectNear(defaults.game_profiles.at("CS").yaw, 0.022, 1e-9,
                   "default cs yaw");
        expectNear(defaults.game_profiles.at("CS").pitch, 0.022, 1e-9,
                   "default cs pitch");
        expectNear(defaults.game_profiles.at("UNIFIED").sens, 1.0, 1e-9,
                   "default unified sensitivity");
        expectNear(defaults.pipeline_tracer_max_frames, 1000.0, 0.0,
                   "default pipeline trace capacity");
    }
    std::filesystem::remove(defaultsPath, removeError);

    const std::filesystem::path legacyPath = "xen_config_legacy_prediction_test.ini";
    {
        std::ofstream legacyFile(legacyPath);
        legacyFile << "prediction_enabled = true\n"
                   << "predictionInterval = 0.027\n"
                   << "prediction_tau = 0.042\n";
    }
    Config migrated{};
    if (!migrated.loadConfig(legacyPath.string()))
    {
        std::cerr << "legacy prediction config migration failed\n";
        ++failures;
    }
    else
    {
        expectNear(migrated.prediction_lead_ms, 27.0, 1e-6,
                   "legacy prediction interval migrates to milliseconds");
        expectNear(migrated.prediction_velocity_tau_ms, 42.0, 1e-6,
                   "legacy prediction tau migrates to milliseconds");
        expectTrue(migrated.saveConfig(legacyPath.string()),
                   "migrated prediction config saves successfully");
        std::ifstream migratedFile(legacyPath);
        const std::string migratedText(
            (std::istreambuf_iterator<char>(migratedFile)),
            std::istreambuf_iterator<char>());
        expectTrue(migratedText.find("prediction_lead_ms = 27") != std::string::npos,
                   "saved config writes new prediction lead key");
        expectTrue(migratedText.find("prediction_velocity_tau_ms = 42") != std::string::npos,
                   "saved config writes compatible prediction window key");
        expectTrue(migratedText.find("prediction_strength = 1") != std::string::npos,
                   "legacy config receives bounded constant-velocity prediction strength");
        expectTrue(migratedText.find("profile_calibration_enabled = false") != std::string::npos,
                   "saved config persists passive profile calibration state");
        expectTrue(migratedText.find("machine_profile_cache_enabled = false") != std::string::npos &&
                   migratedText.find("machine_profile_cache_path = ") != std::string::npos &&
                   migratedText.find("machine_profile_aim_mode = hipfire") != std::string::npos,
                   "saved config persists explicit machine cache controls");
        expectTrue(migratedText.find("aim_pipeline_mode = legacy") != std::string::npos,
                   "saved config persists the safe new pipeline mode");
        expectTrue(migratedText.find("aim_motion_compensation_delay_ms = 12") != std::string::npos,
                   "saved config persists the production motion compensation delay");
        expectTrue(migratedText.find("aim_motion_compensation_response_ms = 0") != std::string::npos,
                   "saved config persists the production fixed-delay response");
        expectTrue(migratedText.find("aim_shadow_command_to_frame_delay_ms = 60") != std::string::npos,
                    "saved config persists the explicit shadow delay");
        expectTrue(migratedText.find("aim_shadow_command_response_ms = 0") != std::string::npos,
                    "saved config persists the compatible shadow response width");
        expectTrue(migratedText.find("aim_shadow_feedforward_gain = 0") != std::string::npos &&
                   migratedText.find("aim_shadow_settle_error_deg = 0.08") != std::string::npos &&
                   migratedText.find("aim_shadow_settle_rate_dps = 1.2") != std::string::npos &&
                   migratedText.find("aim_shadow_reverse_confirm_ms = 80") != std::string::npos &&
                   migratedText.find("aim_shadow_vertical_catch_up_deg = 0.8") != std::string::npos &&
                   migratedText.find("aim_shadow_integral_time_ms = 0") != std::string::npos &&
                   migratedText.find("aim_shadow_lead_horizon_ms = 0") != std::string::npos &&
                   migratedText.find("aim_shadow_estimator_mode = kalman") != std::string::npos &&
                   migratedText.find("aim_shadow_ca_jerk_std_dps3 = 8000") != std::string::npos &&
                   migratedText.find("aim_shadow_maneuver_rate_threshold_dps = 12") != std::string::npos &&
                   migratedText.find("aim_shadow_maneuver_hold_ms = 120") != std::string::npos,
                   "saved config persists independently disabled P0-4A controller terms");
        expectTrue(migratedText.find("trajectory_shaper_mode = off") != std::string::npos &&
                   migratedText.find("trajectory_output_hz = 240") != std::string::npos,
                   "saved config persists the P0-4B pass-through baseline");
        expectTrue(migratedText.find("manual_control_enabled = false") != std::string::npos &&
                   migratedText.find("manual_control_enter_dps = 0.6") != std::string::npos,
                   "saved config persists manual arbitration opt-in settings");
        expectTrue(migratedText.find("predictionInterval") == std::string::npos,
                   "saved config removes legacy prediction interval key");
    }
    std::filesystem::remove(legacyPath, removeError);

    const std::filesystem::path unsafeWindowPath = "xen_config_prediction_window_test.ini";
    {
        std::ofstream unsafeWindowFile(unsafeWindowPath);
        unsafeWindowFile << "prediction_velocity_tau_ms = 15\n";
    }
    Config clampedWindow{};
    expectTrue(clampedWindow.loadConfig(unsafeWindowPath.string()),
               "previous prediction window config loads successfully");
    expectNear(clampedWindow.prediction_velocity_tau_ms, 40.0, 0.0,
               "unsafe two-frame prediction window migrates to robust minimum");
    std::filesystem::remove(unsafeWindowPath, removeError);

    const std::filesystem::path calibrationConfigPath =
        "xen_config_profile_calibration_test.ini";
    {
        std::ofstream calibrationConfigFile(calibrationConfigPath);
        calibrationConfigFile << "profile_calibration_enabled = true\n";
    }
    Config enabledCalibration{};
    expectTrue(enabledCalibration.loadConfig(calibrationConfigPath.string()),
               "passive profile calibration config loads successfully");
    expectTrue(enabledCalibration.profile_calibration_enabled,
               "passive profile calibration enabled state is restored");
    std::filesystem::remove(calibrationConfigPath, removeError);

    const std::filesystem::path machineCacheConfigPath =
        "xen_config_machine_profile_cache_test.ini";
    {
        std::ofstream machineCacheConfigFile(machineCacheConfigPath);
        machineCacheConfigFile << "machine_profile_cache_enabled = true\n"
                               << "machine_profile_cache_path = C:\\\\calibration\\\\machine.ini\n"
                               << "machine_profile_aim_mode = scope1\n";
    }
    Config machineCacheConfig{};
    expectTrue(machineCacheConfig.loadConfig(machineCacheConfigPath.string()),
               "machine profile cache config loads successfully");
    expectTrue(machineCacheConfig.machine_profile_cache_enabled,
               "machine profile cache enabled state is restored");
    expectString(machineCacheConfig.machine_profile_aim_mode, "scope1",
                 "machine profile aim mode is restored");
    std::filesystem::remove(machineCacheConfigPath, removeError);

    const std::filesystem::path shadowModePath = "xen_config_shadow_mode_test.ini";
    {
        std::ofstream shadowModeFile(shadowModePath);
        shadowModeFile << "aim_pipeline_mode = shadow\n";
    }
    Config shadowMode{};
    expectTrue(shadowMode.loadConfig(shadowModePath.string()),
               "shadow pipeline config loads successfully");
    expectString(shadowMode.aim_pipeline_mode, "shadow",
                 "shadow pipeline config state is restored");
    std::filesystem::remove(shadowModePath, removeError);

    const std::filesystem::path invalidPipelineModePath =
        "xen_config_invalid_pipeline_mode_test.ini";
    {
        std::ofstream invalidPipelineModeFile(invalidPipelineModePath);
        invalidPipelineModeFile << "aim_pipeline_mode = typo\n";
    }
    Config invalidPipelineMode{};
    expectTrue(invalidPipelineMode.loadConfig(invalidPipelineModePath.string()),
               "invalid pipeline mode config loads safely");
    expectString(invalidPipelineMode.aim_pipeline_mode, "legacy",
                 "invalid pipeline mode falls back to the formal legacy chain");
    std::filesystem::remove(invalidPipelineModePath, removeError);

    const std::filesystem::path unsafeDelayPath = "xen_config_shadow_delay_test.ini";
    {
        std::ofstream unsafeDelayFile(unsafeDelayPath);
        unsafeDelayFile << "aim_shadow_command_to_frame_delay_ms = 999\n";
    }
    Config clampedDelay{};
    expectTrue(clampedDelay.loadConfig(unsafeDelayPath.string()),
               "shadow delay config loads successfully");
    expectNear(clampedDelay.aim_shadow_command_to_frame_delay_ms, 250.0, 0.0,
               "shadow delay remains bounded by the documented safety maximum");
    std::filesystem::remove(unsafeDelayPath, removeError);

    const std::filesystem::path unsafeResponsePath = "xen_config_shadow_response_width_test.ini";
    {
        std::ofstream unsafeResponseFile(unsafeResponsePath);
        unsafeResponseFile << "aim_shadow_command_response_ms = 999\n";
    }
    Config clampedResponse{};
    expectTrue(clampedResponse.loadConfig(unsafeResponsePath.string()),
               "shadow response width config loads successfully");
    expectNear(clampedResponse.aim_shadow_command_response_ms, 100.0, 0.0,
               "shadow response width remains bounded by the documented safety maximum");
    std::filesystem::remove(unsafeResponsePath, removeError);

    const std::filesystem::path unsafeMotionCompensationDelayPath =
        "xen_config_motion_compensation_delay_test.ini";
    {
        std::ofstream unsafeDelayFile(unsafeMotionCompensationDelayPath);
        unsafeDelayFile << "aim_motion_compensation_delay_ms = 999\n";
    }
    Config clampedMotionCompensationDelay{};
    expectTrue(clampedMotionCompensationDelay.loadConfig(
                   unsafeMotionCompensationDelayPath.string()),
               "production motion compensation delay config loads successfully");
    expectNear(clampedMotionCompensationDelay.aim_motion_compensation_delay_ms, 250.0, 0.0,
               "production motion compensation delay remains within the safety bound");
    std::filesystem::remove(unsafeMotionCompensationDelayPath, removeError);

    const std::filesystem::path unsafeMotionCompensationResponsePath =
        "xen_config_motion_compensation_response_test.ini";
    {
        std::ofstream unsafeResponseFile(unsafeMotionCompensationResponsePath);
        unsafeResponseFile << "aim_motion_compensation_response_ms = 999\n";
    }
    Config clampedMotionCompensationResponse{};
    expectTrue(clampedMotionCompensationResponse.loadConfig(
                   unsafeMotionCompensationResponsePath.string()),
               "production motion compensation response config loads successfully");
    expectNear(clampedMotionCompensationResponse.aim_motion_compensation_response_ms,
               100.0, 0.0,
               "production motion compensation response remains within the safety bound");
    std::filesystem::remove(unsafeMotionCompensationResponsePath, removeError);

    const std::filesystem::path unsafeShadowControllerPath =
        "xen_config_shadow_controller_test.ini";
    {
        std::ofstream unsafeShadowControllerFile(unsafeShadowControllerPath);
        unsafeShadowControllerFile
            << "aim_shadow_response_ms = 1\n"
            << "aim_shadow_max_speed_cps = 9999\n"
            << "aim_shadow_feedforward_gain = 9\n"
            << "aim_shadow_settle_error_deg = 9\n"
            << "aim_shadow_settle_rate_dps = 99\n"
            << "aim_shadow_reverse_confirm_ms = 999\n"
            << "aim_shadow_vertical_catch_up_deg = 99\n"
            << "aim_shadow_integral_time_ms = 1\n"
            << "aim_shadow_integral_zone_deg = 99\n"
            << "aim_shadow_lead_horizon_ms = 999\n"
            << "aim_shadow_lead_strength = 9\n"
            << "aim_shadow_estimator_mode = typo\n"
            << "aim_shadow_ca_jerk_std_dps3 = 999999\n"
            << "aim_shadow_maneuver_rate_threshold_dps = 0\n"
            << "aim_shadow_maneuver_hold_ms = 9999\n";
    }
    Config clampedShadowController{};
    expectTrue(clampedShadowController.loadConfig(unsafeShadowControllerPath.string()),
               "shadow angle controller config loads successfully");
    expectNear(clampedShadowController.aim_shadow_response_ms, 10.0, 0.0,
               "shadow response uses its documented lower bound");
    expectNear(clampedShadowController.aim_shadow_max_speed_cps, 4000.0, 0.0,
               "shadow speed uses the device safety bound");
    expectNear(clampedShadowController.aim_shadow_feedforward_gain, 2.0, 0.0,
               "shadow feedforward gain remains bounded");
    expectNear(clampedShadowController.aim_shadow_settle_error_deg, 1.0, 0.0,
               "shadow settle error remains bounded");
    expectNear(clampedShadowController.aim_shadow_settle_rate_dps, 20.0, 0.0,
               "shadow settle rate remains bounded");
    expectNear(clampedShadowController.aim_shadow_reverse_confirm_ms, 250.0, 0.0,
               "shadow low-speed reverse confirmation remains bounded");
    expectNear(clampedShadowController.aim_shadow_vertical_catch_up_deg, 20.0, 0.0,
               "shadow vertical catch-up threshold remains bounded");
    expectNear(clampedShadowController.aim_shadow_integral_time_ms, 50.0, 0.0,
               "nonzero shadow integral time uses a stable minimum");
    expectNear(clampedShadowController.aim_shadow_integral_zone_deg, 10.0, 0.0,
               "shadow integral zone remains bounded");
    expectNear(clampedShadowController.aim_shadow_lead_horizon_ms, 250.0, 0.0,
               "shadow experience lead horizon remains bounded");
    expectNear(clampedShadowController.aim_shadow_lead_strength, 4.0, 0.0,
               "shadow experience lead strength remains bounded");
    expectString(clampedShadowController.aim_shadow_estimator_mode, "kalman",
                 "unknown shadow estimator safely falls back to frozen Kalman");
    expectNear(clampedShadowController.aim_shadow_ca_jerk_std_dps3, 100000.0, 0.0,
               "shadow acceleration jerk noise remains bounded");
    expectNear(clampedShadowController.aim_shadow_maneuver_rate_threshold_dps, 0.1, 1e-6,
               "shadow maneuver threshold remains positive");
    expectNear(clampedShadowController.aim_shadow_maneuver_hold_ms, 1000.0, 0.0,
               "shadow maneuver hold remains bounded");
    std::filesystem::remove(unsafeShadowControllerPath, removeError);

    const std::filesystem::path unsafeTrajectoryPath =
        "xen_config_trajectory_shaper_test.ini";
    {
        std::ofstream unsafeTrajectoryFile(unsafeTrajectoryPath);
        unsafeTrajectoryFile
            << "trajectory_shaper_mode = ruckig\n"
            << "trajectory_output_hz = 9999\n"
            << "trajectory_max_velocity_cps = 9999\n"
            << "trajectory_max_acceleration_cps2 = 1\n"
            << "trajectory_max_jerk_cps3 = 1\n";
    }
    Config clampedTrajectory{};
    expectTrue(clampedTrajectory.loadConfig(unsafeTrajectoryPath.string()),
               "trajectory shaper config loads successfully");
    expectString(clampedTrajectory.trajectory_shaper_mode, "off",
                 "unavailable Ruckig mode fails safely to pass-through");
    expectNear(clampedTrajectory.trajectory_output_hz, 1000.0, 0.0,
               "trajectory output rate remains bounded");
    expectNear(clampedTrajectory.trajectory_max_velocity_cps, 4000.0, 0.0,
               "trajectory speed remains bounded by the device safety maximum");
    expectNear(clampedTrajectory.trajectory_max_acceleration_cps2, 1000.0, 0.0,
               "trajectory acceleration uses a nonzero engineering minimum");
    expectNear(clampedTrajectory.trajectory_max_jerk_cps3, 10000.0, 0.0,
               "trajectory jerk uses a nonzero engineering minimum");
    std::filesystem::remove(unsafeTrajectoryPath, removeError);

    const std::filesystem::path speedLimitPath = "xen_config_speed_limit_test.ini";
    {
        std::ofstream speedLimitFile(speedLimitPath);
        speedLimitFile << "move_max_speed_cps = 5000\n";
    }
    Config clampedSpeed{};
    expectTrue(clampedSpeed.loadConfig(speedLimitPath.string()),
               "expanded movement speed config loads successfully");
    expectNear(clampedSpeed.move_max_speed_cps, 4000.0, 0.0,
               "movement speed remains bounded by the documented safety maximum");
    std::filesystem::remove(speedLimitPath, removeError);

    const std::filesystem::path catchUpLimitPath =
        "xen_config_catch_up_speed_limit_test.ini";
    {
        std::ofstream catchUpLimitFile(catchUpLimitPath);
        catchUpLimitFile << "move_max_speed_cps = 3600\n"
                         << "move_catch_up_max_speed_cps = 2000\n";
    }
    Config clampedCatchUpLow{};
    expectTrue(clampedCatchUpLow.loadConfig(catchUpLimitPath.string()),
               "conditional catch-up speed config loads successfully");
    expectNear(clampedCatchUpLow.move_catch_up_max_speed_cps, 3600.0, 0.0,
               "conditional catch-up speed cannot fall below the base limit");
    {
        std::ofstream catchUpLimitFile(catchUpLimitPath);
        catchUpLimitFile << "move_max_speed_cps = 3200\n"
                         << "move_catch_up_max_speed_cps = 5000\n";
    }
    Config clampedCatchUpHigh{};
    expectTrue(clampedCatchUpHigh.loadConfig(catchUpLimitPath.string()),
               "oversized conditional catch-up speed config loads successfully");
    expectNear(clampedCatchUpHigh.move_catch_up_max_speed_cps, 4000.0, 0.0,
               "conditional catch-up speed remains bounded by the device safety maximum");
    std::filesystem::remove(catchUpLimitPath, removeError);

    if (failures != 0)
    {
        std::cerr << failures << " config test(s) failed\n";
        return 1;
    }

    std::cout << "config tests passed\n";
    return 0;
}
