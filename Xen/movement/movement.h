#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "mouse/mouse.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace movement {
enum class Mode { SPIN, LARGE_JUMP };
enum class Trigger { WHEEL_DOWN, WHEEL_UP, KEY };
enum class ReportMode { CUMULATIVE, RELATIVE_DELTA };
struct Config {
    bool enabled = false;
    bool spin_enabled = true;
    bool large_enabled = true;
    Trigger spin_trigger = Trigger::WHEEL_UP;
    Trigger large_trigger = Trigger::WHEEL_DOWN;
    int spin_virtual_key = 0;
    int large_virtual_key = 0;
    int jump_delay_ms = 0;
    int trigger_guard_ms = 150;
    int spin_duration_ms = 200;
    double spin_angle_degrees = 90;
    int large_duration_ms = 170;
    double large_angle_degrees = 25;
    bool large_ctrl_enabled = true;
    int large_ctrl_delay_ms = 650;
    int large_ctrl_hold_ms = 200;
    double sensitivity = 1.4;
    double yaw_degrees_per_count = 0.022;
    ReportMode report_mode = ReportMode::RELATIVE_DELTA;
    bool wheel_down_positive = false;
    bool operator==(const Config&) const = default;
};
bool valid_config(const Config& config) noexcept;
enum class State { STOPPED, IDLE, WAITING_DIRECTION, COOLDOWN, DELAY, TURNING, CTRL_PENDING, CLEANING, FAULT };
enum class Direction { LEFT, RIGHT };
struct Snapshot {
    State state = State::STOPPED;
    Direction direction = Direction::LEFT;
    std::uint64_t started = 0, completed = 0, canceled = 0;
    int sent_dx = 0;
    std::string error;
};
// 生命周期由单一调用方串行管理；配置、取消及快照可跨线程调用。
// 发送方向键、水平位移及可选Left Ctrl短按；跳跃由用户物理触发及游戏绑定产生。
class Worker final {
public:
    // Long Jump固定先A左转再D右转；旋转跳仍由物理A/D选侧。
    // 暂停回调true请求瞬停让出键盘并返回清理确认；false在保护结束后允许重建输入基线。
    Worker(std::shared_ptr<IMouseController> mouse, std::function<bool()> output_permission,
           std::function<bool(bool)> counter_strafe_suspension = {});
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
