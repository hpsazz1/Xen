#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "Xen.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "debug/pipeline_tracer.h"

namespace {

// ============================================================================
// 辅助函数
// ============================================================================

/** @brief 生成默认 CSV 导出文件名（含时间戳） */
std::string defaultCsvPath()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream oss;
    oss << "pipeline_trace_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << ".csv";
    return oss.str();
}

/**
 * @brief 将帧数据提取为 PlotLines 所需的浮点数组
 * @param frames   帧数据
 * @param getter   字段提取器（lambda: PipelineFrame → double）
 * @param maxPoints 最大点数（降采样用）
 * @return 浮点值数组
 */
std::vector<float> extractSeries(
    const std::vector<PipelineFrame>& frames,
    double (*getter)(const PipelineFrame&),
    int maxPoints = 0)
{
    std::vector<float> out;
    if (frames.empty()) return out;

    size_t n = frames.size();
    if (maxPoints > 0 && n > static_cast<size_t>(maxPoints))
    {
        // 均匀降采样
        out.reserve(maxPoints);
        double step = static_cast<double>(n - 1) / (maxPoints - 1);
        for (int i = 0; i < maxPoints; ++i)
        {
            size_t idx = static_cast<size_t>(std::round(i * step));
            if (idx >= n) idx = n - 1;
            out.push_back(static_cast<float>(getter(frames[idx])));
        }
    }
    else
    {
        out.reserve(n);
        for (const auto& f : frames)
            out.push_back(static_cast<float>(getter(f)));
    }
    return out;
}

// 字段提取器
double getRawX(const PipelineFrame& f)  { return f.rawPivotX; }
double getRawY(const PipelineFrame& f)  { return f.rawPivotY; }
double getMcX(const PipelineFrame& f)   { return f.mcPivotX; }
double getMcY(const PipelineFrame& f)   { return f.mcPivotY; }
double getPredX(const PipelineFrame& f) { return f.predX; }
double getPredY(const PipelineFrame& f) { return f.predY; }
double getVelMag(const PipelineFrame& f){ return std::hypot(f.velocityX, f.velocityY); }
double getSpeedMul(const PipelineFrame& f) { return f.speedMultiplier; }
double getDist(const PipelineFrame& f)  { return f.distToTarget; }
double getCountsMag(const PipelineFrame& f) { return std::hypot(f.countsX, f.countsY); }

/** @brief 截断显示坐标值（保留 1 位小数） */
const char* fmtCoord(double v)
{
    static char buf[32];
    if (std::abs(v) < 1e-6)
        snprintf(buf, sizeof(buf), "0.0");
    else
        snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

/** @brief 截断显示整数值 */
const char* fmtInt(int v)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%+d", v);
    return buf;
}

/** @brief 截断显示浮点差值（带符号） */
const char* fmtDelta(double v)
{
    static char buf[32];
    if (std::abs(v) < 1e-6)
        snprintf(buf, sizeof(buf), " 0.0");
    else if (v > 0)
        snprintf(buf, sizeof(buf), "+%.1f", v);
    else
        snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

// ============================================================================
// 表格绘制辅助
// ============================================================================

void drawStageTable(const PipelineFrame& pf)
{
    if (!ImGui::BeginTable("##stage_table", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchSame))
        return;

    ImGui::TableSetupColumn("处理阶段", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn(u8"Δ距离", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableHeadersRow();

    auto row = [](const char* stage, double x, double y, double prevX, double prevY) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(stage);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(fmtCoord(x));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(fmtCoord(y));
        ImGui::TableSetColumnIndex(3);
        double d = std::hypot(x - prevX, y - prevY);
        if (std::abs(d) < 1e-6)
            ImGui::TextUnformatted("-");
        else
            ImGui::Text("%.1f", d);
        return std::make_pair(x, y);
    };

    // 前 4 个阶段是绝对坐标
    auto p0 = std::make_pair(0.0, 0.0);
    auto p1 = row(u8"原始目标",  pf.rawPivotX, pf.rawPivotY, p0.first, p0.second);
    auto p2 = row(u8"运动补偿",  pf.mcPivotX,  pf.mcPivotY,  p1.first, p1.second);
    auto p3 = row(u8"预测外推",  pf.predX,      pf.predY,      p2.first, p2.second);

    // Stage 4: 速度曲线 (显示倍率，非坐标)
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(u8"距离/倍率");
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%.1f px", pf.distToTarget);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.3fx", pf.speedMultiplier);
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted("-");

    // 后 4 个阶段是增量 (delta)
    auto p4 = std::make_pair(0.0, 0.0);
    auto p5 = row(u8"PurePursuit(Δ)", pf.ppDx,     pf.ppDy,     p4.first, p4.second);
    auto p6 = row(u8"→Counts",       pf.countsX,   pf.countsY,   p5.first, p5.second);
    auto p7 = row(u8"→EMA平滑",      pf.emaCountsX, pf.emaCountsY, p6.first, p6.second);

    // Stage 8: 最终输出 (整数)
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(u8"最终输出(int)");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(fmtInt(pf.finalMx));
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(fmtInt(pf.finalMy));
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted("-");

    ImGui::EndTable();
}

} // anonymous namespace

// ============================================================================
// 主绘制函数
// ============================================================================

void draw_pipeline_tracer()
{
    bool changed = false;

    // ========== 控制栏 ==========
    if (OverlayUI::BeginSection(u8"流水线追踪", "pipeline_tracer_section"))
    {
        // ---- 启用开关 ----
        {
            bool prevEnabled = config.pipeline_tracer_enabled;
            if (OverlayUI::CheckboxRow(u8"启用追踪", &config.pipeline_tracer_enabled))
            {
                g_pipelineTracer.setEnabled(config.pipeline_tracer_enabled);
                if (config.pipeline_tracer_enabled != prevEnabled)
                    OverlayConfig_MarkDirty();
                changed = true;
            }
        }

        // ---- 缓冲帧数滑块 ----
        {
            int maxFrames = config.pipeline_tracer_max_frames;
            if (OverlayUI::SliderIntRow(u8"缓冲帧数", &maxFrames, 50, 2000, "%d"))
            {
                config.pipeline_tracer_max_frames = maxFrames;
                g_pipelineTracer.setMaxFrames(static_cast<size_t>(maxFrames));
                OverlayConfig_MarkDirty();
                changed = true;
            }
        }

        // ---- 操作按钮行 ----
        {
            const auto row = OverlayUI::BeginSettingRow(u8"操作");
            const float btnW = (row.controlWidth - 12.0f) / 3.0f;

            // 清除
            if (ImGui::Button(u8"清除数据", ImVec2(btnW, 0.0f)))
                g_pipelineTracer.clear();

            ImGui::SameLine(0.0f, 6.0f);

            // 导出 CSV
            if (ImGui::Button(u8"导出CSV", ImVec2(btnW, 0.0f)))
            {
                std::string path = defaultCsvPath();
                if (!g_pipelineTracer.exportCSV(path))
                {
                    // 导出失败静默处理（错误已在 exportCSV 内打印）
                }
            }

            ImGui::SameLine(0.0f, 6.0f);

            // 帧计数显示
            ImGui::TextDisabled(u8"帧:%lld", static_cast<long long>(g_pipelineTracer.getFrameCounter()));

            OverlayUI::EndSettingRow(row);
        }

        OverlayUI::EndSection();
    }

    // ========== 获取数据 ==========
    auto frames = g_pipelineTracer.getFrames();
    if (frames.empty())
    {
        if (config.pipeline_tracer_enabled)
            OverlayUI::TextRow(u8"等待数据… 瞄准目标后将自动记录各阶段坐标。", IM_COL32(128, 140, 160, 255));
        else
            OverlayUI::TextRow(u8"追踪已禁用。勾选「启用追踪」开始记录流水线数据。", IM_COL32(128, 140, 160, 255));
        if (changed) OverlayConfig_MarkDirty();
        return;
    }

    const auto& latest = frames.back();
    constexpr int kMaxPlotPoints = 600;  // PlotLines 最多显示 600 个采样点

    // ========== X 坐标时序图 ==========
    if (OverlayUI::BeginSection(u8"X 坐标 — 各阶段对比", "pt_x_coord"))
    {
        auto rawSeries  = extractSeries(frames, getRawX,  kMaxPlotPoints);
        auto mcSeries   = extractSeries(frames, getMcX,   kMaxPlotPoints);
        auto predSeries = extractSeries(frames, getPredX, kMaxPlotPoints);

        if (!rawSeries.empty())
        {
            // 计算 Y 轴范围
            float allMin = rawSeries[0], allMax = rawSeries[0];
            for (auto* s : { &rawSeries, &mcSeries, &predSeries })
                for (float v : *s)
                { if (v < allMin) allMin = v; if (v > allMax) allMax = v; }
            float range = allMax - allMin;
            if (range < 20.0f) { float mid = (allMin + allMax) * 0.5f; allMin = mid - 10.0f; allMax = mid + 10.0f; }

            ImGui::PlotLines("##plot_x_raw",  rawSeries.data(),  static_cast<int>(rawSeries.size()),  0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"原始");
            ImGui::PlotLines("##plot_x_mc",   mcSeries.data(),   static_cast<int>(mcSeries.size()),   0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"补偿");
            ImGui::PlotLines("##plot_x_pred", predSeries.data(), static_cast<int>(predSeries.size()), 0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"预测");
        }
        OverlayUI::EndSection();
    }

    // ========== Y 坐标时序图 ==========
    if (OverlayUI::BeginSection(u8"Y 坐标 — 各阶段对比", "pt_y_coord"))
    {
        auto rawSeries  = extractSeries(frames, getRawY,  kMaxPlotPoints);
        auto mcSeries   = extractSeries(frames, getMcY,   kMaxPlotPoints);
        auto predSeries = extractSeries(frames, getPredY, kMaxPlotPoints);

        if (!rawSeries.empty())
        {
            float allMin = rawSeries[0], allMax = rawSeries[0];
            for (auto* s : { &rawSeries, &mcSeries, &predSeries })
                for (float v : *s)
                { if (v < allMin) allMin = v; if (v > allMax) allMax = v; }
            float range = allMax - allMin;
            if (range < 20.0f) { float mid = (allMin + allMax) * 0.5f; allMin = mid - 10.0f; allMax = mid + 10.0f; }

            ImGui::PlotLines("##plot_y_raw",  rawSeries.data(),  static_cast<int>(rawSeries.size()),  0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"原始");
            ImGui::PlotLines("##plot_y_mc",   mcSeries.data(),   static_cast<int>(mcSeries.size()),   0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"补偿");
            ImGui::PlotLines("##plot_y_pred", predSeries.data(), static_cast<int>(predSeries.size()), 0, nullptr, allMin, allMax, ImVec2(0, 80));
            ImGui::SameLine(); ImGui::TextDisabled(u8"预测");
        }
        OverlayUI::EndSection();
    }

    // ========== 速度与倍率时序图 ==========
    if (OverlayUI::BeginSection(u8"速度与倍率", "pt_speed"))
    {
        auto velSeries = extractSeries(frames, getVelMag,    kMaxPlotPoints);
        auto mulSeries = extractSeries(frames, getSpeedMul,  kMaxPlotPoints);
        auto distSeries = extractSeries(frames, getDist,     kMaxPlotPoints);

        if (!velSeries.empty())
        {
            float velMin = 0.0f, velMax = 1.0f;
            for (float v : velSeries) { if (v > velMax) velMax = v; }
            velMax = std::max(velMax, 10.0f);

            ImGui::PlotLines("##plot_vel", velSeries.data(), static_cast<int>(velSeries.size()), 0,
                u8"速度(px/s)", 0.0f, velMax, ImVec2(0, 60));
        }
        if (!mulSeries.empty())
        {
            ImGui::PlotLines("##plot_mul", mulSeries.data(), static_cast<int>(mulSeries.size()), 0,
                u8"速度倍率", 0.0f, 1.0f, ImVec2(0, 60));
        }
        OverlayUI::EndSection();
    }

    // ========== 最新帧阶段明细表 ==========
    if (OverlayUI::BeginSection(u8"最新帧阶段明细", "pt_stage_table"))
    {
        ImGui::TextDisabled(u8"帧 #%lld | 分辨率 %d | FPS %.1f | 延迟 %.1fms | 类别 %d | 外部速度 %s",
            static_cast<long long>(latest.frameId),
            latest.resolution,
            latest.fpsValue,
            latest.observationAgeSec * 1000.0,
            latest.targetClassId,
            latest.hasExternalVel ? u8"是" : u8"否");

        ImGui::Spacing();
        drawStageTable(latest);
        OverlayUI::EndSection();
    }

    // ========== 统计摘要 ==========
    if (OverlayUI::BeginSection(u8"统计摘要", "pt_stats"))
    {
        double sumVel = 0.0, sumSpeedMul = 0.0, sumDist = 0.0, sumDelay = 0.0;
        double maxVel = 0.0;
        int velCount = 0;

        for (const auto& f : frames)
        {
            double v = std::hypot(f.velocityX, f.velocityY);
            if (v > 0.0) { sumVel += v; velCount++; if (v > maxVel) maxVel = v; }
            sumSpeedMul += f.speedMultiplier;
            sumDist += f.distToTarget;
            sumDelay += f.observationAgeSec;
        }

        size_t n = frames.size();
        double avgVel = velCount > 0 ? sumVel / velCount : 0.0;
        double avgSpeedMul = n > 0 ? sumSpeedMul / n : 0.0;
        double avgDist = n > 0 ? sumDist / n : 0.0;
        double avgDelayMs = n > 0 ? (sumDelay / n) * 1000.0 : 0.0;

        ImGui::Text(u8"帧数: %zu / %zu", n, g_pipelineTracer.getMaxFrames());
        ImGui::Text(u8"平均速度: %.1f px/s", avgVel);
        ImGui::Text(u8"峰值速度: %.1f px/s", maxVel);
        ImGui::Text(u8"平均距离: %.1f px", avgDist);
        ImGui::Text(u8"平均倍率: %.3f", avgSpeedMul);
        ImGui::Text(u8"平均延迟: %.1f ms", avgDelayMs);

        // 统计 Counts 输出幅度
        double sumCountsMag = 0.0;
        int countNonZero = 0;
        for (const auto& f : frames)
        {
            double m = std::hypot(f.finalMx, f.finalMy);
            if (m > 0.5) { sumCountsMag += m; countNonZero++; }
        }
        if (countNonZero > 0)
            ImGui::Text(u8"平均输出: %.1f counts (%d 帧非零)", sumCountsMag / countNonZero, countNonZero);

        OverlayUI::EndSection();
    }

    if (changed)
        OverlayConfig_MarkDirty();
}
