#include "config/config.h"

#include "aim/aim_config_internal.h"

#include <SimpleIni.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxLogModuleOverrides = 64;
constexpr std::size_t kMaxLogModuleNameLength = 64;

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::vector<int> parse_int_list(const char* value,
                                const std::vector<int>& fallback) {
    if (!value || *value == '\0') return fallback;
    std::vector<int> result;
    std::istringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        try {
            std::size_t consumed = 0;
            const int parsed = std::stoi(token, &consumed);
            if (consumed != token.size() || parsed < 0) return fallback;
            result.push_back(parsed);
        } catch (...) {
            return fallback;
        }
    }
    return result;
}

std::string format_int_list(const std::vector<int>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) stream << ',';
        stream << values[index];
    }
    return stream.str();
}

BackendType parse_backend(const char* value, BackendType fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "cuda") return BackendType::CUDA;
    if (normalized == "tensorrt") return BackendType::TENSORRT;
    if (normalized == "directml") return BackendType::DIRECTML;
    if (normalized == "openvino") return BackendType::OPENVINO;
    if (normalized == "cpu") return BackendType::CPU;
    return fallback;
}

const char* backend_name(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::CUDA: return "cuda";
        case BackendType::TENSORRT: return "tensorrt";
        case BackendType::DIRECTML: return "directml";
        case BackendType::OPENVINO: return "openvino";
        case BackendType::CPU: return "cpu";
    }
    return "cpu";
}

OpenVinoDevice parse_openvino_device(
        const char* value, OpenVinoDevice fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "gpu") return OpenVinoDevice::GPU;
    if (normalized == "cpu") return OpenVinoDevice::CPU;
    if (normalized == "npu") return OpenVinoDevice::NPU;
    return fallback;
}

const char* openvino_device_name(OpenVinoDevice device) noexcept {
    switch (device) {
        case OpenVinoDevice::GPU: return "gpu";
        case OpenVinoDevice::CPU: return "cpu";
        case OpenVinoDevice::NPU: return "npu";
    }
    return "unknown";
}

CaptureBackend parse_capture_backend(
        const char* value, CaptureBackend fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "desktop_duplication") {
        return CaptureBackend::DESKTOP_DUPLICATION;
    }
    if (normalized == "udp_mjpeg") return CaptureBackend::UDP_MJPEG;
    if (normalized == "xudp_jpeg") return CaptureBackend::XUDP_JPEG;
    if (normalized == "ndi") return CaptureBackend::NDI;
    return fallback;
}

const char* capture_backend_name(CaptureBackend backend) noexcept {
    switch (backend) {
        case CaptureBackend::DESKTOP_DUPLICATION:
            return "desktop_duplication";
        case CaptureBackend::UDP_MJPEG:
            return "udp_mjpeg";
        case CaptureBackend::XUDP_JPEG:
            return "xudp_jpeg";
        case CaptureBackend::NDI:
            return "ndi";
    }
    return "desktop_duplication";
}

MouseBackend parse_mouse_backend(
        const char* value, MouseBackend fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "win32_send_input") {
        return MouseBackend::WIN32_SEND_INPUT;
    }
    if (normalized == "kmbox_net") return MouseBackend::KMBOX_NET;
    if (normalized == "makcu") return MouseBackend::MAKCU;
    return fallback;
}

const char* mouse_backend_name(MouseBackend backend) noexcept {
    switch (backend) {
        case MouseBackend::WIN32_SEND_INPUT: return "win32_send_input";
        case MouseBackend::KMBOX_NET: return "kmbox_net";
        case MouseBackend::MAKCU: return "makcu";
    }
    return "win32_send_input";
}

NetworkFrameLayout parse_network_frame_layout(
        const char* value, NetworkFrameLayout fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "full_frame_1_to_1") {
        return NetworkFrameLayout::FULL_FRAME_1_TO_1;
    }
    if (normalized == "full_frame_scaled") {
        return NetworkFrameLayout::FULL_FRAME_SCALED;
    }
    if (normalized == "center_crop_1_to_1") {
        return NetworkFrameLayout::CENTER_CROP_1_TO_1;
    }
    return fallback;
}

const char* network_frame_layout_name(NetworkFrameLayout layout) noexcept {
    switch (layout) {
        case NetworkFrameLayout::FULL_FRAME_1_TO_1:
            return "full_frame_1_to_1";
        case NetworkFrameLayout::FULL_FRAME_SCALED:
            return "full_frame_scaled";
        case NetworkFrameLayout::CENTER_CROP_1_TO_1:
            return "center_crop_1_to_1";
    }
    return "full_frame_1_to_1";
}

OutputFormat parse_output_format(const char* value, OutputFormat fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "auto") return OutputFormat::AUTO;
    if (normalized == "channel_first") return OutputFormat::CHANNEL_FIRST;
    if (normalized == "anchor_first_objectness") {
        return OutputFormat::ANCHOR_FIRST_OBJECTNESS;
    }
    if (normalized == "end_to_end") return OutputFormat::END_TO_END;
    return fallback;
}

bool parse_log_level(const char* value, LogLevel& result) {
    if (!value) return true;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "trace") {
        result = LogLevel::TRACE;
    } else if (normalized == "debug") {
        result = LogLevel::DEBUG;
    } else if (normalized == "info") {
        result = LogLevel::INFO;
    } else if (normalized == "warn" || normalized == "warning") {
        result = LogLevel::WARN;
    } else if (normalized == "error") {
        result = LogLevel::ERROR;
    } else if (normalized == "off") {
        result = LogLevel::OFF;
    } else {
        return false;
    }
    return true;
}

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::TRACE: return "trace";
        case LogLevel::DEBUG: return "debug";
        case LogLevel::INFO:  return "info";
        case LogLevel::WARN:  return "warn";
        case LogLevel::ERROR: return "error";
        case LogLevel::OFF:   return "off";
    }
    return "off";
}

bool valid_log_module_name(const std::string& name) noexcept {
    if (name.empty() || name.size() > kMaxLogModuleNameLength) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

const char* output_format_name(OutputFormat format) noexcept {
    switch (format) {
        case OutputFormat::AUTO: return "auto";
        case OutputFormat::CHANNEL_FIRST: return "channel_first";
        case OutputFormat::ANCHOR_FIRST_OBJECTNESS:
            return "anchor_first_objectness";
        case OutputFormat::END_TO_END: return "end_to_end";
    }
    return "auto";
}

UiTheme parse_ui_theme(const char* value, UiTheme fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "light") return UiTheme::LIGHT;
    if (normalized == "dark") return UiTheme::DARK;
    return fallback;
}

const char* ui_theme_name(UiTheme theme) noexcept {
    switch (theme) {
        case UiTheme::LIGHT: return "light";
        case UiTheme::DARK: return "dark";
    }
    return "light";
}

bool finite_unit(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool valid_kmbox_uuid(const std::string& value) noexcept {
    if (value.size() != 8) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool valid_makcu_port(const std::string& value) noexcept {
    if (value.size() < 4U || value.size() > 6U ||
        std::tolower(static_cast<unsigned char>(value[0])) != 'c' ||
        std::tolower(static_cast<unsigned char>(value[1])) != 'o' ||
        std::tolower(static_cast<unsigned char>(value[2])) != 'm' ||
        (value.size() > 4U && value[3] == '0')) {
        return false;
    }
    int port_number = 0;
    for (std::size_t index = 3U; index < value.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if (!std::isdigit(character)) return false;
        port_number = port_number * 10 + static_cast<int>(character - '0');
    }
    return port_number >= 1 && port_number <= 256;
}

bool valid_ipv4_address(const std::string& value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.') {
        return false;
    }
    int part_count = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('.', begin);
        const std::size_t length =
            (end == std::string::npos ? value.size() : end) - begin;
        if (length == 0 || length > 3) return false;
        int part = 0;
        for (std::size_t index = begin; index < begin + length; ++index) {
            const unsigned char ch =
                static_cast<unsigned char>(value[index]);
            if (!std::isdigit(ch)) return false;
            part = part * 10 + static_cast<int>(ch - '0');
        }
        if (part > 255) return false;
        ++part_count;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return part_count == 4;
}

void set_error(std::string& error, const std::string& value) noexcept {
    try {
        error = value;
    } catch (...) {
    }
}

} // namespace

bool validate_app_config(const AppConfig& config,
                         std::string& error) noexcept {
    try {
        constexpr int kMaxCaptureSourceDimension = 16384;
        constexpr int kMaxLogRingBufferCapacity = 65'536;
        constexpr int kMaxLogFileCount = 200'000;
        if (config.detector.model_path.empty()) {
            error = "Detector 模型路径不能为空";
            return false;
        }
        const bool log_invalid =
            static_cast<int>(config.log.global_level) <
                static_cast<int>(LogLevel::TRACE) ||
            static_cast<int>(config.log.global_level) >
                static_cast<int>(LogLevel::OFF) ||
            (config.log.enable_ringbuf &&
             (config.log.ringbuf_capacity <= 0 ||
              config.log.ringbuf_capacity > kMaxLogRingBufferCapacity)) ||
            (config.log.enable_file &&
             (config.log.log_dir.empty() ||
              config.log.file_max_size_mb <= 0 ||
              config.log.file_max_count <= 0 ||
              config.log.file_max_count > kMaxLogFileCount)) ||
            (config.log.enable_debug_file && config.log.log_dir.empty());
        if (log_invalid) {
            error = "Log 配置非法";
            return false;
        }
        if (config.log.module_levels.size() > kMaxLogModuleOverrides) {
            error = "Log 模块等级数量超过上限";
            return false;
        }
        for (const auto& [module, level] : config.log.module_levels) {
            if (!valid_log_module_name(module) ||
                static_cast<int>(level) <
                    static_cast<int>(LogLevel::TRACE) ||
                static_cast<int>(level) >
                    static_cast<int>(LogLevel::OFF)) {
                error = "Log 模块等级配置非法";
                return false;
            }
        }
        const bool detector_backend_invalid =
            config.detector.backend != BackendType::CUDA &&
            config.detector.backend != BackendType::TENSORRT &&
            config.detector.backend != BackendType::DIRECTML &&
            config.detector.backend != BackendType::OPENVINO &&
            config.detector.backend != BackendType::CPU;
        const bool openvino_device_invalid =
            config.detector.openvino_device != OpenVinoDevice::GPU &&
            config.detector.openvino_device != OpenVinoDevice::CPU &&
            config.detector.openvino_device != OpenVinoDevice::NPU;
        const bool openvino_device_id_invalid =
            config.detector.backend == BackendType::OPENVINO &&
            config.detector.openvino_device != OpenVinoDevice::GPU &&
            config.detector.device_id != 0;
        if (detector_backend_invalid || openvino_device_invalid ||
            openvino_device_id_invalid || config.detector.device_id < 0 ||
            config.detector.input_width < 0 ||
            config.detector.input_height < 0 ||
            ((config.detector.input_width == 0) !=
             (config.detector.input_height == 0)) ||
            !finite_unit(config.detector.conf_threshold) ||
            !finite_unit(config.detector.nms_threshold) ||
            config.detector.top_k <= 0) {
            error = "Detector 配置非法";
            return false;
        }
        const auto valid_network_layout =
            [kMaxCaptureSourceDimension](NetworkFrameLayout layout,
                                         int source_width,
                                         int source_height,
                                         bool allow_missing_source) {
                if (layout == NetworkFrameLayout::FULL_FRAME_1_TO_1) {
                    return source_width == 0 && source_height == 0;
                }
                return (layout == NetworkFrameLayout::FULL_FRAME_SCALED ||
                        layout == NetworkFrameLayout::CENTER_CROP_1_TO_1) &&
                       ((allow_missing_source && source_width == 0 &&
                         source_height == 0) ||
                        (source_width > 0 && source_height > 0 &&
                         source_width <= kMaxCaptureSourceDimension &&
                         source_height <= kMaxCaptureSourceDimension));
            };
        const bool common_capture_invalid =
            (config.capture.backend != CaptureBackend::DESKTOP_DUPLICATION &&
             config.capture.backend != CaptureBackend::UDP_MJPEG &&
             config.capture.backend != CaptureBackend::XUDP_JPEG &&
             config.capture.backend != CaptureBackend::NDI) ||
            config.capture.roi_width <= 0 || config.capture.roi_height <= 0 ||
            (!config.capture.center_roi &&
             (config.capture.roi_x < 0 || config.capture.roi_y < 0)) ||
            config.capture.acquire_timeout_ms < 0;
        const bool desktop_invalid =
            config.capture.backend == CaptureBackend::DESKTOP_DUPLICATION &&
            (config.capture.adapter_index < 0 ||
             config.capture.output_index < 0);
        const bool udp_invalid =
            config.capture.backend == CaptureBackend::UDP_MJPEG &&
            (config.capture.udp_url.empty() ||
             config.capture.udp_read_timeout_ms <= 0 ||
             config.capture.udp_read_timeout_ms > 1000 ||
             config.capture.udp_disconnect_timeout_ms <
                 config.capture.udp_read_timeout_ms ||
             config.capture.udp_disconnect_timeout_ms > 60000 ||
             !valid_network_layout(config.capture.udp_frame_layout,
                                   config.capture.udp_source_width,
                                    config.capture.udp_source_height, false));
        const bool xudp_invalid =
            config.capture.backend == CaptureBackend::XUDP_JPEG &&
            (config.capture.udp_url.empty() ||
             config.capture.udp_read_timeout_ms <= 0 ||
             config.capture.udp_read_timeout_ms > 1000 ||
             config.capture.udp_disconnect_timeout_ms <
                 config.capture.udp_read_timeout_ms ||
             config.capture.udp_disconnect_timeout_ms > 60000);
        const bool ndi_invalid =
            config.capture.backend == CaptureBackend::NDI &&
            (config.capture.ndi_source_name.empty() ||
             config.capture.ndi_discovery_timeout_ms <= 0 ||
             config.capture.ndi_discovery_timeout_ms > 60000 ||
             config.capture.ndi_receive_timeout_ms <= 0 ||
             config.capture.ndi_receive_timeout_ms > 1000 ||
             config.capture.ndi_disconnect_timeout_ms <
                 config.capture.ndi_receive_timeout_ms ||
             config.capture.ndi_disconnect_timeout_ms > 60000 ||
             !valid_network_layout(config.capture.ndi_frame_layout,
                                   config.capture.ndi_source_width,
                                   config.capture.ndi_source_height,
                                   config.capture.ndi_require_frame_metadata));
        const bool d3d11_cuda_interop_invalid =
            config.capture.enable_d3d11_cuda_interop &&
            (config.capture.backend !=
                 CaptureBackend::DESKTOP_DUPLICATION ||
             config.detector.backend != BackendType::TENSORRT ||
             !config.detector.enable_trt_cuda_graph ||
             !config.detector.enable_gpu_preprocess ||
             (config.detector.input_width > 0 &&
              (config.detector.input_width != config.capture.roi_width ||
               config.detector.input_height != config.capture.roi_height)));
        const bool d3d11_directml_interop_invalid =
            config.capture.enable_d3d11_directml_interop &&
            (config.capture.backend !=
                 CaptureBackend::DESKTOP_DUPLICATION ||
             config.detector.backend != BackendType::DIRECTML ||
             (config.detector.input_width > 0 &&
              (config.detector.input_width != config.capture.roi_width ||
               config.detector.input_height != config.capture.roi_height)));
        const bool d3d11_interop_modes_conflict =
            config.capture.enable_d3d11_cuda_interop &&
            config.capture.enable_d3d11_directml_interop;
        if (common_capture_invalid || desktop_invalid || udp_invalid ||
            xudp_invalid || ndi_invalid || d3d11_cuda_interop_invalid ||
            d3d11_directml_interop_invalid ||
            d3d11_interop_modes_conflict) {
            error = "Capture 配置非法";
            return false;
        }
        if (!aim::detail::valid_aim_config(config.aim)) {
            error = "Aim 配置非法";
            return false;
        }
        const bool mouse_backend_invalid =
            config.mouse.backend != MouseBackend::WIN32_SEND_INPUT &&
            config.mouse.backend != MouseBackend::KMBOX_NET &&
            config.mouse.backend != MouseBackend::MAKCU;
        const bool kmbox_invalid =
            config.mouse.backend == MouseBackend::KMBOX_NET &&
            (!valid_ipv4_address(config.mouse.kmbox_ip) ||
             config.mouse.kmbox_port <= 0 ||
             config.mouse.kmbox_port > 65535 ||
             !valid_kmbox_uuid(config.mouse.kmbox_uuid) ||
             config.mouse.kmbox_connect_timeout_ms <= 0 ||
             config.mouse.kmbox_connect_timeout_ms > 10000 ||
             config.mouse.kmbox_command_timeout_ms <= 0 ||
             config.mouse.kmbox_command_timeout_ms > 1000);
        const bool makcu_invalid =
            config.mouse.backend == MouseBackend::MAKCU &&
            (!valid_makcu_port(config.mouse.makcu_port) ||
             config.mouse.makcu_baud_rate != 4000000 ||
             config.mouse.makcu_connect_timeout_ms <= 0 ||
             config.mouse.makcu_connect_timeout_ms > 10000 ||
             config.mouse.makcu_command_timeout_ms <= 0 ||
             config.mouse.makcu_command_timeout_ms > 1000);
        if (mouse_backend_invalid || kmbox_invalid || makcu_invalid) {
            error = "Mouse 配置非法";
            return false;
        }
        const bool physical_keyboard_invalid =
            config.mouse.allow_send_input &&
            (config.keyboard.aim_hold_virtual_keys.empty() ||
             config.keyboard.emergency_virtual_keys.empty());
        if (!valid_keyboard_config(config.keyboard) ||
            physical_keyboard_invalid ||
            config.runtime.profile_window < 64 ||
            config.runtime.profile_window > 4096 ||
            config.ui.width < kMinimumUiWidth ||
            config.ui.height < kMinimumUiHeight ||
            (config.ui.theme != UiTheme::LIGHT &&
             config.ui.theme != UiTheme::DARK)) {
            error = "Runtime、Keyboard 或 UI 配置非法";
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "校验配置时发生未知异常");
        return false;
    }
}

bool load_app_config(const std::string& path,
                     AppConfig& config,
                     std::string& error) noexcept {
    try {
        CSimpleIniA ini;
        ini.SetUnicode(true);
        const SI_Error result = ini.LoadFile(path.c_str());
        if (result < 0) {
            error = "无法读取配置文件: " + path;
            return false;
        }

        AppConfig candidate = config;
        const char* configured_log_level = ini.GetValue(
            "log", "global_level", nullptr);
        if (!parse_log_level(configured_log_level,
                             candidate.log.global_level)) {
            error = "Log global_level 配置非法";
            return false;
        }
        candidate.log.enable_console = ini.GetBoolValue(
            "log", "enable_console", candidate.log.enable_console);
        candidate.log.enable_file = ini.GetBoolValue(
            "log", "enable_file", candidate.log.enable_file);
        candidate.log.enable_debug_file = ini.GetBoolValue(
            "log", "enable_debug_file", candidate.log.enable_debug_file);
        candidate.log.enable_ringbuf = ini.GetBoolValue(
            "log", "enable_ringbuf", candidate.log.enable_ringbuf);
        candidate.log.ringbuf_capacity = static_cast<int>(ini.GetLongValue(
            "log", "ringbuf_capacity", candidate.log.ringbuf_capacity));
        candidate.log.log_dir = ini.GetValue(
            "log", "log_dir", candidate.log.log_dir.c_str());
        candidate.log.file_max_size_mb = static_cast<int>(ini.GetLongValue(
            "log", "file_max_size_mb", candidate.log.file_max_size_mb));
        candidate.log.file_max_count = static_cast<int>(ini.GetLongValue(
            "log", "file_max_count", candidate.log.file_max_count));
        CSimpleIniA::TNamesDepend log_module_keys;
        if (ini.GetAllKeys("log_modules", log_module_keys)) {
            candidate.log.module_levels.clear();
            for (const auto& key : log_module_keys) {
                const std::string module = lowercase_ascii(
                    key.pItem ? key.pItem : "");
                LogLevel module_level = LogLevel::INFO;
                const char* configured_level = key.pItem
                    ? ini.GetValue("log_modules", key.pItem, nullptr)
                    : nullptr;
                if (!valid_log_module_name(module) || !configured_level ||
                    !parse_log_level(configured_level, module_level) ||
                    !candidate.log.module_levels.emplace(
                        module, module_level).second) {
                    error = "Log 模块等级配置非法: " + module;
                    return false;
                }
            }
        }

        candidate.detector.model_path =
            ini.GetValue("detector", "model_path",
                         candidate.detector.model_path.c_str());
        candidate.detector.backend = parse_backend(
            ini.GetValue("detector", "backend"),
            candidate.detector.backend);
        candidate.detector.device_id = static_cast<int>(ini.GetLongValue(
            "detector", "device_id", candidate.detector.device_id));
        candidate.detector.openvino_device = parse_openvino_device(
            ini.GetValue("detector", "openvino_device"),
            candidate.detector.openvino_device);
        candidate.detector.input_width = static_cast<int>(ini.GetLongValue(
            "detector", "input_width", candidate.detector.input_width));
        candidate.detector.input_height = static_cast<int>(ini.GetLongValue(
            "detector", "input_height", candidate.detector.input_height));
        candidate.detector.conf_threshold = static_cast<float>(ini.GetDoubleValue(
            "detector", "conf_threshold", candidate.detector.conf_threshold));
        candidate.detector.nms_threshold = static_cast<float>(ini.GetDoubleValue(
            "detector", "nms_threshold", candidate.detector.nms_threshold));
        candidate.detector.top_k = static_cast<int>(ini.GetLongValue(
            "detector", "top_k", candidate.detector.top_k));
        candidate.detector.output_format = parse_output_format(
            ini.GetValue("detector", "output_format"),
            candidate.detector.output_format);
        candidate.detector.enable_fp16 = ini.GetBoolValue(
            "detector", "enable_fp16", candidate.detector.enable_fp16);
        candidate.detector.enable_trt_cuda_graph = ini.GetBoolValue(
            "detector", "enable_trt_cuda_graph",
            candidate.detector.enable_trt_cuda_graph);
        candidate.detector.enable_gpu_preprocess = ini.GetBoolValue(
            "detector", "enable_gpu_preprocess",
            candidate.detector.enable_gpu_preprocess);
        candidate.detector.trt_cache_path = ini.GetValue(
            "detector", "trt_cache_path",
            candidate.detector.trt_cache_path.c_str());

        candidate.capture.backend = parse_capture_backend(
            ini.GetValue("capture", "backend"),
            candidate.capture.backend);
        candidate.capture.adapter_index = static_cast<int>(ini.GetLongValue(
            "capture", "adapter_index", candidate.capture.adapter_index));
        candidate.capture.output_index = static_cast<int>(ini.GetLongValue(
            "capture", "output_index", candidate.capture.output_index));
        candidate.capture.enable_d3d11_cuda_interop = ini.GetBoolValue(
            "capture", "enable_d3d11_cuda_interop",
            candidate.capture.enable_d3d11_cuda_interop);
        candidate.capture.enable_d3d11_directml_interop =
            ini.GetBoolValue(
                "capture", "enable_d3d11_directml_interop",
                candidate.capture.enable_d3d11_directml_interop);
        candidate.capture.udp_url = ini.GetValue(
            "capture", "udp_url", candidate.capture.udp_url.c_str());
        candidate.capture.udp_read_timeout_ms = static_cast<int>(
            ini.GetLongValue("capture", "udp_read_timeout_ms",
                             candidate.capture.udp_read_timeout_ms));
        candidate.capture.udp_disconnect_timeout_ms = static_cast<int>(
            ini.GetLongValue("capture", "udp_disconnect_timeout_ms",
                             candidate.capture.udp_disconnect_timeout_ms));
        candidate.capture.udp_frame_layout = parse_network_frame_layout(
            ini.GetValue("capture", "udp_frame_layout"),
            candidate.capture.udp_frame_layout);
        candidate.capture.udp_source_width = static_cast<int>(
            ini.GetLongValue("capture", "udp_source_width",
                             candidate.capture.udp_source_width));
        candidate.capture.udp_source_height = static_cast<int>(
            ini.GetLongValue("capture", "udp_source_height",
                             candidate.capture.udp_source_height));
        candidate.capture.ndi_source_name = ini.GetValue(
            "capture", "ndi_source_name",
            candidate.capture.ndi_source_name.c_str());
        candidate.capture.ndi_discovery_timeout_ms = static_cast<int>(
            ini.GetLongValue("capture", "ndi_discovery_timeout_ms",
                             candidate.capture.ndi_discovery_timeout_ms));
        candidate.capture.ndi_receive_timeout_ms = static_cast<int>(
            ini.GetLongValue("capture", "ndi_receive_timeout_ms",
                             candidate.capture.ndi_receive_timeout_ms));
        candidate.capture.ndi_disconnect_timeout_ms = static_cast<int>(
            ini.GetLongValue("capture", "ndi_disconnect_timeout_ms",
                             candidate.capture.ndi_disconnect_timeout_ms));
        candidate.capture.ndi_frame_layout = parse_network_frame_layout(
            ini.GetValue("capture", "ndi_frame_layout"),
            candidate.capture.ndi_frame_layout);
        candidate.capture.ndi_source_width = static_cast<int>(
            ini.GetLongValue("capture", "ndi_source_width",
                             candidate.capture.ndi_source_width));
        candidate.capture.ndi_source_height = static_cast<int>(
            ini.GetLongValue("capture", "ndi_source_height",
                             candidate.capture.ndi_source_height));
        candidate.capture.ndi_require_frame_metadata = ini.GetBoolValue(
            "capture", "ndi_require_frame_metadata",
            candidate.capture.ndi_require_frame_metadata);
        candidate.capture.roi_width = static_cast<int>(ini.GetLongValue(
            "capture", "roi_width", candidate.capture.roi_width));
        candidate.capture.roi_height = static_cast<int>(ini.GetLongValue(
            "capture", "roi_height", candidate.capture.roi_height));
        candidate.capture.center_roi = ini.GetBoolValue(
            "capture", "center_roi", candidate.capture.center_roi);
        candidate.capture.roi_x = static_cast<int>(ini.GetLongValue(
            "capture", "roi_x", candidate.capture.roi_x));
        candidate.capture.roi_y = static_cast<int>(ini.GetLongValue(
            "capture", "roi_y", candidate.capture.roi_y));
        candidate.capture.acquire_timeout_ms = static_cast<int>(ini.GetLongValue(
            "capture", "acquire_timeout_ms",
            candidate.capture.acquire_timeout_ms));

        candidate.aim.person_class_ids = parse_int_list(
            ini.GetValue("aim", "person_class_ids"),
            candidate.aim.person_class_ids);
        candidate.aim.head_class_ids = parse_int_list(
            ini.GetValue("aim", "head_class_ids"),
            candidate.aim.head_class_ids);
#define XEN_READ_AIM_FLOAT(name) \
        candidate.aim.name = static_cast<float>(ini.GetDoubleValue( \
            "aim", #name, candidate.aim.name))
#define XEN_READ_AIM_INT(name) \
        candidate.aim.name = static_cast<int>(ini.GetLongValue( \
            "aim", #name, candidate.aim.name))
        XEN_READ_AIM_FLOAT(high_confidence);
        XEN_READ_AIM_FLOAT(low_confidence);
        XEN_READ_AIM_INT(min_confirmed_hits);
        XEN_READ_AIM_INT(max_lost_frames);
        XEN_READ_AIM_FLOAT(min_iou);
        XEN_READ_AIM_FLOAT(max_center_distance);
        XEN_READ_AIM_FLOAT(switch_margin);
        XEN_READ_AIM_INT(switch_confirm_frames);
        XEN_READ_AIM_INT(switch_cooldown_frames);
        XEN_READ_AIM_FLOAT(acquisition_range_percent);
        XEN_READ_AIM_FLOAT(body_aim_height_ratio);
        XEN_READ_AIM_FLOAT(body_aim_range_percent);
        XEN_READ_AIM_FLOAT(deadzone_pixels);
        XEN_READ_AIM_FLOAT(smoothing);
        XEN_READ_AIM_FLOAT(counts_per_pixel_x);
        XEN_READ_AIM_FLOAT(counts_per_pixel_y);
        XEN_READ_AIM_FLOAT(max_counts_per_frame);
        candidate.aim.enable_delay_compensation = ini.GetBoolValue(
            "aim", "enable_delay_compensation",
            candidate.aim.enable_delay_compensation);
        XEN_READ_AIM_FLOAT(control_delay_ms);
        XEN_READ_AIM_FLOAT(max_delay_compensation_ms);
        XEN_READ_AIM_FLOAT(max_delay_compensation_percent);
        candidate.aim.enable_prediction = ini.GetBoolValue(
            "aim", "enable_prediction",
            candidate.aim.enable_prediction);
        XEN_READ_AIM_FLOAT(max_prediction_lead_percent);
        XEN_READ_AIM_FLOAT(predicted_gain);
#undef XEN_READ_AIM_FLOAT
#undef XEN_READ_AIM_INT

        candidate.mouse.backend = parse_mouse_backend(
            ini.GetValue("mouse", "backend"), candidate.mouse.backend);
        candidate.mouse.allow_send_input = ini.GetBoolValue(
            "mouse", "allow_send_input",
            candidate.mouse.allow_send_input);
        candidate.mouse.kmbox_ip = ini.GetValue(
            "mouse", "kmbox_ip", candidate.mouse.kmbox_ip.c_str());
        candidate.mouse.kmbox_port = static_cast<int>(ini.GetLongValue(
            "mouse", "kmbox_port", candidate.mouse.kmbox_port));
        candidate.mouse.kmbox_uuid = ini.GetValue(
            "mouse", "kmbox_uuid", candidate.mouse.kmbox_uuid.c_str());
        candidate.mouse.kmbox_connect_timeout_ms = static_cast<int>(
            ini.GetLongValue(
                "mouse", "kmbox_connect_timeout_ms",
                candidate.mouse.kmbox_connect_timeout_ms));
        candidate.mouse.kmbox_command_timeout_ms = static_cast<int>(
            ini.GetLongValue(
                "mouse", "kmbox_command_timeout_ms",
                candidate.mouse.kmbox_command_timeout_ms));
        candidate.mouse.makcu_port = ini.GetValue(
            "mouse", "makcu_port", candidate.mouse.makcu_port.c_str());
        candidate.mouse.makcu_baud_rate = static_cast<int>(ini.GetLongValue(
            "mouse", "makcu_baud_rate", candidate.mouse.makcu_baud_rate));
        candidate.mouse.makcu_connect_timeout_ms = static_cast<int>(
            ini.GetLongValue(
                "mouse", "makcu_connect_timeout_ms",
                candidate.mouse.makcu_connect_timeout_ms));
        candidate.mouse.makcu_command_timeout_ms = static_cast<int>(
            ini.GetLongValue(
                "mouse", "makcu_command_timeout_ms",
                candidate.mouse.makcu_command_timeout_ms));
        const auto load_virtual_keys = [&](const char* plural_key,
                                           const char* legacy_key,
                                           const std::vector<int>& fallback) {
            const char* plural_value = ini.GetValue("keyboard", plural_key);
            if (plural_value) {
                // 复数字段显式允许空值，以便用户清空后禁用对应功能。
                if (*plural_value == '\0') return std::vector<int>{};
                return parse_int_list(plural_value, fallback);
            }
            const char* legacy_value = ini.GetValue("keyboard", legacy_key);
            if (!legacy_value) return fallback;
            const long legacy = ini.GetLongValue(
                "keyboard", legacy_key, -1);
            return legacy >= 1 && legacy <= 0xFF
                ? std::vector<int>{static_cast<int>(legacy)}
                : fallback;
        };
        candidate.keyboard.aim_hold_virtual_keys = load_virtual_keys(
            "aim_hold_virtual_keys", "aim_hold_virtual_key",
            candidate.keyboard.aim_hold_virtual_keys);
        candidate.keyboard.emergency_virtual_keys = load_virtual_keys(
            "emergency_virtual_keys", "emergency_virtual_key",
            candidate.keyboard.emergency_virtual_keys);
        candidate.keyboard.runtime_toggle_virtual_keys = load_virtual_keys(
            "runtime_toggle_virtual_keys", "runtime_toggle_virtual_key",
            candidate.keyboard.runtime_toggle_virtual_keys);
        candidate.runtime.profile_window = static_cast<int>(ini.GetLongValue(
            "runtime", "profile_window", candidate.runtime.profile_window));
        candidate.ui.width = static_cast<int>(ini.GetLongValue(
            "ui", "width", candidate.ui.width));
        candidate.ui.height = static_cast<int>(ini.GetLongValue(
            "ui", "height", candidate.ui.height));
        candidate.ui.enable_vsync = ini.GetBoolValue(
            "ui", "enable_vsync", candidate.ui.enable_vsync);
        candidate.ui.open_detached_preview_on_start = ini.GetBoolValue(
            "ui", "open_detached_preview_on_start",
            candidate.ui.open_detached_preview_on_start);
        candidate.ui.theme = parse_ui_theme(
            ini.GetValue("ui", "theme"), candidate.ui.theme);

        if (!validate_app_config(candidate, error)) return false;
        config = std::move(candidate);
        return true;
    } catch (...) {
        set_error(error, "读取配置时发生未知异常");
        return false;
    }
}

bool load_or_create_app_config(const std::string& path,
                               AppConfig& config,
                               bool& created,
                               std::string& error) noexcept {
    created = false;
    if (load_app_config(path, config, error)) return true;

    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error || exists) {
        return false;
    }

    // 此时 config 仍是调用方传入的代码默认值。只为真正缺失的文件生成完整配置，
    // 避免把拼写错误或被截断的现有配置静默恢复为默认值。
    if (!save_app_config(path, config, error)) return false;
    created = true;
    return true;
}

bool save_app_config(const std::string& path,
                     const AppConfig& config,
                     std::string& error) noexcept {
    try {
        if (!validate_app_config(config, error)) return false;
        CSimpleIniA ini;
        ini.SetUnicode(true);
        ini.SetValue("detector", "model_path", config.detector.model_path.c_str());
        ini.SetValue("detector", "backend", backend_name(config.detector.backend));
        ini.SetLongValue("detector", "device_id", config.detector.device_id);
        ini.SetValue(
            "detector", "openvino_device",
            openvino_device_name(config.detector.openvino_device));
        ini.SetLongValue("detector", "input_width", config.detector.input_width);
        ini.SetLongValue("detector", "input_height", config.detector.input_height);
        ini.SetDoubleValue("detector", "conf_threshold", config.detector.conf_threshold);
        ini.SetDoubleValue("detector", "nms_threshold", config.detector.nms_threshold);
        ini.SetLongValue("detector", "top_k", config.detector.top_k);
        ini.SetValue("detector", "output_format",
                     output_format_name(config.detector.output_format));
        ini.SetBoolValue("detector", "enable_fp16", config.detector.enable_fp16);
        ini.SetBoolValue("detector", "enable_trt_cuda_graph",
                         config.detector.enable_trt_cuda_graph);
        ini.SetBoolValue("detector", "enable_gpu_preprocess",
                         config.detector.enable_gpu_preprocess);
        ini.SetValue("detector", "trt_cache_path",
                     config.detector.trt_cache_path.c_str());

        ini.SetValue("capture", "backend",
                     capture_backend_name(config.capture.backend));
        ini.SetLongValue("capture", "adapter_index", config.capture.adapter_index);
        ini.SetLongValue("capture", "output_index", config.capture.output_index);
        ini.SetBoolValue("capture", "enable_d3d11_cuda_interop",
                         config.capture.enable_d3d11_cuda_interop);
        ini.SetBoolValue("capture", "enable_d3d11_directml_interop",
                         config.capture.enable_d3d11_directml_interop);
        ini.SetValue("capture", "udp_url", config.capture.udp_url.c_str());
        ini.SetLongValue("capture", "udp_read_timeout_ms",
                         config.capture.udp_read_timeout_ms);
        ini.SetLongValue("capture", "udp_disconnect_timeout_ms",
                         config.capture.udp_disconnect_timeout_ms);
        ini.SetValue("capture", "udp_frame_layout",
                     network_frame_layout_name(config.capture.udp_frame_layout));
        ini.SetLongValue("capture", "udp_source_width",
                         config.capture.udp_source_width);
        ini.SetLongValue("capture", "udp_source_height",
                         config.capture.udp_source_height);
        ini.SetValue("capture", "ndi_source_name",
                     config.capture.ndi_source_name.c_str());
        ini.SetLongValue("capture", "ndi_discovery_timeout_ms",
                         config.capture.ndi_discovery_timeout_ms);
        ini.SetLongValue("capture", "ndi_receive_timeout_ms",
                         config.capture.ndi_receive_timeout_ms);
        ini.SetLongValue("capture", "ndi_disconnect_timeout_ms",
                         config.capture.ndi_disconnect_timeout_ms);
        ini.SetValue("capture", "ndi_frame_layout",
                     network_frame_layout_name(config.capture.ndi_frame_layout));
        ini.SetLongValue("capture", "ndi_source_width",
                         config.capture.ndi_source_width);
        ini.SetLongValue("capture", "ndi_source_height",
                         config.capture.ndi_source_height);
        ini.SetBoolValue("capture", "ndi_require_frame_metadata",
                         config.capture.ndi_require_frame_metadata);
        ini.SetLongValue("capture", "roi_width", config.capture.roi_width);
        ini.SetLongValue("capture", "roi_height", config.capture.roi_height);
        ini.SetBoolValue("capture", "center_roi", config.capture.center_roi);
        ini.SetLongValue("capture", "roi_x", config.capture.roi_x);
        ini.SetLongValue("capture", "roi_y", config.capture.roi_y);
        ini.SetLongValue("capture", "acquire_timeout_ms",
                         config.capture.acquire_timeout_ms);

        ini.SetValue("aim", "person_class_ids",
                     format_int_list(config.aim.person_class_ids).c_str());
        ini.SetValue("aim", "head_class_ids",
                     format_int_list(config.aim.head_class_ids).c_str());
#define XEN_WRITE_AIM_FLOAT(name) \
        ini.SetDoubleValue("aim", #name, config.aim.name)
#define XEN_WRITE_AIM_INT(name) \
        ini.SetLongValue("aim", #name, config.aim.name)
        XEN_WRITE_AIM_FLOAT(high_confidence);
        XEN_WRITE_AIM_FLOAT(low_confidence);
        XEN_WRITE_AIM_INT(min_confirmed_hits);
        XEN_WRITE_AIM_INT(max_lost_frames);
        XEN_WRITE_AIM_FLOAT(min_iou);
        XEN_WRITE_AIM_FLOAT(max_center_distance);
        XEN_WRITE_AIM_FLOAT(switch_margin);
        XEN_WRITE_AIM_INT(switch_confirm_frames);
        XEN_WRITE_AIM_INT(switch_cooldown_frames);
        XEN_WRITE_AIM_FLOAT(acquisition_range_percent);
        XEN_WRITE_AIM_FLOAT(body_aim_height_ratio);
        XEN_WRITE_AIM_FLOAT(body_aim_range_percent);
        XEN_WRITE_AIM_FLOAT(deadzone_pixels);
        XEN_WRITE_AIM_FLOAT(smoothing);
        XEN_WRITE_AIM_FLOAT(counts_per_pixel_x);
        XEN_WRITE_AIM_FLOAT(counts_per_pixel_y);
        XEN_WRITE_AIM_FLOAT(max_counts_per_frame);
        ini.SetBoolValue("aim", "enable_delay_compensation",
                         config.aim.enable_delay_compensation);
        XEN_WRITE_AIM_FLOAT(control_delay_ms);
        XEN_WRITE_AIM_FLOAT(max_delay_compensation_ms);
        XEN_WRITE_AIM_FLOAT(max_delay_compensation_percent);
        ini.SetBoolValue("aim", "enable_prediction",
                         config.aim.enable_prediction);
        XEN_WRITE_AIM_FLOAT(max_prediction_lead_percent);
        XEN_WRITE_AIM_FLOAT(predicted_gain);
#undef XEN_WRITE_AIM_FLOAT
#undef XEN_WRITE_AIM_INT

        ini.SetValue("mouse", "backend",
                     mouse_backend_name(config.mouse.backend));
        ini.SetBoolValue("mouse", "allow_send_input",
                         config.mouse.allow_send_input);
        ini.SetValue("mouse", "kmbox_ip", config.mouse.kmbox_ip.c_str());
        ini.SetLongValue("mouse", "kmbox_port", config.mouse.kmbox_port);
        ini.SetValue("mouse", "kmbox_uuid",
                     config.mouse.kmbox_uuid.c_str());
        ini.SetLongValue("mouse", "kmbox_connect_timeout_ms",
                         config.mouse.kmbox_connect_timeout_ms);
        ini.SetLongValue("mouse", "kmbox_command_timeout_ms",
                         config.mouse.kmbox_command_timeout_ms);
        ini.SetValue("mouse", "makcu_port",
                     config.mouse.makcu_port.c_str());
        ini.SetLongValue("mouse", "makcu_baud_rate",
                         config.mouse.makcu_baud_rate);
        ini.SetLongValue("mouse", "makcu_connect_timeout_ms",
                         config.mouse.makcu_connect_timeout_ms);
        ini.SetLongValue("mouse", "makcu_command_timeout_ms",
                         config.mouse.makcu_command_timeout_ms);
        ini.SetValue(
            "keyboard", "aim_hold_virtual_keys",
            format_int_list(config.keyboard.aim_hold_virtual_keys).c_str());
        ini.SetValue(
            "keyboard", "emergency_virtual_keys",
            format_int_list(config.keyboard.emergency_virtual_keys).c_str());
        ini.SetValue(
            "keyboard", "runtime_toggle_virtual_keys",
            format_int_list(config.keyboard.runtime_toggle_virtual_keys).c_str());
        ini.SetValue("log", "global_level",
                     log_level_name(config.log.global_level));
        ini.SetBoolValue("log", "enable_console",
                         config.log.enable_console);
        ini.SetBoolValue("log", "enable_file", config.log.enable_file);
        ini.SetBoolValue("log", "enable_debug_file",
                         config.log.enable_debug_file);
        ini.SetBoolValue("log", "enable_ringbuf",
                         config.log.enable_ringbuf);
        ini.SetLongValue("log", "ringbuf_capacity",
                         config.log.ringbuf_capacity);
        ini.SetValue("log", "log_dir", config.log.log_dir.c_str());
        ini.SetLongValue("log", "file_max_size_mb",
                         config.log.file_max_size_mb);
        ini.SetLongValue("log", "file_max_count",
                         config.log.file_max_count);
        std::vector<std::pair<std::string, LogLevel>> module_levels(
            config.log.module_levels.begin(),
            config.log.module_levels.end());
        std::sort(
            module_levels.begin(), module_levels.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        for (const auto& [module, level] : module_levels) {
            ini.SetValue(
                "log_modules", module.c_str(), log_level_name(level));
        }
        ini.SetLongValue("runtime", "profile_window",
                         config.runtime.profile_window);
        ini.SetLongValue("ui", "width", config.ui.width);
        ini.SetLongValue("ui", "height", config.ui.height);
        ini.SetBoolValue("ui", "enable_vsync", config.ui.enable_vsync);
        ini.SetBoolValue(
            "ui", "open_detached_preview_on_start",
            config.ui.open_detached_preview_on_start);
        ini.SetValue("ui", "theme", ui_theme_name(config.ui.theme));

        if (ini.SaveFile(path.c_str()) < 0) {
            error = "无法写入配置文件: " + path;
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "写入配置时发生未知异常");
        return false;
    }
}
