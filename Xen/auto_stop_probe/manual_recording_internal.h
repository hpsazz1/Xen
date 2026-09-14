#ifndef AUTO_STOP_MANUAL_RECORDING_INTERNAL_H
#define AUTO_STOP_MANUAL_RECORDING_INTERNAL_H
#include "auto_stop_probe/manual_sampling_internal.h"
#include "auto_stop_probe/counterpulse_hud.h"
#include "auto_stop_probe/training_evaluation_internal.h"
#include "runtime/input_training_internal.h"
#include <mutex>
#include <thread>

namespace auto_stop_probe_detail {
inline MouseConfig manual_monitor_config(const MouseConfig& original) {
    if (original.backend != MouseBackend::KMBOX_NET || original.kmbox_ip.empty() ||
        original.kmbox_port == 0 || original.kmbox_uuid.empty())
        throw std::runtime_error("人工录制需要已有KMBOX配置");
    auto result = original;
    result.allow_send_input = false;
    result.kmbox_connect_timeout_ms = std::min(result.kmbox_connect_timeout_ms, 2000);
    return result;
}
struct ManualRecordingResult { Json analysis, archive; };
// 最终报告仅由已关闭且通过清单检查的原始档案生成；不包含Reader提前分析但未落盘的尾部。
inline Json finalize_manual_archive(const std::filesystem::path& raw_directory,
    const SamplingSettings& settings, std::int64_t stopped_at_ns) {
    ManualSamplingAccumulator model(settings);
    input_training::ArchiveSummary archive;
    std::string error;
    std::int64_t last = 0;
    if (!input_training::visit_archive(raw_directory, [&](const input_training::Event& event) {
        model.consume(event);
        last = std::max(last, event.received_at_ns);
    }, archive, error)) throw std::runtime_error("人工原始归档未完成，不能生成最终采样报告");
    if (stopped_at_ns < last || stopped_at_ns < 0 || stopped_at_ns > INT64_MAX - 100000000000LL)
        throw std::runtime_error("停止水位早于已归档事件或超出时间范围");
    // 事件上的gap已在准确位置回放；尾部缺口另外使未结束hold失效。
    if (archive.trailing_gap) model.gap();
    auto analysis = model.snapshot(stopped_at_ns, false);
    analysis["archive_status"] = input_training::status_name(archive.status);
    analysis["archive_dropped_events"] = archive.dropped;
    analysis["archive_trailing_gap"] = archive.trailing_gap;
    analysis["archive_complete"] = archive.status == input_training::Status::STOPPED &&
        archive.events != 0 && !archive.dropped && !archive.trailing_gap &&
        analysis.value("invalid_events", 0ULL) == 0 && analysis.value("gap_count", 0ULL) == 0;
    analysis["finalized_from_persisted_raw"] = true;
    return analysis;
}
// 只有显式人工入口调用；工厂配置固定禁用软件输入，既有独占租约继续生效。
inline ManualRecordingResult record_manual_monitor(const MouseConfig& original,
    const std::filesystem::path& directory, const SamplingSettings& settings, int duration_ms,
    CounterpulseHud& hud, const std::function<bool()>& canceled) {
    struct State {
        std::mutex mutex;
        ManualSamplingAccumulator model;
        explicit State(const SamplingSettings& settings) : model(settings) {}
    };
    auto state = std::make_shared<State>(settings);
    std::shared_ptr<IMouseController> device = MouseDeviceFactory::create(manual_monitor_config(original));
    struct Close { std::shared_ptr<IMouseController> value; ~Close() { if (value) value->close(); } } close{device};
    if (!device || !device->open() || !device->output_owner_exclusive())
        throw std::runtime_error("人工监听连接或独占失败");
    if (!device->set_input_report_subscription(true)) throw std::runtime_error("人工监听订阅失败");
    auto source = std::make_shared<runtime::detail::InputTrainingSource>(device);
    input_training::Session session;
    if (!session.start(directory / "raw", {}, [source, state] {
        auto batch = source->read();
        std::lock_guard lock(state->mutex);
        if (batch.gap || batch.dropped_events || batch.trailing_gap) state->model.gap();
        for (const auto& event : batch.events) state->model.consume(event);
        return batch;
    })) throw std::runtime_error("人工原始归档启动失败");
    const auto deadline = Clock::now() + std::chrono::milliseconds(duration_ms);
    while (Clock::now() < deadline && !canceled() && !hud.closed() && !hud.stop_requested()) {
        const auto archive = session.snapshot();
        if (!archive || archive->status != input_training::Status::RECORDING) break;
        {
            std::lock_guard lock(state->mutex);
            auto snapshot = state->model.snapshot(ns(Clock::now()), true);
            snapshot["recording"] = true;
            snapshot["recording_id"] = directory.filename().string();
            hud.publish(snapshot);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    device->freeze_input_reports();
    const auto stopped_at_ns = ns(Clock::now());
    session.stop();
    if (session.snapshot()->status == input_training::Status::STOP_TIMEOUT)
        throw std::runtime_error("人工归档停止超时，不能将前缀当成完成记录");
    // Session的最终Reader调用排空冻结水位；只有之后才能关闭monitor。
    source.reset();
    device->close();
    ManualRecordingResult result;
    result.analysis = finalize_manual_archive(directory / "raw", settings, stopped_at_ns);
    result.archive = training_snapshot_json(*session.snapshot(), "KMBOX_MONITOR");
    result.analysis["recording"] = false;
    result.analysis["recording_id"] = directory.filename().string();
    result.analysis["archive"] = result.archive;
    result.analysis["software_input_output"] = false;
    result.analysis["physical_validation_passed"] = false;
    hud.publish(result.analysis);
    return result;
}
}
#endif
