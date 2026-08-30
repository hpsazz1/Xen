#include "config/config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

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

void test_current_code_defaults() {
    const AppConfig config;
    expect(config.detector.model_path == "14wv11.onnx" &&
               config.detector.backend == BackendType::TENSORRT &&
               config.detector.openvino_device == OpenVinoDevice::CPU &&
               config.detector.enable_fp16 &&
               config.detector.enable_trt_cuda_graph &&
               config.detector.enable_gpu_preprocess,
           "代码 Detector 默认值应匹配当前 TensorRT 配置");
    expect(config.capture.backend == CaptureBackend::NDI &&
               config.capture.ndi_source_name == "HPSAZZ (Xen-ROI-320)" &&
               config.capture.ndi_discovery_timeout_ms == 10000 &&
               config.capture.ndi_clock_sync_url ==
                   "udp://192.168.3.10:5011" &&
               config.capture.ndi_frame_layout ==
                   NetworkFrameLayout::CENTER_CROP_1_TO_1 &&
               config.capture.ndi_source_width == 2560 &&
               config.capture.ndi_source_height == 1440 &&
               config.capture.roi_width == 320 &&
               config.capture.roi_height == 320,
           "代码 Capture 默认值应匹配当前 NDI 320 配置");
    expect(config.aim.person_class_ids == std::vector<int>({0, 2}) &&
               config.aim.head_class_ids == std::vector<int>({1, 3}) &&
               config.aim.smoothing == 0.475f &&
               config.aim.counts_per_pixel_x == 0.40f &&
               config.aim.counts_per_pixel_y == 0.40f &&
               config.aim.max_counts_per_frame == 12.0f &&
               config.aim.enable_delay_compensation &&
               config.aim.control_delay_ms == 40.0f &&
               config.aim.max_delay_compensation_ms == 44.0f &&
               config.aim.enable_prediction &&
               config.aim.max_prediction_lead_percent == 35.0f &&
               config.aim.predicted_gain == 0.50f,
           "代码 Aim 默认值应匹配当前 prediction 配置");
    expect(config.mouse.backend == MouseBackend::KMBOX_NET &&
               !config.mouse.allow_send_input &&
               config.mouse.kmbox_ip == "192.168.2.188" &&
               config.mouse.kmbox_port == 13384 &&
               config.mouse.kmbox_uuid == "7679E04E",
           "代码 Mouse 默认值应包含设备参数并保持物理输出禁用");
    expect(config.log.global_level == LogLevel::INFO &&
               config.ui.open_detached_preview_on_start,
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
               loaded.detector.backend == BackendType::TENSORRT &&
               loaded.capture.backend == CaptureBackend::NDI &&
               loaded.aim.enable_prediction &&
               loaded.mouse.backend == MouseBackend::KMBOX_NET &&
               !loaded.mouse.allow_send_input,
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
                   std::vector<int>{118},
           "旧版单键配置必须迁移为单元素绑定集合");
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

} // namespace

int main() {
    test_current_code_defaults();
    test_load_or_create_default_config();
    test_round_trip();
    test_removed_observe_only_control_config();
    test_log_defaults_and_invalid_level();
    test_existing_file_rejects_malformed_typed_values();
    test_atomic_save_preserves_existing_file_on_write_failure();
    test_atomic_save_preserves_existing_file_on_replace_failure();
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
