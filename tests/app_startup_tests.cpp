#include "app/exit_codes_internal.h"
#include "app/startup_internal.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

struct ErrorCapture {
    int call_count = 0;
    std::string message;
};

void capture_error(void* context, const std::string& message) {
    auto& capture = *static_cast<ErrorCapture*>(context);
    ++capture.call_count;
    capture.message = message;
}

app::detail::AppStartupErrorAdapter adapter_for(
        ErrorCapture& capture) noexcept {
    return {&capture, capture_error};
}

bool log_failure_is_owned() {
    ErrorCapture error;
    app::detail::AppStartupBoundary boundary(adapter_for(error));
    return !boundary.observe_log_initialization(false) &&
           boundary.finish(0) == app::detail::kAppStartupFailureExitCode &&
           error.call_count == 1 &&
           error.message == "日志系统初始化失败，Xen 无法继续启动。";
}

bool overlay_init_failure_is_owned() {
    ErrorCapture error;
    app::detail::AppStartupBoundary boundary(adapter_for(error));
    return boundary.observe_log_initialization(true) &&
           !boundary.observe_overlay_initialization(false) &&
           boundary.finish(0) == app::detail::kAppStartupFailureExitCode &&
           error.call_count == 1 &&
           error.message == "Overlay 初始化失败，Xen 无法创建主窗口。";
}

bool overlay_render_failure_is_owned() {
    ErrorCapture error;
    app::detail::AppStartupBoundary boundary(adapter_for(error));
    const bool observed =
        boundary.observe_log_initialization(true) &&
        boundary.observe_overlay_initialization(true) &&
        !boundary.observe_overlay_render(false, {});
    const int exit_code = boundary.finish(0);
    const int repeated_exit_code = boundary.finish(0);
    return observed &&
           exit_code == app::detail::kAppStartupFailureExitCode &&
           repeated_exit_code == app::detail::kAppStartupFailureExitCode &&
           error.call_count == 1 &&
           error.message == "Overlay 渲染失败，Xen 已安全停止。";
}

bool overlay_present_error_is_copied_into_visible_terminal() {
    ErrorCapture error;
    app::detail::AppStartupBoundary boundary(adapter_for(error));
    std::string present_error =
        "IDXGISwapChain::Present 失败，HRESULT=0x887A0005 "
        "(DXGI_ERROR_DEVICE_REMOVED)。";
    const bool observed =
        boundary.observe_log_initialization(true) &&
        boundary.observe_overlay_initialization(true) &&
        !boundary.observe_overlay_render(false, present_error);
    present_error = "调用方错误缓冲已被覆盖";
    const int exit_code = boundary.finish(0);
    const int repeated_exit_code = boundary.finish(0);
    return observed &&
           exit_code == app::detail::kAppStartupFailureExitCode &&
           repeated_exit_code == app::detail::kAppStartupFailureExitCode &&
           error.call_count == 1 &&
           error.message ==
               "Overlay 渲染失败，Xen 已安全停止。原因："
               "IDXGISwapChain::Present 失败，HRESULT=0x887A0005 "
               "(DXGI_ERROR_DEVICE_REMOVED)。";
}

bool normal_and_restart_exits_are_preserved() {
    ErrorCapture error;
    app::detail::AppStartupBoundary boundary(adapter_for(error));
    return boundary.observe_log_initialization(true) &&
           boundary.observe_overlay_initialization(true) &&
           boundary.observe_overlay_render(true, {}) &&
           boundary.finish(0) == 0 &&
           boundary.finish(
               static_cast<int>(app::detail::kReleaseRestartExitCode)) ==
               static_cast<int>(app::detail::kReleaseRestartExitCode) &&
           error.call_count == 0;
}

} // namespace

int main() {
    expect(log_failure_is_owned(), "Log 初始化失败必须产生一次可见错误和失败退出");
    expect(overlay_init_failure_is_owned(), "Overlay 初始化失败必须归属启动边界");
    expect(overlay_render_failure_is_owned(), "重复结束不能重复展示渲染失败");
    expect(overlay_present_error_is_copied_into_visible_terminal(),
           "渲染错误详情必须由边界持有，不能引用已覆盖的调用方缓冲");
    expect(normal_and_restart_exits_are_preserved(), "正常与重启退出码必须保留");
    if (failures != 0) {
        std::cerr << "App 启动边界测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "App 启动边界五项测试全部通过。\n";
    return 0;
}
