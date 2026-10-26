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
}
#endif
