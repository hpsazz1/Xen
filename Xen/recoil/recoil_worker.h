#ifndef RECOIL_WORKER_H
#define RECOIL_WORKER_H
#include <functional>
#include <memory>
#include "recoil/recoil.h"
#include "recoil/recoil_config.h"
#include "recoil/recoil_calibration.h"
#include "recoil/motion_ledger.h"
#include "trigger/trigger_worker.h"

enum class RecoilFiringSource { UNKNOWN, INPUT_ESTIMATED, COMMAND_ESTIMATED };
enum class RecoilDispatchRejection {
    NONE, ARBITER_LOCK_BUSY, ARBITER_AUXILIARY_PENDING, ARBITER_OUTPUT_FAULT,
    CONTEXT_CHANGED, DISABLED, NOT_HELD, INPUT_UNHEALTHY, PERMISSION_DENIED,
    NOT_FOCUSED, PROFILE_MISMATCH, EXPIRED, BUDGET_EXCEEDED, CANCELED, STOPPING, CALIBRATION_BUDGET
};
inline const char* RecoilDispatchRejectionName(RecoilDispatchRejection reason) noexcept {
    switch (reason) {
    case RecoilDispatchRejection::NONE: return "NONE";
    case RecoilDispatchRejection::ARBITER_LOCK_BUSY: return "ARBITER_LOCK_BUSY";
    case RecoilDispatchRejection::ARBITER_AUXILIARY_PENDING: return "ARBITER_AUXILIARY_PENDING";
    case RecoilDispatchRejection::ARBITER_OUTPUT_FAULT: return "ARBITER_OUTPUT_FAULT";
    case RecoilDispatchRejection::CONTEXT_CHANGED: return "CONTEXT_CHANGED";
    case RecoilDispatchRejection::DISABLED: return "DISABLED";
    case RecoilDispatchRejection::NOT_HELD: return "NOT_HELD";
    case RecoilDispatchRejection::INPUT_UNHEALTHY: return "INPUT_UNHEALTHY";
    case RecoilDispatchRejection::PERMISSION_DENIED: return "PERMISSION_DENIED";
    case RecoilDispatchRejection::NOT_FOCUSED: return "NOT_FOCUSED";
    case RecoilDispatchRejection::PROFILE_MISMATCH: return "PROFILE_MISMATCH";
    case RecoilDispatchRejection::EXPIRED: return "EXPIRED";
    case RecoilDispatchRejection::BUDGET_EXCEEDED: return "BUDGET_EXCEEDED";
    case RecoilDispatchRejection::CANCELED: return "CANCELED";
    case RecoilDispatchRejection::STOPPING: return "STOPPING";
    case RecoilDispatchRejection::CALIBRATION_BUDGET: return "CALIBRATION_BUDGET";
    }
    return "UNKNOWN";
}
inline const char* RecoilFiringSourceName(RecoilFiringSource source) noexcept {
    switch(source) {
    case RecoilFiringSource::INPUT_ESTIMATED:return "INPUT_ESTIMATED";
    case RecoilFiringSource::COMMAND_ESTIMATED:return "COMMAND_ESTIMATED";
    default:return "UNKNOWN";
    }
}
struct RecoilExecutionRecord {
    RecoilIntent intent;
    RecoilReceipt receipt;
    std::shared_ptr<const RecoilProfile> profile;
    bool backend_called = false;
    RecoilTime firing_started_at{};
    RecoilFiringSource firing_source = RecoilFiringSource::UNKNOWN;
    // 仅描述本次派发决定；NONE不代表后端成功，成功与未知仍由receipt表达。
    RecoilDispatchRejection dispatch_rejection = RecoilDispatchRejection::NONE;
    RecoilTime sampled_at{}, arbitration_at{}, context_checked_at{}, backend_called_at{}, backend_returned_at{};
    // 软件扳机命令关联；实体输入无命令id。区间仅属于设备调用，不是实弹时刻误差。
    std::uint64_t source_firing_id = 0;
    std::optional<std::int64_t> firing_uncertainty_ns;
};
struct RecoilExecutionLog {
    std::vector<RecoilExecutionRecord> records;
    std::uint64_t dropped_count = 0;
};

struct RecoilEventSlice;
class RecoilWorker {
public:
    RecoilWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter, std::shared_ptr<MotionLedger> ledger,
        std::function<RecoilInput()> context, std::function<TriggerFiringSignal()> firing);
    ~RecoilWorker();
    bool start(const RecoilConfig& config) noexcept;
    bool start_calibration(const RecoilConfig& config, std::shared_ptr<const RecoilCalibrationPermit> permit) noexcept;
    void cancel() noexcept;
    void stop() noexcept;
    RecoilSnapshot snapshot() const noexcept;
    // 冷快照；最多2048条，dropped_count非零表示此前会话证据已不完整。
    RecoilExecutionLog execution_log() const;
    RecoilEventSlice read_execution_events(std::uint64_t after_sequence, std::size_t max_events = 256) const;
    RecoilCalibrationBudgetSnapshot calibration_snapshot() const noexcept;
private:
    bool start_impl(const RecoilConfig& config) noexcept;
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
