#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "counterpulse_hud.h"
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
    struct PlotPoint { double value = 0; bool valid = false; std::uint64_t ordinal = 0; };
    std::vector<PlotPoint> deltas, ratios;
    std::wstring left_title, right_title, footer;
    sampling_detail::SamplingSettings settings;
    std::jthread worker;
    HWND window = nullptr;
    HWND stop_button = nullptr;
    HFONT font = nullptr;
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
                Json small{{"ordinal", shot.value("ordinal", i + 1)},
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
            compact["total_timings"] = input.value("total_timings", timings.size());
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
                    if (!SetWindowTextW(window, title.c_str())) throw std::runtime_error("录制ID标题更新失败");
                }
                displayed_version = newest->version;
                recording.store(displayed.value("recording", true));
                monitor_source.store(displayed.value("source", "") == "KMBOX_MONITOR");
            }
        } else {
            displayed = analyze_counterpulse_live_sampling(Json{{"commands", commands}}, settings, now);
            displayed["timings"] = Json::array();
            // 自动组仅显示已确认的同轴释放→对向按下ACK间隔。
            int previous_direction = 0;
            std::int64_t released = 0;
            for (const auto& item : commands) {
                if (item["kind"] != "wasd" || item["disposition"] != static_cast<int>(KeyboardDisposition::ACKNOWLEDGED)) continue;
                const int value = item["value"];
                const auto ack = item["ack_received_ns"].get<std::int64_t>();
                if (!value && previous_direction) released = ack;
                else if (value == 2 || value == 8) {
                    if (released && previous_direction && value != previous_direction)
                        displayed["timings"].push_back({{"delta_ms", (ack - released) / 1e6},
                            {"ordinal", displayed["timings"].size() + 1}, {"grade", "ACK_INTERVAL"}});
                    previous_direction = value; released = 0;
                }
            }
        }
        const auto& analysis = displayed;
        auto current = analysis.at("current_model");
        if (lost.load()) current["valid"] = false;
        deltas.clear(); ratios.clear();
        const auto& timings = analysis.at("timings");
        for (std::size_t i = timings.size() > 32 ? timings.size() - 32 : 0; i < timings.size(); ++i) {
            const auto& point = timings[i];
            const double delta = point.value("delta_ms", 0.0);
            deltas.push_back({delta, point.contains("delta_ms") && std::isfinite(delta) && !lost.load(), point.value("ordinal", i + 1)});
        }
        const auto& shots = analysis.at("shots");
        for (std::size_t i = shots.size() > 32 ? shots.size() - 32 : 0; i < shots.size(); ++i) {
            PlotPoint point; point.ordinal = shots[i].value("ordinal", i + 1);
            for (const auto& first : shots[i].at("samples")) {
                if (first.value("kind", "FIRST_MODEL_SAMPLE") != "FIRST_MODEL_SAMPLE" ||
                    first.value("time_ns", now) > now) continue;
                if (first.value("valid", false) && first.contains("speed_ratio") && first["speed_ratio"].is_number()) {
                    point.value = first["speed_ratio"].get<double>();
                    point.valid = std::isfinite(point.value) && point.value >= 0 && !lost.load();
                }
                break;
            }
            ratios.push_back(point);
        }
        double mean = 0, variance = 0; unsigned valid_deltas = 0, valid_ratios = 0, stable = 0;
        for (const auto& point : deltas) if (point.valid) { mean += point.value; ++valid_deltas; }
        if (valid_deltas) mean /= valid_deltas;
        for (const auto& point : deltas) if (point.valid) variance += (point.value - mean) * (point.value - mean);
        for (const auto& point : ratios) if (point.valid) { ++valid_ratios; if (point.value <= 1) ++stable; }
        std::wostringstream left, right, bottom;
        left << L"换键时间趋势  ·  最近32次\n总次数 " << analysis.value("total_timings", timings.size());
        if (valid_deltas) left << std::fixed << std::setprecision(1) << L"  均值 " << mean << L" ms  σ " << std::sqrt(variance / valid_deltas) << L" ms";
        else left << L"  均值 / σ：暂无有效记录";
        right << L"开枪模型稳定性  ·  最近32次首样本\n按住 " << analysis.value("shot_count", shots.size()) << L" 次";
        if (valid_ratios) right << std::fixed << std::setprecision(0) << L"  阈值内 " << 100.0 * stable / valid_ratios << L"%";
        else right << L"  阈值内比例：暂无";
        right << std::fixed << std::setprecision(2) << L"  阈值 " << settings.max_move_speed * settings.clean_shot_speed_ratio;
        bottom << (monitor_source.load() ? L"KMBOX_MONITOR 接收域" : L"COMMAND_ACK_PROXY 回执代理")
            << L"  |  " << (ending.load() ? L"结束保留，可关闭窗口" : L"实时反馈")
            << L"  |  当前：" << describe(current);
        if (current.value("valid", false) && current["speed_ratio"].is_number()) bottom << L" q=" << std::setprecision(2) << current["speed_ratio"].get<double>();
        if (lost.load()) bottom << L"  |  显示缺口 " << lost.load();
        bottom << L"\n没有实测游戏速度；q 为模型速度/模型阈值，样本不等于子弹，图形不另设评分标准。";
        left_title = left.str(); right_title = right.str(); footer = bottom.str();
        if (stop_button) EnableWindow(stop_button, recording.load() && !stop_recording.load());
        if (displayed_version) snapshots_displayed.store(displayed_version);
        if (ending.load()) state.store(2);
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint(HWND hwnd, HDC dc) const noexcept {
        RECT rect{}; GetClientRect(hwnd, &rect);
        const auto brush = CreateSolidBrush(RGB(20, 25, 35)); FillRect(dc, &rect, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(230, 240, 245));
        const auto previous = SelectObject(dc, font);
        RECT left{14, 8, 439, 54}, right{460, 8, 890, 54}, bottom{14, 231, 890, 278};
        DrawTextW(dc, left_title.c_str(), -1, &left, DT_LEFT | DT_TOP | DT_NOPREFIX);
        DrawTextW(dc, right_title.c_str(), -1, &right, DT_LEFT | DT_TOP | DT_NOPREFIX);
        DrawTextW(dc, footer.c_str(), -1, &bottom, DT_LEFT | DT_TOP | DT_NOPREFIX);
        const auto plot = [&](const std::vector<PlotPoint>& points, int x, bool bars) {
            constexpr int width = 412, top = 65, height = 133;
            double maximum = bars ? 1.5 : 10.0;
            for (const auto& point : points) if (point.valid) maximum = std::max(maximum, std::abs(point.value));
            maximum *= 1.1;
            const auto y = [&](double value) { return bars ? top + height - static_cast<int>(value / maximum * height) :
                top + height / 2 - static_cast<int>(value / maximum * (height / 2)); };
            const auto grid = CreatePen(PS_SOLID, 1, RGB(74, 82, 99)); const auto saved_pen = SelectObject(dc, grid);
            MoveToEx(dc, x, y(bars ? 1.0 : 0.0), nullptr); LineTo(dc, x + width, y(bars ? 1.0 : 0.0));
            SelectObject(dc, saved_pen); DeleteObject(grid);
            bool linked = false; POINT previous_point{};
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& point = points[i]; const int px = x + static_cast<int>((i + 0.5) * width / 32);
                const COLORREF color = !point.valid ? RGB(118, 124, 138) : !bars ? RGB(109, 183, 244) :
                    point.value > 1 ? RGB(244, 158, 71) : RGB(80, 207, 163);
                const auto pen = CreatePen(PS_SOLID, 2, color); const auto old_pen = SelectObject(dc, pen);
                if (bars && point.valid) {
                    RECT bar{px - 4, y(point.value), px + 4, top + height};
                    if (bar.top == bar.bottom) --bar.top;
                    const auto fill = CreateSolidBrush(color); FillRect(dc, &bar, fill); DeleteObject(fill);
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
            const wchar_t* legend = bars ? L"q=1 模型阈值" : L"0 ms 换键间隔";
            TextOutW(dc, x + 270, top + 2, legend, static_cast<int>(wcslen(legend)));
        };
        plot(deltas, 18, false); plot(ratios, 466, true);
        SelectObject(dc, previous);
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
        if (message == WM_PRINTCLIENT) { self->paint(hwnd, reinterpret_cast<HDC>(wparam)); return 0; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; const auto dc = BeginPaint(hwnd, &paint);
            self->paint(hwnd, dc); EndPaint(hwnd, &paint); return 0;
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
            constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
            constexpr DWORD extended = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
            RECT size{0, 0, 900, 280};
            if (!AdjustWindowRectEx(&size, style, FALSE, extended)) throw std::runtime_error("HUD尺寸不可用");
            window = CreateWindowExW(extended, type.lpszClassName, L"Xen 输入训练反馈 · 关闭窗口结束展示", style,
                20, 20, size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, this);
            if (!window) throw std::runtime_error("HUD窗口不可用");
            if (external) {
                stop_button = CreateWindowExW(WS_EX_NOACTIVATE, L"BUTTON", L"停止录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    782, 250, 104, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), instance, nullptr);
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
        {"physical_validation_passed", false}, {"actual_game_speed", nullptr}};
}
}
