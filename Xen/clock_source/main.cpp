#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

#include "clock_sync/clock_sync.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

std::atomic<bool> g_stop_requested{false};

BOOL WINAPI handle_console_event(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        g_stop_requested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

void print_usage() {
    std::cout << "用法: XenClockSource [--bind udp://IPv4:port]\n";
}

} // namespace

int main(int argc, char** argv) {
    clock_sync::ServerConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--bind" && index + 1 < argc) {
            config.bind_url = argv[++index];
            continue;
        }
        std::cerr << "未知或不完整参数: " << argument << '\n';
        print_usage();
        return 2;
    }

    clock_sync::Server server;
    if (!server.open(config)) {
        std::cerr << "源机时钟旁路启动失败: " << server.last_error() << '\n';
        return 1;
    }
    SetConsoleCtrlHandler(handle_console_event, TRUE);
    std::cout << "源机时钟旁路已监听 " << config.bind_url
              << "；仅返回时间证据，不接触 NDI 图像或输入设备。\n";
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        if (!server.serve_once(100)) {
            std::cerr << "源机时钟旁路失败: " << server.last_error() << '\n';
            SetConsoleCtrlHandler(handle_console_event, FALSE);
            return 1;
        }
    }
    server.close();
    SetConsoleCtrlHandler(handle_console_event, FALSE);
    return 0;
}
