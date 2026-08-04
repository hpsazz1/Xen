#ifndef BENCHMARK_INTERNAL_H
#define BENCHMARK_INTERNAL_H

#include "debug/debug.h"
#include "runtime/runtime.h"

#include <cstdint>
#include <string>

namespace benchmark::detail {

enum class CoveragePhase {
    STARTUP,
    WARMUP,
    FORMAL,
};

// 以实际交付的 Pipeline 样本作为边界，避免主线程 Snapshot 与异步队列
// 之间出现竞态。首个成功样本单独归入 startup，随后才开始 warmup。
class CoverageTracker final {
public:
    bool observe(CoveragePhase phase,
                 const RuntimePipelineSample& sample,
                 std::string& error) noexcept {
        try {
            if (sample.sequence == 0 || sample.sequence <= last_sequence_) {
                error = "覆盖分段样本序号不是严格递增";
                return false;
            }
            if (sample.runtime_overwritten_frames < last_overwritten_frames_) {
                error = "Runtime 覆盖累计值发生回退";
                return false;
            }
            if (sample.source_dropped_frames < last_source_dropped_frames_ ||
                sample.transport_dropped_frames <
                    last_transport_dropped_frames_ ||
                sample.transport_invalid_packets <
                    last_transport_invalid_packets_) {
                error = "Capture/传输累计计数发生回退";
                return false;
            }
            if (!valid_transition(phase)) {
                error = "覆盖分段状态迁移非法";
                return false;
            }

            DebugCoveragePhaseSummary& target = phase_summary(phase);
            if (target.sample_count == 0) {
                target.first_sequence = sample.sequence;
            }
            target.last_sequence = sample.sequence;
            ++target.sample_count;
            target.sequence_gaps += sample.sequence - last_sequence_ - 1;
            target.runtime_overwritten_frames +=
                sample.runtime_overwritten_frames - last_overwritten_frames_;
            target.counter_matches_sequence_gaps =
                target.runtime_overwritten_frames == target.sequence_gaps;

            last_sequence_ = sample.sequence;
            last_overwritten_frames_ = sample.runtime_overwritten_frames;
            last_source_dropped_frames_ = sample.source_dropped_frames;
            last_transport_dropped_frames_ =
                sample.transport_dropped_frames;
            last_transport_invalid_packets_ =
                sample.transport_invalid_packets;
            phase_ = phase;
            has_sample_ = true;
            if (phase == CoveragePhase::STARTUP) {
                summary_.warmup_start_overwritten_frames =
                    last_overwritten_frames_;
                summary_.warmup_end_overwritten_frames =
                    last_overwritten_frames_;
                summary_.formal_end_overwritten_frames =
                    last_overwritten_frames_;
            } else if (phase == CoveragePhase::WARMUP) {
                summary_.warmup_end_overwritten_frames =
                    last_overwritten_frames_;
                summary_.formal_end_overwritten_frames =
                    last_overwritten_frames_;
            } else {
                summary_.formal_end_overwritten_frames =
                    last_overwritten_frames_;
            }
            error.clear();
            return true;
        } catch (...) {
            error = "累计覆盖分段时发生未知异常";
            return false;
        }
    }

    bool finish(std::uint64_t expected_warmup_samples,
                std::uint64_t expected_formal_samples,
                const RuntimeSnapshot& final_snapshot,
                std::string& error) noexcept {
        if (summary_.startup.sample_count != 1 ||
            summary_.warmup.sample_count != expected_warmup_samples ||
            expected_formal_samples == 0 ||
            summary_.formal.sample_count != expected_formal_samples) {
            error = "覆盖分段样本数与 startup/warmup/formal 边界不一致";
            return false;
        }
        if (final_snapshot.overwritten_frames < last_overwritten_frames_ ||
            final_snapshot.source_dropped_frames <
                last_source_dropped_frames_ ||
            final_snapshot.transport_dropped_frames <
                last_transport_dropped_frames_ ||
            final_snapshot.transport_invalid_packets <
                last_transport_invalid_packets_) {
            error = "Runtime 停止时累计计数发生回退";
            return false;
        }
        const std::uint64_t trailing =
            final_snapshot.overwritten_frames - last_overwritten_frames_;
        summary_.formal.trailing_runtime_overwritten_frames = trailing;
        summary_.formal.runtime_overwritten_frames += trailing;
        summary_.formal.counter_matches_sequence_gaps =
            summary_.formal.runtime_overwritten_frames ==
                summary_.formal.sequence_gaps + trailing;
        summary_.formal_end_overwritten_frames =
            final_snapshot.overwritten_frames;
        summary_.available = true;
        error.clear();
        return true;
    }

    const DebugCoverageSummary& summary() const noexcept {
        return summary_;
    }

private:
    bool valid_transition(CoveragePhase next) const noexcept {
        if (!has_sample_) return next == CoveragePhase::STARTUP;
        if (phase_ == CoveragePhase::STARTUP) {
            return next == CoveragePhase::WARMUP ||
                   next == CoveragePhase::FORMAL;
        }
        if (phase_ == CoveragePhase::WARMUP) {
            return next == CoveragePhase::WARMUP ||
                   next == CoveragePhase::FORMAL;
        }
        return next == CoveragePhase::FORMAL;
    }

    DebugCoveragePhaseSummary& phase_summary(CoveragePhase phase) noexcept {
        if (phase == CoveragePhase::STARTUP) return summary_.startup;
        if (phase == CoveragePhase::WARMUP) return summary_.warmup;
        return summary_.formal;
    }

    DebugCoverageSummary summary_;
    CoveragePhase phase_ = CoveragePhase::STARTUP;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_overwritten_frames_ = 0;
    std::uint64_t last_source_dropped_frames_ = 0;
    std::uint64_t last_transport_dropped_frames_ = 0;
    std::uint64_t last_transport_invalid_packets_ = 0;
    bool has_sample_ = false;
};

struct SamplePhaseObservation {
    CoveragePhase phase = CoveragePhase::STARTUP;
    bool measurement_begins = false;
};

// 将 startup/warmup/formal 路由从 2 ms drain 批次中抽离。同一批次跨阶段
// 或 stop 后 final drain 继续到达时，都只由已成功样本数决定下一阶段。
class SamplePhaseTracker final {
public:
    explicit SamplePhaseTracker(std::uint64_t warmup_samples) noexcept
        : expected_warmup_samples_(warmup_samples) {}

    bool observe(const RuntimePipelineSample& sample,
                 SamplePhaseObservation& observation,
                 std::string& error) noexcept {
        if (!debug_sample_succeeded(sample)) {
            error = "失败 Pipeline 样本不得进入阶段计数";
            return false;
        }

        CoveragePhase phase = CoveragePhase::FORMAL;
        if (startup_successful_ == 0) {
            phase = CoveragePhase::STARTUP;
        } else if (warmup_successful_ < expected_warmup_samples_) {
            phase = CoveragePhase::WARMUP;
        }
        if (!coverage_.observe(phase, sample, error)) return false;

        observation = {};
        observation.phase = phase;
        if (phase == CoveragePhase::STARTUP) {
            ++startup_successful_;
            observation.measurement_begins =
                expected_warmup_samples_ == 0;
        } else if (phase == CoveragePhase::WARMUP) {
            ++warmup_successful_;
            observation.measurement_begins =
                warmup_successful_ == expected_warmup_samples_;
        } else {
            ++formal_successful_;
        }
        error.clear();
        return true;
    }

    bool finish(const RuntimeSnapshot& final_snapshot,
                std::string& error) noexcept {
        return coverage_.finish(
            expected_warmup_samples_, formal_successful_,
            final_snapshot, error);
    }

    std::uint64_t startup_successful() const noexcept {
        return startup_successful_;
    }

    std::uint64_t warmup_successful() const noexcept {
        return warmup_successful_;
    }

    std::uint64_t formal_successful() const noexcept {
        return formal_successful_;
    }

    const DebugCoverageSummary& coverage() const noexcept {
        return coverage_.summary();
    }

private:
    CoverageTracker coverage_;
    std::uint64_t expected_warmup_samples_ = 0;
    std::uint64_t startup_successful_ = 0;
    std::uint64_t warmup_successful_ = 0;
    std::uint64_t formal_successful_ = 0;
};

} // namespace benchmark::detail

#endif // BENCHMARK_INTERNAL_H
