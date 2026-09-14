#include "overlay/input_training_panel.h"
#include "overlay/overlay.h"
#include "input_training/input_training.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
struct IdScope {
    explicit IdScope(const char* id) { ImGui::PushID(id); }
    ~IdScope() { ImGui::PopID(); }
};
struct DisabledScope {
    explicit DisabledScope(bool disabled) { ImGui::BeginDisabled(disabled); }
    ~DisabledScope() { ImGui::EndDisabled(); }
};
void help(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

const char* grade_label(input_training::Grade grade) {
    using input_training::Grade;
    switch (grade) {
    case Grade::PERFECT: return "完美";
    case Grade::EXCELLENT: return "优秀";
    case Grade::EARLY: return "偏早";
    case Grade::LATE: return "偏晚";
    default: return "未分类";
    }
}

ImU32 grade_color(input_training::Grade grade) {
    using input_training::Grade;
    switch (grade) {
    case Grade::PERFECT: return IM_COL32(45, 170, 125, 255);
    case Grade::EXCELLENT: return IM_COL32(55, 145, 225, 255);
    case Grade::EARLY: return IM_COL32(200, 130, 45, 255);
    case Grade::LATE: return IM_COL32(210, 75, 95, 255);
    default: return IM_COL32(135, 135, 145, 255);
    }
}

void timing_summary(const input_training::Snapshot& snapshot) {
    std::array<std::uint64_t, 5> counts{};
    double maximum_ms = 10.0;
    for (const auto& timing : snapshot.timings) {
        const auto grade = static_cast<std::size_t>(timing.grade);
        ++counts[grade < counts.size() ? grade : 4];
        maximum_ms = std::max(maximum_ms, std::abs(timing.delta_ns / 1e6));
    }
    const auto total = snapshot.timings.size();
    const auto classified = total - counts[4];
    ImGui::SeparatorText("换键接收时序");
    ImGui::TextWrapped("参考分级：|时差| ≤ 2 ms 完美，≤ 10 ms 优秀；其余负值偏早、正值偏晚。负值为重叠，正值为空隙。评分不代表角色停稳。");
    ImGui::Text("近期可分类 %llu / %llu，未分类 %llu；Run 配对总数 %llu，未配对边沿 %llu",
        static_cast<unsigned long long>(classified), static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(counts[4]), static_cast<unsigned long long>(snapshot.total_timings),
        static_cast<unsigned long long>(snapshot.unpaired_edges));
    if (ImGui::BeginTable("timing_counts", 4, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 4; ++i) {
            ImGui::TableNextColumn();
            ImGui::Text("%s  %llu", grade_label(static_cast<input_training::Grade>(i)),
                static_cast<unsigned long long>(counts[i]));
            if (classified) ImGui::Text("%.1f%%", 100.0 * counts[i] / classified);
            else ImGui::TextUnformatted("占比不可评估");
        }
        ImGui::EndTable();
    }
    if (snapshot.timings.empty()) {
        ImGui::TextUnformatted("暂无换键记录；空样本不计为 0% 成功。");
        return;
    }
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(160.0f, ImGui::GetContentRegionAvail().x), height = 110.0f;
    ImGui::InvisibleButton("timing_distribution", {width, height});
    help("横向由旧到新；纵向是带符号接收时差，单位 ms。中心线是 0；未知传输误差不能据此推断物理按键精度。");
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(235, 240, 246, 255), 4);
    draw->AddLine({origin.x + 8, origin.y + height / 2}, {origin.x + width - 8, origin.y + height / 2}, IM_COL32(145, 150, 160, 255));
    for (std::size_t i = 0; i < total; ++i) {
        const auto& timing = snapshot.timings[i];
        const float x = origin.x + 12 + (width - 24) * static_cast<float>(i) / std::max<std::size_t>(1, total - 1);
        const float y = origin.y + height / 2 - static_cast<float>(timing.delta_ns / 1e6 / maximum_ms) * (height / 2 - 12);
        draw->AddCircleFilled({x, y}, 3.0f, grade_color(timing.grade));
    }
    ImGui::Text("上 +%.1f ms / 中 0 / 下 -%.1f ms；横向从旧到新", maximum_ms, maximum_ms);
    if (ImGui::BeginTable("timing_records", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, {0, 155})) {
        ImGui::TableSetupColumn("方向"); ImGui::TableSetupColumn("时差 / ms");
        ImGui::TableSetupColumn("接收域分级"); ImGui::TableSetupColumn("时间可信度");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(total));
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& timing = snapshot.timings[total - 1 - i];
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%c → %c", timing.from, timing.to);
            ImGui::TableNextColumn(); ImGui::Text("%+.3f", timing.delta_ns / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(grade_label(timing.grade));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(timing.atomic_ambiguous ? "同包边沿歧义" : timing.uncertainty_crosses_boundary ? "误差跨分类边界" : timing.timing_uncertainty_known ? "误差已提供" : "传输误差未知");
        }
        ImGui::EndTable();
    }
}

void trajectory(const input_training::Hold& hold, int selected) {
    if (!hold.motion_available) {
        ImGui::TextWrapped("轨迹不可用：来源位移的增量语义尚未验证。KMBOX monitor 原始报告已保留；不能用 raw_x/raw_y 猜出物理路径，也不绘制伪零轨迹。");
        return;
    }
    if (hold.points.empty()) return;
    // 只限制单帧画图窗口；原始事件、导出和全量时间索引均不抽样。
    constexpr std::size_t kVisiblePoints = 4096;
    const auto end = std::min(static_cast<std::size_t>(selected), hold.points.size() - 1);
    const auto begin = end >= kVisiblePoints ? end + 1 - kVisiblePoints : 0;
    double low_x = static_cast<double>(hold.points[begin].x), high_x = low_x;
    double low_y = static_cast<double>(hold.points[begin].y), high_y = low_y;
    for (auto i = begin; i <= end; ++i) {
        low_x = std::min(low_x, static_cast<double>(hold.points[i].x)); high_x = std::max(high_x, static_cast<double>(hold.points[i].x));
        low_y = std::min(low_y, static_cast<double>(hold.points[i].y)); high_y = std::max(high_y, static_cast<double>(hold.points[i].y));
    }
    ImGui::TextWrapped("相对 counts 输入层；%s。蓝 → 橙为时间方向，黄圈为显示拐点（≥45°），箭头指示移动方向。", hold.physical_motion_verified ? "来源声明物理位移已验证" : "物理来源未实机验证，不代表弹着点或游戏视角");
    ImGui::Text("显示原始索引 %llu…%llu / %llu 点（最多 4096 点；拖动时间轴浏览全部）",
        static_cast<unsigned long long>(hold.points[begin].raw_index), static_cast<unsigned long long>(hold.points[end].raw_index),
        static_cast<unsigned long long>(hold.points.size()));
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(160.0f, ImGui::GetContentRegionAvail().x), height = 280.0f;
    ImGui::InvisibleButton("hold_trajectory", {width, height});
    help("横向和纵向使用相同缩放比例，+x 向右、+y 向下；按当前原始时间窗适配画布，未归一化为弹道。可拖动上方原始事件滑块查看长按的其他时段。");
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(235, 240, 246, 255), 4);
    const double scale = std::min((width - 48) / std::max(1.0, high_x - low_x), (height - 48) / std::max(1.0, high_y - low_y));
    const double center_x = low_x + (high_x - low_x) / 2, center_y = low_y + (high_y - low_y) / 2;
    auto position = [&](std::size_t i) {
        return ImVec2{origin.x + width / 2 + static_cast<float>((hold.points[i].x - center_x) * scale),
                      origin.y + height / 2 + static_cast<float>((hold.points[i].y - center_y) * scale)};
    };
    const auto first_ns = hold.points.front().event.received_at_ns;
    const double duration_ns = std::max(1.0, static_cast<double>(hold.points.back().event.received_at_ns) - static_cast<double>(first_ns));
    std::size_t last_arrow = begin;
    for (auto i = begin + 1; i <= end; ++i) {
        const auto a = position(i - 1), b = position(i);
        const float t = static_cast<float>(std::clamp((static_cast<double>(hold.points[i].event.received_at_ns) - first_ns) / duration_ns, 0.0, 1.0));
        const auto color = ImGui::ColorConvertFloat4ToU32({0.2f + 0.7f * t, 0.55f - 0.15f * t, 0.9f - 0.65f * t, 1});
        draw->AddLine(a, b, color, 1.8f);
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::sqrt(dx * dx + dy * dy);
        if (length >= 9.0f && i - last_arrow >= std::max<std::size_t>(1, (end - begin) / 20)) {
            const float ux = dx / length, uy = dy / length;
            draw->AddTriangleFilled(b, {b.x - 7 * ux + 3 * uy, b.y - 7 * uy - 3 * ux}, {b.x - 7 * ux - 3 * uy, b.y - 7 * uy + 3 * ux}, color);
            last_arrow = i;
        }
        if (std::abs(hold.points[i].turn_radians) >= 0.7853981633974483)
            draw->AddCircle(b, 3.0f, IM_COL32(220, 165, 35, 255), 8);
    }
    const auto a = position(begin), b = position(end);
    draw->AddCircleFilled(a, 5, IM_COL32(45, 170, 125, 255));
    draw->AddCircleFilled(b, 5, IM_COL32(210, 75, 95, 255));
    draw->AddText({a.x + 7, a.y - 14}, IM_COL32(30, 95, 70, 255), begin == 0 ? "起点" : "窗口起点");
    const char* end_label = end + 1 != hold.points.size() ? "所选时刻" :
        hold.end == input_training::HoldEnd::RELEASED ? "终点" :
        hold.end == input_training::HoldEnd::ACTIVE ? "当前末点" : "不完整末点";
    draw->AddText({b.x + 7, b.y + 3}, IM_COL32(140, 45, 65, 255), end_label);
    ImGui::Text("本窗缩放 %.3f px/count；全段首点 (%lld, %lld)，末点 (%lld, %lld)", scale,
        static_cast<long long>(hold.points.front().x), static_cast<long long>(hold.points.front().y),
        static_cast<long long>(hold.points.back().x), static_cast<long long>(hold.points.back().y));
}
}

struct InputTrainingPanel::Impl {
    std::string directory = "reports/input-training", load_path;
    std::uint64_t selected_hold_id = 0;
    int selected_raw = 0;
    bool follow_tail = true;

    void render(const std::shared_ptr<const input_training::Snapshot>& snapshot, OverlayActions& actions) {
        if (!ImGui::CollapsingHeader("输入训练与报告", ImGuiTreeNodeFlags_DefaultOpen)) {
            help("评估 KMBOX monitor 接收时序与按住记录；与模型训练无关。不会启动控制输出、修改急停或压枪参数。");
            return;
        }
        help("评估输入接收时序和完整按住记录；与模型训练无关。按需记录，UI 不读取设备或磁盘。");
        IdScope id_scope("input_training_panel");
        ImGui::TextWrapped("来源：已有 KMBOX monitor 或离线 Run。记录不会武装输出。接收时间不是物理按下、游戏开枪或停稳时间；设备源丢包目前不可验证。");
        const bool busy = snapshot && (snapshot->status == input_training::Status::RECORDING || snapshot->status == input_training::Status::REPLAYING);
        {
        DisabledScope disabled(busy);
        ImGui::TextUnformatted("录制根目录");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##training_directory", &directory);
        help("录制根目录，默认 reports/input-training；开始后由 App 创建唯一子目录，不覆盖旧档。目录不可写时报告失败，不开始无限采集。");
        if (ImGui::Button("开始输入记录")) { actions.training_directory = directory; actions.training_start_requested = true; }
        help("只读取已经打开的 KMBOX monitor；未连接或后端不支持时明确失败。不启动游戏、检测或鼠标输出；达到预算自动停止。");
        }
        ImGui::SameLine();
        {
        DisabledScope disabled(!busy);
        if (ImGui::Button("结束并保存")) actions.training_stop_requested = true;
        help("停止接收并排空已入队事件；没有合法松开边沿的按住片段标为不完整。离线读取停止时不改原档。");
        }
        {
        DisabledScope disabled(busy);
        ImGui::TextUnformatted("回看 Run 目录");
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##training_load_path", &load_path);
        help("填写具体 Run 目录；后台读取原始档案并复用评估器。不是录制根目录，不覆盖原始数据。");
        DisabledScope no_path(load_path.empty());
        if (ImGui::Button("读取离线记录")) { actions.training_load_path = load_path; actions.training_load_requested = true; }
        help("仅回放档案事实用于浏览和评分，不向鼠标设备发送命令。文件无效或缺块会报告失败或不完整。");
        }
        if (snapshot) {
            ImGui::TextUnformatted(snapshot->replay_source ? "当前来源：离线 Run 回放（不等于实机验收）" : "当前来源：在线 KMBOX monitor 接收报告");
            ImGui::Text("状态：%s", input_training::status_name(snapshot->status));
            if (!snapshot->directory.empty()) ImGui::TextWrapped("Run：%s", snapshot->directory.c_str());
            if (!snapshot->error.empty()) ImGui::TextWrapped("记录问题：%s", snapshot->error.c_str());
            ImGui::Text("已收 %llu；丢弃 %llu；无效 %llu；归档 %.2f MiB / %llu 块",
                static_cast<unsigned long long>(snapshot->received_events), static_cast<unsigned long long>(snapshot->dropped_events),
                static_cast<unsigned long long>(snapshot->invalid_events), snapshot->archive_bytes / 1048576.0,
                static_cast<unsigned long long>(snapshot->chunks));
            timing_summary(*snapshot);
            ImGui::SeparatorText("按下至松开的原始输入记录");
            ImGui::Text("Run 片段总数 %llu；近期保留 %llu（另含进行中片段）",
                static_cast<unsigned long long>(snapshot->total_holds), static_cast<unsigned long long>(snapshot->holds.size()));
            std::shared_ptr<const input_training::Hold> selected;
            for (const auto& hold : snapshot->holds) if (hold && hold->id == selected_hold_id) selected = hold;
            if (snapshot->active_hold && snapshot->active_hold->id == selected_hold_id) selected = snapshot->active_hold;
            if (!selected) selected = snapshot->active_hold ? snapshot->active_hold : snapshot->holds.empty() ? nullptr : snapshot->holds.back();
            if (selected) {
                if (selected_hold_id != selected->id) { selected_hold_id = selected->id; follow_tail = true; }
                const auto label = "按住 #" + std::to_string(selected->id);
                // 所有会抛出的标签分配在 BeginCombo 前完成，展开期间只浏览已有字符串。
                std::vector<std::pair<std::shared_ptr<const input_training::Hold>, std::string>> hold_items;
                auto prepare_item = [&](const std::shared_ptr<const input_training::Hold>& hold) {
                    if (hold) hold_items.emplace_back(hold, "#" + std::to_string(hold->id) + " / " + input_training::hold_end_name(hold->end));
                };
                prepare_item(snapshot->active_hold);
                for (auto it = snapshot->holds.rbegin(); it != snapshot->holds.rend(); ++it) prepare_item(*it);
                if (ImGui::BeginCombo("查看片段", label.c_str())) {
                    for (const auto& [hold, name] : hold_items) {
                        if (ImGui::Selectable(name.c_str(), selected_hold_id == hold->id)) { selected = hold; selected_hold_id = hold->id; follow_tail = true; }
                    }
                    ImGui::EndCombo();
                }
                help("选择已保留按住片段；完整原始事件仍在 Run 中。Recoil 输出耗尽不会被当作物理松开。");
                ImGui::TextWrapped("结束：%s；接收流完整：%s；源丢包可验证：%s；同包边界歧义：%s",
                    input_training::hold_end_name(selected->end), selected->complete_received_stream ? "是" : "否",
                    selected->source_loss_verifiable ? "是" : "否", selected->boundary_ambiguous ? "有" : "无");
                if (!selected->points.empty()) {
                    const auto maximum = static_cast<int>(std::min<std::size_t>(selected->points.size() - 1, std::numeric_limits<int>::max()));
                    selected_raw = follow_tail ? maximum : std::clamp(selected_raw, 0, maximum);
                    if (ImGui::SliderInt("原始事件时间轴", &selected_raw, 0, maximum)) follow_tail = false;
                    help("索引直接映射原始事件，包含零位移按钮报告；没有 2 ms 合并或 128 点重采样。接收间隔不等于设备产生间隔。");
                    ImGui::Checkbox("跟随末尾", &follow_tail);
                    help("启用时随新快照查看最后一个原始事件；拖动时间轴后自动关闭，不改变记录。");
                    const auto& point = selected->points[static_cast<std::size_t>(selected_raw)];
                    const double elapsed = (static_cast<double>(point.event.received_at_ns) - selected->points.front().event.received_at_ns) / 1e6;
                    ImGui::Text("raw %llu / epoch %llu / seq %llu / +%.3f ms / 左键 %s",
                        static_cast<unsigned long long>(point.raw_index), static_cast<unsigned long long>(point.event.epoch),
                        static_cast<unsigned long long>(point.event.sequence), elapsed, point.event.left_down ? "按住" : "松开");
                    if (point.event.motion_valid) ImGui::Text("dx %d / dy %d counts；转角 %+.1f°", point.event.dx, point.event.dy, point.turn_radians * 57.29577951308232);
                    else ImGui::TextUnformatted("本报告位移不可解释；未将缺失值填成 0。");
                    if (point.event.gap || point.boundary_ambiguous) ImGui::TextUnformatted("所选事件含缺口或同包边界歧义，不推测包内先后。");
                }
                trajectory(*selected, selected_raw);
            } else ImGui::TextUnformatted("暂无按住记录；录制开始时已按住或缺失边沿的片段会标记不完整。");
        } else ImGui::TextUnformatted("尚未记录。开始前需要已有可用 KMBOX 输入；也可直接读取离线 Run。");
        ImGui::SeparatorText("后续评估");
        ImGui::TextWrapped("开枪稳定：缺少游戏实际逐发和移动误差来源，当前不可评估，不生成稳定占比。身法：后续先核实转向和空中状态；跳键窗口不等于空中加速。");
    }
};

InputTrainingPanel::InputTrainingPanel() : impl_(std::make_unique<Impl>()) {}
InputTrainingPanel::~InputTrainingPanel() = default;
void InputTrainingPanel::render(const std::shared_ptr<const input_training::Snapshot>& snapshot, OverlayActions& actions) noexcept {
    try {
        impl_->render(snapshot, actions);
    } catch (...) {
        // 失败仅取消训练页本帧动作；自身 ID/禁用栈由 RAII 还原，不影响控制动作。
        actions.training_start_requested = false;
        actions.training_stop_requested = false;
        actions.training_load_requested = false;
        ImGui::TextUnformatted("输入训练界面暂时无法显示；本帧训练动作已取消，请重试。");
    }
}
