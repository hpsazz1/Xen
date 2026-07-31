#include "config/config.h"

#include <SimpleIni.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
    if (normalized == "cpu") return BackendType::CPU;
    return fallback;
}

const char* backend_name(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::CUDA: return "cuda";
        case BackendType::TENSORRT: return "tensorrt";
        case BackendType::DIRECTML: return "directml";
        case BackendType::CPU: return "cpu";
    }
    return "cpu";
}

CaptureBackend parse_capture_backend(
        const char* value, CaptureBackend fallback) {
    if (!value) return fallback;
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "desktop_duplication") {
        return CaptureBackend::DESKTOP_DUPLICATION;
    }
    if (normalized == "udp_mjpeg") return CaptureBackend::UDP_MJPEG;
    if (normalized == "ndi") return CaptureBackend::NDI;
    return fallback;
}

const char* capture_backend_name(CaptureBackend backend) noexcept {
    switch (backend) {
        case CaptureBackend::DESKTOP_DUPLICATION:
            return "desktop_duplication";
        case CaptureBackend::UDP_MJPEG:
            return "udp_mjpeg";
        case CaptureBackend::NDI:
            return "ndi";
    }
    return "desktop_duplication";
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
        if (config.detector.model_path.empty()) {
            error = "Detector 模型路径不能为空";
            return false;
        }
        if (config.detector.device_id < 0 ||
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
        if (common_capture_invalid || desktop_invalid || udp_invalid ||
            ndi_invalid) {
            error = "Capture 配置非法";
            return false;
        }
        if (!finite_unit(config.aim.high_confidence) ||
            !finite_unit(config.aim.low_confidence) ||
            config.aim.high_confidence < config.aim.low_confidence ||
            config.aim.min_confirmed_hits <= 0 ||
            config.aim.max_lost_frames < 0 ||
            config.aim.max_counts_per_frame <= 0.0f) {
            error = "Aim 配置非法";
            return false;
        }
        if (config.keyboard.aim_hold_virtual_key <= 0 ||
            config.keyboard.emergency_virtual_key <= 0 ||
            config.runtime.profile_window < 64 ||
            config.runtime.profile_window > 4096 ||
            config.ui.width < 640 || config.ui.height < 480 ||
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
        candidate.detector.model_path =
            ini.GetValue("detector", "model_path",
                         candidate.detector.model_path.c_str());
        candidate.detector.backend = parse_backend(
            ini.GetValue("detector", "backend"),
            candidate.detector.backend);
        candidate.detector.device_id = static_cast<int>(ini.GetLongValue(
            "detector", "device_id", candidate.detector.device_id));
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
        XEN_READ_AIM_FLOAT(body_aim_height_ratio);
        XEN_READ_AIM_FLOAT(deadzone_pixels);
        XEN_READ_AIM_FLOAT(smoothing);
        XEN_READ_AIM_FLOAT(counts_per_pixel_x);
        XEN_READ_AIM_FLOAT(counts_per_pixel_y);
        XEN_READ_AIM_FLOAT(max_counts_per_frame);
        XEN_READ_AIM_FLOAT(predicted_gain);
#undef XEN_READ_AIM_FLOAT
#undef XEN_READ_AIM_INT

        candidate.mouse.allow_send_input = ini.GetBoolValue(
            "mouse", "allow_send_input",
            candidate.mouse.allow_send_input);
        candidate.keyboard.aim_hold_virtual_key = static_cast<int>(
            ini.GetLongValue("keyboard", "aim_hold_virtual_key",
                             candidate.keyboard.aim_hold_virtual_key));
        candidate.keyboard.emergency_virtual_key = static_cast<int>(
            ini.GetLongValue("keyboard", "emergency_virtual_key",
                             candidate.keyboard.emergency_virtual_key));
        candidate.runtime.profile_window = static_cast<int>(ini.GetLongValue(
            "runtime", "profile_window", candidate.runtime.profile_window));
        candidate.ui.width = static_cast<int>(ini.GetLongValue(
            "ui", "width", candidate.ui.width));
        candidate.ui.height = static_cast<int>(ini.GetLongValue(
            "ui", "height", candidate.ui.height));
        candidate.ui.enable_vsync = ini.GetBoolValue(
            "ui", "enable_vsync", candidate.ui.enable_vsync);
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
        ini.SetValue("detector", "trt_cache_path",
                     config.detector.trt_cache_path.c_str());

        ini.SetValue("capture", "backend",
                     capture_backend_name(config.capture.backend));
        ini.SetLongValue("capture", "adapter_index", config.capture.adapter_index);
        ini.SetLongValue("capture", "output_index", config.capture.output_index);
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
        XEN_WRITE_AIM_FLOAT(body_aim_height_ratio);
        XEN_WRITE_AIM_FLOAT(deadzone_pixels);
        XEN_WRITE_AIM_FLOAT(smoothing);
        XEN_WRITE_AIM_FLOAT(counts_per_pixel_x);
        XEN_WRITE_AIM_FLOAT(counts_per_pixel_y);
        XEN_WRITE_AIM_FLOAT(max_counts_per_frame);
        XEN_WRITE_AIM_FLOAT(predicted_gain);
#undef XEN_WRITE_AIM_FLOAT
#undef XEN_WRITE_AIM_INT

        ini.SetBoolValue("mouse", "allow_send_input",
                         config.mouse.allow_send_input);
        ini.SetLongValue("keyboard", "aim_hold_virtual_key",
                         config.keyboard.aim_hold_virtual_key);
        ini.SetLongValue("keyboard", "emergency_virtual_key",
                         config.keyboard.emergency_virtual_key);
        ini.SetLongValue("runtime", "profile_window",
                         config.runtime.profile_window);
        ini.SetLongValue("ui", "width", config.ui.width);
        ini.SetLongValue("ui", "height", config.ui.height);
        ini.SetBoolValue("ui", "enable_vsync", config.ui.enable_vsync);
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
