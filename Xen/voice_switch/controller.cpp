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
    bool recognition_enabled = false;
    bool refresh_pending = false, refreshing = false;
    Json entries = Json::array();
    std::map<std::string, std::string> paths;
    std::string map_name;
    std::uint64_t map_epoch = 0;
    debug_session::Mode mode = debug_session::Mode::MOVEMENT_TEST;
    std::uint64_t guidance_revision = 0;
    bool guidance_sent = false;
    bool menu_selection = false;
    bool owns_load = false;
    std::uint64_t last_menu_id = 0;
    std::vector<int> keys;
    enum class Phase { IDLE, RECORDING, WAITING, LOADING } phase = Phase::IDLE;
    std::uint64_t id = 0, round_epoch = 0, round_revision = 0;
    std::string round_map, plan_id;
    std::chrono::steady_clock::time_point deadline{};
    Impl(Server& network, debug_session::Session& target) : server(network), session(target) {}
    bool send(Json message) { return server.send(connection, message.dump()); }
    void cancel() {
        if (phase != Phase::IDLE) {
            if (menu_selection) send({{"type","catalog_result"},{"id",id},{"ok",false},
                {"reason","方案选择已取消"},{"plan_id",plan_id}});
            else send({{"type","cancel"},{"id",id}});
        }
        phase = Phase::IDLE; release_required = true;
    }
    void finish(bool ok, const std::string& reason) {
        if (phase == Phase::IDLE) return;
        send({{"type",menu_selection ? "catalog_result" : "result"},{"id",id},{"ok",ok},{"reason",reason},{"plan_id",plan_id}});
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
            // 语音为可选入口；断线不能撤销手机选择或正在进行的道具操作。
            phase = Phase::IDLE; ready = false; release_required = true; recognition_enabled = false;
            connected = network.connected; connection = network.connection_epoch;
            guidance_sent = false;
            last_menu_id = 0; menu_selection = false;
        }
        const auto selected_mode = session.snapshot()->movement_test_mode;
        const bool mode_changed = selected_mode != mode;
        if (mode_changed) { cancel(); mode = selected_mode; refresh_pending = true; }
        if (keys != config.voice_virtual_keys) { cancel(); keys = config.voice_virtual_keys; }
        if ((changed_connection && connected) || map_name != (context.map.valid ? context.map.map_name : std::string{}) ||
            map_epoch != context.map.context_epoch) refresh_pending = true;
        if (refreshing && !session.busy()) refreshing = false;
        if (refresh_pending && connected && !blocked && !capture && !session.busy() && phase == Phase::IDLE) {
            debug_session::Request refresh; refresh.mode = mode;
            const auto before = session.snapshot();
            refresh.load_path = mode == debug_session::Mode::GRENADE_TEST
                ? before->grenade_selected_path : before->movement_selected_path;
            if (session.dispatch(debug_session::Action::LIST_MOVEMENT_PLANS,refresh,context)) {
                refresh_pending = false; refreshing = true;
            }
        }
        const auto snapshot = session.snapshot();
        // 引导状态独立于收音轮次；首连同步，后续只推送变化，不传送图像。
        if (connected && (!guidance_sent || guidance_revision != snapshot->grenade_guidance_revision)) {
            if (send({{"type","guidance"},{"revision",snapshot->grenade_guidance_revision},
                {"state",snapshot->grenade_guidance_state},{"plan_id",snapshot->grenade_guidance_plan_id},
                {"reference_id",snapshot->grenade_guidance_reference_id},{"message",snapshot->grenade_guidance_message}})) {
                guidance_revision = snapshot->grenade_guidance_revision; guidance_sent = true;
            }
        }
        std::vector<std::tuple<std::string,std::string,std::string,std::string>> candidates;
        const auto& saved = mode == debug_session::Mode::GRENADE_TEST
            ? snapshot->grenade_saved_plans : snapshot->movement_saved_plans;
        if (context.map.valid) for (const auto& item : saved) {
            if (item.map != context.map.map_name || (mode != debug_session::Mode::GRENADE_TEST && item.voice_command.empty())) continue;
            const auto filename = std::filesystem::u8path(item.path).filename().u8string();
            candidates.emplace_back(std::string(filename.begin(),filename.end()), item.voice_command, item.path, item.raw_name);
        }
        std::sort(candidates.begin(), candidates.end());
        Json next_entries = Json::array();
        std::map<std::string,std::string> next_paths;
        for (const auto& [candidate_id,command,path,title] : candidates) {
            std::string team;
            // 只使用名称中明确的来源编号，不从烟雾目标或地图常识猜阵营。
            for (const auto* side : {"CT", "T"}) {
                const std::string token = std::string(" ") + side;
                const auto start = (" " + title).find(token);
                if (start == std::string::npos) continue;
                const auto suffix = (" " + title).substr(start + token.size());
                if ((!suffix.empty() && suffix.front() >= '0' && suffix.front() <= '9') || suffix.starts_with("来源")) {
                    team = side; break;
                }
            }
            next_entries.push_back({{"id",candidate_id},{"command",command},{"name",title},
                {"map",context.map.map_name},{"team",team}});
            next_paths[candidate_id] = path;
        }
        const bool content_changed = next_entries != entries || mode_changed;
        const auto name = context.map.valid ? context.map.map_name : std::string{};
        const bool context_changed = name != map_name || map_epoch != context.map.context_epoch;
        if (content_changed) {
            if (revision == std::numeric_limits<std::uint64_t>::max()) { cancel(); return; }
            ++revision; entries = std::move(next_entries);
        }
        paths = std::move(next_paths); map_name = name; map_epoch = context.map.context_epoch;
        if (changed_connection || content_changed || context_changed) {
            ready = false;
            if (connected) send({{"type","catalog"},{"map",map_name},{"epoch",map_epoch},{"revision",revision},
                {"domain",mode == debug_session::Mode::GRENADE_TEST ? "grenade" : "movement"},{"entries",entries}});
        }
        const bool input_unavailable = blocked || capture || !input.input_healthy;
        const bool unavailable = input_unavailable || (!menu_selection && keys.empty());
        if (unavailable || (phase != Phase::IDLE && !current(context))) cancel();
        if (phase != Phase::IDLE && std::chrono::steady_clock::now() >= deadline) finish(false,"语音请求超时");
        // 停止、手动编辑及地图变化先决定 still_current，再让 Session 提交异步结果。
        const bool may_commit = phase == Phase::LOADING && !unavailable && current(context);
        if (owns_load) if (const auto completed = session.finish_voice(context, may_commit)) {
            owns_load=false;
            if(phase==Phase::LOADING) finish(*completed, *completed ? "" : "方案载入未完成或上下文已变化");
        }
        for (const auto& incoming : server.poll()) {
            if (incoming.connection_epoch != connection) continue;
            const auto message = Json::parse(incoming.body);
            const auto type = message.value("type",std::string{});
            if (type == "ready") {
                ready = message.value("epoch",std::uint64_t{}) == map_epoch &&
                    message.value("revision",std::uint64_t{}) == revision && context.map.valid;
                recognition_enabled = ready && message.value("voice_enabled",true);
                continue;
            }
            if (type == "catalog_select") {
                const auto request_id = message.value("id",std::uint64_t{});
                const auto selected_id = message.value("plan_id",std::string{});
                send({{"type","catalog_result"},{"id",request_id},{"ok",false},
                    {"reason","主机清单已停用，请在手机网页选择"},{"plan_id",selected_id}});
                continue;
            }
            if (menu_selection || phase == Phase::IDLE || message.value("id",std::uint64_t{}) != id) continue;
            if (type == "fail") { finish(false,message.value("reason",std::string("未识别口令"))); continue; }
            if (type != "select" || phase != Phase::WAITING) continue;
            plan_id = message.value("plan_id",std::string{});
            const auto selected = paths.find(plan_id);
            bool unique_command = false;
            for (const auto& entry : entries) if (entry["id"] == plan_id) {
                unique_command = !entry["command"].get<std::string>().empty() && std::count_if(entries.begin(),entries.end(),[&](const Json& other) {
                    return other["command"] == entry["command"];
                }) == 1;
            }
            if (unavailable || !current(context) || selected == paths.end() || !unique_command ||
                message.value("map",std::string{}) != round_map ||
                message.value("epoch",std::uint64_t{}) != round_epoch ||
                message.value("revision",std::uint64_t{}) != round_revision) { finish(false,"候选或地图上下文已失效"); continue; }
            debug_session::Request request;
            request.mode = mode; request.load_path = selected->second;
            if (!session.select_voice(request,context)) { finish(false,"当前状态不允许切换方案"); continue; }
            owns_load=true;
            phase = Phase::LOADING;
        }
        if (input_unavailable) return;
        bool released = input.capture_state_valid;
        for (const int key : keys) if (key < 1 || key > 255 || input.capture_virtual_keys[static_cast<std::size_t>(key)]) released = false;
        if (released) release_required = false;
        for (const auto& event : input.events) {
            if (event.type == KeyboardEventType::GRENADE_MENU) continue;
            if (event.type != KeyboardEventType::VOICE_HOLD_CHANGED) continue;
            if (keys.empty() || !recognition_enabled) continue;
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
            menu_selection = false;
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
    catch (...) {
        cancel();
        if(impl_->owns_load && impl_->session.finish_voice(context,false).has_value()) impl_->owns_load=false;
    }
}
}
