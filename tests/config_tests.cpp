#include "config/config.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void test_round_trip() {
    AppConfig source;
    source.detector.model_path = "models/test.onnx";
    source.detector.backend = BackendType::TENSORRT;
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
    source.capture.ndi_frame_layout =
        NetworkFrameLayout::CENTER_CROP_1_TO_1;
    source.capture.ndi_source_width = 2560;
    source.capture.ndi_source_height = 1440;
    source.capture.ndi_require_frame_metadata = false;
    source.aim.person_class_ids = {0, 2};
    source.aim.head_class_ids = {1, 3};
    source.mouse.allow_send_input = true;
    source.ui.width = 1024;
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
           loaded.capture.ndi_frame_layout ==
               NetworkFrameLayout::CENTER_CROP_1_TO_1 &&
           loaded.capture.ndi_source_width == 2560 &&
           loaded.capture.ndi_source_height == 1440 &&
           !loaded.capture.ndi_require_frame_metadata &&
           loaded.aim.person_class_ids == source.aim.person_class_ids &&
           loaded.mouse.allow_send_input && loaded.ui.width == 1024 &&
           loaded.ui.theme == UiTheme::DARK,
           "配置往返后关键字段必须保持一致");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_invalid_config() {
    AppConfig config;
    std::string error;
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
    config.capture.backend = CaptureBackend::UDP_MJPEG;
    config.capture.udp_read_timeout_ms = 500;
    config.capture.udp_disconnect_timeout_ms = 100;
    expect(!validate_app_config(config, error),
           "UDP 断流判定短于单次读取超时时必须拒绝配置");
    config.capture.udp_disconnect_timeout_ms = 1000;
    config.capture.udp_frame_layout =
        UdpFrameLayout::CENTER_CROP_1_TO_1;
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

} // namespace

int main() {
    test_round_trip();
    test_invalid_config();
    test_xudp_backend_round_trip();
    if (failures != 0) {
        std::cerr << "Config 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Config 测试全部通过。\n";
    return 0;
}
