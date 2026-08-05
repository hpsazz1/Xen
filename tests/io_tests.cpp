#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>

#ifdef ERROR
#undef ERROR
#endif

#include "capture/capture.h"
#include "capture/ndi_internal.h"
#include "capture/network_internal.h"
#include "capture/udp_internal.h"
#include "capture/xudp_internal.h"
#include "keyboard/keyboard.h"
#include "keyboard/keyboard_internal.h"
#include "log/log.h"
#include "mouse/mouse.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#if XEN_HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

class FakeInputDevice final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    bool move(const MouseMoveCommand&) noexcept override { return false; }
    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = snapshot_;
        return true;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::DISABLED; }
    std::string last_error() const override { return {}; }

    InputSnapshot snapshot_;
};

void test_mouse_disabled_by_default() {
    MouseConfig config;
    auto mouse = MouseDeviceFactory::create(config);
    expect(mouse && mouse->open(), "Win32 Mouse 后端应可初始化");
    expect(mouse && mouse->status() == MouseStatus::DISABLED,
           "物理输出默认必须为 DISABLED");
    expect(mouse && !mouse->move({1, 1}),
           "未显式允许 SendInput 时不得提交命令");
}

void test_ndi_silence_watchdog_tracks_first_frame_and_reopen() {
    using Watchdog = capture::detail::NdiSilenceWatchdog;
    const auto started = Watchdog::Clock::time_point{};
    Watchdog watchdog;
    watchdog.reset(started);

    expect(!watchdog.received_valid_frame() &&
               watchdog.timeout_ms(10000, 2000) == 10000,
           "NDI receiver 创建后、首帧前必须使用完整发现预算");
    expect(!watchdog.expired(started + std::chrono::milliseconds(9999),
                             10000, 2000) &&
               watchdog.expired(started + std::chrono::milliseconds(10000),
                                10000, 2000),
           "NDI 首帧发现预算必须在精确边界到期");

    const auto first_frame = started + std::chrono::milliseconds(4000);
    watchdog.record_valid_frame(first_frame);
    expect(watchdog.received_valid_frame() &&
               watchdog.timeout_ms(10000, 2000) == 2000,
           "NDI 收到有效首帧后必须切换为断流预算");
    expect(!watchdog.expired(first_frame + std::chrono::milliseconds(1999),
                             10000, 2000) &&
               watchdog.expired(first_frame + std::chrono::milliseconds(2000),
                                10000, 2000),
           "NDI 首帧后的断流预算必须从最后有效帧开始计算");

    const auto reopened = started + std::chrono::seconds(30);
    watchdog.reset(reopened);
    expect(!watchdog.received_valid_frame() &&
               watchdog.timeout_ms(10000, 2000) == 10000 &&
               !watchdog.expired(reopened + std::chrono::milliseconds(2000),
                                 10000, 2000),
           "NDI 同进程重新打开后必须重新进入首帧发现阶段");
}

void test_invalid_keyboard_config() {
    KeyboardConfig config;
    config.aim_hold_virtual_keys = {0};
    KeyboardListener keyboard(config);
    expect(!keyboard.open() && keyboard.status() == KeyboardStatus::FAILURE,
           "非法虚拟键配置必须失败关闭");

    config.aim_hold_virtual_keys = {0x02};
    config.emergency_virtual_keys = {0x02};
    KeyboardListener conflicting_keyboard(config);
    expect(!conflicting_keyboard.open() &&
               conflicting_keyboard.status() == KeyboardStatus::FAILURE,
           "按住启用键与急停键相同时必须失败关闭");

    config.emergency_virtual_keys = {0x100};
    KeyboardListener out_of_range_keyboard(config);
    expect(!out_of_range_keyboard.open() &&
               out_of_range_keyboard.status() == KeyboardStatus::FAILURE,
           "超出 Win32 虚拟键范围的配置必须失败关闭");
}

void test_keyboard_event_state_machine() {
    KeyboardConfig config;
    config.aim_hold_virtual_keys = {0x02, 0x05};
    config.emergency_virtual_keys = {0x23, 0x06};
    config.runtime_toggle_virtual_keys = {0x77, 0x04};
    keyboard::detail::KeyboardEventState state;
    std::array<bool, 256> keys{};
    auto polled = keyboard::detail::update_keyboard_events(
        state, config, keys);
    expect(polled.count == 0, "初始未按键状态不应产生事件");

    keys[0x05] = true;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 1 &&
               polled.events[0].type ==
                   KeyboardEventType::AIM_HOLD_CHANGED &&
               polled.events[0].active,
           "按住启用键按下必须产生 active=true 边沿");
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 0, "持续按住不得重复产生启用事件");

    keys[0x06] = true;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 1 &&
               polled.events[0].type == KeyboardEventType::EMERGENCY_STOP,
           "急停按下必须产生一次上升沿事件");
    keys[0x05] = false;
    keys[0x02] = true;
    keys[0x04] = true;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 1 &&
               polled.events[0].type == KeyboardEventType::RUNTIME_TOGGLE,
           "同组另一按住键接替时不得释放，鼠标中键应触发运行切换");

    keys[0x06] = false;
    keys[0x04] = false;
    keys[0x02] = false;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 1 &&
               polled.events[0].type ==
                   KeyboardEventType::AIM_HOLD_CHANGED &&
               !polled.events[0].active,
           "按住组全部释放后必须产生 active=false 边沿");

    keys[0x23] = true;
    keys[0x77] = true;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 2 &&
               polled.events[0].type == KeyboardEventType::EMERGENCY_STOP &&
               polled.events[1].type == KeyboardEventType::RUNTIME_TOGGLE,
           "键盘急停和运行切换上升沿必须按安全优先顺序报告");
    keys[0x23] = false;
    keys[0x77] = false;
    keyboard::detail::update_keyboard_events(state, config, keys);
    keys[0x77] = true;
    polled = keyboard::detail::update_keyboard_events(state, config, keys);
    expect(polled.count == 1 &&
               polled.events[0].type == KeyboardEventType::RUNTIME_TOGGLE,
           "同一个 F8 释放后再次按下必须产生第二次运行切换上升沿");
}

void test_keyboard_listener_uses_selected_device() {
    auto device = std::make_shared<FakeInputDevice>();
    device->snapshot_.status = InputMonitorStatus::READY;
    device->snapshot_.state_valid = true;
    KeyboardListener listener(KeyboardConfig{}, device);
    expect(listener.open(), "共享输入设备监听器必须可初始化");

    device->snapshot_.virtual_keys[0x02] = true;
    auto events = listener.poll();
    expect(events.size() == 1U &&
               events[0].type == KeyboardEventType::AIM_HOLD_CHANGED &&
               events[0].active,
           "所选设备右键必须产生瞄准按住事件");

    for (const InputMonitorStatus status : {
             InputMonitorStatus::WAITING,
             InputMonitorStatus::STALE,
             InputMonitorStatus::FAILURE,
             InputMonitorStatus::CLOSED}) {
        device->snapshot_.status = status;
        device->snapshot_.state_valid = false;
        events = listener.poll();
        expect(events.empty(),
               "所选设备非 READY 状态不得被猜测为瞄准释放事件");
    }

    device->snapshot_ = {};
    device->snapshot_.status = InputMonitorStatus::FAILURE;
    device->snapshot_.state_valid = true;
    events = listener.poll();
    expect(events.size() == 1U &&
               events[0].type == KeyboardEventType::AIM_HOLD_CHANGED &&
               !events[0].active,
           "明确全释放快照即使伴随链路故障也必须产生瞄准释放事件");

    device->snapshot_ = {};
    device->snapshot_.status = InputMonitorStatus::READY;
    device->snapshot_.state_valid = true;
    device->snapshot_.virtual_keys[0x23] = true;
    device->snapshot_.virtual_keys[0x77] = true;
    events = listener.poll();
    expect(events.size() == 2U &&
               events[0].type == KeyboardEventType::EMERGENCY_STOP &&
               events[1].type == KeyboardEventType::RUNTIME_TOGGLE,
           "所选设备 End/F8 必须按急停优先顺序生成事件");
}

void test_invalid_capture_config() {
    CaptureConfig config;
    config.roi_width = 0;
    auto capture = create_capture(config);
    expect(capture && !capture->open(),
           "非法 ROI 必须在访问 DXGI 设备前被拒绝");
    expect(capture && capture->status() == CaptureStatus::INVALID_CONFIG,
           "非法 Capture 配置应返回明确状态");
}

void fill_udp_slot(
        const std::shared_ptr<capture::detail::UdpDecodedFrame>& slot,
        std::uint64_t sequence,
        unsigned char marker) {
    expect(slot != nullptr, "UDP 解码池应提供可写槽");
    if (!slot) return;
    slot->bgr.create(2, 2, CV_8UC3);
    slot->bgr.setTo(cv::Scalar(marker, marker, marker));
    slot->timing.sequence = sequence;
    slot->timing.captured_at = std::chrono::steady_clock::now();
    slot->source_width = 2;
    slot->source_height = 2;
}

void test_udp_latest_frame_pool() {
    capture::detail::UdpLatestFramePool pool;
    auto first = pool.acquire_write();
    fill_udp_slot(first, 1, 10);
    first->timing.capture_stages.ndi_valid = true;
    pool.publish(first);
    auto second = pool.acquire_write();
    fill_udp_slot(second, 2, 20);
    second->timing.capture_stages.ndi_valid = true;
    pool.publish(second);

    CapturedFrame output;
    expect(pool.take_latest(0, output) && output.timing.sequence == 2,
           "UDP 队列必须跳过未消费旧帧并返回最新帧");
    expect(output.timing.source_dropped_frames == 1,
           "覆盖未消费 UDP 帧时必须累计源端丢帧");
    expect(output.timing.capture_stages.ndi_valid &&
               std::isfinite(
                   output.timing.capture_stages.pool_publish_ms) &&
               output.timing.capture_stages.pool_publish_ms >= 0.0,
           "网络最新帧池必须在发布锁内固化可读取的 publish 耗时");
    expect(output.bgr_storage &&
           output.bgr.at<cv::Vec3b>(0, 0)[0] == 20,
           "UDP 输出必须通过 storage 所有权保持解码槽有效");
    expect(!pool.take_latest(2, output),
           "同一 UDP 帧不得被重复交付");

    first.reset();
    second.reset();
    auto third = pool.acquire_write();
    fill_udp_slot(third, 3, 30);
    if (third) third->timing.capture_stages = {};
    pool.publish(third);
    expect(third && !third->timing.capture_stages.ndi_valid &&
               third->timing.capture_stages.pool_publish_ms == 0.0,
           "未启用性能探针时网络池不得制造伪造的分段耗时");
    expect(output.bgr.at<cv::Vec3b>(0, 0)[0] == 20,
           "发布新帧不得覆写消费者仍持有的 UDP 缓冲");
}

void test_udp_pool_is_bounded() {
    capture::detail::UdpLatestFramePool pool;
    std::array<std::shared_ptr<capture::detail::UdpDecodedFrame>,
               capture::detail::UdpLatestFramePool::kSlotCount> writers;
    for (auto& writer : writers) writer = pool.acquire_write();
    expect(!pool.acquire_write(),
           "UDP 解码池满载时必须拒绝写槽而不是扩容");
    pool.record_drop();
    expect(pool.dropped_frames() == 1,
           "UDP 解码池满载丢弃必须可观测");
    writers.front().reset();
    expect(pool.acquire_write() != nullptr,
           "释放 UDP 槽后必须能够继续接收新帧");
}

void test_udp_frame_geometry() {
    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.roi_width = 320;
    config.roi_height = 320;

    capture::detail::UdpFrameGeometry geometry;
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 2560;
    config.udp_source_height = 1440;
    expect(capture::detail::resolve_udp_frame_geometry(
               config, 320, 320, geometry),
           "主机中心 1:1 预裁剪几何必须可解析");
    expect(geometry.source_width == 2560 &&
               geometry.source_height == 1440 &&
               geometry.source_roi_x == 1120.0 &&
               geometry.source_roi_y == 560.0 &&
               geometry.decoded_roi_width == 320 &&
               geometry.decoded_roi_height == 320 &&
               geometry.source_pixels_per_pixel_x == 1.0 &&
               geometry.source_pixels_per_pixel_y == 1.0,
           "2560x1440 主机中心 320x320 ROI 必须映射到 (1120,560)");

    config.udp_frame_layout = UdpFrameLayout::FULL_FRAME_SCALED;
    expect(capture::detail::resolve_udp_frame_geometry(
               config, 640, 360, geometry),
           "缩放完整帧几何必须可解析");
    expect(geometry.source_roi_x == 640.0 &&
               geometry.source_roi_y == 80.0 &&
               geometry.source_pixels_per_pixel_x == 4.0 &&
               geometry.source_pixels_per_pixel_y == 4.0,
           "640x360 完整帧中心 320x320 ROI 必须按主机像素比例映射");

    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 300;
    expect(!capture::detail::resolve_udp_frame_geometry(
               config, 320, 320, geometry),
           "完整 FOV 小于 OBS 预裁剪帧时必须安全失败");
}

void test_xen_metadata_geometry() {
    capture::detail::XenFrameMetadata metadata;
    expect(capture::detail::parse_xen_frame_metadata(
               "<xen version=\"1\" source_width=\"2560\" "
               "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
               "roi_width=\"320\" roi_height=\"320\"/>", metadata),
           "NDI Xen metadata 必须能被严格解析");
    expect(metadata.source_width == 2560 && metadata.source_height == 1440 &&
               metadata.roi_x == 1120 && metadata.roi_y == 560 &&
               metadata.roi_width == 320 && metadata.roi_height == 320,
           "NDI metadata 的主机 FOV 与 ROI 坐标必须保持原值");
    capture::detail::NetworkGeometryConfig config;
    config.layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.roi_width = 160;
    config.roi_height = 160;
    capture::detail::NetworkFrameGeometry geometry;
    expect(!capture::detail::resolve_network_frame_geometry(
               config, 320, 320, geometry, &metadata),
           "metadata ROI 与本地 Detector ROI 尺寸不一致时必须拒绝");
    expect(!capture::detail::parse_xen_frame_metadata(
               "<xen version=\"1\" source_width=\"2560\" "
               "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
               "roi_width=\"320\" roi_height=\"320\" extra=\"1\"/>",
               metadata),
           "NDI metadata 出现未知属性时必须拒绝而不是猜测");
}

void test_invalid_udp_capture_config() {
    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.udp_url.clear();
    auto capture = create_capture(config);
    expect(capture && !capture->open(),
           "空 UDP 地址必须在访问网络前被拒绝");
    expect(capture && capture->status() == CaptureStatus::INVALID_CONFIG,
           "非法 UDP Capture 配置应返回明确状态");
    expect(std::string(CaptureBackendName(config.backend)) == "UDP_MJPEG",
           "UDP Capture 后端名称必须稳定可观测");

    config.udp_url = "udp://127.0.0.1:5000";
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    auto missing_source = create_capture(config);
    expect(missing_source && !missing_source->open() &&
               missing_source->status() == CaptureStatus::INVALID_CONFIG,
           "中心预裁剪模式缺少主机完整 FOV 时必须在监听前失败");
}

capture::detail::XudpFrameDescriptor make_xudp_descriptor(
        std::uint64_t frame_id,
        std::size_t frame_size,
        std::uint32_t encoded_width = 320,
        std::uint32_t encoded_height = 320) {
    capture::detail::XudpFrameDescriptor descriptor;
    descriptor.stream_id = 0x0102030405060708ULL;
    descriptor.frame_id = frame_id;
    descriptor.frame_size = static_cast<std::uint32_t>(frame_size);
    descriptor.encoded_width = encoded_width;
    descriptor.encoded_height = encoded_height;
    descriptor.source_width = 2560;
    descriptor.source_height = 1440;
    descriptor.source_roi_x = 1120;
    descriptor.source_roi_y = 560;
    descriptor.source_roi_width = 320;
    descriptor.source_roi_height = 320;
    descriptor.frame_rate_n = 60000;
    descriptor.frame_rate_d = 1001;
    descriptor.sent_timestamp_ns = 123456789ULL + frame_id;
    return descriptor;
}

using XudpRange = std::pair<std::size_t, std::size_t>;

std::vector<std::vector<std::uint8_t>> make_xudp_packets(
        const capture::detail::XudpFrameDescriptor& descriptor,
        std::span<const std::uint8_t> payload,
        std::span<const XudpRange> ranges = {}) {
    std::vector<XudpRange> default_ranges;
    if (ranges.empty()) {
        const std::size_t first = payload.size() / 3;
        const std::size_t second = payload.size() / 3;
        default_ranges = {
            {0, first},
            {first, second},
            {first + second, payload.size() - first - second}};
        ranges = default_ranges;
    }
    std::array<std::uint8_t, capture::detail::kXudpSha256Bytes> sha256{};
    expect(capture::detail::compute_xudp_frame_sha256(
               descriptor, payload, sha256),
           "XUDP 测试帧 SHA-256 必须可计算");

    std::vector<std::vector<std::uint8_t>> packets;
    packets.reserve(ranges.size());
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto [offset, size] = ranges[index];
        capture::detail::XudpPacketHeader header;
        header.frame = descriptor;
        header.fragment_index = static_cast<std::uint16_t>(index);
        header.fragment_count = static_cast<std::uint16_t>(ranges.size());
        header.fragment_offset = static_cast<std::uint32_t>(offset);
        header.fragment_payload_size = static_cast<std::uint32_t>(size);
        header.frame_sha256 = sha256;
        std::vector<std::uint8_t> packet;
        const bool in_bounds = offset <= payload.size() &&
                               size <= payload.size() - offset;
        expect(in_bounds && capture::detail::serialize_xudp_packet(
                   header, in_bounds ? payload.subspan(offset, size)
                                     : std::span<const std::uint8_t>{},
                   packet),
               "XUDP 测试分片必须可序列化");
        packets.push_back(std::move(packet));
    }
    return packets;
}

capture::detail::XudpConsumeResult consume_xudp_packets(
        capture::detail::XudpFrameAssembler& assembler,
        const std::vector<std::vector<std::uint8_t>>& packets,
        std::span<const std::size_t> order,
        capture::detail::XudpCompletedFrame& completed) {
    auto result = capture::detail::XudpConsumeResult::IGNORED;
    for (const std::size_t index : order) {
        result = assembler.consume_packet(
            packets[index], std::chrono::steady_clock::now(), completed);
    }
    return result;
}

void test_xudp_packetizer_boundaries() {
    std::vector<std::uint8_t> payload(3000);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index % 251U);
    }
    const auto descriptor = make_xudp_descriptor(8, payload.size(), 320, 320);
    capture::detail::XudpFramePacketizer packetizer;
    expect(packetizer.prepare_frame(descriptor, payload, 1400) &&
               packetizer.fragment_count() == 3,
           "XUDP Packetizer 必须按 1400 字节数据报上限计算三个连续分片");

    std::size_t expected_offset = 0;
    for (std::size_t index = 0; index < packetizer.fragment_count(); ++index) {
        std::vector<std::uint8_t> packet;
        capture::detail::XudpPacketHeader header;
        std::span<const std::uint8_t> fragment;
        const bool serialized = packetizer.serialize_fragment(
            index, payload, packet);
        expect(serialized && packet.size() <= 1400 &&
                   capture::detail::parse_xudp_packet(
                       packet, header, fragment) &&
                   header.fragment_index == index &&
                   header.fragment_count == packetizer.fragment_count() &&
                   header.fragment_offset == expected_offset &&
                   header.fragment_payload_size == fragment.size(),
               "XUDP Packetizer 分片必须连续、不重叠且不超过数据报上限");
        expected_offset += fragment.size();
    }
    expect(expected_offset == payload.size(),
           "XUDP Packetizer 全部分片必须恰好覆盖完整帧");

    const std::vector<std::uint8_t> relocated_payload = payload;
    std::vector<std::uint8_t> packet;
    expect(!packetizer.serialize_fragment(0, relocated_payload, packet),
           "XUDP Packetizer 必须拒绝 prepare 后更换 payload 地址");
    expect(!packetizer.prepare_frame(
               descriptor, payload, capture::detail::kXudpHeaderBytes),
           "XUDP Packetizer 必须拒绝无法容纳 payload 的数据报上限");
}

void test_xudp_serialization_and_reassembly() {
    const std::vector<std::uint8_t> payload{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const auto descriptor = make_xudp_descriptor(9, payload.size(), 2, 3);
    const std::array<XudpRange, 1> single_range{{{0, payload.size()}}};
    const auto single = make_xudp_packets(descriptor, payload, single_range);
    expect(single.size() == 1 &&
               single[0].size() ==
                   capture::detail::kXudpHeaderBytes + payload.size(),
           "XUDP 数据报必须使用固定 124 字节头");
    if (single.empty()) return;
    const auto& bytes = single.front();
    expect(bytes[0] == 'X' && bytes[1] == 'U' && bytes[2] == 'D' &&
               bytes[3] == 'P' && bytes[4] == 0 && bytes[5] == 1 &&
               bytes[12] == 0x01 && bytes[19] == 0x08 &&
               bytes[27] == 9,
           "XUDP magic、版本和 64 位字段必须按大端序编码");

    capture::detail::XudpPacketHeader parsed;
    std::span<const std::uint8_t> parsed_payload;
    expect(capture::detail::parse_xudp_packet(
               bytes, parsed, parsed_payload) &&
               parsed.frame.stream_id == descriptor.stream_id &&
               parsed.frame.frame_id == descriptor.frame_id &&
               parsed.frame.source_roi_x == 1120 &&
               std::equal(parsed_payload.begin(), parsed_payload.end(),
                          payload.begin()),
           "XUDP 大端序序列化与解析必须完整往返");

    const auto packets = make_xudp_packets(descriptor, payload);
    capture::detail::XudpFrameAssembler assembler;
    capture::detail::XudpCompletedFrame completed;
    const std::array<std::size_t, 2> first_order{{2, 0}};
    expect(consume_xudp_packets(
               assembler, packets, first_order, completed) ==
               capture::detail::XudpConsumeResult::INCOMPLETE,
           "XUDP 乱序分片在未齐时必须保持 INCOMPLETE");
    expect(assembler.consume_packet(
               packets[2], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::IGNORED,
           "内容与范围相同的重复 XUDP 分片必须幂等忽略");
    expect(assembler.consume_packet(
               packets[1], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::FRAME &&
               std::equal(completed.jpeg.begin(), completed.jpeg.end(),
                          payload.begin()),
           "XUDP 三个乱序分片必须重组为原始完整帧");
}

void test_xudp_rejects_invalid_fragments() {
    const std::vector<std::uint8_t> payload{
        1, 2, 3, 4, 5, 6};
    const auto descriptor = make_xudp_descriptor(20, payload.size(), 2, 3);
    const auto packets = make_xudp_packets(descriptor, payload);
    capture::detail::XudpCompletedFrame completed;

    capture::detail::XudpFrameAssembler conflict_assembler;
    expect(conflict_assembler.consume_packet(
               packets[0], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::INCOMPLETE,
           "冲突测试首分片必须先进入在途状态");
    auto conflicting_packet = packets[0];
    conflicting_packet.back() ^= 0xff;
    expect(conflict_assembler.consume_packet(
               conflicting_packet, std::chrono::steady_clock::now(),
               completed) ==
               capture::detail::XudpConsumeResult::INVALID_PACKET &&
               conflict_assembler.transport_invalid_packets() == 1,
           "同索引但内容冲突的 XUDP 重复分片必须拒绝");
    expect(conflict_assembler.consume_packet(
               packets[1], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::IGNORED,
           "已判冲突的 XUDP 帧不得由后续分片复活");

    const std::array<XudpRange, 3> gap_ranges{{
        {0, 2}, {3, 2}, {5, 1}}};
    const auto gap_packets = make_xudp_packets(
        make_xudp_descriptor(21, payload.size(), 2, 3), payload, gap_ranges);
    capture::detail::XudpFrameAssembler gap_assembler;
    const std::array<std::size_t, 3> order{{0, 1, 2}};
    expect(consume_xudp_packets(
               gap_assembler, gap_packets, order, completed) ==
               capture::detail::XudpConsumeResult::INVALID_PACKET,
           "分片齐全但覆盖存在缺口时必须拒绝 XUDP 帧");

    const std::array<XudpRange, 3> overlap_ranges{{
        {0, 3}, {2, 2}, {4, 2}}};
    const auto overlap_packets = make_xudp_packets(
        make_xudp_descriptor(22, payload.size(), 2, 3), payload,
        overlap_ranges);
    capture::detail::XudpFrameAssembler overlap_assembler;
    expect(consume_xudp_packets(
               overlap_assembler, overlap_packets, order, completed) ==
               capture::detail::XudpConsumeResult::INVALID_PACKET,
           "分片覆盖发生重叠时必须拒绝 XUDP 帧");

    const std::array<XudpRange, 1> single_range{{{0, payload.size()}}};
    auto tampered_packets = make_xudp_packets(
        make_xudp_descriptor(23, payload.size(), 2, 3), payload,
        single_range);
    tampered_packets[0][92] ^= 0xff;
    capture::detail::XudpFrameAssembler hash_assembler;
    expect(hash_assembler.consume_packet(
               tampered_packets[0], std::chrono::steady_clock::now(),
               completed) ==
               capture::detail::XudpConsumeResult::INVALID_PACKET,
           "帧 SHA-256 与完整 payload 不一致时必须拒绝 XUDP 帧");
}

void test_xudp_sequence_and_geometry() {
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6};
    capture::detail::XudpFrameAssembler assembler;
    capture::detail::XudpCompletedFrame completed;
    const std::array<std::size_t, 3> order{{0, 1, 2}};
    const auto frame1 = make_xudp_packets(
        make_xudp_descriptor(1, payload.size(), 2, 3), payload);
    expect(consume_xudp_packets(assembler, frame1, order, completed) ==
               capture::detail::XudpConsumeResult::FRAME,
           "XUDP 序号测试第一帧必须完整发布");
    const auto frame2 = make_xudp_packets(
        make_xudp_descriptor(2, payload.size(), 2, 3), payload);
    expect(assembler.consume_packet(
               frame2[0], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::INCOMPLETE,
           "XUDP 第二帧缺片时必须保持在途状态");
    const auto frame3 = make_xudp_packets(
        make_xudp_descriptor(3, payload.size(), 2, 3), payload);
    expect(consume_xudp_packets(assembler, frame3, order, completed) ==
               capture::detail::XudpConsumeResult::FRAME &&
               completed.source_received_frames == 2 &&
               completed.transport_dropped_frames == 1,
           "源帧号从 1 跳到 3 时必须统计一帧传输丢失");

    capture::detail::XudpFrameAssembler bounded_assembler;
    const auto frame10 = make_xudp_packets(
        make_xudp_descriptor(10, payload.size(), 2, 3), payload);
    const auto frame11 = make_xudp_packets(
        make_xudp_descriptor(11, payload.size(), 2, 3), payload);
    const auto frame12 = make_xudp_packets(
        make_xudp_descriptor(12, payload.size(), 2, 3), payload);
    const auto frame13 = make_xudp_packets(
        make_xudp_descriptor(13, payload.size(), 2, 3), payload);
    expect(bounded_assembler.consume_packet(
               frame10[0], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::INCOMPLETE &&
               bounded_assembler.consume_packet(
                   frame11[0], std::chrono::steady_clock::now(), completed) ==
                   capture::detail::XudpConsumeResult::INCOMPLETE &&
               bounded_assembler.consume_packet(
                   frame12[0], std::chrono::steady_clock::now(), completed) ==
                   capture::detail::XudpConsumeResult::INCOMPLETE &&
               bounded_assembler.consume_packet(
                   frame13[0], std::chrono::steady_clock::now(), completed) ==
                   capture::detail::XudpConsumeResult::INCOMPLETE,
           "第四个 XUDP 在途帧必须复用固定槽而不是扩容失败");
    expect(bounded_assembler.consume_packet(
               frame10[1], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::IGNORED,
           "三槽满载后迟到旧帧不得挤掉更新的在途帧");
    expect(bounded_assembler.consume_packet(
               frame13[1], std::chrono::steady_clock::now(), completed) ==
               capture::detail::XudpConsumeResult::INCOMPLETE &&
               bounded_assembler.consume_packet(
                   frame13[2], std::chrono::steady_clock::now(), completed) ==
                   capture::detail::XudpConsumeResult::FRAME,
           "三槽淘汰最旧帧后必须仍可完成最新 XUDP 帧");

    capture::detail::XudpFrameAssembler stream_assembler;
    const auto old_stream_frame1 = make_xudp_packets(
        make_xudp_descriptor(1, payload.size(), 2, 3), payload);
    expect(consume_xudp_packets(
               stream_assembler, old_stream_frame1, order, completed) ==
               capture::detail::XudpConsumeResult::FRAME,
           "XUDP 旧发送会话首帧必须发布");
    auto new_stream_descriptor =
        make_xudp_descriptor(1, payload.size(), 2, 3);
    ++new_stream_descriptor.stream_id;
    const auto new_stream_frame1 = make_xudp_packets(
        new_stream_descriptor, payload);
    expect(consume_xudp_packets(
               stream_assembler, new_stream_frame1, order, completed) ==
               capture::detail::XudpConsumeResult::FRAME,
           "发送端重启更换 stream_id 后必须接受新流首帧");
    const auto delayed_old_frame = make_xudp_packets(
        make_xudp_descriptor(2, payload.size(), 2, 3), payload);
    expect(stream_assembler.consume_packet(
               delayed_old_frame[0], std::chrono::steady_clock::now(),
               completed) == capture::detail::XudpConsumeResult::IGNORED,
           "新流建立后旧 stream_id 的迟到包必须忽略而不能切回旧流");

    CaptureConfig config;
    config.backend = CaptureBackend::XUDP_JPEG;
    config.roi_width = 320;
    config.roi_height = 320;
    capture::detail::NetworkFrameGeometry geometry;
    const auto roi_descriptor = make_xudp_descriptor(1, 100);
    expect(capture::detail::resolve_xudp_frame_geometry(
               config, roi_descriptor, geometry) &&
               geometry.source_width == 2560 &&
               geometry.source_height == 1440 &&
               geometry.source_roi_x == 1120.0 &&
               geometry.source_roi_y == 560.0 &&
               geometry.source_pixels_per_pixel_x == 1.0 &&
               geometry.source_pixels_per_pixel_y == 1.0,
           "XUDP 320x320 主机 ROI 必须固定映射到 (1120,560)");

    auto scaled_descriptor = make_xudp_descriptor(2, 100, 640, 360);
    scaled_descriptor.source_roi_x = 0;
    scaled_descriptor.source_roi_y = 0;
    scaled_descriptor.source_roi_width = 2560;
    scaled_descriptor.source_roi_height = 1440;
    expect(capture::detail::resolve_xudp_frame_geometry(
               config, scaled_descriptor, geometry) &&
               geometry.source_roi_x == 640.0 &&
               geometry.source_roi_y == 80.0 &&
               geometry.source_pixels_per_pixel_x == 4.0 &&
               geometry.source_pixels_per_pixel_y == 4.0,
           "XUDP 缩放完整帧必须按 (4,4) 比例还原主机坐标");
}

#if XEN_HAS_NDI

bool wait_for_capture_frame(ICapture& capture,
                            std::uint64_t after_sequence,
                            CapturedFrame& frame);

void send_ndi_test_frame(NDIlib_send_instance_t sender,
                         cv::Scalar color,
                         const std::string& metadata) {
    cv::Mat frame(320, 320, CV_8UC4,
                  cv::Scalar(color[0], color[1], color[2], 255.0));
    NDIlib_video_frame_v2_t video{};
    video.xres = frame.cols;
    video.yres = frame.rows;
    video.FourCC = NDIlib_FourCC_type_BGRX;
    video.frame_rate_N = 60;
    video.frame_rate_D = 1;
    video.frame_format_type = NDIlib_frame_format_type_progressive;
    video.timecode = NDIlib_send_timecode_synthesize;
    video.p_data = frame.data;
    video.line_stride_in_bytes = static_cast<int>(frame.step);
    video.p_metadata = metadata.c_str();
    NDIlib_send_send_video_v2(sender, &video);
}

void test_ndi_loopback() {
    CaptureConfig config;
    config.backend = CaptureBackend::NDI;
    config.ndi_source_name = "Auto";
    config.ndi_discovery_timeout_ms = 500;
    config.ndi_receive_timeout_ms = 50;
    config.ndi_disconnect_timeout_ms = 500;
    config.enable_performance_probes = true;
    config.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
    config.ndi_require_frame_metadata = true;
    config.roi_width = 320;
    config.roi_height = 320;
    config.acquire_timeout_ms = 20;

    auto capture = create_capture(config);
    expect(capture && capture->open(),
           "NDI Capture 必须能在真实 SDK 下打开");
    if (!capture || capture->status() == CaptureStatus::UNSUPPORTED) return;

    NDIlib_send_create_t sender_settings{};
    sender_settings.p_ndi_name = "Xen NDI Loopback Sender";
    sender_settings.clock_video = false;
    sender_settings.clock_audio = false;
    NDIlib_send_instance_t sender = NDIlib_send_create(&sender_settings);
    expect(sender != nullptr, "NDI 回环测试发送端必须创建成功");
    if (!sender) {
        capture->close();
        return;
    }
    const std::string metadata =
        "<xen version=\"1\" source_width=\"2560\" "
        "source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\" "
        "roi_width=\"320\" roi_height=\"320\"/>";
    for (int index = 0; index < 20; ++index) {
        send_ndi_test_frame(
            sender, cv::Scalar(24.0, 96.0, 208.0), metadata);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CapturedFrame frame;
    const bool received_first = wait_for_capture_frame(*capture, 0, frame);
    expect(received_first && frame.bgr.cols == 320 &&
               frame.bgr.rows == 320 && frame.bgr.type() == CV_8UC3,
           "NDI BGRX 回环必须输出 320x320 BGR ROI");
    expect(received_first && frame.source_width == 2560 &&
               frame.source_height == 1440 && frame.roi_x == 1120.0 &&
               frame.roi_y == 560.0 &&
               frame.source_pixels_per_pixel_x == 1.0 &&
               frame.source_pixels_per_pixel_y == 1.0,
           "NDI metadata 必须将辅机接收帧映射到主机 (1120,560)");
    expect(received_first && frame.timing.source_fps == 60.0 &&
               frame.timing.source_received_frames > 0,
           "NDI 帧必须携带 SDK 源帧率与接收统计");
    const auto valid_timing = [](double value) {
        return std::isfinite(value) && value >= 0.0;
    };
    expect(received_first && frame.timing.capture_stages.ndi_valid &&
               !frame.timing.capture_stages.runtime_handoff_valid &&
               valid_timing(frame.timing.capture_stages.receive_call_ms) &&
               valid_timing(frame.timing.capture_stages.metadata_ms) &&
               valid_timing(frame.timing.capture_stages.geometry_ms) &&
               valid_timing(frame.timing.capture_stages.pool_acquire_ms) &&
               valid_timing(frame.timing.capture_stages.color_convert_ms) &&
               valid_timing(frame.timing.capture_stages.pool_publish_ms) &&
               frame.timing.capture_stages.runtime_capture_grab_ms == 0.0 &&
               frame.timing.capture_stages.runtime_queue_publish_ms == 0.0,
           "NDI 性能探针必须发布有限非负的 Capture 分段耗时，且不伪造 Runtime 交接耗时");
    const cv::Scalar first_mean = received_first
        ? cv::mean(frame.bgr) : cv::Scalar{};

    bool received_second = false;
    bool performance_query_observed =
        received_first &&
        frame.timing.capture_stages.performance_query_sampled;
    bool performance_query_valid = !performance_query_observed ||
        valid_timing(frame.timing.capture_stages.performance_query_ms);
    bool queue_probe_observed = received_first &&
        frame.timing.capture_stages.queue_depth_sampled;
    bool queue_probe_valid = !queue_probe_observed ||
        (valid_timing(frame.timing.capture_stages.queue_query_ms) &&
         frame.timing.capture_stages.queued_video_frames >= 0 &&
         frame.timing.capture_stages.queued_audio_frames >= 0 &&
         frame.timing.capture_stages.queued_metadata_frames >= 0);
    std::uint64_t after_sequence = frame.timing.sequence;
    const auto probe_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < probe_deadline &&
           (!received_second || !performance_query_observed ||
            !queue_probe_observed)) {
        send_ndi_test_frame(
            sender, cv::Scalar(208.0, 48.0, 20.0), metadata);
        if (wait_for_capture_frame(*capture, after_sequence, frame)) {
            received_second = true;
            after_sequence = frame.timing.sequence;
            const auto& stages = frame.timing.capture_stages;
            if (stages.performance_query_sampled) {
                performance_query_observed = true;
                performance_query_valid = performance_query_valid &&
                    valid_timing(stages.performance_query_ms);
            }
            if (stages.queue_depth_sampled) {
                queue_probe_observed = true;
                queue_probe_valid = queue_probe_valid &&
                    valid_timing(stages.queue_query_ms) &&
                    stages.queued_video_frames >= 0 &&
                    stages.queued_audio_frames >= 0 &&
                    stages.queued_metadata_frames >= 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const cv::Scalar second_mean = received_second
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_second &&
               std::abs(first_mean[0] - second_mean[0]) > 100.0,
           "NDI 连续变化帧不得重放首次输入");
    expect(performance_query_observed && performance_query_valid,
           "NDI 探针必须观测并计时周期性 SDK performance 查询");
    expect(queue_probe_observed && queue_probe_valid,
           "NDI 探针必须在首帧或一秒周期内发布合法 SDK queue 深度");

    NDIlib_send_destroy(sender);
    bool access_lost = false;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < disconnect_deadline) {
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::ACCESS_LOST) {
            access_lost = true;
            break;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            break;
        }
    }
    expect(access_lost,
           "NDI 源断开超过阈值后必须进入 ACCESS_LOST");
    capture->close();
}

#else

void test_ndi_unsupported_without_sdk() {
    CaptureConfig config;
    config.backend = CaptureBackend::NDI;
    auto capture = create_capture(config);
    expect(capture && !capture->open() &&
               capture->status() == CaptureStatus::UNSUPPORTED,
           "未安装 NDI SDK 时选择 NDI 必须明确返回 UNSUPPORTED");
}

#endif

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockSession() {
        if (ready_) WSACleanup();
    }

    bool ready() const noexcept { return ready_; }

private:
    bool ready_ = false;
};

unsigned short reserve_loopback_port() {
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    int address_size = sizeof(address);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address),
                    &address_size) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return 0;
    }
    closesocket(socket_handle);
    return ntohs(address.sin_port);
}

bool send_fragmented_jpeg(SOCKET socket_handle,
                          const sockaddr_in& destination,
                          const std::vector<unsigned char>& jpeg) {
    if (jpeg.size() < 4) return false;
    const std::array<std::pair<std::size_t, std::size_t>, 3> fragments{{
        {0, 1},
        {1, jpeg.size() - 2},
        {jpeg.size() - 1, 1},
    }};
    for (const auto [offset, size] : fragments) {
        const int sent = sendto(
            socket_handle,
            reinterpret_cast<const char*>(jpeg.data() + offset),
            static_cast<int>(size), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent != static_cast<int>(size)) return false;
    }
    return true;
}

bool send_xudp_datagrams(
        SOCKET socket_handle,
        const sockaddr_in& destination,
        const std::vector<std::vector<std::uint8_t>>& packets,
        std::span<const std::size_t> order) {
    for (const std::size_t index : order) {
        if (index >= packets.size()) return false;
        const auto& packet = packets[index];
        const int sent = sendto(
            socket_handle,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent != static_cast<int>(packet.size())) return false;
    }
    return true;
}

bool wait_for_capture_frame(ICapture& capture,
                            std::uint64_t after_sequence,
                            CapturedFrame& frame) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const CaptureStatus status = capture.grab(frame);
        if (status == CaptureStatus::FRAME &&
            frame.timing.sequence > after_sequence) {
            return true;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            return false;
        }
    }
    return false;
}

void test_udp_mjpeg_loopback() {
    WinsockSession winsock;
    expect(winsock.ready(), "Winsock 必须可用于 UDP 回环测试");
    if (!winsock.ready()) return;
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "UDP 回环测试必须取得临时端口");
    if (port == 0) return;

    cv::Mat first_source(320, 320, CV_8UC3, cv::Scalar(24, 96, 208));
    cv::Mat second_source(320, 320, CV_8UC3, cv::Scalar(208, 48, 20));
    std::vector<unsigned char> first_jpeg;
    std::vector<unsigned char> second_jpeg;
    expect(cv::imencode(".jpg", first_source, first_jpeg) &&
               cv::imencode(".jpg", second_source, second_jpeg) &&
               !first_jpeg.empty() && !second_jpeg.empty(),
           "UDP 回环测试必须生成两张内容不同的真实 JPEG 帧");
    if (first_jpeg.empty() || second_jpeg.empty()) return;

    CaptureConfig config;
    config.backend = CaptureBackend::UDP_MJPEG;
    config.udp_url = "udp://127.0.0.1:" + std::to_string(port);
    config.udp_read_timeout_ms = 50;
    config.udp_disconnect_timeout_ms = 300;
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 2560;
    config.udp_source_height = 1440;
    config.roi_width = 320;
    config.roi_height = 320;
    config.acquire_timeout_ms = 20;

    auto capture = create_capture(config);
    const bool opened = capture && capture->open();
    expect(opened, "生产 UDP Capture 必须能打开本机 MJPEG 数据流" +
                       (capture ? ": " + capture->last_error() : ""));
    if (!opened) return;

    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    expect(sender != INVALID_SOCKET, "UDP 回环发送套接字必须创建成功");
    if (sender == INVALID_SOCKET) {
        capture->close();
        return;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);

    expect(send_fragmented_jpeg(sender, destination, first_jpeg),
           "第一张 JPEG 的 SOI/EOI 跨数据报发送必须成功");
    CapturedFrame frame;
    const bool received_first = wait_for_capture_frame(*capture, 0, frame);
    const std::uint64_t first_sequence = frame.timing.sequence;
    const cv::Scalar first_mean = received_first
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_first && !frame.bgr.empty() &&
               frame.bgr.type() == CV_8UC3 && frame.bgr.cols == 320 &&
               frame.bgr.rows == 320,
           "UDP Capture 必须重组跨数据报标记并解码 320x320 BGR 帧");
    expect(received_first && frame.source_width == 2560 &&
               frame.source_height == 1440 && frame.encoded_width == 320 &&
               frame.encoded_height == 320 && frame.roi_x == 1120.0 &&
               frame.roi_y == 560.0 &&
               frame.source_pixels_per_pixel_x == 1.0 &&
               frame.source_pixels_per_pixel_y == 1.0,
           "辅机收到的 320x320 帧必须保留主机 2560x1440 中心 ROI 语义");

    expect(send_fragmented_jpeg(sender, destination, second_jpeg),
           "第二张变化 JPEG 的跨数据报发送必须成功");
    const bool received_second = wait_for_capture_frame(
        *capture, first_sequence, frame);
    const cv::Scalar second_mean = received_second
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_second && frame.timing.sequence > first_sequence &&
               std::abs(first_mean[0] - second_mean[0]) > 100.0,
           "连续 UDP 帧内容变化时输出必须随输入变化，不能重放旧帧");

    closesocket(sender);
    bool access_lost = false;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < disconnect_deadline) {
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::ACCESS_LOST) {
            access_lost = true;
            break;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            break;
        }
    }
    expect(access_lost,
           "UDP 长时间无可解码帧时必须进入 ACCESS_LOST 而非无限等待");
    capture->close();
}

void test_xudp_jpeg_loopback() {
    WinsockSession winsock;
    expect(winsock.ready(), "Winsock 必须可用于 XUDP 回环测试");
    if (!winsock.ready()) return;
    const unsigned short port = reserve_loopback_port();
    expect(port != 0, "XUDP 回环测试必须取得临时端口");
    if (port == 0) return;

    cv::Mat first_source(320, 320, CV_8UC3, cv::Scalar(32, 80, 220));
    cv::Mat third_source(320, 320, CV_8UC3, cv::Scalar(220, 40, 16));
    std::vector<std::uint8_t> first_jpeg;
    std::vector<std::uint8_t> third_jpeg;
    expect(cv::imencode(".jpg", first_source, first_jpeg) &&
               cv::imencode(".jpg", third_source, third_jpeg) &&
               !first_jpeg.empty() && !third_jpeg.empty(),
           "XUDP 回环测试必须生成两张内容不同的真实 JPEG 帧");
    if (first_jpeg.empty() || third_jpeg.empty()) return;

    CaptureConfig config;
    config.backend = CaptureBackend::XUDP_JPEG;
    config.udp_url = "udp://127.0.0.1:" + std::to_string(port);
    config.udp_read_timeout_ms = 50;
    config.udp_disconnect_timeout_ms = 300;
    // 这些裸 UDP 字段故意不完整；XUDP 必须只信任协议头中的主机几何。
    config.udp_frame_layout = UdpFrameLayout::CENTER_CROP_1_TO_1;
    config.udp_source_width = 0;
    config.udp_source_height = 0;
    config.roi_width = 320;
    config.roi_height = 320;
    config.acquire_timeout_ms = 20;

    auto capture = create_capture(config);
    const bool opened = capture && capture->open();
    expect(opened, "生产 XUDP Capture 必须能打开本机协议流" +
                       (capture ? ": " + capture->last_error() : ""));
    if (!opened) return;

    const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    expect(sender != INVALID_SOCKET, "XUDP 回环发送套接字必须创建成功");
    if (sender == INVALID_SOCKET) {
        capture->close();
        return;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);

    const std::array<std::size_t, 3> unordered{{2, 0, 1}};
    const auto first_packets = make_xudp_packets(
        make_xudp_descriptor(1, first_jpeg.size()), first_jpeg);
    expect(send_xudp_datagrams(
               sender, destination, first_packets, unordered),
           "第一张 XUDP JPEG 的乱序分片发送必须成功");
    CapturedFrame frame;
    const bool received_first = wait_for_capture_frame(*capture, 0, frame);
    const std::uint64_t first_sequence = frame.timing.sequence;
    const cv::Scalar first_mean = received_first
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_first && !frame.bgr.empty() &&
               frame.bgr.type() == CV_8UC3 && frame.bgr.cols == 320 &&
               frame.bgr.rows == 320 && frame.source_width == 2560 &&
               frame.source_height == 1440 && frame.roi_x == 1120.0 &&
               frame.roi_y == 560.0 &&
               frame.source_pixels_per_pixel_x == 1.0 &&
               frame.source_pixels_per_pixel_y == 1.0,
           "XUDP 320x320 帧必须携带主机 2560x1440 的中心 ROI 坐标");
    expect(received_first && frame.timing.source_sequence_valid &&
               frame.timing.source_sequence == 1 &&
               frame.timing.source_received_frames == 1 &&
               std::abs(frame.timing.source_fps - 60000.0 / 1001.0) < 0.001,
           "XUDP 首帧必须发布源帧号、接收计数与源 FPS");

    const auto missing_packets = make_xudp_packets(
        make_xudp_descriptor(2, first_jpeg.size()), first_jpeg);
    const std::array<std::size_t, 1> only_first{{0}};
    expect(send_xudp_datagrams(
               sender, destination, missing_packets, only_first),
           "缺片 XUDP 帧的首分片发送必须成功");
    const std::array<std::uint8_t, 4> malformed{{'B', 'A', 'D', '!'}};
    expect(sendto(
               sender, reinterpret_cast<const char*>(malformed.data()),
               static_cast<int>(malformed.size()), 0,
               reinterpret_cast<const sockaddr*>(&destination),
               sizeof(destination)) == static_cast<int>(malformed.size()),
           "XUDP 异常数据报发送必须成功");

    const auto third_descriptor = make_xudp_descriptor(3, third_jpeg.size());
    const auto third_packets = make_xudp_packets(
        third_descriptor, third_jpeg);
    expect(send_xudp_datagrams(
               sender, destination, third_packets, unordered),
           "第三张 XUDP JPEG 的乱序分片发送必须成功");
    const bool received_third = wait_for_capture_frame(
        *capture, first_sequence, frame);
    const cv::Scalar third_mean = received_third
        ? cv::mean(frame.bgr) : cv::Scalar{};
    expect(received_third && frame.timing.sequence > first_sequence &&
               std::abs(first_mean[0] - third_mean[0]) > 100.0,
           "XUDP 连续有效帧必须随输入变化，不能重放旧帧");
    expect(received_third && frame.timing.source_sequence_valid &&
               frame.timing.source_sequence == 3 &&
               frame.timing.source_received_frames == 2 &&
               frame.timing.transport_dropped_frames == 1 &&
               frame.timing.transport_invalid_packets >= 1 &&
               frame.timing.source_timestamp_valid &&
               frame.timing.source_timestamp ==
                   static_cast<std::int64_t>(
                       third_descriptor.sent_timestamp_ns),
           "XUDP 必须分开报告源帧号缺口、异常包和发送时间戳");

    closesocket(sender);
    bool access_lost = false;
    const auto disconnect_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < disconnect_deadline) {
        const CaptureStatus status = capture->grab(frame);
        if (status == CaptureStatus::ACCESS_LOST) {
            access_lost = true;
            break;
        }
        if (status != CaptureStatus::FRAME &&
            status != CaptureStatus::NO_FRAME) {
            break;
        }
    }
    expect(access_lost,
           "XUDP 长时间无完整有效帧时必须进入 ACCESS_LOST");
    capture->close();
}

} // namespace

int main() {
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);
    test_mouse_disabled_by_default();
    test_ndi_silence_watchdog_tracks_first_frame_and_reopen();
    test_invalid_keyboard_config();
    test_keyboard_event_state_machine();
    test_keyboard_listener_uses_selected_device();
    test_invalid_capture_config();
    test_udp_latest_frame_pool();
    test_udp_pool_is_bounded();
    test_udp_frame_geometry();
    test_xen_metadata_geometry();
    test_invalid_udp_capture_config();
    test_xudp_packetizer_boundaries();
    test_xudp_serialization_and_reassembly();
    test_xudp_rejects_invalid_fragments();
    test_xudp_sequence_and_geometry();
#if XEN_HAS_NDI
    test_ndi_loopback();
#else
    test_ndi_unsupported_without_sdk();
#endif
    test_udp_mjpeg_loopback();
    test_xudp_jpeg_loopback();
    Log::shutdown();

    if (failures != 0) {
        std::cerr << "输入输出测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "输入输出测试全部通过。\n";
    return 0;
}
