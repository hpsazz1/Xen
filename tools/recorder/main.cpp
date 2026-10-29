// SPDX-License-Identifier: GPL-2.0-or-later
#include "recorder.h"
#include "sync_log.h"
#include "diagnostics.h"
#include "default_obs_root.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace xen::recorder;
std::wstring wide(const std::string& text) {
    const auto count = MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);
    return result;
}
std::string narrow(const std::wstring& text) {
    const auto count = WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);
    return result;
}
struct Options {
    Config config;
    std::string auxiliary_url = "http://192.168.3.20:5015";
    double duration = 0;
    bool automatic = false;
};
Options arguments() {
    Options options; options.config.obs_root = XEN_DEFAULT_OBS_ROOT;
    wchar_t videos[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_MYVIDEO,nullptr,SHGFP_TYPE_CURRENT,videos)))
        options.config.directory = std::filesystem::path(videos)/L"XenRecorder";
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(),&count);
    if (!args) throw std::runtime_error("命令行读取失败");
    try {
        for (int i = 1; i < count; ++i) {
            const std::wstring arg(args[i]);
            auto value = [&]() -> std::wstring { if (++i >= count) throw std::runtime_error("命令行选项缺少值"); return args[i]; };
            if (arg == L"--obs-root") options.config.obs_root = value();
            else if (arg == L"--output") options.config.directory = value();
            else if (arg == L"--fps") options.config.fps = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == L"--bitrate") options.config.bitrate_kbps = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == L"--width") options.config.width = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == L"--height") options.config.height = static_cast<std::uint32_t>(std::stoul(value()));
            else if (arg == L"--duration") { options.duration = std::stod(value()); options.automatic = true; }
            else if (arg == L"--aux-url") { options.auxiliary_url = narrow(value()); if (options.auxiliary_url == "none") options.auxiliary_url.clear(); }
            else if (arg == L"--no-audio") options.config.audio = false;
            else if (arg == L"--audio") options.config.audio = true;
            else if (arg == L"--synthetic") options.config.synthetic = true;
            else if (arg == L"--allow-software-encoder") options.config.allow_software_encoder = true;
            else throw std::runtime_error("未知命令行选项："+narrow(arg));
        }
        if (options.automatic && (options.duration < 6 || options.duration > 43200))
            throw std::runtime_error("自动录制时长应为6～43200秒");
        if (options.config.directory.empty()) throw std::runtime_error("请指定录像目录");
    } catch (...) { LocalFree(args); throw; }
    LocalFree(args); return options;
}
void prepare_runtime(const Config& config) {
    const auto bin = config.obs_root/L"bin"/L"64bit";
    if (!std::filesystem::exists(bin/L"obs.dll")) throw std::runtime_error("找不到指定OBS的obs.dll");
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS) ||
        !AddDllDirectory(bin.c_str())) throw std::runtime_error("OBS运行库目录配置失败");
    if (!LoadLibraryExW((bin/L"obs.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        throw std::runtime_error("OBS运行库或其依赖无法加载");
    // 官方编码模块的辅助进程继承此进程PATH；不修改系统或用户环境变量。
    const auto needed = GetEnvironmentVariableW(L"PATH",nullptr,0);
    std::wstring previous(needed,L'\0');
    if (needed) { GetEnvironmentVariableW(L"PATH",previous.data(),needed); previous.resize(needed-1); }
    const auto path = bin.wstring()+L";"+previous;
    if (!SetEnvironmentVariableW(L"PATH",path.c_str())) throw std::runtime_error("编码辅助进程环境配置失败");
}
class Application {
public:
    explicit Application(Options options) : options_(std::move(options)) {}
    ~Application() { if (session_open_) finish(); }
    bool start() {
        std::string error;
        if (!recorder_.start(options_.config,error)) { message_ = error; return false; }
        session_ = "rec-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(system_ns()/1000000);
        try { log_ = std::make_unique<SyncLog>(recorder_.snapshot().video_path,session_,options_.auxiliary_url); }
        catch (const std::exception& e) { recorder_.stop(); message_ = e.what(); return false; }
        started_ = steady_ns(); first_ = false; stopping_ = false; close_when_stopped_ = false; session_open_ = true; failed_ = false;
        message_ = "录制中，首尾自动写入SYNC；无需按Ctrl+F7"; return true;
    }
    bool mark() {
        const auto id = "SYNC-"+session_+"-"+std::to_string(++serial_);
        if (!recorder_.mark(id)) { message_ = "标记仍在显示，稍后再补标记"; return false; }
        message_ = "已请求烧入 "+id; return true;
    }
    void request_stop(bool close = false) {
        close_when_stopped_ = close_when_stopped_ || close;
        if (stopping_) return;
        stopping_ = true; stop_requested_ = steady_ns(); tail_marked_ = false;
        message_ = "正在写入尾部SYNC，约2秒后完成保存";
    }
    void tick() {
        for (auto& marker : recorder_.drain_markers()) if (log_) log_->submit(std::move(marker));
        const auto snapshot = recorder_.snapshot();
        if (!snapshot.running) { if (session_open_) finish(); return; }
        const auto now = steady_ns();
        if (!first_ && now-started_ >= 800000000LL) { first_ = mark(); }
        if (options_.automatic && !stopping_ && now-started_ >= static_cast<std::int64_t>((options_.duration-2.3)*1e9))
            request_stop();
        if (stopping_) {
            // 当前标记未消失时等其结束，尾标记必须另有独立编号。
            if (first_ && !tail_marked_ && now-stop_requested_ < 5000000000LL) {
                if (mark()) { tail_marked_ = true; tail_at_ = now; }
            }
            if ((tail_marked_ && now-tail_at_ >= 2200000000LL) || now-stop_requested_ >= 8000000000LL)
                finish();
        }
    }
    void finish() noexcept {
        if (!session_open_) return;
        session_open_ = false;
        recorder_.stop();
        try {
            if (log_) {
                for (auto& marker : recorder_.drain_markers()) log_->submit(std::move(marker));
                log_->finish(recorder_.snapshot());
            }
            const auto s = recorder_.snapshot();
            const bool index_complete = log_ && log_->local_complete() && s.markers_matched>=2 &&
                !s.markers_dropped && !s.markers_pending && !s.render_queue_gaps && !s.packet_queue_gaps;
            message_ = !s.stop_completed || !s.last_error.empty() ? "录制结束存在错误："+s.last_error :
                index_complete ? "已保存录像和同步索引" : "录像已保存；同步索引存在缺口，请查看诊断日志";
        } catch (const std::exception& e) { message_ = e.what(); failed_ = true; }
        stopping_ = false;
    }
    std::wstring display() const {
        const auto s = recorder_.snapshot();
        std::wostringstream out;
        out << (s.running ? L"录制中 " : L"已停止 ") << s.width << L"×" << s.height << L" / " << s.fps << L" FPS  " << wide(s.encoder_id)
            << L"\r\n" << wide(message_);
        if (log_) out << L"\r\n" << wide(log_->status());
        out << L"\r\n已编码帧：" << s.encoded_packets << L"  标记匹配：" << s.markers_matched << L"/" << s.markers_requested
            << L"  队列缺口：" << s.render_queue_gaps+s.packet_queue_gaps
            << L"  渲染/编码掉帧：" << s.render_lagged_frames << L"/" << s.encoder_skipped_frames
            << L"\r\n" << (s.video_path.empty() ? options_.config.directory.wstring() : s.video_path.wstring());
        return out.str();
    }
    bool running() const { return recorder_.snapshot().running; }
    bool close_ready() const { return close_when_stopped_ && !running(); }
    int result() const {
        const auto s = recorder_.snapshot();
        return failed_ || (log_ && !log_->local_complete()) || !s.stop_completed || !s.last_error.empty() || s.markers_matched < 2 ||
            s.markers_dropped || s.markers_pending || s.packet_queue_gaps || s.render_queue_gaps ? 1 : 0;
    }
    const Config& config() const { return options_.config; }
    bool automatic() const { return options_.automatic; }
private:
    Options options_;
    Recorder recorder_;
    std::unique_ptr<SyncLog> log_;
    std::string session_,message_ = "开启辅机详细记录和Runtime，再开始录制。首尾自动SYNC直接进入视频。";
    std::uint64_t serial_ = 0;
    std::int64_t started_ = 0,stop_requested_ = 0,tail_at_ = 0;
    bool first_ = false,stopping_ = false,tail_marked_ = false,close_when_stopped_ = false,failed_ = false,session_open_ = false;
};
// GUI只发布命令和读取不可变快照；libobs从初始化至关闭由同一工作线程持有。
class UiController {
public:
    enum class Command { Start, Mark, Stop, Close };
    struct View { std::wstring text; bool running = false, closed = false; };
    explicit UiController(Options options) : directory_(options.config.directory),
        thread_([this,options=std::move(options)]() mutable { run(std::move(options)); }) {}
    ~UiController() { send(Command::Close); if (thread_.joinable()) thread_.join(); }
    void send(Command command) {
        std::lock_guard lock(mutex_);
        if (command == Command::Close) close_requested_ = true;
        else if (!close_requested_ && commands_.size()<16) commands_.push_back(command);
        condition_.notify_one();
    }
    View view() const { std::lock_guard lock(mutex_); return view_; }
    const std::filesystem::path& directory() const { return directory_; }
private:
    void run(Options options) {
        try {
            Application app(std::move(options)); bool closing = false;
            for (;;) {
                std::deque<Command> commands;
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait_for(lock,std::chrono::milliseconds(20),[&]{return (!closing && close_requested_) || !commands_.empty();});
                    commands.swap(commands_);
                    if (close_requested_ && !closing) { commands.push_back(Command::Close); closing = true; }
                }
                for (const auto command : commands) {
                    if (command == Command::Start && !app.running() && !closing) app.start();
                    else if (command == Command::Mark && app.running()) app.mark();
                    else if (command == Command::Stop && app.running()) app.request_stop();
                    else if (command == Command::Close && app.running()) app.request_stop(true);
                }
                // 先让窗口读到“正在保存”，有界封尾等待不会阻塞Windows消息泵。
                {
                    std::lock_guard lock(mutex_); view_ = {app.display(),app.running(),false};
                }
                app.tick();
                {
                    std::lock_guard lock(mutex_); view_ = {app.display(),app.running(),closing && !app.running()};
                    if (view_.closed) return;
                }
            }
        } catch (const std::exception& error) {
            std::unique_lock lock(mutex_); view_ = {wide(error.what()),false,false};
            condition_.wait(lock,[&]{return close_requested_;}); view_.closed = true;
        }
    }
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> commands_;
    View view_;
    bool close_requested_ = false;
    std::thread thread_;
};
UiController* controller = nullptr;
HWND status_window = nullptr;
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto control = [&](const wchar_t* kind,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
            auto child = CreateWindowW(kind,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE); return child;
        };
        control(L"STATIC",L"Xen 独立录制 · 全画面 · 默认240 FPS · 不采集系统声音",0,18,18,690,24,0);
        control(L"STATIC",L"开始后返回游戏即可；停止时自动补尾标记。详细记录需在辅机开启。",0,18,48,690,30,0);
        control(L"BUTTON",L"开始录制",BS_PUSHBUTTON,18,88,140,36,1);
        control(L"BUTTON",L"补一个SYNC",BS_PUSHBUTTON,172,88,140,36,2);
        control(L"BUTTON",L"停止并保存",BS_PUSHBUTTON,326,88,140,36,3);
        control(L"BUTTON",L"打开录像目录",BS_PUSHBUTTON,480,88,160,36,4);
        status_window = control(L"STATIC",L"",SS_LEFT,18,144,690,154,0);
        SetTimer(window,1,50,nullptr); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case 1: controller->send(UiController::Command::Start); break;
        case 2: controller->send(UiController::Command::Mark); break;
        case 3: controller->send(UiController::Command::Stop); break;
        case 4: ShellExecuteW(window,L"open",controller->directory().c_str(),nullptr,nullptr,SW_SHOWNORMAL); break;
        }
        return 0;
    case WM_TIMER: {
        const auto view = controller->view(); SetWindowTextW(status_window,view.text.c_str());
        EnableWindow(GetDlgItem(window,1),!view.running);
        EnableWindow(GetDlgItem(window,2),view.running);
        EnableWindow(GetDlgItem(window,3),view.running);
        if (view.closed) DestroyWindow(window); return 0;
    }
    case WM_CLOSE:
        controller->send(UiController::Command::Close); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    try {
        auto options = arguments(); prepare_runtime(options.config);
        const auto output = options.config.directory;
        Diagnostics diagnostics(output);
        if (options.automatic) {
            Application app(std::move(options));
            if (!app.start()) {
                std::filesystem::create_directories(output);
                std::ofstream file(output/L"startup-error.txt",std::ios::binary); file << narrow(app.display()); return 2;
            }
            while (app.running()) { app.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            app.finish();
            return app.result();
        }
        UiController ui(std::move(options)); controller = &ui;
        WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = window_proc;
        wc.lpszClassName = L"XenRecorderWindow"; wc.hCursor = LoadCursorW(nullptr,IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        if (!RegisterClassW(&wc)) throw std::runtime_error("窗口注册失败");
        auto window = CreateWindowW(wc.lpszClassName,L"Xen 独立录制",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,750,350,nullptr,nullptr,instance,nullptr);
        if (!window) throw std::runtime_error("窗口创建失败");
        ShowWindow(window,show); MSG msg{};
        while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    } catch (const std::exception& error) {
        MessageBoxW(nullptr,wide(error.what()).c_str(),L"Xen 独立录制",MB_OK|MB_ICONERROR); return 2;
    }
}
