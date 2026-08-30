#ifndef APP_STARTUP_INTERNAL_H
#define APP_STARTUP_INTERNAL_H

#include <cstdint>
#include <string>

namespace app::detail {

inline constexpr int kAppStartupFailureExitCode = 1;

enum class AppStartupFailure : std::uint8_t {
    NONE,
    LOG_INITIALIZATION,
    OVERLAY_INITIALIZATION,
    OVERLAY_RENDER,
};

struct AppStartupErrorAdapter {
    void* context = nullptr;
    void (*show_error)(void* context, const std::string& message) = nullptr;
};

// wWinMain 把真实 Log/Overlay 结果交给此 owner；它只拥有进程终局与一次性
// 用户可见错误，不拥有 Runtime、窗口或日志资源的销毁顺序。
class AppStartupBoundary final {
public:
    explicit AppStartupBoundary(AppStartupErrorAdapter adapter) noexcept;

    bool observe_log_initialization(bool initialized) noexcept;
    bool observe_overlay_initialization(bool initialized) noexcept;
    bool observe_overlay_render(
        bool rendered, const std::string& detail) noexcept;

    int finish(int successful_exit_code) noexcept;
    AppStartupFailure failure() const noexcept { return failure_; }

private:
    bool observe(bool succeeded, AppStartupFailure failure) noexcept;
    void present_failure() noexcept;

    AppStartupErrorAdapter adapter_;
    AppStartupFailure failure_ = AppStartupFailure::NONE;
    std::string owned_failure_message_;
    bool failure_presented_ = false;
};

} // namespace app::detail

#endif // APP_STARTUP_INTERNAL_H
