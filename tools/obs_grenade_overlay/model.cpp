// SPDX-License-Identifier: GPL-2.0-or-later
#include "model.h"

namespace xen_obs {
namespace {
std::string text(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}
}

std::string percent_encode(std::string_view value, bool file_path) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (file_path && (c == '/' || c == ':'))) result.push_back(static_cast<char>(c));
        else { result.push_back('%'); result.push_back(digits[c >> 4]); result.push_back(digits[c & 15]); }
    }
    return result;
}

bool valid_plan_id(std::string_view value) {
    return !value.empty() && value.size() <= 200 && value.ends_with(".json") &&
        value.find_first_of("/\\\r\n") == std::string_view::npos && value.find("..") == std::string_view::npos;
}

void Model::reset() { seen_request_.clear(); display_request_.clear(); initialized_ = false; displayed_ = false; }

View Model::update(const Json& selection, const Json& reference, const Json& catalog,
                   const std::string& base_url) {
    View result;
    const auto request_id = text(selection, "request_id");
    const auto requested = text(selection, "plan_id");
    const auto generation = text(selection, "generation");
    const auto request = !request_id.empty() && !generation.empty() && valid_plan_id(requested)
        ? generation + "\n" + request_id + "\n" + requested : std::string{};
    // 首次连接只记基线，不能把上次投掷留下的selected误认为新的人工选择。
    if (!initialized_) { initialized_ = true; seen_request_ = request; return result; }
    if (request != seen_request_) {
        seen_request_ = request;
        display_request_ = request;
        displayed_ = false;
    }
    if (request.empty() || display_request_ != request) return result;
    const auto selection_state = text(selection, "state");
    if (selection_state == "pending") return result;
    if (selection_state != "selected") { display_request_.clear(); return result; }
    const auto actual = text(selection, "actual_plan_id");
    const auto guide = selection.find("guidance");
    if (!valid_plan_id(actual) || actual != requested || guide == selection.end() || !guide->is_object() ||
        text(*guide, "plan_id") != actual) {
        // Session回执与App快照可能相差一次发布；只允许未显示的新请求等待身份一致。
        if (displayed_) display_request_.clear();
        return result;
    }
    const auto revision = text(reference, "revision");
    const bool ready = reference.is_object() && reference.value("ready", false) &&
        text(reference, "plan_id") == actual && !revision.empty();
    const auto state = text(*guide, "state");
    if (state != "selected" && state != "aligning" && state != "failed") {
        display_request_.clear(); return result;
    }
    // 对齐、取消后已清除display_request，后续selected、F10换图和重新找点均不能复活它。

    std::string title = actual.substr(0, actual.size() - 5);
    if (catalog.is_array()) for (const auto& entry : catalog) {
        if (entry.is_object() && text(entry, "id") == actual && !text(entry, "name").empty()) {
            title = text(entry, "name"); break;
        }
    }
    result.visible = true;
    displayed_ = true;
    result.payload = ready ? reference : Json::object();
    result.payload["title"] = title;
    result.payload["state"] = state;
    result.payload["message"] = text(*guide, "message");
    result.payload["ready"] = ready;
    if (ready) {
        const auto suffix = "?plan_id=" + percent_encode(actual) + "&revision=" + percent_encode(revision);
        result.payload["standing_url"] = base_url + "/reference/standing.png" + suffix;
        result.payload["aim_url"] = base_url + "/reference/aim.png" + suffix;
    } else {
        result.payload["reason"] = "参考图待采集；试投成功并复位后按F10。";
    }
    return result;
}
}
