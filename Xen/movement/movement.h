#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "mouse/mouse.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace movement {
enum class Mode { SPIN, LARGE_JUMP };
enum class ReportMode { CUMULATIVE, RELATIVE_DELTA };
struct Config {
    bool enabled = false;
    Mode mode = Mode::SPIN;
    int jump_delay_ms = 20;
    int spin_duration_ms = 300;
    double spin_angle_degrees = 90;
    int large_duration_ms = 150;
    double large_angle_degrees = 15;
    double sensitivity = 1.4;
    double yaw_degrees_per_count = 0.022;
    ReportMode report_mode = ReportMode::CUMULATIVE;
    bool wheel_down_positive = true;
    bool operator==(const Config&) const = default;
};
bool valid_config(const Config& config) noexcept;
enum class State { STOPPED, IDLE, DELAY, TURNING, CLEANING, FAULT };
enum class Direction { LEFT, RIGHT };
struct Snapshot {
    State state = State::STOPPED;
    Direction direction = Direction::LEFT;
    std::uint64_t started = 0, completed = 0, canceled = 0;
    int sent_dx = 0;
    std::string error;
};
// 生命周期由单一调用方串行管理；配置、取消及快照可跨线程调用。
// 只发送方向键与水平位移；跳跃始终由用户物理滚轮及游戏绑定产生。
class Worker final {
public:
    Worker(std::shared_ptr<IMouseController> mouse, std::function<bool()> output_permission);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    bool start(const Config& config) noexcept;
    void configure(const Config& config) noexcept;
    void cancel() noexcept;
    void stop() noexcept;
    Snapshot snapshot() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
