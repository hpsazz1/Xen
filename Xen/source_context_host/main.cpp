#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "source_context/source_context.h"
#include <atomic>
#include <charconv>
#include <iostream>
#include <string>

namespace {
std::atomic<bool> stop_requested{false};
BOOL WINAPI console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
bool number(const char* text, int& result) {
    const std::string value(text);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
void usage() {
    std::cout << "用法: XenSourceContext --enable --host IPv4 --port 端口 --process 游戏.exe [--ttl-ms 毫秒]\n"
              << "鉴权值仅从 XEN_SOURCE_CONTEXT_TOKEN 环境读取；默认不启动，不发送设备输入。\n";
}
}

int main(int argc, char** argv) {
    source_context::SourceContextConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--help" || argument == "-h") { usage(); return 0; }
        if (argument == "--enable") { config.enabled = true; continue; }
        if (i + 1 >= argc) { usage(); return 2; }
        if (argument == "--host") config.host = argv[++i];
        else if (argument == "--process") config.process_name = argv[++i];
        else if (argument == "--port") {
            int value = 0;
            if (!number(argv[++i], value) || value < 1 || value > 65535) { usage(); return 2; }
            config.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--ttl-ms") {
            if (!number(argv[++i], config.ttl_ms)) { usage(); return 2; }
        } else { usage(); return 2; }
    }
    if (!config.enabled) { usage(); return 0; }
    char token[1025]{};
    const DWORD length = GetEnvironmentVariableA("XEN_SOURCE_CONTEXT_TOKEN", token, sizeof(token));
    if (length < 32 || length >= sizeof(token)) {
        std::cerr << "源端状态鉴权环境缺失或长度无效。\n";
        return 2;
    }
    config.token.assign(token, length);
    SecureZeroMemory(token, sizeof(token));
    source_context::SourceContextServer server;
    if (!server.start(config)) { std::cerr << server.last_error() << '\n'; return 1; }
    SetConsoleCtrlHandler(console_event, TRUE);
    std::cout << "源端前台状态服务已启动；Ctrl+C 退出。\n";
    while (!stop_requested.load()) {
        if (!server.serve_once()) {
            std::cerr << "源端状态服务失败，已停止。\n";
            SetConsoleCtrlHandler(console_event, FALSE);
            return 1;
        }
    }
    server.stop();
    SetConsoleCtrlHandler(console_event, FALSE);
    return 0;
}
