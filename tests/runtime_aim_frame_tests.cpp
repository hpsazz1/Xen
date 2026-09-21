#include "runtime/aim_frame_internal.h"
#include "weapon/weapon_internal.h"

#include <cmath>
#include <iostream>
#include <opencv2/imgproc.hpp>

namespace {
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
                expect(next.has_command && runtime::detail::aim_frame_dispatch_allowed(frame, current_permission) &&
                           feedback_aim.record_backend_completed_command(next.command.sequence,
                               frame.control_at + std::chrono::microseconds(100),
                               next.command.dx_counts, next.command.dy_counts),
                       "按住许可保持时下一帧正常发送并确认，不应要求松键重按");
            }
        }
    }
    expect(failures == 0, "Runtime/Aim 组装合同失败");
    return failures ? 1 : 0;
}
