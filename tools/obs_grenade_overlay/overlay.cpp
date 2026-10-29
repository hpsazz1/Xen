// SPDX-License-Identifier: GPL-2.0-or-later
// OBS负责道具图片与菜单；独立录像同步小窗由Win32绘制，不进入中央NDI场景。
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#ifdef ERROR
#undef ERROR
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "model.h"
#include "window_surface.h"
#include "hotkeys.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char* obs_module_description(void) { return "Xen 道具参考图：OBS渲染、游戏右上角显示"; }
MODULE_EXPORT const char* obs_module_name(void) { return "Xen 道具提示"; }
MODULE_EXPORT const char* obs_module_author(void) { return "Xen"; }

namespace {
using Clock = std::chrono::steady_clock;
using Json = xen_obs::Json;
constexpr uint32_t kWidth = 344, kHeight = 576;
constexpr uint32_t kMenuWidth = 1120, kMenuHeight = 640;
constexpr UINT kApplySettings = WM_APP + 21;
constexpr UINT kShowSyncMarker = WM_APP + 22, kSyncFailure = WM_APP + 23;
constexpr wchar_t kWindowClass[] = L"XenObsGrenadeReference";
constexpr char kSettingsSource[] = "xen_grenade_overlay_settings";

std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::int64_t system_ns() {
    FILETIME value{}; GetSystemTimePreciseAsFileTime(&value);
    ULARGE_INTEGER combined{}; combined.LowPart=value.dwLowDateTime; combined.HighPart=value.dwHighDateTime;
    return static_cast<std::int64_t>((combined.QuadPart-116444736000000000ULL)*100);
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("UTF-8文本无效");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}

std::string string_field(const Json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET input = nullptr) : value(input) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

struct Endpoint {
    std::string base;
    std::wstring host;
    INTERNET_PORT port = 0;
};

Endpoint endpoint(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() > 512) throw std::runtime_error("辅机地址过长");
    const auto input = wide(url);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(input.c_str(), static_cast<DWORD>(input.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTP || !parts.dwHostNameLength ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        (parts.dwUrlPathLength && std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/"))
        throw std::runtime_error("辅机地址应为http://地址:5015，不带路径或账号");
    return {std::move(url), std::wstring(parts.lpszHostName, parts.dwHostNameLength), parts.nPort};
}

struct HttpReply { DWORD status = 0; Json document = Json::object(); };

HttpReply request_json(HINTERNET session, HINTERNET connection, const std::string& path,
                      const Json* body = nullptr, std::int64_t* received_ns = nullptr) {
    const auto request_path = wide(path);
    InternetHandle request(WinHttpOpenRequest(connection, body ? L"POST" : L"GET", request_path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request.value) throw std::runtime_error("建立HTTP请求失败");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    auto content = body ? body->dump() : std::string{};
    const auto headers = body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(request.value, headers, body ? DWORD(-1) : 0,
                            body ? content.data() : WINHTTP_NO_REQUEST_DATA,
                            static_cast<DWORD>(content.size()),static_cast<DWORD>(content.size()),0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        throw std::runtime_error("辅机HTTP连接不可用");
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("辅机HTTP状态不可用");
    std::string bytes;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) throw std::runtime_error("读取HTTP内容失败");
        if (!available) break;
        if (Clock::now() >= deadline || bytes.size() + available > 512 * 1024)
            throw std::runtime_error("辅机响应超过读取预算");
        const auto offset = bytes.size(); bytes.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request.value, bytes.data() + offset, available, &received))
            throw std::runtime_error("读取HTTP内容失败");
        bytes.resize(offset + received);
        if (!received) break;
    }
    if (received_ns) *received_ns=steady_ns();
    auto document = Json::parse(bytes, nullptr, false);
    if (document.is_discarded()) document = Json::object();
    (void)session;
    return {status, std::move(document)};
}
HttpReply get_json(HINTERNET session, HINTERNET connection, const std::string& path) {
    return request_json(session,connection,path);
}

struct Configuration {
    bool enabled = true;
    std::string url = "http://127.0.0.1:5015";
    std::string capture_python, capture_root, capture_destination;
    uint64_t revision = 0;
};

struct Publication {
    xen_obs::View view;
    Json selection;
    Clock::time_point received{};
    uint64_t configuration = 0;
};
struct SyncJob {
    std::string marker_id;
    uint64_t configuration = 0;
    std::int64_t issued_at_ns = 0;
    bool painted = false;
    Json packet;
};

class Manager;
std::unique_ptr<Manager> manager;

class Manager {
public:
    ~Manager() { stop(); }

    void configure(obs_data_t* settings) noexcept {
        try {
            Configuration next;
            next.enabled = obs_data_get_bool(settings, "enabled");
            next.url = obs_data_get_string(settings, "auxiliary_url");
            next.capture_python = obs_data_get_string(settings, "capture_python");
            next.capture_root = obs_data_get_string(settings, "capture_root");
            next.capture_destination = obs_data_get_string(settings, "capture_destination");
            {
                std::lock_guard lock(mutex_);
                next.revision = configuration_.revision + 1;
                configuration_ = next;
                publication_.reset();
            }
            if (!config_file_.empty()) {
                obs_data_t* saved = obs_data_create();
                obs_data_set_bool(saved, "enabled", next.enabled);
                obs_data_set_string(saved, "auxiliary_url", next.url.c_str());
                obs_data_set_string(saved, "capture_python", next.capture_python.c_str());
                obs_data_set_string(saved, "capture_root", next.capture_root.c_str());
                obs_data_set_string(saved, "capture_destination", next.capture_destination.c_str());
                if (!obs_data_save_json_safe(saved, config_file_.c_str(), "tmp", "bak"))
                    blog(LOG_WARNING, "[xen-obs-overlay] 配置未能保存");
                obs_data_release(saved);
            }
            condition_.notify_all();
            if (const auto window = handle_.load()) PostMessageW(window, kApplySettings, 0, 0);
        } catch (...) { blog(LOG_WARNING, "[xen-obs-overlay] 配置未能更新"); }
    }

    void start() {
        if (started_) return;
        started_ = true;
        char* config_path = obs_module_config_path("settings.json");
        if (!config_path) throw std::runtime_error("OBS插件配置路径不可用");
        config_file_ = config_path; bfree(config_path);
        std::filesystem::create_directories(std::filesystem::path(wide(config_file_)).parent_path());
        obs_data_t* settings = obs_data_create_from_json_file_safe(config_file_.c_str(), "bak");
        if (!settings) settings = obs_data_create();
        obs_data_set_default_bool(settings, "enabled", true);
        obs_data_set_default_string(settings, "auxiliary_url", "http://127.0.0.1:5015");
        settings_source_ = obs_source_create_private(kSettingsSource, "Xen 道具提示设置", settings);
        obs_data_release(settings);
        if (!settings_source_) throw std::runtime_error("OBS提示设置源创建失败");

        char* html_path = obs_module_file("reference.html");
        if (!html_path) throw std::runtime_error("缺少reference.html");
        std::string path = html_path; bfree(html_path);
        page_url_ = xen_obs::file_url(std::filesystem::path(wide(path)));

        HMODULE instance = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&window_proc), &instance);
        WNDCLASSEXW descriptor{}; descriptor.cbSize = sizeof(descriptor);
        descriptor.lpfnWndProc = window_proc; descriptor.hInstance = instance;
        descriptor.lpszClassName = kWindowClass;
        descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("OBS提示窗口注册失败");
        instance_ = instance;
        sync_source_session_="obs-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
        const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_LAYERED | WS_EX_TRANSPARENT, kWindowClass, L"Xen OBS 道具提示", WS_POPUP,
            0, 0, kWidth, kHeight, nullptr, nullptr, instance_, this);
        if (!window) throw std::runtime_error("OBS提示窗口创建失败");
        handle_.store(window);
        prepare_window(window, 230);
        obs_data_t* browser_settings = obs_data_create();
        obs_data_set_bool(browser_settings, "is_local_file", false);
        obs_data_set_string(browser_settings, "url", (page_url_ + "?payload=%7B%7D").c_str());
        obs_data_set_int(browser_settings, "width", kWidth);
        obs_data_set_int(browser_settings, "height", kHeight);
        obs_data_set_int(browser_settings, "fps", 20);
        obs_data_set_bool(browser_settings, "fps_custom", true);
        obs_data_set_bool(browser_settings, "shutdown", false);
        obs_data_set_bool(browser_settings, "reroute_audio", false);
        browser_ = obs_source_create_private("browser_source", "Xen 道具参考渲染", browser_settings);
        obs_data_release(browser_settings);
        if (!browser_) throw std::runtime_error("OBS浏览器源不可用");
        obs_source_set_muted(browser_, true);

        gs_init_data graphics{};
        graphics.window.hwnd = window; graphics.cx = kWidth; graphics.cy = kHeight;
        graphics.format = GS_BGRA; graphics.zsformat = GS_ZS_NONE; graphics.num_backbuffers = 2;
        display_ = obs_display_create(&graphics, 0xFF181818);
        if (!display_) throw std::runtime_error("OBS提示GPU显示创建失败");
        obs_display_add_draw_callback(display_, draw, this);
        obs_display_set_enabled(display_, false);
        verify_capture_exclusion(window);
        menu_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
            kWindowClass, L"Xen OBS 道具菜单", WS_POPUP, 0, 0, kMenuWidth, kMenuHeight,
            nullptr, nullptr, instance_, this);
        if (!menu_window_) throw std::runtime_error("OBS道具菜单创建失败");
        prepare_window(menu_window_, 235);
        obs_data_t* menu_settings = obs_data_create();
        obs_data_set_string(menu_settings, "url", "about:blank");
        obs_data_set_int(menu_settings, "width", kMenuWidth);
        obs_data_set_int(menu_settings, "height", kMenuHeight);
        obs_data_set_int(menu_settings, "fps", 20);
        obs_data_set_bool(menu_settings, "fps_custom", true);
        obs_data_set_bool(menu_settings, "shutdown", false);
        menu_browser_ = obs_source_create_private("browser_source", "Xen 道具菜单渲染", menu_settings);
        obs_data_release(menu_settings);
        if (!menu_browser_) throw std::runtime_error("OBS菜单浏览器源不可用");
        obs_source_set_muted(menu_browser_, true);
        graphics.window.hwnd = menu_window_; graphics.cx = kMenuWidth; graphics.cy = kMenuHeight;
        menu_display_ = obs_display_create(&graphics, 0xFF181818);
        if (!menu_display_) throw std::runtime_error("OBS菜单GPU显示创建失败");
        obs_display_add_draw_callback(menu_display_, draw_menu, this);
        obs_display_set_enabled(menu_display_, false);
        verify_capture_exclusion(menu_window_);
        sync_window_=CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_LAYERED | WS_EX_TRANSPARENT,kWindowClass,L"Xen 录像同步标记",WS_POPUP,
            0,0,560,64,nullptr,nullptr,instance_,this);
        if (!sync_window_ || xen_obs::prepare_sync_window(sync_window_) != ERROR_SUCCESS)
            throw std::runtime_error("录像同步窗口创建或录制属性设置失败");
        for (size_t index=0;index<hotkey_ids_.size();++index)
            hotkey_ids_[index] = GlobalAddAtomW(xen_obs::kHotkeys[index].atom_name);
        SetTimer(window, 1, 100, nullptr);
        worker_ = std::thread([this] { poll(); });
        blog(LOG_INFO, "[xen-obs-overlay] 已加载：OBS私有浏览器源与桌面显示，不加入NDI场景");
    }

    void open_settings() {
        if (settings_source_) obs_frontend_open_source_properties(settings_source_);
    }

    void stop() noexcept {
        if (stopping_.exchange(true)) return;
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (const auto window = handle_.load()) KillTimer(window, 1);
        close_menu(false);
        for (size_t index=0;index<hotkey_ids_.size();++index) if (hotkey_ids_[index]) {
            UnregisterHotKey(handle_.load(), hotkey_ids_[index]); GlobalDeleteAtom(hotkey_ids_[index]);
        }
        if (capture_process_) {
            TerminateProcess(capture_process_, 1); WaitForSingleObject(capture_process_, 2000);
            CloseHandle(capture_process_); capture_process_ = nullptr;
        }
        if (menu_display_) {
            obs_display_remove_draw_callback(menu_display_, draw_menu, this);
            obs_display_destroy(menu_display_); menu_display_ = nullptr;
        }
        if (menu_browser_) { obs_source_release(menu_browser_); menu_browser_ = nullptr; }
        if (menu_window_) { DestroyWindow(menu_window_); menu_window_ = nullptr; }
        if (sync_window_) { DestroyWindow(sync_window_); sync_window_=nullptr; }
        set_visible(false);
        // 先解绑GPU回调并销毁display，之后才能释放它访问的browser和本对象。
        if (display_) {
            obs_display_remove_draw_callback(display_, draw, this);
            obs_display_destroy(display_); display_ = nullptr;
        }
        if (browser_) { obs_source_release(browser_); browser_ = nullptr; }
        if (settings_source_) { obs_source_release(settings_source_); settings_source_ = nullptr; }
        if (const auto window = handle_.exchange(nullptr)) DestroyWindow(window);
        if (instance_) UnregisterClassW(kWindowClass, instance_);
    }

private:
    static void prepare_window(HWND window, BYTE opacity) noexcept {
        const auto error = xen_obs::prepare_desktop_window(window, opacity);
        if (error != ERROR_SUCCESS)
            blog(LOG_WARNING, "[xen-obs-overlay] 系统截图隐藏未设置成功 error=%lu", error);
    }

    static void verify_capture_exclusion(HWND window) noexcept {
        DWORD affinity = WDA_NONE;
        if (GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE)
            blog(LOG_INFO, "[xen-obs-overlay] 实际显示窗口截图排除已回读：0x11");
        else blog(LOG_WARNING, "[xen-obs-overlay] 实际显示窗口截图排除未生效 value=0x%lx", affinity);
    }

    static bool game_window(HWND window) noexcept {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        wchar_t path[32768]; DWORD length = static_cast<DWORD>(std::size(path));
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!queried) return false;
        const auto file = std::filesystem::path(std::wstring(path, length)).filename().wstring();
        return _wcsicmp(file.c_str(), L"cs2.exe") == 0;
    }

    void update_hotkeys(bool enabled) noexcept {
        const auto foreground = GetForegroundWindow();
        if (foreground != checked_foreground_) {
            checked_foreground_ = foreground;
            foreground_is_game_ = game_window(foreground);
        }
        const bool wanted = enabled && (foreground_is_game_ || foreground == menu_window_);
        for (size_t index=0;index<hotkey_ids_.size();++index) {
            const auto& binding = xen_obs::kHotkeys[index];
            const bool sync = binding.capture_role && std::string_view(binding.capture_role)=="sync";
            const bool binding_wanted = wanted && (!sync || sync_available_.load());
            if (binding_wanted == hotkeys_registered_[index] || !hotkey_ids_[index]) continue;
            if (binding_wanted) {
                const auto& binding = xen_obs::kHotkeys[index];
                hotkeys_registered_[index] = RegisterHotKey(handle_.load(), hotkey_ids_[index], binding.modifiers, binding.virtual_key) != FALSE;
                if (!hotkeys_registered_[index] && !hotkey_error_logged_[index]) {
                    blog(LOG_WARNING, "[xen-obs-overlay] 热键注册失败 vk=%u modifiers=0x%x error=%lu", binding.virtual_key, binding.modifiers, GetLastError());
                    hotkey_error_logged_[index] = true;
                }
            } else { UnregisterHotKey(handle_.load(), hotkey_ids_[index]); hotkeys_registered_[index] = false; }
        }
    }

    void toggle_menu() noexcept {
        if (menu_visible_) { close_menu(); return; }
        if (capture_process_ || pending_capture_ || !menu_browser_) return;
        try {
            Configuration config; Json selection;
            { std::lock_guard lock(mutex_); config = configuration_; if (publication_) selection = publication_->selection; }
            if (!config.enabled) return;
            const auto target = endpoint(config.url);
            return_window_ = GetForegroundWindow();
            menu_selection_.open(selection);
            menu_configuration_ = config.revision;
            MONITORINFO info{}; info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(return_window_, MONITOR_DEFAULTTOPRIMARY), &info))
                throw std::runtime_error("菜单显示器尺寸不可用");
            const auto screen_width = info.rcMonitor.right - info.rcMonitor.left;
            const auto screen_height = info.rcMonitor.bottom - info.rcMonitor.top;
            const auto available_width = std::max(1L,screen_width-32);
            const auto available_height = std::max(1L,screen_height-32);
            const auto width = std::min(std::clamp(screen_width * 7 / 16,960L,static_cast<long>(kMenuWidth)),
                std::min(available_width,available_height * static_cast<long>(kMenuWidth) / static_cast<long>(kMenuHeight)));
            const auto height = std::min(available_height,std::max(1L,
                width * static_cast<long>(kMenuHeight) / static_cast<long>(kMenuWidth)));
            // 每次呼出刷新GSI与清单；旧选择只作基线，不能立即关闭刚打开的菜单。
            obs_data_t* settings = obs_source_get_settings(menu_browser_);
            const auto url = target.base + "/obs-menu#open-" + std::to_string(++menu_open_count_);
            obs_data_set_string(settings, "url", url.c_str());
            // 以真实客户区像素排版，不能将固定小纹理拉伸到大窗口造成文字发虚。
            obs_data_set_int(settings, "width", width);
            obs_data_set_int(settings, "height", height);
            obs_source_update(menu_browser_, settings); obs_data_release(settings);
            set_visible(false);
            obs_display_resize(menu_display_, width, height);
            obs_source_inc_showing(menu_browser_); obs_display_set_enabled(menu_display_, true);
            menu_visible_ = true;
            SetWindowPos(menu_window_, HWND_TOPMOST, info.rcMonitor.left + (info.rcMonitor.right-info.rcMonitor.left-width)/2,
                info.rcMonitor.top+(info.rcMonitor.bottom-info.rcMonitor.top-height)/2, width, height, 0);
            xen_obs::round_desktop_window(menu_window_, width, height);
            ShowWindow(menu_window_, SW_SHOW);
            verify_capture_exclusion(menu_window_);
            blog(LOG_INFO, "[xen-obs-overlay] 菜单窗口与Browser均为%ldx%ld，原尺寸渲染", width, height);
            if (SetForegroundWindow(menu_window_)) {
                SetFocus(menu_window_); ClipCursor(nullptr);
                obs_source_send_focus(menu_browser_, true);
            } else blog(LOG_WARNING, "[xen-obs-overlay] 菜单已显示，但系统未授予前台焦点");
        } catch (const std::exception& error) { blog(LOG_WARNING, "[xen-obs-overlay] 菜单未打开：%s", error.what()); }
    }

    void close_menu(bool restore = true) noexcept {
        if (!menu_visible_) return;
        const bool owns_focus = GetForegroundWindow() == menu_window_;
        menu_visible_ = false;
        if (GetCapture() == menu_window_) ReleaseCapture();
        obs_source_send_focus(menu_browser_, false);
        ShowWindow(menu_window_, SW_HIDE); obs_display_set_enabled(menu_display_, false);
        obs_source_dec_showing(menu_browser_); menu_selection_.reset();
        if (restore && owns_focus && IsWindow(return_window_)) SetForegroundWindow(return_window_);
    }

    LRESULT menu_message(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_MOUSEACTIVATE) return MA_ACTIVATE;
        if (message == WM_NCHITTEST) return HTCLIENT;
        if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) { close_menu(); return 0; }
        if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && menu_visible_) { close_menu(false); return 0; }
        if (!menu_browser_) return DefWindowProcW(window, message, wp, lp);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            obs_source_send_focus(menu_browser_, message == WM_SETFOCUS); return 0;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
            message == WM_MOUSEWHEEL || message == WM_MOUSELEAVE) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (message == WM_MOUSEWHEEL) ScreenToClient(window, &point);
            RECT client{}; GetClientRect(window, &client);
            obs_mouse_event event{};
            event.x = point.x * static_cast<int>(obs_source_get_width(menu_browser_)) / std::max(1L, client.right);
            event.y = point.y * static_cast<int>(obs_source_get_height(menu_browser_)) / std::max(1L, client.bottom);
            if (GetKeyState(VK_LBUTTON) & 0x8000) event.modifiers |= INTERACT_MOUSE_LEFT;
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
                if (message == WM_LBUTTONDOWN) SetCapture(window);
                obs_source_send_mouse_click(menu_browser_, &event, MOUSE_LEFT, message == WM_LBUTTONUP, 1);
                if (message == WM_LBUTTONUP && GetCapture() == window) ReleaseCapture();
            } else if (message == WM_MOUSEWHEEL) obs_source_send_mouse_wheel(menu_browser_, &event, 0, GET_WHEEL_DELTA_WPARAM(wp));
            else {
                obs_source_send_mouse_move(menu_browser_, &event, message == WM_MOUSELEAVE);
                if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking); }
            }
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    static void draw_menu(void* data, uint32_t cx, uint32_t cy) {
        const auto* self = static_cast<Manager*>(data);
        if (!self->menu_browser_) return;
        gs_viewport_push(); gs_projection_push(); gs_matrix_push();
        gs_set_viewport(0, 0, cx, cy);
        gs_ortho(0, static_cast<float>(obs_source_get_width(self->menu_browser_)),
            0, static_cast<float>(obs_source_get_height(self->menu_browser_)), -100, 100);
        gs_matrix_identity();
        obs_source_video_render(self->menu_browser_);
        gs_matrix_pop(); gs_projection_pop(); gs_viewport_pop();
    }

    void notice(std::string message) {
        blog(LOG_INFO, "[xen-obs-overlay] %s", message.c_str());
        notice_ = {{"title", "道具截图"}, {"message",std::move(message)}, {"notification",true}};
        notice_until_ = Clock::now() + std::chrono::seconds(6);
    }

    void request_capture(const char* role) noexcept {
        if (capture_process_ || pending_capture_) return;
        if (sync_window_) ShowWindow(sync_window_,SW_HIDE);
        capture_role_ = role;
        if (menu_visible_) close_menu();
        pending_capture_ = GetForegroundWindow();
        set_visible(false); notice_until_ = {};
        // 全屏采集前让桌面合成移除插件窗口，不把提示卡写进原图。
        capture_after_ = Clock::now() + std::chrono::milliseconds(150);
    }

    static std::wstring quote_argument(const std::wstring& value) {
        std::wstring result = L"\""; size_t slashes = 0;
        for (const auto c : value) {
            if (c == L'\\') { ++slashes; continue; }
            result.append(c == L'\"' ? slashes*2+1 : slashes, L'\\'); slashes = 0; result.push_back(c);
        }
        result.append(slashes*2, L'\\'); result.push_back(L'\"'); return result;
    }

    void start_capture(const Configuration& config) {
        const auto foreground = pending_capture_; pending_capture_ = nullptr;
        try {
            const auto target = endpoint(config.url);
            if (config.capture_python.empty() || config.capture_root.empty() || config.capture_destination.empty())
                throw std::runtime_error("采集路径尚未配置，请在OBS道具设置中填写");
            const auto root = std::filesystem::path(wide(config.capture_root));
            const auto jobs = root / L"capture-jobs"; std::filesystem::create_directories(jobs);
            capture_status_ = jobs / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".json");
            char* script_path = obs_module_file("capture/worker.py");
            if (!script_path) throw std::runtime_error("采集内部作业未安装");
            const std::string script_text = script_path; bfree(script_path);
            const auto script = std::filesystem::canonical(std::filesystem::path(wide(script_text)));
            DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
            const auto executable = wide(config.capture_python);
            std::wstring arguments = quote_argument(executable) + L" -X utf8 -B " + quote_argument(script.wstring());
            const auto add = [&](const wchar_t* name, const std::wstring& value) { arguments += L" "; arguments += name; arguments += L" "; arguments += quote_argument(value); };
            add(L"--host", target.host); add(L"--web-port", std::to_wstring(target.port));
            add(L"--root", root.wstring()); add(L"--destination", wide(config.capture_destination));
            add(L"--window", std::to_wstring(reinterpret_cast<std::uintptr_t>(foreground)));
            add(L"--process", std::to_wstring(process)); add(L"--job-status", capture_status_.wstring());
            add(L"--role", wide(capture_role_));
            STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION launched{};
            if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                nullptr, script.parent_path().c_str(), &startup, &launched))
                throw std::runtime_error("无法启动采集内部作业，请核对Python路径");
            CloseHandle(launched.hThread); capture_process_ = launched.hProcess;
            capture_job_.start(GetTickCount64());
            blog(LOG_INFO, "[xen-obs-overlay] %s采集作业已开始", capture_role_ == "standing" ? "PgUp站位图" : "PgDn瞄点图");
        } catch (const std::exception& error) { notice(error.what()); }
    }

    void poll_capture() {
        if (!capture_process_) return;
        Json status;
        try {
            if (std::filesystem::file_size(capture_status_) < 16384) {
                std::ifstream input(capture_status_); status = Json::parse(input, nullptr, false);
            }
        } catch (...) {}
        DWORD code = STILL_ACTIVE; GetExitCodeProcess(capture_process_, &code);
        if (!capture_job_.running()) {
            if (code != STILL_ACTIVE) { CloseHandle(capture_process_); capture_process_ = nullptr; }
            return;
        }
        const auto message = capture_job_.poll(GetTickCount64(), code == STILL_ACTIVE ? std::nullopt : std::optional<int>(code), status);
        if (!message) return;
        if (code == STILL_ACTIVE) {
            TerminateProcess(capture_process_, 1);
            if (WaitForSingleObject(capture_process_, 2000) != WAIT_OBJECT_0) {
                blog(LOG_WARNING, "[xen-obs-overlay] 采集作业停止未确认");
                notice(*message + "；作业停止尚未确认，原资源保留");
                return; // 继续持有句柄，实际退出前不能并行写下一份资源。
            }
        }
        CloseHandle(capture_process_); capture_process_ = nullptr;
        notice(*message);
    }

    void fail_sync(const std::string& reason, const std::string& marker_id = {}) noexcept {
        try {
            {
                std::lock_guard lock(mutex_);
                if (!marker_id.empty() && marker_id!=sync_current_id_) return;
                sync_failure_=reason; sync_job_.reset(); sync_show_.reset(); sync_show_permit_.reset();
                sync_current_id_.clear(); sync_inflight_.store(false);
            }
            blog(LOG_WARNING,"[xen-obs-sync] 同步标记未确认：%s",reason.c_str());
            if (const auto window=handle_.load()) PostMessageW(window,kSyncFailure,0,0);
        } catch (...) {}
    }

    void expire_sync_show() noexcept {
        try {
            std::string expired;
            {
                std::lock_guard lock(mutex_);
                if (sync_show_permit_ && xen_obs::sync_show_expired(*sync_show_permit_,steady_ns()))
                    expired=sync_show_permit_->marker_id;
            }
            if (!expired.empty()) fail_sync(expired+"：同步显示请求超过两秒，已取消，不自动重放",expired);
        } catch (...) {}
    }

    void request_sync_marker() noexcept {
        if (capture_process_ || pending_capture_ || menu_visible_ || !sync_available_.load() ||
            (sync_window_ && IsWindowVisible(sync_window_)) || sync_inflight_.exchange(true)) return;
        try {
            std::lock_guard lock(mutex_);
            if (!configuration_.enabled) { sync_inflight_.store(false); return; }
            SyncJob job; job.marker_id="SYNC-"+sync_source_session_+"-"+std::to_string(++sync_sequence_);
            job.configuration=configuration_.revision;
            job.issued_at_ns=steady_ns();
            sync_current_id_=job.marker_id;
            sync_show_permit_=xen_obs::SyncShowPermit{job.marker_id,job.configuration,job.issued_at_ns};
            sync_job_=std::move(job);
            condition_.notify_all();
        } catch (...) { fail_sync("同步请求创建失败"); }
    }

    void show_sync_marker() noexcept {
        std::string marker_id;
        try {
            SyncJob job;
            {
                std::lock_guard lock(mutex_);
                if (!sync_show_) return;
                job=std::move(*sync_show_); sync_show_.reset();
                marker_id=job.marker_id;
                if (!configuration_.enabled || configuration_.revision!=job.configuration)
                    throw std::runtime_error("标记显示前插件设置已改变");
                if (!sync_show_permit_ || !xen_obs::sync_show_current(*sync_show_permit_,job.marker_id,
                        configuration_.revision,steady_ns()))
                    throw std::runtime_error("同步显示请求已过期或被新编号替代");
            }
            const auto foreground=GetForegroundWindow();
            if (!sync_window_ || !game_window(foreground) || capture_process_ || pending_capture_ || menu_visible_)
                throw std::runtime_error("请在游戏前台且没有采集作业时同步");
            MONITORINFO info{}; info.cbSize=sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(foreground,MONITOR_DEFAULTTOPRIMARY),&info))
                throw std::runtime_error("同步标记显示器不可用");
            const auto& screen=info.rcMonitor;
            RECT marker{screen.left+12,screen.top+12,screen.left+572,screen.top+76};
            const LONG center_x=(screen.left+screen.right)/2, center_y=(screen.top+screen.bottom)/2;
            RECT roi{center_x-160,center_y-160,center_x+160,center_y+160}, overlap{};
            if (marker.right>screen.right || marker.bottom>screen.bottom || IntersectRect(&overlap,&marker,&roi))
                throw std::runtime_error("显示器不足以在中央ROI外显示同步标记");
            active_sync_=std::move(job);
            sync_text_=wide(active_sync_.marker_id);
            sync_paint_pending_=true;
            sync_until_=Clock::now()+std::chrono::seconds(2);
            SetWindowPos(sync_window_,HWND_TOPMOST,marker.left,marker.top,560,64,SWP_NOACTIVATE);
            ShowWindow(sync_window_,SW_SHOWNOACTIVATE);
            if (!RedrawWindow(sync_window_,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW))
                throw std::runtime_error("同步标记绘制请求失败");
        } catch (const std::exception& error) {
            sync_paint_pending_=false;
            if (sync_window_) ShowWindow(sync_window_,SW_HIDE);
            fail_sync(error.what(),marker_id);
        } catch (...) { fail_sync("同步标记显示失败",marker_id); }
    }

    LRESULT sync_message(HWND window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message==WM_NCHITTEST) return HTTRANSPARENT;
        if (message==WM_ERASEBKGND) return 1;
        if (message==WM_CLOSE) { ShowWindow(window,SW_HIDE); return 0; }
        if (message==WM_PAINT) {
            // 这是源机GDI绘制调用区间；窗口合成和录像首个可见帧仍须由成片确认。
            const auto began=steady_ns(), utc=system_ns();
            PAINTSTRUCT paint{}; const auto dc=BeginPaint(window,&paint);
            RECT client{}; GetClientRect(window,&client);
            const bool drawn=dc && xen_obs::paint_sync_window(dc,client,sync_text_.c_str());
            const bool ended=EndPaint(window,&paint)!=FALSE;
            const auto completed=steady_ns();
            if (sync_paint_pending_) {
                sync_paint_pending_=false;
                if (!drawn || !ended) { fail_sync("同步标记绘制未完成",active_sync_.marker_id); return 0; }
                try {
                    active_sync_.painted=true;
                    active_sync_.packet=xen_obs::sync_paint_packet(active_sync_.marker_id,sync_source_session_,began,completed,utc);
                    {
                        std::lock_guard lock(mutex_);
                        if (!sync_show_permit_ || !xen_obs::sync_show_current(*sync_show_permit_,active_sync_.marker_id,
                                configuration_.revision,completed))
                            throw std::runtime_error("同步绘制完成时显示请求已过期");
                        sync_job_=active_sync_; sync_show_permit_.reset();
                    }
                    condition_.notify_all();
                    blog(LOG_INFO,"[xen-obs-sync] 绘制完成 id=%s source_begin=%lld source_end=%lld utc=%lld；非视频PTS",
                        active_sync_.marker_id.c_str(),static_cast<long long>(began),
                        static_cast<long long>(completed),static_cast<long long>(utc));
                } catch (...) { fail_sync("同步标记证据创建失败或已过期",active_sync_.marker_id); }
            }
            return 0;
        }
        return DefWindowProcW(window,message,wp,lp);
    }

    void process_sync(const Configuration& config) noexcept {
        std::optional<SyncJob> job;
        { std::lock_guard lock(mutex_); job=std::move(sync_job_); sync_job_.reset(); }
        if (!job) return;
        const auto marker_id=job->marker_id;
        try {
            if (!config.enabled || config.revision!=job->configuration)
                throw std::runtime_error("同步期间插件设置已改变");
            const auto target=endpoint(config.url);
            InternetHandle session(WinHttpOpen(L"XenObsVideoSync/1",WINHTTP_ACCESS_TYPE_NO_PROXY,
                WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
            if (!session.value) throw std::runtime_error("同步HTTP初始化失败");
            WinHttpSetTimeouts(session.value,250,250,250,250);
            InternetHandle connection(WinHttpConnect(session.value,target.host.c_str(),target.port,0));
            if (!connection.value) throw std::runtime_error("同步HTTP连接失败");
            if (!job->painted) {
                const auto status=get_json(session.value,connection.value,"/sync-status.json");
                if (status.status!=200 || !xen_obs::sync_status_available(status.document)) {
                    const auto reason=string_field(status.document,"reason");
                    throw std::runtime_error(reason.empty() ? "辅机未开启详细运行记录、Runtime未就绪或F7已被占用" : reason);
                }
                {
                    std::lock_guard lock(mutex_);
                    if (!sync_show_permit_ || !xen_obs::sync_show_current(*sync_show_permit_,marker_id,
                            config.revision,steady_ns()))
                        throw std::runtime_error("同步预检查完成时显示请求已过期");
                    sync_show_=std::move(job);
                }
                if (!PostMessageW(handle_.load(),kShowSyncMarker,0,0))
                    throw std::runtime_error("同步显示请求未送达");
                return;
            }
            job->packet["source_send_steady_ns"]=std::to_string(steady_ns());
            job->packet["source_send_system_unix_ns"]=std::to_string(system_ns());
            blog(LOG_INFO,"[xen-obs-sync] 发送绘制证据 id=%s source_send=%s utc=%s",
                job->marker_id.c_str(),job->packet.at("source_send_steady_ns").get_ref<const std::string&>().c_str(),
                job->packet.at("source_send_system_unix_ns").get_ref<const std::string&>().c_str());
            std::int64_t received=0;
            const auto reply=request_json(session.value,connection.value,"/sync-marker.json",&job->packet,&received);
            if (reply.status<200 || reply.status>=300 || !xen_obs::sync_reply_accepted(reply.document,job->marker_id)) {
                const auto reason=string_field(reply.document,"reason");
                throw std::runtime_error(reason.empty() ? "辅机拒绝同步标记或未返回匹配的排队回执" : reason);
            }
            const auto receipt=xen_obs::sync_receipt_packet(job->packet,reply.document,received);
            // 原始四时戳独立保存，辅助机后续校准不依赖本机与辅机UTC已经一致。
            blog(LOG_INFO,"[xen-obs-sync] 四时戳 id=%s source_send=%s aux_received=%s aux_replied=%s source_received=%lld",
                job->marker_id.c_str(),job->packet.at("source_send_steady_ns").get_ref<const std::string&>().c_str(),
                reply.document.at("aux_received_steady_ns").get_ref<const std::string&>().c_str(),
                reply.document.at("aux_replied_steady_ns").get_ref<const std::string&>().c_str(),static_cast<long long>(received));
            const auto final=request_json(session.value,connection.value,"/sync-marker.json",&receipt);
            if (final.status<200 || final.status>=300 || !xen_obs::sync_reply_accepted(final.document,job->marker_id))
                throw std::runtime_error("同步绘制已排队，但四时戳补充回执未确认");
            {
                std::lock_guard lock(mutex_);
                if (sync_current_id_==marker_id) { sync_current_id_.clear(); sync_inflight_.store(false); }
            }
            blog(LOG_INFO,"[xen-obs-sync] 同步证据已排队 id=%s；落盘以辅机归档为准",job->marker_id.c_str());
        } catch (const std::exception& error) { fail_sync(marker_id+"："+error.what(),marker_id); }
        catch (...) { fail_sync(marker_id+"：同步传输失败",marker_id); }
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        Manager* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && window == self->menu_window_) return self->menu_message(window, message, wp, lp);
        if (self && window == self->sync_window_) return self->sync_message(window,message,wp,lp);
        if (self && message == kShowSyncMarker) { self->show_sync_marker(); return 0; }
        if (self && message == kSyncFailure) {
            std::string reason;
            { std::lock_guard lock(self->mutex_); reason=self->sync_failure_; }
            self->notice("录像同步失败："+reason); return 0;
        }
        if (self && message == WM_HOTKEY) {
            for (size_t index=0;index<self->hotkey_ids_.size();++index) {
                if (wp != self->hotkey_ids_[index]) continue;
                const auto role = xen_obs::kHotkeys[index].capture_role;
                if (role && std::string_view(role)=="sync") self->request_sync_marker();
                else if (role) self->request_capture(role); else self->toggle_menu();
                break;
            }
            return 0;
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_ERASEBKGND) return 1;
        if (self && (message == WM_TIMER || message == kApplySettings)) { self->tick(); return 0; }
        if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }

    static void draw(void* data, uint32_t cx, uint32_t cy) {
        auto* self = static_cast<Manager*>(data);
        if (!self->browser_) return;
        gs_viewport_push(); gs_projection_push(); gs_matrix_push();
        gs_set_viewport(0, 0, cx, cy);
        gs_ortho(0.0f, static_cast<float>(obs_source_get_width(self->browser_)), 0.0f,
                 static_cast<float>(obs_source_get_height(self->browser_)), -100.0f, 100.0f);
        gs_matrix_identity();
        obs_source_video_render(self->browser_);
        gs_matrix_pop(); gs_projection_pop(); gs_viewport_pop();
    }

    void set_visible(bool show) noexcept {
        const auto window = handle_.load();
        if (!window || !browser_) return;
        if (show == visible_) return;
        if (show) {
            obs_source_inc_showing(browser_);
            if (display_) obs_display_set_enabled(display_, true);
            RECT desktop{};
            const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info)) desktop = info.rcMonitor;
            else { desktop.right = GetSystemMetrics(SM_CXSCREEN); desktop.bottom = GetSystemMetrics(SM_CYSCREEN); }
            SetWindowPos(window, HWND_TOPMOST, std::max(desktop.left, desktop.right - static_cast<LONG>(kWidth) - 16),
                         desktop.top + 16, kWidth, card_height_, SWP_NOACTIVATE);
            xen_obs::round_desktop_window(window, kWidth, card_height_);
            ShowWindow(window, SW_SHOWNOACTIVATE);
            verify_capture_exclusion(window);
        } else {
            ShowWindow(window, SW_HIDE);
            if (display_) obs_display_set_enabled(display_, false);
            if (visible_) obs_source_dec_showing(browser_);
        }
        visible_ = show;
    }

    void tick() noexcept {
        try {
            if (stopping_) return;
            Configuration config;
            std::shared_ptr<const Publication> current;
            { std::lock_guard lock(mutex_); config = configuration_; current = publication_; }
            update_hotkeys(config.enabled);
            expire_sync_show();
            if (sync_window_ && IsWindowVisible(sync_window_) &&
                (Clock::now()>=sync_until_ || !config.enabled || !foreground_is_game_))
                ShowWindow(sync_window_,SW_HIDE);
            poll_capture();
            if (pending_capture_ && Clock::now() >= capture_after_) start_capture(config);
            if (menu_visible_) {
                if (!config.enabled || menu_configuration_ != config.revision) close_menu();
                else if (current && menu_selection_.should_close(current->selection)) close_menu();
                else { set_visible(false); return; }
            }
            if (capture_process_ || pending_capture_) { set_visible(false); return; }
            if (Clock::now() < notice_until_) { show_payload(notice_, 112); return; }
            if (!config.enabled || !current || current->configuration != config.revision ||
                Clock::now() - current->received > std::chrono::milliseconds(1200) || !current->view.visible) {
                set_visible(false); return;
            }
            show_payload(current->view.payload, kHeight);
        } catch (...) { set_visible(false); }
    }

    void show_payload(const Json& value, uint32_t height) {
            const auto payload = value.dump();
            if (payload != last_payload_) {
                set_visible(false);
                card_height_ = height;
                obs_display_resize(display_, kWidth, height);
                obs_data_t* settings = obs_source_get_settings(browser_);
                obs_data_set_int(settings, "height", height);
                obs_data_set_string(settings, "url", (page_url_ + "?payload=" + xen_obs::percent_encode(payload)).c_str());
                obs_source_update(browser_, settings); obs_data_release(settings);
                last_payload_ = payload;
            }
            set_visible(true);
    }

    void poll() noexcept {
        xen_obs::Model model;
        uint64_t last_configuration = 0;
        std::string cache_identity;
        Json reference = Json::object(), catalog = Json::array();
        Clock::time_point next_reference{};
        bool online = false;
        while (!stopping_) {
            Configuration config;
            { std::lock_guard lock(mutex_); config = configuration_; }
            expire_sync_show();
            process_sync(config);
            if (config.revision != last_configuration) {
                model.reset(); cache_identity.clear(); reference = Json::object(); catalog = Json::array();
                last_configuration = config.revision;
            }
            if (config.enabled) try {
                const auto target = endpoint(config.url);
                InternetHandle session(WinHttpOpen(L"XenObsOverlay/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
                if (!session.value) throw std::runtime_error("HTTP初始化失败");
                WinHttpSetTimeouts(session.value, 250, 250, 250, 250);
                InternetHandle connection(WinHttpConnect(session.value, target.host.c_str(), target.port, 0));
                if (!connection.value) throw std::runtime_error("HTTP连接初始化失败");
                // 同步端点单独失败不能重置道具模型/图片publication。
                try {
                    const auto sync_status=get_json(session.value,connection.value,"/sync-status.json");
                    sync_available_.store(sync_status.status==200 && xen_obs::sync_status_available(sync_status.document));
                } catch (...) { sync_available_.store(false); }
                const auto selection = get_json(session.value, connection.value, "/selection.json");
                if (selection.status != 200 || !selection.document.is_object())
                    throw std::runtime_error("辅机方案状态不可用");
                const auto id = string_field(selection.document, "actual_plan_id");
                const auto identity = string_field(selection.document, "generation") + "\n" + id;
                if (identity != cache_identity) {
                    reference = Json::object(); catalog = Json::array(); next_reference = {};
                    if (xen_obs::valid_plan_id(id)) {
                        auto list = get_json(session.value, connection.value, "/catalog.json");
                        if (list.status == 200 && list.document.is_array()) catalog = std::move(list.document);
                    }
                    cache_identity = identity;
                }
                if (xen_obs::valid_plan_id(id) && Clock::now() >= next_reference) {
                    auto resources = get_json(session.value, connection.value, "/reference.json?plan_id=" + xen_obs::percent_encode(id));
                    reference = resources.status == 200 && resources.document.is_object()
                        ? std::move(resources.document) : Json::object();
                    next_reference = Clock::now() + std::chrono::seconds(1);
                }
                auto next = std::make_shared<Publication>();
                next->view = model.update(selection.document, reference, catalog, target.base);
                next->selection = selection.document;
                next->received = Clock::now(); next->configuration = config.revision;
                { std::lock_guard lock(mutex_); if (configuration_.revision == config.revision) publication_ = std::move(next); }
                if (!online) { online = true; blog(LOG_INFO, "[xen-obs-overlay] 辅机只读状态已连接"); }
            } catch (const std::exception&) {
                model.reset();
                sync_available_.store(false);
                { std::lock_guard lock(mutex_); publication_.reset(); }
                if (online) { online = false; blog(LOG_WARNING, "[xen-obs-overlay] 辅机状态不可用，提示已隐藏，等待连接恢复"); }
            } catch (...) {
                model.reset();
                sync_available_.store(false);
                std::lock_guard lock(mutex_); publication_.reset();
            }
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return stopping_ || configuration_.revision != config.revision || sync_job_.has_value();
            });
        }
    }

    std::atomic<bool> stopping_{false};
    std::atomic<HWND> handle_{nullptr};
    bool started_ = false, visible_ = false;
    HINSTANCE instance_ = nullptr;
    obs_source_t* settings_source_ = nullptr;
    obs_source_t* browser_ = nullptr;
    obs_display_t* display_ = nullptr;
    std::string config_file_, page_url_, last_payload_;
    std::mutex mutex_;
    std::condition_variable condition_;
    Configuration configuration_;
    std::shared_ptr<const Publication> publication_;
    std::thread worker_;
    HWND menu_window_ = nullptr, return_window_ = nullptr, checked_foreground_ = nullptr, pending_capture_ = nullptr;
    HWND sync_window_ = nullptr;
    std::atomic<bool> sync_available_{false}, sync_inflight_{false};
    std::optional<SyncJob> sync_job_, sync_show_;
    std::optional<xen_obs::SyncShowPermit> sync_show_permit_;
    SyncJob active_sync_;
    std::string sync_source_session_, sync_failure_, sync_current_id_;
    std::wstring sync_text_;
    uint64_t sync_sequence_ = 0;
    bool sync_paint_pending_ = false;
    Clock::time_point sync_until_{};
    obs_source_t* menu_browser_ = nullptr;
    obs_display_t* menu_display_ = nullptr;
    bool menu_visible_ = false, foreground_is_game_ = false;
    std::array<bool,xen_obs::kHotkeys.size()> hotkeys_registered_{};
    std::array<bool,xen_obs::kHotkeys.size()> hotkey_error_logged_{};
    std::array<ATOM,xen_obs::kHotkeys.size()> hotkey_ids_{};
    std::string capture_role_ = "aim";
    uint64_t menu_configuration_ = 0;
    uint64_t menu_open_count_ = 0;
    uint32_t card_height_ = kHeight;
    xen_obs::MenuSelectionState menu_selection_;
    xen_obs::CaptureJobState capture_job_;
    HANDLE capture_process_ = nullptr;
    std::filesystem::path capture_status_;
    Clock::time_point capture_after_{}, notice_until_{};
    Json notice_;
};

const char* settings_name(void*) { return "Xen 道具提示"; }
void* settings_create(obs_data_t* settings, obs_source_t*) {
    if (manager) manager->configure(settings);
    return manager.get();
}
void settings_destroy(void*) {}
void settings_update(void* data, obs_data_t* settings) {
    if (data) static_cast<Manager*>(data)->configure(settings);
}
void settings_defaults(obs_data_t* settings) {
    obs_data_set_default_bool(settings, "enabled", true);
    obs_data_set_default_string(settings, "auxiliary_url", "http://127.0.0.1:5015");
}
obs_properties_t* settings_properties(void*) {
    auto* properties = obs_properties_create();
    auto* enabled = obs_properties_add_bool(properties, "enabled", "启用道具提示、采集和视频同步标记");
    obs_property_set_long_description(enabled, "小键盘右键开关道具菜单，PgUp/PgDn分别采集站位/瞄点。辅机Runtime和详细运行记录就绪且F7未被控制占用时，游戏前台Ctrl+F7显示两秒录像同步编号；不启停Runtime、不发送游戏输入。N卡需启用桌面录制，并先确认短样片录到编号。");
    auto* address = obs_properties_add_text(properties, "auxiliary_url", "辅机接口地址", OBS_TEXT_DEFAULT);
    obs_property_set_long_description(address, "填写辅机接口地址，例如http://192.168.3.20:5015。复用原地址读取道具和同步可用状态，提交点选方案及视频同步证据，不增加监听服务。");
    auto* python = obs_properties_add_path(properties, "capture_python", "采集Python", OBS_PATH_FILE, "Python (*.exe)", nullptr);
    obs_property_set_long_description(python, "使用主机已有Python 3，安装时自动填写；只在PgUp/PgDn采集作业期间启动，无语音模型依赖。");
    auto* root = obs_properties_add_path(properties, "capture_root", "采集本地目录", OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_property_set_long_description(root, "保留原图、自动命名资源及待发送记录；沿用主机cache/grenade。");
    auto* destination = obs_properties_add_text(properties, "capture_destination", "辅机资源目录", OBS_TEXT_DEFAULT);
    obs_property_set_long_description(destination, "辅机cache/grenade的已有共享路径；PgUp/PgDn自动打包同步，不需要单独启动采集程序。");
    return properties;
}
void settings_menu(void*) { if (manager) manager->open_settings(); }
void frontend_event(enum obs_frontend_event event, void*) {
    if (!manager) return;
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        try { manager->start(); }
        catch (const std::exception& error) { blog(LOG_ERROR, "[xen-obs-overlay] %s", error.what()); manager->stop(); }
    } else if (event == OBS_FRONTEND_EVENT_EXIT) manager->stop();
}
}

bool obs_module_load(void) {
    try {
        manager = std::make_unique<Manager>();
        obs_source_info info{};
        info.id = kSettingsSource; info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_DO_NOT_DUPLICATE;
        info.get_name = settings_name; info.create = settings_create; info.destroy = settings_destroy;
        info.update = settings_update; info.get_defaults = settings_defaults; info.get_properties = settings_properties;
        obs_register_source(&info);
        obs_frontend_add_tools_menu_item("Xen 道具提示", settings_menu, nullptr);
        obs_frontend_add_event_callback(frontend_event, nullptr);
        return true;
    } catch (...) { manager.reset(); return false; }
}

void obs_module_unload(void) {
    obs_frontend_remove_event_callback(frontend_event, nullptr);
    manager.reset();
}
