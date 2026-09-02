#ifndef MOUSE_KMBOX_NET_INTERNAL_H
#define MOUSE_KMBOX_NET_INTERNAL_H

#include "mouse/mouse.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace mouse::detail {

// 只读描述 recvfrom 实际交付的数据报；accepted_as_monitor_state 记录
// 当前生产 parser 的接纳结果，不把它提升为协议合法性结论。
struct KmboxMonitorPacketObservation {
    std::int64_t received_at_steady_ns = 0;
    std::size_t datagram_size = 0;
    int source_address_size = 0;
    int source_family = 0;
    bool source_endpoint_valid = false;
    std::array<std::uint8_t, 4> source_ipv4{};
    std::uint16_t source_port = 0;
    std::uint16_t monitor_local_port = 0;
    std::array<std::uint8_t, 4> configured_device_ipv4{};
    std::uint16_t configured_device_port = 0;
    bool source_ip_matches_configured_device = false;
    bool source_port_matches_configured_device = false;
    bool exact_monitor_packet_size = false;
    bool mouse_report_id_present = false;
    std::uint8_t mouse_report_id = 0;
    bool mouse_buttons_present = false;
    std::uint8_t mouse_buttons = 0;
    bool keyboard_report_id_present = false;
    std::uint8_t keyboard_report_id = 0;
    bool keyboard_modifiers_present = false;
    std::uint8_t keyboard_modifiers = 0;
    bool accepted_as_monitor_state = false;
    std::uint64_t monitor_sequence_before = 0;
    std::uint64_t monitor_sequence_after = 0;
    std::uint64_t monitor_sequence = 0;
};

class IKmboxMonitorPacketObserver {
public:
    virtual ~IKmboxMonitorPacketObserver() = default;

    virtual void observe_kmbox_monitor_packet(
        const KmboxMonitorPacketObservation& observation,
        std::span<const std::uint8_t> payload) noexcept = 0;

protected:
    IKmboxMonitorPacketObserver() = default;
};

// 进程内只允许一个弱持有 observer；不改变 monitor 接纳、键态或输出。
// 调用方销毁 shared_ptr 后自动失效，不延长诊断对象生命周期。
bool install_kmbox_monitor_packet_observer(
    const std::shared_ptr<IKmboxMonitorPacketObserver>& observer) noexcept;

std::unique_ptr<IMouseController> create_kmbox_net_controller(
    const MouseConfig& config) noexcept;

} // namespace mouse::detail

#endif // MOUSE_KMBOX_NET_INTERNAL_H
