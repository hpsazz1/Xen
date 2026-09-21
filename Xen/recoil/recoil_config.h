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
    // 历史配置/归档兼容字段；不再对Aim逐帧输出追加滚动累计额度。
    int budget_window_ms = 16;
    // 历史归档兼容字段，普通Recoil不再据此读取或判断视觉许可。
    int max_observation_age_ms = 50;
};
#endif
