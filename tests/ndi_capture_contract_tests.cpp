#include "ndi_capture_test_system.h"

#include <iostream>
#include <memory>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[失败] " << message << '\n';
        ++failures;
    }
}

void test_reused_frame_does_not_inherit_source_timing() {
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(
        ndi_test::config(), std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "注入 SDK 的真实 NDI Capture 必须打开");
        return;
    }
    CapturedFrame frame;
    input->send_video({});
    if (!ndi_test::receive(*capture, frame)) {
        expect(false, "必须从 ICapture::grab 收到合法首帧");
        return;
    }
    const auto* first_slot = frame.bgr_storage.get();
    const auto* first_pixels = frame.bgr.data;
    expect(frame.timing.source_time_timing_valid &&
               frame.timing.source_clock_status == SourceClockStatus::VALID &&
               frame.timing.source_time_at < frame.timing.captured_at,
           "合法首帧必须携带当前 source 映射");
    ndi_test::release(frame);

    input->send_video({});
    if (!ndi_test::receive(*capture, frame)) {
        expect(false, "必须消费第二帧使首帧槽可再次使用");
        return;
    }
    ndi_test::release(frame);

    ndi_test::VideoInput missing;
    missing.blue = 99;
    missing.timestamp = NDIlib_recv_timestamp_undefined;
    missing.timecode = NDIlib_send_timecode_synthesize;
    missing.frame_rate_n = 0;
    missing.frame_rate_d = 0;
    input->send_video(std::move(missing));
    if (!ndi_test::receive(*capture, frame)) {
        expect(false, "缺失 timestamp 的本帧仍必须可作为普通图像消费");
        return;
    }
    expect(frame.bgr_storage.get() == first_slot && frame.bgr.data == first_pixels &&
               frame.bgr.at<cv::Vec3b>(0, 0)[0] == 99,
           "第三帧必须复用首帧槽和 Mat 存储并携带本帧像素");
    expect(frame.timing.sequence == 3 &&
               !frame.timing.source_timestamp_valid &&
               frame.timing.source_timestamp == NDIlib_recv_timestamp_undefined &&
               !frame.timing.source_timecode_valid && frame.timing.source_fps == 0.0 &&
               frame.timing.source_time_basis == SourceTimeBasis::UNAVAILABLE &&
               frame.timing.source_clock_status == SourceClockStatus::UNSYNCHRONIZED &&
               !frame.timing.source_time_timing_valid &&
               frame.timing.source_time_at == std::chrono::steady_clock::time_point{} &&
               frame.timing.source_clock_uncertainty_ms == 0.0 &&
               frame.timing.source_clock_round_trip_ms == 0.0 &&
               frame.timing.source_clock_rate == 1.0 &&
               frame.timing.source_clock_mapping_age_ms == 0.0 &&
               frame.timing.source_clock_sample_count == 0 &&
               frame.timing.source_clock_session_id == 0,
           "同槽 undefined 帧不得继承上一帧 fps、basis、映射时刻或 clock 诊断");
    capture->close();
}

void test_reused_frame_keeps_current_invalid_mapping(
        ndi_test::MappingMode mapping, SourceClockStatus expected_status) {
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(
        ndi_test::config(), std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "映射状态转换 fixture 必须打开真实 NDI Capture");
        return;
    }
    CapturedFrame frame;
    const cv::Mat* first_slot = nullptr;
    const std::uint8_t* first_pixels = nullptr;
    for (int index = 0; index < 2; ++index) {
        input->send_video({});
        if (!ndi_test::receive(*capture, frame)) {
            expect(false, "映射状态转换必须先消费两张 VALID 帧");
            return;
        }
        expect(frame.timing.source_time_timing_valid,
               "状态转换前必须确实具有 VALID 映射");
        if (index == 0) {
            first_slot = frame.bgr_storage.get();
            first_pixels = frame.bgr.data;
        }
        ndi_test::release(frame);
    }
    ndi_test::VideoInput current;
    current.mapping = mapping;
    current.timestamp = 2000000;
    current.blue = 81;
    input->send_video(std::move(current));
    if (!ndi_test::receive(*capture, frame)) {
        expect(false, "STALE 或未来映射的图像必须仍可消费");
        return;
    }
    expect(frame.bgr_storage.get() == first_slot && frame.bgr.data == first_pixels &&
               frame.bgr.at<cv::Vec3b>(0, 0)[0] == 81,
           "无效映射对照必须经过同一槽和 Mat 存储复用");
    expect(frame.timing.sequence == 3 && frame.timing.source_timestamp_valid &&
               frame.timing.source_timestamp == 2000000 &&
               frame.timing.source_time_basis == SourceTimeBasis::NDI_SDK_SUBMISSION &&
               frame.timing.source_clock_status == expected_status &&
               !frame.timing.source_time_timing_valid &&
               frame.timing.source_time_at == std::chrono::steady_clock::time_point{} &&
               frame.timing.source_clock_uncertainty_ms == 0.25 &&
               frame.timing.source_clock_round_trip_ms == 0.5 &&
               frame.timing.source_clock_rate == 1.0001 &&
               frame.timing.source_clock_mapping_age_ms == 2.0 &&
               frame.timing.source_clock_sample_count == 8 &&
               frame.timing.source_clock_session_id == 73,
           "无效映射不得继承旧时刻，同时必须保留当前 status 与 clock 诊断");
    capture->close();
}

void test_required_metadata_does_not_accept_configuration_fallback() {
    auto config = ndi_test::config();
    config.roi_width = 160;
    config.roi_height = 160;
    config.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.ndi_source_width = 2560;
    config.ndi_source_height = 1440;
    config.ndi_require_frame_metadata = true;
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(config, std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "required metadata 配置与合法 fallback 必须可打开");
        return;
    }
    ndi_test::VideoInput video;
    video.width = 320;
    video.height = 320;
    // XML 合法，但其 320x320 ROI 不满足本次请求的 160x160。
    // 配置仍可独立导出中心 (1200,640)，这不能冒充采用 metadata 的 (100,200)。
    video.metadata =
        "<xen version=\"1\" source_width=\"2560\" source_height=\"1440\" "
        "roi_x=\"100\" roi_y=\"200\" roi_width=\"320\" roi_height=\"320\"/>";
    input->send_video(std::move(video));
    CapturedFrame frame;
    const bool received = ndi_test::receive(*capture, frame);
    expect(!received && frame.bgr.empty() &&
               capture->status() == CaptureStatus::INVALID_CONFIG,
           "required 必须拒绝未采用 XML metadata、仅配置 fallback 可算的帧");
    capture->close();
}

CaptureConfig metadata_config(bool required) {
    auto config = ndi_test::config();
    config.roi_width = 160;
    config.roi_height = 160;
    config.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.ndi_source_width = 2560;
    config.ndi_source_height = 1440;
    config.ndi_require_frame_metadata = required;
    return config;
}

void test_optional_metadata_keeps_configuration_fallback() {
    for (const bool with_metadata : {false, true}) {
        auto system = std::make_unique<ndi_test::CaptureSystem>();
        auto* input = system.get();
        auto capture = capture::detail::create_ndi_capture(
            metadata_config(false), std::move(system));
        if (!capture || !capture->open()) {
            expect(false, "optional fallback 配置必须可打开");
            return;
        }
        ndi_test::VideoInput video;
        video.width = 320;
        video.height = 320;
        if (with_metadata) {
            video.metadata =
                "<xen version=\"1\" source_width=\"2560\" source_height=\"1440\" "
                "roi_x=\"100\" roi_y=\"200\" roi_width=\"320\" roi_height=\"320\"/>";
        }
        input->send_video(std::move(video));
        CapturedFrame frame;
        const bool received = ndi_test::receive(*capture, frame);
        expect(received && frame.width == 160 && frame.height == 160 &&
                   frame.source_width == 2560 && frame.source_height == 1440 &&
                   frame.roi_x == 1200.0 && frame.roi_y == 640.0 &&
                   frame.source_pixels_per_pixel_x == 1.0 &&
                   frame.source_pixels_per_pixel_y == 1.0,
               "optional 缺失或不兼容 metadata 必须保持合法配置的中心 fallback");
        capture->close();
    }
}

void test_compatible_and_cached_metadata_are_used(bool required) {
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(
        metadata_config(required), std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "兼容 metadata 配置必须可打开");
        return;
    }
    CapturedFrame frame;
    for (int index = 0; index < 2; ++index) {
        ndi_test::VideoInput video;
        video.width = 160;
        video.height = 160;
        video.blue = static_cast<std::uint8_t>(31 + index);
        if (index == 0) {
            video.metadata =
                "<xen version=\"1\" source_width=\"2560\" source_height=\"1440\" "
                "roi_x=\"100\" roi_y=\"200\" roi_width=\"160\" roi_height=\"160\"/>";
        }
        input->send_video(std::move(video));
        const bool received = ndi_test::receive(*capture, frame);
        expect(received && frame.width == 160 && frame.height == 160 &&
                   frame.source_width == 2560 && frame.source_height == 1440 &&
                   frame.roi_x == 100.0 && frame.roi_y == 200.0 &&
                   frame.source_pixels_per_pixel_x == 1.0 &&
                   frame.source_pixels_per_pixel_y == 1.0 &&
                   frame.bgr.at<cv::Vec3b>(0, 0)[0] == 31 + index,
               "required 与 optional 均须采用兼容 metadata，并保留后帧使用既有缓存的行为");
        if (!received) return;
        ndi_test::release(frame);
    }
    capture->close();
}

void test_required_missing_metadata_is_rejected() {
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(
        metadata_config(true), std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "required 缺失 metadata 对照必须打开独立新会话");
        return;
    }
    ndi_test::VideoInput video;
    video.width = 320;
    video.height = 320;
    input->send_video(std::move(video));
    CapturedFrame frame;
    const bool received = ndi_test::receive(*capture, frame);
    expect(!received && frame.bgr.empty() &&
               capture->status() == CaptureStatus::INVALID_CONFIG,
           "required 新会话既无随帧 metadata 又无缓存时必须明确拒绝");
    capture->close();
}

} // namespace

int main() {
    test_reused_frame_does_not_inherit_source_timing();
    test_reused_frame_keeps_current_invalid_mapping(
        ndi_test::MappingMode::STALE, SourceClockStatus::STALE);
    test_reused_frame_keeps_current_invalid_mapping(
        ndi_test::MappingMode::FUTURE, SourceClockStatus::INVALID);
    test_required_metadata_does_not_accept_configuration_fallback();
    test_optional_metadata_keeps_configuration_fallback();
    test_compatible_and_cached_metadata_are_used(false);
    test_compatible_and_cached_metadata_are_used(true);
    test_required_missing_metadata_is_rejected();
    if (failures != 0) {
        std::cerr << "NDI 采集合同失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "NDI 采集合同测试通过\n";
    return 0;
}
