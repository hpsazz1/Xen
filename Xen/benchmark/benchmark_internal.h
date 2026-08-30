#ifndef BENCHMARK_INTERNAL_H
#define BENCHMARK_INTERNAL_H

#include "debug/debug.h"
#include "runtime/runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

struct FormalSampleSummary {
    // raw schema 17 的 sample_count 只表示留样数；这里显式命名为
    // formal_sample_count，避免把全程成功总数误写成留样窗口大小。
    std::uint64_t formal_sample_count = 0;
    std::uint64_t successful_samples = 0;
    std::uint64_t failed_samples = 0;
    std::size_t retained_sample_count = 0;
    std::uint64_t omitted_sample_count = 0;
};

// 正式 Benchmark 唯一的 staging 报告提交 seam：成对生成报告后，按
// formal 总数、固定容量尾窗与 omitted 守恒校验发布前汇总。
bool finalize_report(
    DebugReport& report,
    const RuntimeSnapshot& final_snapshot,
    const DebugCoverageSummary& coverage,
    const FormalSampleSummary& formal_summary,
    std::uint64_t phase_formal_samples,
    bool performance_probes_enabled,
    CaptureBackend capture_backend,
    std::string& error) noexcept;

// formal 成功样本由这里统一聚合并写入固定容量环。生产循环与测试读取
// 同一组按 sequence 排序的 span，避免 DebugReport 满容量后逐样本搬移。
class FormalSampleTracker final {
public:
    explicit FormalSampleTracker(std::size_t retention_capacity)
        : retention_capacity_(retention_capacity) {
        retained_samples_.reserve(retention_capacity_);
    }

    bool observe(const RuntimePipelineSample& sample,
                 std::string& error) noexcept {
        if (!debug_sample_succeeded(sample)) {
            error = "失败 Pipeline 样本不得进入 formal 报告";
            return false;
        }
        if (summary_.formal_sample_count ==
                std::numeric_limits<std::uint64_t>::max()) {
            error = "formal 成功样本计数溢出";
            return false;
        }
        if (retention_capacity_ == 0 || storage_released_) {
            error = "formal 报告留样库存不可用";
            return false;
        }

        try {
            if (retained_samples_.size() < retention_capacity_) {
                retained_samples_.push_back(sample);
            } else {
                retained_samples_[next_write_index_] = sample;
                next_write_index_ =
                    (next_write_index_ + 1) % retention_capacity_;
                ++summary_.omitted_sample_count;
            }
            ++summary_.formal_sample_count;
            ++summary_.successful_samples;
            summary_.retained_sample_count = retained_samples_.size();
        } catch (...) {
            error = "写入 formal 固定容量留样环失败";
            return false;
        }
        error.clear();
        return true;
    }

    bool gates_satisfied(std::uint64_t minimum_samples,
                         bool time_gate_satisfied) const noexcept {
        return summary_.formal_sample_count >= minimum_samples &&
            time_gate_satisfied;
    }

    const FormalSampleSummary& summary() const noexcept {
        return summary_;
    }

    std::size_t retention_capacity() const noexcept {
        return retention_capacity_;
    }

    std::size_t retention_storage_bytes() const noexcept {
        return retained_samples_.capacity() *
            sizeof(RuntimePipelineSample);
    }

    std::array<std::span<const RuntimePipelineSample>, 2>
    retained_sample_spans() const noexcept {
        std::array<std::span<const RuntimePipelineSample>, 2> spans;
        if (retained_samples_.empty()) return spans;
        if (retained_samples_.size() < retention_capacity_ ||
            next_write_index_ == 0) {
            spans[0] = std::span(retained_samples_);
            return spans;
        }
        spans[0] = std::span(retained_samples_).subspan(next_write_index_);
        spans[1] = std::span(retained_samples_).first(next_write_index_);
        return spans;
    }

    void release_retained_storage() noexcept {
        std::vector<RuntimePipelineSample>().swap(retained_samples_);
        next_write_index_ = 0;
        storage_released_ = true;
    }

private:
    FormalSampleSummary summary_;
    std::vector<RuntimePipelineSample> retained_samples_;
    std::size_t retention_capacity_ = 0;
    std::size_t next_write_index_ = 0;
    bool storage_released_ = false;
};

enum class ReportFileFormat {
    CSV,
    JSON,
};

// DebugReport 在 staging 中只接收最终尾窗，因此其内部 dropped 为 0。
// 正式报告发布前只允许精确改写这一条元数据，逐样本正文保持不变。
inline bool rewrite_report_samples_dropped_line(
        std::string_view line,
        ReportFileFormat format,
        std::uint64_t omitted_sample_count,
        std::string& rewritten) {
    const std::string_view marker = format == ReportFileFormat::CSV
        ? "# report_samples_dropped,0"
        : "  \"report_samples_dropped\": 0,";
    if (line != marker) {
        rewritten.clear();
        return false;
    }
    const std::string value = std::to_string(omitted_sample_count);
    rewritten = format == ReportFileFormat::CSV
        ? "# report_samples_dropped," + value
        : "  \"report_samples_dropped\": " + value + ',';
    return true;
}

} // namespace benchmark::detail

#endif // BENCHMARK_INTERNAL_H
