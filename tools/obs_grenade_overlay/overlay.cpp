// SPDX-License-Identifier: GPL-2.0-or-later
// OBS负责图片、文字和标记的GPU绘制；Win32仅管理桌面窗口的位置和输入行为。
#include <windows.h>
#include <winhttp.h>
#ifdef ERROR
#undef ERROR
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "model.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
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
constexpr UINT kApplySettings = WM_APP + 21;
constexpr wchar_t kWindowClass[] = L"XenObsGrenadeReference";
constexpr char kSettingsSource[] = "xen_grenade_overlay_settings";

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

HttpReply get_json(HINTERNET session, HINTERNET connection, const std::string& path) {
    const auto request_path = wide(path);
    InternetHandle request(WinHttpOpenRequest(connection, L"GET", request_path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request.value) throw std::runtime_error("建立HTTP请求失败");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr))
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
    auto document = Json::parse(bytes, nullptr, false);
    if (document.is_discarded()) document = Json::object();
    (void)session;
    return {status, std::move(document)};
}

struct Configuration {
    bool enabled = true;
    std::string url = "http://127.0.0.1:5015";
    uint64_t revision = 0;
};

struct Publication {
    xen_obs::View view;
    Clock::time_point received{};
    uint64_t configuration = 0;
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
        if (!RegisterClassExW(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("OBS提示窗口注册失败");
        instance_ = instance;
        const auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_LAYERED | WS_EX_TRANSPARENT, kWindowClass, L"Xen OBS 道具提示", WS_POPUP,
            0, 0, kWidth, kHeight, nullptr, nullptr, instance_, this);
        if (!window) throw std::runtime_error("OBS提示窗口创建失败");
        handle_.store(window);
        SetLayeredWindowAttributes(window, 0, 230, LWA_ALPHA);
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
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        Manager* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
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
        gs_ortho(0.0f, static_cast<float>(kWidth), 0.0f, static_cast<float>(kHeight), -100.0f, 100.0f);
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
                         desktop.top + 16, kWidth, kHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(window, SW_SHOWNOACTIVATE);
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
            if (!config.enabled || !current || current->configuration != config.revision ||
                Clock::now() - current->received > std::chrono::milliseconds(1200) || !current->view.visible) {
                set_visible(false); return;
            }
            const auto payload = current->view.payload.dump();
            if (payload != last_payload_) {
                set_visible(false);
                obs_data_t* settings = obs_source_get_settings(browser_);
                obs_data_set_string(settings, "url", (page_url_ + "?payload=" + xen_obs::percent_encode(payload)).c_str());
                obs_source_update(browser_, settings); obs_data_release(settings);
                last_payload_ = payload;
            }
            set_visible(true);
        } catch (...) { set_visible(false); }
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
                next->received = Clock::now(); next->configuration = config.revision;
                { std::lock_guard lock(mutex_); if (configuration_.revision == config.revision) publication_ = std::move(next); }
                if (!online) { online = true; blog(LOG_INFO, "[xen-obs-overlay] 辅机只读状态已连接"); }
            } catch (const std::exception&) {
                model.reset();
                { std::lock_guard lock(mutex_); publication_.reset(); }
                if (online) { online = false; blog(LOG_WARNING, "[xen-obs-overlay] 辅机状态不可用，提示已隐藏，等待连接恢复"); }
            } catch (...) {
                model.reset();
                std::lock_guard lock(mutex_); publication_.reset();
            }
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return stopping_ || configuration_.revision != config.revision;
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
    auto* enabled = obs_properties_add_bool(properties, "enabled", "显示游戏右上角参考图");
    obs_property_set_long_description(enabled, "开启后只在手机重新点选方案时显示站位和瞄点；对齐完成、开始投掷、取消或断线时隐藏，旧选择不会再次弹出。");
    auto* address = obs_properties_add_text(properties, "auxiliary_url", "辅机道具网址", OBS_TEXT_DEFAULT);
    obs_property_set_long_description(address, "填写辅机HTTP地址，例如http://192.168.3.20:5015。只读取已有方案和参考图，修改后立即重连并保存。");
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
