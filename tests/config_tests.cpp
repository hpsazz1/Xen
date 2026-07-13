#include "config/config.h"

#include <cmath>
#include <iostream>
#include <mutex>

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
}

int main()
{
    Config config{};
    config.auto_derive_tracker_params = true;
    config.move_response_ms = 73.0f;
    config.move_max_speed_cps = 1337.0f;

    config.applyAutoDerivedTrackerParams(320, 240);
    expectNear(config.move_response_ms, 73.0, 0.0,
               "auto derive preserves configured response time");
    expectNear(config.move_max_speed_cps, 1337.0, 0.0,
               "auto derive preserves configured maximum speed");
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
    expectNear(config.ml_termination_frames, 62.0, 0.0,
               "runtime re-derive applies updated fps to tracker");
    expectNear(config.ml_measurement_stddev, 5.0, 1e-6,
               "runtime re-derive applies updated resolution to tracker");

    if (failures != 0)
    {
        std::cerr << failures << " config test(s) failed\n";
        return 1;
    }

    std::cout << "config tests passed\n";
    return 0;
}
