#include "ndi_capture_test_system.h"

#include <atomic>
#include <thread>
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

// 只在 SDK seam 注入接收错误，像素、映射与借用帧仍复用原有 fixture。
class ReconnectingSystem final : public capture::detail::INdiCaptureSystem {
public:
    ndi_test::CaptureSystem input;
    std::atomic<bool> interrupt{false};
    std::atomic<int> created{0};
    std::atomic<bool> invalid_video{false};
    std::atomic<int> invalid_received{0};
    CaptureStatus open(const clock_sync::ClientConfig& c, std::string& e) noexcept override { return input.open(c,e); }
    void close() noexcept override { input.close(); }
    NDIlib_find_instance_t find_create(const NDIlib_find_create_t* s) noexcept override { return input.find_create(s); }
    void find_destroy(NDIlib_find_instance_t f) noexcept override { input.find_destroy(f); }
    bool find_wait(NDIlib_find_instance_t f, std::uint32_t t) noexcept override { return input.find_wait(f,t); }
    const NDIlib_source_t* find_sources(NDIlib_find_instance_t f, std::uint32_t* n) noexcept override { return input.find_sources(f,n); }
    NDIlib_recv_instance_t recv_create(const NDIlib_recv_create_v3_t* s) noexcept override {
        auto receiver = input.recv_create(s);
        if (receiver) ++created;
        return receiver;
    }
    void recv_destroy(NDIlib_recv_instance_t r) noexcept override { input.recv_destroy(r); }
    int recv_connections(NDIlib_recv_instance_t r) noexcept override { return input.recv_connections(r); }
    NDIlib_frame_type_e recv_capture(NDIlib_recv_instance_t r, NDIlib_video_frame_v2_t* v,
            NDIlib_metadata_frame_t* m, std::uint32_t t) noexcept override {
        if (interrupt.exchange(false)) return NDIlib_frame_type_error;
        if (invalid_video.load()) {
            *v = {};
            ++invalid_received;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return NDIlib_frame_type_video;
        }
        return input.recv_capture(r,v,m,t);
    }
    void recv_free_video(NDIlib_recv_instance_t r, NDIlib_video_frame_v2_t* v) noexcept override { input.recv_free_video(r,v); }
    void recv_free_metadata(NDIlib_recv_instance_t r, NDIlib_metadata_frame_t* m) noexcept override { input.recv_free_metadata(r,m); }
    void recv_performance(NDIlib_recv_instance_t r, NDIlib_recv_performance_t* t, NDIlib_recv_performance_t* d) noexcept override { input.recv_performance(r,t,d); }
    void recv_queue(NDIlib_recv_instance_t r, NDIlib_recv_queue_t* q) noexcept override { input.recv_queue(r,q); }
    clock_sync::MappingResult map_source_timestamp(std::int64_t t,
            std::chrono::steady_clock::time_point n) const noexcept override { return input.map_source_timestamp(t,n); }
};

void test_invalid_video_cannot_extend_valid_frame_deadline(bool received_before) {
    auto system = std::make_unique<ReconnectingSystem>();
    auto* input = system.get();
    auto config = ndi_test::config();
    config.ndi_discovery_timeout_ms = 120;
    config.ndi_disconnect_timeout_ms = 120;
    config.ndi_receive_timeout_ms = 5;
    auto capture = capture::detail::create_ndi_capture(config, std::move(system));
    input->invalid_video = !received_before;
    if (!capture || !capture->open()) { expect(false, "无效视频超时fixture必须打开"); return; }
    CapturedFrame frame;
    if (received_before) {
        input->input.send_video({});
        expect(ndi_test::receive(*capture, frame), "断流场景必须先收到合法视频");
        ndi_test::release(frame);
        input->invalid_video = true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (capture->status() != CaptureStatus::ACCESS_LOST &&
           std::chrono::steady_clock::now() < deadline) {
        capture->grab(frame);
    }
    expect(input->invalid_received.load() > 0 && capture->status() == CaptureStatus::ACCESS_LOST,
           received_before ? "持续坏video必须按最后有效视频期限判定断流" :
                             "首帧前持续坏video不能绕过发现期限");
    capture->close();
}

void test_invalid_video_recovers_before_deadline() {
    auto system = std::make_unique<ReconnectingSystem>();
    auto* input = system.get();
    auto config = ndi_test::config();
    config.ndi_discovery_timeout_ms = 1000;
    config.ndi_disconnect_timeout_ms = 1000;
    config.ndi_receive_timeout_ms = 5;
    auto capture = capture::detail::create_ndi_capture(config, std::move(system));
    input->invalid_video = true;
    if (!capture || !capture->open()) { expect(false, "无效视频恢复fixture必须打开"); return; }
    CapturedFrame frame;
    for (int round = 0; round < 2; ++round) {
        const int before = input->invalid_received.load();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (input->invalid_received.load() < before + 5 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        input->input.send_video({});
        input->invalid_video = false;
        expect(ndi_test::receive(*capture, frame) && frame.timing.sequence == round + 1,
               "首次等待与有效帧后坏video在期限内恢复时均须继续发布新帧");
        ndi_test::release(frame);
        input->invalid_video = true;
    }
    capture->close();
}

void test_receiver_generation_survives_internal_reconnect() {
    expect(FrameTiming{}.receiver_generation == 0, "非 NDI 默认帧不声明接收器代次");
    auto system = std::make_unique<ReconnectingSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(ndi_test::config(), std::move(system));
    if (!capture || !capture->open()) { expect(false, "重连 fixture 必须打开"); return; }
    CapturedFrame frame;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        input->input.send_video({});
        if (!ndi_test::receive(*capture, frame)) { expect(false, "重连前必须收到图像"); return; }
        expect(frame.timing.receiver_generation == 1 && frame.timing.sequence == sequence,
               "同一接收器的代次不随帧递增，帧序号保持原语义");
        ndi_test::release(frame);
    }
    input->interrupt = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (input->created.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (input->created.load() != 2) { expect(false, "SDK 接收错误必须触发内部接收器重建"); return; }
    input->input.send_video({});
    if (!ndi_test::receive(*capture, frame)) { expect(false, "重建后必须恢复图像"); return; }
    expect(frame.timing.receiver_generation == 2 && frame.timing.sequence == 3,
           "内部重建必须递增代次，但不能重置现有帧序号");
    ndi_test::release(frame);
    capture->close();
    if (!capture->open()) { expect(false, "同一个 Capture 必须可重新打开"); return; }
    input->input.send_video({});
    if (!ndi_test::receive(*capture, frame)) { expect(false, "重新打开后必须收到图像"); return; }
    expect(frame.timing.receiver_generation == 1 && frame.timing.sequence == 1,
           "显式重新打开必须重置接收器代次和现有帧序号");
    capture->close();
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
                   frame.roi_x == 1200.0 && frame.roi_y == 640.0 && !frame.source_mapping_verified &&
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
                   frame.roi_x == 100.0 && frame.roi_y == 200.0 && frame.source_mapping_verified &&
                   frame.source_pixels_per_pixel_x == 1.0 &&
                   frame.source_pixels_per_pixel_y == 1.0 &&
                   frame.bgr.at<cv::Vec3b>(0, 0)[0] == 31 + index,
               "required 与 optional 均须采用兼容 metadata，并保留后帧使用既有缓存的行为");
        if (!received) return;
        ndi_test::release(frame);
    }
    capture->close();
}

void test_roi320_mapping_evidence_and_slot_reuse(bool encoded_full_frame) {
    expect(!CapturedFrame{}.source_mapping_verified,
           "默认帧不能声明源坐标映射已验证");
    auto config = metadata_config(false);
    config.roi_width = 320;
    config.roi_height = 320;
    auto system = std::make_unique<ndi_test::CaptureSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(config, std::move(system));
    if (!capture || !capture->open()) {
        expect(false, "320 ROI metadata fixture 必须打开");
        return;
    }
    CapturedFrame frame;
    const cv::Mat* first_slot = nullptr;
    for (int index = 0; index < 3; ++index) {
        ndi_test::VideoInput video;
        video.width = encoded_full_frame ? 2560 : 320;
        video.height = encoded_full_frame ? 1440 : 320;
        video.blue = static_cast<std::uint8_t>(41 + index);
        // 第三帧显式无效 XML 不可沿用先前缓存，也不可继承复用槽的证据。
        video.metadata = index < 2
            ? "<xen version=\"1\" source_width=\"2560\" source_height=\"1440\" "
              "roi_x=\"100\" roi_y=\"200\" roi_width=\"320\" roi_height=\"320\"/>"
            : "<xen version=\"invalid\"/>";
        input->send_video(std::move(video));
        if (!ndi_test::receive(*capture, frame)) {
            expect(false, "320 ROI 合法 metadata 与 optional fallback 必须均可消费");
            return;
        }
        expect(frame.width == 320 && frame.height == 320 &&
                   frame.bgr.cols == 320 && frame.bgr.rows == 320 &&
                   frame.source_width == 2560 && frame.source_height == 1440 &&
                   frame.encoded_width == (encoded_full_frame ? 2560 : 320) &&
                   frame.encoded_height == (encoded_full_frame ? 1440 : 320) &&
                   frame.source_pixels_per_pixel_x == 1.0 &&
                   frame.source_pixels_per_pixel_y == 1.0 &&
                   frame.bgr.at<cv::Vec3b>(0, 0)[0] == 41 + index,
               "原始裁剪与全幅后裁剪必须都保留 320 原像素、encoded 尺寸及 1:1 比例");
        expect(frame.source_mapping_verified == (index < 2) &&
                   frame.roi_x == (index < 2 ? 100.0 : 1120.0) &&
                   frame.roi_y == (index < 2 ? 200.0 : 560.0),
               "有效 metadata 才验证原点；配置 fallback 原点仍保留但证据为未知");
        if (index == 0) first_slot = frame.bgr_storage.get();
        if (index == 2)
            expect(frame.bgr_storage.get() == first_slot,
                   "无效 metadata 对照必须复用曾验证的同一帧槽");
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
    test_invalid_video_cannot_extend_valid_frame_deadline(false);
    test_invalid_video_cannot_extend_valid_frame_deadline(true);
    test_invalid_video_recovers_before_deadline();
    test_receiver_generation_survives_internal_reconnect();
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
    test_roi320_mapping_evidence_and_slot_reuse(false);
    test_roi320_mapping_evidence_and_slot_reuse(true);
    if (failures != 0) {
        std::cerr << "NDI 采集合同失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "NDI 采集合同测试通过\n";
    return 0;
}
