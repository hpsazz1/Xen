// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_OBS_GRENADE_MODEL_H
#define XEN_OBS_GRENADE_MODEL_H

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xen_obs {
using Json = nlohmann::json;

struct View {
    bool visible = false;
    Json payload = Json::object();
};

// 只解释只读HTTP快照；不选择方案、不发送游戏输入。
class Model {
public:
    View update(const Json& selection, const Json& reference, const Json& catalog,
                const std::string& base_url);
    void reset();
private:
    std::string seen_request_, display_request_;
    bool initialized_ = false, displayed_ = false;
};

// 只管理一次采集的等待状态；子进程启停与采集文件仍由调用方持有。
class CaptureJobState {
public:
    bool start(std::uint64_t now_ms);
    std::optional<std::string> poll(std::uint64_t now_ms, std::optional<int> exit_code,
                                    const Json& status);
    bool running() const { return running_; }
private:
    std::uint64_t started_ms_ = 0;
    bool running_ = false;
};

// 菜单打开时记录回执基线；同方案也必须收到新的实际选择回执才关闭。
class MenuSelectionState {
public:
    void open(const Json& selection);
    bool should_close(const Json& selection);
    void reset();
private:
    std::string generation_, request_id_;
    bool opened_ = false, baseline_known_ = false, pending_after_open_ = false;
};

std::string percent_encode(std::string_view value, bool file_path = false);
std::string file_url(const std::filesystem::path& path);
bool valid_plan_id(std::string_view value);

// 时间戳以十进制字符串传输，避免JSON/网页浮点舍入；此包只证明绘制调用区间。
Json sync_paint_packet(const std::string& marker_id, const std::string& source_session,
                       std::int64_t paint_begin, std::int64_t paint_end, std::int64_t paint_system);
bool sync_reply_accepted(const Json& reply, const std::string& marker_id) noexcept;
Json sync_receipt_packet(const Json& paint, const Json& reply, std::int64_t received);

// UI显示许可只在请求后的两秒内有效；过期/设置变化/另一编号均不能迟到显示。
struct SyncShowPermit {
    std::string marker_id;
    std::uint64_t configuration = 0;
    std::int64_t issued_steady_ns = 0;
};
bool sync_show_current(const SyncShowPermit& permit, const std::string& marker_id,
                       std::uint64_t configuration, std::int64_t now) noexcept;
bool sync_show_expired(const SyncShowPermit& permit, std::int64_t now) noexcept;

// 同步状态故障只关闭同步入口；不抛到原道具publication轮询的异常边界。
bool sync_status_available(const Json& reply) noexcept;
}
#endif
