#ifndef RECOIL_WORKER_H
#define RECOIL_WORKER_H
#include <functional>
#include <memory>
#include "recoil/recoil.h"
#include "recoil/recoil_config.h"
#include "recoil/motion_ledger.h"
#include "trigger/trigger_worker.h"

enum class RecoilFiringSource { UNKNOWN, INPUT_ESTIMATED, COMMAND_ESTIMATED };
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
};
struct RecoilExecutionLog {
    std::vector<RecoilExecutionRecord> records;
    std::uint64_t dropped_count = 0;
};

class RecoilWorker {
public:
    RecoilWorker(std::shared_ptr<IMouseController> mouse,
        std::shared_ptr<AutoStopOutputArbiter> arbiter, std::shared_ptr<MotionLedger> ledger,
        std::function<RecoilInput()> context, std::function<TriggerFiringSignal()> firing);
    ~RecoilWorker();
    bool start(const RecoilConfig& config) noexcept;
    void cancel() noexcept;
    void stop() noexcept;
    RecoilSnapshot snapshot() const noexcept;
    // 冷快照；最多2048条，dropped_count非零表示此前会话证据已不完整。
    RecoilExecutionLog execution_log() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
