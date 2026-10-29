// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_RECORDER_MARKER_H
#define XEN_RECORDER_MARKER_H

#include <cstdint>
#include <string>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace xen::recorder {
struct RenderedMarker {
    std::string id;
    std::int64_t render_begin_steady_ns = 0, render_end_steady_ns = 0;
    std::int64_t render_begin_system_unix_ns = 0, render_end_system_unix_ns = 0;
    std::uint64_t render_frame_time_ns = 0;
    bool first_render = false;
    // 零CTS与非空error表示没有取得可匹配的渲染候选，不冒充视频PTS。
    std::string error;
};
struct MarkerDrain {
    std::vector<RenderedMarker> markers;
    std::uint64_t dropped = 0; // 自本source创建以来的累计覆盖数。
};

// 独立libobs进程自注册源；不创建窗口、不修改外部OBS场景或游戏输入。
// create/mark/drain/reset由同一冷路径owner串行调用，渲染回调并发由内部处理。
class MarkerSource final {
public:
    MarkerSource() = default;
    ~MarkerSource();
    MarkerSource(const MarkerSource&) = delete;
    MarkerSource& operator=(const MarkerSource&) = delete;
    bool create(std::uint32_t width = 560, std::uint32_t height = 72) noexcept;
    obs_source_t* source() const noexcept { return source_; } // borrowed。
    bool mark(const std::string& id, std::uint32_t duration_ms = 2000) noexcept;
    MarkerDrain drain();
    void reset() noexcept; // 必须在当前obs_shutdown之前调用。
private:
    obs_source_t* source_ = nullptr;
};

// 返回owned source引用；纯合成背景，不打开屏幕、音频、游戏或输入设备。
obs_source_t* create_synthetic_background(std::uint32_t width, std::uint32_t height) noexcept;
}
#endif
