#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif

#include "config/config.h"
#include "recoil/recoil.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_win32(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + "，Win32Error=" + std::to_string(GetLastError()));
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.value_, nullptr));
        return *this;
    }
    void reset(HANDLE next = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = next;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_ = nullptr;
};

std::uint64_t window_number(HWND window) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window));
}

bool same_window(HWND window, DWORD process, DWORD thread = 0) noexcept {
    DWORD found_process = 0;
    const auto found_thread = GetWindowThreadProcessId(window, &found_process);
    return found_thread != 0 && found_process == process && (!thread || found_thread == thread) && IsWindow(window);
}

struct WindowSearch {
    DWORD process = 0;
    DWORD main_thread = 0;
    HWND owner = nullptr;
    HWND found = nullptr;
    bool picker = false;
};

BOOL CALLBACK find_window(HWND window, LPARAM opaque) noexcept {
    auto& search = *reinterpret_cast<WindowSearch*>(opaque);
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (process != search.process || !IsWindowVisible(window)) return TRUE;
    wchar_t text[128]{};
    if (search.picker) {
        if (thread == search.main_thread || GetWindow(window, GW_OWNER) != search.owner) return TRUE;
        GetWindowTextW(window, text, 128);
        if (std::wstring_view(text) != L"选择弹道曲线 JSON") return TRUE;
    } else {
        GetClassNameW(window, text, 128);
        if (std::wstring_view(text) != L"XenCodexOverlay") return TRUE;
    }
    search.found = window;
    return FALSE;
}

HWND search_window(WindowSearch search) {
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    require_win32(enumerated || search.found, "目标窗口枚举失败");
    return search.found;
}

void write_json(const std::filesystem::path& path, const Json& value) {
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << value.dump(2) << '\n';
        stream.close();
        require(stream.good(), "探针见证文件写入失败");
    }
    require_win32(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
        "探针见证文件原子替换失败");
}

struct RemainingWindows {
    DWORD process = 0;
    Json windows = Json::array();
    bool callback_failed = false;
};

BOOL CALLBACK collect_windows(HWND window, LPARAM opaque) noexcept {
    auto& found = *reinterpret_cast<RemainingWindows*>(opaque);
    try {
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(window, &process);
        if (process != found.process) return TRUE;
        wchar_t name[128]{};
        GetClassNameW(window, name, 128);
        found.windows.push_back({{"hwnd", window_number(window)}, {"thread_id", thread},
            {"class", utf8(std::filesystem::path(name))}, {"visible", IsWindowVisible(window) != FALSE}});
        return TRUE;
    } catch (...) { found.callback_failed = true; return FALSE; }
}

struct Probe {
    std::filesystem::path executable;
    std::filesystem::path output;
    bool error_mode = false;
    Clock::time_point started = Clock::now();
    Clock::time_point close_started{};
    Handle job;
    Handle process;
    Handle primary_thread;
    Handle picker_thread;
    DWORD process_id = 0;
    DWORD main_thread_id = 0;
    DWORD picker_thread_id = 0;
    HWND main_window = nullptr;
    HWND picker = nullptr;
    HWND prompt = nullptr;
    bool assigned = false;
    bool output_owned = false;
    bool prepared = false;
    bool close_requested = false;
    bool pre_exit_observed = false;
    bool process_exited = false;
    bool no_process_windows = false;
    bool job_empty = false;
    bool failed = false;
    bool rescue_used = false;
    bool forced = false;
    bool completed = false;
    DWORD exit_code = STILL_ACTIVE;
    std::string status = "preparing";
    std::string failure;
    Json events = Json::array();
    Json cleanup_errors = Json::array();

    ~Probe() {
        // 仅在显式失败回收本身异常时兜底；强制路径始终写入失败，不混入正常通过。
        if (completed || (!process && !job)) return;
        failed = rescue_used = true;
        status = "failed";
        if (process && WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0) {
            forced = true;
            if (assigned && job) TerminateJobObject(job.get(), 125);
            else TerminateProcess(process.get(), 125);
            process_exited = WaitForSingleObject(process.get(), 3000) == WAIT_OBJECT_0;
        }
        picker_thread.reset();
        primary_thread.reset();
        process.reset();
        if (job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
                if (accounting.ActiveProcesses != 0) { forced = true; TerminateJobObject(job.get(), 125); }
                const auto deadline = Clock::now() + std::chrono::seconds(3);
                do {
                    if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                        &accounting, sizeof(accounting), nullptr)) break;
                    if (accounting.ActiveProcesses == 0) { job_empty = true; break; }
                    Sleep(1);
                } while (Clock::now() < deadline);
            }
        }
        try { record("last_resort_cleanup", {{"forced", forced}, {"job_empty_before_close", job_empty},
            {"process_exited", process_exited}}); } catch (...) {}
    }

    long long elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    }

    Json state() const {
        return {{"schema_version", 1}, {"status", status}, {"pid", process_id}, {"mode", error_mode ? "error" : "picker"},
            {"executable", utf8(executable)}, {"output", utf8(output)}, {"ready", prepared},
            {"main_hwnd", window_number(main_window)}, {"main_thread_id", main_thread_id},
            {"picker_hwnd", window_number(picker)}, {"picker_thread_id", picker_thread_id},
            {"prompt_hwnd", window_number(prompt)}, {"close_requested", close_requested},
            {"pre_exit_observed", pre_exit_observed}, {"process_exited", process_exited},
            {"no_process_windows", no_process_windows}, {"job_empty", job_empty},
            {"exit_code", exit_code}, {"failed", failed}, {"failure", failure},
            {"rescue_used", rescue_used}, {"forced", forced}, {"cleanup_errors", cleanup_errors},
            {"events", events}};
    }

    void save() const {
        if (!output_owned) return;
        const auto current = state();
        write_json(output / "witness.json", current);
        write_json(output / "state.json", current);
    }

    void record(const char* event, Json detail = Json::object(), bool persist = true) {
        detail["event"] = event;
        detail["elapsed_ms"] = elapsed_ms();
        events.push_back(std::move(detail));
        if (persist) save();
    }

    void prepare_files() {
        require(executable.is_absolute() && std::filesystem::is_regular_file(executable),
            "必须传入存在的Xen.exe绝对路径");
        require(output.is_absolute() && !std::filesystem::exists(output),
            "必须传入尚不存在的独立绝对输出目录");
        std::filesystem::create_directories(output.parent_path());
        require(std::filesystem::create_directory(output), "独立输出目录创建失败");
        output_owned = true;
        std::filesystem::create_directory(output / "profiles");
        std::filesystem::create_directories(output / "cache/recoil");
        AppConfig config;
        config.mouse.backend = MouseBackend::WIN32_SEND_INPUT;
        config.mouse.allow_send_input = false;
        config.keyboard.aim_hold_virtual_keys.clear();
        config.keyboard.emergency_virtual_keys.clear();
        config.keyboard.runtime_toggle_virtual_keys.clear();
        config.keyboard.anomaly_mark_virtual_keys.clear();
        config.keyboard.debug_test_virtual_keys.clear();
        config.keyboard.debug_test_enabled = false;
        config.auto_stop.enabled = config.auto_stop.cycle_enabled = false;
        config.trigger.enabled = config.trigger.fire_enabled = false;
        config.recoil.enabled = config.movement.enabled = config.gsi.enabled = config.source_context.enabled = false;
        config.recoil.profile_directory = "profiles";
        config.recoil.sensitivity = 1;
        config.runtime.diagnostics_enabled = false;
        config.ui.width = 1100;
        config.ui.height = 850;
        config.log.log_dir = "logs";
        std::string error;
        const bool saved = save_app_config(utf8(output / "config.ini"), config, error);
        require(saved, error.c_str());
        RecoilProfile profile;
        profile.id = "xen-exe-shutdown-fixture";
        profile.weapon_id = "ak47";
        profile.state = RecoilProfileState::IMPORTED;
        profile.points = {{0, 0, 0}, {100, 2, 4}};
        {
            std::ofstream stream(output / "profiles/imported.json");
            stream << serialize_recoil_profile(profile);
            require(stream.good(), "合成曲线写入失败");
        }
        {
            std::ofstream stream(output / "cache/recoil/workflow-settings.json");
            stream << R"({"ak47":{"selected_file":"imported.json"}})";
            require(stream.good(), "合成弹道选择写入失败");
        }
        // 环境仅属于本探针及其子进程；不读取、输出或保存继承环境中的其他内容。
        require_win32(SetEnvironmentVariableW(L"XEN_DATA_ROOT", output.c_str()) != 0, "独立数据根设置失败");
        for (const auto* name : {L"XEN_RELEASE_ROOT", L"XEN_RUNTIME_ID", L"XEN_RELEASE_BACKENDS", L"XEN_RUNTIME_DIAGNOSTICS"})
            require_win32(SetEnvironmentVariableW(name, nullptr) != 0, "子进程发布环境隔离失败");
        record("fixture_prepared", {{"physical_output_allowed", false}, {"runtime_requested", false},
            {"prepare_deadline_ms", 300000}, {"close_deadline_ms", 3000}});
    }

    void launch() {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        require_win32(static_cast<bool>(job), "独立Job创建失败");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require_win32(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)) != 0,
            "独立Job退出兜底设置失败");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION child{};
        std::wstring command = L"\"" + executable.native() + L"\"";
        require_win32(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, output.c_str(), &startup, &child) != 0, "真实Xen.exe创建失败");
        process.reset(child.hProcess);
        primary_thread.reset(child.hThread);
        process_id = child.dwProcessId;
        // 创建后到Resume之间的所有失败都保留句柄，交给失败回收处理已存在的挂起进程。
        require_win32(AssignProcessToJobObject(job.get(), process.get()) != 0, "真实Xen.exe加入独立Job失败");
        assigned = true;
        const DWORD previous_suspend_count = ResumeThread(primary_thread.get());
        const DWORD resume_error = previous_suspend_count == static_cast<DWORD>(-1) ? GetLastError() : ERROR_SUCCESS;
        record("primary_thread_resumed", {{"previous_suspend_count", previous_suspend_count}, {"win32_error", resume_error}});
        require(previous_suspend_count == 1, "真实Xen.exe主线程恢复前的挂起计数必须恰好为1");
        primary_thread.reset();
        status = "waiting_main_window";
        record("process_started", {{"pid", process_id}, {"created_suspended", true},
            {"assigned_before_resume", true}, {"process_handle", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(process.get()))}});
        std::cout << "真实Xen.exe已启动，PID=" << process_id << "，证据目录=" << utf8(output) << std::endl;
    }

    DWORD process_state() const {
        const DWORD value = WaitForSingleObject(process.get(), 0);
        require_win32(value != WAIT_FAILED, "进程句柄状态读取失败");
        return value;
    }

    bool verified_main() const noexcept {
        if (!same_window(main_window, process_id, main_thread_id)) return false;
        wchar_t name[128]{};
        GetClassNameW(main_window, name, 128);
        return std::wstring_view(name) == L"XenCodexOverlay";
    }

    void wait_until_close_request() {
        const auto deadline = started + std::chrono::seconds(300);
        while (Clock::now() < deadline) {
            require(process_state() == WAIT_TIMEOUT, "真实Xen.exe在关闭场景准备完成前已退出");
            if (!main_window) {
                main_window = search_window({process_id});
                if (main_window) {
                    main_thread_id = GetWindowThreadProcessId(main_window, nullptr);
                    status = "waiting_picker";
                    record("main_window_observed", {{"main_hwnd", window_number(main_window)}, {"thread_id", main_thread_id}});
                }
            }
            if (main_window && !picker) {
                require(verified_main(), "准备期间已固定的主窗口失效");
                picker = search_window({process_id, main_thread_id, main_window, nullptr, true});
                if (picker) {
                    picker_thread_id = GetWindowThreadProcessId(picker, nullptr);
                    picker_thread.reset(OpenThread(SYNCHRONIZE, FALSE, picker_thread_id));
                    require_win32(static_cast<bool>(picker_thread), "真实文件选择线程句柄打开失败");
                    require(same_window(picker, process_id, picker_thread_id) && GetWindow(picker, GW_OWNER) == main_window,
                        "持有线程句柄后文件选择窗口身份发生变化");
                    require(!IsWindowEnabled(main_window), "文件选择器未实际禁用真实主窗口");
                    status = error_mode ? "waiting_error_prompt" : "picker_observed";
                    record("picker_observed", {{"picker_hwnd", window_number(picker)}, {"thread_id", picker_thread_id},
                        {"owner_is_main", true}, {"main_disabled", true}});
                }
            }
            if (picker) {
                require(same_window(picker, process_id, picker_thread_id) && IsWindowVisible(picker),
                    "关闭请求前已固定的文件选择窗口消失");
                require(WaitForSingleObject(picker_thread.get(), 0) == WAIT_TIMEOUT,
                    "关闭请求前实际文件选择线程必须仍在运行");
                if (error_mode && !prompt) {
                    const auto popup = GetWindow(picker, GW_ENABLEDPOPUP);
                    if (popup && popup != picker && IsWindowVisible(popup) && !IsWindowEnabled(picker) &&
                        GetWindow(popup, GW_OWNER) == picker && same_window(popup, process_id, picker_thread_id)) {
                        prompt = popup;
                        record("error_prompt_observed", {{"prompt_hwnd", window_number(prompt)},
                            {"same_sta", true}, {"direct_owner_is_picker", true}, {"picker_disabled", true}});
                    }
                }
                if ((!error_mode || prompt) && !prepared) {
                    prepared = true;
                    status = "ready";
                    record("ready_to_close", {{"main_alive", verified_main()}, {"picker_visible", true},
                        {"error_prompt_visible", prompt != nullptr}});
                    std::cout << "ready：已观察完整" << (error_mode ? "错误提示" : "文件选择器")
                        << "场景，等待close.request" << std::endl;
                }
            }
            if (prepared && std::filesystem::exists(output / "close.request")) return;
            Sleep(10);
        }
        throw std::runtime_error("准备场景或等待关闭请求超过300秒预算");
    }

    void close_and_observe() {
        require(process_state() == WAIT_TIMEOUT && verified_main(), "投递关闭前真实进程与主窗口必须仍存在");
        require(picker && same_window(picker, process_id, picker_thread_id) && IsWindowVisible(picker) &&
            WaitForSingleObject(picker_thread.get(), 0) == WAIT_TIMEOUT, "投递关闭前文件选择器与线程必须仍活跃");
        require(!error_mode || (prompt && same_window(prompt, process_id, picker_thread_id) &&
            GetWindow(prompt, GW_OWNER) == picker && IsWindowVisible(prompt) && !IsWindowEnabled(picker)),
            "错误场景必须在真实错误提示仍打开时请求关闭");
        status = "closing";
        close_requested = true;
        close_started = Clock::now();
        // 关闭期间不写磁盘，以免错过短暂的生产主窗口仍存活、picker已经回收的观察窗口。
        record("main_close_requested", {{"repeat_count", 2}, {"main_hwnd", window_number(main_window)},
            {"process_running_before", true}, {"picker_thread_running_before", true}}, false);
        require_win32(PostMessageW(main_window, WM_CLOSE, 0, 0) != 0, "真实主窗口关闭投递失败");
        require_win32(PostMessageW(main_window, WM_CLOSE, 0, 0) != 0, "真实主窗口重复关闭投递失败");
        const auto deadline = close_started + std::chrono::seconds(3);
        while (Clock::now() <= deadline) {
            const DWORD before = process_state();
            if (before == WAIT_OBJECT_0) { process_exited = true; break; }
            const bool main_before = verified_main();
            const DWORD thread_state = WaitForSingleObject(picker_thread.get(), 0);
            require_win32(thread_state != WAIT_FAILED, "关闭期间文件选择线程状态读取失败");
            const bool picker_gone = !IsWindow(picker);
            const bool prompt_gone = !prompt || !IsWindow(prompt);
            const bool main_after = verified_main();
            const DWORD after = process_state();
            if (!pre_exit_observed && before == WAIT_TIMEOUT && after == WAIT_TIMEOUT &&
                main_before && main_after && thread_state == WAIT_OBJECT_0 && picker_gone && prompt_gone) {
                pre_exit_observed = true;
                record("pre_exit_reclamation_observed", {{"process_running_before", true}, {"process_running_after", true},
                    {"main_alive_before", true}, {"main_alive_after", true}, {"picker_thread_signaled", true},
                    {"picker_gone", true}, {"prompt_gone", true}}, false);
            }
            if (after == WAIT_OBJECT_0) { process_exited = true; break; }
            Sleep(1);
        }
        require(process_exited, "真实主窗口关闭后3秒内进程未退出");
        require_win32(GetExitCodeProcess(process.get(), &exit_code) != 0, "真实进程退出码读取失败");
        RemainingWindows remaining{process_id};
        const BOOL enumerated = EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&remaining));
        require_win32(enumerated != 0, "退出后目标PID窗口枚举失败");
        require(!remaining.callback_failed, "退出后目标PID窗口见证收集失败");
        no_process_windows = remaining.windows.empty();
        record("process_exit_observed", {{"exit_code", exit_code}, {"remaining_process_windows", remaining.windows},
            {"window_enumeration_succeeded", true},
            {"close_elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - close_started).count()}}, false);
        // 先保存进程、线程与窗口证据，再释放进程引用；之后只凭私有Job查询，不用裸PID重新识别进程。
        picker_thread.reset();
        primary_thread.reset();
        process.reset();
        wait_for_empty_job(deadline, false);
        save();
        require(pre_exit_observed, "未观察到进程退出前主窗仍存在且文件选择线程/窗口均回收的快照，不能外推为通过");
        require(exit_code == 0, "真实Xen.exe未正常返回0");
        require(no_process_windows, "真实Xen.exe退出后仍存在目标PID窗口");
        require(job_empty, "关闭3秒预算内私有Job活动进程数未归零");
        require(Clock::now() <= deadline, "完整关闭证据超出3秒预算");
        status = "passed";
        completed = true;
        record("passed", {{"forced", false}, {"rescue_used", false}});
    }

    void wait_for_empty_job(Clock::time_point deadline, bool persist) {
        if (!job) return;
        DWORD active_count = 0;
        do {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            require_win32(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), nullptr) != 0, "独立Job活动进程查询失败");
            active_count = accounting.ActiveProcesses;
            if (active_count == 0) { job_empty = true; break; }
            Sleep(1);
        } while (Clock::now() < deadline);
        record("job_process_count_observed", {{"query_succeeded", true}, {"active_processes", active_count},
            {"empty_before_job_close", job_empty}, {"process_handles_released", !process && !primary_thread && !picker_thread}}, persist);
    }

    void fail_and_collect(const char* message) noexcept {
        failed = true;
        rescue_used = true;
        status = "failed";
        try { failure = message; record("failure", {{"message", failure}}); } catch (...) {}
        try {
            if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
                // 仅失败救援可以直接关子窗；这些消息永远不能参与正常通过证据。
                if (picker && same_window(picker, process_id, picker_thread_id)) {
                    const auto popup = GetWindow(picker, GW_ENABLEDPOPUP);
                    if (popup && popup != picker && GetWindow(popup, GW_OWNER) == picker &&
                        same_window(popup, process_id, picker_thread_id)) PostMessageW(popup, WM_CLOSE, 0, 0);
                    PostMessageW(picker, WM_COMMAND, IDCANCEL, 0);
                }
                if (verified_main()) PostMessageW(main_window, WM_CLOSE, 0, 0);
                const DWORD graceful = WaitForSingleObject(process.get(), 1000);
                if (graceful != WAIT_OBJECT_0) {
                    forced = true;
                    const BOOL terminated = assigned && job ? TerminateJobObject(job.get(), 125) : TerminateProcess(process.get(), 125);
                    if (!terminated) cleanup_errors.push_back({{"operation", "force_terminate"}, {"win32_error", GetLastError()}});
                }
                const DWORD waited = WaitForSingleObject(process.get(), 3000);
                process_exited = waited == WAIT_OBJECT_0;
                if (!process_exited) cleanup_errors.push_back({{"operation", "wait_process_exit"}, {"wait_result", waited}});
            } else if (process) process_exited = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
            if (process && process_exited) {
                if (!GetExitCodeProcess(process.get(), &exit_code))
                    cleanup_errors.push_back({{"operation", "get_exit_code"}, {"win32_error", GetLastError()}});
                RemainingWindows remaining{process_id};
                const BOOL enumerated = EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&remaining));
                no_process_windows = enumerated && !remaining.callback_failed && remaining.windows.empty();
                record("failed_run_process_recovered", {{"remaining_process_windows", remaining.windows},
                    {"window_enumeration_succeeded", enumerated != FALSE && !remaining.callback_failed},
                    {"forced", forced}, {"exit_code", exit_code}}, false);
            }
            // 未加入Job的子进程也必须单独确认退出；保留未知状态的句柄供析构兜底。
            if (process_exited) {
                picker_thread.reset();
                primary_thread.reset();
                process.reset();
            }
            if (job) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                require_win32(QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr) != 0, "失败回收Job查询失败");
                if (accounting.ActiveProcesses != 0) {
                    forced = true;
                    require_win32(TerminateJobObject(job.get(), 125) != 0, "失败回收私有Job终止失败");
                }
                wait_for_empty_job(Clock::now() + std::chrono::seconds(3), false);
            }
            completed = (process_id == 0 || process_exited) && (!assigned || job_empty);
            record("failed_run_cleanup_finished", {{"forced", forced}, {"job_empty_before_close", job_empty},
                {"process_exited", process_exited}, {"cleanup_complete", completed}});
        } catch (const std::exception& error) {
            try { cleanup_errors.push_back({{"message", error.what()}}); save(); } catch (...) {}
        } catch (...) {
            try { cleanup_errors.push_back({{"message", "失败回收发生未知异常"}}); save(); } catch (...) {}
        }
    }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 4, "用法：xen_exe_shutdown_probe <Xen.exe绝对路径> <全新绝对输出目录> <picker|error>");
        const std::wstring_view mode(argv[3]);
        require(mode == L"picker" || mode == L"error", "观察场景必须为picker或error");
        Probe probe;
        probe.executable = std::filesystem::path(argv[1]);
        probe.output = std::filesystem::path(argv[2]);
        probe.error_mode = mode == L"error";
        try {
            probe.prepare_files();
            probe.launch();
            probe.wait_until_close_request();
            probe.close_and_observe();
            std::cout << "真实Xen.exe关闭验证通过：" << (probe.error_mode ? "错误提示" : "文件选择器") << std::endl;
            return 0;
        } catch (const std::exception& error) {
            probe.fail_and_collect(error.what());
            std::cerr << "真实Xen.exe关闭验证失败：" << error.what() << std::endl;
            return 1;
        } catch (...) {
            probe.fail_and_collect("外部观察发生未知异常");
            std::cerr << "真实Xen.exe关闭验证失败：未知异常" << std::endl;
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "外部关闭探针参数失败：" << error.what() << std::endl;
        return 1;
    }
}
