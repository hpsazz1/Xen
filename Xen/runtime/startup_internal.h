#ifndef RUNTIME_STARTUP_INTERNAL_H
#define RUNTIME_STARTUP_INTERNAL_H

#include "runtime/runtime.h"

#include <functional>
#include <thread>

namespace runtime::detail {

// 仅替换外部采集、顶层线程创建及启动发布屏障；各实例独立持有，不改变默认生产路径。
struct StartupAdapter {
    std::function<std::unique_ptr<ICapture>(const CaptureConfig&)> create_capture;
    std::function<void(Runtime&)> before_running;
    // 仅供确定性回归模拟 std::thread 构造失败，不接管子模块线程或物理输出。
    std::function<std::thread(std::function<void()>)> create_thread;
    // 推理已完成且Detector锁已释放，供真实重载/采集会话交错回归使用；默认不安装。
    std::function<void(std::uint64_t, DetectionStatus)> before_collection_offer;
};

} // namespace runtime::detail

#endif
