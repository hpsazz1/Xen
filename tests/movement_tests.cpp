#include "movement/movement.h"
#include "log/log.h"
#include "movement/movement_internal.h"
#include "mouse/input_report_internal.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void wait_for(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error("身法专项等待超时");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
class Fake final : public IMouseController {
public:
    mutable std::mutex mutex;
    std::vector<InputReportEvent> reports;
    std::vector<int> moves, software;
    std::vector<bool> masks;
    std::vector<int> mask_keys;
    std::uint64_t sequence = 0, epoch = 1;
    std::atomic<int> cleanups{0};
    int keyboard_delay_ms = 0;
    int cleanup_delay_ms = 0;
    std::atomic<bool> fail_move{false}, fail_cleanup{false}, fail_mask{false};
    bool subscribed = false;
    bool ctrl_supported = true, ctrl_down = false;
    bool fail_ctrl_down = false, fail_ctrl_up = false;
    std::vector<std::pair<bool, Clock::time_point>> ctrl_events;
    bool supports_left_ctrl_key() const noexcept override { return ctrl_supported; }
    KeyboardReceipt set_left_ctrl_key(bool down) noexcept override {
        std::lock_guard lock(mutex); ctrl_down = down; ctrl_events.emplace_back(down, Clock::now());
        return {(down ? fail_ctrl_down : fail_ctrl_up) ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED};
    }
    bool ctrl_held() const { std::lock_guard lock(mutex); return ctrl_down; }

    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard lock(mutex);
        moves.push_back(command.dx_counts);
        MouseMoveReceipt result;
        result.succeeded = !fail_move.load() && command.dy_counts == 0;
        return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        if (keyboard_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(keyboard_delay_ms));
        std::lock_guard lock(mutex); software.push_back(mask);
        return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        std::lock_guard lock(mutex); masks.push_back(masked); mask_keys.push_back(key);
        return {(key == 1 || key == 2 || key == 8) && !fail_mask.load() ? KeyboardDisposition::ACKNOWLEDGED : KeyboardDisposition::APPLICATION_UNKNOWN};
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups;
        { std::lock_guard lock(mutex); if (!fail_cleanup.load()) ctrl_down = false; }
        if (cleanup_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(cleanup_delay_ms));
        return {fail_cleanup.load() ? KeyboardDisposition::APPLICATION_UNKNOWN : KeyboardDisposition::ACKNOWLEDGED};
    }
    bool set_movement_report_subscription(bool value) noexcept override {
        std::lock_guard lock(mutex); subscribed = value; return true;
    }
    bool read_movement_reports(InputReportCursor& cursor, InputReportBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        batch = {}; batch.subscribed = subscribed; batch.epoch = epoch; batch.final_sequence = sequence;
        batch.status = InputMonitorStatus::READY;
        if (cursor.epoch != epoch) cursor = {epoch, 0};
        for (const auto& event : reports) {
            if (event.epoch == epoch && event.sequence > cursor.sequence && batch.count < batch.events.size()) {
                batch.events[batch.count++] = event; cursor.sequence = event.sequence;
            }
        }
        return true;
    }
    void report(int x, int wheel, bool valid = true, std::uint8_t usage = 0, std::uint8_t second = 0, int age_ms = 0) {
        std::lock_guard lock(mutex);
        InputReportEvent event;
        event.epoch = epoch; event.sequence = ++sequence; event.state_valid = valid;
        event.received_at_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        event.received_at_steady_ns -= static_cast<std::int64_t>(age_ms) * 1000000;
        event.keyboard_usages[0] = usage; event.keyboard_usages[1] = second;
        event.raw_x = static_cast<std::int16_t>(x); event.raw_wheel = static_cast<std::int16_t>(wheel);
        reports.push_back(event);
    }
    void new_epoch() { std::lock_guard lock(mutex); ++epoch; sequence = 0; reports.clear(); }
    int total() const { std::lock_guard lock(mutex); int result = 0; for (int value : moves) result += value; return result; }
    std::vector<int> keys() const { std::lock_guard lock(mutex); return software; }
};
movement::Config config() {
    movement::Config result;
    // 多数场景使用累计滚轮单旋转夹具；相对报告和双模式场景另行显式覆盖。
    result.spin_enabled = true;
    result.large_enabled = false;
    result.report_mode = movement::ReportMode::CUMULATIVE;
    result.large_ctrl_enabled = false;
    result.trigger_guard_ms = 0; result.wheel_down_positive = true;
    result.enabled = true; result.spin_trigger = movement::Trigger::WHEEL_DOWN; result.jump_delay_ms = 0; result.spin_duration_ms = 12;
    result.large_duration_ms = 12; result.spin_angle_degrees = 1.0; result.large_angle_degrees = 1.0;
    result.sensitivity = 1.0; result.yaw_degrees_per_count = 0.01;
    return result;
}

void trigger(const std::shared_ptr<Fake>& fake, int x = -10) {
    fake->report(0, 0, true, 0x04); fake->report(x, 1, true, x < 0 ? 0x04 : 0x07);
}
void timed_ctrl_lifecycle() {
    require(movement::Config{}.large_angle_degrees == 25 && movement::Config{}.large_duration_ms == 170,
        "正式Long Jump默认参数错误");
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto fake = std::make_shared<Fake>(); auto value = config();
        value.spin_enabled = false; value.large_enabled = true; value.large_ctrl_enabled = true;
        value.large_ctrl_delay_ms = 80; value.large_ctrl_hold_ms = 15;
        if (scenario == 0) {
            value.large_angle_degrees = 25; value.large_duration_ms = 170;
            value.large_ctrl_delay_ms = 650; value.large_ctrl_hold_ms = 30;
        }
        if (scenario == 3) fake->fail_ctrl_down = true;
        if (scenario == 4) fake->fail_ctrl_up = true;
        if (scenario == 5) { fake->fail_ctrl_down = true; fake->fail_cleanup = true; }
        if (scenario == 6) fake->ctrl_supported = false;
        std::atomic<bool> suspended{false};
        movement::Worker worker(fake, [] { return true; },
            [&](bool value) { suspended = value; return true; });
        require(worker.start(value), "Ctrl测试启动失败");
        const auto began = Clock::now(); trigger(fake);
        if (scenario == 1) {
            wait_for([&]{return worker.snapshot().state == movement::State::CTRL_PENDING;}); worker.cancel();
        }
        if (scenario == 2) { wait_for([&]{return fake->ctrl_held();}); worker.cancel(); }
        if (scenario == 0) wait_for([&]{return worker.snapshot().completed == 1;});
        else if (scenario == 6) wait_for([&]{return worker.snapshot().state == movement::State::FAULT;});
        else wait_for([&]{return worker.snapshot().canceled == 1;});
        if (scenario == 0 || scenario == 1 || scenario == 2 || scenario == 6) wait_for([&]{return !suspended.load();});
        else require(suspended.load(), "未知清理释放了瞬停暂停责任");
        worker.stop();
        std::lock_guard lock(fake->mutex);
        if (scenario == 0) {
            require(fake->ctrl_events.size() == 2 && fake->ctrl_events[0].first && !fake->ctrl_events[1].first,
                "Ctrl非单次按下释放");
            require(fake->ctrl_events[0].second - began >= std::chrono::milliseconds(645), "Ctrl未等待触发延迟");
            require(fake->ctrl_events[1].second - fake->ctrl_events[0].second >= std::chrono::milliseconds(30), "Ctrl保持不足");
            require(fake->software == std::vector<int>({2,8,0}), "等待Ctrl时未先释放转向或增加多余键");
        }
        if (scenario == 1 || scenario == 6) require(fake->ctrl_events.empty(), "取消等待/能力不足仍发送Ctrl");
        if (scenario == 6) require(fake->moves.empty() && fake->software.empty() && fake->masks.empty(), "能力不足仍部分执行");
        if (scenario != 5) require(!suspended.load(), "stopped clean worker must return counter-strafe ownership");
        if (scenario != 5) require(!fake->ctrl_down, "Ctrl正常或故障取消后未清理");
    }
}

void long_ctrl_hold_lifecycle() {
    for (int hold : {200, 201, 2000}) {
        auto value = movement::Config{}; value.large_ctrl_hold_ms = hold;
        require(movement::valid_config(value), "Ctrl扩展按住范围未接受");
    }
    for (int hold : {-1, 0, 2001}) {
        auto value = movement::Config{}; value.large_ctrl_hold_ms = hold;
        require(!movement::valid_config(value), "Ctrl按住越界未拒绝");
    }
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto fake = std::make_shared<Fake>(); auto value = config();
        value.spin_enabled = false; value.large_enabled = true; value.large_ctrl_enabled = true;
        value.large_ctrl_delay_ms = 0; value.large_ctrl_hold_ms = 2000;
        fake->fail_ctrl_up = scenario >= 5;
        fake->fail_cleanup = scenario == 6;
        std::atomic<bool> allowed{true}, suspended{false};
        movement::Worker worker(fake, [&] { return allowed.load(); },
            [&](bool pause) { suspended = pause; return true; });
        require(worker.start(value), "长Ctrl启动失败"); trigger(fake);
        wait_for([&] { return fake->ctrl_held(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        require(fake->ctrl_held() && suspended.load() && worker.snapshot().completed == 0,
            "长Ctrl被旧200ms上限截断或提前归还所有权");
        fake->report(-10, 2, true, 0x04);
        const auto interrupted = Clock::now();
        if (scenario == 0) { value.large_ctrl_hold_ms = 1; worker.configure(value); }
        else if (scenario == 1) worker.cancel();
        else if (scenario == 2) allowed = false; // Runtime急停先撤销输出许可。
        else if (scenario == 3) worker.stop();
        else if (scenario == 4) { value.large_enabled = false; worker.configure(value); }
        if (scenario == 0) wait_for([&] { return worker.snapshot().completed == 1; });
        else wait_for([&] { return worker.snapshot().canceled == 1; });
        if (scenario >= 1 && scenario <= 4)
            require(Clock::now() - interrupted < std::chrono::milliseconds(500), "取消仍等待长Ctrl计时结束");
        if (scenario >= 5) require(worker.snapshot().state == movement::State::FAULT, "Ctrl释放失败未锁定");
        worker.stop();
        require(worker.snapshot().started == 1, "长Ctrl期间排队重复触发");
        std::lock_guard lock(fake->mutex);
        if (scenario == 0) {
            require(fake->ctrl_events.size() == 2 && fake->ctrl_events[0].first && !fake->ctrl_events[1].first,
                "长Ctrl非单次按下释放");
            require(fake->ctrl_events[1].second - fake->ctrl_events[0].second >= std::chrono::milliseconds(2000),
                "长Ctrl计时不足或本轮受下次配置影响");
        }
        if (scenario == 6) require(fake->cleanups.load() >= 2 && suspended.load(), "未知清理丢失软件键责任");
        else require(!fake->ctrl_down && !suspended.load(), "长Ctrl结束后未释放或未归还所有权");
    }
}

void relative_field_and_wait_expiry() {
    for (bool relative : {false, true}) {
        auto value = config(); value.spin_trigger = movement::Trigger::WHEEL_UP;
        value.large_enabled = true; value.wheel_down_positive = false; value.trigger_guard_ms = 150;
        value.report_mode = relative ? movement::ReportMode::RELATIVE_DELTA : movement::ReportMode::CUMULATIVE;
        auto fake = std::make_shared<Fake>();
        movement::Worker worker(fake, [] { return true; });
        require(worker.start(value), "报告对照启动失败");
        fake->report(0,0); fake->report(1,1,true,0x04);
        wait_for([&]{return worker.snapshot().completed == 1;});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        fake->report(1,0,true,0x04); // 现场1→0在保护后到达。
        if (!relative) {
            wait_for([&]{return worker.snapshot().completed == 2;});
            require(fake->keys() == std::vector<int>({2,2,8}), "累计兼容未重现现场反向差分"); worker.stop(); continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        require(worker.snapshot().started == 1 && fake->keys() == std::vector<int>({2}), "相对模式归零误触发Long Jump");
        fake->report(1,-1,true,0x04);
        wait_for([&]{return worker.snapshot().completed == 2;});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        fake->report(1,-1,true,0x07); // 完全复带鼠标字段，只改键盘。
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(worker.snapshot().started == 2, "重复鼠标字段重播");
        fake->report(2,-1,true,0x07); // 保持既有新鼠标报告同值wheel语义，不增加锁死。
        wait_for([&]{return worker.snapshot().completed == 3;});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        fake->new_epoch(); fake->report(0,-1,true,0x04);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(worker.snapshot().started == 3, "重连/序号复位首包触发");
        fake->report(0,1,true,0x04); // 保留符号反向。
        wait_for([&]{return worker.snapshot().completed == 4;});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        fake->report(0,0); fake->report(0,1); // 无方向等待。
        wait_for([&]{return worker.snapshot().state == movement::State::WAITING_DIRECTION;});
        const auto count = fake->keys().size();
        fake->report(0,0); fake->report(0,0,true,0x04,0x07);
        wait_for([&]{return worker.snapshot().canceled == 1;});
        require(fake->keys().size() == count && fake->keys() == std::vector<int>({2,2,8,2,8,2}), "等待无方向或双按产生输出/串动作");
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        fake->report(0,0,true,0x04);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(worker.snapshot().started == 5, "迟到方向激活旧触发");
        fake->report(0,1,true,0x04);
        wait_for([&]{return worker.snapshot().completed == 5;}); worker.stop();
    }
}
void wheel_mapping_default_regression() {
    auto fake=std::make_shared<Fake>();
    movement::Worker worker(fake,[]{return true;});
    auto value=config(); value.spin_trigger=movement::Trigger::WHEEL_UP; value.large_enabled=true;
    value.wheel_down_positive=movement::Config{}.wheel_down_positive;
    require(worker.start(value),"启动失败");
    fake->report(0,0); fake->report(0,1,true,0x07);
    wait_for([&]{return worker.snapshot().completed==1;});
    require(fake->keys()==std::vector<int>({8}) && fake->total()==100,"正向上滚错误触发大跳"); worker.stop();
}
void pause() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
void spin_trigger(const std::shared_ptr<Fake>& fake, int side = -1) {
    fake->report(0, 0); fake->report(-side * 100, 1, true, side < 0 ? 0x04 : 0x07);
}

void guard_boundary_and_packets() {
    require(movement::Config{}.trigger_guard_ms==150 && !movement::Config{}.wheel_down_positive,"默认保护/轮极性错误");
    require(!movement::detail::trigger_guard_finished(99,101,100),"保护期旧事件在期后重放");
    require(!movement::detail::trigger_guard_finished(101,99,100),"处理时刻未过保护仍放行");
    require(movement::detail::trigger_guard_finished(100,100,100),"保护边界新事件未放行");
    std::array<std::uint8_t,20> packet{};
    packet[6]=1;
    require(mouse::detail::parse_input_report(packet,1,true).raw_wheel==1,"原始正滚轮解码反号");
    packet[6]=255; packet[7]=255;
    require(mouse::detail::parse_input_report(packet,1,true).raw_wheel==-1,"原始负滚轮解码反号");
}
void guard_consumes_all_triggers() {
    for(bool large:{false,true}) {
        auto fake=std::make_shared<Fake>(); fake->cleanup_delay_ms=20;
        movement::Worker worker(fake,[]{return true;});
        auto value=config(); value.wheel_down_positive=false; value.spin_trigger=movement::Trigger::WHEEL_UP;
        value.large_enabled=true; value.trigger_guard_ms=100; value.spin_duration_ms=40;value.large_duration_ms=25;
        require(worker.start(value),"启动失败");
        int wheel=large?-1:1;
        fake->report(0,0);fake->report(0,wheel,true,0x07);
        wait_for([&]{return !fake->keys().empty();});
        fake->report(0,++wheel,true,0x04);fake->report(0,--wheel,true,0x07);
        wait_for([&]{return fake->cleanups.load()==1;});
        fake->report(0,++wheel,true,0x07); // cleanup中的滚轮也只能清尾。
        wait_for([&]{return worker.snapshot().completed==1;});
        require(worker.snapshot().state==movement::State::COOLDOWN,"完成后未进入保护");
        fake->report(0,++wheel,true,0x07);fake->report(0,--wheel,true,0x04);fake->report(0,--wheel,true,0x04);
        pause();require(worker.snapshot().started==1,"保护期同向或反向连滚启动第二动作");
        wait_for([&]{return worker.snapshot().state==movement::State::IDLE;});
        fake->report(0,++wheel,true,0x07,0,80); // 采集于保护期的延迟报告（仍在100ms新鲜度内）。
        pause();require(worker.snapshot().started==1,"保护结束重放保护期旧滚轮");
        fake->report(0,wheel,true,0x07);pause();
        require(worker.snapshot().started==1,"无新滚轮重放已消费边沿");
        fake->report(0,++wheel,true,0x07);
        wait_for([&]{return worker.snapshot().completed==2;});
        require(fake->keys().size()==(large?3u:2u),"另一动作混入当前周期");worker.stop();
    }
}
void waiting_guard_key_and_epoch() {
    auto fake=std::make_shared<Fake>();movement::Worker worker(fake,[]{return true;});
    auto value=config();value.wheel_down_positive=false;value.spin_trigger=movement::Trigger::WHEEL_UP;
    value.large_enabled=true;value.trigger_guard_ms=80;
    require(worker.start(value),"启动失败");fake->report(0,0);fake->report(0,1);
    wait_for([&]{return worker.snapshot().state==movement::State::WAITING_DIRECTION;});
    fake->report(0,0);fake->report(0,-1);pause();
    require(worker.snapshot().started==1 && fake->keys().empty(),"等待方向被大跳触发替换");
    worker.cancel();wait_for([&]{return worker.snapshot().canceled==1;});
    fake->new_epoch();fake->report(0,100,true,0x07);fake->report(0,101,true,0x07);pause();
    require(worker.snapshot().started==1,"重连清空动作保护");
    wait_for([&]{return worker.snapshot().state==movement::State::IDLE;});
    fake->report(0,101,true,0x07);pause();require(worker.snapshot().started==1,"重连旧序号/轮值重放");
    fake->report(0,102,true,0x07);wait_for([&]{return worker.snapshot().completed==1;});worker.stop();
    auto keys=std::make_shared<Fake>();movement::Worker other(keys,[]{return true;});
    value.large_enabled=false;value.spin_trigger=movement::Trigger::KEY;value.spin_virtual_key=0x20;
    require(other.start(value),"启动失败");keys->report(0,0);keys->report(0,0,true,0x2c,0x04);
    wait_for([&]{return other.snapshot().completed==1;});
    keys->report(0,0);keys->report(0,0,true,0x2c,0x07);pause();
    wait_for([&]{return other.snapshot().state==movement::State::IDLE;});
    keys->report(0,0,true,0x2c,0x07);pause();require(other.snapshot().started==1,"保护期按键在期后长按重放");
    keys->report(0,0);keys->report(0,0,true,0x2c,0x07);wait_for([&]{return other.snapshot().completed==2;});other.stop();
}

void keyboard_direction() {
    for (int sign : {-1, 1}) {
        for (bool preheld : {false, true}) {
            auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
            auto value = config(); value.spin_duration_ms = 40;
            require(worker.start(value), "启动失败");
            const auto usage = static_cast<std::uint8_t>(sign < 0 ? 0x04 : 0x07);
            fake->report(0, 0, true, preheld ? usage : 0);
            fake->report(-sign * 100, 1, true, usage); // 鼠标与键盘相反，必须遵从键盘。
            wait_for([&] { return !fake->keys().empty(); });
            fake->report(sign * 100, 2, true, sign < 0 ? 0x07 : 0x04);
            fake->report(0, 3); // 松键也不能在周期中换向或排队。
            wait_for([&] { return worker.snapshot().completed == 1; });
            require(fake->total() == sign * 100 && fake->keys() == std::vector<int>({sign < 0 ? 2 : 8}),
                "A左D右/鼠标独立/周期锁定失败");
            { std::lock_guard lock(fake->mutex); require(fake->mask_keys == std::vector<int>({1,2,8}), "物理A/D未屏蔽，可能抵消反侧"); }
            pause(); require(worker.snapshot().started == 1 && fake->cleanups == 1, "执行输入排成下一动作");
            worker.stop(); require(fake->cleanups == 1, "停止重复清理");
        }
    }
}
void waiting_and_rearm() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value=config(); value.spin_trigger=movement::Trigger::KEY; value.spin_virtual_key=0x20;
    require(worker.start(value), "启动失败");
    fake->report(0,0); fake->report(100,0,true,0x2c);
    wait_for([&] { return worker.snapshot().state == movement::State::WAITING_DIRECTION; });
    fake->report(-100,0,true,0x04,0x07); pause();
    require(fake->keys().empty() && fake->total()==0 && fake->cleanups==0, "无方向或双按产生输出");
    fake->report(-100,0,true,0x2c,0x07);
    wait_for([&] { return worker.snapshot().completed==1; });
    fake->report(100,0,true,0x2c,0x04); pause();
    require(worker.snapshot().started==1 && fake->total()==100, "启动键长按重触发或软件回显选侧");
    fake->report(100,0); fake->report(100,0,true,0x2c,0x04);
    wait_for([&] { return worker.snapshot().completed==2; });
    require(fake->total()==0, "释放重新按下未新选A"); worker.stop();
}
void fixed_large_and_tail() {
    auto fake=std::make_shared<Fake>();
    movement::Worker worker(fake, [] { return true; });
    auto value=config(); value.spin_trigger=movement::Trigger::WHEEL_UP; value.large_enabled=true;
    require(worker.start(value), "启动失败");
    fake->report(0,0,true,0x1a); fake->report(0,1,true,0x1a);
    wait_for([&] { return worker.snapshot().completed==1; });
    require(fake->keys()==std::vector<int>({2,8}) && fake->total()==0 && fake->cleanups==1,
        "大跳未固定A到D或未对称清理");
    std::size_t move_count; { std::lock_guard lock(fake->mutex); move_count=fake->moves.size(); }
    fake->report(0,1,true,0x1a); pause();
    { std::lock_guard lock(fake->mutex); require(fake->moves.size()==move_count, "完成后有残留move"); }
    require(worker.snapshot().started==1, "持W/重复滚轮重触发");
    fake->report(0,2,true,0x07); // 第二轮物理D不能改变固定A首侧。
    wait_for([&] { return worker.snapshot().completed==2; });
    require(fake->keys()==std::vector<int>({2,8,2,8}) && fake->total()==0, "新大跳未保持固定A到D");
    fake->report(0,1,true,0x1a); // 相反轮边沿会启动旋转等待，绝不能借旧软件A/D输出。
    wait_for([&] { return worker.snapshot().state==movement::State::WAITING_DIRECTION; });
    pause(); require(fake->keys().size()==4 && fake->total()==0, "尾轮边沿复用了大跳方向"); worker.stop();
}
void cancellation_and_faults() {
    for (int kind=0; kind<5; ++kind) {
        auto fake=std::make_shared<Fake>(); std::atomic<bool> allowed{true};
        movement::Worker worker(fake,[&] { return allowed.load(); });
        auto value=config(); value.spin_duration_ms=300;
        require(worker.start(value), "启动失败");
        if (kind==0) { fake->report(0,0); fake->report(0,1); wait_for([&] { return worker.snapshot().state==movement::State::WAITING_DIRECTION; }); }
        else { spin_trigger(fake); wait_for([&] { return !fake->keys().empty(); }); }
        if(kind==2) allowed=false;
        else if(kind==3) { value.spin_enabled=false; worker.configure(value); }
        else if(kind==4) fake->report(0,2,false);
        else worker.cancel();
        wait_for([&] { return worker.snapshot().canceled==1; });
        require(fake->cleanups==(kind==0?0:1), "等待/输出取消清理责任错误"); worker.stop();
    }
    auto fake=std::make_shared<Fake>(); fake->fail_mask=true; fake->fail_cleanup=true;
    movement::Worker worker(fake,[]{return true;}); require(worker.start(config()),"启动失败"); spin_trigger(fake);
    wait_for([&]{return worker.snapshot().state==movement::State::FAULT;});
    fake->report(0,2,true,0x07); pause(); require(worker.snapshot().started==1,"故障未闭锁");
    worker.stop(); require(!worker.start(config()),"清理欠账重启");
}
void reports_and_restart() {
    auto fake=std::make_shared<Fake>(); movement::Worker worker(fake,[]{return true;});
    require(worker.start(config()),"启动失败");
    fake->report(0,0); fake->report(0,1,true,0x04,0,200); pause();
    require(fake->keys().empty(),"过期启动事件被接受");
    fake->new_epoch(); fake->report(0,100,true,0x04); pause(); require(fake->keys().empty(),"epoch首包被当触发");
    fake->report(0,101,true,0x07); wait_for([&]{return worker.snapshot().completed==1;});
    worker.stop(); require(worker.start(config()),"重新启动失败");
    fake->new_epoch(); fake->report(0,0); fake->report(0,1);
    wait_for([&]{return worker.snapshot().state==movement::State::WAITING_DIRECTION;});
    require(fake->keys().size()==1,"重启复用了上轮方向"); worker.stop();
    for(auto mode:{movement::ReportMode::CUMULATIVE,movement::ReportMode::RELATIVE_DELTA}) {
        auto device=std::make_shared<Fake>(); movement::Worker other(device,[]{return true;});
        auto value=config(); value.report_mode=mode; require(other.start(value),"启动失败");
        device->report(32760,mode==movement::ReportMode::CUMULATIVE?32767:0);
        device->report(-32760,mode==movement::ReportMode::CUMULATIVE?-32768:1,true,0x07);
        wait_for([&]{return other.snapshot().completed==1;});
        device->report(-32760,mode==movement::ReportMode::CUMULATIVE?-32768:1,true,0x07); pause();
        require(other.snapshot().started==1 && device->total()==100,"轮回绕/重复包处理失败"); other.stop();
    }
}
void config_delay_and_fail_move() {
    auto fake=std::make_shared<Fake>(); movement::Worker worker(fake,[]{return true;});
    auto value=config(); value.spin_duration_ms=50; require(worker.start(value),"启动失败"); spin_trigger(fake);
    wait_for([&]{return !fake->keys().empty();}); value.spin_angle_degrees=2; worker.configure(value);
    wait_for([&]{return worker.snapshot().completed==1;}); require(fake->total()==-100,"运行配置未冻结");
    fake->report(0,2,true,0x07); wait_for([&]{return worker.snapshot().completed==2;});
    require(fake->total()==100,"下一动作未用新配置"); worker.stop();
    auto failed=std::make_shared<Fake>(); failed->fail_move=true; movement::Worker other(failed,[]{return true;});
    require(other.start(config()),"启动失败"); spin_trigger(failed);
    wait_for([&]{return other.snapshot().canceled==1;}); require(failed->cleanups==1 && other.snapshot().state==movement::State::FAULT,"move失败未清理"); other.stop();
    auto delayed=std::make_shared<Fake>(); delayed->keyboard_delay_ms=30; movement::Worker last(delayed,[]{return true;});
    require(last.start(config()),"启动失败"); spin_trigger(delayed); wait_for([&]{return last.snapshot().completed==1;});
    {std::lock_guard lock(delayed->mutex); require(delayed->moves==std::vector<int>({-100}),"ACK重新计时");} last.stop();
}
void spin_and_symmetric_large() {
    for (auto mode : {movement::Mode::SPIN, movement::Mode::LARGE_JUMP}) {
        for (int sign : {-1, 1}) {
            auto fake = std::make_shared<Fake>();
            movement::Worker worker(fake, [] { return true; });
            auto value = config(); value.spin_enabled = mode == movement::Mode::SPIN; value.large_enabled = !value.spin_enabled;
            require(worker.start(value), "启动失败"); trigger(fake, sign * 10);
            wait_for([&] { return worker.snapshot().completed == 1; });
            require(fake->total() == (mode == movement::Mode::SPIN ? sign * 100 : 0), "角度累计或大跳对称错误");
            const auto keys = fake->keys();
            require(keys.front() == (mode == movement::Mode::LARGE_JUMP || sign < 0 ? 2 : 8), "左右方向键映射错误");
            if (mode == movement::Mode::LARGE_JUMP)
                require(keys == std::vector<int>({2,8}), "大跳第二段未反向");
            require(fake->cleanups == 1, "动作完成未清理");
            worker.stop(); require(fake->cleanups == 1, "无债务停止重复发送清理");
        }
    }
}
void disabled_has_no_output() {
    auto fake = std::make_shared<Fake>();
    movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.enabled = false;
    require(worker.start(value), "禁用启动失败"); trigger(fake);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); worker.stop();
    require(fake->cleanups == 0 && fake->keys().empty() && fake->total() == 0, "禁用功能产生输出");
}
void cancel_and_permission() {
    for (bool permission_cancel : {false, true}) {
        auto fake = std::make_shared<Fake>(); std::atomic<bool> allowed{true};
        movement::Worker worker(fake, [&] { return allowed.load(); });
        auto value = config(); value.spin_duration_ms = 300;
        require(worker.start(value), "启动失败"); trigger(fake);
        wait_for([&] { return !fake->keys().empty(); });
        if (permission_cancel) allowed = false; else worker.cancel();
        wait_for([&] { return worker.snapshot().canceled == 1; });
        require(fake->cleanups == 1, "取消未清理"); worker.stop();
    }
}
void fault_latches() {
    auto fake = std::make_shared<Fake>(); fake->fail_mask = true; fake->fail_cleanup = true;
    movement::Worker worker(fake, [] { return true; });
    require(worker.start(config()), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::FAULT; });
    fake->report(-20, 2, true, 0x04); fake->report(-30, 3, true, 0x04);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(worker.snapshot().started == 1 && !worker.snapshot().error.empty(), "故障未闭锁");
    worker.stop(); require(!worker.start(config()), "清理未确认仍重新启动");
}
void wrap_and_relative_dedup() {
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        require(worker.start(config()), "启动失败");
        fake->report(32760, 32767, true, 0x04); fake->report(-32760, -32768, true, 0x07);
        wait_for([&] { return worker.snapshot().completed == 1; });
        require(fake->total() == 100, "signed16回绕方向错误"); worker.stop();
    }
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        auto value = config(); value.report_mode = movement::ReportMode::RELATIVE_DELTA;
        require(worker.start(value), "启动失败"); trigger(fake);
        wait_for([&] { return worker.snapshot().completed == 1; });
        for (int i = 0; i < 5; ++i) fake->report(-10, 1, true, 0x04);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(worker.snapshot().started == 1, "相对复带包重复触发");
        fake->report(0, 0, true, 0x04); fake->report(10, 1, true, 0x04);
        wait_for([&] { return worker.snapshot().completed == 2; }); worker.stop();
    }
}
void running_config_is_frozen() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_duration_ms = 60;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::TURNING; });
    value.spin_angle_degrees = 2; worker.configure(value);
    fake->report(-20, 2, true, 0x04); fake->report(-30, 3, true, 0x04);
    wait_for([&] { return worker.snapshot().completed == 1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(fake->total() == -100 && worker.snapshot().started == 1, "运行配置未冻结或执行中滚轮排队");
    fake->report(-30, 3, true, 0x04); fake->report(-40, 4, true, 0x04);
    wait_for([&] { return worker.snapshot().completed == 2; });
    require(fake->total() == -300, "新动作未使用新参数"); worker.stop();
}
void epoch_and_delay_cancel() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.jump_delay_ms = 200;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().state == movement::State::DELAY; });
    worker.cancel();
    wait_for([&] { return worker.snapshot().canceled == 1; });
    require(fake->cleanups == 0 && fake->keys().empty(), "延迟期取消产生无主清理");
    fake->new_epoch(); fake->report(1000, 1000, true, 0x04);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    require(worker.snapshot().started == 1, "新epoch首包被误作滚轮");
    fake->report(1010, 1001, true, 0x04);
    wait_for([&] { return worker.snapshot().started == 2; });
    fake->report(1020, 1002, false, 0x04);
    wait_for([&] { return worker.snapshot().canceled == 2; });
    require(fake->keys().empty(), "失效输入仍执行方向键"); worker.stop();
}
void disabling_active_item_cleans_up() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_duration_ms = 500;
    require(worker.start(value), "启动失败"); trigger(fake);
    wait_for([&] { return !fake->keys().empty(); });
    value.spin_enabled = false; worker.configure(value);
    wait_for([&] { return worker.snapshot().canceled == 1; });
    require(fake->cleanups == 1, "关闭单项未立即取消并清理"); worker.stop();
}
void shared_trigger_clock() {
    require(movement::Config{}.jump_delay_ms == 0, "默认仍起跳后等待");
    auto fake = std::make_shared<Fake>(); fake->keyboard_delay_ms = 30;
    movement::Worker worker(fake, [] { return true; });
    require(worker.start(config()), "启动失败"); trigger(fake);
    wait_for([&] { return worker.snapshot().completed == 1; });
    { std::lock_guard lock(fake->mutex);
      require(fake->moves.size() == 1 && fake->moves[0] == -100,
          "方向键ACK之后重新起算鼠标计时，额外延长动作"); }
    worker.stop();
}
void long_jump_ack_does_not_consume_turn() {
    auto fake = std::make_shared<Fake>(); fake->keyboard_delay_ms = 60;
    movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_enabled = false; value.large_enabled = true;
    value.large_duration_ms = 30;
    require(worker.start(value), "Long Jump启动失败"); const auto began = Clock::now(); trigger(fake);
    wait_for([&] { return worker.snapshot().completed == 1; });
    { std::lock_guard lock(fake->mutex);
      int left = 0, right = 0;
      for (int delta : fake->moves) { if (delta < 0) ++left; if (delta > 0) ++right; }
      require(left > 1 && right > 1, "Long Jump方向ACK吞掉转动窗口，整段退化为一次突转");
      require(fake->software == std::vector<int>({2, 8}), "两阶段增加了多余释放或改变固定A到D");
    }
    require(fake->total() == 0, "Long Jump两段总角度不对称");
    const auto elapsed = Clock::now() - began;
    require(elapsed >= std::chrono::milliseconds(180) && elapsed < std::chrono::seconds(1), "Long Jump总时长未包含两次ACK及完整两段，或发生无界等待"); worker.stop();
    auto cancel_fake = std::make_shared<Fake>(); cancel_fake->keyboard_delay_ms = 60;
    movement::Worker canceled(cancel_fake, [] { return true; });
    require(canceled.start(value), "取消测试启动失败"); trigger(cancel_fake);
    wait_for([&] { std::lock_guard lock(cancel_fake->mutex); return cancel_fake->masks.size() == 3; });
    canceled.cancel();
    wait_for([&] { return canceled.snapshot().canceled == 1; });
    { std::lock_guard lock(cancel_fake->mutex); require(cancel_fake->moves.empty(), "ACK等待期间取消后仍转动"); }
    require(cancel_fake->cleanups == 1, "取消未清理键盘"); canceled.stop();
}
void latest_direction_and_independent_triggers() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_trigger = movement::Trigger::WHEEL_UP;
    value.large_enabled = true;
    require(worker.start(value), "双项启动失败");
    fake->report(0, 0, true, 0x04); fake->report(-100, 0, true, 0x04); fake->report(-99, -1, true, 0x07);
    wait_for([&] { return worker.snapshot().completed == 1; });
    require(fake->total() == 100 && fake->keys().front() == 8, "物理D被鼠标方向覆盖或上滚未触发旋转跳");
    fake->report(-109, 0, true, 0x04);
    wait_for([&] { return worker.snapshot().completed == 2; });
    auto keys = fake->keys();
    require(keys.size() == 3 && keys[1] == 2 && keys[2] == 8 && fake->total() == 100,
        "双项并存下滚未独立触发大跳");
    worker.stop();
}
void key_trigger_and_stale_direction() {
    auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
    auto value = config(); value.spin_trigger = movement::Trigger::KEY; value.spin_virtual_key = 0x20;
    require(worker.start(value), "按键模式启动失败");
    fake->report(0, 0, true, 0x04); fake->report(-1, 0, true, 0x2c, 0x04);
    wait_for([&] { return worker.snapshot().completed == 1; });
    fake->report(-2, 0, true, 0x2c, 0x04);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(worker.snapshot().started == 1, "按键长按重复触发");
    fake->report(-2, 0, true, 0x04); fake->report(-3, 0, true, 0x2c, 0x04);
    wait_for([&] { return worker.snapshot().completed == 2; });
    worker.stop();
    auto stale = std::make_shared<Fake>(); movement::Worker other(stale, [] { return true; });
    require(other.start(config()), "启动失败"); stale->report(0,0); stale->report(-10,0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); stale->report(-10,1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(other.snapshot().state == movement::State::WAITING_DIRECTION && stale->keys().empty(), "陈旧方向或默认左侧触发动作"); other.stop();
}
void mouse_failure_and_polarity() {
    {
        auto fake = std::make_shared<Fake>(); fake->fail_move = true;
        movement::Worker worker(fake, [] { return true; });
        require(worker.start(config()), "启动失败"); trigger(fake);
        wait_for([&] { return worker.snapshot().canceled == 1; });
        require(worker.snapshot().state == movement::State::FAULT && fake->cleanups == 1,
            "鼠标失败未清理并闭锁"); worker.stop();
    }
    {
        auto fake = std::make_shared<Fake>(); movement::Worker worker(fake, [] { return true; });
        auto value = config(); value.wheel_down_positive = false;
        require(worker.start(value), "启动失败");
        fake->report(0, 0, true, 0x04); fake->report(10, -1, true, 0x07);
        wait_for([&] { return worker.snapshot().completed == 1; });
        fake->report(20, -2, true, 0x07);
        wait_for([&] { return worker.snapshot().completed == 2; });
        require(fake->total() == 200, "滚轮负向或动作结束后首个滚轮被丢弃"); worker.stop();
    }
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--diagnostic-trace") {
        try {
            LogConfig cfg; cfg.enable_console = false; cfg.enable_file = false; cfg.enable_ringbuf = true;
            Log::init(cfg);
            long_jump_ack_does_not_consume_turn();
            std::string trace;
            for (const auto& line : Log::get_ring_buffer()) trace += line + "\n";
            for (const char* field : {"stage=begin", "stage=owner_ready", "stage=masks_done", "stage=phase_1_end",
                "stage=phase_clock_started", "phase=2", "raw_prev=", "guard_until_us=", "ack_us=", "stage=cancel_requested"})
                require(trace.find(field) != std::string::npos, "身法诊断缺少关键时间线字段");
            std::cout << trace; Log::shutdown(); return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; Log::shutdown(); return 1; }
    }
    try {
        require(movement::valid_config(movement::Config{}),"默认配置非法");
        auto invalid=config(); invalid.sensitivity=0; require(!movement::valid_config(invalid),"零灵敏度未拒绝");
        invalid=config(); invalid.large_enabled=true; require(!movement::valid_config(invalid),"双触发冲突未拒绝");
        timed_ctrl_lifecycle(); long_ctrl_hold_lifecycle(); relative_field_and_wait_expiry(); guard_boundary_and_packets(); guard_consumes_all_triggers(); waiting_guard_key_and_epoch(); wheel_mapping_default_regression(); spin_and_symmetric_large(); disabled_has_no_output(); cancel_and_permission(); fault_latches(); wrap_and_relative_dedup(); running_config_is_frozen(); epoch_and_delay_cancel(); mouse_failure_and_polarity(); shared_trigger_clock(); long_jump_ack_does_not_consume_turn(); disabling_active_item_cleans_up(); latest_direction_and_independent_triggers(); key_trigger_and_stale_direction();
        keyboard_direction(); waiting_and_rearm(); fixed_large_and_tail(); cancellation_and_faults(); reports_and_restart(); config_delay_and_fail_move();
        std::cout << "身法专项通过\n"; return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
