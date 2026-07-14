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
    config.move_integral_time_ms = 333.0f;

    config.applyAutoDerivedTrackerParams(320, 240);
    expectNear(config.move_response_ms, 73.0, 0.0,
               "auto derive preserves configured response time");
    expectNear(config.move_max_speed_cps, 1337.0, 0.0,
               "auto derive preserves configured maximum speed");
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
        expectNear(defaults.move_max_speed_cps, 1440.0, 0.0,
                   "default maximum speed uses four-chain nine-grid result");
        expectNear(defaults.move_integral_time_ms, 0.0, 0.0,
                   "moving integral remains disabled before field validation");
        expectNear(defaults.prediction_lead_ms, 50.0, 0.0,
                   "default prediction uses kinematic replay lead");
        expectNear(defaults.prediction_velocity_tau_ms, 50.0, 0.0,
                   "default prediction uses robust velocity regression window");
        expectNear(defaults.prediction_strength, 1.0, 0.0,
                   "default prediction uses bounded constant-velocity strength");
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

    if (failures != 0)
    {
        std::cerr << failures << " config test(s) failed\n";
        return 1;
    }

    std::cout << "config tests passed\n";
    return 0;
}
