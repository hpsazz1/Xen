#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "counterpulse_hud.h"
#include "hud_feedback_internal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cmath>

namespace auto_stop_probe_detail {
class CounterpulseHud::Impl {
public:
    struct Entry {
        std::int64_t planned = 0, submit = 0, ack = 0, completed = 0, returned = 0;
        int value = 0, disposition = 0, shot = 0;
        bool keyboard = false;
    };
    static constexpr std::size_t capacity = 512;
    std::array<Entry, capacity> queue;
    std::atomic<std::size_t> written{0}, consumed{0};
    std::atomic<unsigned> lost{0};
    std::atomic<unsigned> paint_count{0}, title_updates{0};
    std::atomic<unsigned> valid_first_plot_count{0};
    std::atomic<int> state{0}; // 0启动中、1显示中、2结束保留、3窗口/模型失败。
    std::atomic<bool> ending{false}, execution_success{false}, closing{false};
    std::atomic<std::int64_t> finished_at{0};
    const bool external;
    struct PublishedSnapshot { Json value; unsigned version; };
    std::atomic<std::shared_ptr<const PublishedSnapshot>> pending_snapshot;
    Json displayed = {{"shots", Json::array()}, {"timings", Json::array()}, {"current_model", {{"valid", false}}}};
    std::atomic<unsigned> snapshots{0}, snapshots_displayed{0}, snapshots_superseded{0};
    std::atomic<bool> monitor_source{false}, window_closed{false};
    std::atomic<bool> stop_recording{false}, recording{true};
    struct PlotPoint {
        double value = 0; bool valid = false; std::uint64_t ordinal = 0;
        bool operator==(const PlotPoint& other) const {
            return valid == other.valid && ordinal == other.ordinal && (!valid || value == other.value);
        }
    };
    std::vector<PlotPoint> deltas, ratios;
    std::wstring left_title, right_title, footer;
    std::wstring parameter_text;
    double default_threshold_q = 1;
    sampling_detail::SamplingSettings settings;
    std::jthread worker;
    HWND window = nullptr;
    HWND stop_button = nullptr;
    HFONT font = nullptr;
    std::wstring window_title;
    Json commands = Json::array();

    explicit Impl(const sampling_detail::SamplingSettings& value, bool external_snapshots) : external(external_snapshots), settings(value) {
        worker = std::jthread([this] { run(); });
    }
    ~Impl() { closing.store(true); if (worker.joinable()) worker.join(); }

    void enqueue(const Json& command) noexcept {
        if (external) { ++lost; return; }
        try {
            Entry event;
            const auto kind = command.at("kind").get<std::string>();
            if (kind != "wasd" && kind != "left_button") throw std::runtime_error("HUD命令种类无效");
            event.keyboard = kind == "wasd";
            event.value = command.at("value").get<int>();
            if (event.value < 0 || event.value > (event.keyboard ? 15 : 1))
                throw std::runtime_error("HUD键态越界");
            event.disposition = command.at("disposition").get<int>();
            event.shot = command.at("shot_index").get<int>();
            event.planned = command.at("planned_ns").get<std::int64_t>();
            event.submit = command.at("submit_ns").get<std::int64_t>();
            event.ack = command.at("ack_received_ns").get<std::int64_t>();
            event.completed = command.at("backend_completed_ns").get<std::int64_t>();
            event.returned = command.at("returned_ns").get<std::int64_t>();
            const auto next = written.load(std::memory_order_relaxed);
            if (next - consumed.load(std::memory_order_acquire) >= capacity) { ++lost; return; }
            queue[next % capacity] = event;
            written.store(next + 1, std::memory_order_release);
        } catch (...) { ++lost; }
    }
    void publish_snapshot(const Json& input) noexcept {
        try {
            if (!external || !input.is_object()) throw std::runtime_error("HUD外部模式快照无效");
            const auto& analysis = input.contains("analysis") ? input.at("analysis") : input;
            const auto source = input.value("source", analysis.value("source", ""));
            if (source != "KMBOX_MONITOR" && source != "COMMAND_ACK_PROXY") throw std::runtime_error("HUD来源无效");
            Json compact{{"shots", Json::array()}, {"timings", Json::array()},
                {"current_model", analysis.value("current_model", Json{{"valid", false}})}, {"source", source},
                {"recording", input.value("recording", true)},
                {"recording_id", input.value("recording_id", analysis.value("recording_id", ""))}};
            const auto& shots = analysis.at("shots");
            if (!shots.is_array()) throw std::runtime_error("HUD开火记录无效");
            compact["shot_count"] = analysis.value("shot_count", shots.size());
            for (std::size_t i = shots.size() > 32 ? shots.size() - 32 : 0; i < shots.size(); ++i) {
                const auto& shot = shots[i];
                Json small{{"ordinal", shot.value("ordinal", i + 1)}, {"atomic_ambiguous", shot.value("atomic_ambiguous", false)},
                    {"down_model", shot.value("down_model", Json{{"valid", false}})}, {"samples", Json::array()}};
                if (shot.contains("samples") && shot["samples"].is_array())
                    for (const auto& sample : shot["samples"])
                        if (sample.value("kind", "FIRST_MODEL_SAMPLE") == "FIRST_MODEL_SAMPLE") {
                            small["samples"].push_back(sample); break;
                        }
                compact["shots"].push_back(std::move(small));
            }
            const auto& timings = input.contains("timings") ? input.at("timings") : analysis.value("timings", Json::array());
            if (!timings.is_array()) throw std::runtime_error("HUD换键记录无效");
            compact["total_timings"] = input.value("total_timing_count", input.value("total_timings",
                analysis.value("total_timing_count", analysis.value("total_timings", timings.size()))));
            auto full_analysis = analysis;
            full_analysis["timings"] = timings;
            full_analysis["total_timings"] = compact["total_timings"];
            compact["feedback"] = summarize_hud_feedback(full_analysis, settings);
            for (std::size_t i = timings.size() > 32 ? timings.size() - 32 : 0; i < timings.size(); ++i) {
                auto point = timings[i];
                if (!point.contains("ordinal")) point["ordinal"] = i + 1;
                compact["timings"].push_back(std::move(point));
            }
            auto next = std::make_shared<PublishedSnapshot>(PublishedSnapshot{std::move(compact), 0});
            next->version = snapshots.fetch_add(1) + 1;
            if (pending_snapshot.exchange(std::move(next))) ++snapshots_superseded;
        } catch (...) { ++lost; }
    }
    static std::wstring describe(const Json& value) {
        if (!value.value("valid", false)) return L"不可用";
        const auto classification = value.value("classification", "UNAVAILABLE");
        if (classification == "WITHIN_MODEL_THRESHOLD") return L"模型阈值内";
        if (classification == "MICRO") return L"模型微动";
        if (classification == "RUNNING") return L"模型移动中";
        return L"不可用";
    }
    static std::wstring wide(const std::string& value) {
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return L"暂无";
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
        return result;
    }
    static std::wstring number(const Json& value, int precision = 1) {
        if (!value.is_number()) return L"暂无";
        std::wostringstream out; out << std::fixed << std::setprecision(precision) << value.get<double>(); return out.str();
    }
    void refresh() {
        unsigned displayed_version = 0;
        auto index = consumed.load(std::memory_order_relaxed);
        const auto end = written.load(std::memory_order_acquire);
        for (; index < end; ++index) {
            const auto& e = queue[index % capacity];
            if (commands.size() == capacity) { ++lost; continue; }
            commands.push_back({{"kind", e.keyboard ? "wasd" : "left_button"}, {"value", e.value},
                {"disposition", e.disposition}, {"shot_index", e.shot}, {"planned_ns", e.planned},
                {"submit_ns", e.submit}, {"ack_received_ns", e.ack},
                {"backend_completed_ns", e.completed}, {"returned_ns", e.returned}});
        }
        consumed.store(index, std::memory_order_release);
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (const auto ended = finished_at.load(); ended > 0) now = std::min(now, ended);
        if (external) {
            if (auto newest = pending_snapshot.exchange({})) {
                displayed = newest->value;
                const auto recording_id = displayed.value("recording_id", "");
                if (!recording_id.empty()) {
                    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, recording_id.data(),
                        static_cast<int>(recording_id.size()), nullptr, 0);
                    if (count <= 0) throw std::runtime_error("录制ID不是有效UTF-8");
                    std::wstring title(static_cast<std::size_t>(count), L'\0');
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, recording_id.data(),
                        static_cast<int>(recording_id.size()), title.data(), count);
                    title += L" · 输入训练反馈";
                    if (title != window_title) {
                        if (!SetWindowTextW(window, title.c_str())) throw std::runtime_error("录制ID标题更新失败");
                        window_title = std::move(title);
                        ++title_updates;
                    }
                }
                displayed_version = newest->version;
                recording.store(displayed.value("recording", true));
                monitor_source.store(displayed.value("source", "") == "KMBOX_MONITOR");
            }
        } else {
            displayed = analyze_counterpulse_live_sampling(Json{{"commands", commands}}, settings, now);
            displayed["feedback"] = summarize_hud_feedback(displayed, settings);
        }
        const auto& analysis = displayed;
        auto current = analysis.at("current_model");
        if (lost.load()) current["valid"] = false;
        const auto previous_deltas = deltas, previous_ratios = ratios;
        deltas.clear(); ratios.clear();
        const auto& timings = analysis.at("timings");
        for (std::size_t i = timings.size() > 32 ? timings.size() - 32 : 0; i < timings.size(); ++i) {
            const auto& point = timings[i];
            const bool numeric = point.contains("delta_ms") && point["delta_ms"].is_number();
            const double delta = numeric ? point["delta_ms"].get<double>() : 0;
            deltas.push_back({delta, numeric && std::isfinite(delta) && std::abs(delta) <= 120 &&
                !point.value("atomic_ambiguous", false) && !lost.load(), point.value("ordinal", i + 1)});
        }
        const auto& shots = analysis.at("shots");
        for (std::size_t i = shots.size() > 32 ? shots.size() - 32 : 0; i < shots.size(); ++i) {
            PlotPoint point; point.ordinal = shots[i].value("ordinal", i + 1);
            for (const auto& first : shots[i].at("samples")) {
                if (first.value("kind", "FIRST_MODEL_SAMPLE") != "FIRST_MODEL_SAMPLE" ||
                    (!external && first.value("time_ns", now) > now)) continue;
                if (first.value("valid", false) && first.contains("speed_ratio") && first["speed_ratio"].is_number()) {
                    point.value = first["speed_ratio"].get<double>();
                    point.valid = std::isfinite(point.value) && point.value >= 0 &&
                        !shots[i].value("atomic_ambiguous", false) && !first.value("atomic_ambiguous", false) && !lost.load();
                }
                break;
            }
            ratios.push_back(point);
        }
        valid_first_plot_count.store(static_cast<unsigned>(std::count_if(ratios.begin(), ratios.end(), [](const auto& point) { return point.valid; })));
        const auto feedback = analysis.contains("feedback") ? analysis.at("feedback") : summarize_hud_feedback(analysis, settings);
        const auto& timing = feedback.at("timing"); const auto& shooting = feedback.at("shooting");
        const auto& baseline = feedback.at("baseline");
        default_threshold_q = baseline.at("default_threshold_q").get<double>();
        std::wostringstream left, right, bottom, parameters;
        left << L"急停换键反馈  ·  统计近300有效记录 / 折线近32次\n"
            << L"最近 " << number(timing["latest_delta_ms"]) << L" ms · " << wide(timing["latest_grade"].get<std::string>())
            << L"   整体习惯：" << wide(timing["habit"].get<std::string>()) << L"\n"
            << L"平均快慢 " << number(timing["mean_ms"]) << L" ms   波动 σ " << number(timing["stddev_ms"]) << L" ms\n"
            << L"最早 " << number(timing["min_ms"]) << L" / 最晚 " << number(timing["max_ms"]) << L" ms   优秀率 " << number(timing["excellent_percent"]) << L"%\n"
            << L"次数 " << number(timing["count"], 0) << L"   有效 " << number(timing["valid_count"], 0)
            << L"   青≤2ms / 绿≤10ms / 早黄 / 晚红";
        right << L"开枪模型稳定  ·  统计全部有效采样 / 柱图近32首样本\n"
            << L"最近 " << number(shooting["latest_ratio"], 2) << L"x  误差 "
            << number(shooting["latest_error"].is_number() ? Json(shooting["latest_error"].get<double>() * 100) : Json(nullptr), 0) << L"%"
            << L" · " << wide(shooting["latest_classification"].get<std::string>()) << L"\n"
            << L"平均误差 " << number(shooting["mean_error"], 2) << L"   阈值内比例 " << number(shooting["stable_percent"]) << L"%\n"
            << L"按住 " << number(shooting["hold_count"], 0) << L" 次   有效采样 " << number(shooting["valid_count"], 0)
            << L" / " << number(shooting["sample_count"], 0) << L"\n"
            << L"线性 q 柱：绿0–1 / 黄1–1.5 / 超出1.5红";
        const auto parameter_line = [&](const wchar_t* label, const Json& values) {
            parameters << label << L"最大速度 " << number(values["max_move_speed"], 2) << L"  阈比 " << number(values["clean_shot_speed_ratio"], 2)
                << L"  加速 " << number(values["accel_per_sec"]) << L"  自然减速 " << number(values["natural_decel_per_sec"])
                << L"  反制动 " << number(values["counter_strafe_accel_per_sec"])
                << L"  首采样 " << number(values["fire_sample_delay_ms"], 0) << L"ms"
                << L"  短按界 " << number(values["tap_max_hold_ms"], 0) << L"ms"
                << L"  连续采样 " << number(values["auto_fire_interval_ms"], 0) << L"ms\n";
        };
        parameter_line(L"默认基准：", baseline["default_sampling"]);
        parameter_line(L"当前参数：", baseline["active_sampling"]);
        parameters << L"换键参考0ms / 完美±2ms / 优秀±10ms；";
        if (std::abs(default_threshold_q - 1) < 1e-9) parameters << L"当前与默认阈比参考线重合 q=1";
        else parameters << L"当前阈线 q=1，默认阈比参考线 q=" << std::setprecision(3) << default_threshold_q;
        parameters << L"。未按默认参数重跑模型。";
        bottom << (monitor_source.load() ? L"KMBOX_MONITOR 接收域" : L"COMMAND_ACK_PROXY 回执代理")
            << L"  |  " << (ending.load() ? L"结束保留，可关闭窗口" : L"实时反馈")
            << L"  |  当前：" << describe(current);
        if (current.value("valid", false) && current["speed_ratio"].is_number()) bottom << L" q=" << std::setprecision(2) << current["speed_ratio"].get<double>();
        if (lost.load()) bottom << L"  |  显示缺口 " << lost.load();
        bottom << L"\n没有实测游戏速度；q 为模型速度/模型阈值，样本不等于子弹，图形不另设评分标准。";
        const bool changed = left_title != left.str() || right_title != right.str() || footer != bottom.str() || parameter_text != parameters.str() ||
            previous_deltas != deltas || previous_ratios != ratios;
        left_title = left.str(); right_title = right.str(); footer = bottom.str();
        parameter_text = parameters.str();
        if (stop_button) {
            const bool enabled = recording.load() && !stop_recording.load();
            if ((IsWindowEnabled(stop_button) != FALSE) != enabled) EnableWindow(stop_button, enabled);
        }
        if (displayed_version) snapshots_displayed.store(displayed_version);
        if (ending.load()) state.store(2);
        if (changed) InvalidateRect(window, nullptr, FALSE);
    }
    void paint(HWND hwnd, HDC dc) const noexcept {
        RECT rect{}; GetClientRect(hwnd, &rect);
        const auto brush = CreateSolidBrush(RGB(20, 25, 35)); FillRect(dc, &rect, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(230, 240, 245));
        const auto previous = SelectObject(dc, font);
        RECT left{14, 8, 540, 110}, right{562, 8, 1090, 110}, bottom{14, 377, 1090, 428};
        RECT parameters{14, 310, 1090, 372};
        DrawTextW(dc, left_title.c_str(), -1, &left, DT_LEFT | DT_TOP | DT_NOPREFIX);
        DrawTextW(dc, right_title.c_str(), -1, &right, DT_LEFT | DT_TOP | DT_NOPREFIX);
        DrawTextW(dc, footer.c_str(), -1, &bottom, DT_LEFT | DT_TOP | DT_NOPREFIX);
        DrawTextW(dc, parameter_text.c_str(), -1, &parameters, DT_LEFT | DT_TOP | DT_NOPREFIX);
        const auto plot = [&](const std::vector<PlotPoint>& points, int x, bool bars) {
            constexpr int width = 512, top = 143, height = 132;
            double maximum = bars ? std::max(1.8, default_threshold_q) : 100.0;
            if (bars) {
                for (const auto& point : points) if (point.valid) maximum = std::max(maximum, std::abs(point.value));
                maximum *= 1.1;
            }
            const auto y = [&](double value) { return bars ? top + height - static_cast<int>(value / maximum * height) :
                top + height / 2 - static_cast<int>(std::clamp(value, -100.0, 100.0) / maximum * (height / 2)); };
            const auto grid = CreatePen(PS_SOLID, 1, RGB(74, 82, 99)); const auto saved_pen = SelectObject(dc, grid);
            MoveToEx(dc, x, y(bars ? 1.0 : 0.0), nullptr); LineTo(dc, x + width, y(bars ? 1.0 : 0.0));
            SelectObject(dc, saved_pen); DeleteObject(grid);
            if (bars && std::abs(default_threshold_q - 1) >= 1e-9) {
                const auto line = CreatePen(PS_DOT, 1, RGB(109, 183, 244)); const auto old = SelectObject(dc, line);
                MoveToEx(dc, x, y(default_threshold_q), nullptr); LineTo(dc, x + width, y(default_threshold_q));
                SelectObject(dc, old); DeleteObject(line);
            }
            bool linked = false; POINT previous_point{};
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& point = points[i]; const int px = x + (points.size() == 1 ? width / 2 :
                    static_cast<int>(i * (width - 10) / (points.size() - 1)) + 5);
                const double rounded = std::round(point.value * 10) / 10;
                const COLORREF color = !point.valid ? RGB(118, 124, 138) : !bars ?
                    (std::abs(rounded) <= 2 ? RGB(94, 234, 212) : std::abs(rounded) <= 10 ? RGB(74, 222, 128) :
                        rounded < 0 ? RGB(251, 191, 36) : RGB(248, 113, 113)) : RGB(74, 222, 128);
                const auto pen = CreatePen(PS_SOLID, 2, color); const auto old_pen = SelectObject(dc, pen);
                if (bars && point.valid) {
                    const auto segment = [&](double low, double high, COLORREF fill_color) {
                        if (high < low || (high == low && high != 0)) return;
                        RECT bar{px - 5, y(high), px + 5, y(low)};
                        if (bar.top == bar.bottom) --bar.top;
                        const auto fill = CreateSolidBrush(fill_color); FillRect(dc, &bar, fill); DeleteObject(fill);
                    };
                    segment(0, std::min(point.value, 1.0), RGB(74, 222, 128));
                    if (point.value > 1) segment(1, std::min(point.value, 1.5), RGB(251, 191, 36));
                    if (point.value > 1.5) segment(1.5, point.value, RGB(248, 113, 113));
                } else if (point.valid) {
                    if (linked) { MoveToEx(dc, previous_point.x, previous_point.y, nullptr); LineTo(dc, px, y(point.value)); }
                    Ellipse(dc, px - 2, y(point.value) - 2, px + 3, y(point.value) + 3);
                    previous_point = {px, y(point.value)}; linked = true;
                } else {
                    linked = false; MoveToEx(dc, px - 3, top + height - 6, nullptr); LineTo(dc, px + 3, top + height);
                    MoveToEx(dc, px + 3, top + height - 6, nullptr); LineTo(dc, px - 3, top + height);
                }
                SelectObject(dc, old_pen); DeleteObject(pen);
                if (i == 0 || i + 1 == points.size() || (i + 1) % 8 == 0) {
                    wchar_t label[24]{}; swprintf_s(label, L"%llu", static_cast<unsigned long long>(point.ordinal));
                    TextOutW(dc, px - 8, top + height + 7, label, static_cast<int>(wcslen(label)));
                }
            }
            if (points.empty()) TextOutW(dc, x + 135, top + 50, L"等待有效样本", 6);
            const wchar_t* legend = bars ? (std::abs(default_threshold_q - 1) < 1e-9 ? L"当前=默认阈比参考 q=1" : L"实线 当前q=1 / 蓝虚线 默认阈比") : L"上+100ms偏晚 / 下−100ms偏早";
            TextOutW(dc, x + 190, top - 22, legend, static_cast<int>(wcslen(legend)));
        };
        plot(deltas, 18, false); plot(ratios, 568, true);
        SelectObject(dc, previous);
    }
    void present(HWND hwnd, HDC target) noexcept {
        RECT bounds{}; GetClientRect(hwnd, &bounds);
        const auto memory = CreateCompatibleDC(target);
        const auto bitmap = CreateCompatibleBitmap(target, bounds.right, bounds.bottom);
        if (!memory || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            state.store(3); closing.store(true); return;
        }
        const auto previous = SelectObject(memory, bitmap);
        paint(hwnd, memory);
        // 完整帧在内存DC绘制后一次呈现，窗口DC不再暴露背景擦除中间态。
        if (!BitBlt(target, 0, 0, bounds.right, bounds.bottom, memory, 0, 0, SRCCOPY)) {
            state.store(3); closing.store(true);
        }
        SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (message == WM_COMMAND && LOWORD(wparam) == 1001 && HIWORD(wparam) == BN_CLICKED) {
            self->stop_recording.store(true);
            if (self->stop_button) EnableWindow(self->stop_button, FALSE);
            return 0;
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PRINTCLIENT) { self->present(hwnd, reinterpret_cast<HDC>(wparam)); return 0; }
        if (message == WM_PAINT) {
            ++self->paint_count;
            PAINTSTRUCT paint{}; const auto dc = BeginPaint(hwnd, &paint);
            self->present(hwnd, dc); EndPaint(hwnd, &paint); return 0;
        }
        if (message == WM_CLOSE) { self->closing.store(true); return 0; }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void run() noexcept {
        const auto instance = GetModuleHandleW(nullptr);
        try {
            WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = instance;
            type.lpszClassName = L"XenCounterpulseReadOnlyHud";
            if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                throw std::runtime_error("HUD窗口类不可用");
            font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            if (!font) throw std::runtime_error("HUD字体不可用");
            constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
            constexpr DWORD extended = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
            RECT size{0, 0, 1100, 430};
            if (!AdjustWindowRectEx(&size, style, FALSE, extended)) throw std::runtime_error("HUD尺寸不可用");
            window = CreateWindowExW(extended, type.lpszClassName, L"Xen 输入训练反馈 · 关闭窗口结束展示", style,
                20, 20, size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, this);
            if (!window) throw std::runtime_error("HUD窗口不可用");
            if (external) {
                stop_button = CreateWindowExW(WS_EX_NOACTIVATE, L"BUTTON", L"停止录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    982, 402, 104, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), instance, nullptr);
                if (!stop_button) throw std::runtime_error("HUD停止按钮不可用");
                SendMessageW(stop_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            }
            if (!SetLayeredWindowAttributes(window, 0, 240, LWA_ALPHA) ||
                !SetWindowPos(window, HWND_TOPMOST, 20, 20, size.right - size.left, size.bottom - size.top, SWP_NOACTIVATE | SWP_SHOWWINDOW))
                throw std::runtime_error("HUD显示不可用");
            state.store(1);
            while (!closing.load()) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                refresh();
                MsgWaitForMultipleObjectsEx(0, nullptr, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        } catch (...) { state.store(3); }
        if (window) DestroyWindow(window);
        if (font) DeleteObject(font);
        window_closed.store(true);
    }
};
CounterpulseHud::CounterpulseHud(const sampling_detail::SamplingSettings& settings, bool external_snapshots)
    : impl_(std::make_unique<Impl>(settings, external_snapshots)) {}
CounterpulseHud::~CounterpulseHud() = default;
void CounterpulseHud::observe(const Json& command) noexcept { impl_->enqueue(command); }
void CounterpulseHud::publish(const Json& snapshot) noexcept { impl_->publish_snapshot(snapshot); }
bool CounterpulseHud::closed() const noexcept { return impl_->window_closed.load(); }
bool CounterpulseHud::stop_requested() const noexcept { return impl_->stop_recording.load(); }
void CounterpulseHud::finish(const Json& report) noexcept {
    try { impl_->execution_success.store(report.value("success", false)); }
    catch (...) { impl_->execution_success.store(false); }
    impl_->finished_at.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    impl_->ending.store(true);
}
Json CounterpulseHud::status() const {
    const auto value = impl_->state.load();
    const auto consumed = impl_->consumed.load();
    const auto written = impl_->written.load();
    return {{"state", value == 0 ? "STARTING" : value == 1 ? "VISIBLE" : value == 2 ? "FINISHED_VISIBLE" : "FAILED"},
        {"success", value != 0 && value != 3 && impl_->lost.load() == 0}, {"dropped_commands", impl_->lost.load()},
        {"queued_commands", written - consumed}, {"source", impl_->monitor_source.load() ? "KMBOX_MONITOR" : "COMMAND_ACK_PROXY"},
        {"external_snapshots", impl_->external}, {"snapshots_received", impl_->snapshots.load()},
        {"snapshots_displayed", impl_->snapshots_displayed.load()}, {"snapshots_superseded", impl_->snapshots_superseded.load()},
        {"closed", impl_->window_closed.load()},
        {"recording", impl_->external && impl_->recording.load()}, {"stop_requested", impl_->stop_recording.load()},
        {"paint_count", impl_->paint_count.load()}, {"title_updates", impl_->title_updates.load()},
        {"valid_first_plot_count", impl_->valid_first_plot_count.load()},
        {"physical_validation_passed", false}, {"actual_game_speed", nullptr}};
}
}
