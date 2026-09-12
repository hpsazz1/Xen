#ifndef RECOIL_CALIBRATION_IO_H
#define RECOIL_CALIBRATION_IO_H
#include "recoil/recoil_calibration.h"
#include "config/config.h"
#include <filesystem>

struct RecoilCalibrationPrepareRequest {
    std::filesystem::path profile_path, config_path, output_directory, executable_path;
    RecoilCalibrationEnvironment environment;
    RecoilCalibrationLimits limits;
    int hold_virtual_key = 0, cancel_virtual_key = 0;
};
struct RecoilCalibrationPrepared {
    RecoilCalibrationManifest manifest;
    std::shared_ptr<const RecoilProfile> profile;
    AppConfig config;
    std::filesystem::path directory;
    std::string manifest_sha256, confirmation, launch_command;
};
// 纯离线准备服务，可由UI调用；不创建Mouse、不启动网络，不写生产配置。
bool prepare_recoil_calibration(const RecoilCalibrationPrepareRequest&,
    RecoilCalibrationPrepared&, std::string& error) noexcept;
bool load_recoil_calibration_prepared(const std::filesystem::path&,
    RecoilCalibrationPrepared&, std::string& error) noexcept;
bool load_recoil_calibration_request(const std::filesystem::path&,
    RecoilCalibrationPrepareRequest&, std::string& error) noexcept;
// 两项确认均通过且不可变快照/当前配置仍匹配才返回；未创建任何设备对象。
bool verify_recoil_calibration_launch(const std::filesystem::path&, bool allow_physical_output,
    const std::string& confirmation, RecoilCalibrationPrepared&, std::string& error) noexcept;
// 创建式永久消费标记；重复/进程崩溃不可自动续跑。只在launch预检后使用。
bool consume_recoil_calibration_session(const RecoilCalibrationPrepared&, std::string& error) noexcept;
bool write_recoil_calibration_result(const RecoilCalibrationPrepared&, const std::string& termination,
    const RecoilCalibrationBudgetSnapshot&, const std::string& archive_status, std::string& error) noexcept;
#endif
