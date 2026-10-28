#include "debug_session/grenade_posture_internal.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using debug_session::detail::GrenadePosture;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Device final : public IMouseController {
public:
    bool supported = true, exclusive = true, input_valid = true, readable = true;
    bool held = false, fail_down = false, fail_up = false;
    MouseStatus current_status = MouseStatus::READY;
    std::vector<std::string> events;
    int downs = 0, ups = 0, whole_cleanups = 0;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    MouseStatus status() const noexcept override { return current_status; }
    std::string last_error() const override { return {}; }
    bool output_owner_exclusive() const noexcept override { return exclusive; }
    bool supports_left_ctrl_key() const noexcept override { return supported; }
    bool poll_input(InputSnapshot& value) noexcept override {
        value = {}; value.status = InputMonitorStatus::READY; value.state_valid = input_valid;
        return readable;
    }
    KeyboardReceipt set_left_ctrl_key(bool down) noexcept override {
        events.push_back(down ? "down" : "up"); held = down;
        if (down) ++downs; else ++ups;
        return {(down ? fail_down : fail_up) ? KeyboardDisposition::APPLICATION_UNKNOWN
                                            : KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++whole_cleanups; return {}; }
};
struct Fixture {
    std::shared_ptr<Device> device = std::make_shared<Device>();
    source_context::SourceContextSnapshot source;
    bool permitted = true;
    int finishes = 0;
    bool finish_known = false;
    Fixture() {
        source.available = true; source.focused = true; source.session_id = 11;
        source.control_safety_epoch = 3;
    }
    std::unique_ptr<GrenadePosture> posture() {
        return std::make_unique<GrenadePosture>(device, [this] { return source; },
            [this] { return permitted; }, [this](bool known) {
                ++finishes; finish_known = known;
                device->events.push_back(known ? "finish_known" : "finish_unknown");
            });
    }
};
void hold_and_finish() {
    Fixture f;
    {
        auto posture = f.posture();
        require(posture->start() && f.device->held && posture->valid(),
            "蹲姿helper必须按下Ctrl并在跨调用等待期间保持有效");
        require(!posture->cleanup_known(), "存在按下债务时不得声称Ctrl已经释放");
        f.device->events.push_back("align");
        require(posture->valid() && f.device->held && f.finishes == 0,
            "找点结束和等待下键不得提前释放Ctrl或归还独占租约");
        f.device->events.push_back("prethrow");
        require(posture->start() && f.device->downs == 1, "重复start不重复按下Ctrl");
        f.device->events.push_back("throw");
        require(posture->finish() && posture->cleanup_known() && !posture->valid() && !f.device->held,
            "投掷结束须先确认释放Ctrl再结束姿态");
        require(posture->finish() && f.device->ups == 1 && f.finishes == 1 && f.finish_known,
            "显式完成、重复完成及析构只能清理和归还一次");
    }
    require(f.device->events == std::vector<std::string>{"down","align","prethrow","throw","up","finish_known"},
        "跨找点至投掷的归还顺序必须为按下、持有、释放、归还租约");
    require(f.device->whole_cleanups == 0, "姿态helper只管理Ctrl，不清理不属于它的方向键");
}
void cancellation_conditions() {
    for (int condition = 0; condition < 8; ++condition) {
        Fixture f;
        auto posture = f.posture(); require(posture->start(), "取消夹具必须先成功持蹲");
        switch (condition) {
        case 0: f.source.focused = false; break;
        case 1: f.source.available = false; break;
        case 2: ++f.source.session_id; break;
        case 3: ++f.source.control_safety_epoch; break;
        case 4: f.permitted = false; break;
        case 5: f.device->input_valid = false; break;
        case 6: f.device->exclusive = false; break;
        default: f.device->current_status = MouseStatus::CLOSED; break;
        }
        require(!posture->valid(), "失焦、源代次、许可、设备和输入失效必须撤销持蹲有效性");
        require(posture->finish() && f.device->ups == 1 && f.finishes == 1 && f.finish_known,
            "许可撤销后仍必须尝试释放Ctrl并归还已清理租约");
    }
    Fixture f;
    { auto posture = f.posture(); require(posture->start(), "析构夹具必须先持蹲"); }
    require(f.device->ups == 1 && f.finishes == 1 && f.finish_known,
        "未显式finish的姿态拥有者析构必须释放Ctrl");
}
void failed_receipts() {
    Fixture down;
    down.device->fail_down = true;
    auto posture = down.posture();
    require(!posture->start() && !posture->valid() && !posture->cleanup_known(),
        "按下未确认仍须保留可能按下的清理债务");
    require(posture->finish() && down.device->ups == 1 && down.finish_known,
        "按下未确认也必须发释放，释放ACK后才能确认已清理");
    Fixture up;
    {
        auto held = up.posture(); require(held->start(), "释放失败夹具先持蹲");
        up.device->fail_up = true;
        require(!held->finish() && !held->cleanup_known() && !up.finish_known,
            "释放未确认必须将finish(false)交给租约，不得恢复其他输出");
        require(!held->finish() && up.device->ups == 1 && up.finishes == 1,
            "未知释放不自动重试或伪装为成功");
    }
    require(up.device->ups == 1 && up.finishes == 1, "释放失败后析构不得自行重试设备");
}
void rejected_before_output() {
    for (int condition = 0; condition < 7; ++condition) {
        Fixture f;
        switch (condition) {
        case 0: f.device->supported = false; break;
        case 1: f.device->exclusive = false; break;
        case 2: f.device->input_valid = false; break;
        case 3: f.device->readable = false; break;
        case 4: f.source.session_id = 0; break;
        case 5: f.source.focused = false; break;
        default: f.permitted = false; break;
        }
        auto posture = f.posture();
        require(!posture->start() && f.device->downs == 0 && f.device->ups == 0 && posture->cleanup_known(),
            "不支持Ctrl或初始许可失效时必须零输出拒绝");
        require(posture->finish() && f.finishes == 1 && f.finish_known && f.device->ups == 0,
            "零输出失败仍须归还租约，不发送无归属释放");
    }
}
}
int main() {
    try {
        hold_and_finish(); cancellation_conditions(); failed_receipts(); rejected_before_output();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
