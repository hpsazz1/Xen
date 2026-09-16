#include "config/config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    auto path = std::filesystem::temp_directory_path() / ("xen-recoil-config-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    try {
        AppConfig config; std::string error;
        config.recoil.enabled = true;
        check(!validate_app_config(config, error), "缺GSI与场景不能启用");
        config.gsi.enabled = true;
        config.source_context.enabled = true; config.source_context.host = "127.0.0.1";
        config.source_context.port = 5014; config.source_context.process_name = "synthetic.exe";
        config.recoil.sensitivity = 1.25; config.recoil.game_build = "synthetic"; config.recoil.conditions = "synthetic_standing";
        check(validate_app_config(config, error), error.c_str());
        auto missing = config;
        missing.recoil.game_build.clear(); missing.recoil.conditions.clear();
        check(!validate_app_config(missing, error) && error.find("游戏版本") != std::string::npos &&
            error.find("适用条件") != std::string::npos && error.find("GSI") == std::string::npos &&
            error.find("源焦点") == std::string::npos,
            "已配置GSI和源焦点时仅指出实际缺少的游戏版本与适用条件");
        check(save_app_config(path.string(), config, error), error.c_str());
        AppConfig loaded;
        check(load_app_config(path.string(), loaded, error), error.c_str());
        check(loaded.recoil.enabled && loaded.gsi.enabled && loaded.recoil.sensitivity == 1.25 &&
            loaded.recoil.conditions == config.recoil.conditions, "压枪配置无GSI令牌往返");
        std::ifstream input(path); std::string text((std::istreambuf_iterator<char>(input)), {}); input.close();
        check(text.find("XEN_GSI_TOKEN") == std::string::npos, "配置不生成GSI环境变量要求");
        config.recoil.hold_virtual_key = 0x23;
        check(validate_app_config(config, error), "生产压枪忽略校准链的额外许可键");
        { std::ofstream old(path); old << "[recoil]\nhold_virtual_key=invalid\n"; }
        check(load_app_config(path.string(), loaded, error) && loaded.recoil.hold_virtual_key == 0,
            "旧额外许可不解析且归零");
        check(save_app_config(path.string(), loaded, error), error.c_str());
        { std::ifstream migrated(path); std::string bytes((std::istreambuf_iterator<char>(migrated)), {});
          const auto section = bytes.find("[recoil]");
          const auto end = bytes.find('[', section + 1);
          check(section != std::string::npos && bytes.substr(section, end - section).find("hold_virtual_key") == std::string::npos,
              "保存移除压枪额外许可"); }
        config.recoil.hold_virtual_key = 0; config.recoil.use_trial = true;
        check(!validate_app_config(config, error), "试验模式需要明确独立引用");
        { std::ofstream old(path); old << "[recoil]\nenabled=invalid\n"; }
        check(!load_app_config(path.string(), loaded, error), "坏类型不能默默转换");
        { std::ofstream old(path); old << "[ui]\nwidth=900\n"; }
        check(load_app_config(path.string(), loaded, error) && !loaded.recoil.enabled && !loaded.gsi.enabled, "旧配置关闭新模块");
        std::filesystem::remove(path);
        std::cout << "recoil_config_tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
