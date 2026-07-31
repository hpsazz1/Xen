#ifndef SENDER_REPORT_H
#define SENDER_REPORT_H

#include "sender/sender.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sender::detail {

struct SenderFrameSample {
    std::uint64_t frame_id = 0;
    double capture_ms = 0.0;
    double encode_ms = 0.0;
    double packetize_ms = 0.0;
    double send_ms = 0.0;
    double sender_total_ms = 0.0;
    std::uint64_t datagrams = 0;
    std::uint64_t jpeg_bytes = 0;
    std::uint64_t wire_bytes = 0;
};

struct SenderRunGeometry {
    int source_width = 0;
    int source_height = 0;
    int encoded_width = 0;
    int encoded_height = 0;
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
    double source_pixels_per_pixel_x = 1.0;
    double source_pixels_per_pixel_y = 1.0;
};

struct SenderRunReport {
    std::string destination_url;
    std::string stop_reason;
    int adapter_index = 0;
    int output_index = 0;
    int jpeg_quality = 85;
    int max_datagram_bytes = 1400;
    std::uint32_t fps = 240;
    std::uint64_t maximum_frames = 0;
    std::uint64_t maximum_seconds = 0;
    double elapsed_seconds = 0.0;
    SenderRunGeometry geometry;
    XudpSenderStats stats;
    std::vector<SenderFrameSample> samples;
    std::uint64_t samples_dropped = 0;
};

// 报告只接受完整成功发送过的运行，并拒绝覆盖既有目标。写入同目录临时文件、
// flush/close 成功后再原子改名，失败时删除本轮临时文件。
bool write_sender_run_report(
    const std::string& path,
    const SenderRunReport& report,
    std::string& error) noexcept;

} // namespace sender::detail

#endif // SENDER_REPORT_H
