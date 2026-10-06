#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR
#include "lineup/service.h"
#include "config/config.h"
#include "log/log.h"
#include "weapon/weapon.h"
#include <SimpleIni.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> stopping{false};
BOOL WINAPI console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stopping = true;
        return TRUE;
    }
    return FALSE;
}
struct Socket {
    SOCKET value = INVALID_SOCKET;
    explicit Socket(SOCKET v = INVALID_SOCKET) : value(v) {}
    ~Socket() {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
    Socket(const Socket &) = delete;
};
struct Options {
    std::string bind = "127.0.0.1", source, gsi_mode = "shared";
    std::filesystem::path data = "lineup-data", web, gsi_config, config_path;
    CaptureConfig capture;
    weapon::GsiConfig shared_config;
    int locate_virtual_key = 0, throw_virtual_key = 0;
    bool calibration_configured = false;
    std::string config_error, capture_error, size_origin = "fallback_unverified";
    bool config_loaded = false, check_config = false, version = false;
    int port = 8879, width = 1920, height = 1080, seconds = 0;
    bool synthetic = false, help = false;
};
bool integer(const std::string &s, int &n) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    return ec == std::errc{} && end == s.data() + s.size();
}
bool private_address(const std::string &s) {
    IN_ADDR a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1)
        return false;
    auto n = ntohl(a.s_addr);
    return (n >> 24) == 127 || (n >> 24) == 10 || (n >> 16) == 0xc0a8 || (n >> 20) == 0xac1 ||
           (n >> 16) == 0xa9fe;
}
Options parse(int argc, char **argv) {
    Options o;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    o.web = std::filesystem::path(path).parent_path() / "lineup-web";
    // 先读取同一权威配置，再应用显式旧命令行参数；不写回用户 INI。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc)
            o.config_path = std::filesystem::absolute(std::filesystem::u8path(argv[++i]));
    }
    if (!o.config_path.empty()) {
        AppConfig app;
        const auto utf8 = o.config_path.u8string();
        o.config_loaded = load_app_config(std::string(utf8.begin(), utf8.end()), app, o.config_error);
        if (o.config_loaded) {
            o.capture = app.capture;
            o.shared_config = app.gsi;
            o.locate_virtual_key = app.keyboard.lineup_locate_virtual_key;
            o.throw_virtual_key = app.keyboard.lineup_throw_virtual_key;
            o.calibration_configured = !app.lineup.calibration_file.empty() && !app.lineup.calibration_context.empty();
            o.source = app.capture.ndi_source_name;
            o.width = app.capture.ndi_source_width;
            o.height = app.capture.ndi_source_height;
            if (o.width > 0 && o.height > 0) o.size_origin = "shared_ini";
            if (o.width == 0) o.width = 1920;
            if (o.height == 0) o.height = 1080;
            if (app.capture.backend != CaptureBackend::NDI)
                o.capture_error = "共用配置的采集后端不是 NDI；仍可静态浏览";

        } else o.capture_error = o.config_error;
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check-config") { o.check_config = true; continue; }
        if (a == "--version") { o.version = true; continue; }
        if (a == "--synthetic") {
            o.synthetic = true;
            o.source.clear();
            continue;
        }
        if (a == "--help") {
            o.help = true;
            continue;
        }
        if (++i >= argc)
            throw std::runtime_error("参数缺少值");
        std::string v = argv[i];
        if (a == "--config")
            continue;
        else if (a == "--bind")
            o.bind = v;
        else if (a == "--data")
            o.data = std::filesystem::u8path(v);
        else if (a == "--web")
            o.web = std::filesystem::u8path(v);
        else if (a == "--ndi-source")
            o.source = v;
        else if (a == "--gsi-mode")
            o.gsi_mode = v;
        else if (a == "--gsi-config")
            o.gsi_config = std::filesystem::u8path(v);
        else if (a == "--port" && integer(v, o.port)) {
        } else if (a == "--source-width" && integer(v, o.width)) {
            o.size_origin = "legacy_override";
        } else if (a == "--source-height" && integer(v, o.height)) {
            o.size_origin = "legacy_override";
        } else if (a == "--max-seconds" && integer(v, o.seconds)) {
        } else
            throw std::runtime_error("未知或无效参数: " + a);
    }
    if (!private_address(o.bind) || o.port < 1024 || o.port > 65535 || o.width < 64 ||
        o.height < 64 || o.width > 7680 || o.height > 4320 || o.seconds < 0 ||
        (o.synthetic && !o.source.empty()))
        throw std::runtime_error("地址/尺寸/源参数无效；仅绑定明确回环或私网 IPv4");
    if (o.gsi_mode != "shared" && o.gsi_mode != "direct") throw std::runtime_error("GSI mode must be shared or direct");
    return o;
}
weapon::GsiConfig read_gsi_config(const Options &options) {
    weapon::GsiConfig config = options.shared_config;
    if (!options.gsi_config.empty()) {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(options.gsi_config.c_str()) < 0)
            throw std::runtime_error("无法读取指定的 GSI INI 文件");
        config.enabled = ini.GetBoolValue("gsi", "enabled", false);
        config.bind_address = ini.GetValue("gsi", "bind_address", "127.0.0.1");
        config.allowed_peer_ipv4 = ini.GetValue("gsi", "allowed_peer_ipv4", "");
        const long port = ini.GetLongValue("gsi", "port", 5013);
        if (port < 1 || port > 65535 || (options.gsi_mode == "direct" && port == options.port))
            throw std::runtime_error("GSI 端口非法或与网页端口冲突；未更改配置");
        config.port = static_cast<std::uint16_t>(port);
        config.ttl_ms = static_cast<int>(ini.GetLongValue("gsi", "ttl_ms", config.ttl_ms));
        config.request_timeout_ms = static_cast<int>(ini.GetLongValue("gsi", "request_timeout_ms", config.request_timeout_ms));
        config.max_clock_skew_ms = static_cast<int>(ini.GetLongValue("gsi", "max_clock_skew_ms", config.max_clock_skew_ms));
    }
    if (options.gsi_mode == "direct" && config.enabled && config.port == options.port)
        throw std::runtime_error("GSI 端口与网页端口冲突；未更改配置");
    if (!private_address(config.bind_address) ||
        (!config.allowed_peer_ipv4.empty() && !private_address(config.allowed_peer_ipv4)) ||
        (config.enabled && !weapon::valid_config(config)))
        throw std::runtime_error("GSI 配置无效；独立服务仅使用明确的回环或私网地址，不自动修改已有配置");
    return config;
}
nlohmann::json startup_state(const Options &o) {
    const auto path = o.config_path.u8string();
    const auto legacy = o.gsi_config.u8string();
    return {{"version", "lineup-increment-20261005-2"},
            {"config_path", std::string(path.begin(), path.end())},
            {"config_loaded", o.config_loaded}, {"config_error", o.config_error},
            {"source", o.synthetic ? "synthetic-test-fixture" : o.source},
            {"source_width", o.width}, {"source_height", o.height},
            {"size_origin", o.size_origin},
            {"capture_error", o.synthetic ? "" : o.capture_error},
            {"geometry_mode", o.synthetic ? "synthetic" : (o.config_loaded ? "shared_capture_roi" : "legacy_full_frame")},
            {"geometry_limit", o.config_loaded ? "仅使用当前采集 ROI；配置几何不是实际传输尺寸或全屏位置证明" : "旧全帧入口要求合法 metadata"},
            {"configured_roi_width", o.config_loaded ? o.capture.roi_width : o.width},
            {"configured_roi_height", o.config_loaded ? o.capture.roi_height : o.height},
            {"gsi_mode", o.gsi_mode},
            {"locate_virtual_key", o.locate_virtual_key},
            {"throw_virtual_key", o.throw_virtual_key},
            {"throw_hotkey_status", o.throw_virtual_key ? "requires_running_runtime" : "unbound"},
            {"calibration_configured", o.calibration_configured},
            {"calibration_note", "configured does not mean measured or validated; Runtime verifies evidence"},
            {"locate_hotkey_status", o.locate_virtual_key ? "requires_running_runtime" : "unbound"},
            {"gsi_config_path", legacy.empty() ? std::string(path.begin(), path.end()) : std::string(legacy.begin(), legacy.end())},
            {"clock_sync_url", o.config_loaded ? o.capture.ndi_clock_sync_url : ""}};
}
std::string read_file(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("文件不可读");
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void send_all(SOCKET s, const char *p, size_t size) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (size && !stopping) {
        if (Clock::now() > deadline)
            return;
        int n = send(s, p, static_cast<int>(std::min<size_t>(size, 65536)), 0);
        if (n <= 0)
            return;
        p += n;
        size -= n;
    }
}
void reply(SOCKET s, int code, const std::string &type, const std::string &body) {
    std::string header =
        "HTTP/1.1 " + std::to_string(code) + " Result\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self' blob:; style-src 'self'; "
        "script-src 'self'; connect-src 'self'; frame-ancestors 'none'\r\n\r\n";
    send_all(s, header.data(), header.size());
    send_all(s, body.data(), body.size());
}
struct Request {
    std::string method, target, body;
    std::map<std::string, std::string> headers;
};
bool read_request(SOCKET s, Request &r) {
    std::string buffer;
    char chunk[4096];
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    size_t end = std::string::npos;
    while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (Clock::now() > deadline || buffer.size() > 8192)
            return false;
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0)
            return false;
        buffer.append(chunk, n);
    }
    if (end > 8192)
        return false;
    std::istringstream lines(buffer.substr(0, end));
    std::string line, version, extra;
    if (!std::getline(lines, line))
        return false;
    std::istringstream first(line);
    first >> r.method >> r.target >> version;
    if ((first >> extra) || version != "HTTP/1.1" || r.target.size() > 1024)
        return false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            return false;
        std::string key = line.substr(0, colon), value = line.substr(colon + 1);
        const std::string punctuation = "!#$%&'*+-.^_`|~";
        if (!std::all_of(key.begin(), key.end(), [&](unsigned char c) {
                return std::isalnum(c) || punctuation.find(c) != std::string::npos;
            }))
            return false;
        if (std::any_of(value.begin(), value.end(),
                        [](unsigned char c) { return (c < 32 && c != '\t') || c == 127; }))
            return false;
        for (char &c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        if (r.headers.contains(key))
            return false;
        r.headers[key] = value;
    }
    if (!r.headers.contains("host") || r.headers.contains("transfer-encoding"))
        return false;
    if (r.headers["host"].empty() || r.headers["host"].find_first_of(" \t/\\") != std::string::npos)
        return false;
    int length = 0;
    if (r.headers.contains("content-length") && !integer(r.headers["content-length"], length))
        return false;
    if (length < 0 || length > 65536)
        return false;
    r.body = buffer.substr(end + 4);
    while (r.body.size() < static_cast<size_t>(length)) {
        if (Clock::now() > deadline)
            return false;
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0)
            return false;
        r.body.append(chunk, n);
    }
    return r.body.size() == static_cast<size_t>(length);
}
void handle(SOCKET socket, lineup::Service &service, const Options &o) {
    try {
        Request r;
        if (!read_request(socket, r)) {
            reply(socket, 400, "text/plain; charset=utf-8", "请求无效");
            return;
        }
        if (r.method == "POST" && r.target == "/api/command") {
            if (r.headers["content-type"] != "application/json" ||
                (r.headers.contains("origin") &&
                 r.headers["origin"] != "http://" + r.headers["host"])) {
                reply(socket, 400, "text/plain; charset=utf-8", "请求来源或格式无效");
                return;
            }
            auto result = service.command(nlohmann::json::parse(r.body));
            reply(socket, result.status, "application/json; charset=utf-8", result.json.dump());
            return;
        }
        if (r.method != "GET") {
            reply(socket, 405, "text/plain", "method");
            return;
        }
        if (r.target == "/api/state") {
            auto state = service.state();
            state["startup"] = startup_state(o);
            reply(socket, 200, "application/json; charset=utf-8", state.dump());
            return;
        }
        if (r.target.starts_with("/api/preview?frame_id=")) {
            auto bytes =
                service.preview(r.target.substr(std::string("/api/preview?frame_id=").size()));
            if (bytes)
                reply(socket, 200, "image/jpeg", {bytes->begin(), bytes->end()});
            else
                reply(socket, 410, "text/plain", "expired");
            return;
        }
        if (r.target.starts_with("/api/reference/")) {
            auto bytes = service.reference(r.target.substr(std::string("/api/reference/").size()));
            if (bytes)
                reply(socket, 200, "image/png", {bytes->begin(), bytes->end()});
            else
                reply(socket, 404, "text/plain", "not found");
            return;
        }
        if (r.target.starts_with("/api/standpoint/")) {
            auto bytes = service.reference("standpoint:" +
                                           r.target.substr(std::string("/api/standpoint/").size()));
            if (bytes)
                reply(socket, 200, "image/png", {bytes->begin(), bytes->end()});
            else
                reply(socket, 404, "text/plain", "not found");
            return;
        }
        const std::map<std::string, std::pair<std::string, std::string>> files{
            {"/", {"index.html", "text/html; charset=utf-8"}},
            {"/index.html", {"index.html", "text/html; charset=utf-8"}},
            {"/app.js", {"app.js", "text/javascript; charset=utf-8"}},
            {"/style.css", {"style.css", "text/css; charset=utf-8"}}};
        auto it = files.find(r.target);
        if (it == files.end()) {
            reply(socket, 404, "text/plain", "not found");
            return;
        }
        reply(socket, 200, it->second.second, read_file(o.web / it->second.first));
    } catch (const std::exception &e) {
        reply(socket, 400, "application/json; charset=utf-8",
              nlohmann::json{{"error", e.what()}}.dump());
    }
}
cv::Mat synthetic_image() {
    cv::Mat image(720, 1280, CV_8UC3, cv::Scalar(35, 40, 45));
    cv::RNG rng(20261005);
    for (int i = 0; i < 600; ++i) {
        cv::Point p(rng.uniform(25, 1250), rng.uniform(25, 690));
        cv::Scalar c(rng.uniform(70, 250), rng.uniform(70, 250), rng.uniform(70, 250));
        cv::circle(image, p, rng.uniform(3, 15), c, rng.uniform(1, 4));
    }
    cv::putText(image, "SYNTHETIC TEST FIXTURE - NOT GAME DATA", {45, 55}, cv::FONT_HERSHEY_SIMPLEX,
                .8, {255, 255, 255}, 2);
    return image;
}
void capture_loop(lineup::Service &service, const Options &o, std::stop_token token) {
    if (o.synthetic) {
        const auto image = synthetic_image();
        uint64_t sequence = 0;
        while (!stopping && !token.stop_requested()) {
            CapturedFrame f;
            f.bgr = image;
            f.width = f.source_width = f.encoded_width = image.cols;
            f.height = f.source_height = f.encoded_height = image.rows;
            f.timing.sequence = ++sequence;
            f.timing.captured_at = Clock::now();
            service.submit(f, "synthetic-test-fixture");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }
    if (!o.capture_error.empty()) {
        service.disconnect(o.capture_error);
        return;
    }
    if (o.source.empty()) {
        service.disconnect("未配置 NDI 源；静态浏览可用");
        return;
    }
    CaptureConfig config = o.config_loaded ? o.capture : CaptureConfig{};
    config.backend = CaptureBackend::NDI;
    config.ndi_source_name = o.source;
    if (!o.config_loaded) {
        // 旧命令行全帧入口保留；共享配置入口原样沿用现有低负载 ROI 几何。
        config.ndi_source_width = 0;
        config.ndi_source_height = 0;
        config.ndi_frame_layout = NetworkFrameLayout::FULL_FRAME_1_TO_1;
        config.ndi_require_frame_metadata = true;
        config.roi_width = o.width;
        config.roi_height = o.height;
        config.center_roi = false;
        config.roi_x = config.roi_y = 0;
        config.ndi_discovery_timeout_ms = 1000;
        config.ndi_receive_timeout_ms = 50;
        config.ndi_disconnect_timeout_ms = 1000;
    }
    uint64_t generation = 0;
    while (!stopping && !token.stop_requested()) {
        if (!service.wants_input()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        auto capture = create_capture(config);
        if (!capture || !capture->open()) {
            service.disconnect(capture ? capture->last_error() : "采集创建失败");
            for (int i = 0; i < 10 && !stopping && !token.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const std::string session = "ndi-" + std::to_string(++generation);
        auto last = Clock::now();
        while (!stopping && !token.stop_requested()) {
            if (!service.wants_input())
                break;
            CapturedFrame frame;
            auto status = capture->grab(frame);
            if (status == CaptureStatus::FRAME) {
                if (o.size_origin != "fallback_unverified" &&
                    (frame.source_width != o.width || frame.source_height != o.height)) {
                    service.disconnect("源几何变化，本次参考不可用");
                } else
                    service.submit(frame, session + "-receiver-" +
                                              std::to_string(frame.timing.receiver_generation));
                last = Clock::now();
            } else if (status != CaptureStatus::NO_FRAME && status != CaptureStatus::READY) {
                service.disconnect(capture->last_error());
                break;
            } else if (Clock::now() - last > std::chrono::seconds(1)) {
                service.disconnect("源断流");
                break;
            }
        }
        capture->close();
    }
}
} // namespace
int wmain(int argc, wchar_t **wide_argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 0; i < argc; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                                 nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                throw std::runtime_error("参数编码无效");
            std::string value(size, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, value.data(), size,
                                nullptr, nullptr);
            value.pop_back();
            arguments.push_back(std::move(value));
        }
        std::vector<char *> pointers;
        for (auto &a : arguments)
            pointers.push_back(a.data());
        char **argv = pointers.data();
        const auto o = parse(argc, argv);
        if (o.version) { std::cout << "lineup-increment-20261005-2\n"; return 0; }
        if (o.check_config) {
            auto state = startup_state(o);
            std::string gsi_error;
            try { (void)read_gsi_config(o); } catch (const std::exception &e) { gsi_error = e.what(); }
            state["gsi_config_error"] = gsi_error;
            std::cout << state.dump() << '\n';
            return o.config_error.empty() && state["gsi_config_error"] == "" ? 0 : 1;
        }
        if (o.help) {
            std::cout << "XenLineup 手机配方练习服务，无自动游戏输入\n"
                         "--config <existing Xen INI> --check-config --version (read only)\n"
                         "--data <dir> --bind <private IPv4, default 127.0.0.1> --port <8879>\n"
                         "--ndi-source <name> --source-width <1920> --source-height <1080>\n"
                         "--gsi-config <existing INI> 读取 [gsi]；默认只读 Runtime 本地上下文，不绑定 GSI 端口\n"
                         "--gsi-mode <shared|direct>；direct 仅用于显式独立接收端口。旧命令行全帧模式要求 Xen metadata；--config 沿用现有 ROI。\n"
                         "--synthetic 仅合成行为测试；--max-seconds <0=until Ctrl+C>\n";
            return 0;
        }
        std::cout << startup_state(o).dump() << '\n';
        LogConfig log_config;
        log_config.enable_file = false;
        log_config.global_level = LogLevel::INFO;
        Log::init(log_config);
        Log::register_module("Lineup");
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("Winsock 初始化失败");
        SetConsoleCtrlHandler(console_event, TRUE);
        {
            lineup::Service service(o.data,
                                    o.config_loaded && !o.synthetic ? lineup::ReferenceMode::ROI
                                                                    : lineup::ReferenceMode::FULL_FRAME,
                                    o.config_loaded && !o.synthetic ? o.source : "");
            weapon::GsiReceiver gsi;
            weapon::GsiContextReader shared_gsi;
            weapon::GsiConfig gsi_config;
            std::string gsi_error;
            bool gsi_ready = false;
            try {
                gsi_config = read_gsi_config(o);
                if ((gsi_config.enabled || o.locate_virtual_key != 0) && o.gsi_mode == "shared") shared_gsi.configure(gsi_config);
                if (gsi_config.enabled && o.gsi_mode == "direct") {
                    gsi_ready = gsi.start(gsi_config);
                    if (!gsi_ready) gsi_error = gsi.last_error() + "；端口不可用，请改用 shared 只读模式或已配置的独立端口。当前仍可手动筛选。";
                }
            } catch (const std::exception &e) { gsi_error = e.what(); }
            std::jthread context([&](std::stop_token token) {
                while(!stopping && !token.stop_requested()) {
                    auto snapshot = gsi_ready ? gsi.snapshot() : weapon::WeaponSnapshot{};
                    if (!gsi_ready) snapshot.status = gsi_error.empty() ? weapon::Status::DISABLED : weapon::Status::UNAVAILABLE;
                    if ((gsi_config.enabled || o.locate_virtual_key != 0) && o.gsi_mode == "shared") {
                        snapshot = shared_gsi.snapshot();
                        service.update_gsi(snapshot, shared_gsi.last_error());
                        if (shared_gsi.consume_lineup_locate()) service.request_locate();
                    } else service.update_gsi(snapshot, gsi_error);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            lineup::control::Client execution_client;
            const bool execution_enabled = o.config_loaded && !o.synthetic && o.gsi_mode == "shared" && o.config_error.empty();
            if (execution_enabled) execution_client.start(lineup::control::channel_key(gsi_config));
            std::jthread execution([&](std::stop_token token) {
                bool was_connected = false;
                std::uint64_t last_sequence = 0, last_generation = 0;
                while (!stopping && !token.stop_requested()) {
                    const bool connected = execution_enabled && execution_client.connected();
                    if (connected != was_connected) {
                        service.reset_control(connected ? "runtime_connected_new_locate_required" : "runtime_disconnected");
                        last_sequence = last_generation = 0; was_connected = connected;
                    }
                    if (connected) {
                        service.set_execution_status(execution_client.status());
                        auto request = service.control_request();
                        if (request.mode == lineup::control::Mode::CANCEL || request.mode == lineup::control::Mode::LOCATE ||
                            request.observation.sequence != last_sequence || request.observation.identity.selection_generation != last_generation) {
                            if (!execution_client.publish(request)) service.reset_control("runtime_disconnected");
                            else { last_sequence = request.observation.sequence; last_generation = request.observation.identity.selection_generation; }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (execution_client.connected()) execution_client.publish({});
                execution_client.stop();
            });
            Socket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (listener.value == INVALID_SOCKET)
                throw std::runtime_error("socket 失败");
            BOOL exclusive = TRUE;
            setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<char *>(&exclusive), sizeof(exclusive));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(o.port));
            inet_pton(AF_INET, o.bind.c_str(), &address.sin_addr);
            if (bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                    SOCKET_ERROR ||
                listen(listener.value, 8) == SOCKET_ERROR)
                throw std::runtime_error("绑定失败；不自动更改防火墙或网络设置");
            LOG_INFO("Lineup", "服务 http://{}:{}；样本模式 {}", o.bind, o.port,
                     o.synthetic ? "SYNTHETIC" : "NDI/静态浏览");
            std::jthread capture([&](std::stop_token token) {
                try {
                    capture_loop(service, o, token);
                } catch (const std::exception &e) {
                    service.disconnect(e.what());
                }
            });
            const auto started = Clock::now();
            while (!stopping) {
                if (o.seconds && Clock::now() - started > std::chrono::seconds(o.seconds)) {
                    stopping = true;
                    break;
                }
                fd_set set;
                FD_ZERO(&set);
                FD_SET(listener.value, &set);
                timeval timeout{0, 100000};
                if (select(0, &set, nullptr, nullptr, &timeout) <= 0)
                    continue;
                Socket client(accept(listener.value, nullptr, nullptr));
                if (client.value == INVALID_SOCKET)
                    continue;
                DWORD milliseconds = 500;
                setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<char *>(&milliseconds), sizeof(milliseconds));
                setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<char *>(&milliseconds), sizeof(milliseconds));
                handle(client.value, service, o);
            }
            stopping = true;
            capture.join();
            context.request_stop();
            context.join();
            gsi.stop();
            service.close();
        }
        WSACleanup();
        Log::shutdown();
        return 0;
    } catch (const std::exception &e) {
        stopping = true;
        std::cerr << e.what() << '\n';
        Log::shutdown();
        return 1;
    }
}
