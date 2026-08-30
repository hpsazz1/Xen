#include "app/release_contract_internal.h"
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
    expect(log_failure_is_owned() &&
               overlay_init_failure_is_owned() &&
               overlay_render_failure_is_owned() &&
               overlay_present_error_is_copied_into_visible_terminal() &&
               normal_and_restart_exits_are_preserved(),
           "GUI 生产启动边界必须把 Log/Overlay init/render 失败映射为"
           "固定非零退出和一次用户可见错误，并保留正常/重启终局");
    if (failures != 0) {
        std::cerr << "App 启动边界测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "App 启动边界测试全部通过。\n";
    return 0;
}
