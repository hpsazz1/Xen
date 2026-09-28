#include "runtime/aim_frame_internal.h"
#include "runtime/collection_frame_internal.h"
#include "weapon/weapon_internal.h"

#include <cmath>
#include <iostream>
#include <future>
#include <opencv2/imgproc.hpp>

namespace {
// 仅内存回执，无设备构造、连接或输入订阅。
class SlotFakeMouse final : public IMouseController {
public:
    unsigned calls = 0;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool poll_input(InputSnapshot&) noexcept override { return false; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++calls;
        MouseMoveReceipt receipt;
        receipt.succeeded = true;
        receipt.backend_completed_at = std::chrono::steady_clock::now();
        return receipt;
    }
};
std::string weapon_payload(const char* state, int ammo, const char* name = "weapon_ak47", int health = 100,
        const char* player_id = "76561198000000000", std::uint64_t timestamp = 1700000000) {
    return std::string(R"({"provider":{"appid":730,"steamid":"76561198000000000","timestamp":)") +
        std::to_string(timestamp) + R"(},"player":{"steamid":")" + player_id +
        R"(","activity":"playing","state":{"health":)" + std::to_string(health) +
        R"(},"weapons":{"weapon_0":{"name":")" + name + R"(","state":")" + state +
        R"(","ammo_clip":)" + std::to_string(ammo) + R"(,"ammo_clip_max":30,"ammo_reserve":90}}}})";
}
}

int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "[失败] " << message << '\n'; }
    };
    cv::Mat texture(320, 320, CV_8UC3);
    cv::RNG random(74521);
    random.fill(texture, cv::RNG::UNIFORM, 0, 255);
    runtime::detail::RuntimeObservationClock clock;
    runtime::detail::CameraMotionEstimator estimator;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15;
    Aim aim(config);
    const auto start = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    {
        runtime::detail::CollectionFrameGate gate;
        source_context::SourceContextSnapshot source;
        FrameTiming timing;
        timing.source_time_timing_valid = true;
        timing.source_time_at = start;
        expect(gate.accept(false, source, timing, start), "未配置来源上下文保留通用采集行为");
        expect(!gate.accept(true, source, timing, start), "来源不可用不采集界面");
        source.available = true;
        source.focused = false;
        source.session_id = 1;
        expect(!gate.accept(true, source, timing, start), "游戏失焦不采集");
        source.focused = true;
        auto now = start + std::chrono::milliseconds(10);
        expect(!gate.accept(true, source, timing, now), "恢复焦点不接收此前排队画面");
        timing.source_time_at = now + std::chrono::milliseconds(1);
        now += std::chrono::milliseconds(2);
        expect(gate.accept(true, source, timing, now), "焦点稳定后新帧无需按键或检测框即可采集");
        timing.source_clock_uncertainty_ms = 2.0;
        expect(!gate.accept(true, source, timing, now), "源时间不确定区间跨过聚焦边界时不得采集");
        timing.source_clock_uncertainty_ms = -1.0;
        expect(!gate.accept(true, source, timing, now), "无效时间不确定度不得通过采集门");
        timing.source_clock_uncertainty_ms = 0.0;
        timing.source_time_timing_valid = false;
        expect(!gate.accept(true, source, timing, now), "无法对齐来源时间不冒充游戏新帧");
        timing.source_time_timing_valid = true;
        source.session_id = 2;
        expect(!gate.accept(true, source, timing, now), "来源会话更换丢弃旧画面");
        timing.source_time_at = now + std::chrono::milliseconds(1);
        expect(!gate.accept(true, source, timing, now), "未来来源时间不能通过采集门");
        now += std::chrono::milliseconds(2);
        expect(gate.accept(true, source, timing, now), "新会话新帧恢复采集");
        source.available = false;
        expect(!gate.accept(true, source, timing, now), "上下文失效立即关闭采集门");
        source.available = true;
        now += std::chrono::milliseconds(2);
        expect(!gate.accept(true, source, timing, now), "同一会话失信后也重新隔离积压帧");
        timing.source_time_timing_valid = false;
        timing.captured_at = now + std::chrono::milliseconds(1);
        now += std::chrono::milliseconds(2);
        expect(gate.accept(true, source, timing, now, false), "无源映射后端保留本机新帧采集");
        timing.captured_at = start;
        expect(!gate.accept(true, source, timing, now, false), "无源映射后端也拒绝本机积压旧帧");
    }
    for (int i = 0; i < 3; ++i) {
        CapturedFrame captured;
        captured.width = captured.height = 320;
        captured.source_width = 2560;
        captured.source_height = 1440;
        captured.roi_x = 1120;
        captured.roi_y = 560;
        captured.timing.sequence = 20 + i;
        captured.timing.captured_at = start + std::chrono::milliseconds(i * 4);
        const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, i * 2, 0, 1, 0);
        cv::warpAffine(texture, captured.bgr, transform, texture.size(),
                       cv::INTER_LINEAR, cv::BORDER_REFLECT);
        auto prepared = runtime::detail::prepare_aim_frame(captured,
            {{150.0f + i * 2, 140.0f, 180.0f + i * 2, 200.0f, 0.95f, 0}},
            clock, estimator, true);
        expect(prepared.frame.control_center_x == 160 && prepared.frame.control_center_y == 160,
               "生产组装必须保留主机 FOV 到 ROI 的中心转换");
        expect(prepared.frame.control_at >= captured.timing.captured_at &&
                   prepared.background_motion_ms >= 0,
               "生产控制时间必须在图像观测之后取值");
        // 离线使用显式原控制时间；生产入口的取时先单独检查。
        prepared.frame.control_at = captured.timing.captured_at + std::chrono::milliseconds(2);
        if (prepared.reset_aim) aim.reset();
        const auto result = aim.process(prepared.frame);
        if (i == 0) {
            expect(prepared.frame.background_motion_x.status == AimBackgroundMotionStatus::WARMING,
                   "首张图像不能制造背景零位移");
        } else {
            expect(prepared.frame.background_motion_x.status == AimBackgroundMotionStatus::VALID,
                   "真实生产估计器必须提供有效背景位移");
            expect(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                   "组装到公开 Aim 必须真正消费背景，漏接线应失败");
            expect(std::fabs(result.control.observer_camera_motion_x_source_pixels - 2.0f) < 0.2f,
                   "完整组装路径保持背景方向及像素单位");
        }
    }
    estimator.reset();
    {
        // r4形态：Runtime重置清历史，激活首帧仍在确认目标，下一帧才获目标。
        // 必须经过真实组装入口，不能只在Aim内部保留上一帧lock掩盖reset。
        AimConfig cfg; cfg.min_confirmed_hits = 2;
        cfg.max_counts_per_frame = 14; cfg.soft_zone_radius_percent = 30;
        Aim reset_aim(cfg);
        runtime::detail::RuntimeObservationClock reset_clock;
        runtime::detail::CameraMotionEstimator reset_estimator;
        bool emitted = false;
        bool emitted_after_reset = false;
        for (int i = 0; i < 70; ++i) {
            CapturedFrame captured;
            captured.width = captured.height = captured.source_width = captured.source_height = 320;
            captured.timing.sequence = 100 + i;
            captured.timing.captured_at = start + std::chrono::milliseconds(100 + 4*i);
            auto prepared = runtime::detail::prepare_aim_frame(captured,
                {{210,140,250,240,.95f,0}}, reset_clock, reset_estimator, true, false);
            prepared.frame.control_at = captured.timing.captured_at + std::chrono::milliseconds(2);
            if (i == 0 || i == 36 || prepared.reset_aim) reset_aim.reset();
            const auto result = reset_aim.process(prepared.frame);
            expect(result.status == AimStatus::SUCCESS,"生产重置回归输入必须有效");
            if (i == 0) expect(!result.has_target,"首个激活帧必须实际经过目标待确认");
            if (i == 1) expect(result.has_target && !result.has_command,
                "Runtime重置后首次确认也须从零额度接管，不能满幅冷启动");
            if (i == 36) expect(!result.has_target,"运行中完整reset须清旧目标历史");
            if (i == 37) expect(result.has_target && !result.has_command,
                "已经输出后再次完整reset也不得绕过首次确认缓入");
            const int active_steps = i < 36 ? i-1 : i-37;
            if (active_steps > 0 && active_steps < 25) expect(std::hypot(result.control.shaped_x_counts,
                result.command.dy_counts) <= 14.0f * active_steps*.004f/.1f + 1.0f,
                "Runtime重置后的后续请求必须遵守整段有效时间额度");
            if (i > 1) emitted |= result.has_command;
            if (i > 62) emitted_after_reset |= result.has_command;
        }
        expect(emitted,"重置过渡后必须实际恢复输出，不能恒零通过");
        expect(emitted_after_reset,"再次重置完整过渡后仍须恢复输出");
    }
    {
        weapon::GsiConfig gsi_config;
        gsi_config.enabled = true;
        weapon::detail::GsiState gsi;
        AimConfig session_config;
        session_config.min_confirmed_hits = 1;
        session_config.deadzone_pixels = 0;
        Aim session_aim(session_config);
        runtime::detail::AimWeaponSessionGate session_gate;
        const source_context::SourceContextSnapshot focus{true, true, 1, 1, 0};
        for (int step = 0; step != 6; ++step) {
            const auto now = start + std::chrono::milliseconds(step * 4);
            gsi.ingest(weapon_payload(step == 1 ? "reloading" : "active", step == 2 ? 0 : 30 - step,
                step >= 4 ? "weapon_deagle" : "weapon_ak47"),
                gsi_config, now, 1700000000000);
            const auto weapon = gsi.snapshot(now);
            expect(weapon.status == (step == 1 ? weapon::Status::RELOADING :
                step == 2 ? weapon::Status::EMPTY : weapon::Status::READY),
                "生产 GSI 正确发布 READY、RELOADING 和 EMPTY");
            const auto session = session_gate.update(weapon, true, focus, true, true, now);
            if (session.reset_aim) session_aim.reset();
            if (step == 1 || step == 3 || step == 4)
                expect(session.reset_aim, "普通暂停、恢复及主动切枪必须重选目标和清理控制历史");
            AimFrame frame;
            frame.sequence = step + 1;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160;
            frame.captured_at = now;
            frame.control_at = now + std::chrono::milliseconds(1);
            frame.lock_active = session.allowed;
            if (step != 4) frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
            const auto result = session_aim.process(frame);
            const auto current_session = session_gate.update(gsi.snapshot(now), true, focus, true, true, now);
            const bool dispatched = runtime::detail::aim_frame_dispatch_allowed(frame, true, session, current_session);
            if (step == 4) {
                expect(!result.has_target && !result.has_command,
                    "步枪切到手枪时空的新观测不能继承旧枪滑行目标或命令");
                expect(dispatched, "正常切枪保留持键恢复资格，但仍须新目标才能生成命令");
                continue;
            }
            expect(result.has_command, "生产 GSI 回归经实际 Aim 生成命令");
            expect(dispatched == (step == 0 || step >= 3),
                "持续持键时换弹和空弹暂停 Aim，恢复 READY 后自动恢复");
            expect(session_aim.record_backend_completed_command(result.command.sequence, frame.control_at,
                dispatched ? result.command.dx_counts : 0, dispatched ? result.command.dy_counts : 0),
                "武器暂停发送零反馈，不遗留预计算命令库存");
        }
    }
    {
        weapon::GsiConfig gsi_config;
        gsi_config.enabled = true;
        weapon::detail::GsiState gsi;
        runtime::detail::AimWeaponSessionGate gate;
        const source_context::SourceContextSnapshot focus{true, true, 1, 1, 0};
        gsi.ingest(weapon_payload("active", 30), gsi_config, start, 1700000000000);
        const auto prepared = gate.update(gsi.snapshot(start), true, focus, true, true, start);
        AimConfig delayed_config;
        delayed_config.min_confirmed_hits = 1;
        delayed_config.deadzone_pixels = 0;
        Aim delayed_aim(delayed_config);
        AimFrame frame;
        frame.sequence = 1; frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.captured_at = start; frame.control_at = start + std::chrono::milliseconds(1);
        frame.lock_active = prepared.allowed;
        frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
        const auto result = delayed_aim.process(frame);
        expect(result.has_command, "发送前切枪回归先计算真实 Aim 命令");
        gsi.ingest(weapon_payload("active", 7, "weapon_deagle"), gsi_config, frame.control_at, 1700000000000);
        // 消费者跳过中间手枪状态，回到步枪仍必须撤销已计算的旧会话命令。
        gsi.ingest(weapon_payload("active", 29), gsi_config, frame.control_at, 1700000000000);
        const auto current = gate.update(gsi.snapshot(frame.control_at), true, focus, true, true, frame.control_at);
        expect(current.allowed && current.reset_aim &&
            !runtime::detail::aim_frame_dispatch_allowed(frame, true, prepared, current),
            "发送前 A→B→A 也须拒绝旧命令，但不要求正常切枪松键");
        expect(delayed_aim.record_backend_completed_command(result.command.sequence, frame.control_at, 0, 0),
            "发送前会话撤销先完成零反馈再 reset，不制造未知历史");
        delayed_aim.reset();
        ++frame.sequence; frame.captured_at += std::chrono::milliseconds(4);
        frame.control_at += std::chrono::milliseconds(4); frame.detections.clear();
        expect(!delayed_aim.process(frame).has_target, "切枪后的新帧不得继承被撤销命令对应目标");
    }
    for (const auto& item : std::array<std::pair<const char*, const char*>, 3>{{
            {"weapon_knife", "Knife"}, {"weapon_flashbang", "Grenade"}, {"weapon_c4", "C4"}}}) {
        weapon::GsiConfig gsi_config;
        gsi_config.enabled = true;
        weapon::detail::GsiState gsi;
        runtime::detail::AimWeaponSessionGate gate;
        const source_context::SourceContextSnapshot focus{true, true, 1, 1, 0};
        AimConfig local_config;
        local_config.min_confirmed_hits = 1;
        local_config.deadzone_pixels = 0;
        Aim current_aim(local_config);
        gsi.ingest(weapon_payload("active", 30), gsi_config, start, 1700000000000);
        const auto initial_weapon = gsi.snapshot(start);
        const auto initial_session = gate.update(initial_weapon, true, focus, true, true, start);
        AimFrame frame;
        frame.sequence = 1; frame.roi_width = frame.roi_height = 320;
        frame.control_center_x = frame.control_center_y = 160;
        frame.captured_at = start; frame.control_at = start + std::chrono::milliseconds(1);
        frame.lock_active = initial_session.allowed;
        frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
        const auto prepared = current_aim.process(frame);
        expect(initial_session.allowed && prepared.has_command, "非枪切换前实际Aim已有旧武器命令");
        auto nonfirearm_payload = weapon_payload("active", 0, item.first);
        const auto insertion = nonfirearm_payload.find(",\"ammo_clip\"");
        nonfirearm_payload.replace(insertion, nonfirearm_payload.size() - insertion,
            std::string(",\"type\":\"") + item.second + "\"}}}}");
        const auto changed_at = start + std::chrono::milliseconds(2);
        gsi.ingest(nonfirearm_payload, gsi_config, changed_at, 1700000000000);
        const auto nonfirearm = gsi.snapshot(changed_at);
        const auto paused = gate.update(nonfirearm, true, focus, true, true, changed_at);
        expect(!nonfirearm.valid && nonfirearm.control_safety_epoch == initial_weapon.control_safety_epoch &&
            runtime::detail::weapon_session_trusted(nonfirearm, changed_at),
            "已识别刀手雷C4无需弹匣字段且仅暂停开火，不打断控制会话信任");
        expect(!paused.allowed && paused.reset_aim &&
            !runtime::detail::aim_frame_dispatch_allowed(frame, true, initial_session, paused),
            "切非枪立即拒绝已计算的旧枪Aim命令并撤销旧目标");
        expect(current_aim.record_backend_completed_command(prepared.command.sequence, changed_at, 0, 0),
            "被非枪撤销的Aim命令记录零反馈");
        current_aim.reset();
        const auto ready_at = start + std::chrono::milliseconds(4);
        gsi.ingest(weapon_payload("active", 29), gsi_config, ready_at, 1700000000000);
        const auto recovered = gate.update(gsi.snapshot(ready_at), true, focus, true, true, ready_at);
        expect(recovered.allowed && recovered.reset_aim,
            "非枪回枪持续许可直接恢复新Aim会话，不要求松方向或功能键");
        if (recovered.reset_aim) current_aim.reset();
        ++frame.sequence; frame.captured_at = ready_at; frame.control_at = ready_at + std::chrono::milliseconds(1);
        frame.lock_active = recovered.allowed; frame.detections.clear();
        expect(!current_aim.process(frame).has_target, "非枪恢复空观测不能继承旧枪目标");
        ++frame.sequence; frame.captured_at += std::chrono::milliseconds(4); frame.control_at += std::chrono::milliseconds(4);
        frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
        const auto fresh = current_aim.process(frame);
        expect(fresh.has_command && runtime::detail::aim_frame_dispatch_allowed(frame, true, recovered, recovered),
            "恢复后新观测经实际Aim产生可发送新命令");
    }
    for (int failure = 0; failure != 7; ++failure) {
        weapon::GsiConfig gsi_config;
        gsi_config.enabled = true;
        weapon::detail::GsiState gsi;
        runtime::detail::AimWeaponSessionGate gate;
        source_context::SourceContextSnapshot focus{true, true, 1, 1, 0};
        gsi.ingest(weapon_payload("active", 30), gsi_config, start, 1700000000000);
        expect(gate.update(gsi.snapshot(start), true, focus, true, true, start).allowed,
            "安全负例先建立健康武器会话");
        auto now = start + std::chrono::milliseconds(1);
        if (failure == 0) now = start + std::chrono::seconds(3);
        if (failure == 1) gsi.ingest(weapon_payload("active", 29, "weapon_ak47", 100, "76561198000000001"),
            gsi_config, now, 1700000000000);
        if (failure == 2) gsi.ingest(weapon_payload("active", 29, "weapon_unknown"), gsi_config, now, 1700000000000);
        if (failure == 3 || failure == 6)
            gsi.ingest(weapon_payload("active", 29, "weapon_ak47", 0), gsi_config, now, 1700000000000);
        if (failure == 4) focus.focused = false;
        if (failure == 5) focus.session_id = 2;
        if (failure != 6) expect(!gate.update(gsi.snapshot(now), true, focus, true, true, now).allowed,
            "过期、身份异常、未知武器、死亡、失焦和来源会话变化均撤销 Aim 许可");
        // 新报文恢复健康；case 6 的消费者完全未看到中间死亡快照。
        now += std::chrono::milliseconds(1);
        gsi.ingest(weapon_payload("active", 28, "weapon_ak47", 100, "76561198000000000", 1700000004),
            gsi_config, now, 1700000004000);
        focus.focused = true;
        expect(!gate.update(gsi.snapshot(now), true, focus, true, true, now).allowed,
            "信任中断恢复后持续持键仍不得自启动，跳过死亡也由持久代际保护");
        gate.update(gsi.snapshot(now), true, focus, true, false, now);
        expect(gate.update(gsi.snapshot(now), true, focus, true, true, now).allowed,
            "健康时真实松键后允许新 Aim 会话");
    }
    {
        runtime::detail::AimWeaponSessionGate gate;
        expect(gate.update({}, false, {}, false, true, start).allowed,
            "未启用 GSI 和源焦点的纯视觉配置保留现有许可路径");
    }
    {
        AimFrame delayed;
        delayed.captured_at = start;
        delayed.control_at = start + std::chrono::milliseconds(200);
        const auto deadline = runtime::detail::aim_output_slot_deadline(delayed, std::chrono::milliseconds(305));
        expect(deadline == start + AimFrame::kObservationHorizon && deadline < delayed.control_at,
               "后端预算不能放行超出既有模型帧龄的旧观测");
        delayed.control_at = start + std::chrono::milliseconds(2);
        expect(runtime::detail::aim_output_slot_deadline(delayed, std::chrono::milliseconds(5)) ==
                   start + std::chrono::milliseconds(7), "较短事务预算仍限制输出等待，不能被帧龄预算放大");
    }
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        using namespace runtime::detail;
        const auto fresh_frame = [] (std::uint64_t sequence) {
            AimFrame frame;
            frame.sequence = sequence;
            frame.captured_at = frame.control_at = Clock::now();
            frame.lock_active = true;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160;
            frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
            return frame;
        };
        for (bool occupied : {false, true}) {
            SafetyGate gate;
            gate.set_input_health(true);
            expect(gate.arm(), "时隙回归必须建立健康武装前置");
            gate.set_hold(true);
            AutoStopOutputArbiter arbiter;
            auto frame = fresh_frame(400);
            AimOutputSlot rejected;
            if (occupied) {
                auto owner = arbiter.try_enter_cleanup();
                expect(owner.owns_lock(), "超时回归必须实际持有共享事务锁");
                auto waiter = std::async(std::launch::async, [&] {
                    frame = fresh_frame(400);
                    return acquire_aim_output_slot(frame, gate, arbiter, 5ms);
                });
                rejected = waiter.get();
                owner.unlock();
            } else {
                frame.captured_at -= 101ms;
                rejected = acquire_aim_output_slot(frame, gate, arbiter, 305ms);
            }
            expect(!rejected.guard.owns_lock() && !frame.lock_active &&
                (rejected.rejection.reason == AimDispatchRejection::ENTRY_DEADLINE_EXPIRED ||
                    (occupied && rejected.rejection.reason == AimDispatchRejection::WAIT_DEADLINE_EXPIRED)),
                "旧观察与真实锁超期须分类拒绝且当前帧禁发");
            expect(gate.can_dispatch() && !gate.visual_output_blocked() && !gate.emergency_stopped(),
                "单帧时隙超期不得锁存视觉阻断或制造急停");
            expect(rejected.reset_aim_only, "普通时限拒绝仅重置Aim控制状态，不请求源世代重建");
            auto next = fresh_frame(401);
            auto resumed = acquire_aim_output_slot(next, gate, arbiter, 305ms);
            expect(resumed.guard.owns_lock() && aim_frame_dispatch_allowed(next, gate.can_dispatch()),
                "时限拒绝后新观察必须重新获得真实事务门，无需重新武装");
            SlotFakeMouse mouse;
            AimConfig resumed_config;
            resumed_config.min_confirmed_hits = 1;
            resumed_config.deadzone_pixels = 0;
            resumed_config.smoothing = 1;
            Aim resumed_aim(resumed_config);
            const auto result = resumed_aim.process(next);
            expect(result.has_command, "恢复帧须通过公开Aim生成实际非零命令");
            if (result.has_command && resumed.guard.owns_lock() &&
                    aim_frame_dispatch_allowed(next, gate.can_dispatch()) &&
                    aim_output_fresh_before_send(next, gate, resumed, Clock::now())) {
                const auto receipt = mouse.move({result.command.dx_counts, result.command.dy_counts});
                expect(resumed_aim.record_backend_completed_command(result.command.sequence,
                    receipt.backend_completed_at, result.command.dx_counts, result.command.dy_counts),
                    "恢复帧Fake输出须通过真实Aim完成回执");
            }
            expect(mouse.calls == 1, "普通拒绝后的新鲜帧须实际经过Fake输出一次");
            AimDispatchRejectionSummary summary;
            record_aim_dispatch_rejection(summary, rejected.rejection);
            record_aim_dispatch_rejection(summary, resumed.rejection);
            expect(summary.total == 1 && summary.first.sequence == 400 && summary.last.sequence == 400 &&
                summary.first.reason == rejected.rejection.reason && summary.first.wait_ms >= 0 &&
                summary.first.observation_age_ms >= 0,
                "拒绝累计须保留原帧与原因，不把随后成功写为拒绝");
        }
        for (bool during_wait : {false, true}) {
            SafetyGate gate;
            gate.set_input_health(true); gate.arm(); gate.set_hold(true);
            AutoStopOutputArbiter arbiter;
            auto frame = fresh_frame(500);
            AimOutputSlot rejected;
            if (during_wait) {
                auto owner = arbiter.try_enter_cleanup();
                std::promise<void> started;
                auto ready = started.get_future();
                auto waiter = std::async(std::launch::async, [&] {
                    started.set_value();
                    return acquire_aim_output_slot(frame, gate, arbiter, 305ms);
                });
                ready.wait();
                arbiter.latch_output_fault();
                owner.unlock();
                rejected = waiter.get();
            } else {
                arbiter.latch_output_fault();
                rejected = acquire_aim_output_slot(frame, gate, arbiter, 305ms);
            }
            expect(!rejected.guard.owns_lock() && rejected.rejection.reason == AimDispatchRejection::OUTPUT_FAULT &&
                gate.emergency_stopped() && !gate.can_dispatch(), "共享未知输出仍须急停，不能按瞬时丢帧恢复");
            expect(!rejected.reset_aim_only, "硬故障不能误归类为Aim局部恢复");
            auto next = fresh_frame(501);
            auto refused = acquire_aim_output_slot(next, gate, arbiter, 305ms);
            expect(!refused.guard.owns_lock() && !gate.arm(), "新观察不得清除共享故障或重新武装");
            expect(arbiter.try_enter_cleanup().owns_lock(), "硬故障仍须保留设备清理入口");
        }
        SafetyGate gate;
        gate.set_input_health(true); gate.arm(); gate.set_hold(true);
        AutoStopOutputArbiter arbiter;
        auto frame = fresh_frame(600);
        AimConfig feedback_config;
        feedback_config.min_confirmed_hits = 1;
        feedback_config.deadzone_pixels = 0;
        feedback_config.smoothing = 1;
        feedback_config.counts_per_pixel_x = feedback_config.counts_per_pixel_y = 1;
        feedback_config.max_counts_per_frame = 100;
        feedback_config.enable_delay_compensation = true;
        feedback_config.control_delay_ms = 15;
        Aim feedback(feedback_config);
        Aim completion_witness(feedback_config);
        auto slot = acquire_aim_output_slot(frame, gate, arbiter, 305ms);
        expect(slot.guard.owns_lock(), "计算后过期回归必须先实际获锁");
        const auto calculated = feedback.process(frame);
        const auto witness_command = completion_witness.process(frame);
        expect(calculated.has_command, "计算后过期回归须存在真实Aim预计算命令");
        const auto expired_at = slot.deadline + 1ns;
        expect(!aim_output_fresh_before_send(frame, gate, slot, expired_at) && !frame.lock_active &&
            slot.rejection.reason == AimDispatchRejection::COMPUTE_DEADLINE_EXPIRED,
            "计算跨deadline必须拒绝本帧发送");
        expect(feedback.record_backend_completed_command(calculated.command.sequence, expired_at, 0, 0),
            "计算超期未发送仍须以零完成原位结算，不能留下虚构库存");
        expect(witness_command.has_command && completion_witness.record_backend_completed_command(
            witness_command.command.sequence, expired_at, 0, 0),
            "独立库存见证必须接受同一未发送命令的零完成记录");
        slot.guard.unlock();
        finish_aim_output_slot(feedback, slot);
        frame = fresh_frame(601);
        frame.captured_at = expired_at + 1ms;
        frame.control_at = frame.captured_at;
        auto resumed = acquire_aim_output_slot(frame, gate, arbiter, 305ms);
        const auto after = feedback.process(frame);
        const auto unreset_after = completion_witness.process(frame);
        expect(after.control.pending_net_x_counts == 0 && unreset_after.control.pending_net_x_counts == 0,
            "超期零回执本身不得形成净库存，不能仅靠后续reset掩盖未发送命令");
        expect(resumed.guard.owns_lock() && gate.can_dispatch(), "计算超期后的新帧同样须保留恢复资格");
        resumed.guard.unlock();
        gate.block_visual_output();
        auto blocked_frame = fresh_frame(602);
        auto blocked = acquire_aim_output_slot(blocked_frame, gate, arbiter, 305ms);
        expect(!blocked.guard.owns_lock() && gate.visual_output_blocked(),
            "瞬时拒绝恢复不得清除其他原因已建立的视觉硬阻断");

        RuntimeObservationClock observation_clock;
        CameraMotionEstimator camera;
        Aim local_aim(feedback_config);
        std::uint64_t epoch = 0;
        const auto observed_at = Clock::now() - 200ms;
        for (int i = 0; i < 3; ++i) {
            CapturedFrame captured;
            captured.width = captured.height = captured.source_width = captured.source_height = 320;
            captured.bgr = texture;
            captured.timing.sequence = 700 + i;
            captured.timing.captured_at = observed_at + i * 4ms;
            auto prepared = prepare_aim_frame(captured, {}, observation_clock, camera, true);
            if (i == 0) epoch = prepared.frame.observation_epoch;
            else expect(!prepared.reset_aim && prepared.frame.observation_epoch == epoch &&
                    prepared.frame.background_motion_x.status == AimBackgroundMotionStatus::VALID,
                "普通时限拒绝后的连续图像必须保持观察世代和背景测量，不自造Trigger换源");
            SafetyGate local_gate;
            local_gate.set_input_health(true); local_gate.arm(); local_gate.set_hold(true);
            AutoStopOutputArbiter local_arbiter;
            auto expired = acquire_aim_output_slot(prepared.frame, local_gate, local_arbiter, 305ms);
            expect(expired.reset_aim_only, "连续观察回归必须真实经过旧帧拒绝");
            local_aim.process(prepared.frame);
            finish_aim_output_slot(local_aim, expired);
        }
    }
    for (bool frame_permission : {false, true}) {
        for (bool current_permission : {false, true}) {
            AimConfig feedback_config;
            feedback_config.min_confirmed_hits = 1;
            feedback_config.deadzone_pixels = 0;
            feedback_config.smoothing = 1;
            feedback_config.counts_per_pixel_x = feedback_config.counts_per_pixel_y = 1;
            feedback_config.max_counts_per_frame = 100;
            feedback_config.enable_delay_compensation = true;
            feedback_config.control_delay_ms = 15;
            Aim feedback_aim(feedback_config);
            AimFrame frame;
            frame.sequence = 1;
            frame.roi_width = frame.roi_height = 320;
            frame.control_center_x = frame.control_center_y = 160;
            frame.captured_at = start;
            frame.control_at = start + std::chrono::milliseconds(1);
            frame.lock_active = frame_permission;
            frame.detections = {{180, 120, 220, 200, 0.95f, 0}};
            const auto result = feedback_aim.process(frame);
            expect(result.status == AimStatus::SUCCESS && result.has_command,
                   "反馈回归必须通过公开Aim接口生成非零预计算命令");
            // 同步改变发送时许可，复现组帧后按键变化，无需线程时序或真实设备。
            const bool dispatched = runtime::detail::aim_frame_dispatch_allowed(frame, current_permission);
            expect(dispatched == (frame_permission && current_permission),
                   "帧计算时未许可不得被后来按键追溯发送；发送前撤销仍必须拒绝");
            expect(feedback_aim.record_backend_completed_command(result.command.sequence,
                       frame.control_at + std::chrono::microseconds(100),
                       dispatched ? result.command.dx_counts : 0,
                       dispatched ? result.command.dy_counts : 0),
                   "Runtime生产发送门后的实际反馈必须与Aim预计算历史一致");
            if (!frame_permission && current_permission) {
                ++frame.sequence;
                frame.captured_at += std::chrono::milliseconds(4);
                frame.control_at += std::chrono::milliseconds(4);
                frame.lock_active = current_permission;
                const auto next = feedback_aim.process(frame);
                expect(next.has_target && !next.has_command &&
                           runtime::detail::aim_frame_dispatch_allowed(frame, current_permission),
                       "新按键许可生效但接管首步零额度，不追溯发送旧帧");
                bool recovered = false;
                for (int i=0;i<30;++i) {
                    ++frame.sequence;
                    frame.captured_at += std::chrono::milliseconds(4);
                    frame.control_at += std::chrono::milliseconds(4);
                    const auto continued = feedback_aim.process(frame);
                    expect(continued.has_target && runtime::detail::aim_frame_dispatch_allowed(frame,current_permission),
                           "持续按住保持许可和目标，不要求松键重按");
                    if (continued.has_command) {
                        recovered = true;
                        expect(feedback_aim.record_backend_completed_command(continued.command.sequence,
                            frame.control_at + std::chrono::microseconds(100),
                            continued.command.dx_counts,continued.command.dy_counts),"接管后真实发送记录须可确认");
                    }
                }
                expect(recovered,"持续按住须在过渡后恢复非零输出");
            }
        }
    }
    expect(failures == 0, "Runtime/Aim 组装合同失败");
    return failures ? 1 : 0;
}
