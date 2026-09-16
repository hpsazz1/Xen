#ifndef RECOIL_CONFIG_H
#define RECOIL_CONFIG_H
#include <string>
#include "recoil/recoil.h"

// 常用强度属于编辑草稿；Runtime只读取活动索引中武器、输入路径与灵敏度匹配的已校准版本。
struct RecoilConfig {
    bool enabled = false;
    bool mixed_aim = false;
    int hold_virtual_key = 0;
    std::string profile_directory = "cache/recoil/profiles";
    // 兼容既有配置与校准归档的可选元数据，不参与生产曲线匹配。
    std::string game_build, conditions;
    std::string input_path = "kmbox_net";
    double sensitivity = 0;
    std::string fire_mode = "automatic";
    // 同一真实时间窗口内Aim和压枪共同消费物理额度，不按worker tick补满。
    int budget_window_ms = 16;
    int max_observation_age_ms = 50;
};
#endif
