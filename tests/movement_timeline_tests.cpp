#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
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
    int dx = 0, dy = 0, wheels = 0, wheel_calls = 0, cleanups = 0, masks = 0, software = 0;
    int physical = 1;
    bool space = false, ctrl = false, fail_key = false, fail_cleanup = false, fail_move = false;
    bool delay_key = false;
    bool button_supported = true, left = false, right = false;
    bool fail_button_down = false, fail_button_up = false, shared_button_fault = false;
    int left_ups = 0, right_ups = 0;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_ctrl_key() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return button_supported; }
    bool left_button_faulted() const noexcept override { return shared_button_fault; }
    ButtonReceipt set_left_button(bool down) noexcept override {
        events.push_back(down ? "left_down" : "left_up");
        if (!down) ++left_ups;
        left = down;
        return {((down && fail_button_down) || (!down && fail_button_up)) ?
            ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED};
    }
    ButtonReceipt set_right_button(bool down) noexcept override {
        events.push_back(down ? "right_down" : "right_up");
        if (!down) ++right_ups;
        right = down;
        return {((down && fail_button_down) || (!down && fail_button_up)) ?
            ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED};
    }
    MouseMoveReceipt move(const MouseMoveCommand& c) noexcept override {
        dx += c.dx_counts; dy += c.dy_counts; MouseMoveReceipt r; r.succeeded = !fail_move; return r;
    }
    MouseMoveReceipt scroll_wheel(int value) noexcept override {
        wheels += value; ++wheel_calls; MouseMoveReceipt r; r.succeeded = !fail_move; return r;
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
Action button(Button value, int start, int duration) {
    Action a; a.kind = ActionKind::BUTTON; a.button = value; a.start_ms = start; a.duration_ms = duration; return a;
}
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
    bad = j; bad["schema_version"] = 4; require(!parse_plan(bad, read, error), "未知版本未拒绝");
    bad = j; bad.erase("voice_command"); require(!parse_plan(bad, read, error), "V3缺少口令字段未拒绝");
    bad = j; bad["voice_command"] = 12; require(!parse_plan(bad, read, error), "非文本口令未拒绝");
    bad = j; bad["voice_command"] = std::string(257, 'a');
    require(!parse_plan(bad, read, error), "超长口令未拒绝");
    auto voiced = j; voiced["voice_command"] = "跳箱"; voiced["map"] = "de_dust2";
    require(parse_plan(voiced, read, error) && plan_json(read) == voiced, "地图口令往返丢失或动作变化");
    auto legacy = j; legacy["schema_version"] = 1; legacy.erase("voice_command");
    require(parse_plan(legacy, read, error) && read.actions.size() == 2, "V1方案不兼容");
    require(read.voice_command.empty(), "V1口令未清空");
    legacy["actions"][0]["physical"] = true;
    require(!parse_plan(legacy, read, error), "V1未知字段被接受");
    p.actions.push_back(turn(20, 50, 12)); require(!validate_plan(p, error), "旋转重叠未拒绝");
}
void recorded_tracks() {
    Plan p; p.mask.enabled = false;
    Action physical = key(Key::W, 0, 400); physical.physical = true;
    Action mask_w{ActionKind::MASK, Key::W, 80, 140};
    Action mask_a{ActionKind::MASK, Key::A, 120, 180};
    Action motion{ActionKind::MOVE, Key::W, 0, 400}; motion.dx = 81; motion.dy = -37;
    Action additive = motion; additive.dx = -21; additive.dy = 7;
    Action wheel{ActionKind::WHEEL, Key::W, 50, 20}; wheel.wheel = -2;
    p.actions = {physical, mask_w, mask_a, motion, additive, wheel};
    auto j = plan_json(p); Plan restored; std::string error;
    require(parse_plan(j, restored, error) && plan_json(restored) == j, "录制轨道序列化丢失字段");
    auto legacy = j; legacy["schema_version"] = 2; legacy.erase("voice_command");
    restored.voice_command = "旧口令";
    require(parse_plan(legacy, restored, error) && restored.voice_command.empty() && plan_json(restored) == j,
        "V2动作升级丢失或口令未清空");
    legacy["voice_command"] = "未声明的新字段";
    require(!parse_plan(legacy, restored, error), "V2意外接受新字段");
    Fake f; bool suppressed = false, resumed = false, independent = false;
    auto cb = permission(); cb.progress = [&](const Progress& progress) {
        if (progress.elapsed_ms >= 85 && progress.elapsed_ms < 210) {
            require(!(f.software & 1), "物理W屏蔽段被重新合成为软件W"); suppressed = true;
        }
        if (progress.elapsed_ms >= 225 && progress.elapsed_ms < 290) {
            require(f.software == 1 && f.masks == 2, "独立W/A屏蔽结束时间错误"); independent = true;
        }
        if (progress.elapsed_ms >= 310 && progress.elapsed_ms < 390 && f.software == 1 && f.masks == 0) resumed = true;
    };
    const auto result = run_plan(f, restored, scale(), cb);
    require(result.outcome == Outcome::COMPLETED && suppressed && resumed && independent, "录制屏蔽轨道未正确执行");
    require(f.dx == 60 && f.dy == -30 && f.wheels == -2 && f.wheel_calls == 1, "录制XY叠加或滚轮次数错误");
    require(f.cleanups == 1 && f.effective_keys() == 1, "录制完成未归还物理W");
    Fake canceled; cb = permission(); cb.canceled = [&] { return canceled.masks != 0; };
    const auto stopped = run_plan(canceled, p, scale(), cb);
    require(stopped.outcome == Outcome::CANCELED && canceled.cleanups == 1 && canceled.masks == 0,
        "录制屏蔽轨道取消未清理");
    p.actions.push_back(key(Key::W, 0, 400));
    Fake software; cb = permission(); bool software_kept = false;
    cb.progress = [&](const Progress& progress) {
        if (progress.elapsed_ms >= 85 && progress.elapsed_ms < 210) {
            require(software.software == 1, "物理屏蔽错误地屏蔽了软件W轨道"); software_kept = true;
        }
    };
    require(run_plan(software, p, scale(), cb).outcome == Outcome::COMPLETED && software_kept,
        "物理屏蔽错误地屏蔽了软件W轨道");
    Plan hidden; hidden.mask.enabled = false;
    auto hidden_w = key(Key::W, 10, 10); hidden_w.physical = true;
    hidden.actions = {key(Key::A, 0, 100), hidden_w, {ActionKind::MASK, Key::W, 0, 100}};
    Fake delayed; delayed.delay_key = true;
    require(run_plan(delayed, hidden, scale(), permission()).outcome == Outcome::COMPLETED,
        "完整屏蔽内的短物理轨道被误报为漏发");
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
void first_output_admission() {
    // 首动作前的空等待不能提前消费画面新鲜窗。
    Fake device; Plan p; p.mask.enabled = false; p.actions = {key(Key::A,40,30)};
    auto cb = permission(); bool expired = false; int checks = 0;
    cb.progress = [&](const Progress& progress) { if(progress.elapsed_ms >= 10) expired = true; };
    cb.first_output_permitted = [&] { ++checks; return !expired; };
    auto result = run_plan(device,p,scale(),cb);
    require(result.outcome == Outcome::CANCELED && checks == 1 && device.events.empty(),
        "等待首动作时提前消费验证结果或拒绝后仍输出");
    for (const auto kind : {ActionKind::KEY,ActionKind::BUTTON,ActionKind::MOVE,ActionKind::WHEEL}) {
        Fake denied; Action action; action.kind=kind; action.start_ms=0; action.duration_ms=20;
        action.dx=10; action.wheel=1; p.actions={action}; cb=permission();
        cb.first_output_permitted=[] {return false;};
        result=run_plan(denied,p,scale(),cb);
        require(result.outcome==Outcome::CANCELED && denied.events.empty() && denied.dx==0 && denied.wheel_calls==0,
            "一种实际设备输出绕过首次许可或虚构清理责任");
    }
    Fake accepted; p.actions={key(Key::A,0,30)}; cb=permission(); checks=0;
    cb.first_output_permitted=[&] {++checks;return true;};
    require(run_plan(accepted,p,scale(),cb).outcome==Outcome::COMPLETED && checks==1 && accepted.cleanups==1,
        "首次许可未保持一次消费及原动作清理");
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
void button_contract_and_jump_order() {
    Plan p; p.mask.enabled = false;
    p.actions = {button(Button::RIGHT, 0, 80), button(Button::LEFT, 0, 80), key(Key::SPACE, 80, 40)};
    auto j = plan_json(p); Plan parsed; std::string error;
    require(parse_plan(j, parsed, error) && plan_json(parsed) == j, "按钮方案往返丢失");
    auto bad = j; bad["actions"][0]["button"] = "middle";
    require(!parse_plan(bad, parsed, error), "未知鼠标按钮未拒绝");
    bad = j; bad["actions"][0]["physical"] = true;
    require(!parse_plan(bad, parsed, error), "按钮物理字段未拒绝");
    bad = j; bad["schema_version"] = 2; bad.erase("voice_command");
    require(!parse_plan(bad, parsed, error), "旧版本意外接受新按钮动作");
    Fake f; const auto result = run_plan(f, p, scale(), permission());
    require(result.outcome == Outcome::COMPLETED && result.cleanup_known && !f.left && !f.right,
        "双键跳投未完成或未释放");
    const auto jump = std::find(f.events.begin(), f.events.end(), "space_down");
    const auto left_up = std::find(f.events.begin(), f.events.end(), "left_up");
    const auto right_up = std::find(f.events.begin(), f.events.end(), "right_up");
    require(jump != f.events.end() && jump < left_up && jump < right_up, "松按钮先于同刻跳跃");
    require(f.left_ups == 2 && f.right_ups == 2 && f.cleanups == 1, "双键未分别承担最终释放责任");
    Fake unsupported; unsupported.button_supported = false;
    const auto rejected = run_plan(unsupported, p, scale(), permission());
    require(rejected.outcome == Outcome::FAILED && unsupported.events.empty(), "不支持按钮的设备仍输出");
}
void button_failure_cleanup_and_missed_interval() {
    Plan p; p.mask.enabled = false;
    p.actions = {button(Button::LEFT, 0, 80), button(Button::RIGHT, 0, 80)};
    Fake canceled; auto cb = permission(); cb.canceled = [&] { return canceled.left && canceled.right; };
    auto r = run_plan(canceled, p, scale(), cb);
    require(r.outcome == Outcome::CANCELED && r.cleanup_known && canceled.left_ups == 1 && canceled.right_ups == 1,
        "取消未释放所有已尝试按钮");
    Fake unknown; unknown.fail_button_down = true;
    r = run_plan(unknown, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && r.cleanup_known && unknown.left_ups == 1 && unknown.right_ups == 0,
        "按钮按下ACK未知未清理或清理无关按钮");
    Fake dirty; dirty.fail_button_up = true;
    r = run_plan(dirty, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && !r.cleanup_known && dirty.right_ups == 1 && dirty.left_ups >= 1,
        "一侧清理失败跳过另一侧或假报已释放");
    Fake shared; shared.shared_button_fault = true;
    r = run_plan(shared, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && !r.cleanup_known, "共享按钮故障被单侧ACK掩盖");
    Fake delayed; delayed.delay_key = true;
    p.actions = {key(Key::A, 0, 100), button(Button::LEFT, 10, 1)};
    r = run_plan(delayed, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && r.error.find("错过") != std::string::npos && delayed.left_ups == 0,
        "完整短按钮区间错过仍被当作成功");
    Fake at_start; at_start.delay_key = true;
    p.actions = {key(Key::A, 0, 100), button(Button::LEFT, 0, 1)};
    r = run_plan(at_start, p, scale(), permission());
    require(r.outcome == Outcome::FAILED && at_start.left_ups == 0 && !at_start.left,
        "键盘ACK跨过按钮区间仍补发过期按下");
    Fake covered; p.actions = {button(Button::LEFT, 0, 100), button(Button::LEFT, 10, 1)};
    cb = permission(); bool slept = false;
    cb.progress = [&](const Progress&) { if (!slept) { slept = true; std::this_thread::sleep_for(std::chrono::milliseconds(30)); } };
    r = run_plan(covered, p, scale(), cb);
    require(r.outcome == Outcome::COMPLETED && std::count(covered.events.begin(), covered.events.end(), "left_down") == 1,
        "重叠按钮区间未按并集执行");
    Fake ordered; p.actions = {button(Button::LEFT, 0, 80), button(Button::RIGHT, 0, 40)};
    cb = permission(); slept = false;
    cb.progress = [&](const Progress&) {
        if (!slept) { slept = true; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    };
    r = run_plan(ordered, p, scale(), cb);
    const auto right_up = std::find(ordered.events.begin(), ordered.events.end(), "right_up");
    const auto left_up = std::find(ordered.events.begin(), ordered.events.end(), "left_up");
    require(r.outcome == Outcome::COMPLETED && right_up != ordered.events.end() && right_up < left_up,
        "同轮跨过两个结束时刻后颠倒了先松右键再松左键的配方顺序");
}
void validate_directory(const std::filesystem::path& directory) {
    require(std::filesystem::is_directory(directory), "方案目录不存在");
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".json" || entry.path().filename() == "manifest.json") continue;
        require(entry.is_regular_file() && !entry.is_symlink(), "方案必须为普通文件");
        require(entry.file_size() <= 16 * 1024 * 1024, "方案文件超过16MiB");
        std::ifstream input(entry.path(), std::ios::binary);
        require(static_cast<bool>(input), "无法读取方案文件");
        try {
            nlohmann::json document; input >> document;
            Plan parsed, restored; std::string error;
            if (!parse_plan(document, parsed, error)) throw std::runtime_error(error);
            const auto canonical = plan_json(parsed);
            if (!parse_plan(canonical, restored, error)) throw std::runtime_error(error);
            require(plan_json(restored) == canonical, "方案往返不一致");
        } catch (const std::exception& e) {
            throw std::runtime_error(entry.path().filename().string() + ": " + e.what());
        }
        ++count;
    }
    require(count > 0, "方案目录没有待校验JSON");
    std::cout << "方案目录生产解析及往返通过：" << count << " 条；未执行任何方案\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 1) {
            require(argc == 3 && std::string(argv[1]) == "--validate-directory", "用法：--validate-directory PATH");
            validate_directory(std::filesystem::u8path(argv[2]));
            return 0;
        }
        json_contract(); parallel_union_and_restore(); failures_and_cancel(); missed_interval_and_mask_window(); recorded_tracks();
        button_contract_and_jump_order(); button_failure_cleanup_and_missed_interval();
        first_output_admission();
        std::cout << "身法时间轴专项通过\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
