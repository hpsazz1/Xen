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
        config.gsi.enabled = true; config.gsi.expected_player_id = "76561198000000000";
        config.gsi.token = "SECRET_NOT_FOR_DISK_01234567890123456789";
        config.source_context.enabled = true; config.source_context.host = "127.0.0.1";
        config.source_context.port = 5014; config.source_context.process_name = "synthetic.exe";
        config.recoil.sensitivity = 1.25; config.recoil.game_build = "synthetic"; config.recoil.conditions = "synthetic_standing";
        check(validate_app_config(config, error), error.c_str());
        check(save_app_config(path.string(), config, error), error.c_str());
        AppConfig loaded;
        check(load_app_config(path.string(), loaded, error), error.c_str());
        check(loaded.recoil.enabled && loaded.gsi.enabled && loaded.recoil.sensitivity == 1.25 &&
            loaded.recoil.conditions == config.recoil.conditions && loaded.gsi.token.empty(), "压枪往返与认证不落盘");
        std::ifstream input(path); std::string text((std::istreambuf_iterator<char>(input)), {}); input.close();
        check(text.find(config.gsi.token) == std::string::npos, "秘密不得出现在文件");
        config.recoil.hold_virtual_key = 0x23;
        check(!validate_app_config(config, error), "End不能作压枪键");
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
