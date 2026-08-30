#include "app/startup_internal.h"

namespace app::detail {
namespace {

const char* failure_message(AppStartupFailure failure) noexcept {
    switch (failure) {
        case AppStartupFailure::LOG_INITIALIZATION:
            return "日志系统初始化失败，Xen 无法继续启动。";
        case AppStartupFailure::OVERLAY_INITIALIZATION:
            return "Overlay 初始化失败，Xen 无法创建主窗口。";
        case AppStartupFailure::OVERLAY_RENDER:
            return "Overlay 渲染失败，Xen 已安全停止。";
        case AppStartupFailure::NONE:
            break;
    }
    return "Xen 启动发生未知错误。";
}

} // namespace

AppStartupBoundary::AppStartupBoundary(
        AppStartupErrorAdapter adapter) noexcept
    : adapter_(adapter) {}

bool AppStartupBoundary::observe_log_initialization(
        bool initialized) noexcept {
    return observe(initialized, AppStartupFailure::LOG_INITIALIZATION);
}

bool AppStartupBoundary::observe_overlay_initialization(
        bool initialized) noexcept {
    return observe(
        initialized, AppStartupFailure::OVERLAY_INITIALIZATION);
}

bool AppStartupBoundary::observe_overlay_render(
        bool rendered, const std::string& detail) noexcept {
    if (!rendered && failure_ == AppStartupFailure::NONE &&
        !detail.empty()) {
        try {
            owned_failure_message_ =
                "Overlay 渲染失败，Xen 已安全停止。原因：" + detail;
        } catch (...) {
            owned_failure_message_.clear();
        }
    }
    return observe(rendered, AppStartupFailure::OVERLAY_RENDER);
}

int AppStartupBoundary::finish(int successful_exit_code) noexcept {
    if (failure_ == AppStartupFailure::NONE) return successful_exit_code;
    present_failure();
    return kAppStartupFailureExitCode;
}

bool AppStartupBoundary::observe(
        bool succeeded, AppStartupFailure failure) noexcept {
    if (succeeded) return true;
    if (failure_ == AppStartupFailure::NONE) failure_ = failure;
    return false;
}

void AppStartupBoundary::present_failure() noexcept {
    if (failure_presented_ || !adapter_.show_error) return;
    failure_presented_ = true;
    try {
        if (!owned_failure_message_.empty()) {
            adapter_.show_error(adapter_.context, owned_failure_message_);
        } else {
            adapter_.show_error(
                adapter_.context, failure_message(failure_));
        }
    } catch (...) {
    }
}

} // namespace app::detail
