#include "overlay/movement_panel.h"
#include "overlay/overlay.h"
#include "movement/timeline.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace {
using namespace debug_session;
using movement::timeline::Plan;
using movement::timeline::ActionKind;
using movement::timeline::Key;
using movement::timeline::Button;
using TimelineAction = movement::timeline::Action;

enum class Grip { LEFT, RIGHT, BOTH };
enum class Travel { STILL, STEP, WALK, RUN };
struct GrenadePreset {
    const char* name;
    bool jump, crouch;
    Grip grip;
    Travel travel;
};
constexpr std::array<GrenadePreset,24> kGrenadePresets{{
    {"左键投掷",false,false,Grip::LEFT,Travel::STILL},
    {"左键跳投",true,false,Grip::LEFT,Travel::STILL},
    {"W 跳投",true,false,Grip::LEFT,Travel::STEP},
    {"蹲跳投",true,true,Grip::LEFT,Travel::STILL},
    {"蹲 W 跳投",true,true,Grip::LEFT,Travel::STEP},
    {"左右双键 W 跳投",true,false,Grip::BOTH,Travel::STEP},
    {"右键投掷",false,false,Grip::RIGHT,Travel::STILL},
    {"左右双键投掷",false,false,Grip::BOTH,Travel::STILL},
    {"右键跳投",true,false,Grip::RIGHT,Travel::STILL},
    {"左右双键跳投",true,false,Grip::BOTH,Travel::STILL},
    {"W 前移投掷",false,false,Grip::LEFT,Travel::STEP},
    {"蹲投掷",false,true,Grip::LEFT,Travel::STILL},
    {"蹲 W 前移投掷",false,true,Grip::LEFT,Travel::STEP},
    {"静步 W 投掷",false,false,Grip::LEFT,Travel::WALK},
    {"静步 W 跳投",true,false,Grip::LEFT,Travel::WALK},
    {"助跑 W 投掷",false,false,Grip::LEFT,Travel::RUN},
    {"助跑 W 跳投",true,false,Grip::LEFT,Travel::RUN},
    {"右键静步 W 投掷",false,false,Grip::RIGHT,Travel::WALK},
    {"右键静步 W 跳投",true,false,Grip::RIGHT,Travel::WALK},
    {"左右双键静步 W 投掷",false,false,Grip::BOTH,Travel::WALK},
    {"左右双键静步 W 跳投",true,false,Grip::BOTH,Travel::WALK},
    {"右键助跑 W 跳投",true,false,Grip::RIGHT,Travel::RUN},
    {"左右双键助跑 W 跳投",true,false,Grip::BOTH,Travel::RUN},
    {"蹲静步 W 跳投",true,true,Grip::LEFT,Travel::WALK}
}};
constexpr std::array<std::string_view,15> kGrenadeMaps{"de_mirage","de_dust2","de_inferno","de_ancient",
    "de_anubis","de_nuke","de_overpass","de_train","de_vertigo","de_cache",
    "cs_office","cs_italy","ar_baggage","ar_shoots","ar_pool_day"};

void apply_grenade_preset(Plan& plan, const GrenadePreset& preset) {
    // 原六套参数保持；新增组合只提供可微调初稿，不推断任何地图点位的已验时序。
    plan.actions.clear();
    const auto button = [&](Button value, int duration) {
        TimelineAction action;
        action.kind = ActionKind::BUTTON; action.button = value; action.duration_ms = duration;
        plan.actions.push_back(action);
    };
    if (preset.grip != Grip::LEFT) button(Button::RIGHT,1000);
    if (preset.grip != Grip::RIGHT) button(Button::LEFT,preset.grip == Grip::BOTH ? 1001 : 1000);
    if (preset.jump) plan.actions.push_back({ActionKind::KEY,Key::SPACE,1000,40,0});
    const bool moving = preset.travel != Travel::STILL;
    if (moving) {
        const int start = preset.travel == Travel::WALK ? 800 : preset.travel == Travel::RUN ? 700 : 1000;
        plan.actions.push_back({ActionKind::KEY,Key::W,start,1100-start,0});
    }
    if (preset.crouch) plan.actions.push_back({ActionKind::KEY,Key::CTRL,0,moving ? 1100 : 1040,0});
    if (preset.travel == Travel::WALK) plan.actions.push_back({ActionKind::KEY,Key::SHIFT,0,1100,0});
    plan.mask = {true,15,0,0};
}

void tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
bool button(const char* label, const char* help, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled(); tip(help); return clicked;
}
bool text_field(const char* label, std::string& value, const char* help) {
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x * .7f));
    const bool changed = ImGui::InputText(label, &value);
    tip(help); return changed;
}
std::string action_label(const TimelineAction& action) {
    if (action.kind == ActionKind::BUTTON) return action.button == Button::LEFT ? "鼠标左键" : "鼠标右键";
    if (action.kind == ActionKind::TURN) return action.angle_degrees < 0 ? "鼠标左转" : "鼠标右转";
    if (action.kind == ActionKind::MOVE) return "鼠标位移";
    if (action.kind == ActionKind::WHEEL) return "滚轮";
    const std::string prefix = action.kind == ActionKind::MASK ? "屏蔽 " : action.physical ? "物理 " : "软件 ";
    switch (action.key) {
    case Key::W: return prefix + "W";
    case Key::A: return prefix + "A";
    case Key::S: return prefix + "S";
    case Key::D: return prefix + "D";
    case Key::SPACE: return prefix + "空格";
    case Key::CTRL: return prefix + "Ctrl";
    case Key::SHIFT: return prefix + "Left Shift";
    }
    return "未知";
}
std::string console_command(const std::string& notes) {
    // 只提取六个数字组成的定位命令，不把备注中的后续文本或其他命令复制到控制台。
    static const std::regex expression([] {
        const std::string number = R"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)";
        return "\\bsetpos[ \\t]+" + number + "[ \\t]+" + number + "[ \\t]+" + number +
            "[ \\t]*;[ \\t]*setang[ \\t]+" + number + "[ \\t]+" + number + "[ \\t]+" + number;
    }());
    std::smatch match;
    return std::regex_search(notes, match, expression) ? match.str() : std::string{};
}
int natural_compare(std::string_view a, std::string_view b) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            auto end_a = i, end_b = j;
            while (end_a < a.size() && digit(a[end_a])) ++end_a;
            while (end_b < b.size() && digit(b[end_b])) ++end_b;
            while (i < end_a && a[i] == '0') ++i;
            while (j < end_b && b[j] == '0') ++j;
            if (end_a - i != end_b - j) return end_a - i < end_b - j ? -1 : 1;
            const auto order = a.substr(i,end_a-i).compare(b.substr(j,end_b-j));
            if (order) return order;
            i = end_a; j = end_b;
        } else {
            const auto left = fold(static_cast<unsigned char>(a[i++]));
            const auto right = fold(static_cast<unsigned char>(b[j++]));
            if (left != right) return left < right ? -1 : 1;
        }
    }
    return i == a.size() ? (j == b.size() ? 0 : -1) : 1;
}
int grenade_map_rank(std::string_view map) {
    // 与按地图采集清单保持一致；未列出的自定义地图稳定排在后面。
    return static_cast<int>(std::find(kGrenadeMaps.begin(),kGrenadeMaps.end(),map) - kGrenadeMaps.begin());
}
const std::string& grenade_name(const MovementSavedPlan& item) {
    return item.raw_name.empty() ? item.name : item.raw_name;
}
std::string_view grenade_order_name(const MovementSavedPlan& item) {
    std::string_view name = grenade_name(item), map = item.map;
    if (map.starts_with("de_")) map.remove_prefix(3);
    if (!map.empty() && name.size() > map.size() && name[map.size()] == ' ' &&
        natural_compare(name.substr(0,map.size()),map) == 0) name.remove_prefix(map.size()+1);
    return name;
}
int station_group(std::string_view name) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (name.size() > 1 && (name[0] == 'T' || name[0] == 't') && digit(name[1])) return 0;
    if (name.size() > 2 && (name[0] == 'C' || name[0] == 'c') &&
        (name[1] == 'T' || name[1] == 't') && digit(name[2])) return 1;
    return 2;
}
bool grenade_less(const MovementSavedPlan* a, const MovementSavedPlan* b) {
    const auto map_a = grenade_map_rank(a->map), map_b = grenade_map_rank(b->map);
    if (map_a != map_b) return map_a < map_b;
    if (a->map != b->map) return a->map < b->map;
    const auto name_a = grenade_order_name(*a), name_b = grenade_order_name(*b);
    const auto group_a = station_group(name_a), group_b = station_group(name_b);
    if (group_a != group_b) return group_a < group_b;
    if (const auto order = natural_compare(name_a,name_b)) return order < 0;
    return a->path < b->path;
}
void timeline(const Plan& plan) {
    const int duration = movement::timeline::duration_ms(plan);
    ImGui::Text("时间轴：0～%d ms（每行对应一个动作）", duration);
    if (plan.actions.empty() || duration <= 0) return;
    if (ImGui::BeginChild("movement_timeline", {0, 180}, true)) {
        auto* draw = ImGui::GetWindowDrawList();
        const float label_width = 125;
        const float width = std::max(40.f, ImGui::GetContentRegionAvail().x - label_width);
        for (std::size_t i = 0; i < plan.actions.size(); ++i) {
            const auto& action = plan.actions[i];
            const auto origin = ImGui::GetCursorScreenPos();
            ImGui::Text("%zu. %s", i + 1, action_label(action).c_str());
            const float start = std::clamp(static_cast<float>(action.start_ms) / duration, 0.f, 1.f);
            const float end = std::clamp(static_cast<float>(static_cast<double>(action.start_ms) + action.duration_ms) / duration, 0.f, 1.f);
            draw->AddRectFilled({origin.x + label_width, origin.y}, {origin.x + label_width + width, origin.y + 16}, ImGui::GetColorU32(ImGuiCol_FrameBg));
            draw->AddRectFilled({origin.x + label_width + width * start, origin.y},
                {origin.x + label_width + std::max(width * start + 2.f, width * end), origin.y + 16},
                ImGui::GetColorU32(action.kind == ActionKind::TURN ? ImGuiCol_PlotLines : ImGuiCol_ButtonHovered));
        }
    }
    ImGui::EndChild();
}
}

struct MovementPanel::Impl {
    struct GrenadeIdentity {
        std::string name, map, location, team;
        bool operator==(const GrenadeIdentity&) const = default;
    };
    struct PendingGrenadeSave {
        Json plan;
        GrenadeIdentity identity;
        std::uint64_t catalog = 0;
    };
    Plan plan;
    Request request;
    std::string error, map_filter, gsi_map, gsi_team;
    bool grenade() const { return request.mode == Mode::GRENADE_TEST; }
    bool initialized = false, catalog_requested = false, seen_loaded_slot = false;
    bool initial_draft = true, custom_map = false;
    bool new_grenade_draft = false;
    std::optional<GrenadeIdentity> grenade_baseline;
    std::optional<PendingGrenadeSave> pending_grenade_save;
    std::uint64_t seen_draft = 0, seen_catalog = 0;

    explicit Impl(Mode mode) {
        request.mode = mode;
        request.output_root = "cache/debug";
        if (grenade()) {
            request.movement_library_root = "cache/grenade/plans";
            plan.name = "新道具方案";
            TimelineAction action; action.kind = ActionKind::BUTTON;
            action.button = Button::LEFT; action.duration_ms = 1000;
            plan.actions.push_back(action);
        } else {
            plan.actions.push_back({ActionKind::KEY, Key::SPACE, 0, 40, 0});
        }
    }
    bool update_request() {
        request.plan_text.clear();
        if (!movement::timeline::validate_plan(plan, error)) return false;
        request.plan_text = movement::timeline::plan_json(plan).dump(2);
        error.clear();
        return true;
    }
    void changed(OverlayActions& actions) {
        request.grenade_update_selected = false;
        update_request();
        actions.movement_draft_changed = true;
        actions.debug_request = request;
    }
    std::string selected_team() const {
        const auto side = movement::timeline::plan_team(plan);
        return !side.empty() ? side : plan.map == gsi_map ? gsi_team : std::string{};
    }
    static GrenadeIdentity identity(const Plan& value) {
        return {value.name,value.map,value.location,movement::timeline::plan_team(value)};
    }
    bool updates_current_grenade() const {
        return !request.load_path.empty() && !new_grenade_draft;
    }
    void new_grenade(OverlayActions& actions) {
        plan = Plan{}; plan.name = "新道具方案"; plan.map = gsi_map; plan.team = gsi_team;
        TimelineAction action; action.kind = ActionKind::BUTTON; action.button = Button::LEFT; action.duration_ms = 1000;
        plan.actions.push_back(action);
        request.load_path.clear(); request.movement_save_as = false;
        grenade_baseline.reset(); pending_grenade_save.reset();
        new_grenade_draft = true; initial_draft = true; custom_map = false;
        changed(actions);
    }
    void send(Action action, OverlayActions& actions, bool save_as = false) {
        request.grenade_update_selected = false;
        if (action == Action::DELETE_PLAN && (!grenade() || request.load_path.empty())) return;
        if (grenade() && action == Action::SAVE_PLAN) {
            const auto side = selected_team();
            const auto valid_map = !plan.map.empty() && plan.map.size() <= 83 &&
                plan.map.front() >= 'a' && plan.map.front() <= 'z' &&
                std::all_of(plan.map.begin(),plan.map.end(),[](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                });
            if (!valid_map || side.empty()) {
                error = "请选择地图和T/CT阵营；自定义地图标识仅使用小写字母、数字和下划线";
                return;
            }
            plan.team = side;
            save_as = save_as || !updates_current_grenade();
        }
        if (action != Action::CANCEL && action != Action::LOAD_PLAN && action != Action::LIST_MOVEMENT_PLANS && action != Action::DELETE_PLAN &&
            action != Action::START_MOVEMENT_RECORDING && action != Action::STOP_MOVEMENT_RECORDING &&
            !update_request()) return;
        if (grenade()) {
            if (action == Action::SAVE_PLAN)
                pending_grenade_save = PendingGrenadeSave{Json::parse(request.plan_text),identity(plan),seen_catalog};
            else if (action == Action::LOAD_PLAN || action == Action::DELETE_PLAN) pending_grenade_save.reset();
        }
        request.movement_save_as = save_as;
        request.grenade_update_selected = grenade() && action == Action::SAVE_PLAN && !save_as && updates_current_grenade();
        actions.debug_action = action;
        actions.debug_request = request;
    }
    void sync(const Snapshot* snapshot, OverlayActions& actions) {
        if (!initialized) { initialized = true; update_request(); }
        gsi_map.clear(); gsi_team.clear();
        if (!snapshot) return;
        if (snapshot->movement_map.valid) {
            gsi_map = snapshot->movement_map.map_name;
            if (snapshot->movement_map.local_team != weapon::Team::UNKNOWN)
                gsi_team = weapon::team_name(snapshot->movement_map.local_team);
        }
        const auto revision = grenade() ? snapshot->grenade_catalog_revision : snapshot->movement_catalog_revision;
        if (grenade() && pending_grenade_save && revision != pending_grenade_save->catalog &&
            !snapshot->grenade_selected_path.empty() && snapshot->grenade_baseline == pending_grenade_save->plan) {
            // 保存只回传目录和基线，不能等待新的loaded_draft才能接住新文件身份。
            request.load_path = snapshot->grenade_selected_path;
            grenade_baseline = pending_grenade_save->identity;
            new_grenade_draft = false; pending_grenade_save.reset();
        }
        if (revision != seen_catalog) {
            seen_catalog = revision;
            if (!grenade()) request.load_path = snapshot->movement_selected_path;
            else if (!new_grenade_draft) {
                request.load_path = snapshot->grenade_selected_path;
                if (request.load_path.empty()) grenade_baseline.reset();
                else {
                    Plan baseline; std::string reason;
                    if (movement::timeline::parse_plan(snapshot->grenade_baseline,baseline,reason))
                        grenade_baseline = identity(baseline);
                }
            }
        }
        const auto loaded_revision = grenade() ? snapshot->grenade_loaded_revision : snapshot->movement_loaded_revision;
        const auto& loaded_draft = grenade() ? snapshot->grenade_loaded_draft : snapshot->movement_loaded_draft;
        // 每个页面消费自己的载入结果，另一页异步完成不能挤掉尚未显示的草稿。
        // 零版本兼容旧快照；本域开始发布后不再回退到全局单槽。
        const bool legacy = !seen_loaded_slot && loaded_revision == 0 && snapshot->draft_plan_mode == request.mode;
        const auto draft_revision = legacy ? snapshot->draft_plan_revision : loaded_revision;
        if ((draft_revision != seen_draft || (loaded_revision && !seen_loaded_slot)) && (loaded_revision || legacy)) {
            seen_draft = draft_revision;
            seen_loaded_slot |= loaded_revision != 0;
            Plan loaded;
            if (movement::timeline::parse_plan(legacy ? snapshot->draft_plan : loaded_draft, loaded, error)) {
                plan = std::move(loaded); initial_draft = false; custom_map = false;
                if (grenade()) {
                    request.load_path = snapshot->grenade_selected_path;
                    // 载入槽可能先于目录回执到达，不能把暂时的空路径重新当作显式新增。
                    new_grenade_draft = false; pending_grenade_save.reset();
                    if (request.load_path.empty()) grenade_baseline.reset(); else grenade_baseline = identity(plan);
                }
                update_request();
            }
        }
        // 初始空草稿只补空值；载入和手动选择之后不随GSI换图、换边改写方案。
        if (grenade() && initial_draft) {
            if (plan.map.empty() && !custom_map) plan.map = gsi_map;
            if (plan.team.empty()) plan.team = gsi_team;
            update_request();
        }
    }
    void library(const Snapshot* snapshot, OverlayActions& actions, bool busy) {
        std::vector<const MovementSavedPlan*> entries;
        if (snapshot) {
            const auto& saved = grenade() ? snapshot->grenade_saved_plans : snapshot->movement_saved_plans;
            for (const auto& item : saved) entries.push_back(&item);
        }
        if (grenade()) {
            std::sort(entries.begin(), entries.end(), grenade_less);
            ImGui::BeginDisabled(busy);
            if (ImGui::BeginCombo("筛选地图", map_filter.empty() ? "全部地图" : map_filter.c_str())) {
                if (ImGui::Selectable("全部地图", map_filter.empty())) map_filter.clear();
                std::vector<std::string> maps;
                for (const auto* item : entries)
                    if (!item->map.empty()) maps.push_back(item->map);
                std::sort(maps.begin(), maps.end(), [](const auto& a, const auto& b) {
                    const auto rank_a = grenade_map_rank(a), rank_b = grenade_map_rank(b);
                    return rank_a != rank_b ? rank_a < rank_b : a < b;
                });
                maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
                for (const auto& map : maps)
                    if (ImGui::Selectable(map.c_str(), map_filter == map)) map_filter = map;
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            tip("按地图筛选道具；列表依次按地图、T/CT站位编号和道具名称排列，T2在T10前，同名备用瞄角相邻。筛选不载入或执行方案。");
        }
        const auto label = [&](const MovementSavedPlan& item) {
            if (!grenade()) return item.name;
            auto name = grenade_name(item);
            if (map_filter.empty() && !item.map.empty()) name += " / " + item.map;
            return name;
        };
        std::string selected = "未保存的新方案";
        for (const auto* item : entries)
            if (item->path == request.load_path) { selected = label(*item); break; }
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x * .7f));
        if (ImGui::BeginCombo(grenade() ? "道具方案" : "已保存方案", selected.c_str())) {
            int visible = 0;
            for (const auto* item : entries) {
                if (grenade() && !map_filter.empty() && item->map != map_filter) continue;
                ++visible;
                ImGui::PushID(item->path.c_str());
                if (ImGui::Selectable(label(*item).c_str(), item->path == request.load_path)) {
                    send(Action::LOAD_PLAN, actions);
                    // 成功载入后才更新保存目标，避免旧草稿覆盖新选择。
                    actions.debug_request.load_path = item->path;
                }
                tip(item->path.c_str());
                ImGui::PopID();
            }
            if (!visible) ImGui::TextDisabled("当前筛选没有方案，可刷新或切换地图。");
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        tip(grenade() ? "只载入独立道具库；不会显示或覆盖身法方案。运行期间不切换方案。" :
            "选择后自动载入，可继续修改并保存；同名方案可通过悬浮路径区分。运行期间不切换方案。");
        ImGui::SameLine();
        if (button("刷新方案", "只列出本页主动保存的方案；测试记录和录制草稿不会加入列表。", !busy))
            send(Action::LIST_MOVEMENT_PLANS, actions);
        if (grenade()) {
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize("新增道具").x + ImGui::GetStyle().FramePadding.x * 2)
                ImGui::NewLine();
            if (button("新增道具","清空当前保存目标和草稿，默认使用有效GSI地图与阵营、站姿左键1000ms；不继承旧图片或试投记录，不自动保存或执行。",!busy))
                new_grenade(actions);
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize("删除方案").x + ImGui::GetStyle().FramePadding.x * 2)
                ImGui::NewLine();
            if (button("删除方案","将当前已载入的方案及其独占参考资源移入backups，保留可恢复备份；不保存未保存编辑、不执行设备。运行和录制期间不可删除。",!busy && !request.load_path.empty()))
                send(Action::DELETE_PLAN,actions);
        }
    }
    void copy_preset(const movement::Config& config, bool spin, bool right, OverlayActions& actions) {
        Plan copy;
        copy.name = spin ? "旋转跳副本" : "大跳副本";
        copy.notes = "主动发送空格的固定方向时间轴；原辅助页的物理跳跃触发与方向选择保持不变。";
        copy.actions.push_back({ActionKind::KEY, Key::SPACE, 0, 40, 0});
        const int start = config.jump_delay_ms;
        const int duration = spin ? config.spin_duration_ms : config.large_duration_ms;
        const double angle = spin ? config.spin_angle_degrees : config.large_angle_degrees;
        const bool first_right = spin && right;
        copy.actions.push_back({ActionKind::KEY, first_right ? Key::D : Key::A, start, duration, 0});
        copy.actions.push_back({ActionKind::TURN, Key::W, start, duration, first_right ? angle : -angle});
        if (!spin) {
            copy.actions.push_back({ActionKind::KEY, Key::D, start + duration, duration, 0});
            copy.actions.push_back({ActionKind::TURN, Key::W, start + duration, duration, angle});
            if (config.large_ctrl_enabled)
                copy.actions.push_back({ActionKind::KEY, Key::CTRL, config.large_ctrl_delay_ms, config.large_ctrl_hold_ms, 0});
        }
        plan = std::move(copy); error.clear(); changed(actions);
    }
    void editor(OverlayActions& actions, const movement::Config& config, const Snapshot* snapshot) {
        const auto prior_team = grenade() ? selected_team() : std::string{};
        bool change = text_field(grenade() ? "道具名称" : "方案名称", plan.name, "本页独立方案文件的名称，不影响另一类方案。");
        if (grenade()) {
            if (change && plan.team.empty()) plan.team = prior_team;
            ImGui::SetNextItemWidth(std::max(80.f,ImGui::GetContentRegionAvail().x * .7f));
            if (ImGui::BeginCombo("地图", custom_map ? "自定义地图" : plan.map.empty() ? "请选择地图" : plan.map.c_str())) {
                std::vector<std::string> maps;
                for (const auto map : kGrenadeMaps) maps.emplace_back(map);
                if (!gsi_map.empty()) maps.push_back(gsi_map);
                if (!plan.map.empty()) maps.push_back(plan.map);
                if (snapshot) for (const auto& item : snapshot->grenade_saved_plans)
                    if (!item.map.empty()) maps.push_back(item.map);
                std::sort(maps.begin(),maps.end(),[](const auto& a, const auto& b) {
                    const auto left = grenade_map_rank(a), right = grenade_map_rank(b);
                    return left != right ? left < right : a < b;
                });
                maps.erase(std::unique(maps.begin(),maps.end()),maps.end());
                for (const auto& map : maps)
                    if (ImGui::Selectable(map.c_str(),!custom_map && plan.map == map)) {
                        plan.map = map; custom_map = false; change = true;
                    }
                if (ImGui::Selectable("自定义地图",custom_map)) { custom_map = true; change = true; }
                ImGui::EndCombo();
            }
            tip("选择方案所属地图，包含预置地图、GSI当前地图和方案库地图；其他地图可填写自定义标识。新草稿默认GSI，手动选择后保持，保存不会切换游戏地图。");
            if (custom_map)
                change |= text_field("自定义地图标识",plan.map,"填写游戏GSI使用的地图标识，最多83字节，仅小写字母、数字和下划线，例如de_my_map；不填写路径。");
            const auto side = selected_team();
            ImGui::SetNextItemWidth(std::max(80.f,ImGui::GetContentRegionAvail().x * .7f));
            if (ImGui::BeginCombo("阵营",side == "T" ? "T / 匪" : side == "CT" ? "CT / 警" : "请选择阵营")) {
                if (ImGui::Selectable("T / 匪",side == "T")) { plan.team = "T"; change = true; }
                if (ImGui::Selectable("CT / 警",side == "CT")) { plan.team = "CT"; change = true; }
                ImGui::EndCombo();
            }
            tip("保存独立T/CT阵营，名称可自由填写。新草稿默认GSI；旧方案优先保留已存阵营或原名称识别，同图且未指定时采用GSI。采集时仍核对实际游戏地图与阵营。");
            if (button("使用当前地图与阵营","把有效GSI地图及本地T/CT填入当前草稿；保存后生效，不执行游戏操作。",!gsi_map.empty() && !gsi_team.empty())) {
                plan.map = gsi_map; plan.team = gsi_team; custom_map = false; change = true;
            }
            ImGui::SetNextItemWidth(std::max(80.f,ImGui::GetContentRegionAvail().x * .7f));
            if (ImGui::BeginCombo("瞄点姿态",plan.crouch ? "蹲姿" : "站姿")) {
                if (ImGui::Selectable("站姿",!plan.crouch)) { plan.crouch = false; change = true; }
                if (ImGui::Selectable("蹲姿",plan.crouch)) { plan.crouch = true; change = true; }
                ImGui::EndCombo();
            }
            tip("PgDn保存实际画面；选蹲姿时需先手动蹲下并稳定后再按Ctrl+PgDn。上键自动蹲下找瞄点并保持，下键投完或取消时释放蹲姿；投掷动作与原时序保持。选择姿态只修改草稿，保存后生效，不立即输出。");
        } else {
            change |= text_field("地图（可选）", plan.map, "保存地图标识，手动身法方案不受地图限制。");
            if (button("使用当前地图", "将有效 GSI 地图填入当前草稿，保存后写入本页方案库。", snapshot && snapshot->movement_map.valid)) {
                plan.map = snapshot->movement_map.map_name; change = true;
            }
        }
        if (!grenade())
            change |= text_field("点位（可选）", plan.location, "保存点位名称；每次测试前由用户对齐位置和朝向。");
        change |= text_field(grenade() ? "定位与瞄角指令" : "起始说明", plan.notes,
            grenade() ? "在此编辑或粘贴setpos/setang，原有附注也保留；复制按钮只复制定位命令，不自动执行。" :
                "记录起始朝向、站位和人工准备步骤；不参与设备执行。");
        auto command = console_command(plan.notes);
        if (!command.empty()) {
            if (!grenade()) {
                ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x * .7f));
                ImGui::InputText("定位与瞄角指令", &command, ImGuiInputTextFlags_ReadOnly);
                tip("从起始说明提取的 setpos/setang；只提供复制，不自动执行控制台命令。道具辅助另通过参考图对齐视角，仍需先手动站到固定点位。");
            }
            if (button("复制定位指令", "只复制当前配方的精确坐标和瞄角；关闭控制台、确认站位后再按测试键执行当前动作。"))
                ImGui::SetClipboardText(command.c_str());
        }
        if (grenade()) {
            ImGui::SetNextItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x * .7f));
            if (ImGui::BeginCombo("投掷动作", "选择动作预设…")) {
                for (const auto& preset : kGrenadePresets) {
                    if (ImGui::Selectable(preset.name)) { apply_grenade_preset(plan,preset); change = true; }
                    tip("套用动作和方向键屏蔽；保留名称、地图、瞄点姿态、坐标及保存目标，只修改草稿，不自动保存或投掷。新增组合是待按点位调试的初稿。");
                }
                ImGui::EndCombo();
            }
            tip("静步使用左Shift，W默认提前200ms；助跑W默认提前300ms。可在下面按点位调整起点与时长，套用后用F11试投；不是遗迹CT所有点位的通用时序。");
        }
        if (button("添加动作", "添加独立动作；时间从脚本启动起算，与其他行可以重叠。")) {
            plan.actions.push_back({}); change = true;
        }
        int remove = -1, duplicate = -1;
        if (ImGui::BeginTable("movement_actions", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, {0, 360})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("动作"); ImGui::TableSetupColumn("开始 / ms");
            ImGui::TableSetupColumn("时长 / ms"); ImGui::TableSetupColumn("动作参数");
            ImGui::TableSetupColumn("编辑"); ImGui::TableHeadersRow();
            const char* types[]{"W", "A", "S", "D", "空格 / 跳跃", "Left Ctrl / 蹲", "鼠标左转", "鼠标右转", "屏蔽 W", "屏蔽 A", "屏蔽 S", "屏蔽 D", "鼠标位移", "滚轮", "鼠标左键", "鼠标右键", "Left Shift / 静步"};
            for (std::size_t i = 0; i < plan.actions.size(); ++i) {
                auto& action = plan.actions[i];
                ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                int kind = action.kind == ActionKind::KEY && action.key == Key::SHIFT ? 16 :
                    action.kind == ActionKind::TURN ? (action.angle_degrees < 0 ? 6 : 7) :
                    action.kind == ActionKind::MASK ? 8 + static_cast<int>(action.key) :
                    action.kind == ActionKind::MOVE ? 12 : action.kind == ActionKind::WHEEL ? 13 :
                    action.kind == ActionKind::BUTTON ? (action.button == Button::LEFT ? 14 : 15) : static_cast<int>(action.key);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##action", &kind, types, 17)) {
                    action.kind = kind < 6 || kind == 16 ? ActionKind::KEY : kind < 8 ? ActionKind::TURN : kind < 12 ? ActionKind::MASK : kind == 12 ? ActionKind::MOVE : kind == 13 ? ActionKind::WHEEL : ActionKind::BUTTON;
                    if (kind < 6) action.key = static_cast<Key>(kind);
                    else if (kind == 16) action.key = Key::SHIFT;
                    else if (kind < 8) action.angle_degrees = (kind == 6 ? -1 : 1) * std::max(1.0, std::abs(action.angle_degrees));
                    else if (kind < 12) action.key = static_cast<Key>(kind - 8);
                    else if (kind >= 14) action.button = kind == 14 ? Button::LEFT : Button::RIGHT;
                    if (action.kind != ActionKind::KEY) action.physical = false;
                    if (action.kind == ActionKind::WHEEL) { action.duration_ms = 1; if (!action.wheel) action.wheel = 1; }
                    else if (action.duration_ms <= 0) action.duration_ms = 100;
                    if (action.kind == ActionKind::MOVE && !action.dx && !action.dy) action.dx = 1;
                    change = true;
                }
                tip("按键、屏蔽和鼠标可并行。物理按键轨道在同键屏蔽区间内不输出，软件按键照常执行。");
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
                change |= ImGui::InputInt("##start", &action.start_ms, 0, 0);
                tip("相对脚本启动的毫秒数；修改本行不会移动其他动作。");
                ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(-1);
                if (action.kind == ActionKind::WHEEL) ImGui::TextDisabled("瞬时事件");
                else change |= ImGui::InputInt("##duration", &action.duration_ms, 0, 0);
                tip("按住或旋转的持续毫秒数，必须大于零；中间留空可实现连续点按。");
                ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-1);
                if (action.kind == ActionKind::TURN) {
                    double angle = std::abs(action.angle_degrees);
                    if (ImGui::InputDouble("##angle", &angle, 0, 0, "%.2f")) {
                        action.angle_degrees = (kind == 6 ? -1 : 1) * std::abs(angle); change = true;
                    }
                    tip("水平旋转目标角度；使用共享灵敏度与 m_yaw 换算，实际角度需游戏内核对。");
                } else if (action.kind == ActionKind::MOVE) {
                    change |= ImGui::InputInt("X##dx", &action.dx, 0, 0);
                    ImGui::SetNextItemWidth(-1);
                    change |= ImGui::InputInt("Y##dy", &action.dy, 0, 0);
                    tip("此区间的鼠标计数总量；按时长分配，不是屏幕像素或游戏角度。");
                    const double degrees_per_count = config.sensitivity * config.yaw_degrees_per_count;
                    const bool angle_valid = std::isfinite(degrees_per_count) && degrees_per_count > 0;
                    double angle = angle_valid ? action.dx * degrees_per_count : 0;
                    ImGui::BeginDisabled(!angle_valid);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputDouble("水平角度##move_angle", &angle, 0, 0, "%.3f") && std::isfinite(angle)) {
                        action.dx = static_cast<int>(std::clamp(std::round(angle / degrees_per_count),
                            static_cast<double>(std::numeric_limits<int>::min()),
                            static_cast<double>(std::numeric_limits<int>::max())));
                        change = true;
                    }
                    ImGui::EndDisabled();
                    tip("按共享灵敏度和 m_yaw 换算为整数 X 计数；负数左转、正数右转。实际转角需游戏内核对，Y 保留原始计数。");
                } else if (action.kind == ActionKind::WHEEL) {
                    change |= ImGui::InputInt("档数##wheel", &action.wheel, 0, 0);
                    tip("有符号滚轮档数，在开始时刻发送一次，不模拟按住。");
                } else if (action.kind == ActionKind::KEY) {
                    change |= ImGui::Checkbox("物理轨道", &action.physical);
                    tip("来自录制的物理按键。回放时扣除同键屏蔽区间；取消勾选后作为软件按键执行。");
                } else if (action.kind == ActionKind::BUTTON) {
                    ImGui::TextDisabled("按住后释放");
                    tip("起点按下、时长结束释放；左键和右键可重叠以组合投法。时序是待人工调试的方案参数，不能代替游戏落点验证。");
                } else ImGui::TextDisabled("物理屏蔽");
                ImGui::TableSetColumnIndex(4);
                if (button("复制", "复制当前动作及其时间，之后可分别修改。")) duplicate = static_cast<int>(i);
                ImGui::SameLine();
                if (button("删除", "只删除当前动作，不移动其他动作。")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (duplicate >= 0) { const auto copied = plan.actions[duplicate]; plan.actions.push_back(copied); change = true; }
        if (remove >= 0) { plan.actions.erase(plan.actions.begin() + remove); change = true; }
        change |= ImGui::Checkbox("屏蔽物理 WASD", &plan.mask.enabled);
        tip("只屏蔽选中的物理方向键，脚本方向键仍可执行；结束先释放脚本输入再恢复物理直通，仍按住 W 会继续前进。");
        ImGui::BeginDisabled(!plan.mask.enabled);
        const char* keys[]{"W", "A", "S", "D"};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            bool selected = (plan.mask.wasd & (1 << i)) != 0;
            if (ImGui::Checkbox(keys[i], &selected)) {
                plan.mask.wasd = selected ? plan.mask.wasd | (1 << i) : plan.mask.wasd & ~(1 << i); change = true;
            }
            tip("选择此物理方向键是否在屏蔽区间内交给脚本接管。");
        }
        bool first_jump = plan.mask.start_ms == -1;
        if (ImGui::Checkbox("首次空格开始屏蔽", &first_jump)) { plan.mask.start_ms = first_jump ? -1 : 0; change = true; }
        tip("以脚本首次空格按下为起点，不检测实际离地；无空格动作时不屏蔽，需要屏蔽时请指定起点。");
        if (!first_jump) {
            change |= ImGui::InputInt("屏蔽开始 / ms", &plan.mask.start_ms);
            tip("从脚本启动起算，屏蔽开始时也拦截已经持续按住的物理方向键。");
        }
        change |= ImGui::InputInt("屏蔽时长 / ms", &plan.mask.duration_ms);
        tip("0 表示持续到脚本结束和清理；正数指定屏蔽时长。");
        ImGui::EndDisabled();
        if (change) changed(actions);
        std::string validation;
        if (!movement::timeline::validate_plan(plan, validation)) ImGui::TextWrapped("待修正：%s", validation.c_str());
        else timeline(plan);
    }
};

MovementPanel::MovementPanel(debug_session::Mode mode) : impl_(std::make_unique<Impl>(mode)) {}
MovementPanel::~MovementPanel() = default;

void MovementPanel::render(const AppConfig& config, const Snapshot* snapshot, OverlayActions& actions) noexcept {
    try {
        auto& ui = *impl_;
        ui.sync(snapshot, actions);
        const bool busy = snapshot && snapshot->busy;
        const bool recording = snapshot && snapshot->movement_recording;
        if (!ui.grenade()) {
            ImGui::TextWrapped("录制可同时保留人工操作、原身法软件动作和屏蔽区间。录原快捷身法时，先启动 Runtime，再开始录制并执行身法。");
            ImGui::BeginDisabled(recording || busy);
            const char* motion_modes[]{"只存原始记录", "按相对位移解释", "按累计坐标解释"};
            ImGui::SetNextItemWidth(230);
            ImGui::Combo("录制鼠标解释", &ui.request.movement_record_motion_mode, motion_modes, 3);
            ImGui::EndDisabled();
            tip("原始 XY 语义尚未实机标定。选择相对或累计时按该解释生成可修改的方案；原始数据始终保留。只存原始记录不会生成回放方案。");
            ImGui::TextWrapped("鼠标原始值尚未实机标定：按所选解释生成编辑稿，原始数据始终保留；只存原始记录不生成回放方案。");
            if (!recording) {
                if (button("开始录制", "被动录制，不启动 Runtime，不要求当前草稿有效。录制结束后可继续编辑生成的方案。", !busy))
                    ui.send(Action::START_MOVEMENT_RECORDING, actions);
            } else {
                if (button("停止录制并生成方案", "停止采集并保留原始记录；生成当前草稿，点击保存才写入方案文件。不会停止原 Runtime。"))
                    ui.send(Action::STOP_MOVEMENT_RECORDING, actions);
                ImGui::SameLine(); ImGui::Text("已录制 %llu 个事件", static_cast<unsigned long long>(snapshot->movement_record_events));
            }
            if (snapshot && !snapshot->movement_recording_path.empty())
                ImGui::TextWrapped("录制文件：%s", snapshot->movement_recording_path.c_str());
            ImGui::Separator();
        }
        if (ui.grenade()) {
            ImGui::TextWrapped("主机OBS插件右键呼出道具菜单，点选后显示站位和瞄点参考；拿出对应道具，大致转向目标后上键对齐、下键投掷、左键取消，End全局急停。按键须启用并保存，F11手动测试仍保留。");
            tip("道具辅助只调整视角，不替代固定站位；OBS菜单点选不会自动对齐或投掷。上/下/左默认使用方向区，可在上方捕获实际数字区按键；右键由OBS插件呼出或隐藏菜单。对齐沿用当前参考资源，已通过的动作时序保持原样。");
            if (ui.new_grenade_draft || !ui.updates_current_grenade()) {
                ImGui::TextDisabled("当前为未保存的新道具草稿；保存后可用PgUp/PgDn采集参考图。");
                tip("当前是独立空白草稿，不沿用之前方案的站位图、瞄点图或投掷确认。");
            } else if (snapshot) {
                ImGui::TextWrapped("对齐状态：%s", snapshot->grenade_guidance_message.empty()
                    ? "等待选择方案与参考资源" : snapshot->grenade_guidance_message.c_str());
                tip("状态来自当前道具对齐会话。画面匹配完成只表示对齐判断通过，实际投掷结果仍需游戏内确认。");
                if (!snapshot->grenade_guidance_reference_id.empty()) {
                    ImGui::TextWrapped("参考资源：%s", snapshot->grenade_guidance_reference_id.c_str());
                    tip("当前方案绑定的参考资源标识；原始全屏图和320×320裁图存放于方案资源目录，不写入程序二进制。");
                } else {
                    ImGui::TextDisabled("参考资源尚未就绪：先采集当前固定点位的原始全屏图。");
                    tip("原图需保持与实时采集一致的画面尺寸、裁剪和视角设置；纯天空或缺少共同静态特征的方案暂不启用自动对齐。");
                }
            }
            ui.library(snapshot, actions, busy || recording);
        } else ImGui::TextWrapped("手动编排并行动作，保存为独立方案；每次执行前请对齐站位与朝向。修改只影响下次测试，当前执行使用启动时的参数。");
        ImGui::BeginDisabled(recording || (busy && snapshot->state == debug_session::State::WORKING));
        if (!ui.grenade() && ImGui::CollapsingHeader("原旋转跳与大跳预设")) {
            const auto& move = config.movement;
            ImGui::TextWrapped("原辅助页继续保留物理跳跃触发、A/D 选侧和固定 A→D 大跳逻辑。下面只复制参数为主动发送空格的自定义脚本。");
            ImGui::Text("旋转跳 %.2f° / %d ms；大跳每侧 %.2f° / %d ms", move.spin_angle_degrees, move.spin_duration_ms, move.large_angle_degrees, move.large_duration_ms);
            if (button("复制左旋转跳", "以当前旋转跳参数生成主动跳跃、固定向左的时间轴。")) ui.copy_preset(move, true, false, actions);
            ImGui::SameLine();
            if (button("复制右旋转跳", "以当前旋转跳参数生成主动跳跃、固定向右的时间轴。")) ui.copy_preset(move, true, true, actions);
            ImGui::SameLine();
            if (button("复制大跳", "复制当前 A→D 时长、转角及可选定时蹲；不修改原预设。")) ui.copy_preset(move, false, false, actions);
        }
        ui.editor(actions, config.movement, snapshot);
        ImGui::EndDisabled();
        if (!ui.error.empty()) ImGui::TextWrapped("方案错误：%s", ui.error.c_str());
        ImGui::Separator();
        if (!ui.grenade()) ui.library(snapshot, actions, busy || recording);
        if (button("校验方案", "仅校验时间轴及参数，不输出设备。", !busy && !recording)) ui.send(Action::VALIDATE, actions);
        ImGui::SameLine();
        if (ui.grenade()) {
            const bool updating = ui.updates_current_grenade();
            if (button(updating ? "更新当前道具" : "保存新道具",
                    updating ? "将当前名称、地图、点位、阵营和动作更新到已载入的道具文件。" :
                        "当前没有保存目标，将草稿保存为新的道具文件。",
                    !busy && !recording)) ui.send(Action::SAVE_PLAN,actions,!updating);
            if (updating) {
                ImGui::SameLine();
                if (button("复制为新道具","把当前道具复制为另一个新文件，原方案及参考图保持；新文件需独立采集和试投。",!busy && !recording))
                    ui.send(Action::SAVE_PLAN,actions,true);
            }
        } else {
            if (button("保存方案", "保存当前修改；已选方案直接更新，新方案自动保存到方案库，无需填写路径。", !busy && !recording)) ui.send(Action::SAVE_PLAN, actions);
            ImGui::SameLine();
            if (button("另存新方案", "将当前草稿保存为新文件，保留原方案。", !busy && !recording)) ui.send(Action::SAVE_PLAN, actions, true);
        }
        ImGui::Text("共享灵敏度 %.3f；m_yaw %.6f", config.movement.sensitivity, config.movement.yaw_degrees_per_count);
        tip("换算参数在辅助 / 身法修改；脚本目标角度不等于游戏已实际完成的转角。");
        const bool enabled = snapshot && snapshot->movement_test_enabled && snapshot->movement_test_mode == ui.request.mode;
        if (snapshot && snapshot->movement_test_enabled && !enabled) {
            ImGui::TextColored({1.f, .65f, .2f, 1.f}, "注意：当前F11仍属于%s。", snapshot->movement_test_mode == Mode::GRENADE_TEST ? "道具测试" : "身法编排");
            ImGui::TextWrapped("浏览或编辑本页不会接管快捷键；请明确启用本页后再按F11。");
        }
        if (!enabled) {
            if (button(ui.grenade() ? "启用道具测试" : "启用身法测试", "允许本页后续测试输出；启用本身不执行动作。改参、保存和载入后无需重新启用，End 或关闭测试会撤销。", !busy && !recording))
                ui.send(Action::ENABLE_MOVEMENT_TEST, actions);
        } else {
            ImGui::TextUnformatted(ui.grenade() ? "道具测试已启用：F11投掷当前道具，修改用于下一次。" : "身法测试已启用：修改后可直接测试或使用全局测试键。");
            if (button(ui.grenade() ? "投掷一次" : "测试一次", "按最新有效草稿执行一次；运行期间改参用于下一次，忙碌不排队，End 急停。", !busy && !recording))
                ui.send(Action::RUN_MOVEMENT_TEST, actions);
        }
        ImGui::SameLine();
        if (button(ui.grenade() ? "关闭道具测试" : "关闭身法测试", "取消当前测试并撤销后续输出授权，释放脚本输入、恢复物理按键；清理未确认时保留故障。", busy || enabled))
            ui.send(Action::CANCEL, actions);
        if (!ui.catalog_requested && !busy && !recording && actions.debug_action == Action::NONE) {
            ui.catalog_requested = true;
            ui.send(Action::LIST_MOVEMENT_PLANS, actions);
        }
        if (snapshot && !snapshot->message.empty()) ImGui::TextWrapped("任务反馈：%s", snapshot->message.c_str());
        ImGui::TextWrapped(ui.grenade() ? "道具库独立保存；计时完成只证明命令执行，实际烟雾落点需要游戏核对。" : "全局测试键与执行结果沿用调试模块。计时完成只证明命令执行，实际身法效果需本地游戏核对。");
    } catch (...) {
        impl_->changed(actions);
        ImGui::TextUnformatted(impl_->grenade() ? "道具草稿无法显示，请重新载入有效方案。" : "身法草稿无法显示，请重新载入有效方案。");
    }
}
