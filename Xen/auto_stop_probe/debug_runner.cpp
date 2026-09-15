#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include "config/config.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/readiness_internal.h"
#include "auto_stop_probe/preroll_internal.h"
#include "auto_stop_probe/training_evaluation_internal.h"
#include "auto_stop_probe/hud_feedback_internal.h"
#include "auto_stop_probe/sampling_analysis_internal.h"
#include "auto_stop_probe/debug_report_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "auto_stop_probe/manual_recording_internal.h"
#include "auto_stop_probe/manual_labels_internal.h"
#include "auto_stop_probe/default_baseline_internal.h"
#include "runtime/input_training_internal.h"
#include "source_context/source_context.h"

#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/debug_resources_internal.h"
#include "auto_stop_probe/capture_evidence_internal.h"
namespace auto_stop_probe_detail {
namespace {
void write_json(const std::filesystem::path& path, const Json& report) {
    auto temporary = path; temporary += ".writing";
    {
        std::ofstream file(temporary); file.exceptions(std::ios::badbit | std::ios::failbit);
        file << report.dump(2) << '\n';
        file.flush();
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("报告原子保存失败");
}
void write_analysis_files(const std::filesystem::path& directory, Json analysis) {
    analysis["feedback"] = summarize_hud_feedback(analysis, parse_sampling_settings(analysis.at("settings")));
    if (analysis.contains("operation_intervals")) analysis["operation_intervals"]["recording_id"] = analysis.value("recording_id", "");
    if (analysis.contains("manual_plan_proposals")) analysis["manual_plan_proposals"]["recording_id"] = analysis.value("recording_id", "");
    write_json(directory / "sampling-analysis.json", analysis);
    if (analysis.contains("operation_intervals")) write_json(directory / "operation-intervals.json", analysis["operation_intervals"]);
    if (analysis.contains("manual_plan_proposals")) write_json(directory / "manual-plan-proposals.json", analysis["manual_plan_proposals"]);
    const auto path = directory / "debug-report.html";
    auto temporary = path; temporary += ".writing";
    {
        std::ofstream file(temporary, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << render_counterpulse_debug_report(analysis);
        file.flush();
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("调试报告原子保存失败");
}
Json read_bounded_json(const std::filesystem::path& path, std::uintmax_t limit = 64 * 1024 * 1024) {
    if (std::filesystem::file_size(path) > limit) throw std::runtime_error("输入文件过大");
    std::ifstream stream(path);
    return Json::parse(stream, nullptr, true, true);
}
void describe_manual_quality(Json& analysis) {
    if (!analysis.value("received_events", 0ULL) || analysis.value("invalid_events", 0ULL) ||
        analysis.value("gap_count", 0ULL)) analysis["archive_complete"] = false;
    analysis["quality_issues"] = Json::array();
    for (const auto* key : {"gap_count", "invalid_events", "missing_start_count"})
        if (analysis.value(key, 0ULL)) analysis["quality_issues"].push_back(std::string("MANUAL_") + key);
    if (!analysis.value("received_events", 0ULL)) analysis["quality_issues"].push_back("NO_INPUT_EVENTS");
    if (!analysis.value("archive_complete", false)) analysis["quality_issues"].push_back("ARCHIVE_INCOMPLETE");
    for (const auto& shot : analysis["shots"]) if (!shot.value("complete_hold", false)) {
        analysis["quality_issues"].push_back("INCOMPLETE_HOLD"); break;
    }
    analysis["analysis_complete"] = analysis["quality_issues"].empty();
}
}

Json validate_debug_plan(const Json& value) { return counterpulse_plan_json(parse_counterpulse_plan(value)); }
Json make_fire_test_plan(const Json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("shot_hold_ms") || !value.contains("fire_interval_ms") ||
        !value["shot_hold_ms"].is_number_integer() || !value["fire_interval_ms"].is_number_integer())
        throw std::runtime_error("射击配置仅接受两个整数字段");
    const auto hold = value["shot_hold_ms"].get<std::int64_t>(), interval = value["fire_interval_ms"].get<std::int64_t>();
    if (hold < 1 || hold > 2000 || interval < 1 || interval > 5000 || interval <= hold)
        throw std::runtime_error("射击配置范围无效");
    return validate_debug_plan({{"schema_version",2},{"capture_enabled",false},{"baseline","stationary"},{"shots",15},
        {"fire_delay_ms",1},{"fire_interval_ms",interval},{"move_during_fire_delay",false},{"move_ms",1},
        {"counter_hold_ms",1},{"counter_delay_ms",0},{"shot_after_release_ms",0},{"shot_hold_ms",hold},
        {"late_tolerance_ms",5},{"direction",2}});
}
Json run_debug(const DebugRunRequest& request, const DebugRunCallbacks& callbacks) {
    const auto output = request.output;
    auto config = request.config;
    const auto document = request.plan;
    auto sampling_settings = SamplingSettings{};
    auto hud = request.hud;
    bool local_canceled = false, cleanup_finished = false, created = false, execution_entered = false, subscription_entered = false;
    const auto is_canceled = [&] { return local_canceled || (callbacks.canceled && callbacks.canceled()) ||
        (hud && (hud->closed() || hud->stop_requested())); };
    std::string stage = "VALIDATION", failure_reason;
    Json readiness = {{"reason","NOT_CHECKED"}}, capture_diagnostic = Json::object(), cancellation_context = Json::object();
    auto progress = [&](const char* next) {
        stage = next;
        Json snapshot{{"stage",stage},{"readiness",readiness},{"capture",capture_diagnostic}};
        if (created) write_json(output / "startup.json",snapshot);
        if (callbacks.publish) callbacks.publish(snapshot);
    };
    try {
        if (!request.sampling_settings.empty()) sampling_settings = parse_sampling_settings(request.sampling_settings);
        if (is_canceled()) throw std::runtime_error("任务已取消");
        if (request.mode != DebugRunMode::Counterpulse) {
            if (request.allow_physical_output || !request.confirmation.empty()) throw std::runtime_error("离线及录制拒绝物理授权");
            const auto create_output = [&] {
                if (is_canceled()) throw std::runtime_error("任务已取消");
                if (!std::filesystem::create_directory(output)) throw std::runtime_error("需要新输出目录");
                created = true;
            };
            Json result;
            if (request.mode == DebugRunMode::DeriveDefaults) {
                result = derive_default_baseline(sampling_settings);
                result["settings_source"] = request.sampling_settings.empty() ? "REFERENCE_INITIAL_ASSUMPTIONS" : "USER_SUPPLIED_ASSUMPTIONS";
                create_output();
                write_json(output / "sampling-settings.json",result.at("sampling_settings"));
                write_json(output / "plan.json",result.at("candidate_plan"));
                write_json(output / "default-baseline.json",result);
            } else if (request.mode == DebugRunMode::DeriveManualPlan) {
                const auto analysis = read_bounded_json(request.input / "sampling-analysis.json");
                if (analysis.contains("human_labels") && !analysis["human_labels"].value("recording_usable",true))
                    throw std::runtime_error("用户已排除此录制");
                if (std::filesystem::exists(request.input / "labels.json") &&
                    !read_bounded_json(request.input / "labels.json",16384).value("recording_usable",true))
                    throw std::runtime_error("用户已排除此录制");
                const auto& groups = analysis.at("manual_plan_proposals").at("groups");
                if (!groups.is_array() || groups.size() > 100 || request.candidate_index >= groups.size())
                    throw std::runtime_error("候选索引无效");
                const auto& group = groups.at(request.candidate_index);
                const auto candidate = request.plan.empty() ? group.value("candidate_plan",Json(nullptr)) : request.plan;
                if (!candidate.is_object()) throw std::runtime_error("候选不可直接执行，请人工编辑后校验");
                const auto plan = validate_debug_plan(candidate);
                const auto settings = analysis.at("settings");
                (void)parse_sampling_settings(settings);
                create_output();
                write_json(output / "plan.json",plan);
                write_json(output / "sampling-settings.json",settings);
                result = {{"plan",plan},{"sampling_settings",settings},{"parent_run",request.input.string()},
                    {"candidate_index",request.candidate_index},{"physical_output",false},{"status","PREPARED_NOT_LAUNCHED"}};
                write_json(output / "derived-plan.json",result);
            } else if (request.mode == DebugRunMode::ManualRecording) {
                if (!request.device || !hud || request.recording_duration_ms < 1000 || request.recording_duration_ms > 600000)
                    throw std::runtime_error("人工录制需要设备、HUD和有界时长");
                create_output();
                write_json(output / "sampling-settings.json",sampling_settings_json(sampling_settings));
                write_json(output / "labels.json",{{"schema_version",1},{"recording_id",output.filename().string()},
                    {"recording_usable",true},{"qualified_shot_ranges",Json::array()},{"rejected_shot_ranges",Json::array()},
                    {"note","仅按用户明确反馈填写，未标记不是合格"}});
                auto recorded = record_manual_monitor(config.mouse,output,sampling_settings,request.recording_duration_ms,*hud,is_canceled,request.device);
                result = std::move(recorded.analysis);
                result["archive_complete"] = recorded.archive.value("success",false) &&
                    recorded.archive.value("received_events",0ULL) != 0 && recorded.archive.value("dropped_events",0ULL) == 0 &&
                    recorded.archive.value("invalid_events",0ULL) == 0;
                describe_manual_quality(result);
                result = apply_manual_labels(std::move(result),read_bounded_json(output / "labels.json",16384));
                result["hud_status"] = hud->status();
                write_json(output / "training-evaluation.json",recorded.archive);
                write_analysis_files(output,result);
            } else if (request.mode == DebugRunMode::EvaluateManual) {
                const auto labels = read_bounded_json(request.input / "labels.json",16384);
                if (!labels.value("recording_usable",true)) throw std::runtime_error("用户已排除此录制");
                const auto original = read_bounded_json(request.input / "sampling-settings.json",16384);
                const auto previous = read_bounded_json(request.input / "sampling-analysis.json");
                result = finalize_manual_archive(request.input / "raw",request.sampling_settings.empty() ? parse_sampling_settings(original) : sampling_settings,
                    previous.at("time_ns").get<std::int64_t>());
                result["recording_id"] = request.input.filename().string();
                result["original_sampling_settings"] = original;
                result["sampling_settings_overridden"] = !request.sampling_settings.empty();
                describe_manual_quality(result);
                result = apply_manual_labels(std::move(result),labels);
                create_output();
                write_analysis_files(output,result);
            } else {
                auto commands = read_bounded_json(request.input,4 * 1024 * 1024);
                if (!request.sampling_settings.empty()) {
                    commands["original_sampling_settings"] = commands.value("sampling_settings",Json(nullptr));
                    commands["sampling_settings"] = sampling_settings_json(sampling_settings);
                    commands["sampling_settings_overridden"] = true;
                }
                create_output();
                const auto evaluation = evaluate_counterpulse_training(commands,output / "command-training");
                write_json(output / "training-evaluation.json",{{"schema_version",1},{"command_ack",evaluation},{"monitor",nullptr},{"physical_output",false}});
                result = analyze_counterpulse_sampling(commands);
                result["task_success"] = evaluation.value("success",false);
                write_analysis_files(output,result);
            }
            if (callbacks.publish) callbacks.publish(result);
            return result;
        }
        if (!request.allow_physical_output || request.confirmation != "AUTO_STOP_COUNTERPULSE" || !request.device)
            throw std::runtime_error("需要本轮双授权及独占设备");
        const auto plan = parse_counterpulse_plan(document);
        if (config.mouse.backend != MouseBackend::KMBOX_NET) throw std::runtime_error("设备类型不适用");
        // 注入设备已经建立连接；沿用其实际超时，不修改副本冒充重配。
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("需要不存在的输出目录");
        created = true;
        write_json(output / "plan.json", document);
        progress("SOURCE_CONFIGURATION");
        // 凭据仅进内存，既有生产配置的启用状态不被写回。
        auto source_config = config.source_context;
        source_config.enabled = true;
        if (const char* token = std::getenv("XEN_SOURCE_CONTEXT_TOKEN")) source_config.token = token;
        if (source_config.token.empty() || source_config.host.empty() || source_config.port == 0) {
            failure_reason = "SOURCE_CONFIGURATION_MISSING";
            throw std::runtime_error("缺少源焦点配置或进程环境凭据");
        }
        struct Resources { std::shared_ptr<IMouseController> mouse; source_context::SourceContextClient focus; ~Resources() { focus.stop(); } } resources;
        resources.mouse = request.device;
        progress("SOURCE_START");
        if (!resources.focus.start(source_config)) throw std::runtime_error("源焦点服务不可用");
        if (!resources.mouse || !resources.mouse->output_owner_exclusive()) throw std::runtime_error("需要注入独占设备");
        const auto ready_deadline = Clock::now() + std::chrono::seconds(15);
        std::uint64_t focus_session = 0;
        CounterpulseReadinessAccumulator readiness_gate;
        auto next_status = Clock::now();
        progress("READINESS");
        std::cout << "等待源程序聚焦和键鼠全松，最多15秒；就绪后自动执行一组，End/Ctrl+C停止。\n";
        while (Clock::now() < ready_deadline && !is_canceled()) {
            const auto focus = resources.focus.snapshot();
            InputSnapshot physical;
            const bool polled = resources.mouse->poll_input(physical);
            readiness = readiness_gate.update(focus, physical, polled,
                is_canceled(), ns(Clock::now()));
            if (Clock::now() >= next_status || readiness["ready"].get<bool>()) {
                progress("READINESS");
                next_status = Clock::now() + std::chrono::milliseconds(250);
            }
            if (readiness["flags"]["cancelled"].get<bool>()) { local_canceled = true; break; }
            if (readiness["ready"].get<bool>()) { focus_session = focus.session_id; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (focus_session == 0 || is_canceled()) {
            failure_reason = is_canceled() ? "USER_CANCELLED" : "READINESS_TIMEOUT";
            progress("READINESS");
            throw std::runtime_error("未就绪或已取消");
        }
        subscription_entered = true;
        progress("EVENT_SUBSCRIPTION");
        DebugWasdSubscription wasd_subscription(*resources.mouse);
        if (!wasd_subscription.start()) throw std::runtime_error("原始输入事件不可用");
        std::unique_ptr<Evidence> evidence;
        if (plan.capture_enabled) {
            progress("CAPTURE_OPEN");
            evidence = std::make_unique<Evidence>(config.capture);
        } else {
            capture_diagnostic = {{"enabled", false}, {"reason", "DISABLED_BY_PLAN"}};
        }
        std::unique_ptr<MonitorTraining> training;
        auto cancel = [&]() -> std::string {
            if (is_canceled()) return "USER_STOP";
            if (!cleanup_finished && training && !training->recording()) return "INPUT_TRAINING_STOPPED";
            const auto focus = resources.focus.snapshot();
            if (!focus.available || !focus.focused || focus.session_id != focus_session) return "SOURCE_FOCUS";
            if (!cleanup_finished) {
                InputSnapshot physical;
                const bool polled = resources.mouse->poll_input(physical);
                const auto permission = evaluate_counterpulse_readiness(focus, physical, polled, false);
                if (permission["flags"]["cancelled"].get<bool>()) { local_canceled = true; return "USER_STOP"; }
                if (permission["flags"]["monitor_invalid"].get<bool>()) return "MONITOR_INVALID";
                if (permission["flags"]["physical_keys_held"].get<bool>()) return "PHYSICAL_INPUT";
            }
            if (evidence && evidence->failed.load()) {
                if (cancellation_context.empty()) cancellation_context = evidence->snapshot();
                return "CAPTURE_FAILED";
            }
            if (evidence && evidence->latest_ns.load() != 0 && ns(Clock::now()) - evidence->latest_ns.load() > 100000000) {
                if (cancellation_context.empty()) cancellation_context = evidence->snapshot();
                return "CAPTURE_STALE";
            }
            return {};
        };
        if (evidence) {
            CounterpulsePrerollGate preroll(evidence->opened_ns);
            std::string previous_phase;
            while (true) {
                const auto pre_failure = cancel();
                const auto decision = evidence->evaluate(preroll, !pre_failure.empty());
                const std::string phase = pre_failure.empty() ? decision.reason : pre_failure;
                if (phase != previous_phase) {
                    capture_diagnostic = evidence->snapshot();
                    progress(phase.c_str());
                    previous_phase = phase;
                }
                if (!pre_failure.empty() || decision.state == PrerollState::FAILED) {
                    capture_diagnostic = evidence->snapshot();
                    failure_reason = pre_failure.empty() ? decision.reason : pre_failure;
                    throw std::runtime_error("射前证据或焦点无效");
                }
                if (decision.state == PrerollState::READY) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        progress("EXECUTION");
        if (document.value("schema_version", 0) == 2)
            training = std::make_unique<MonitorTraining>(resources.mouse, output / "monitor-training");
        execution_entered = true;

        auto report = execute_counterpulse(*resources.mouse, plan, cancel, {}, true,
            [&](const Json& command) { if (hud) hud->observe(command); });
        report["sampling_settings"] = sampling_settings_json(sampling_settings);
        if (training) {
            progress("MONITOR_TRAINING_STOP");
            try { finish_monitor_training(*training); }
            catch (const DebugRunFailure&) {
                failure_reason = "MONITOR_TRAINING_STOP_TIMEOUT";
                throw;
            }
        }
        if (!wasd_subscription.finish()) {
            failure_reason = "WASD_SUBSCRIPTION_STOP_FAILED";
            throw std::runtime_error("WASD订阅未结束");
        }

        cleanup_finished = true;
        report["command_timeout_ms"] = config.mouse.kmbox_command_timeout_ms;
        report["cancellation_context"] = cancellation_context;
        report["capture_enabled"] = plan.capture_enabled;
        report["capture_complete"] = false;
        report["capture_frames"] = 0;
        report["scene_settled"] = nullptr;
        // 无采集模式只保存命令结果；不创建采集源，不等待图像或执行图像门禁。
        progress(evidence ? "SAVE_EVIDENCE" : "SAVE_RESULT");
        write_json(output / "result.json", report);
        if (training) {
            const auto command_evaluation = evaluate_counterpulse_training(report, output / "command-training");
            const auto monitor_evaluation = training->report();
            report["monitor_evidence"] = {{"source", "KMBOX_MONITOR"},
                {"received_events", monitor_evaluation.value("received_events", Json(nullptr))},
                {"samples_present", monitor_evaluation.value("samples_present", Json(nullptr))}};
            write_json(output / "training-evaluation.json", {{"schema_version", 1},
                {"command_ack", command_evaluation}, {"monitor", monitor_evaluation},
                {"game_shot_stability", nullptr}, {"scene_settled", nullptr}});
            report["training_archive_success"] = command_evaluation.value("success", false) &&
                monitor_evaluation.value("success", false);
            write_json(output / "result.json", report);
        }
        if (evidence) {
            const auto post_end = Clock::now() + std::chrono::milliseconds(std::max(300, plan.cycle_budget_ms()));
            while (Clock::now() < post_end && cancel().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto post_failure = cancel();
            const bool post_complete = Clock::now() >= post_end && post_failure.empty();
            evidence->save(output / "frames");
            report["capture_frames"] = evidence->count.load();
            report["post_roll_failure"] = post_failure;
            report["last_frame_received_ns"] = evidence->latest_ns.load();
            report["capture_complete"] = !evidence->failed.load() && post_complete;
            report["cancellation_context"] = cancellation_context;
        } else {
            report["capture_status"] = "DISABLED_BY_PLAN";
        }
        write_json(output / "result.json", report);
        if (hud) {
            // 设备已关闭；仅保留显示，让末次短按的延迟样本按真实时间到期。
            std::this_thread::sleep_for(std::chrono::milliseconds(sampling_settings.fire_sample_delay_ms + 40));
            hud->finish(report);
            const auto hud_deadline = Clock::now() + std::chrono::milliseconds(80);
            while (Clock::now() < hud_deadline) {
                const auto state = hud->status();
                if (state["state"] == "FAILED" ||
                    (state["state"] == "FINISHED_VISIBLE" && state["queued_commands"] == 0)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        report["hud_status"] = hud ? hud->status() : Json{{"state", "DISABLED"}, {"physical_validation_passed", false}};
        write_json(output / "result.json", report);
        auto sampling_analysis = analyze_counterpulse_sampling(report);
        write_analysis_files(output, sampling_analysis);
        report["sampling_analysis"] = std::move(sampling_analysis);
        write_json(output / "result.json", report);
        std::cout << "组结束，执行结果与采样调试报告已保存；停稳效果由人工观察判断。\n";
        report["task_success"] = report.value("success", false) && report.value("training_archive_success", true) &&
            (!plan.capture_enabled || report.value("capture_complete", false));
        if (callbacks.publish) callbacks.publish(report);
        return report;
    } catch (...) {
        const auto reason = failure_reason.empty() ? stage + "_FAILED" : failure_reason;
        const bool output_not_started = request.mode == DebugRunMode::Counterpulse && !subscription_entered && !execution_entered &&
            (stage == "VALIDATION" || stage == "SOURCE_CONFIGURATION" || stage == "SOURCE_START" || stage == "READINESS");
        if (created) { try { write_json(output / "failure.json",{{"success",false},{"reason",reason},
            {"stage",stage},{"readiness",readiness},{"capture",capture_diagnostic},{"execution_entered",execution_entered},
            {"output_not_started",output_not_started}}); } catch (...) {} }
        // 仅输出本模块固定原因和阶段，绝不拼接捕获的原异常或配置值。
        const char* description = stage == "SOURCE_CONFIGURATION" ? "源焦点配置缺失或不可用" :
            stage == "SOURCE_START" ? "源焦点服务启动失败" : stage == "READINESS" ? "源焦点或输入未就绪" :
            stage == "VALIDATION" ? "调试参数校验未通过" : "原生调试任务未完成";
        throw DebugRunFailure(std::string(description) + "：" + reason + " [" + stage + "]", output_not_started);
    }
}
}
