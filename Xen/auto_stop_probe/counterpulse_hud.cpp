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
    sampling_detail::SamplingSettings settings;
    std::jthread worker;
    HWND window = nullptr;
    HFONT font = nullptr;
    std::wstring text = L"输入模型 HUD\n等待命令回执…\nACK 代理；没有实测游戏速度";
    Json commands = Json::array();

    explicit Impl(const sampling_detail::SamplingSettings& value) : settings(value) {
        worker = std::jthread([this] { run(); });
    }
    ~Impl() { closing.store(true); if (worker.joinable()) worker.join(); }

    void enqueue(const Json& command) noexcept {
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
    static std::wstring describe(const Json& value) {
        if (!value.value("valid", false)) return L"不可用";
        const auto classification = value.value("classification", "UNAVAILABLE");
        if (classification == "WITHIN_MODEL_THRESHOLD") return L"模型阈值内";
        if (classification == "MICRO") return L"模型微动";
        if (classification == "RUNNING") return L"模型移动中";
        return L"不可用";
    }
    void refresh() {
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
        const auto analysis = analyze_counterpulse_live_sampling(Json{{"commands", commands}}, settings, now);
        auto current = analysis.at("current_model");
        Json down, sample;
        std::size_t due_samples = 0;
        for (const auto& shot : analysis.at("shots")) {
            down = shot.at("down_model");
            for (const auto& item : shot.at("samples")) {
                if (item.at("time_ns").get<std::int64_t>() > now) continue;
                // 到期与有效性都由共享分析器决定，不能绕过重复DOWN或无效回执。
                sample = item;
                ++due_samples;
            }
        }
        if (lost.load()) {
            current["valid"] = false;
            if (!down.is_null()) down["valid"] = false;
            if (!sample.is_null()) sample["valid"] = false;
        }
        std::wostringstream lines;
        lines << L"输入模型 HUD  ·  ACK 代理\n";
        if (lost.load()) lines << L"显示证据有缺口，丢失 " << lost.load() << L" 条；判定不可用\n";
        else if (ending.load()) lines << (execution_success.load() ? L"执行结束；保留最近判定\n" : L"执行未完成；保留最近判定\n");
        else lines << L"运行中  ·  回执 " << commands.size() << L" 条\n";
        lines << std::fixed << std::setprecision(3);
        if (current.value("valid", false))
            lines << L"模型速度 " << current["estimated_speed"].get<double>() << L" / 阈值 "
                << settings.max_move_speed * settings.clean_shot_speed_ratio << L"\n阈值比例 "
                << current["speed_ratio"].get<double>() << L"  ·  " << describe(current) << L"\n";
        else lines << L"模型速度 / 阈值比例：不可用\n";
        lines << L"最近按下：" << (down.is_null() ? L"等待" : describe(down)) << L"\n";
        lines << L"最近到期采样：" << (sample.is_null() ? L"等待" : describe(sample)) << L"\n";
        lines << L"开火按住 " << analysis.at("shots").size() << L" 次 · 已到期样本 " << due_samples << L"\n";
        if (!commands.empty()) {
            const auto& last = commands.back();
            lines << L"阶段：" << (last["kind"] == "left_button" ?
                (last["value"] == 1 ? L"开火保持" : L"射后等待 / 移动前") :
                (last["value"] == 0 ? L"方向释放 / 等待" : L"方向保持")) << L"\n";
        }
        lines << L"没有实测游戏速度；不代表停稳或子弹命中";
        text = lines.str();
        if (ending.load()) state.store(2);
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint(HWND hwnd, HDC dc) const noexcept {
        RECT rect{}; GetClientRect(hwnd, &rect);
        const auto brush = CreateSolidBrush(RGB(20, 25, 35)); FillRect(dc, &rect, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(230, 240, 245));
        const auto previous = SelectObject(dc, font);
        rect.left += 16; rect.top += 12; rect.right -= 12;
        DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
        SelectObject(dc, previous);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
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
            font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            if (!font) throw std::runtime_error("HUD字体不可用");
            window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
                WS_EX_LAYERED, type.lpszClassName, L"Xen 输入模型 HUD", WS_POPUP,
                20, 20, 520, 360, nullptr, nullptr, instance, this);
            if (!window) throw std::runtime_error("HUD窗口不可用");
            if (!SetLayeredWindowAttributes(window, 0, 240, LWA_ALPHA) ||
                !SetWindowPos(window, HWND_TOPMOST, 20, 20, 520, 360, SWP_NOACTIVATE | SWP_SHOWWINDOW))
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
    }
};
CounterpulseHud::CounterpulseHud(const sampling_detail::SamplingSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}
CounterpulseHud::~CounterpulseHud() = default;
void CounterpulseHud::observe(const Json& command) noexcept { impl_->enqueue(command); }
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
        {"queued_commands", written - consumed}, {"source", "COMMAND_ACK_PROXY"},
        {"physical_validation_passed", false}, {"actual_game_speed", nullptr}};
}
}
