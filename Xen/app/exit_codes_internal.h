#ifndef APP_EXIT_CODES_INTERNAL_H
#define APP_EXIT_CODES_INTERNAL_H

#include <cstdint>

namespace app::detail {

// Launcher 与主程序共享的重启终局；纯启动边界测试无需引入 Provider 类型。
inline constexpr std::uint32_t kReleaseRestartExitCode = 42;

} // namespace app::detail

#endif // APP_EXIT_CODES_INTERNAL_H
