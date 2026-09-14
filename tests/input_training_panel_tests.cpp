#include "overlay/input_training_panel.h"
#include "overlay/overlay.h"
#include "input_training/input_training.h"

#include <imgui.h>
#include <cmath>
#include <iostream>
#include <memory>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}

int draw(InputTrainingPanel& panel, const std::shared_ptr<const input_training::Snapshot>& snapshot) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({900, 3000});
    ImGui::Begin("input_training_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
    OverlayActions actions;
    panel.render(snapshot, actions);
    expect(!actions.start_requested && !actions.stop_requested && !actions.training_start_requested &&
        !actions.training_stop_requested && !actions.training_load_requested && actions.runtime_intents.empty(),
        "只浏览训练快照不得自动请求录制、Runtime 启停或武装输出");
    ImGui::End();
    ImGui::Render();
    const auto* data = ImGui::GetDrawData();
    expect(data && data->Valid, "空样本、原始报告和长轨迹都必须可生成合法绘制数据");
    if (!data) return 0;
    for (int n = 0; n < data->CmdListsCount; ++n) {
        for (const auto& vertex : data->CmdLists[n]->VtxBuffer)
            expect(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "绘制坐标不得因空分母或零位移变成非有限值");
    }
    return data->TotalVtxCount;
}
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 3200}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        InputTrainingPanel panel;
        draw(panel, nullptr);
        auto snapshot = std::make_shared<input_training::Snapshot>();
        snapshot->status = input_training::Status::STOPPED;
        draw(panel, snapshot);
        snapshot->invalid_events = 2; snapshot->dropped_events = 1;
        for (int i = 0; i < 5; ++i) {
            input_training::Timing timing;
            timing.grade = static_cast<input_training::Grade>(i);
            timing.delta_ns = (i - 2) * 12000000;
            snapshot->timings.push_back(timing);
        }
        auto hold = std::make_shared<input_training::Hold>();
        hold->id = 1; hold->end = input_training::HoldEnd::GAP;
        for (int i = 0; i < 10000; ++i) {
            input_training::Point point;
            point.raw_index = i; point.event.sequence = i + 1;
            point.event.received_at_ns = static_cast<std::int64_t>(i) * 1000000;
            point.event.raw_report_valid = true;
            point.x = i; point.y = i % 40;
            hold->points.push_back(point);
        }
        snapshot->holds.push_back(hold);
        const int raw_only_vertices = draw(panel, snapshot);
        // 同一批原始报告只有显式声明位移可用后才能产生路径图。
        auto available = std::make_shared<input_training::Hold>(*hold);
        available->motion_available = true;
        snapshot->holds[0] = available;
        const int trajectory_vertices = draw(panel, snapshot);
        expect(trajectory_vertices > raw_only_vertices + 1000,
            "位移语义未知的原始报告不得生成零轨迹；可解释相对 counts 才绘制路径");
        expect(trajectory_vertices < 200000, "长按浏览应限制单帧显示窗口，不能逐帧无界绘制全部历史");
        auto zero = std::make_shared<input_training::Hold>();
        zero->id = 2; zero->motion_available = true;
        zero->points.resize(1);
        snapshot->holds.clear(); snapshot->active_hold = zero;
        draw(panel, snapshot);
    }
    ImGui::DestroyContext();
    return failures == 0 ? 0 : 1;
}
