#ifndef RUNTIME_STARTUP_INTERNAL_H
#define RUNTIME_STARTUP_INTERNAL_H

#include "runtime/runtime.h"

#include <functional>

namespace runtime::detail {

// 仅替换外部采集边界及启动发布屏障；各实例独立持有，不改变默认生产路径。
struct StartupAdapter {
    std::function<std::unique_ptr<ICapture>(const CaptureConfig&)> create_capture;
    std::function<void(Runtime&)> before_running;
};

} // namespace runtime::detail

#endif
