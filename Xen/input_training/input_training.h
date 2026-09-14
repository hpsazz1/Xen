#ifndef INPUT_TRAINING_H
#define INPUT_TRAINING_H

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace input_training {
// 位图只表示来源报告的原始键态，不抹去同轴双键重叠。
inline constexpr std::uint8_t kW = 1, kA = 2, kS = 4, kD = 8;
struct Event {
    std::uint64_t epoch = 0, sequence = 0;
    std::int64_t received_at_ns = 0;
    std::int32_t dx = 0, dy = 0;
    std::uint8_t held_mask = 0;
    bool left_down = false, state_valid = false, motion_valid = false;
    bool gap = false, physical_motion_verified = false, source_loss_verifiable = false;
    std::array<std::uint8_t, 20> raw_report{};
    bool raw_report_valid = false;
    std::uint32_t datagram_size = 0;
    // -1 表示未知传输/接收误差；非负值只表示调用方已核实的时间误差。
    std::int64_t timing_uncertainty_ns = -1;
};
enum class Grade { PERFECT, EXCELLENT, EARLY, LATE, UNCLASSIFIED };
struct Timing {
    std::uint64_t epoch = 0, release_sequence = 0, press_sequence = 0;
    char from = 'A', to = 'D';
    std::int64_t delta_ns = 0, completed_at_ns = 0;
    Grade grade = Grade::UNCLASSIFIED;
    bool atomic_ambiguous = false, uncertainty_crosses_boundary = false;
    bool timing_uncertainty_known = false;
};
enum class HoldEnd { ACTIVE, RELEASED, MISSING_START, MISSING_END, GAP, LIMIT, CANCELED };
struct Point {
    Event event;
    std::uint64_t raw_index = 0;
    std::int64_t x = 0, y = 0;
    double turn_radians = 0;
    bool boundary_ambiguous = false;
};
struct Hold {
    std::uint64_t id = 0;
    HoldEnd end = HoldEnd::ACTIVE;
    bool complete_received_stream = false, source_loss_verifiable = false;
    bool physical_motion_verified = false, motion_available = false, boundary_ambiguous = false;
    std::vector<Point> points;
};
enum class Status { IDLE, RECORDING, REPLAYING, STOPPED, LIMIT, FAILED, STOP_TIMEOUT };
struct Snapshot {
    Status status = Status::IDLE;
    bool replay_source = false;
    std::string directory, error;
    std::uint64_t received_events = 0, dropped_events = 0, invalid_events = 0;
    std::uint64_t unpaired_edges = 0, total_timings = 0, total_holds = 0;
    std::uint64_t archive_bytes = 0, chunks = 0;
    // 仅接收域评分；未知硬件/网络误差绝不由 2ms 标签推导物理精度。
    std::vector<Timing> timings;
    std::vector<std::shared_ptr<const Hold>> holds;
    std::shared_ptr<const Hold> active_hold;
};
struct Limits {
    std::size_t queue_events = 65536, chunk_events = 4096;
    std::size_t max_hold_events = 262144, retained_hold_events = 262144;
    std::size_t recent_timings = 300, recent_holds = 100;
    std::uint64_t max_run_events = 1048576, max_run_bytes = 256ull * 1024 * 1024, max_chunks = 4096;
    unsigned stop_timeout_ms = 1500;
};
struct ReadBatch {
    std::vector<Event> events;
    bool gap = false;
    std::uint64_t dropped_events = 0;
    bool trailing_gap = false;
};
struct ArchiveSummary {
    Status status = Status::FAILED;
    std::uint64_t chunks = 0, events = 0, dropped = 0, bytes = 0;
    bool trailing_gap = false;
    std::size_t max_hold_events = 0;
};
// 同步、有界遍历既有 CSV，不修改档案。visitor 只能进行离线分析，不调用设备。
// true 只证明格式和数量一致；LIMIT/dropped/trailing_gap 仍须由调用者拒绝校准。
// 失败可能已经交付前缀，调用者必须丢弃本次派生结果；summary 重置为 FAILED。
bool visit_archive(const std::filesystem::path& directory, const std::function<void(const Event&)>& visitor,
    ArchiveSummary& summary, std::string& error, Limits limits = {}) noexcept;
class Session {
public:
    // Reader 必须有界、无磁盘等待；每次返回尚未消费报告，空批表示暂时无新报告。
    // stop 禁止 submit 后，后台最后调用一次 Reader 取得水位，再排空队列。
    // 一次会话只提交一个已排序来源；start/load/stop 由同一生命周期 owner 串行调用。
    using Reader = std::function<ReadBatch()>;
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool start(const std::filesystem::path& directory, Limits limits = {}, Reader reader = {}) noexcept;
    bool submit(const Event& event) noexcept;
    void stop() noexcept;
    // 读取本模块发布的原始 chunk，复用同一评估器；后台执行，不写回档案。
    bool load(const std::filesystem::path& directory, Limits limits = {}) noexcept;
    std::shared_ptr<const Snapshot> snapshot() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
const char* grade_name(Grade grade) noexcept;
const char* hold_end_name(HoldEnd end) noexcept;
const char* status_name(Status status) noexcept;
}
#endif
