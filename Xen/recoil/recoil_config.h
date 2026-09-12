#ifndef RECOIL_CONFIG_H
#define RECOIL_CONFIG_H
#include <string>
#include "recoil/recoil.h"

// 常用强度属于编辑草稿；Runtime只读取已保存且校准条件一致的完整版本。
struct RecoilConfig {
    bool enabled = false;
    bool mixed_aim = false;
    int hold_virtual_key = 0;
    std::string profile_directory = "cache/recoil/profiles";
    std::string game_build, conditions;
    std::string input_path = "kmbox_net";
    double sensitivity = 0;
    std::string fire_mode = "automatic";
    // 已校准固定版本覆盖，与活动索引分开。沿用旧INI键名；保存后持续有效，不是一次Run许可。
    bool use_trial = false;
    std::string trial_file;
    // 同一真实时间窗口内Aim和压枪共同消费物理额度，不按worker tick补满。
    int budget_window_ms = 16;
    int max_observation_age_ms = 50;
};
#endif
