#include "config/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] bool valid() const {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::filesystem::path make_temp_test_directory(const char* name) {
    const auto base = std::filesystem::temp_directory_path();
    const std::string prefix =
        std::string("xen_config_") + name + "_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto path = base / (prefix + "_" + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) return path;
    }
    return {};
}

bool write_file_bytes(const std::filesystem::path& path,
                      const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

bool directory_contains_only(const std::filesystem::path& directory,
                             const std::filesystem::path& expected) {
    std::error_code error;
    std::size_t entry_count = 0;
    bool found_expected = false;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        ++entry_count;
        found_expected = found_expected || iterator->path() == expected;
    }
    return !error && entry_count == 1 && found_expected;
}

void test_utf8_config_path() {
    const auto directory = make_temp_test_directory("utf8");
    expect(!directory.empty(), "应创建 UTF-8 路径测试目录");
    if (directory.empty()) return;
    const auto nested = directory / std::filesystem::path(u8"发行版_\U0001F680");
    std::filesystem::create_directory(nested);
    const auto path = nested / std::filesystem::path(u8"默认配置_\U0001F9EA.ini");
    const auto encoded = path.u8string();
    const std::string utf8_path(encoded.begin(), encoded.end());
    AppConfig config;
    std::string error;
    bool created = false;
    expect(load_or_create_app_config(utf8_path, config, created, error) &&
               created && std::filesystem::is_regular_file(path),
           "中文及非系统代码页路径应在准确位置创建配置");
    config.detector.backend = BackendType::CPU;
    expect(save_app_config(utf8_path, config, error),
           "UTF-8 配置路径应支持原子保存");
    AppConfig loaded;
    expect(load_app_config(utf8_path, loaded, error) &&
               loaded.detector.backend == BackendType::CPU,
           "UTF-8 配置路径应读取已保存值");
    created = true;
    expect(load_or_create_app_config(utf8_path, loaded, created, error) && !created,
           "UTF-8 已有配置不得误判为缺失");

    const auto before = read_file_bytes(path);
    {
        ScopedHandle locked(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        expect(locked.valid(), "应锁住 UTF-8 配置以检查原子失败保护");
        if (locked.valid()) {
            config.detector.backend = BackendType::DIRECTML;
            expect(!save_app_config(utf8_path, config, error),
                   "UTF-8 目标拒绝替换时应明确失败");
        }
    }
    expect(read_file_bytes(path) == before && directory_contains_only(nested, path),
           "UTF-8 保存失败应保留原文件且清理临时文件");
    const std::string malformed = "[movement]\nenabled=invalid\n";
    expect(write_file_bytes(path, malformed), "应写入 UTF-8 路径下的无效配置");
    created = true;
    expect(!load_or_create_app_config(utf8_path, loaded, created, error) &&
               !created && read_file_bytes(path) == malformed,
           "UTF-8 路径下无效已有文件不得被默认配置覆盖");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_current_code_defaults() {
    const AppConfig config;
    expect(config.detector.model_path == "14wv11.onnx" &&
               config.detector.backend == BackendType::CPU &&
               config.detector.openvino_device == OpenVinoDevice::CPU &&
               !config.detector.enable_fp16 &&
               config.detector.enable_trt_cuda_graph &&
               config.detector.enable_gpu_preprocess,
           "代码 Detector 默认值应使用便携 CPU 入口");
    expect(config.capture.backend == CaptureBackend::DESKTOP_DUPLICATION &&
               config.capture.udp_url.empty() &&
               config.capture.ndi_source_name.empty() &&
               config.capture.ndi_discovery_timeout_ms == 5000 &&
               config.capture.ndi_clock_sync_url.empty() &&
               config.capture.ndi_frame_layout ==
                   NetworkFrameLayout::FULL_FRAME_1_TO_1 &&
               config.capture.ndi_source_width == 0 &&
               config.capture.ndi_source_height == 0 &&
               config.capture.roi_width == 320 &&
               config.capture.roi_height == 320,
           "代码 Capture 默认值应使用本机桌面且不绑定源机器");
    expect(config.aim.person_class_ids == std::vector<int>({0, 2}) &&
               config.aim.head_class_ids == std::vector<int>({1, 3}) &&
               config.aim.smoothing == 0.475f &&
               config.aim.counts_per_pixel_x == 0.425f &&
               config.aim.counts_per_pixel_y == 0.40f &&
               config.aim.body_aim_height_ratio == 0.16f &&
               config.aim.soft_zone_radius_percent == 30.0f &&
               config.recoil.sensitivity == 1.4 &&
               config.recoil.mixed_aim && config.trigger.random_timing_enabled &&
               config.trigger.require_stop && config.trigger.allow_estimated_stop &&
               config.aim.max_counts_per_frame == 14.0f &&
               config.aim.enable_delay_compensation &&
               config.aim.control_delay_ms == 15.0f &&
               config.aim.max_delay_compensation_ms == 44.0f &&
               !config.aim.enable_prediction &&
               config.aim.max_prediction_lead_percent == 35.0f &&
               config.aim.predicted_gain == 0.50f,
           "代码 Aim 默认值应匹配已接受的分轴 tracking 配置");
    expect(config.mouse.backend == MouseBackend::WIN32_SEND_INPUT &&
               !config.mouse.allow_send_input &&
               config.mouse.kmbox_ip.empty() &&
               config.mouse.kmbox_port == 0 &&
               config.mouse.kmbox_uuid.empty() && !config.trigger.fire_enabled,
           "代码 Mouse 默认值不绑定设备且保持物理输出禁用");
    expect(config.log.global_level == LogLevel::INFO &&
               !config.ui.open_detached_preview_on_start,
           "代码 Log/UI 默认值应匹配当前配置");
}

void test_removed_observe_only_control_config() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_removed_observe_only_control.ini";
    std::string error;
    expect(save_app_config(path.string(), source, error),
           "默认配置应成功写入: " + error);
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    expect(text.find("allow_observe_only_control") == std::string::npos,
           "已删除的 Observe-only 控制接口不得继续写入配置");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_load_or_create_default_config() {
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_generated_default_config.ini";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    AppConfig config;
    bool created = false;
    std::string error;
    expect(load_or_create_app_config(path.string(), config, created, error) &&
               created && std::filesystem::is_regular_file(path),
           "config.ini 缺失时应写出完整代码默认配置: " + error);

    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.detector.backend == BackendType::CPU &&
               loaded.capture.backend == CaptureBackend::DESKTOP_DUPLICATION &&
               !loaded.aim.enable_prediction &&
               loaded.aim.enable_delay_compensation &&
               loaded.aim.smoothing == 0.475f &&
               loaded.aim.counts_per_pixel_x == 0.425f &&
               loaded.aim.counts_per_pixel_y == 0.40f &&
               loaded.aim.body_aim_height_ratio == 0.16f &&
               loaded.aim.soft_zone_radius_percent == 30.0f &&
               loaded.recoil.sensitivity == 1.4 &&
               loaded.recoil.mixed_aim && loaded.trigger.random_timing_enabled &&
               loaded.trigger.require_stop && loaded.trigger.allow_estimated_stop &&
               loaded.aim.max_counts_per_frame == 14.0f &&
               loaded.aim.control_delay_ms == 15.0f &&
               loaded.mouse.backend == MouseBackend::WIN32_SEND_INPUT &&
               !loaded.mouse.allow_send_input &&
               !loaded.movement.enabled && loaded.movement.spin_enabled &&
               loaded.movement.large_enabled && loaded.movement.large_ctrl_enabled &&
               loaded.movement.spin_angle_degrees == 90 &&
               loaded.movement.spin_duration_ms == 200 &&
               loaded.movement.large_angle_degrees == 25 &&
               loaded.movement.large_duration_ms == 170 &&
               loaded.movement.large_ctrl_delay_ms == 650 &&
               loaded.movement.large_ctrl_hold_ms == 200 &&
               loaded.movement.trigger_guard_ms == 150 &&
               loaded.movement.sensitivity == 1.4 &&
               loaded.movement.yaw_degrees_per_count == 0.022 &&
               loaded.movement.report_mode == movement::ReportMode::RELATIVE_DELTA,
           "生成的默认配置应可回读且不得开启物理输出: " + error);

    const std::string invalid_text =
        "[detector]\ninput_width=-1\ninput_height=-1\n";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << invalid_text;
    }
    created = true;
    expect(!load_or_create_app_config(path.string(), config, created, error) &&
               !created,
           "已有但无效的 config.ini 应报错且不得按默认值覆盖");
    std::ifstream input(path, std::ios::binary);
    const std::string actual{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    expect(actual == invalid_text,
           "已有但无效的 config.ini 内容必须保持不变");
    input.close();
    std::filesystem::remove(path, ignored);
}

void test_round_trip() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    source.detector.backend = BackendType::TENSORRT;
    source.detector.openvino_device = OpenVinoDevice::NPU;
    source.detector.enable_gpu_preprocess = false;
    source.capture.roi_width = 416;
    source.capture.roi_height = 416;
    source.capture.backend = CaptureBackend::UDP_MJPEG;
    source.capture.udp_url = "udp://127.0.0.1:5500";
    source.capture.udp_read_timeout_ms = 300;
    source.capture.udp_disconnect_timeout_ms = 2500;
    source.capture.udp_frame_layout =
        UdpFrameLayout::CENTER_CROP_1_TO_1;
    source.capture.udp_source_width = 2560;
    source.capture.udp_source_height = 1440;
    source.capture.ndi_source_name = "HOST (Xen ROI)";
    source.capture.ndi_discovery_timeout_ms = 4500;
    source.capture.ndi_receive_timeout_ms = 40;
    source.capture.ndi_disconnect_timeout_ms = 1800;
    source.capture.ndi_clock_sync_url = "udp://127.0.0.1:5011";
    source.capture.ndi_clock_sync_interval_ms = 300;
    source.capture.ndi_clock_sync_timeout_ms = 150;
    source.capture.ndi_clock_mapping_max_age_ms = 1200;
    source.capture.ndi_frame_layout =
        NetworkFrameLayout::CENTER_CROP_1_TO_1;
    source.capture.ndi_source_width = 2560;
    source.capture.ndi_source_height = 1440;
    source.capture.ndi_require_frame_metadata = false;
    source.aim.person_class_ids = {0, 2};
    source.aim.head_class_ids = {1, 3};
    source.aim.acquisition_range_percent = 110.0f;
    source.aim.body_aim_range_percent = 62.0f;
    source.aim.enable_prediction = true;
    source.aim.enable_delay_compensation = true;
    source.aim.control_delay_ms = 7.5f;
    source.aim.max_delay_compensation_ms = 18.0f;
    source.aim.max_delay_compensation_percent = 12.0f;
    source.aim.max_prediction_lead_percent = 18.0f;
    source.mouse.backend = MouseBackend::KMBOX_NET;
    source.mouse.allow_send_input = true;
    source.mouse.kmbox_ip = "127.0.0.1";
    source.mouse.kmbox_port = 6234;
    source.mouse.kmbox_uuid = "A1b2C3d4";
    source.mouse.kmbox_connect_timeout_ms = 900;
    source.mouse.kmbox_command_timeout_ms = 250;
    source.mouse.makcu_port = "COM17";
    source.mouse.makcu_baud_rate = 4000000;
    source.mouse.makcu_connect_timeout_ms = 800;
    source.mouse.makcu_command_timeout_ms = 120;
    source.keyboard.aim_hold_virtual_keys = {0x02, 0x05};
    source.keyboard.emergency_virtual_keys = {0x23, 0x06};
    source.keyboard.runtime_toggle_virtual_keys = {0x77, 0x04};
    source.keyboard.debug_test_enabled = true;
    source.keyboard.debug_test_virtual_keys = {0x79,0x7A};
    source.keyboard.anomaly_mark_virtual_keys = {0x78};
    source.keyboard.lineup_locate_virtual_key = 0x7B;
    source.keyboard.lineup_throw_virtual_key = 0x7C;
    source.lineup.calibration_file = "calibration/lineup-measured.json";
    source.lineup.calibration_context = "fixture-game-conditions";
    source.log.global_level = LogLevel::WARN;
    source.log.enable_console = false;
    source.log.enable_file = false;
    source.log.enable_debug_file = true;
    source.log.enable_ringbuf = true;
    source.log.ringbuf_capacity = 2048;
    source.log.log_dir = "cache/test-logs";
    source.log.file_max_size_mb = 4;
    source.log.file_max_count = 5;
    source.log.module_levels = {
        {"detector", LogLevel::DEBUG},
        {"capture", LogLevel::WARN},
    };
    source.ui.width = 1024;
    source.ui.open_detached_preview_on_start = true;
    source.ui.theme = UiTheme::DARK;

    const auto path = std::filesystem::temp_directory_path() /
                      "xen_config_round_trip.ini";
    std::string error;
    expect(save_app_config(path.string(), source, error),
           "有效配置应成功写入: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error),
           "写入后的配置应成功读取: " + error);
    expect(loaded.detector.backend == BackendType::TENSORRT &&
           loaded.detector.openvino_device == OpenVinoDevice::NPU &&
           !loaded.detector.enable_gpu_preprocess &&
           loaded.capture.roi_width == 416 &&
           loaded.capture.backend == CaptureBackend::UDP_MJPEG &&
           loaded.capture.udp_url == source.capture.udp_url &&
           loaded.capture.udp_read_timeout_ms == 300 &&
           loaded.capture.udp_disconnect_timeout_ms == 2500 &&
           loaded.capture.udp_frame_layout ==
               UdpFrameLayout::CENTER_CROP_1_TO_1 &&
           loaded.capture.udp_source_width == 2560 &&
           loaded.capture.udp_source_height == 1440 &&
           loaded.capture.ndi_source_name == source.capture.ndi_source_name &&
           loaded.capture.ndi_discovery_timeout_ms == 4500 &&
           loaded.capture.ndi_receive_timeout_ms == 40 &&
           loaded.capture.ndi_disconnect_timeout_ms == 1800 &&
           loaded.capture.ndi_clock_sync_url ==
               source.capture.ndi_clock_sync_url &&
           loaded.capture.ndi_clock_sync_interval_ms == 300 &&
           loaded.capture.ndi_clock_sync_timeout_ms == 150 &&
           loaded.capture.ndi_clock_mapping_max_age_ms == 1200 &&
           loaded.capture.ndi_frame_layout ==
               NetworkFrameLayout::CENTER_CROP_1_TO_1 &&
           loaded.capture.ndi_source_width == 2560 &&
           loaded.capture.ndi_source_height == 1440 &&
           !loaded.capture.ndi_require_frame_metadata &&
           loaded.aim.person_class_ids == source.aim.person_class_ids &&
           loaded.aim.acquisition_range_percent == 110.0f &&
           loaded.aim.body_aim_range_percent == 62.0f &&
           loaded.aim.enable_prediction &&
           loaded.aim.enable_delay_compensation &&
           loaded.aim.control_delay_ms == 7.5f &&
           loaded.aim.max_delay_compensation_ms == 18.0f &&
           loaded.aim.max_delay_compensation_percent == 12.0f &&
           loaded.aim.max_prediction_lead_percent == 18.0f &&
           loaded.mouse.backend == MouseBackend::KMBOX_NET &&
           loaded.mouse.allow_send_input &&
           loaded.mouse.kmbox_ip == "127.0.0.1" &&
           loaded.mouse.kmbox_port == 6234 &&
           loaded.mouse.kmbox_uuid == "A1b2C3d4" &&
           loaded.mouse.kmbox_connect_timeout_ms == 900 &&
           loaded.mouse.kmbox_command_timeout_ms == 250 &&
           loaded.mouse.makcu_port == "COM17" &&
           loaded.mouse.makcu_baud_rate == 4000000 &&
           loaded.mouse.makcu_connect_timeout_ms == 800 &&
           loaded.mouse.makcu_command_timeout_ms == 120 &&
           loaded.keyboard.debug_test_enabled == source.keyboard.debug_test_enabled &&
           loaded.keyboard.debug_test_virtual_keys == source.keyboard.debug_test_virtual_keys &&
           loaded.keyboard.anomaly_mark_virtual_keys == source.keyboard.anomaly_mark_virtual_keys &&
           loaded.keyboard.lineup_locate_virtual_key == source.keyboard.lineup_locate_virtual_key &&
           loaded.keyboard.lineup_throw_virtual_key == source.keyboard.lineup_throw_virtual_key &&
           loaded.lineup.calibration_file == source.lineup.calibration_file &&
           loaded.lineup.calibration_context == source.lineup.calibration_context &&
           loaded.keyboard.aim_hold_virtual_keys ==
               source.keyboard.aim_hold_virtual_keys &&
           loaded.keyboard.emergency_virtual_keys ==
               source.keyboard.emergency_virtual_keys &&
           loaded.keyboard.runtime_toggle_virtual_keys ==
               source.keyboard.runtime_toggle_virtual_keys &&
           loaded.log.global_level == LogLevel::WARN &&
           !loaded.log.enable_console && !loaded.log.enable_file &&
           loaded.log.enable_debug_file && loaded.log.enable_ringbuf &&
           loaded.log.ringbuf_capacity == 2048 &&
           loaded.log.log_dir == "cache/test-logs" &&
           loaded.log.file_max_size_mb == 4 &&
           loaded.log.file_max_count == 5 &&
           loaded.log.module_levels == source.log.module_levels &&
           loaded.ui.width == 1024 &&
           loaded.ui.open_detached_preview_on_start &&
           loaded.ui.theme == UiTheme::DARK,
           "配置往返后关键字段必须保持一致");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_openvino_config_validation() {
    AppConfig config;
    config.detector.model_path = "models/test.onnx";
    config.detector.backend = BackendType::OPENVINO;
    config.detector.openvino_device = OpenVinoDevice::CPU;
    config.detector.device_id = 0;
    std::string error;
    expect(validate_app_config(config, error),
           "OpenVINO CPU 的设备索引 0 应通过配置校验: " + error);

    config.detector.device_id = 1;
    expect(!validate_app_config(config, error),
           "OpenVINO CPU 不得接受非零设备索引");
    config.detector.openvino_device = OpenVinoDevice::NPU;
    expect(!validate_app_config(config, error),
           "OpenVINO NPU 不得接受非零设备索引");
    config.detector.openvino_device = OpenVinoDevice::GPU;
    expect(validate_app_config(config, error),
           "OpenVINO GPU 应接受显式设备索引: " + error);
}

void test_makcu_config_round_trip() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    source.mouse.backend = MouseBackend::MAKCU;
    source.mouse.allow_send_input = true;
    source.mouse.makcu_port = "COM8";
    source.mouse.makcu_baud_rate = 4000000;
    source.mouse.makcu_connect_timeout_ms = 700;
    source.mouse.makcu_command_timeout_ms = 80;
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_makcu_config_round_trip.ini";
    std::string error;
    expect(save_app_config(path.string(), source, error),
           "MAKCU 配置应成功写入: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.mouse.backend == MouseBackend::MAKCU &&
               loaded.mouse.allow_send_input &&
               loaded.mouse.makcu_port == "COM8" &&
               loaded.mouse.makcu_baud_rate == 4000000 &&
               loaded.mouse.makcu_connect_timeout_ms == 700 &&
               loaded.mouse.makcu_command_timeout_ms == 80,
           "makcu 后端名称与串口参数必须完整往返: " + error);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_log_defaults_and_invalid_level() {
    const auto defaults_path = std::filesystem::temp_directory_path() /
                               "xen_config_without_log.ini";
    {
        std::ofstream output(defaults_path, std::ios::binary);
        output << "[detector]\nmodel_path=model.onnx\n";
    }

    AppConfig defaults;
    std::string error;
    expect(load_app_config(defaults_path.string(), defaults, error),
           "缺少 [log] 的旧配置仍应使用日志默认值");
    expect(defaults.log.global_level == LogLevel::INFO &&
               defaults.log.enable_console && defaults.log.enable_file &&
               !defaults.log.enable_debug_file && defaults.log.enable_ringbuf &&
               defaults.log.ringbuf_capacity == 1024 &&
               defaults.log.module_levels.empty(),
           "旧配置加载后的日志默认值不正确");
    std::error_code ignored;
    std::filesystem::remove(defaults_path, ignored);

    const auto invalid_path = std::filesystem::temp_directory_path() /
                              "xen_config_invalid_log_level.ini";
    {
        std::ofstream output(invalid_path, std::ios::binary);
        output << "[detector]\nmodel_path=model.onnx\n"
                  "[log]\nglobal_level=verbose\n";
    }
    AppConfig invalid;
    error.clear();
    expect(!load_app_config(invalid_path.string(), invalid, error) &&
               error.find("global_level") != std::string::npos,
           "未知日志等级必须明确拒绝并返回字段错误");
    std::filesystem::remove(invalid_path, ignored);

    const auto invalid_module_path =
        std::filesystem::temp_directory_path() /
        "xen_config_invalid_log_module_level.ini";
    {
        std::ofstream output(invalid_module_path, std::ios::binary);
        output << "[detector]\nmodel_path=model.onnx\n"
                  "[log_modules]\ndetector=verbose\n";
    }
    error.clear();
    expect(!load_app_config(invalid_module_path.string(), invalid, error) &&
               error.find("detector") != std::string::npos,
           "未知模块日志等级必须明确拒绝并返回模块名");
    std::filesystem::remove(invalid_module_path, ignored);
}

void test_log_output_levels_round_trip() {
    const auto directory = make_temp_test_directory("log_output_levels");
    expect(!directory.empty(), "日志等级往返测试临时目录必须创建成功");
    if (directory.empty()) return;
    const auto path = directory / "config.ini";
    for (const auto level : {
             LogLevel::OFF, LogLevel::ERROR, LogLevel::WARN, LogLevel::INFO}) {
        AppConfig source;
        source.log.global_level = level;
        source.log.module_levels.emplace("capture", LogLevel::WARN);
        std::string error;
        expect(save_app_config(path.string(), source, error),
               "日志输出四档必须可保存: " + error);
        AppConfig loaded;
        // 与写入值不同，避免加载失败或漏字段被默认值掩盖。
        loaded.log.global_level = level == LogLevel::OFF
            ? LogLevel::INFO : LogLevel::OFF;
        expect(load_app_config(path.string(), loaded, error) &&
                   loaded.log.global_level == level &&
                   loaded.log.enable_console == source.log.enable_console &&
                   loaded.log.enable_file == source.log.enable_file &&
                   loaded.log.enable_debug_file == source.log.enable_debug_file &&
                   loaded.log.enable_ringbuf == source.log.enable_ringbuf &&
                   loaded.log.module_levels == source.log.module_levels,
               "日志输出四档必须准确往返，OFF 不得丢失输出目的地与模块配置: " + error);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(directory, ignored);
}

void test_existing_file_rejects_malformed_typed_values() {
    struct InvalidValueCase {
        const char* name;
        const char* content;
        const char* expected_key;
    };
    const InvalidValueCase cases[]{
        {
            "enum",
            "[detector]\nmodel_path=mutated.onnx\nbackend=tensor_rt_typo\n",
            "detector.backend",
        },
        {
            "number",
            "[detector]\nmodel_path=mutated.onnx\n"
            "[runtime]\nprofile_window=not-a-number\n",
            "runtime.profile_window",
        },
        {
            "bool",
            "[detector]\nmodel_path=mutated.onnx\n"
            "[ui]\nenable_vsync=sometimes\n",
            "ui.enable_vsync",
        },
        {
            "list",
            "[detector]\nmodel_path=mutated.onnx\n"
            "[aim]\nperson_class_ids=0,typo\n",
            "aim.person_class_ids",
        },
    };

    for (const auto& test_case : cases) {
        const auto path = std::filesystem::temp_directory_path() /
            (std::string("xen_config_invalid_existing_") +
             test_case.name + ".ini");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << test_case.content;
        }

        AppConfig config;
        config.detector.model_path = "original-before-load.onnx";
        const BackendType original_backend = config.detector.backend;
        const int original_profile_window = config.runtime.profile_window;
        const bool original_vsync = config.ui.enable_vsync;
        const std::vector<int> original_person_ids =
            config.aim.person_class_ids;
        std::string error;
        expect(!load_app_config(path.string(), config, error) &&
                   error.find(test_case.expected_key) != std::string::npos &&
                   config.detector.model_path ==
                       "original-before-load.onnx" &&
                   config.detector.backend == original_backend &&
                   config.runtime.profile_window == original_profile_window &&
                   config.ui.enable_vsync == original_vsync &&
                   config.aim.person_class_ids == original_person_ids,
               std::string("已有配置的非法 typed value 必须拒绝且保持 caller config：") +
                   test_case.expected_key + "；error=" + error);

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

void test_atomic_save_preserves_existing_file_on_write_failure() {
    const auto directory = make_temp_test_directory("save_write_failure");
    expect(!directory.empty(), "应能创建配置原子保存的隔离测试目录");
    if (directory.empty()) return;

    const auto path = directory / "config.ini";
    const std::string old_bytes =
        "; old config sentinel\r\n"
        "[detector]\r\n"
        "model_path=old-model.onnx\r\n";
    expect(write_file_bytes(path, old_bytes),
           "写故障测试应能预置旧配置字节");

    AppConfig replacement;
    replacement.detector.model_path = "replacement-model.onnx";
    std::string error;
    bool saved = true;
    {
        // 允许其他读取，但拒绝普通写入以及 replace/rename 所需的
        // DELETE 共享；测试只锁定隔离目录内的 config.ini。
        ScopedHandle lock(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        expect(lock.valid(),
               "写故障测试应能锁定隔离配置，Win32 error=" +
                   std::to_string(GetLastError()));
        if (lock.valid()) {
            saved = save_app_config(path.string(), replacement, error);
        }
    }

    expect(!saved && !error.empty(),
           "目标写入被拒绝时 save_app_config 必须明确失败");
    expect(read_file_bytes(path) == old_bytes,
           "目标写入被拒绝时必须逐字节保留旧配置");
    expect(directory_contains_only(directory, path),
           "目标写入被拒绝后不得遗留临时文件");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_atomic_save_preserves_existing_file_on_replace_failure() {
    const auto directory = make_temp_test_directory("save_replace_failure");
    expect(!directory.empty(), "应能创建配置原子替换的隔离测试目录");
    if (directory.empty()) return;

    const auto path = directory / "config.ini";
    const std::string old_bytes =
        "; old config sentinel\r\n"
        "[detector]\r\n"
        "model_path=old-model.onnx\r\n";
    expect(write_file_bytes(path, old_bytes),
           "替换故障测试应能预置旧配置字节");

    AppConfig replacement;
    replacement.detector.model_path = "replacement-model.onnx";
    std::string error;
    bool saved = true;
    {
        // 零访问句柄不与 CRT 独占 fopen 的访问需求冲突；它允许
        // 普通覆盖写，但拒绝 ReplaceFileW/rename 所需的 DELETE 共享。
        // 因而旧版直接 SaveFile 会稳定暴露为红灯。
        ScopedHandle lock(CreateFileW(
            path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        expect(lock.valid(),
               "替换故障测试应能锁定隔离配置，Win32 error=" +
                   std::to_string(GetLastError()));
        if (lock.valid()) {
            saved = save_app_config(path.string(), replacement, error);
        }
    }

    expect(!saved && !error.empty(),
           "原子替换被拒绝时 save_app_config 必须明确失败");
    expect(read_file_bytes(path) == old_bytes,
           "原子替换被拒绝时必须逐字节保留旧配置");
    expect(directory_contains_only(directory, path),
           "原子替换被拒绝后不得遗留临时文件");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void configure_test_kmbox(AppConfig& config) {
    // 使用隔离假设备参数，仅测试配置，不创建网络连接。
    config.mouse.backend = MouseBackend::KMBOX_NET;
    config.mouse.kmbox_ip = "127.0.0.1";
    config.mouse.kmbox_port = 13384;
    config.mouse.kmbox_uuid = "00000000";
}

void test_auto_stop_config() {
    AppConfig config;
    configure_test_kmbox(config);
    std::string error;
    expect(!config.auto_stop.enabled && config.auto_stop.activation_virtual_key == 0,
           "自动急停默认关闭且未绑定");
    expect(config.auto_stop.release_virtual_keys == std::vector<int>{'1', '2', '3', '4', '5', 'Q'},
           "急停释放键默认数字1至5及Q，任意键触发");
    const auto path = std::filesystem::temp_directory_path() / "xen_auto_stop_config.ini";
    config.auto_stop.enabled = true;
    config.auto_stop.activation_virtual_key = 0x76;
    expect(save_app_config(path.string(), config, error), "自动急停独立节应可保存");
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) && loaded.auto_stop.enabled &&
               loaded.auto_stop.activation_virtual_key == 0x76,
           "自动急停开关和允许键必须往返保留");
    config.auto_stop.release_virtual_keys = {'6', 'E'};
    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
               loaded.auto_stop.release_virtual_keys == config.auto_stop.release_virtual_keys,
           "多个自定义急停释放键必须往返保留");
    for (const std::vector<int>& keys : {std::vector<int>{0}, {256}, {'W'}, {0x76}, {'Q', 'Q'}}) {
        config.auto_stop.release_virtual_keys = keys;
        expect(!validate_app_config(config, error), "无效、屏蔽、冲突或重复释放键必须拒绝");
    }
    config.auto_stop.release_virtual_keys.clear();
    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
               loaded.auto_stop.release_virtual_keys.empty(), "显式清空释放键不恢复默认");
    config.auto_stop.release_virtual_keys = AutoStopConfig{}.release_virtual_keys;
    // 瞄准的多个允许键可与自动急停、自动扳机同时共用；安全动作仍排他。
    config.keyboard.aim_hold_virtual_keys = {0x02, 0x05};
    for (const int key : config.keyboard.aim_hold_virtual_keys) {
        config.auto_stop.activation_virtual_key = key;
        config.trigger.hold_virtual_key = key;
        expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
            loaded.keyboard.aim_hold_virtual_keys == config.keyboard.aim_hold_virtual_keys &&
            loaded.auto_stop.activation_virtual_key == key && loaded.trigger.hold_virtual_key == key,
            "瞄准多键列表中的任意键均可与急停、扳机共用并往返保存");
    }
    config.trigger.hold_virtual_key = 0;
    for (int key : {-1, 256, int('W'), int('A'), int('S'), int('D'), 0x23, 0x77}) {
        config.auto_stop.activation_virtual_key = key;
        expect(!validate_app_config(config, error), "非法、移动或已占用允许键必须拒绝");
    }
    config.auto_stop.activation_virtual_key = 0;
    expect(validate_app_config(config, error), "允许保存尚未绑定的配置，由状态明确未绑定");
    config.mouse.makcu_port = "COM3";
    for (auto backend : {MouseBackend::WIN32_SEND_INPUT, MouseBackend::MAKCU}) {
        config.mouse.backend = backend;
        expect(!validate_app_config(config, error), "非 KMBOX 后端不能启用自动急停");
        config.auto_stop.enabled = false;
        expect(validate_app_config(config, error), "关闭自动急停时兼容其他后端");
        config.auto_stop.enabled = true;
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "[detector]\nmodel_path=model.onnx\n";
    }
    loaded.auto_stop.enabled = true;
    loaded.auto_stop.activation_virtual_key = 0x76;
    expect(load_app_config(path.string(), loaded, error) && !loaded.auto_stop.enabled &&
               loaded.auto_stop.activation_virtual_key == 0,
           "旧配置必须清除已开启的调用方状态");
    expect(loaded.auto_stop.release_virtual_keys == AutoStopConfig{}.release_virtual_keys,
           "旧配置补充默认释放键");
    for (const char* value : {"enabled=perhaps", "activation_virtual_key=1.5",
                              "activation_virtual_key=999999999999999999999999",
                              "release_virtual_keys=49,q", "release_virtual_keys=49,1.5"}) {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "[auto_stop]\n" << value << "\n";
        }
        expect(!load_app_config(path.string(), loaded, error), "自动急停节应严格检查类型");
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_trigger_and_source_context_config() {
    AppConfig config;
    configure_test_kmbox(config);
    std::string error;
    expect(!config.trigger.enabled && config.trigger.hold_virtual_key == 0 && !config.source_context.enabled,
        "扳机与源端状态默认关闭，扳机默认未绑定");
    const auto directory = make_temp_test_directory("trigger_context");
    expect(!directory.empty(), "扳机配置测试应创建隔离目录");
    if (directory.empty()) return;
    const auto path = directory / "config.ini";
    config.trigger.enabled = true;
    config.gsi.enabled = true;
    config.trigger.fire_enabled = false;
    // 本场景只验证扳机几何和共享许可；急停依赖由专门的联动场景覆盖。
    config.trigger.require_stop = false;
    config.trigger.allow_estimated_stop = false;
    config.trigger.hold_virtual_key = config.keyboard.aim_hold_virtual_keys.front();
    config.trigger.head_width_percent = 100.0f;
    config.trigger.head_height_percent = 100.0f;
    config.trigger.body_width_percent = 100.0f;
    config.trigger.body_height_percent = 100.0f;
    config.trigger.general_width_percent = 33.0f;
    config.trigger.general_height_percent = 44.0f;
    config.trigger.min_confidence = 0.75f;
    config.trigger.fire_delay_ms = 0;
    config.trigger.shot_interval_ms = 143;
    config.trigger.press_duration_ms = 27;
    config.trigger.max_hold_ms = 333;
    config.trigger.max_observation_age_ms = 67;
    config.trigger.fire_mode = TriggerFireMode::SINGLE;
    config.source_context.enabled = true;
    config.source_context.host = "127.0.0.1";
    config.source_context.port = 5017;
    config.source_context.process_name = "source-game.exe";
    config.source_context.ttl_ms = 350;
    config.source_context.token = "synthetic-test-marker-never-persist";
    expect(validate_app_config(config, error), "扳机允许与Aim共享许可键，源端字段可独立配置");
    expect(save_app_config(path.string(), config, error), "扳机与源端配置应原子保存");
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error), "扳机与源端配置应加载");
    expect(loaded.trigger.enabled && !loaded.trigger.fire_enabled && loaded.trigger.hold_virtual_key == config.trigger.hold_virtual_key &&
        loaded.trigger.head_width_percent == 100.0f && loaded.trigger.head_height_percent == 100.0f &&
        loaded.trigger.body_width_percent == 100.0f && loaded.trigger.body_height_percent == 100.0f &&
        loaded.trigger.general_width_percent == 33.0f && loaded.trigger.general_height_percent == 44.0f &&
        loaded.trigger.min_confidence == 0.75f && loaded.trigger.fire_delay_ms == 0 &&
        loaded.trigger.shot_interval_ms == AppConfig{}.trigger.shot_interval_ms && loaded.trigger.press_duration_ms == AppConfig{}.trigger.press_duration_ms &&
        loaded.trigger.max_hold_ms == AppConfig{}.trigger.max_hold_ms && loaded.trigger.max_observation_age_ms == 67 &&
        loaded.trigger.fire_mode == TriggerFireMode::SINGLE,
        "扳机通用几何及新鲜度往返，头身与节奏恢复生产默认");
    expect(loaded.source_context.enabled && loaded.source_context.host == "127.0.0.1" &&
        loaded.source_context.port == 5017 && loaded.source_context.process_name == "source-game.exe" &&
        loaded.source_context.ttl_ms == 350 && loaded.source_context.token.empty(), "源端配置往返不持久化认证值");
    {
        std::ifstream saved(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()};
        expect(bytes.find(config.source_context.token) == std::string::npos, "配置文件不能包含源端认证内容");
    }
    for (int key : {-1, 256, 1, int('W'), int('A'), int('S'), int('D'), 0x23, 0x77}) {
        auto candidate = config;
        candidate.trigger.hold_virtual_key = key;
        expect(!validate_app_config(candidate, error), "非法或安全/移动键不得成为扳机许可");
    }
    {
        auto candidate = config;
        candidate.keyboard.emergency_virtual_keys = {0x79};
        candidate.keyboard.runtime_toggle_virtual_keys = {0x7A};
        for (int key : {0x23, 0x77, 0x79, 0x7A}) {
            candidate.trigger.hold_virtual_key = key;
            expect(!validate_app_config(candidate, error), "改绑后仍保留End/F8禁用并检查实际安全键冲突");
        }
    }
    {
        auto candidate = config;
        candidate.trigger.require_stop = true;
        expect(!validate_app_config(candidate, error), "强依赖急停不能在急停关闭时启用");
        candidate.auto_stop.enabled = true;
        for (int key : {0x76, 5, 6}) {
            candidate.auto_stop.activation_virtual_key = key;
            candidate.trigger.hold_virtual_key = key;
            expect(validate_app_config(candidate, error), "扳机可与独立急停许可同键，包括鼠标侧键");
            expect(save_app_config(path.string(), candidate, error) && load_app_config(path.string(), loaded, error) &&
                loaded.trigger.require_stop && loaded.auto_stop.activation_virtual_key == key &&
                loaded.trigger.hold_virtual_key == key, "急停依赖和共享键必须往返保留");
        }
    }
    for (int variant = 0; variant < 7; ++variant) {
        auto candidate = config;
        switch (variant) {
            case 0: candidate.trigger.head_width_percent = 0.0f; break;
            case 1: candidate.trigger.body_height_percent = std::numeric_limits<float>::quiet_NaN(); break;
            case 2: candidate.trigger.fire_delay_ms = -1; break;
            case 3: candidate.trigger.fire_mode = static_cast<TriggerFireMode>(9); break;
            case 4: candidate.source_context.ttl_ms = 19; break;
            case 5: candidate.source_context.port = 0; break;
            case 6: candidate.source_context.process_name.clear(); break;
        }
        expect(!validate_app_config(candidate, error), "几何、时间、模式与源端缺项均应拒绝");
    }
    expect(write_file_bytes(path, "[detector]\nmodel_path=model.onnx\n"), "写入无新节的旧配置");
    expect(load_app_config(path.string(), loaded, error) && !loaded.trigger.enabled && loaded.trigger.fire_enabled &&
        loaded.trigger.hold_virtual_key == 0 && !loaded.source_context.enabled && loaded.source_context.token.empty(),
        "旧配置必须清除调用方遗留的启用和绑定状态");
    for (const char* text : {"[trigger]\nenabled=perhaps\n", "[trigger]\nfire_enabled=perhaps\n", "[trigger]\nmax_observation_age_ms=1.5\n",
            "[trigger]\nhold_virtual_key=999999999999999999999\n", "[trigger]\ngeneral_width_percent=nan\n",
            "[source_context]\nport=65536\n", "[source_context]\nttl_ms=1.5\n"}) {
        expect(write_file_bytes(path, text), "写入畸形配置夹具");
        expect(!load_app_config(path.string(), loaded, error), "新配置节须严格解析而非静默截断");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_legacy_keyboard_config() {
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_legacy_keyboard_config.ini";
    {
        std::ofstream output(path, std::ios::binary);
        output << "[detector]\nmodel_path=model.onnx\n"
                  "[keyboard]\n"
                  "aim_hold_virtual_key=5\n"
                  "emergency_virtual_key=6\n"
                  "runtime_toggle_virtual_key=118\n";
    }
    AppConfig loaded;
    std::string error;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.keyboard.aim_hold_virtual_keys ==
                   std::vector<int>{5} &&
               loaded.keyboard.emergency_virtual_keys ==
                   std::vector<int>{6} &&
               loaded.keyboard.runtime_toggle_virtual_keys ==
                   std::vector<int>{118} && !loaded.keyboard.debug_test_enabled && loaded.keyboard.debug_test_virtual_keys.empty(),
           "旧版单键配置必须迁移为单元素绑定集合");
    expect(loaded.keyboard.anomaly_mark_virtual_keys == std::vector<int>{0x78},
           "旧配置无冲突时默认F9标记");
    expect(write_file_bytes(path, "[detector]\nmodel_path=model.onnx\n[keyboard]\naim_hold_virtual_keys=120\n"),
           "写入已占用F9旧配置");
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.keyboard.aim_hold_virtual_keys == std::vector<int>{120} &&
               loaded.keyboard.anomaly_mark_virtual_keys.empty(),
           "旧配置F9占用时保留原绑定并禁用新增标记键");
    expect(write_file_bytes(path, "[detector]\nmodel_path=model.onnx\n[keyboard]\nanomaly_mark_virtual_keys=\n"),
           "写入显式禁用标记键配置");
    expect(load_app_config(path.string(), loaded, error) && loaded.keyboard.anomaly_mark_virtual_keys.empty(),
           "显式清空标记键不得恢复默认F9");
    expect(loaded.keyboard.lineup_locate_virtual_key == 0, "旧 INI 缺少定位字段默认未绑定");
    expect(write_file_bytes(path, "[lineup]\nlocate_virtual_key=123\n"), "写入可选定位键");
    expect(load_app_config(path.string(), loaded, error) && loaded.keyboard.lineup_locate_virtual_key == 123,
           "同一 INI 的 lineup 专属字段可读入");
    expect(write_file_bytes(path, "[lineup]\nlocate_virtual_key=123oops\n"), "写入非法定位值");
    expect(!load_app_config(path.string(), loaded, error), "定位字段必须严格整数");
    expect(loaded.keyboard.runtime_toggle_virtual_keys == std::vector<int>{118},
           "缺失 keyboard 字段保留此前迁移的 F7 绑定");
    expect(write_file_bytes(path, "[lineup]\nlocate_virtual_key=118\n"), "写入继承绑定冲突定位键");
    expect(!load_app_config(path.string(), loaded, error) &&
               loaded.keyboard.lineup_locate_virtual_key == 123 &&
               loaded.keyboard.runtime_toggle_virtual_keys == std::vector<int>{118},
           "定位不得占用继承的 F7，失败不改已加载配置");
    expect(write_file_bytes(path, "[keyboard]\nruntime_toggle_virtual_keys=119\n[lineup]\nlocate_virtual_key=119\n"),
           "显式配置原 F8 运行键及同键定位冲突");
    expect(!load_app_config(path.string(), loaded, error), "定位不占用原 F8 运行键");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_invalid_config() {
    AppConfig config;
    std::string error;
    config.detector.model_path.clear();
    expect(!validate_app_config(config, error) && !error.empty(),
           "缺少模型路径的默认配置必须失败关闭");
    config.detector.model_path = "model.onnx";
    config.aim.high_confidence = 0.05f;
    config.aim.low_confidence = 0.10f;
    expect(!validate_app_config(config, error),
           "高置信度阈值低于低阈值时必须拒绝配置");
    config.aim.high_confidence = 0.25f;
    config.ui.theme = static_cast<UiTheme>(99);
    expect(!validate_app_config(config, error),
           "未知 UI 主题必须拒绝配置");
    config.ui.theme = UiTheme::LIGHT;
    config.ui.width = kMinimumUiWidth - 1;
    config.ui.height = kMinimumUiHeight;
    expect(!validate_app_config(config, error),
           "UI 宽度低于五页紧凑布局下限时必须拒绝配置");
    config.ui.width = kMinimumUiWidth;
    config.ui.height = kMinimumUiHeight - 1;
    expect(!validate_app_config(config, error),
           "UI 高度低于五页紧凑布局下限时必须拒绝配置");
    config.ui.height = kMinimumUiHeight;
    expect(validate_app_config(config, error),
           "UI 尺寸恰好等于五页紧凑布局下限时应通过配置校验");
    config.capture.backend = CaptureBackend::UDP_MJPEG;
    config.capture.udp_url = "udp://127.0.0.1:5000";
    config.capture.udp_read_timeout_ms = 500;
    config.capture.udp_disconnect_timeout_ms = 100;
    expect(!validate_app_config(config, error),
           "UDP 断流判定短于单次读取超时时必须拒绝配置");
    config.capture.udp_disconnect_timeout_ms = 1000;
    config.capture.udp_frame_layout =
        UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.capture.udp_source_width = 0;
    config.capture.udp_source_height = 0;
    expect(!validate_app_config(config, error),
           "UDP 主机中心预裁剪缺少完整 FOV 尺寸时必须拒绝配置");
    config.capture.udp_source_width = 2560;
    config.capture.udp_source_height = 1440;
    expect(validate_app_config(config, error),
           "显式声明主机完整 FOV 后中心预裁剪配置应有效");

    config.capture.backend = CaptureBackend::NDI;
    config.capture.ndi_source_name.clear();
    expect(!validate_app_config(config, error),
           "NDI 源名称为空时必须拒绝配置");
    config.capture.ndi_source_name = "Auto";
    config.capture.ndi_frame_layout =
        NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.capture.ndi_source_width = 0;
    config.capture.ndi_source_height = 0;
    config.capture.ndi_require_frame_metadata = false;
    expect(!validate_app_config(config, error),
           "NDI 中心预裁剪缺少 metadata 和主机 FOV 时必须拒绝配置");
    config.capture.ndi_require_frame_metadata = true;
    expect(validate_app_config(config, error),
           "强制 Xen metadata 时允许由每帧声明主机 FOV 与 ROI");

    config.capture.backend = CaptureBackend::XUDP_JPEG;
    config.capture.udp_url.clear();
    expect(!validate_app_config(config, error),
           "XUDP 监听地址为空时必须拒绝配置");
    config.capture.udp_url = "udp://127.0.0.1:5600";
    config.capture.udp_frame_layout =
        UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.capture.udp_source_width = 0;
    config.capture.udp_source_height = 0;
    expect(validate_app_config(config, error),
           "XUDP 必须忽略裸 UDP 布局并由协议头提供主机几何");

    config.capture.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.mouse.backend = MouseBackend::KMBOX_NET;
    config.mouse.kmbox_ip.clear();
    config.mouse.kmbox_port = 0;
    config.mouse.kmbox_uuid.clear();
    expect(!validate_app_config(config, error),
           "KMBOX NET 缺少地址、端口和 UUID 时必须拒绝配置");
    config.mouse.kmbox_ip = "127.0.0.1";
    config.mouse.kmbox_port = 6234;
    config.mouse.kmbox_uuid = "1234567Z";
    expect(!validate_app_config(config, error),
           "KMBOX NET UUID 不是 8 位十六进制时必须拒绝配置");
    config.mouse.kmbox_uuid = "12345678";
    config.mouse.kmbox_ip = "127.0.0.256";
    expect(!validate_app_config(config, error),
           "KMBOX NET IPv4 段超出范围时必须拒绝配置");
    config.mouse.kmbox_ip = "127.0.0.1.";
    expect(!validate_app_config(config, error),
           "KMBOX NET IPv4 尾随分隔符必须拒绝配置");
    config.mouse.kmbox_ip = "127.0.0.1";
    config.mouse.kmbox_command_timeout_ms = 1001;
    expect(!validate_app_config(config, error),
           "KMBOX NET 命令超时超过 Pipeline 上限时必须拒绝配置");
    config.mouse.kmbox_command_timeout_ms = 300;
    expect(validate_app_config(config, error),
           "完整 KMBOX NET 配置应通过校验");

    config.mouse.backend = MouseBackend::MAKCU;
    config.mouse.makcu_port.clear();
    expect(!validate_app_config(config, error),
           "MAKCU 缺少显式 COM 口时必须拒绝配置");
    config.mouse.makcu_port = "COM01";
    expect(!validate_app_config(config, error),
           "MAKCU COM 口不得包含前导零");
    config.mouse.makcu_port = "com256";
    expect(validate_app_config(config, error),
           "MAKCU 应接受大小写不敏感的 COM256 上边界");
    config.mouse.makcu_baud_rate = 115200;
    expect(!validate_app_config(config, error),
           "MAKCU 115200 不满足物理键鼠 streaming 最低速率，必须拒绝");
    config.mouse.makcu_baud_rate = 921600;
    expect(!validate_app_config(config, error),
           "MAKCU 必须拒绝非官方稳定档位波特率");
    config.mouse.makcu_baud_rate = 4000000;
    config.mouse.makcu_command_timeout_ms = 1001;
    expect(!validate_app_config(config, error),
           "MAKCU 命令超时超过 Pipeline 上限时必须拒绝配置");
    config.mouse.makcu_command_timeout_ms = 300;
    expect(validate_app_config(config, error),
           "完整 MAKCU 配置应通过校验");

    config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
    config.keyboard.emergency_virtual_keys =
        config.keyboard.aim_hold_virtual_keys;
    expect(!validate_app_config(config, error),
           "按住启用键与急停键冲突时必须拒绝配置");
    config.keyboard.emergency_virtual_keys = {0x100};
    expect(!validate_app_config(config, error),
           "超出 Win32 虚拟键范围时必须拒绝配置");
    config.keyboard.emergency_virtual_keys = {0x23};
    expect(validate_app_config(config, error),
           "互不冲突且位于 Win32 范围内的虚拟键应通过校验");
    expect(config.keyboard.lineup_locate_virtual_key == 0 && config.keyboard.lineup_throw_virtual_key == 0,
           "旧配置定位和投掷均未绑定");
    config.keyboard.lineup_throw_virtual_key = 0x77;
    expect(!validate_app_config(config,error), "投掷不占用运行键");
    config.keyboard.lineup_throw_virtual_key = 0x20;
    expect(!validate_app_config(config,error), "Space只用于动作，不用作投掷触发键");
    config.keyboard.lineup_throw_virtual_key = 0x7B;
    config.keyboard.lineup_locate_virtual_key = 0x7B;
    expect(!validate_app_config(config,error), "定位与投掷必须独立按键");
    config.keyboard.lineup_throw_virtual_key = 0;
    config.keyboard.lineup_locate_virtual_key = 0x7B;
    expect(validate_app_config(config,error), "独立定位绑定有效");
    config.trigger.hold_virtual_key = 0x7B;
    expect(!validate_app_config(config,error), "定位与扳机键冲突必须拒绝");
    config.trigger.hold_virtual_key = 0;
    config.auto_stop.release_virtual_keys.push_back(0x7B);
    expect(!validate_app_config(config,error), "定位与急停释放键冲突必须拒绝");
    config.auto_stop.release_virtual_keys.pop_back();
    config.keyboard.lineup_locate_virtual_key = 0;
    config.keyboard.debug_test_virtual_keys = {0x79};
    config.keyboard.debug_test_enabled = true;
    expect(validate_app_config(config,error), "独立调试开关及无冲突绑定应有效");
    config.trigger.hold_virtual_key = 0x79;
    expect(!validate_app_config(config,error), "调试与扳机功能绑定必须双向互斥");
    config.trigger.hold_virtual_key = 0;
    config.auto_stop.release_virtual_keys.push_back(0x79);
    expect(!validate_app_config(config,error), "调试与急停释放绑定必须互斥");
    config.auto_stop.release_virtual_keys.pop_back();
    config.keyboard.debug_test_virtual_keys.clear(); config.keyboard.debug_test_enabled = false;
    config.keyboard.anomaly_mark_virtual_keys = {0x77};
    expect(!validate_app_config(config,error), "异常标记不能与运行切换共键");
    config.keyboard.anomaly_mark_virtual_keys = {config.auto_stop.activation_virtual_key};
    expect(!validate_app_config(config,error), "异常标记不能与急停允许键共键");
    config.keyboard.anomaly_mark_virtual_keys = {'W'};
    expect(!validate_app_config(config,error), "异常标记不能占用移动键");
    config.keyboard.anomaly_mark_virtual_keys = {0x78};
    config.keyboard.runtime_toggle_virtual_keys = {0x77, 0x77};
    expect(!validate_app_config(config, error),
           "同一功能内重复绑定必须拒绝配置");
    config.keyboard.runtime_toggle_virtual_keys.clear();
    expect(validate_app_config(config, error),
           "运行切换绑定为空时必须允许禁用该功能");
    config.mouse.allow_send_input = true;
    config.keyboard.aim_hold_virtual_keys.clear();
    expect(!validate_app_config(config, error),
           "物理输出启用时按住启用绑定不得为空");
    config.mouse.allow_send_input = false;
    expect(validate_app_config(config, error),
           "禁用物理输出时允许清空按住启用绑定");

    config.log.ringbuf_capacity = 0;
    expect(!validate_app_config(config, error),
           "启用 ring 时零容量必须拒绝日志配置");
    config.log.ringbuf_capacity = 1024;
    config.log.global_level = static_cast<LogLevel>(99);
    expect(!validate_app_config(config, error),
           "未知全局日志等级必须拒绝配置");
    config.log.global_level = LogLevel::TRACE;
    config.log.module_levels.emplace("bad module", LogLevel::INFO);
    expect(!validate_app_config(config, error),
           "含空格的模块日志配置名必须拒绝");
}

void test_complete_aim_config_validation() {
    const auto expect_invalid = [](const AimConfig& aim_config,
                                   const std::string& message) {
        AppConfig config;
        config.detector.model_path = "model.onnx";
        config.aim = aim_config;
        std::string error;
        expect(!validate_app_config(config, error), message);
    };

    const float nan = std::numeric_limits<float>::quiet_NaN();
    AimConfig config;
    config.high_confidence = nan;
    expect_invalid(config, "Aim 高置信度为 NaN 时必须拒绝");
    config = AimConfig{};
    config.low_confidence = nan;
    expect_invalid(config, "Aim 低置信度为 NaN 时必须拒绝");
    config = AimConfig{};
    config.min_iou = nan;
    expect_invalid(config, "Aim IoU 阈值为 NaN 时必须拒绝");
    config = AimConfig{};
    config.max_center_distance = 0.0f;
    expect_invalid(config, "Aim 中心距离阈值非正时必须拒绝");
    config = AimConfig{};
    config.switch_margin = 1.0f;
    expect_invalid(config, "Aim 切换优势达到 1 时必须拒绝");
    config = AimConfig{};
    config.acquisition_range_percent = 4.99f;
    expect_invalid(config, "Aim 搜索范围低于 5% 时必须拒绝");
    config = AimConfig{};
    config.acquisition_range_percent = 150.01f;
    expect_invalid(config, "Aim 搜索范围高于 150% 时必须拒绝");
    config = AimConfig{};
    config.max_prediction_lead_percent = 0.99f;
    expect_invalid(config, "Aim 最大预测提前距离低于 1% 时必须拒绝");
    config = AimConfig{};
    config.max_prediction_lead_percent = 50.01f;
    expect_invalid(config, "Aim 最大预测提前距离高于 50% 时必须拒绝");
    config = AimConfig{};
    config.body_aim_height_ratio = nan;
    expect_invalid(config, "Aim 身体瞄点比例为 NaN 时必须拒绝");
    config = AimConfig{};
    config.body_aim_range_percent = 0.99f;
    expect_invalid(config, "Aim 身体瞄准范围低于 1% 时必须拒绝");
    config = AimConfig{};
    config.body_aim_range_percent = 100.01f;
    expect_invalid(config, "Aim 身体瞄准范围高于 100% 时必须拒绝");
    config = AimConfig{};
    config.deadzone_pixels = -0.01f;
    expect_invalid(config, "Aim 死区为负数时必须拒绝");
    config = AimConfig{};
    config.smoothing = 1.01f;
    expect_invalid(config, "Aim 平滑系数越界时必须拒绝");
    config = AimConfig{};
    config.counts_per_pixel_x = nan;
    expect_invalid(config, "Aim 水平 counts 比例为 NaN 时必须拒绝");
    config = AimConfig{};
    config.counts_per_pixel_y = 0.0f;
    expect_invalid(config, "Aim 垂直 counts 比例非正时必须拒绝");
    config = AimConfig{};
    config.max_counts_per_frame = nan;
    expect_invalid(config, "Aim 单帧限幅为 NaN 时必须拒绝");
    config = AimConfig{};
    config.predicted_gain = 1.01f;
    expect_invalid(config, "Aim 预测增益越界时必须拒绝");
}

void test_xudp_backend_round_trip() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    source.capture.backend = CaptureBackend::XUDP_JPEG;
    source.capture.udp_url = "udp://0.0.0.0:5600";
    source.capture.udp_read_timeout_ms = 80;
    source.capture.udp_disconnect_timeout_ms = 1200;
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_xudp_config_round_trip.ini";
    std::string error;
    expect(save_app_config(path.string(), source, error),
           "XUDP 配置应成功写入: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.capture.backend == CaptureBackend::XUDP_JPEG &&
               loaded.capture.udp_url == source.capture.udp_url &&
               loaded.capture.udp_read_timeout_ms == 80 &&
               loaded.capture.udp_disconnect_timeout_ms == 1200,
           "xudp_jpeg 名称与网络参数必须完整往返");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_d3d11_cuda_interop_config() {
    AppConfig config;
    config.detector.model_path = "models/test.onnx";
    config.detector.backend = BackendType::TENSORRT;
    config.capture.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.capture.enable_d3d11_cuda_interop = true;
    std::string error;
    expect(validate_app_config(config, error),
           "默认 Desktop/TensorRT Graph 组合应接受 D3D11/CUDA 互操作: " +
               error);

    const auto path = std::filesystem::temp_directory_path() /
                      "xen_d3d11_cuda_interop.ini";
    expect(save_app_config(path.string(), config, error),
           "D3D11/CUDA 互操作配置应成功保存: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.capture.enable_d3d11_cuda_interop,
           "D3D11/CUDA 互操作开关必须完整往返");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    config.capture.backend = CaptureBackend::UDP_MJPEG;
    config.capture.udp_url = "udp://127.0.0.1:5000";
    expect(!validate_app_config(config, error),
           "网络 Capture 不得启用 D3D11/CUDA 互操作");
    config.capture.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.detector.backend = BackendType::CUDA;
    expect(!validate_app_config(config, error),
           "普通 CUDA EP 不得启用只为 TensorRT Graph 实现的互操作");
    config.detector.backend = BackendType::TENSORRT;
    config.detector.enable_trt_cuda_graph = false;
    expect(!validate_app_config(config, error),
           "关闭 CUDA Graph 时必须拒绝互操作");
    config.detector.enable_trt_cuda_graph = true;
    config.detector.enable_gpu_preprocess = false;
    expect(!validate_app_config(config, error),
           "关闭 GPU 前处理时必须拒绝互操作");
    config.detector.enable_gpu_preprocess = true;
    config.detector.input_width = 640;
    config.detector.input_height = 640;
    expect(!validate_app_config(config, error),
           "显式模型输入与 Capture ROI 不一致时必须提前拒绝");
}

void test_d3d11_directml_interop_config() {
    AppConfig config;
    config.detector.model_path = "models/test.onnx";
    config.detector.backend = BackendType::DIRECTML;
    config.capture.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.capture.enable_d3d11_directml_interop = true;
    std::string error;
    expect(validate_app_config(config, error),
           "默认 Desktop/DirectML 组合应接受 D3D11 资源桥接: " + error);

    const auto path = std::filesystem::temp_directory_path() /
                      "xen_d3d11_directml_interop.ini";
    expect(save_app_config(path.string(), config, error),
           "D3D11/DirectML 互操作配置应成功保存: " + error);
    AppConfig loaded;
    expect(load_app_config(path.string(), loaded, error) &&
               loaded.capture.enable_d3d11_directml_interop,
           "D3D11/DirectML 互操作开关必须完整往返");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    config.capture.backend = CaptureBackend::UDP_MJPEG;
    config.capture.udp_url = "udp://127.0.0.1:5000";
    expect(!validate_app_config(config, error),
           "网络 Capture 不得启用 D3D11/DirectML 互操作");
    config.capture.backend = CaptureBackend::DESKTOP_DUPLICATION;
    config.detector.backend = BackendType::CPU;
    expect(!validate_app_config(config, error),
           "CPU 后端不得启用 DirectML 资源桥接");
    config.detector.backend = BackendType::DIRECTML;
    config.detector.input_width = 640;
    config.detector.input_height = 640;
    expect(!validate_app_config(config, error),
           "DirectML 显式模型输入与 Capture ROI 不一致时必须拒绝");
    config.detector.input_width = 0;
    config.detector.input_height = 0;
    config.capture.enable_d3d11_cuda_interop = true;
    expect(!validate_app_config(config, error),
           "CUDA 与 DirectML 互操作开关不得同时启用");
}

void test_aim_soft_zone_config() {
    const auto directory = make_temp_test_directory("aim_soft_zone");
    if (directory.empty()) { expect(false, "软化区配置隔离目录"); return; }
    const auto path = directory / "config.ini";
    AppConfig loaded;
    std::string error;
    expect(loaded.aim.soft_zone_radius_percent == 30.0f &&
               loaded.aim.soft_zone_min_strength == 0.2f,
           "发行软化区默认半径30%且中心保留强度为0.2");
    loaded.aim.soft_zone_radius_percent = 40.0f;
    loaded.aim.soft_zone_min_strength = 0.6f;
    expect(write_file_bytes(path, "[aim]\nsmoothing=0.625\n") &&
               load_app_config(path.string(), loaded, error) &&
               loaded.aim.soft_zone_radius_percent == 0.0f &&
               loaded.aim.soft_zone_min_strength == 0.2f &&
               loaded.aim.smoothing == 0.625f,
           "旧配置缺键恢复软化区默认值，不沿用先前值或修改其他参数: " + error);
    loaded.aim.soft_zone_min_strength = 0.6f;
    expect(write_file_bytes(path, "[aim]\nsoft_zone_radius_percent=35\n") &&
               load_app_config(path.string(), loaded, error) &&
               loaded.aim.soft_zone_radius_percent == 35.0f &&
               loaded.aim.soft_zone_min_strength == 0.2f,
           "只有范围键时中心强度恢复默认值");
    expect(write_file_bytes(path, "[aim]\nsoft_zone_min_strength=0.5\n") &&
               load_app_config(path.string(), loaded, error) &&
               loaded.aim.soft_zone_radius_percent == 0.0f &&
               loaded.aim.soft_zone_min_strength == 0.5f,
           "只有强度键时范围恢复关闭值");
    for (const float radius : {0.0f, 25.0f, 100.0f}) {
        for (const float strength : {0.0f, 0.375f, 1.0f}) {
            loaded.aim.soft_zone_radius_percent = radius;
            loaded.aim.soft_zone_min_strength = strength;
            expect(save_app_config(path.string(), loaded, error),
                   "软化区合法边界允许保存: " + error);
            AppConfig restored;
            expect(load_app_config(path.string(), restored, error) &&
                       restored.aim.soft_zone_radius_percent == radius &&
                       restored.aim.soft_zone_min_strength == strength,
                   "软化区参数保存往返，包括关闭时的强度: " + error);
        }
    }
    for (const auto* key : {"soft_zone_radius_percent", "soft_zone_min_strength"}) {
        for (const auto* value : {"nan", "inf", "typo", "1junk", "-0.01", "100.1"}) {
            const auto original = loaded.aim;
            expect(write_file_bytes(path, std::string("[aim]\n") + key + "=" + value + "\n") &&
                       !load_app_config(path.string(), loaded, error),
                   std::string("软化区严格拒绝非法值: ") + key + "=" + value);
            expect(loaded.aim.soft_zone_radius_percent == original.soft_zone_radius_percent &&
                       loaded.aim.soft_zone_min_strength == original.soft_zone_min_strength,
                   "加载失败不得部分覆盖原有软化区配置");
        }
    }
    expect(write_file_bytes(path, "[aim]\nsoft_zone_min_strength=1.01\n") &&
               !load_app_config(path.string(), loaded, error),
           "中心保留强度不能超过 1");
    loaded.aim.soft_zone_radius_percent = 100.01f;
    expect(!save_app_config(path.string(), loaded, error), "保存拒绝超过上界的软化区范围");
    loaded.aim.soft_zone_radius_percent = 0.0f;
    loaded.aim.soft_zone_min_strength = std::numeric_limits<float>::quiet_NaN();
    expect(!save_app_config(path.string(), loaded, error), "关闭软化区也不能保存非法中心强度");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_feature_combinations_round_trip() {
    const auto path = std::filesystem::temp_directory_path() /
                      "xen_feature_combinations.ini";
    for (const bool delay : {false, true}) {
        for (const bool prediction : {false, true}) {
            AppConfig source;
            source.aim.enable_delay_compensation = delay;
            source.aim.enable_prediction = prediction;
            // 模拟独立调参，关闭功能的保存值仍应保留供下次开启。
            source.aim.counts_per_pixel_x = 0.375f;
            source.aim.counts_per_pixel_y = 0.525f;
            source.aim.smoothing = 0.625f;
            std::string error;
            expect(save_app_config(path.string(), source, error),
                   "四组合应允许保存独立参数: " + error);
            AppConfig loaded;
            expect(load_app_config(path.string(), loaded, error) &&
                       loaded.aim.enable_delay_compensation == delay &&
                       loaded.aim.enable_prediction == prediction &&
                       loaded.aim.counts_per_pixel_x == 0.375f &&
                       loaded.aim.counts_per_pixel_y == 0.525f &&
                       loaded.aim.smoothing == 0.625f &&
                       loaded.aim.control_delay_ms == 15.0f &&
                       loaded.aim.max_prediction_lead_percent == 35.0f,
                   "四组合保存回读不得重置开关或合并分轴参数: " + error);
        }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_movement_config() {
    const auto directory = make_temp_test_directory("movement");
    if (directory.empty()) { expect(false, "身法配置隔离目录"); return; }
    const auto path = directory / "config.ini";
    AppConfig config, loaded;
    std::string error;
    expect(!config.movement.enabled && config.movement.spin_enabled && config.movement.large_enabled &&
        config.movement.spin_trigger == movement::Trigger::WHEEL_UP &&
        config.movement.large_trigger == movement::Trigger::WHEEL_DOWN && config.movement.jump_delay_ms == 0,
        "身法总开关默认关闭，上滚旋转跳、下滚大跳且无额外延时");
    expect(config.movement.large_angle_degrees == 25.0 && config.movement.large_duration_ms == 170 &&
        config.movement.large_ctrl_enabled && config.movement.large_ctrl_delay_ms == 650 &&
        config.movement.large_ctrl_hold_ms == 200, "Long Jump 默认每侧25度170ms及650ms后按Ctrl200ms");
    config.movement.enabled = true;
    config.movement.spin_enabled = true;
    config.movement.large_enabled = true;
    config.movement.spin_trigger = movement::Trigger::KEY;
    config.movement.spin_virtual_key = 0x4A;
    config.movement.large_trigger = movement::Trigger::WHEEL_DOWN;
    config.movement.large_virtual_key = 0x4B;
    config.movement.report_mode = movement::ReportMode::RELATIVE_DELTA;
    config.movement.wheel_down_positive = false;
    config.movement.jump_delay_ms = 31;
    config.movement.trigger_guard_ms = 177;
    config.movement.spin_duration_ms = 321;
    config.movement.large_duration_ms = 123;
    config.movement.large_ctrl_enabled = false;
    config.movement.large_ctrl_delay_ms = 611;
    config.movement.large_ctrl_hold_ms = 41;
    config.movement.spin_angle_degrees = 73.5;
    config.movement.large_angle_degrees = 17.25;
    config.movement.sensitivity = 1.4;
    config.movement.yaw_degrees_per_count = 0.023;
    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error),
        "身法配置往返：" + error);
    const auto& value = loaded.movement;
    expect(value.enabled && value.spin_enabled && value.large_enabled &&
        value.spin_trigger == movement::Trigger::KEY && value.spin_virtual_key == 0x4A &&
        value.large_trigger == movement::Trigger::WHEEL_DOWN && value.large_virtual_key == 0x4B &&
        value.report_mode == movement::ReportMode::RELATIVE_DELTA && !value.wheel_down_positive &&
        value.trigger_guard_ms == 177 && value.jump_delay_ms == 31 && value.spin_duration_ms == 321 && value.large_duration_ms == 123 &&
        !value.large_ctrl_enabled && value.large_ctrl_delay_ms == 611 && value.large_ctrl_hold_ms == 41 &&
        value.spin_angle_degrees == 73.5 && value.large_angle_degrees == 17.25 &&
        value.sensitivity == 1.4 && value.yaw_degrees_per_count == 0.023,
        "身法所有字段必须独立保留");
    expect(write_file_bytes(path, "[ui]\nwidth=900\n"), "写入身法旧配置");
    expect(load_app_config(path.string(), loaded, error) && !loaded.movement.enabled &&
        loaded.movement.spin_enabled && !loaded.movement.large_enabled &&
        loaded.movement.spin_trigger == movement::Trigger::WHEEL_UP &&
        loaded.movement.trigger_guard_ms == 150 && !loaded.movement.wheel_down_positive &&
        loaded.movement.large_ctrl_enabled && loaded.movement.large_ctrl_delay_ms == 650 &&
        loaded.movement.large_ctrl_hold_ms == 200 && loaded.movement.large_duration_ms == 170 &&
        loaded.movement.large_angle_degrees == 25.0 &&
        loaded.movement.spin_duration_ms == movement::Config{}.spin_duration_ms &&
        loaded.movement.sensitivity == movement::Config{}.sensitivity,
        "缺少身法节时恢复默认且不继承开启状态");
    expect(write_file_bytes(path, "[movement]\nenabled=true\nmode=large_jump\njump_delay_ms=20\n"), "写入旧大跳配置");
    expect(load_app_config(path.string(), loaded, error) && loaded.movement.enabled &&
        !loaded.movement.spin_enabled && loaded.movement.large_enabled &&
        loaded.movement.large_trigger == movement::Trigger::WHEEL_DOWN && loaded.movement.jump_delay_ms == 20,
        "旧大跳迁移保持原单项、下滚和显式延时");
    expect(save_app_config(path.string(), loaded, error), "保存迁移后的身法配置");
    const auto migrated = read_file_bytes(path);
    expect(migrated.find("\nmode =") == std::string::npos &&
        migrated.find("\nmode=") == std::string::npos &&
        migrated.find("large_trigger") != std::string::npos,
        "迁移保存删除旧mode并写入独立触发");
    expect(write_file_bytes(path, "[movement]\nmode=spin\n"), "写入旧旋转跳配置");
    expect(load_app_config(path.string(), loaded, error) && loaded.movement.spin_enabled &&
        !loaded.movement.large_enabled && loaded.movement.spin_trigger == movement::Trigger::WHEEL_DOWN,
        "显式旧旋转跳保留原下滚触发");
    expect(write_file_bytes(path, "[movement]\nmode=unknown\nspin_enabled=true\nlarge_enabled=true\nspin_trigger=key\nspin_virtual_key=0\n"), "写入新格式未绑定键");
    expect(load_app_config(path.string(), loaded, error) && loaded.movement.spin_virtual_key == 0,
        "新格式优先于旧mode且允许未绑定键");
    for (const char* boundary : {"large_ctrl_delay_ms=0\nlarge_ctrl_hold_ms=1",
             "large_ctrl_delay_ms=2000\nlarge_ctrl_hold_ms=200"}) {
        expect(write_file_bytes(path, std::string("[movement]\n") + boundary + "\n"), "写入Ctrl合法边界");
        expect(load_app_config(path.string(), loaded, error), std::string("接受Ctrl合法边界：") + boundary);
    }

    for (int hold : {1, 30, 200, 201, 2000}) {
        const auto ini = std::string("[movement]\nlarge_ctrl_hold_ms=") + std::to_string(hold) +
            "\nspin_duration_ms=200\nlarge_duration_ms=170\nlarge_angle_degrees=25\nlarge_ctrl_delay_ms=650\n";
        expect(write_file_bytes(path, ini), "写入Ctrl按住范围回归");
        expect(load_app_config(path.string(), loaded, error) && loaded.movement.large_ctrl_hold_ms == hold,
            "接受Ctrl按住时长：" + std::to_string(hold));
        const auto expected = loaded.movement;
        expect(save_app_config(path.string(), loaded, error) && load_app_config(path.string(), loaded, error) &&
            loaded.movement == expected && loaded.movement.large_ctrl_hold_ms == hold &&
            loaded.movement.spin_duration_ms == 200 && loaded.movement.large_duration_ms == 170 &&
            loaded.movement.large_angle_degrees == 25 && loaded.movement.large_ctrl_delay_ms == 650,
            "Ctrl时长持久化不改其他身法参数：" + std::to_string(hold));
    }
    for (int hold : {-1, 0, 2001}) {
        loaded.movement.large_ctrl_hold_ms = hold;
        const auto before = read_file_bytes(path);
        expect(!save_app_config(path.string(), loaded, error) && read_file_bytes(path) == before,
            "保存拒绝Ctrl越界且不改文件：" + std::to_string(hold));
    }
    for (const char* invalid : {"enabled=perhaps", "mode=unknown", "report_mode=unknown",
             "large_ctrl_enabled=perhaps", "large_ctrl_delay_ms=-1", "large_ctrl_delay_ms=2001",
             "large_ctrl_delay_ms=1.5", "large_ctrl_hold_ms=0", "large_ctrl_hold_ms=2001", "large_ctrl_hold_ms=1.5",
             "spin_enabled=perhaps", "large_enabled=perhaps", "spin_trigger=unknown", "large_trigger=unknown",
             "spin_virtual_key=-1", "large_virtual_key=256", "spin_virtual_key=1.5",
             "spin_enabled=true\nspin_trigger=key\nspin_virtual_key=35",
             "spin_enabled=true\nlarge_enabled=true\nspin_trigger=wheel_down\nlarge_trigger=wheel_down",
             "wheel_down_positive=maybe", "trigger_guard_ms=-1", "trigger_guard_ms=2001", "trigger_guard_ms=1.5", "jump_delay_ms=1.5", "spin_duration_ms=abc",
             "large_duration_ms=0", "spin_angle_degrees=nan", "large_angle_degrees=-1",
             "sensitivity=0", "yaw_degrees_per_count=inf"}) {
        loaded.movement.spin_duration_ms = 333;
        expect(write_file_bytes(path, std::string("[movement]\n") + invalid + "\n"), "写入身法非法配置");
        expect(!load_app_config(path.string(), loaded, error), std::string("拒绝身法非法配置：") + invalid);
        expect(loaded.movement.spin_duration_ms == 333, "非法配置不能部分覆盖现有身法参数");
    }
    config.movement.sensitivity = 0;
    expect(!save_app_config(path.string(), config, error), "保存时同样拒绝非法身法参数");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_auxiliary_cycle_config() {
    const auto directory = make_temp_test_directory("auxiliary_cycle");
    if (directory.empty()) { expect(false, "循环配置隔离目录"); return; }
    const auto path = directory / "config.ini";
    AppConfig config, loaded;
    configure_test_kmbox(config);
    std::string error;
    config.auto_stop.enabled = config.auto_stop.cycle_enabled = true;
    config.auto_stop.activation_virtual_key = 5;
    config.trigger.enabled = config.trigger.fire_enabled = true;
    config.trigger.require_stop = config.trigger.allow_estimated_stop = true;
    config.trigger.hold_virtual_key = 5;
    config.mouse.backend = MouseBackend::KMBOX_NET;
    config.gsi.enabled = true;

    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
        loaded.auto_stop.cycle_enabled && loaded.trigger.hold_virtual_key == 5,
        "GSI循环配置必须完整往返：" + error);
    config.trigger.hold_virtual_key = 6;
    expect(!validate_app_config(config, error), "循环禁止双允许键造成归还等待失配");
    config.trigger.hold_virtual_key = 5;
    config.gsi.enabled = false;
    expect(!validate_app_config(config, error), "循环必须使用GSI自动识别武器");
    config.gsi.enabled = true; config.trigger.fire_enabled = false;
    expect(!validate_app_config(config, error), "循环不能等待永远不会发生的点射");
    { std::ofstream out(path); out << "[auto_stop]\ncycle_enabled=perhaps\n"; }
    expect(!load_app_config(path.string(), loaded, error), "循环开关严格解析布尔值");
}

void test_trigger_legacy_timing_migration() {
    const auto directory = make_temp_test_directory("trigger_legacy_timing");
    if (directory.empty()) { expect(false, "扳机迁移隔离目录"); return; }
    const auto path = directory / "config.ini";
    AppConfig loaded;
    configure_test_kmbox(loaded);
    std::string error;
    expect(write_file_bytes(path, "[trigger]\nrange_percent=55\n"), "写入框内范围比例");
    expect(load_app_config(path.string(), loaded, error) && loaded.trigger.range_percent == 55.0f,
        "用户范围比例必须读取而非被默认值覆盖");
    expect(!loaded.trigger.random_timing_enabled && AppConfig{}.trigger.random_timing_enabled,
        "旧配置缺键保持随机时序关闭，新生成发行配置采用当前选项");
    for (const bool enabled : {true, false}) {
        loaded.trigger.random_timing_enabled = enabled;
        expect(save_app_config(path.string(), loaded, error) && load_app_config(path.string(), loaded, error) &&
            loaded.trigger.random_timing_enabled == enabled, "独立随机时序开关保存往返");
    }
    expect(write_file_bytes(path, "[trigger]\nrandom_timing_enabled=perhaps\n") &&
        !load_app_config(path.string(), loaded, error) && error.find("trigger.random_timing_enabled") != std::string::npos,
        "随机时序开关拒绝非法布尔值");
    loaded.trigger.random_timing_enabled = true;
    expect(write_file_bytes(path, "[trigger]\nrange_percent=55\n") && load_app_config(path.string(), loaded, error) &&
        !loaded.trigger.random_timing_enabled, "旧配置缺键不能沿用先前启用的随机时序");
    loaded.trigger.range_percent = 72.0f;
    expect(save_app_config(path.string(), loaded, error) && load_app_config(path.string(), loaded, error) &&
        loaded.trigger.range_percent == 72.0f, "框内范围比例保存往返");
    for (const auto* value : {"nan", "0", "100.1"}) {
        expect(write_file_bytes(path, std::string("[trigger]\nrange_percent=") + value + "\n") &&
            !load_app_config(path.string(), loaded, error), "范围比例不能超出检测框");
    }
    expect(AppConfig{}.trigger.fire_delay_ms == 0 && AppConfig{}.trigger.fire_mode == TriggerFireMode::SINGLE,
        "生产扳机默认首发无额外延迟且使用共享点射");
    expect(AppConfig{}.trigger.head_width_percent == 100.0f && AppConfig{}.trigger.head_height_percent == 100.0f &&
        AppConfig{}.trigger.body_width_percent == 100.0f && AppConfig{}.trigger.body_height_percent == 100.0f,
        "生产扳机默认使用完整头身范围");
    expect(write_file_bytes(path, "[trigger]\nfire_delay_ms=invalid\nshot_interval_ms=invalid\npress_duration_ms=invalid\nmax_hold_ms=invalid\nfire_mode=invalid\nhead_width_percent=invalid\nhead_height_percent=invalid\nbody_width_percent=invalid\nbody_height_percent=invalid\n[weapon_timing]\nenabled=invalid\nmanual_id=unknown\nfile=custom/timing.json\n[recoil]\nhold_virtual_key=invalid\n"), "写入旧扳机字段");
    expect(load_app_config(path.string(), loaded, error) && loaded.trigger.fire_delay_ms == 0 &&
        loaded.trigger.fire_mode == TriggerFireMode::SINGLE && loaded.recoil.hold_virtual_key == 0 &&
        loaded.weapon_timing_file == "custom/timing.json" && loaded.trigger.head_width_percent == 100.0f &&
        loaded.trigger.head_height_percent == 100.0f && loaded.trigger.body_width_percent == 100.0f &&
        loaded.trigger.body_height_percent == 100.0f, "废弃节奏、缩放和额外许可字段不再参与解析: " + error);
    expect(save_app_config(path.string(), loaded, error), "保存迁移配置: " + error);
    const auto bytes = read_file_bytes(path);
    for (const auto* key : {"fire_delay_ms", "shot_interval_ms", "press_duration_ms", "max_hold_ms", "manual_id",
            "head_width_percent", "head_height_percent", "body_width_percent", "body_height_percent"})
        expect(bytes.find(key) == std::string::npos, "保存移除旧节奏字段");
    const auto timing_begin = bytes.find("[weapon_timing]");
    const auto timing_end = bytes.find('[', timing_begin + 1);
    expect(timing_begin != std::string::npos && bytes.substr(timing_begin, timing_end - timing_begin).find("enabled") == std::string::npos,
        "共享节奏不再保存独立启用开关");
    const auto trigger_begin = bytes.find("[trigger]");
    const auto trigger_end = bytes.find('[', trigger_begin + 1);
    expect(trigger_begin != std::string::npos && bytes.substr(trigger_begin, trigger_end - trigger_begin).find("fire_mode") == std::string::npos,
        "普通扳机不再保存独立射击模式");
    loaded.trigger.enabled = true;
    loaded.gsi.enabled = false;
    expect(!validate_app_config(loaded, error), "自动扳机必须复用全局GSI识别");
    loaded.gsi.enabled = true;
    expect(validate_app_config(loaded, error), "开启统一GSI后满足扳机识别依赖");
    loaded.trigger.random_timing_enabled = true;
    expect(validate_app_config(loaded, error), "独立随机时序开关允许生产扳机使用");
    loaded.trigger.random_timing_enabled = false;
    for (int variant = 0; variant < 6; ++variant) {
        auto unsupported = loaded;
        switch (variant) {
            case 0: unsupported.trigger.fire_mode = TriggerFireMode::AUTOMATIC; break;
            case 1: unsupported.trigger.fire_delay_ms = 20; break;
            case 2: unsupported.trigger.head_width_percent = 60.0f; break;
            case 3: unsupported.trigger.head_height_percent = 60.0f; break;
            case 4: unsupported.trigger.body_width_percent = 50.0f; break;
            case 5: unsupported.trigger.body_height_percent = 60.0f; break;
        }
        expect(!validate_app_config(unsupported, error), "程序化生产扳机不得绕过统一点射、首发时序和完整头身范围");
        unsupported.trigger.enabled = false;
        expect(validate_app_config(unsupported, error), "未启用扳机不阻断旧的无关合法参数");
    }
    // seq10915 身体几何子集的合成反例：原完整头框和许可未收录，不能称为完整Run回放。
    loaded.trigger.hold_virtual_key = 5;
    loaded.trigger.require_stop = false;
    TriggerController controller;
    expect(controller.configure(loaded.trigger), "迁移后的生产扳机配置可进入控制器");
    TriggerPermit permit;
    permit.enabled = permit.healthy = permit.focused = permit.armed = true;
    permit.context = {1, true, true, true, true, 60, 300};
    const TriggerTime start{std::chrono::milliseconds(1000)};
    controller.tick(permit, start);
    permit.held = true;
    TriggerObservation observation;
    observation.valid = observation.timing_valid = true;
    observation.epoch = 1; observation.sequence = 10915;
    observation.observed_at = start + std::chrono::milliseconds(1);
    observation.roi_width = observation.roi_height = 320;
    observation.center_x = observation.center_y = 160.0f;
    observation.detections.push_back({133.3125f, 134.5f, 175.1875f, 240.25f, 0.9f, 0});
    const auto decision = controller.observe(observation, permit, observation.observed_at);
    expect(decision.button_action == TriggerButtonAction::DOWN && decision.stop_action == TriggerStopAction::NONE,
        "迁移配置与完整身体范围同次产生首发DOWN，不增加20ms等待或急停联动");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_shared_weapon_timing_config() {
    const auto directory = make_temp_test_directory("weapon_timing");
    if (directory.empty()) { expect(false, "武器配置隔离目录"); return; }
    const auto path = directory / "config.ini";
    AppConfig config, loaded;
    configure_test_kmbox(config);
    configure_test_kmbox(loaded);
    std::string error;
    config.gsi.enabled = true;
    config.weapon_timing_file = "custom/weapon-timing.json";
    config.trigger.allow_estimated_stop = true;
    config.auto_stop.use_counterpulse_timing = true;
    config.auto_stop.counter_hold_ms = 40;
    config.auto_stop.shot_after_release_ms = 18;
    config.auto_stop.experimental_hud_model = true;
    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
        loaded.gsi.enabled &&
        loaded.weapon_timing_file == config.weapon_timing_file && loaded.trigger.allow_estimated_stop &&
        loaded.auto_stop.experimental_hud_model && loaded.auto_stop.use_counterpulse_timing && loaded.auto_stop.counter_hold_ms == 40 &&
        loaded.auto_stop.shot_after_release_ms == 18,
        "共享武器资料独立于压枪关闭且完整往返");
    config.weapon_timing_file.clear();
    expect(!save_app_config(path.string(), config, error), "共享资料路径不能为空");
    config.weapon_timing_file = "custom/weapon-timing.json";
    config.auto_stop.counter_hold_ms = 0;
    expect(!save_app_config(path.string(), config, error), "H40反向保持不能为0");
    config.auto_stop.counter_hold_ms = 40;
    config.auto_stop.shot_after_release_ms = 201;
    expect(!save_app_config(path.string(), config, error), "H40等待上限保持租约内");
    { std::ofstream out(path); out << "[weapon_timing]\nenabled=perhaps\n"; }
    expect(load_app_config(path.string(), loaded, error), "废弃共享开关不再参与校验");
    { std::ofstream out(path); out << "[trigger]\nallow_estimated_stop=perhaps\n"; }
    expect(!load_app_config(path.string(), loaded, error), "估计策略严格布尔校验");
    for (const auto* legacy : {"", "allow_estimated_stop=false\n", "allow_estimated_stop=true\n"}) {
        { std::ofstream out(path);
          out << "[trigger]\nenabled=true\nrequire_stop=true\nhold_virtual_key=5\n" << legacy
              << "[auto_stop]\nenabled=true\nactivation_virtual_key=5\n"
              << "[mouse]\nbackend=kmbox_net\n[gsi]\nenabled=true\n"; }
        expect(load_app_config(path.string(), loaded, error) && loaded.trigger.require_stop &&
            loaded.trigger.allow_estimated_stop && loaded.trigger.enabled &&
            loaded.trigger.hold_virtual_key == 5 && loaded.auto_stop.enabled,
            "旧严格观察联动迁移到移动急停策略，保留启用状态与许可键: " + error);
        expect(save_app_config(path.string(), loaded, error) && load_app_config(path.string(), loaded, error) &&
            loaded.trigger.require_stop && loaded.trigger.allow_estimated_stop,
            "移动急停策略迁移后保存重读保持一致: " + error);
    }
    { std::ofstream out(path); out << "[trigger]\nrequire_stop=true\nallow_estimated_stop=perhaps\n"; }
    expect(!load_app_config(path.string(), loaded, error), "联动迁移不得掩盖旧估计策略非法布尔值");
    { std::ofstream out(path); out << "[auto_stop]\nuse_counterpulse_timing=perhaps\n"; }
    expect(!load_app_config(path.string(), loaded, error), "H40策略严格布尔校验");
    { std::ofstream out(path); out << "[auto_stop]\nexperimental_hud_model=perhaps\n"; }
    expect(!load_app_config(path.string(), loaded, error), "实验策略严格布尔校验");
    { std::ofstream out(path); out << "[auto_stop]\ncounter_hold_ms=40.5\n"; }
    expect(!load_app_config(path.string(), loaded, error), "H40时长严格整数校验");
    { std::ofstream out(path); out << "[trigger]\nenabled=false\n"; }
    expect(load_app_config(path.string(), loaded, error) && !loaded.gsi.enabled &&
        !loaded.trigger.allow_estimated_stop &&
        loaded.auto_stop.experimental_hud_model && loaded.auto_stop.use_counterpulse_timing && loaded.auto_stop.counter_hold_ms == 40 &&
        loaded.auto_stop.shot_after_release_ms == 18,
        "旧配置缺键采用HUD默认，不继承调用方已启用共享或估计策略");
    expect(AppConfig{}.auto_stop.experimental_hud_model && !AutoStopConfig{}.experimental_hud_model,
        "生产默认采用HUD，独立急停模块保留H40回归默认");
    for (const auto* setting : {"", "experimental_hud_model=false\n", "experimental_hud_model=true\n"}) {
        const bool expected_hud = std::string(setting) != "experimental_hud_model=false\n";
        { std::ofstream out(path);
          out << "[auto_stop]\n" << setting << "counter_hold_ms=55\nshot_after_release_ms=27\n"; }
        loaded.auto_stop.experimental_hud_model = !expected_hud;
        expect(load_app_config(path.string(), loaded, error) &&
            loaded.auto_stop.experimental_hud_model == expected_hud &&
            loaded.auto_stop.counter_hold_ms == 55 && loaded.auto_stop.shot_after_release_ms == 27,
            "缺键选择HUD，显式策略优先且不改写自定义时长: " + error);
        expect(save_app_config(path.string(), loaded, error) && load_app_config(path.string(), loaded, error) &&
            loaded.auto_stop.experimental_hud_model == expected_hud &&
            loaded.auto_stop.counter_hold_ms == 55 && loaded.auto_stop.shot_after_release_ms == 27,
            "HUD默认与显式H40选择保存回读一致: " + error);
    }
    expect(AppConfig{}.auto_stop.use_counterpulse_timing,
        "生产默认直接采用已验收反向保持时序");
    for (const auto* legacy : {"", "use_counterpulse_timing=false\n", "use_counterpulse_timing=true\n"}) {
        for (const bool custom : {false, true}) for (const bool enabled : {false, true}) {
            { std::ofstream out(path);
              out << "[auto_stop]\nenabled=" << (enabled ? "true" : "false")
                  << "\nactivation_virtual_key=118\n" << legacy;
              if (custom) out << "counter_hold_ms=55\nshot_after_release_ms=27\n";
              out << "[trigger]\nenabled=false\n[weapon_timing]\nenabled=false\n"; }
            loaded.auto_stop.enabled = !enabled;
            loaded.auto_stop.use_counterpulse_timing = false;
            loaded.trigger.enabled = true;
            loaded.gsi.enabled = true;
            expect(load_app_config(path.string(), loaded, error) &&
                loaded.auto_stop.use_counterpulse_timing &&
                loaded.auto_stop.counter_hold_ms == (custom ? 55 : 40) &&
                loaded.auto_stop.shot_after_release_ms == (custom ? 27 : 18) &&
                loaded.auto_stop.enabled == enabled && loaded.auto_stop.activation_virtual_key == 118 &&
                !loaded.trigger.enabled && !loaded.gsi.enabled,
                "旧时序键缺失或真假值均迁移，保留时长及急停开关，不启用其他模块: " + error);
            expect(save_app_config(path.string(), loaded, error) &&
                read_file_bytes(path).find("use_counterpulse_timing") == std::string::npos,
                "保存迁移配置必须移除弃用时序开关: " + error);
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void test_team_filter_config() {
    const auto directory = make_temp_test_directory("team_filter");
    const auto path = directory / "config.ini";
    AppConfig config, loaded;
    std::string error;
    expect(!config.team_filter.enabled && config.team_filter.ct_class_ids == std::vector<int>({0, 1}) &&
        config.team_filter.t_class_ids == std::vector<int>({2, 3}), "通用模型默认关闭阵营筛选，保留四类模型映射");
    config.team_filter.enabled = true;
    expect(!validate_app_config(config, error), "开启阵营筛选必须有 GSI");
    config.gsi.enabled = true;
    config.team_filter.ct_class_ids = {4, 5};
    config.team_filter.t_class_ids = {6, 7};
    expect(save_app_config(path.string(), config, error) && load_app_config(path.string(), loaded, error) &&
        loaded.team_filter.enabled && loaded.team_filter.ct_class_ids == config.team_filter.ct_class_ids &&
        loaded.team_filter.t_class_ids == config.team_filter.t_class_ids, "自定义阵营映射往返保存: " + error);
    for (const auto ids : {std::vector<int>{}, std::vector<int>{-1}, std::vector<int>{4, 4}, std::vector<int>{6}}) {
        auto invalid = config;
        invalid.team_filter.ct_class_ids = ids;
        expect(!validate_app_config(invalid, error), "拒绝空、负数、重复及跨队重叠映射");
    }
    for (const auto* body : {"enabled=maybe\n", "enabled=true\nct_class_ids=\n",
             "ct_class_ids=-1\n", "t_class_ids=2,x\n", "ct_class_ids=0,0\n", "ct_class_ids=2\n"}) {
        expect(write_file_bytes(path, std::string("[gsi]\nenabled=true\n[team_filter]\n") + body) &&
            !load_app_config(path.string(), loaded, error), "阵营配置严格解析并拒绝非法映射");
    }
    expect(write_file_bytes(path, "[team_filter]\nenabled=false\nct_class_ids=\nt_class_ids=\n") &&
        load_app_config(path.string(), loaded, error) && loaded.team_filter.ct_class_ids.empty() &&
        loaded.team_filter.t_class_ids.empty(), "关闭时允许空映射且不静默恢复默认类别");
    loaded = config;
    expect(write_file_bytes(path, "[gsi]\nenabled=true\n") && load_app_config(path.string(), loaded, error) &&
        !loaded.team_filter.enabled && loaded.team_filter.ct_class_ids == std::vector<int>({0, 1}),
        "旧配置缺少阵营节时不继承调用方已开启状态");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main(int argc, char** argv) {
    // 发布工具复用生产序列化，导出代码默认值；不创建任何设备或运行时。
    if (argc == 3 && std::string(argv[1]) == "--write-default-config") {
        std::string error;
        if (save_app_config(argv[2], AppConfig{}, error)) return 0;
        std::cerr << error << '\n';
        return 1;
    }
    test_utf8_config_path();
    test_movement_config();
    test_team_filter_config();
    test_trigger_legacy_timing_migration();
    test_current_code_defaults();
    test_load_or_create_default_config();
    test_round_trip();
    test_feature_combinations_round_trip();
    test_aim_soft_zone_config();
    test_removed_observe_only_control_config();
    test_log_defaults_and_invalid_level();
    test_log_output_levels_round_trip();
    test_existing_file_rejects_malformed_typed_values();
    test_atomic_save_preserves_existing_file_on_write_failure();
    test_atomic_save_preserves_existing_file_on_replace_failure();
    test_auto_stop_config();
    test_trigger_and_source_context_config();
    test_shared_weapon_timing_config();
    test_auxiliary_cycle_config();
    test_legacy_keyboard_config();
    test_invalid_config();
    test_complete_aim_config_validation();
    test_xudp_backend_round_trip();
    test_d3d11_cuda_interop_config();
    test_d3d11_directml_interop_config();
    test_openvino_config_validation();
    test_makcu_config_round_trip();
    if (failures != 0) {
        std::cerr << "Config 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Config 测试全部通过。\n";
    return 0;
}
