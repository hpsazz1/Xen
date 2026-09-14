#include "input_training/input_training.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace input_training {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::int64_t kPerfectNs = 2000000, kExcellentNs = 10000000, kPairWindowNs = 120000000;
constexpr const char* kColumns = "epoch,sequence,received_at_ns,dx,dy,held_mask,left_down,state_valid,motion_valid,gap,physical_motion_verified,source_loss_verifiable,timing_uncertainty_ns,raw_report_valid,datagram_size,raw_report";
constexpr std::uint64_t kMetadataReserveBytes = 32768;

Grade classify(std::int64_t delta) noexcept {
    if (delta < -kPairWindowNs || delta > kPairWindowNs) return Grade::UNCLASSIFIED;
    if (delta >= -kPerfectNs && delta <= kPerfectNs) return Grade::PERFECT;
    if (delta >= -kExcellentNs && delta <= kExcellentNs) return Grade::EXCELLENT;
    return delta < 0 ? Grade::EARLY : Grade::LATE;
}
struct Edge { std::int64_t time = 0, uncertainty = -1; std::uint64_t sequence = 0; };
struct Axis {
    bool known[2]{};
    std::optional<Edge> release[2], overlap[2];
};
class Evaluator {
public:
    explicit Evaluator(Limits limits) : limits_(limits) {}
    Snapshot result;
    bool limit_hit = false;

    void accept(const Event& event) {
        ++result.received_events;
        const bool stale = have_watermark_ && (event.epoch < previous_.epoch ||
            (event.epoch == previous_.epoch && (event.sequence <= previous_.sequence || event.received_at_ns < previous_.received_at_ns)));
        const bool disorder = have_watermark_ && (event.epoch != previous_.epoch || stale ||
            event.sequence != previous_.sequence + 1);
        const bool invalid = !event.state_valid || event.epoch == 0 || event.sequence == 0 ||
            event.received_at_ns < 0 || event.timing_uncertainty_ns < -1 || (event.held_mask & 0xf0) != 0;
        if (event.gap || disorder || invalid) {
            if (invalid || disorder) ++result.invalid_events;
            break_stream();
        }
        if (invalid || stale) { have_previous_ = false; return; }
        const bool baseline = !have_previous_;
        if (!baseline) {
            assess_axis(axes_[0], event, kA, kD, 'A', 'D');
            assess_axis(axes_[1], event, kW, kS, 'W', 'S');
        }
        const bool down = event.left_down && (baseline || !previous_.left_down);
        const bool up = !event.left_down && !baseline && previous_.left_down;
        if (down) {
            active_ = std::make_unique<Hold>();
            active_->id = ++result.total_holds;
            missing_start_ = baseline;
            active_->source_loss_verifiable = true;
            active_->physical_motion_verified = true;
            active_->motion_available = true;
            last_dx_ = last_dy_ = 0;
        }
        if (active_) {
            if (active_->points.size() >= limits_.max_hold_events) {
                finish_hold(HoldEnd::LIMIT); limit_hit = true;
            } else {
                Point point;
                point.event = event; point.raw_index = result.received_events - 1;
                point.boundary_ambiguous = (down || up) && (event.raw_report_valid ||
                    (event.motion_valid && (event.dx != 0 || event.dy != 0)));
                if (!active_->points.empty()) {
                    point.x = active_->points.back().x; point.y = active_->points.back().y;
                }
                if (event.motion_valid) {
                    point.x += event.dx; point.y += event.dy;
                    if (event.dx != 0 || event.dy != 0) {
                        if (last_dx_ != 0 || last_dy_ != 0) {
                            const auto cross = static_cast<double>(last_dx_) * event.dy - static_cast<double>(last_dy_) * event.dx;
                            const auto dot = static_cast<double>(last_dx_) * event.dx + static_cast<double>(last_dy_) * event.dy;
                            point.turn_radians = std::atan2(cross, dot);
                        }
                        last_dx_ = event.dx; last_dy_ = event.dy;
                    }
                }
                active_->motion_available &= event.motion_valid;
                active_->physical_motion_verified &= event.physical_motion_verified;
                active_->source_loss_verifiable &= event.source_loss_verifiable;
                active_->boundary_ambiguous |= point.boundary_ambiguous;
                active_->points.push_back(point);
                if (up) finish_hold(missing_start_ ? HoldEnd::MISSING_START : HoldEnd::RELEASED);
            }
        }
        previous_ = event; have_previous_ = true; have_watermark_ = true;
    }
    void break_stream() {
        clear_axes();
        if (active_) finish_hold(HoldEnd::GAP);
        have_previous_ = false;
    }
    void finish(HoldEnd reason) {
        clear_axes();
        if (active_) finish_hold(reason);
    }
    std::shared_ptr<const Snapshot> snapshot(Status status, const std::string& directory,
        const std::string& error, std::uint64_t dropped, std::uint64_t bytes, std::uint64_t chunks) const {
        auto value = std::make_shared<Snapshot>(result);
        value->status = status; value->directory = directory; value->error = error;
        value->dropped_events = dropped; value->archive_bytes = bytes; value->chunks = chunks;
        if (active_) value->active_hold = std::make_shared<Hold>(*active_);
        return value;
    }
private:
    Limits limits_;
    Axis axes_[2];
    Event previous_;
    bool have_previous_ = false, have_watermark_ = false, missing_start_ = false;
    std::unique_ptr<Hold> active_;
    std::int32_t last_dx_ = 0, last_dy_ = 0;
    std::size_t retained_points_ = 0;

    void clear_axes() {
        for (auto& axis : axes_) {
            for (int key = 0; key < 2; ++key) {
                result.unpaired_edges += axis.release[key].has_value() + axis.overlap[key].has_value();
            }
            axis = {};
        }
    }
    void finish_hold(HoldEnd reason) {
        active_->end = reason;
        active_->complete_received_stream = reason == HoldEnd::RELEASED && !missing_start_;
        retained_points_ += active_->points.size();
        result.holds.emplace_back(std::move(active_));
        while (result.holds.size() > limits_.recent_holds ||
            (retained_points_ > limits_.retained_hold_events && result.holds.size() > 1)) {
            retained_points_ -= result.holds.front()->points.size();
            result.holds.erase(result.holds.begin());
        }
    }
    void pair(const Event& event, char from, char to, const Edge& release, const Edge& press, bool ambiguous) {
        Timing value;
        value.epoch = event.epoch; value.from = from; value.to = to;
        value.release_sequence = release.sequence; value.press_sequence = press.sequence;
        value.delta_ns = press.time - release.time;
        value.completed_at_ns = event.received_at_ns;
        value.atomic_ambiguous = ambiguous || (release.time == press.time);
        value.grade = value.atomic_ambiguous ? Grade::UNCLASSIFIED : classify(value.delta_ns);
        value.timing_uncertainty_known = release.uncertainty >= 0 && press.uncertainty >= 0;
        if (value.timing_uncertainty_known) {
            // 饱和到配对窗外即可判定跨界，避免恶意离线值产生有符号溢出。
            const auto uncertainty = std::min(release.uncertainty, kPairWindowNs * 2) +
                std::min(press.uncertainty, kPairWindowNs * 2);
            if (value.delta_ns >= -kPairWindowNs * 4 && value.delta_ns <= kPairWindowNs * 4) {
                value.uncertainty_crosses_boundary = classify(value.delta_ns - uncertainty) != value.grade ||
                    classify(value.delta_ns + uncertainty) != value.grade;
                if (value.uncertainty_crosses_boundary) value.grade = Grade::UNCLASSIFIED;
            }
        }
        ++result.total_timings;
        result.timings.push_back(value);
        if (result.timings.size() > limits_.recent_timings) result.timings.erase(result.timings.begin());
    }
    void assess_axis(Axis& axis, const Event& event, std::uint8_t first, std::uint8_t second, char first_name, char second_name) {
        const std::uint8_t bits[2] = { first, second };
        const char names[2] = { first_name, second_name };
        const auto changed = static_cast<std::uint8_t>((event.held_mask ^ previous_.held_mask) & (first | second));
        const Edge edge{ event.received_at_ns, event.timing_uncertainty_ns, event.sequence };
        if (changed == (first | second)) {
            // 同报告的松/按无法解析亚包先后，保留 0 原值且不授予“完美”。
            for (int old = 0; old < 2; ++old) {
                const int next = 1 - old;
                if ((previous_.held_mask & bits[old]) && !(event.held_mask & bits[old]) &&
                    !(previous_.held_mask & bits[next]) && (event.held_mask & bits[next]) && axis.known[old])
                    pair(event, names[old], names[next], edge, edge, true);
            }
            for (int key = 0; key < 2; ++key) {
                result.unpaired_edges += axis.release[key].has_value() + axis.overlap[key].has_value();
                axis.release[key].reset(); axis.overlap[key].reset();
                axis.known[key] = (event.held_mask & bits[key]) != 0;
            }
            return;
        }
        for (int key = 0; key < 2; ++key) {
            if (!(changed & bits[key])) continue;
            const int other = 1 - key;
            if (event.held_mask & bits[key]) {
                if (axis.release[key]) { ++result.unpaired_edges; axis.release[key].reset(); }
                if (previous_.held_mask & bits[other]) {
                    if (axis.known[other]) axis.overlap[other] = edge;
                } else if (axis.release[other]) {
                    pair(event, names[other], names[key], *axis.release[other], edge, false);
                    axis.release[other].reset();
                }
                axis.known[key] = true;
            } else {
                if (axis.overlap[other]) { ++result.unpaired_edges; axis.overlap[other].reset(); }
                if (axis.known[key]) {
                    if (axis.overlap[key]) {
                        pair(event, names[key], names[other], edge, *axis.overlap[key], false);
                        axis.overlap[key].reset();
                    } else axis.release[key] = edge;
                } else ++result.unpaired_edges;
                axis.known[key] = false;
            }
        }
    }
};

std::string encode(const Event& event) {
    std::ostringstream out;
    out << event.epoch << ',' << event.sequence << ',' << event.received_at_ns << ',' << event.dx << ',' << event.dy << ','
        << static_cast<unsigned>(event.held_mask) << ',' << event.left_down << ',' << event.state_valid << ',' << event.motion_valid << ','
        << event.gap << ',' << event.physical_motion_verified << ',' << event.source_loss_verifiable << ','
        << event.timing_uncertainty_ns << ',' << event.raw_report_valid << ',' << event.datagram_size << ',';
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : event.raw_report) out << hex[byte >> 4] << hex[byte & 15];
    out << '\n';
    return out.str();
}
Event decode(const std::string& line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = line.find(',', begin);
        fields.emplace_back(line.data() + begin, (end == std::string::npos ? line.size() : end) - begin);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (fields.size() != 16) throw std::runtime_error("原始事件列数错误");
    auto number = [&](std::size_t i, auto& value) {
        const auto [end, error] = std::from_chars(fields[i].data(), fields[i].data() + fields[i].size(), value);
        if (error != std::errc{} || end != fields[i].data() + fields[i].size()) throw std::runtime_error("原始事件数值错误");
    };
    auto flag = [&](std::size_t i, bool& value) { unsigned raw = 0; number(i, raw);
        if (raw > 1) throw std::runtime_error("原始事件布尔值错误"); value = raw != 0; };
    Event event;
    number(0, event.epoch); number(1, event.sequence); number(2, event.received_at_ns);
    number(3, event.dx); number(4, event.dy); unsigned mask = 0; number(5, mask);
    if (mask > 15) throw std::runtime_error("原始键态超出 WASD"); event.held_mask = static_cast<std::uint8_t>(mask);
    flag(6, event.left_down); flag(7, event.state_valid); flag(8, event.motion_valid); flag(9, event.gap);
    flag(10, event.physical_motion_verified); flag(11, event.source_loss_verifiable);
    number(12, event.timing_uncertainty_ns); flag(13, event.raw_report_valid); number(14, event.datagram_size);
    if (fields[15].size() != 40) throw std::runtime_error("原始报告长度错误");
    for (std::size_t i = 0; i < 20; ++i) {
        unsigned raw = 0;
        const auto text = fields[15].substr(i * 2, 2);
        const auto [end, error] = std::from_chars(text.data(), text.data() + 2, raw, 16);
        if (error != std::errc{} || end != text.data() + 2) throw std::runtime_error("原始报告编码错误");
        event.raw_report[i] = static_cast<std::uint8_t>(raw);
    }
    return event;
}
void publish_file(const std::filesystem::path& path, const std::string& text) {
    // 不覆盖旧文件；临时文件明确表示未完成，发布时仍核对目标。
    auto temporary = path; temporary += ".pending";
    if (std::filesystem::exists(path) || std::filesystem::exists(temporary)) throw std::runtime_error("归档文件已存在");
    std::ofstream stream(temporary, std::ios::binary | std::ios::out);
    stream.write(text.data(), static_cast<std::streamsize>(text.size())); stream.flush();
    if (!stream.good()) throw std::runtime_error("归档写入失败");
    stream.close(); if (stream.fail()) throw std::runtime_error("归档关闭失败");
    if (std::filesystem::exists(path)) throw std::runtime_error("归档发布目标已存在");
    std::filesystem::rename(temporary, path);
}
bool valid_limits(const Limits& limits) noexcept {
    return limits.queue_events > 0 && limits.queue_events <= 65536 && limits.chunk_events > 0 && limits.chunk_events <= 4096 &&
        limits.max_hold_events >= 2 && limits.max_hold_events <= 262144 &&
        limits.retained_hold_events >= limits.max_hold_events && limits.retained_hold_events <= 262144 &&
        limits.recent_timings > 0 && limits.recent_timings <= 300 && limits.recent_holds > 0 && limits.recent_holds <= 100 &&
        limits.max_run_events > 0 && limits.max_run_events <= 1048576 && limits.max_run_bytes >= 1024 &&
        limits.max_run_bytes <= 256ull * 1024 * 1024 && limits.max_chunks > 0 && limits.max_chunks <= 4096 &&
        limits.stop_timeout_ms > 0 && limits.stop_timeout_ms <= 5000;
}
void require_archive_path(const std::filesystem::path& path, bool directory) {
    // 保留逐级检查，不能先 canonical 抹掉符号链接/目录联接证据。
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        if (component == "..") throw std::runtime_error("归档路径不能包含父目录跳转");
        if (component == ".") continue;
        current /= component;
        const auto status = std::filesystem::symlink_status(current);
        if (std::filesystem::is_symlink(status)) throw std::runtime_error("归档路径不能经过符号链接");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("归档路径不存在或含重解析点");
#endif
    }
    if (directory ? !std::filesystem::is_directory(path) : !std::filesystem::is_regular_file(path))
        throw std::runtime_error("归档路径类型不合法");
}
bool archive_line(std::istream& stream, std::string& line, std::size_t limit) {
    line.clear();
    char ch;
    while (stream.get(ch)) {
        if (ch == '\n') return true;
        if (line.size() >= limit) throw std::runtime_error("归档行超出长度预算");
        line.push_back(ch);
    }
    if (stream.bad()) throw std::runtime_error("归档读取失败");
    if (!line.empty()) throw std::runtime_error("归档尾行未完整关闭");
    return false;
}
}

bool visit_archive(const std::filesystem::path& directory, const std::function<void(const Event&)>& visitor,
    ArchiveSummary& summary, std::string& error, Limits limits) noexcept {
    summary = {};
    error.clear();
    try {
        if (directory.empty() || !visitor || !valid_limits(limits)) throw std::runtime_error("归档遍历参数无效");
        // Windows absolute 可能先规范化掉 ..；必须在转换前检查调用方原始路径。
        for (const auto& component : directory)
            if (component == "..") throw std::runtime_error("归档路径不能包含父目录跳转");
        const auto root = std::filesystem::absolute(directory);
        require_archive_path(root, true);
        const auto manifest_path = root / "manifest.txt";
        require_archive_path(manifest_path, false);
        if (std::filesystem::file_size(manifest_path) > 1024) throw std::runtime_error("归档清单超出预算");
        std::ifstream manifest(manifest_path, std::ios::binary);
        std::string line;
        if (!archive_line(manifest, line, 128) || line != "XEN_INPUT_TRAINING_V1")
            throw std::runtime_error("无有效归档 manifest");
        if (!archive_line(manifest, line, 512)) throw std::runtime_error("归档清单缺少水位");
        std::istringstream fields(line);
        std::array<std::uint64_t, 7> values{};
        std::string token;
        for (auto& value : values) {
            if (!(fields >> token)) throw std::runtime_error("归档清单字段不足");
            const auto [end, code] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (code != std::errc{} || end != token.data() + token.size()) throw std::runtime_error("归档清单数值无效");
        }
        if (fields >> token || archive_line(manifest, line, 512)) throw std::runtime_error("归档清单存在多余字段");
        if (values[0] > limits.max_chunks || values[1] > limits.max_run_events || values[3] > limits.max_run_bytes ||
            values[5] > 1 || values[6] < 2 || values[6] > limits.max_hold_events ||
            (values[4] != static_cast<std::uint64_t>(Status::STOPPED) && values[4] != static_cast<std::uint64_t>(Status::LIMIT)))
            throw std::runtime_error("归档清单超限或终止水位无效");
        ArchiveSummary result;
        result.status = static_cast<Status>(values[4]); result.dropped = values[2];
        result.trailing_gap = values[5] != 0; result.max_hold_events = static_cast<std::size_t>(values[6]);
        for (std::uint64_t index = 0; index < values[0]; ++index) {
            const auto path = root / ("events-" + std::to_string(index) + ".csv");
            require_archive_path(path, false);
            const auto file_bytes = std::filesystem::file_size(path);
            if (file_bytes > limits.max_run_bytes - result.bytes || file_bytes > 4096ull * 512 + 1024)
                throw std::runtime_error("原始 chunk 超出字节预算");
            std::ifstream stream(path, std::ios::binary);
            if (!archive_line(stream, line, 512) || line != kColumns) throw std::runtime_error("原始 chunk 表头不匹配");
            std::size_t count = 0;
            while (archive_line(stream, line, 512)) {
                if (++count > 4096 || result.events >= values[1]) throw std::runtime_error("原始事件超出清单或块预算");
                visitor(decode(line));
                ++result.events;
            }
            if (!count || std::filesystem::file_size(path) != file_bytes) throw std::runtime_error("原始 chunk 为空或读取期间变化");
            result.bytes += file_bytes; ++result.chunks;
        }
        if (result.events != values[1] || result.bytes != values[3]) throw std::runtime_error("原始 chunk 与清单数量不一致");
        summary = result;
        return true;
    } catch (const std::exception& exception) {
        try { error = exception.what(); } catch (...) {}
    } catch (...) {
        try { error = "归档遍历或访客失败"; } catch (...) {}
    }
    return false;
}

class Session::Impl {
public:
    struct State {
        explicit State(Limits value) : limits(value), queue(value.queue_events), evaluator(value) {}
        Limits limits;
        std::filesystem::path directory;
        Reader reader;
        std::vector<Event> queue;
        std::size_t head = 0, size = 0;
        std::mutex mutex, completion_mutex;
        std::condition_variable wake, completion;
        std::atomic_bool accepting{ true }, stopping{ false }, finished{ false }, timed_out{ false }, pending_gap{ false };
        std::atomic<std::uint64_t> dropped{ 0 };
        std::atomic<std::shared_ptr<const Snapshot>> published;
        Evaluator evaluator;
        std::uint64_t bytes = 0, chunks = 0;
        bool trailing_gap = false;
        std::string chunk, error;
        std::size_t chunk_count = 0;
        Status status = Status::RECORDING;
        bool replay = false;
        Clock::time_point last_snapshot{};
        std::uint64_t last_published_events = 0;

        void publish(bool force = false) {
            const auto now = Clock::now();
            if (force || (evaluator.result.received_events != last_published_events && now - last_snapshot >= std::chrono::milliseconds(100))) {
                auto value = std::make_shared<Snapshot>(*evaluator.snapshot(timed_out ? Status::STOP_TIMEOUT : status,
                    directory.string(), error, dropped.load(), bytes, chunks));
                value->replay_source = replay;
                published.store(value);
                last_snapshot = now;
                last_published_events = evaluator.result.received_events;
            }
        }
        void flush_chunk() {
            if (!chunk_count) return;
            publish_file(directory / ("events-" + std::to_string(chunks) + ".csv"), std::string(kColumns) + '\n' + chunk);
            ++chunks; chunk.clear(); chunk_count = 0;
        }
        bool consume(Event event) {
            if (evaluator.result.received_events >= limits.max_run_events) { status = Status::LIMIT; return false; }
            if (!replay) {
                if (!chunk_count && chunks >= limits.max_chunks) { status = Status::LIMIT; return false; }
                auto row = encode(event);
                const auto extra = row.size() + (chunk_count ? 0 : std::char_traits<char>::length(kColumns) + 1);
                // 包含版本/清单和最多300行摘要的保留量；小于保留量的预算合法但不能采样。
                if (bytes + extra + kMetadataReserveBytes > limits.max_run_bytes) { status = Status::LIMIT; return false; }
                bytes += extra; chunk += row; ++chunk_count;
            }
            evaluator.accept(event);
            if (!replay && chunk_count >= limits.chunk_events) flush_chunk();
            if (evaluator.limit_hit) { status = Status::LIMIT; return false; }
            return true;
        }
        bool consume_batch(ReadBatch batch) {
            if (batch.events.size() > limits.queue_events) throw std::runtime_error("输入 Reader 返回批次超出有界契约");
            dropped += batch.dropped_events;
            if (batch.gap) {
                if (!batch.events.empty()) batch.events.front().gap = true;
                else {
                    // 没有新报告时也必须立刻结束完整性；后续第一条仍带缺口。
                    evaluator.break_stream(); pending_gap = true;
                }
            }
            for (auto& event : batch.events) {
                event.gap |= pending_gap.exchange(false);
                if (!consume(event)) return false;
            }
            if (batch.trailing_gap) { evaluator.break_stream(); pending_gap = true; }
            return true;
        }
        void run_live() {
            publish_file(directory / "recording.txt", "XEN_INPUT_TRAINING_V1\n接收域输入记录；源传输丢失能力逐事件声明。\nraw_report 为 KMBOX 协议原始报告，位移语义未实机验证；仅 motion_valid 明确声明的事件可积分相对 counts。\n");
            bool final_reader_done = false;
            for (;;) {
                const bool stop_requested = stopping.load();
                if (reader && (!stop_requested || !final_reader_done)) {
                    if (!consume_batch(reader())) break;
                    if (stop_requested) final_reader_done = true;
                }
                std::vector<Event> batch;
                {
                    std::unique_lock lock(mutex);
                    const auto count = std::min(size, limits.chunk_events);
                    batch.reserve(count);
                    for (std::size_t i = 0; i < count; ++i) { batch.push_back(queue[head]); head = (head + 1) % queue.size(); }
                    size -= count;
                }
                bool accepted = true;
                for (auto event : batch) if (!consume(event)) { accepted = false; break; }
                if (!accepted) break;
                if (stopping && batch.empty() && (!reader || final_reader_done)) break;
                publish();
                if (batch.empty()) {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return stopping || size != 0; });
                }
            }
            accepting = false;
            // 自动限额也先冻结来源，不能让收尾磁盘 I/O 延长采集生命周期。
            reader = {};
            trailing_gap = pending_gap.load();
            if (trailing_gap) evaluator.break_stream();
            evaluator.finish(status == Status::LIMIT ? HoldEnd::LIMIT : HoldEnd::MISSING_END);
            flush_chunk();
            if (status == Status::RECORDING) status = Status::STOPPED;
            if (timed_out) { status = Status::STOP_TIMEOUT; return; }
            std::ostringstream summary;
            summary << "from,to,delta_ns,grade,atomic_ambiguous,uncertainty_crosses_boundary\n";
            for (const auto& timing : evaluator.result.timings) summary << timing.from << ',' << timing.to << ',' << timing.delta_ns << ','
                << static_cast<int>(timing.grade) << ',' << timing.atomic_ambiguous << ',' << timing.uncertainty_crosses_boundary << '\n';
            publish_file(directory / "summary.csv", summary.str());
            if (timed_out) { status = Status::STOP_TIMEOUT; return; }
            std::ostringstream manifest;
            manifest << "XEN_INPUT_TRAINING_V1\n" << chunks << ' ' << evaluator.result.received_events << ' '
                << dropped.load() << ' ' << bytes << ' ' << static_cast<int>(status) << ' ' << trailing_gap << ' '
                << limits.max_hold_events << '\n';
            publish_file(directory / "manifest.txt", manifest.str());
        }
        void run_replay() {
            std::ifstream manifest(directory / "manifest.txt", std::ios::binary);
            std::string version;
            if (!std::getline(manifest, version) || version != "XEN_INPUT_TRAINING_V1") throw std::runtime_error("无有效归档 manifest，不能称完整回放");
            std::uint64_t expected_chunks = 0, expected_events = 0, archive_drops = 0, archive_bytes = 0;
            int ending = 0;
            unsigned archive_trailing_gap = 0;
            std::size_t archive_max_hold = 0;
            if (!(manifest >> expected_chunks >> expected_events >> archive_drops >> archive_bytes >> ending >> archive_trailing_gap >> archive_max_hold) ||
                expected_chunks > limits.max_chunks || expected_events > limits.max_run_events || archive_bytes > limits.max_run_bytes ||
                archive_trailing_gap > 1 || archive_max_hold < 2 || archive_max_hold > limits.max_hold_events ||
                (ending != static_cast<int>(Status::STOPPED) && ending != static_cast<int>(Status::LIMIT)))
                throw std::runtime_error("归档清单超限或无效");
            dropped = archive_drops;
            auto replay_limits = limits; replay_limits.max_hold_events = archive_max_hold;
            evaluator = Evaluator(replay_limits);
            for (std::uint64_t index = 0; index < expected_chunks; ++index) {
                if (stopping) { status = Status::STOPPED; evaluator.finish(HoldEnd::CANCELED); return; }
                const auto path = directory / ("events-" + std::to_string(index) + ".csv");
                const auto file_bytes = std::filesystem::file_size(path);
                if (file_bytes > limits.max_run_bytes - bytes || file_bytes > 4096ull * 512 + 1024) throw std::runtime_error("原始 chunk 超出字节预算");
                bytes += file_bytes;
                std::ifstream stream(path, std::ios::binary);
                std::string line;
                if (!std::getline(stream, line) || line != kColumns) throw std::runtime_error("原始 chunk 表头不匹配");
                while (std::getline(stream, line)) {
                    if (line.size() > 512) throw std::runtime_error("原始事件行超限");
                    if (evaluator.result.received_events >= expected_events) throw std::runtime_error("原始事件多于清单，不能静默截断");
                    if (!consume(decode(line)) && status != Status::LIMIT) throw std::runtime_error("原始 chunk 消费失败");
                }
                if (stream.bad()) throw std::runtime_error("原始 chunk 读取失败");
                ++chunks; publish();
            }
            if (evaluator.result.received_events != expected_events || bytes != archive_bytes) throw std::runtime_error("原始 chunk 与清单数量不一致");
            if (archive_trailing_gap) evaluator.break_stream();
            status = static_cast<Status>(ending);
            evaluator.finish(status == Status::LIMIT ? HoldEnd::LIMIT : HoldEnd::MISSING_END);
        }
        void run() noexcept {
            try {
                if (replay) run_replay(); else run_live();
            } catch (const std::exception& exception) {
                status = Status::FAILED; error = exception.what();
                try { evaluator.finish(HoldEnd::CANCELED); } catch (...) {}
            } catch (...) {
                status = Status::FAILED;
                try { error = "输入评估后台失败"; evaluator.finish(HoldEnd::CANCELED); } catch (...) {}
            }
            accepting = false;
            // 释放设备订阅闭包；停止/限额/异常都不能遗留后台采集来源。
            reader = {};
            try { publish(true); } catch (...) {}
            { std::lock_guard lock(completion_mutex); finished = true; }
            completion.notify_all();
        }
    };
    std::atomic<std::shared_ptr<State>> state;
    std::thread worker;
    std::shared_ptr<const Snapshot> idle = std::make_shared<Snapshot>();

    bool start(const std::filesystem::path& directory, Limits limits, Reader reader, bool replay) noexcept {
        try {
            const auto current = state.load();
            if ((current && !current->finished) || directory.empty() || !valid_limits(limits)) return false;
            if (worker.joinable()) worker.join();
            if (!replay) {
                if (directory.has_parent_path()) std::filesystem::create_directories(directory.parent_path());
                if (!std::filesystem::create_directory(directory)) return false;
            }
            auto next = std::make_shared<State>(limits);
            next->directory = directory; next->reader = std::move(reader); next->replay = replay;
            next->status = replay ? Status::REPLAYING : Status::RECORDING;
            next->accepting = !replay; next->publish(true);
            state.store(next);
            worker = std::thread([next] { next->run(); });
            return true;
        } catch (...) {
            const auto current = state.load();
            if (current) { current->accepting = false; current->finished = true; }
            return false;
        }
    }
    void stop() noexcept {
        try {
            const auto current = state.load();
            if (!current) return;
            current->accepting = false; current->stopping = true; current->wake.notify_all();
            std::unique_lock lock(current->completion_mutex);
            const bool completed = current->completion.wait_for(lock, std::chrono::milliseconds(current->limits.stop_timeout_ms), [&] { return current->finished.load(); });
            if (!completed) current->timed_out = true;
            lock.unlock();
            if (worker.joinable()) { if (completed) worker.join(); else worker.detach(); }
        } catch (...) {}
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() { stop(); }
bool Session::start(const std::filesystem::path& directory, Limits limits, Reader reader) noexcept { return impl_->start(directory, limits, std::move(reader), false); }
bool Session::load(const std::filesystem::path& directory, Limits limits) noexcept { return impl_->start(directory, limits, {}, true); }
bool Session::submit(const Event& event) noexcept {
    try {
        const auto state = impl_->state.load();
        if (!state || !state->accepting) return false;
        std::unique_lock lock(state->mutex, std::try_to_lock);
        if (!lock.owns_lock()) { ++state->dropped; state->pending_gap = true; return false; }
        if (!state->accepting) return false;
        if (state->size == state->queue.size()) { ++state->dropped; state->pending_gap = true; return false; }
        auto accepted = event; accepted.gap |= state->pending_gap.exchange(false);
        state->queue[(state->head + state->size) % state->queue.size()] = accepted;
        ++state->size; lock.unlock(); state->wake.notify_one(); return true;
    } catch (...) { return false; }
}
void Session::stop() noexcept { impl_->stop(); }
std::shared_ptr<const Snapshot> Session::snapshot() const noexcept {
    try {
        const auto state = impl_->state.load();
        if (!state) return impl_->idle;
        const auto result = state->published.load();
        if (state->timed_out && result && result->status != Status::STOP_TIMEOUT) {
            auto timeout = std::make_shared<Snapshot>(*result); timeout->status = Status::STOP_TIMEOUT;
            timeout->error = "后台收尾超时；保留未完成记录，不能视为成功归档";
            return timeout;
        }
        return result;
    } catch (...) { return impl_->idle; }
}
const char* grade_name(Grade value) noexcept {
    switch (value) { case Grade::PERFECT: return "完美"; case Grade::EXCELLENT: return "优秀";
    case Grade::EARLY: return "偏早"; case Grade::LATE: return "偏晚"; default: return "未分类"; }
}
const char* hold_end_name(HoldEnd value) noexcept {
    switch (value) { case HoldEnd::ACTIVE: return "按住中"; case HoldEnd::RELEASED: return "已接收松开";
    case HoldEnd::MISSING_START: return "缺少按下"; case HoldEnd::MISSING_END: return "缺少松开";
    case HoldEnd::GAP: return "输入缺口"; case HoldEnd::LIMIT: return "达到预算"; default: return "已取消"; }
}
const char* status_name(Status value) noexcept {
    switch (value) { case Status::IDLE: return "未记录"; case Status::RECORDING: return "记录中";
    case Status::REPLAYING: return "回放中"; case Status::STOPPED: return "已结束";
    case Status::LIMIT: return "达到预算"; case Status::FAILED: return "记录失败"; default: return "收尾超时"; }
}
}
