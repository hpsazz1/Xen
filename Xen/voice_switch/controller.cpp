#include "voice_switch/controller.h"
#include "voice_switch/voice_switch.h"
#include "debug_session/debug_session.h"
#include "keyboard/keyboard.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <tuple>

namespace voice_switch {
using Json = nlohmann::json;
struct Controller::Impl {
    Server& server;
    debug_session::Session& session;
    std::uint64_t connection = 0, revision = 0, next_id = 0;
    bool connected = false, ready = false, release_required = true;
    bool refresh_pending = false, refreshing = false;
    Json entries = Json::array();
    std::map<std::string, std::string> paths;
    std::string map_name;
    std::uint64_t map_epoch = 0;
    std::vector<int> keys;
    enum class Phase { IDLE, RECORDING, WAITING, LOADING } phase = Phase::IDLE;
    std::uint64_t id = 0, round_epoch = 0, round_revision = 0;
    std::string round_map, plan_id;
    std::chrono::steady_clock::time_point deadline{};
    Impl(Server& network, debug_session::Session& target) : server(network), session(target) {}
    bool send(Json message) { return server.send(connection, message.dump()); }
    void cancel() {
        if (phase != Phase::IDLE) send({{"type","cancel"},{"id",id}});
        phase = Phase::IDLE; release_required = true;
    }
    void finish(bool ok, const std::string& reason) {
        if (phase == Phase::IDLE) return;
        send({{"type","result"},{"id",id},{"ok",ok},{"reason",reason},{"plan_id",plan_id}});
        phase = Phase::IDLE;
        // 本轮内的第二次按住也必须先释放，不能在结果之后补启动。
        release_required = true;
    }
    bool current(const debug_session::Context& context) const {
        return connected && context.map.valid && context.map.map_name == round_map &&
            context.map.context_epoch == round_epoch && revision == round_revision;
    }
    void tick(const debug_session::Context& context, const KeyboardPollResult& input,
              const KeyboardConfig& config, bool blocked, bool capture) {
        const auto network = server.snapshot();
        const bool changed_connection = network.connected != connected || network.connection_epoch != connection;
        if (changed_connection) {
            phase = Phase::IDLE; ready = false; release_required = true;
            connected = network.connected; connection = network.connection_epoch;
        }
        if (keys != config.voice_virtual_keys) { cancel(); keys = config.voice_virtual_keys; }
        if ((changed_connection && connected) || map_name != (context.map.valid ? context.map.map_name : std::string{}) ||
            map_epoch != context.map.context_epoch) refresh_pending = true;
        if (refreshing && !session.busy()) refreshing = false;
        if (refresh_pending && connected && !blocked && !capture && !session.busy() && phase == Phase::IDLE) {
            debug_session::Request refresh; refresh.mode = debug_session::Mode::MOVEMENT_TEST;
            refresh.load_path = session.snapshot()->movement_selected_path;
            if (session.dispatch(debug_session::Action::LIST_MOVEMENT_PLANS,refresh,context)) {
                refresh_pending = false; refreshing = true;
            }
        }
        const auto snapshot = session.snapshot();
        std::vector<std::tuple<std::string,std::string,std::string>> candidates;
        if (context.map.valid) for (const auto& item : snapshot->movement_saved_plans) {
            if (item.map != context.map.map_name || item.voice_command.empty()) continue;
            const auto filename = std::filesystem::u8path(item.path).filename().u8string();
            candidates.emplace_back(std::string(filename.begin(),filename.end()), item.voice_command, item.path);
        }
        std::sort(candidates.begin(), candidates.end());
        Json next_entries = Json::array();
        std::map<std::string,std::string> next_paths;
        for (const auto& [candidate_id,command,path] : candidates) {
            next_entries.push_back({{"id",candidate_id},{"command",command}});
            next_paths[candidate_id] = path;
        }
        const bool content_changed = next_entries != entries;
        const auto name = context.map.valid ? context.map.map_name : std::string{};
        const bool context_changed = name != map_name || map_epoch != context.map.context_epoch;
        if (content_changed) {
            if (revision == std::numeric_limits<std::uint64_t>::max()) { cancel(); return; }
            ++revision; entries = std::move(next_entries);
        }
        paths = std::move(next_paths); map_name = name; map_epoch = context.map.context_epoch;
        if (changed_connection || content_changed || context_changed) {
            ready = false;
            if (connected) send({{"type","catalog"},{"map",map_name},{"epoch",map_epoch},{"revision",revision},{"entries",entries}});
        }
        const bool unavailable = blocked || capture || !input.input_healthy || keys.empty();
        if (unavailable || (phase != Phase::IDLE && !current(context))) cancel();
        if (phase != Phase::IDLE && std::chrono::steady_clock::now() >= deadline) finish(false,"语音请求超时");
        // 停止、手动编辑及地图变化先决定 still_current，再让 Session 提交异步结果。
        const bool may_commit = phase == Phase::LOADING && !unavailable && current(context);
        if (const auto completed = session.finish_voice(context, may_commit); completed && phase == Phase::LOADING)
            finish(*completed, *completed ? "" : "方案载入未完成或上下文已变化");
        for (const auto& incoming : server.poll()) {
            if (incoming.connection_epoch != connection) continue;
            const auto message = Json::parse(incoming.body);
            const auto type = message.value("type",std::string{});
            if (type == "ready") {
                ready = message.value("epoch",std::uint64_t{}) == map_epoch &&
                    message.value("revision",std::uint64_t{}) == revision && context.map.valid;
                continue;
            }
            if (phase == Phase::IDLE || message.value("id",std::uint64_t{}) != id) continue;
            if (type == "fail") { finish(false,message.value("reason",std::string("未识别口令"))); continue; }
            if (type != "select" || phase != Phase::WAITING) continue;
            plan_id = message.value("plan_id",std::string{});
            const auto selected = paths.find(plan_id);
            bool unique_command = false;
            for (const auto& entry : entries) if (entry["id"] == plan_id) {
                unique_command = std::count_if(entries.begin(),entries.end(),[&](const Json& other) {
                    return other["command"] == entry["command"];
                }) == 1;
            }
            if (unavailable || !current(context) || selected == paths.end() || !unique_command ||
                message.value("map",std::string{}) != round_map ||
                message.value("epoch",std::uint64_t{}) != round_epoch ||
                message.value("revision",std::uint64_t{}) != round_revision) { finish(false,"候选或地图上下文已失效"); continue; }
            debug_session::Request request;
            request.mode = debug_session::Mode::MOVEMENT_TEST; request.load_path = selected->second;
            if (!session.select_voice(request,context)) { finish(false,"当前状态不允许切换方案"); continue; }
            phase = Phase::LOADING;
        }
        if (unavailable) return;
        bool released = input.capture_state_valid;
        for (const int key : keys) if (key < 1 || key > 255 || input.capture_virtual_keys[static_cast<std::size_t>(key)]) released = false;
        if (released) release_required = false;
        for (const auto& event : input.events) {
            if (event.type != KeyboardEventType::VOICE_HOLD_CHANGED) continue;
            if (!event.active) {
                if (phase == Phase::RECORDING) {
                    if (!send({{"type","end"},{"id",id}})) cancel();
                    else phase = Phase::WAITING;
                }
                continue;
            }
            if (release_required || phase != Phase::IDLE || !connected || !ready || refreshing || refresh_pending || !context.map.valid || entries.empty()) {
                release_required = true; continue;
            }
            if (next_id == std::numeric_limits<std::uint64_t>::max()) continue;
            id = ++next_id; round_map = map_name; round_epoch = map_epoch; round_revision = revision; plan_id.clear();
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            phase = Phase::RECORDING;
            if (!send({{"type","begin"},{"id",id},{"map",round_map},{"epoch",round_epoch},{"revision",round_revision}})) cancel();
        }
    }
};
Controller::Controller(Server& server, debug_session::Session& session) : impl_(std::make_unique<Impl>(server,session)) {}
Controller::~Controller() = default;
void Controller::cancel() noexcept { try { impl_->cancel(); } catch (...) { impl_->phase = Impl::Phase::IDLE; impl_->release_required = true; } }
void Controller::tick(const debug_session::Context& context, const KeyboardPollResult& input,
                      const KeyboardConfig& config, bool blocked, bool capture) noexcept {
    try { impl_->tick(context,input,config,blocked,capture); }
    catch (...) { cancel(); (void)impl_->session.finish_voice(context,false); }
}
}
