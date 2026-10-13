#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace movement::timeline;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Fake final : public IMouseController {
public:
    std::vector<int> keys, mask_bits;
    std::vector<std::string> events;
    int dx = 0, cleanups = 0, masks = 0, software = 0;
    int physical = 1;
    bool space = false, ctrl = false, fail_key = false, fail_cleanup = false, fail_move = false;
    bool delay_key = false;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_ctrl_key() const noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand& c) noexcept override {
        dx += c.dx_counts; MouseMoveReceipt r; r.succeeded = !fail_move; return r;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t value) noexcept override {
        software = value; keys.push_back(value); events.push_back("keys");
        if (delay_key) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        return {fail_key ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_space_key(bool value) noexcept override {
        space = value; events.push_back(value ? "space_down" : "space_up"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_left_ctrl_key(bool value) noexcept override {
        ctrl = value; return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool value) noexcept override {
        mask_bits.push_back(key); if (value) masks |= key; else masks &= ~key;
        events.push_back(value ? "mask" : "unmask"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups; events.push_back("cleanup");
        if (fail_cleanup) return {KeyboardDisposition::APPLICATION_UNKNOWN};
        software = 0; space = ctrl = false; masks = 0; return {KeyboardDisposition::ACKNOWLEDGED};
    }
    int effective_keys() const { return software | (physical & ~masks); }
};
Action key(Key value, int start, int duration) { return {ActionKind::KEY, value, start, duration, 25}; }
Action turn(int start, int duration, double angle) { return {ActionKind::TURN, Key::W, start, duration, angle}; }
Callbacks permission() { return {[] { return true; }, {}, {}}; }
movement::Config scale() { movement::Config c; c.sensitivity = 1; c.yaw_degrees_per_count = 0.1; return c; }
void json_contract() {
    Plan p; p.actions = {key(Key::SPACE, 0, 20), turn(0, 50, -12.3)};
    p.map = "本地训练图"; p.location = "测试点";
    auto j = plan_json(p); Plan read; std::string error;
    require(parse_plan(j, read, error) && plan_json(read) == j, "JSON往返失真");
    const auto original = plan_json(read);
    auto bad = j; bad["actions"][0]["start_ms"] = 0.5;
    require(!parse_plan(bad, read, error) && plan_json(read) == original, "非整数时间被接受或破坏原方案");
    bad = j; bad["unexpected"] = true; require(!parse_plan(bad, read, error), "未知字段未拒绝");
    bad = j; bad["actions"][0]["key"] = "ESC"; require(!parse_plan(bad, read, error), "未知键未拒绝");
    bad = j; bad["schema_version"] = 2; require(!parse_plan(bad, read, error), "未知版本未拒绝");
    p.actions.push_back(turn(20, 50, 12)); require(!validate_plan(p, error), "旋转重叠未拒绝");
}
void parallel_union_and_restore() {
    Fake f; Plan p;
    p.actions = {key(Key::SPACE, 0, 30), key(Key::A, 0, 50), key(Key::A, 20, 60),
        key(Key::D, 0, 80), key(Key::CTRL, 0, 80), turn(0, 80, -12.3)};
    bool observed_parallel = false;
    auto cb = permission(); cb.progress = [&](const Progress&) {
        if (f.space && f.ctrl && f.software == 10 && f.masks == 15) observed_parallel = true;
    };
    auto result = run_plan(f, p, scale(), cb);
    require(result.outcome == Outcome::COMPLETED && result.cleanup_known, "并行方案未完成");
    require(observed_parallel, "没有观察到组合键与屏蔽并行");
    require(f.keys == std::vector<int>({10, 0}), "同键重叠被提前释放或产生重复按下");
    require(f.dx == -123 && result.sent_dx == -123, "累计旋转舍入丢失角度");
    require(f.cleanups == 1 && f.effective_keys() == 1 && !f.space && !f.ctrl, "结束未归还物理W或未清理");
    require(f.events.front() == "mask" && f.events.back() == "cleanup", "屏蔽未先于跳跃或未清理");
}
void failures_and_cancel() {
    Plan p; p.actions = {key(Key::SPACE, 0, 100), key(Key::A, 0, 100), turn(0, 100, 10)};
    Fake canceled; auto cb = permission();
    cb.canceled = [&] { return canceled.software != 0; };
    auto r = run_plan(canceled, p, scale(), cb);
    require(r.outcome == Outcome::CANCELED && canceled.cleanups == 1 && !canceled.space && canceled.dx == 0,
        "键盘ACK后取消仍继续发送或未清理");
    Fake denied; cb = permission(); cb.permitted = [] { return false; };
    r = run_plan(denied, p, scale(), cb);
    require(r.outcome == Outcome::CANCELED && denied.events.empty(), "无许可仍发出设备指令");
    Fake failed; failed.fail_key = true;
    r = run_plan(failed, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && failed.cleanups == 1 && r.cleanup_known, "未知键盘ACK未承担清理责任");
    Fake dirty; dirty.fail_key = dirty.fail_cleanup = true;
    r = run_plan(dirty, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && !r.cleanup_known, "清理未知被宣称成功");
    Fake mouse; mouse.fail_move = true;
    r = run_plan(mouse, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && mouse.cleanups == 1, "鼠标失败未清理");
    Fake thrown; cb = permission(); cb.progress = [](const Progress&) { throw std::runtime_error("取消宿主"); };
    r = run_plan(thrown, p, scale(), cb);
    require(r.outcome == Outcome::FAILED && thrown.cleanups == 1, "宿主异常未收敛并清理");
}
void missed_interval_and_mask_window() {
    Fake covered; covered.delay_key = true; Plan overlap; overlap.mask.enabled = false;
    overlap.actions = {key(Key::A, 0, 100), key(Key::A, 10, 1)};
    auto covered_result = run_plan(covered, overlap, scale(), permission());
    require(covered_result.outcome == Outcome::COMPLETED && covered.keys == std::vector<int>({2, 0}),
        "同键持续按住已经覆盖的短区间被错误判定为漏发");
    Fake delayed; delayed.delay_key = true; Plan p; p.mask.enabled = false;
    p.actions = {key(Key::A, 0, 100), key(Key::SPACE, 10, 10)};
    auto r = run_plan(delayed, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && r.error.find("错过") != std::string::npos && delayed.cleanups == 1,
        "跨过完整按键区间被静默当成完成");
    Fake f; p.mask = {true, 1, 0, 30}; p.actions = {key(Key::A, 0, 80)};
    bool restored_during_action = false;
    auto cb = permission(); cb.progress = [&](const Progress& progress) {
        if (progress.elapsed_ms >= 30 && f.software == 2 && f.effective_keys() == 3) restored_during_action = true;
    };
    r = run_plan(f, p, scale(), cb);
    require(r.outcome == Outcome::COMPLETED && restored_during_action, "独立屏蔽结束未刷新物理状态");
    require(f.mask_bits == std::vector<int>({1, 1}), "屏蔽范围错误");
}
}
int main() {
    try {
        json_contract(); parallel_union_and_restore(); failures_and_cancel(); missed_interval_and_mask_window();
        std::cout << "身法时间轴专项通过\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
