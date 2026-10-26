// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_OBS_GRENADE_MODEL_H
#define XEN_OBS_GRENADE_MODEL_H

#include <nlohmann/json.hpp>
#include <filesystem>
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

std::string percent_encode(std::string_view value, bool file_path = false);
std::string file_url(const std::filesystem::path& path);
bool valid_plan_id(std::string_view value);
}
#endif
