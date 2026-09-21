#include "auto_stop/auto_stop_worker.h"
#include "trigger/trigger_worker.h"
#include "weapon/weapon_timing.h"
#include "weapon/weapon_internal.h"
#include "runtime/weapon_context_internal.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <source_location>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::int64_t ns(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
template<class Predicate> void until(Predicate predicate, std::chrono::milliseconds budget = 500ms,
        const std::source_location source = std::source_location::current()) {
    const auto deadline = Clock::now() + budget;
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error(std::string("组合测试等待超时：") + source.file_name() + ":" + std::to_string(source.line()));
        std::this_thread::sleep_for(1ms);
    }
}
struct KeyCommand {
    std::uint8_t mask;
    Clock::time_point submitted, acknowledged;
};
class FakeMouse final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool left_button_faulted() const noexcept override { return false; }
    bool left_button_cleanup_required() const noexcept override { return left_down.load(); }
    bool poll_input(InputSnapshot& input) noexcept override {
        std::lock_guard lock(mutex);
        input = {}; input.state_valid = true; input.status = InputMonitorStatus::READY;
        input.sequence = sequence; input.virtual_keys[5] = activation;
        input.virtual_keys['W'] = (held & 1) != 0; input.virtual_keys['A'] = (held & 2) != 0;
        input.virtual_keys['S'] = (held & 4) != 0; input.virtual_keys['D'] = (held & 8) != 0;
        return true;
    }
    bool set_wasd_event_subscription(bool value) noexcept override {
        std::lock_guard lock(mutex); subscribed = value; return true;
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        std::lock_guard lock(mutex);
        batch = {}; batch.subscribed = subscribed; cursor.epoch = 1;
        for (const auto& event : events) if (event.sequence > cursor.sequence && batch.count < batch.events.size()) {
            batch.events[batch.count++] = event; cursor.sequence = event.sequence;
        }
        delivered = std::max(delivered, cursor.sequence);
        return true;
    }
    static KeyboardReceipt keyboard_ack() noexcept {
        KeyboardReceipt result;
        result.datagram_sent = true; result.disposition = KeyboardDisposition::ACKNOWLEDGED;
        result.protocol_ack_received_at = Clock::now();
        // 故意区分协议ACK与后端返回，禁止用返回时刻冒充ACK。
        std::this_thread::sleep_for(2ms);
        result.backend_completed_at = Clock::now();
        return result;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        const auto started = Clock::now();
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        software = mask; keys.push_back({mask, started, receipt.protocol_ack_received_at});
        return receipt;
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t key, bool masked) noexcept override {
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        if (masked) masks |= key; else masks &= ~key;
        return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        const auto receipt = keyboard_ack();
        std::lock_guard lock(mutex);
        if (left_down.load()) cleanup_during_shot = true;
        masks = software = 0;
        if (same_key_cleanup_report)
            events.push_back({held, true, 1, ++sequence, ns(Clock::now())});
        return receipt;
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        ButtonReceipt result;
        result.datagram_sent = true; result.disposition = ButtonDisposition::ACKNOWLEDGED;
        result.protocol_ack_received_at = Clock::now();
        std::this_thread::sleep_for(2ms);
        result.backend_completed_at = Clock::now(); result.cleanup_required = down;
        left_down.store(down);
        if (down) ++downs; else ++ups;
        return result;
    }
    void physical(std::uint8_t mask, bool activate) {
        std::lock_guard lock(mutex); held = mask; activation = activate;
        events.push_back({mask, true, 1, ++sequence, ns(Clock::now())});
    }
    void allow(bool activate) { std::lock_guard lock(mutex); activation = activate; }
    bool drained() { std::lock_guard lock(mutex); return delivered == sequence; }
    bool released() { std::lock_guard lock(mutex); return masks == 0 && software == 0 && !left_down; }
    std::vector<KeyCommand> keyboard_commands() { std::lock_guard lock(mutex); return keys; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    std::atomic<unsigned> downs{0}, ups{0}, moves{0};
    std::atomic<bool> cleanup_during_shot{false};
    bool same_key_cleanup_report = false;
private:
    std::mutex mutex;
    std::vector<WasdEvent> events;
    std::vector<KeyCommand> keys;
    std::uint64_t sequence = 0, delivered = 0;
    std::uint8_t held = 0, masks = 0, software = 0;
    bool activation = false, subscribed = false;
    std::atomic<bool> left_down{false};
};
std::shared_ptr<TriggerObservation> fresh_observation(std::uint64_t sequence) {
    auto result = std::make_shared<TriggerObservation>();
    result->detections.push_back({30, 30, 70, 70, .9f, 0});
    result->center_x = result->center_y = 50;
    result->roi_width = result->roi_height = 100;
    result->epoch = 1; result->sequence = sequence;
    result->observed_at = Clock::now(); result->valid = result->timing_valid = true;
    return result;
}
void gsi_trust_breaks_require_release() {
    const auto catalog = weapon::default_timing_catalog();
    for (const std::string_view failure : {"identity", "death", "invalid", "timeout", "focus", "input", "manual", "emergency"}) {
        weapon::GsiConfig gc; gc.enabled = true;
        weapon::detail::GsiState gsi;
        runtime::detail::TriggerWeaponContext adapter;
        TriggerController controller;
        TriggerConfig config; config.enabled = true; config.hold_virtual_key = 5; config.fire_delay_ms = 0;
        require(controller.configure(config), "GSI安全回归配置");
        auto now = Clock::now();
        std::uint64_t ts = 1700000000;
        nlohmann::json data = {{"provider", {{"appid", 730}, {"steamid", "123"}, {"timestamp", ts}}},
            {"player", {{"steamid", "123"}, {"activity", "playing"}, {"state", {{"health", 100}}},
                {"weapons", {{"weapon_0", {{"name", "weapon_ak47"}, {"state", "active"},
                    {"ammo_clip", 30}, {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}}}}}};
        auto ingest = [&] {
            data["provider"]["timestamp"] = ++ts;
            gsi.ingest(data.dump(), gc, now, ts * 1000);
        };
        auto permit = [&](bool held = true) {
            TriggerPermit p; p.enabled = p.healthy = p.focused = p.armed = true; p.held = held;
            p.context = adapter.update(gsi.snapshot(now), catalog, now); return p;
        };
        ingest(); controller.tick(permit(false), now);
        now += 1ms;
        // 未产生旧按钮债务；健康上下文下已建立许可，随后只施加一个安全断点。
        controller.tick(permit(), now);
        if (failure == "identity" || failure == "death" || failure == "invalid" || failure == "timeout") {
            if (failure == "timeout") now += 3s;
            else {
                if (failure == "identity") data["provider"]["steamid"] = data["player"]["steamid"] = "456";
                if (failure == "death") data["player"]["state"]["health"] = 0;
                if (failure == "invalid") data["player"].erase("weapons");
                ingest();
                // 故意不让controller读失败快照，信任断点必须由发布端持久保存。
            }
            data["provider"]["steamid"] = data["player"]["steamid"] = "123";
            data["player"]["state"]["health"] = 100;
            data["player"]["weapons"] = {{"weapon_0", {{"name", "weapon_ak47"}, {"state", "active"},
                {"ammo_clip", 30}, {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}};
            now += 1ms; ingest(); controller.tick(permit(), now);
        } else {
            data["player"]["weapons"]["weapon_0"]["state"] = "reloading";
            now += 1ms; ingest(); controller.tick(permit(), now);
            auto broken = permit();
            if (failure == "focus") broken.focused = false;
            if (failure == "input") broken.healthy = false;
            if (failure == "manual") broken.physical_left_down = true;
            if (failure == "emergency") broken.armed = false;
            now += 1ms; controller.tick(broken, now);
            data["player"]["weapons"]["weapon_0"]["state"] = "active";
            now += 1ms; ingest(); controller.tick(permit(), now);
        }
        now += 1ms;
        require(controller.tick(permit(), now).snapshot.reason == TriggerReason::WAIT_RELEASE,
            "信任/安全中断即使被普通换弹遮盖或消费者跳过，也必须要求松键重新武装");
        controller.tick(permit(false), now);
        now += 1ms;
        auto observation = fresh_observation(1); observation->observed_at = now;
        require(controller.observe(*observation, permit(), now).button_action == TriggerButtonAction::DOWN,
            "健康状态松键后才可建立新的GSI会话");
    }
}

// 走生产解析/连续性/Runtime转换，再驱动两个生产worker；许可始终来自FakeMouse。
void gsi_session_recovery(const char* transition) {
    const auto catalog = weapon::default_timing_catalog();
    weapon::GsiConfig gsi_config; gsi_config.enabled = true;
    weapon::detail::GsiState gsi;
    std::mutex gsi_mutex;
    runtime::detail::TriggerWeaponContext trigger_context;
    std::uint64_t timestamp = 1700000000;
    auto ingest = [&](const char* name, const char* state, int ammo) {
        nlohmann::json payload = {
            {"provider", {{"appid", 730}, {"steamid", "76561198000000000"}, {"timestamp", ++timestamp}}},
            {"player", {{"steamid", "76561198000000000"}, {"activity", "playing"}, {"state", {{"health", 100}}},
                {"weapons", {{"weapon_0", {{"name", name}, {"state", state}, {"ammo_clip", ammo},
                    {"ammo_clip_max", 30}, {"ammo_reserve", 90}}}}}}}};
        std::lock_guard lock(gsi_mutex);
        gsi.ingest(payload.dump(), gsi_config, Clock::now(), timestamp * 1000);
    };
    auto snapshot = [&] { std::lock_guard lock(gsi_mutex); return gsi.snapshot(Clock::now()); };
    ingest("weapon_ak47", "active", 30);
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<std::uint64_t> next_id{0};
    AutoStopWorker stop(mouse, arbiter, [] { return true; }, [&] { return ++next_id; }, [] { return true; },
        [&] { return runtime::detail::auto_stop_weapon_context(snapshot(), true, &catalog, Clock::now()); });
    TriggerWorker trigger(mouse, arbiter, [] { return true; }, [] { return true; }, [&] { return ++next_id; },
        [&](std::uint64_t id) { return stop.request(id); }, [&](std::uint64_t id) { stop.cancel(id); },
        [&] { return trigger_context.update(snapshot(), catalog, Clock::now()); },
        [&] { return stop.estimated_completion_id(); },
        [&](std::uint64_t id, TriggerTime deadline) { stop.resume_movement(id, deadline - 58ms); });
    AutoStopConfig sc{true, 5}; sc.cycle_enabled = true; sc.counter_hold_ms = 40; sc.shot_after_release_ms = 18;
    TriggerConfig tc; tc.enabled = true; tc.hold_virtual_key = 5; tc.fire_delay_ms = 0;
    tc.require_stop = tc.allow_estimated_stop = true; tc.max_observation_age_ms = 300;
    mouse->physical(0, false);
    require(stop.start(sc) && trigger.start(tc), "GSI组合worker启动");
    until([&] { return mouse->drained() && trigger.snapshot().reason == TriggerReason::RELEASED; });
    mouse->physical(2, true);
    std::uint64_t sequence = 0;
    auto publish = [&] { stop.publish_tracking_target(Clock::now() + 300ms); stop.publish_target(Clock::now() + 300ms); trigger.publish(fresh_observation(++sequence)); };
    TriggerSnapshot initial_down;
    until([&] {
        publish();
        const auto firing = trigger.firing_signal();
        const auto current = trigger.snapshot();
        // 按钮ACK先于完整快照发布；两个getter必须用命令编号关联。
        if (!firing.confirmed_down || current.command_id != firing.id ||
            !current.button_may_be_down || current.estimated_stop_request_id == 0) return false;
        initial_down = current;
        return true;
    });
    const auto old_stop = initial_down.estimated_stop_request_id;
    const auto before = mouse->downs.load();
    if (std::string_view(transition) == "switch") ingest("weapon_deagle", "active", 7);
    else ingest("weapon_ak47", transition, 0);
    until([&] { return !trigger.snapshot().button_may_be_down && stop.estimated_completion_id() == 0; });
    if (std::string_view(transition) != "switch") {
        const auto end = Clock::now() + 25ms;
        while (Clock::now() < end) { publish(); std::this_thread::sleep_for(1ms); }
        require(mouse->downs == before, "普通不可用期间不得发送DOWN");
        ingest("weapon_ak47", "active", 30);
    }
    bool recovered = false;
    TriggerSnapshot state;
    const auto end = Clock::now() + 1200ms;
    while (Clock::now() < end) {
        publish();
        const auto firing = trigger.firing_signal();
        const auto current = trigger.snapshot();
        state = current;
        if (firing.confirmed_down && current.command_id == firing.id && current.button_may_be_down &&
            current.command_id != initial_down.command_id && current.estimated_stop_request_id != 0 &&
            current.estimated_stop_request_id != old_stop) { recovered = true; break; }
        std::this_thread::sleep_for(1ms);
    }
    const bool stop_latched = stop.snapshot().release_required;
    trigger.stop(); stop.stop();
    if (!recovered) std::cerr << "transition=" << transition << " trigger=" << static_cast<int>(state.reason)
        << " stop_release_required=" << stop_latched << '\n';
    require(recovered, "持续持键的正常GSI武器过渡必须恢复，不要求松键");
    require(state.estimated_stop_request_id != old_stop && state.estimated_stop_request_id != 0,
        "恢复不得继承旧武器急停编号");
    require(state.firing_context.timing_weapon_id == (std::string_view(transition) == "switch" ? "deagle" : "ak47"),
        "恢复必须使用当前武器时序");
}
void run_weapon(const char* weapon_id, bool cycle = false, bool lose_candidate = false, bool lose_target = false,
                bool leave_trigger_region = false) {
    const auto catalog = weapon::default_timing_catalog();
    const auto* profile = weapon::find_timing(catalog, weapon_id);
    require(profile && profile->enabled, "组合测试须使用共享表有效武器");
    auto mouse = std::make_shared<FakeMouse>();
    // 清理期间的同键监听报告必须允许下一轮真正制动完成并解除Trigger等待。
    mouse->same_key_cleanup_report = cycle;
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<std::uint64_t> next_id{0};
    std::atomic<unsigned> trigger_requests{0};
    AutoStopWorker stop(mouse, arbiter, [] { return true; }, [&] { return ++next_id; }, [] { return true; });
    std::function<void(std::uint64_t, TriggerTime)> resume;
    if (cycle) resume = [&](std::uint64_t id, TriggerTime deadline) {
        const bool accepted = stop.resume_movement(id, deadline - 58ms);
        // 与生产回调一致：跟踪已撤销时归还可拒绝，继续取消旧请求以收齐清理责任。
        if (!accepted && lose_target) stop.cancel(id);
        else require(accepted, "有效循环必须接收当前点射对应的急停归还");
    };
    TriggerWorker trigger(mouse, arbiter, [] { return true; }, [] { return true; }, [&] { return ++next_id; },
        [&](std::uint64_t id) { ++trigger_requests; return stop.request(id); },
        [&](std::uint64_t id) { stop.cancel(id); }, [&] {
            TriggerContext result{1, true, true};
            result.timing_required = result.timing_valid = true;
            result.shot_hold_ms = profile->shot_hold_ms; result.fire_interval_ms = profile->fire_interval_ms;
            result.timing_catalog_revision = catalog.revision; result.timing_weapon_id = profile->canonical_id;
            return result;
        }, [&] { return stop.estimated_completion_id(); }, std::move(resume));
    AutoStopConfig stop_config{true, 5};
    stop_config.cycle_enabled = cycle;
    stop_config.counter_hold_ms = 40; stop_config.shot_after_release_ms = 18;
    TriggerConfig trigger_config;
    trigger_config.enabled = true; trigger_config.hold_virtual_key = 5; trigger_config.fire_delay_ms = 0;
    trigger_config.require_stop = trigger_config.allow_estimated_stop = true;
    trigger_config.max_observation_age_ms = 300;
    if (leave_trigger_region) trigger_config.range_percent = 50;
    mouse->physical(0, false);
    require(stop.start(stop_config), "急停生产worker启动失败");
    require(trigger.start(trigger_config), "扳机生产worker启动失败");
    until([&] { return mouse->drained() && trigger.snapshot().reason == TriggerReason::RELEASED; });
    mouse->physical(2, false);
    until([&] { return mouse->drained(); });
    std::this_thread::sleep_for(100ms);
    require(mouse->downs == 0, "允许键未按下不得开火");
    mouse->allow(true);
    std::uint64_t sequence = 0;
    if (leave_trigger_region) {
        until([&] {
            stop.publish_tracking_target(Clock::now() + 300ms); stop.publish_target(Clock::now() + 300ms);
            trigger.publish(fresh_observation(++sequence));
            return mouse->downs == 1 && trigger.firing_signal().confirmed_down;
        });
        until([&] {
            auto moved = fresh_observation(++sequence);
            // 原身体仍唯一连续匹配，但准星已离开缩小后的触发范围。
            moved->detections[0].x1 += 11;
            moved->detections[0].x2 += 11;
            stop.publish_tracking_target(Clock::now() + 300ms); stop.publish_target(Clock::now() + 300ms);
            trigger.publish(moved);
            return mouse->ups == 1 && !trigger.snapshot().button_may_be_down;
        });
        require(stop.snapshot().cycle_count <= 1, "短点射未结束前不得提前启动下一轮急停");
    }
    std::uint64_t expected_cycles = 2;
    if (lose_candidate) {
        until([&] {
            stop.publish_tracking_target(Clock::now() + 300ms); stop.publish_target(Clock::now() + 300ms);
            trigger.publish(fresh_observation(++sequence));
            return mouse->downs == 1 && trigger.firing_signal().confirmed_down;
        });
        auto missing = fresh_observation(++sequence);
        missing->detections.clear();
        trigger.publish(missing);
        if (lose_target) stop.publish_tracking_target({});
        until([&] { return mouse->ups == 1 && !trigger.snapshot().button_may_be_down; });
        until([&] { const auto current = stop.snapshot();
            return mouse->released() && (lose_target ? current.canceled + current.cycle_count >= 1 : current.cycle_count == 1); });
        expected_cycles = stop.snapshot().cycle_count + 1;
        require(!stop.snapshot().release_required, "候选丢失后的已确认抬键不得要求松键重按");
        const auto waiting_until = Clock::now() + std::chrono::milliseconds(profile->fire_interval_ms) + 80ms;
        while (Clock::now() < waiting_until) {
            stop.publish_tracking_target(lose_target ? Clock::time_point{} : Clock::now() + 300ms);
            stop.publish_target(lose_target ? Clock::now() : Clock::now() + 300ms);
            auto empty = fresh_observation(++sequence);
            empty->detections.clear();
            trigger.publish(empty);
            require(mouse->downs == 1, "候选未恢复期间不得再次开火");
            if (lose_target) require(stop.snapshot().requests == 1, "目标未恢复期间不得重新接管移动");
            std::this_thread::sleep_for(1ms);
        }
    }
    until([&] {
        stop.publish_tracking_target(Clock::now() + 300ms); stop.publish_target(Clock::now() + 300ms);
        trigger.publish(fresh_observation(++sequence));
        return mouse->ups >= 2 && !trigger.snapshot().button_may_be_down &&
            (!cycle || (stop.snapshot().cycle_count >= expected_cycles && mouse->released()));
    }, 2500ms);
    const auto keys = mouse->keyboard_commands();
    const auto stopped = stop.snapshot();
    require(keys.size() == (cycle ? 6 : 3) && keys[0].mask == 0 && keys[1].mask == 8 && keys[2].mask == 0,
        "每次接管均须zero→D→zero，循环每发重新制动；旧模式只制动一次");
    if (cycle) require(keys[3].mask == 0 && keys[4].mask == 8 && keys[5].mask == 0 &&
        stopped.cycle_moving && stopped.cycle_count == expected_cycles && !mouse->cleanup_during_shot.load(),
        "持续按A必须完成两轮反向制动，且仅在LEFT UP确认后归还移动");
    require(keys[2].submitted >= keys[1].acknowledged + 40ms, "反向按住不得早于ACK加40ms释放");
    require(stopped.requests == (cycle ? 2 : 1) && stopped.completed == (cycle ? 2 : 1) && trigger_requests == 0,
        "两发共享一次独立急停，不额外创建观察租约或急停请求");
    require(!stopped.fire_permitted, "估计时序不得伪造严格观察资格");
    mouse->allow(false);
    until([&] { return stop.estimated_completion_id() == 0 && mouse->released(); });
    trigger.stop(); stop.stop();
    const auto log = trigger.execution_log();
    std::vector<TriggerExecutionEvent> down_events, up_events;
    for (const auto& event : log.events) if (event.backend_called && event.receipt_status == TriggerReceiptStatus::ACKNOWLEDGED) {
        if (event.button_action == TriggerButtonAction::DOWN) down_events.push_back(event);
        if (event.button_action == TriggerButtonAction::UP) up_events.push_back(event);
    }
    require(down_events.size() == 2 && up_events.size() == 2, "共享worker应完成两对DOWN/UP并无多余点击");
    require(down_events[0].call_started_at >= keys[2].acknowledged + 18ms,
        "最终软件释放ACK后不足18ms不得发出LEFT DOWN");
    require(down_events[1].call_started_at - down_events[0].call_started_at >= std::chrono::milliseconds(profile->fire_interval_ms),
        "武器两次DOWN实际提交间隔不得短于共享参数");
    for (std::size_t index = 0; index < 2; ++index) {
        if (lose_candidate && index == 0)
            require(up_events[index].snapshot.reason == TriggerReason::NO_CANDIDATE ||
                (lose_target && up_events[index].snapshot.reason == TriggerReason::STOP_EXPIRED),
                "首发必须由候选失效提前抬键，不能用正常到期冒充恢复回归");
        else require(up_events[index].call_started_at >= down_events[index].protocol_ack_received_at + std::chrono::milliseconds(profile->shot_hold_ms),
            "正常LEFT UP提交不得早于DOWN协议ACK加武器按住时长");
        require(down_events[index].snapshot.estimated_stop_request_id == (cycle ? index + 1 : stopped.request_id),
            "每发须绑定对应急停worker的估计完成id；循环不可复用旧id");
        if (cycle) require(down_events[index].call_started_at >= keys[index * 3 + 2].acknowledged + 18ms,
            "每轮LEFT DOWN都需等待本轮反向释放ACK加18ms");
    }
    require(mouse->released() && mouse->moves == 0, "松允许键清理必须归还键鼠且不得产生鼠标位移");
    std::cout << weapon_id << "：共享生产worker组合证据通过，未连接设备\n";
}
// 从未DOWN和已DOWN两个阶段验证真实人物消失；纯触发域丢失仍保留跟踪资格。
void target_loss_lifecycle(bool already_down) {
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<std::uint64_t> next_id{0};
    AutoStopWorker stop(mouse, arbiter, [] { return true; }, [&] { return ++next_id; }, [] { return true; });
    TriggerWorker trigger(mouse, arbiter, [] { return true; }, [] { return true; }, [&] { return ++next_id; },
        [&](std::uint64_t id) { return stop.request(id); }, [&](std::uint64_t id) { stop.cancel(id); }, {},
        [&] { return stop.estimated_completion_id(); },
        [&](std::uint64_t id, TriggerTime deadline) { stop.resume_movement(id, deadline - 58ms); });
    AutoStopConfig sc{true, 5}; sc.cycle_enabled = true;
    TriggerConfig tc; tc.enabled = tc.require_stop = tc.allow_estimated_stop = true;
    tc.hold_virtual_key = 5; tc.fire_delay_ms = 0; tc.range_percent = 50;
    tc.press_duration_ms = 200; tc.shot_interval_ms = 500; tc.max_observation_age_ms = 300;
    mouse->physical(0, false);
    require(stop.start(sc) && trigger.start(tc), "目标生命周期组合worker启动");
    until([&] { return mouse->drained() && trigger.snapshot().reason == TriggerReason::RELEASED; });
    mouse->physical(2, true);
    std::uint64_t sequence = 0;
    auto publish_visible = [&](bool inside) {
        stop.publish_tracking_target(Clock::now() + 300ms);
        stop.publish_target(Clock::now() + 300ms);
        auto observation = fresh_observation(++sequence);
        if (!inside) {
            observation->detections[0].x1 += 11;
            observation->detections[0].x2 += 11;
        }
        trigger.publish(observation);
    };
    until([&] {
        publish_visible(already_down);
        return already_down ? trigger.firing_signal().confirmed_down :
            stop.estimated_completion_id() != 0 && trigger.snapshot().reason == TriggerReason::NO_CANDIDATE;
    });
    const auto previous_stop = stop.estimated_completion_id();
    require(previous_stop != 0, "人物消失前必须已拥有本轮完成资格");
    if (!already_down) {
        // 仅准星离开人物或触发范围，仍有同一跟踪人物，不得归还键盘。
        const auto keep_until = Clock::now() + 80ms;
        while (Clock::now() < keep_until) {
            stop.publish_tracking_target(Clock::now() + 300ms);
            stop.publish_target({}, AutoStopBlockReason::CROSSHAIR_OUTSIDE_TARGET);
            auto outside = fresh_observation(++sequence);
            outside->detections[0].x1 += 40; outside->detections[0].x2 += 40;
            trigger.publish(outside);
            require(stop.estimated_completion_id() == previous_stop && !mouse->released() && mouse->downs == 0,
                "仍有跟踪人物时离开触发域或准星离人物不得误释放急停");
            std::this_thread::sleep_for(1ms);
        }
    }
    auto missing = fresh_observation(++sequence); missing->detections.clear();
    trigger.publish(missing);
    stop.publish_target({}, AutoStopBlockReason::NO_TARGET);
    stop.publish_tracking_target({});
    until([&] { return mouse->released() && !trigger.snapshot().button_may_be_down &&
        stop.estimated_completion_id() == 0; });
    require(!stop.snapshot().release_required, "真实人物消失只撤销本轮急停，不要求松许可键");
    require(!mouse->cleanup_during_shot.load(), "已DOWN后人物消失必须先LEFT UP ACK再归还键盘");
    const auto requests_after_loss = stop.snapshot().requests;
    const auto missing_until = Clock::now() + 70ms;
    while (Clock::now() < missing_until) {
        auto empty = fresh_observation(++sequence); empty->detections.clear(); trigger.publish(empty);
        require(mouse->released() && stop.snapshot().requests == requests_after_loss,
            "人物未恢复期间不得凭旧目标再次接管");
        std::this_thread::sleep_for(1ms);
    }
    until([&] {
        publish_visible(true);
        const auto firing = trigger.firing_signal();
        const auto state = trigger.snapshot();
        return firing.confirmed_down && state.estimated_stop_request_id != 0 &&
            state.estimated_stop_request_id != previous_stop;
    }, 1500ms);
    require(stop.snapshot().requests > requests_after_loss && !stop.snapshot().release_required,
        "持续许可下新人物必须重新急停并绑定新编号后才触发");
    trigger.stop(); stop.stop();
    require(mouse->released() && !mouse->cleanup_during_shot.load(), "组合关闭仍遵循先UP后键盘归还");
}
void stationary_owner_does_not_interrupt_shot() {
    auto mouse = std::make_shared<FakeMouse>();
    auto arbiter = std::make_shared<AutoStopOutputArbiter>();
    std::atomic<std::uint64_t> next_id{0};
    AutoStopWorker stop(mouse, arbiter, [] { return true; }, [&] { return ++next_id; }, [] { return true; });
    TriggerWorker trigger(mouse, arbiter, [] { return true; }, [] { return true; }, [&] { return ++next_id; },
        [&](std::uint64_t id) { return stop.request(id); }, [&](std::uint64_t id) { stop.cancel(id); }, {},
        [&] { return stop.estimated_completion_id(); }, {}, {},
        [&](const InputSnapshot& input) { return stop.idle_for_trigger(input); });
    TriggerConfig config;
    config.enabled = config.require_stop = config.allow_estimated_stop = true;
    config.hold_virtual_key = 5; config.fire_delay_ms = 0;
    config.press_duration_ms = 150; config.shot_interval_ms = 500; config.max_observation_age_ms = 500;
    mouse->physical(0, false);
    require(stop.start(AutoStopConfig{true, 5}) && trigger.start(config), "原地双worker启动");
    until([&] { return trigger.snapshot().reason == TriggerReason::RELEASED; });
    mouse->physical(0, true);
    std::uint64_t sequence = 0;
    until([&] { trigger.publish(fresh_observation(++sequence)); return trigger.firing_signal().confirmed_down; });
    const auto until_hold = Clock::now() + 100ms;
    while (Clock::now() < until_hold) {
        mouse->physical(0, true);
        trigger.publish(fresh_observation(++sequence));
        require(mouse->ups == 0, "owner轮询或重复零报告不得撤销健康原地射击");
        std::this_thread::sleep_for(1ms);
    }
    until([&] { return mouse->ups == 1; });
    const auto log = trigger.execution_log();
    for (const auto& event : log.events)
        if (event.button_action == TriggerButtonAction::UP && event.backend_called)
            require(event.snapshot.reason == TriggerReason::RELEASED, "原地点射仅正常结束，不因假停稳失效提前抬键");
    require(stop.snapshot().requests == 0, "原地无需制造急停请求");
    trigger.stop(); stop.stop();
}
} // namespace
int main() {
    try {
        target_loss_lifecycle(false);
        target_loss_lifecycle(true);
        gsi_trust_breaks_require_release();
        for (const char* transition : {"reloading", "active", "switch"}) gsi_session_recovery(transition);
        stationary_owner_does_not_interrupt_shot();
        for (const char* id : {"deagle", "ak47", "awp"}) { run_weapon(id); run_weapon(id, true); }
        run_weapon("ak47", true, true);
        run_weapon("ak47", true, true, true);
        run_weapon("ak47", true, false, false, true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "武器急停组合回归失败：" << error.what() << '\n';
        return 1;
    }
}
