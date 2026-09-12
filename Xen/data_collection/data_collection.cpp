#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#include <data_collection/data_collection.h>
#include <log/log.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace data_collection {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> next_session{0};
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
class Sha256 {
public:
    Sha256() {
        check(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        const auto status = BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
        if (status < 0) { BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; check(status); }
    }
    ~Sha256() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(std::span<const unsigned char> bytes) {
        while (!bytes.empty()) {
            const auto count = std::min<std::size_t>(bytes.size(), 65536);
            check(BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(count), 0));
            bytes = bytes.subspan(count);
        }
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        check(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0));
        std::string result;
        for (auto value : digest) { result += "0123456789abcdef"[value >> 4]; result += "0123456789abcdef"[value & 15]; }
        return result;
    }
private:
    static void check(NTSTATUS status) { if (status < 0) throw std::runtime_error("SHA-256 计算失败"); }
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};
std::string hash_file(const std::filesystem::path& path) {
    Sha256 hash;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取模型文件");
    std::array<unsigned char, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (input.gcount()) hash.update(std::span(buffer.data(), static_cast<std::size_t>(input.gcount())));
    }
    if (!input.eof()) throw std::runtime_error("文件读取失败");
    return hash.finish();
}
std::string hash_bytes(std::span<const unsigned char> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}
void write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    output.close();
    if (!output) throw std::runtime_error("采集写盘失败");
}
std::int64_t nanoseconds(Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}
}
struct Collector::Impl {
    struct Slot {
        std::vector<unsigned char> pixels;
        std::vector<Detection> detections;
        FrameTiming timing;
        int width{}, height{}, source_width{}, source_height{}, encoded_width{}, encoded_height{};
        double roi_x{}, roi_y{}, scale_x{}, scale_y{};
        std::uint64_t generation{}, id{};
        DetectionStatus status{};
        const char* reason = "novel";
        bool occupied = false;
    };
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::thread worker;
    Config config;
    Snapshot state;
    std::atomic<std::uint64_t> contention_drops{0};
    std::atomic<bool> enabled{false};
    std::vector<Slot> slots;
    std::vector<std::size_t> queue;
    std::size_t queue_head{}, queue_size{};
    std::uint64_t accepted{}, bound_generation{};
    bool generation_bound = false;
    bool stopping = false;
    bool writer_failed = false;
    std::string session_id;
    Clock::time_point last_check{}, next_exploration{};
    std::mt19937_64 random;
    std::array<unsigned char, 768> previous{};
    std::array<Detection, 1024> previous_detections{};
    std::size_t previous_count{};
    bool have_previous = false;
    void fail(const char* message) noexcept { state.paused = true; state.manual_pending = false; try { state.error = message; } catch (...) {} }
    void save(Slot& slot) {
        const auto sample_id = std::to_string(slot.id);
        const auto image_relative = std::filesystem::path("images") / (sample_id + ".png");
        const auto image_path = state.session_directory / image_relative;
        const auto image_pending = state.session_directory / "images" / (sample_id + ".pending");
        const auto record_path = state.session_directory / "samples" / (sample_id + ".json");
        const auto record_pending = state.session_directory / "samples" / (sample_id + ".pending");
        cv::Mat view(slot.height, slot.width, CV_8UC3, slot.pixels.data());
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".png", view, encoded)) throw std::runtime_error("PNG 编码失败");
        Json boxes = Json::array();
        for (const auto& box : slot.detections) boxes.push_back({{"class_id", box.class_id}, {"x1", box.x1}, {"y1", box.y1}, {"x2", box.x2}, {"y2", box.y2}, {"confidence", box.confidence}});
        Json record = {{"schema_version", 1}, {"sample_id", sample_id}, {"session_id", session_id},
            {"image", utf8(image_relative)}, {"image_sha256", hash_bytes(encoded)}, {"width", slot.width}, {"height", slot.height},
            {"sequence", slot.timing.sequence}, {"detector_generation", slot.generation}, {"detection_status", DetectionStatusName(slot.status)},
            {"reason", slot.reason}, {"review_state", slot.status == DetectionStatus::SUCCESS ? "PRELABELED" : "RAW"}, {"detections", boxes},
            {"geometry", {{"roi_x", slot.roi_x}, {"roi_y", slot.roi_y}, {"source_width", slot.source_width}, {"source_height", slot.source_height}, {"encoded_width", slot.encoded_width}, {"encoded_height", slot.encoded_height}, {"source_pixels_per_pixel_x", slot.scale_x}, {"source_pixels_per_pixel_y", slot.scale_y}}},
            {"timing", {{"captured_steady_ns", nanoseconds(slot.timing.captured_at)}, {"source_sequence", slot.timing.source_sequence}, {"source_sequence_valid", slot.timing.source_sequence_valid}, {"source_timestamp", slot.timing.source_timestamp}, {"source_timestamp_valid", slot.timing.source_timestamp_valid}, {"source_time_basis", SourceTimeBasisName(slot.timing.source_time_basis)}, {"source_clock_status", SourceClockStatusName(slot.timing.source_clock_status)}}}};
        const auto text = record.dump(2);
        {
            std::lock_guard lock(mutex);
            if (encoded.size() + text.size() > config.max_bytes - state.bytes) {
                fail("采集磁盘预算已满");
                ++state.dropped;
                return;
            }
        }
        write_file(image_pending, encoded.data(), encoded.size());
        write_file(record_pending, text.data(), text.size());
        // JSON 是提交标记；中断留下的孤立图片不会被数据工具纳入训练。
        std::filesystem::rename(image_pending, image_path);
        std::filesystem::rename(record_pending, record_path);
        std::lock_guard lock(mutex);
        ++state.saved;
        state.bytes += encoded.size() + text.size();
    }
    void run() noexcept {
        for (;;) {
            std::size_t index;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || queue_size != 0; });
                if (!queue_size) return;
                index = queue[queue_head];
                queue_head = (queue_head + 1) % queue.size();
                --queue_size;
            }
            try {
                if (writer_failed) { std::lock_guard lock(mutex); ++state.dropped; }
                else save(slots[index]);
            }
            catch (const std::exception& error) { std::lock_guard lock(mutex); writer_failed = true; fail(error.what()); ++state.dropped; }
            catch (...) { std::lock_guard lock(mutex); writer_failed = true; fail("采集后台未知错误"); ++state.dropped; }
            std::lock_guard lock(mutex);
            slots[index].occupied = false;
            --state.queued;
        }
    }
};
Collector::Collector() : impl_(std::make_unique<Impl>()) { Log::register_module("Data"); }
Collector::~Collector() { stop(); }
bool Collector::start(const Config& config, std::string& error) noexcept {
    stop();
    try {
        if (config.root_directory.empty() || config.model_path.empty() || config.class_names.empty() ||
            !config.max_samples || !config.max_bytes || !config.queue_capacity || config.queue_capacity > 64 ||
            config.buffer_bytes < (config.queue_capacity + 1) * 3 || config.buffer_bytes > 512ULL * 1024 * 1024 ||
            config.interval_ms < 0 || config.exploration_interval_ms <= 0 ||
            !std::isfinite(config.novelty_threshold) || config.novelty_threshold <= 0 || config.novelty_threshold > 1 ||
            !std::isfinite(config.uncertain_confidence) || config.uncertain_confidence < 0 || config.uncertain_confidence > 1)
            throw std::runtime_error("采集配置非法：检查目录、类别、模型和配额");
        for (std::size_t i = 0; i < config.class_names.size(); ++i) {
            const auto& name = config.class_names[i];
            if (name.empty() || std::find(config.class_names.begin(), config.class_names.begin() + i, name) != config.class_names.begin() + i) throw std::runtime_error("类别名称不可为空或重复");
        }
        const auto model_path = std::filesystem::u8path(config.model_path);
        if (!std::filesystem::is_regular_file(model_path)) throw std::runtime_error("模型必须是可读取的真实文件");
        const auto model_hash = hash_file(model_path);
        auto& p = *impl_;
        std::lock_guard lock(p.mutex);
        p.config = config;
        p.state = {};
        p.contention_drops = 0;
        p.queue_head = p.queue_size = 0;
        p.accepted = 0;
        p.generation_bound = false;
        p.stopping = false;
        p.writer_failed = false;
        p.have_previous = false;
        p.last_check = p.next_exploration = {};
        if (!p.config.exploration_seed) p.config.exploration_seed = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
        p.random.seed(p.config.exploration_seed);
        p.slots.clear();
        p.slots.resize(config.queue_capacity + 1);
        for (auto& slot : p.slots) { slot.pixels.resize(static_cast<std::size_t>(config.buffer_bytes / p.slots.size())); slot.detections.reserve(1024); }
        p.queue.resize(config.queue_capacity);
        std::filesystem::create_directories(config.root_directory);
        p.session_id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++next_session);
        p.state.session_directory = std::filesystem::absolute(config.root_directory) / p.session_id;
        if (!std::filesystem::create_directory(p.state.session_directory)) throw std::runtime_error("采集目录已存在，拒绝覆盖");
        std::filesystem::create_directory(p.state.session_directory / "images");
        std::filesystem::create_directory(p.state.session_directory / "samples");
        Json session = {{"schema_version", 1}, {"session_id", p.session_id}, {"class_names", config.class_names}, {"model_path", utf8(std::filesystem::absolute(model_path))}, {"model_sha256", model_hash},
            {"policy", {{"version", 1}, {"max_samples", config.max_samples}, {"max_bytes", config.max_bytes}, {"interval_ms", config.interval_ms}, {"novelty_threshold", config.novelty_threshold}, {"exploration_interval_ms", config.exploration_interval_ms}, {"exploration_rule", "uniform_interval_0.5_to_1.5_independent_of_detections"}, {"exploration_seed", p.config.exploration_seed}, {"uncertain_confidence", config.uncertain_confidence}, {"queue_capacity", config.queue_capacity}, {"buffer_bytes", config.buffer_bytes}}}};
        const auto text = session.dump(2);
        write_file(p.state.session_directory / "session.pending", text.data(), text.size());
        std::filesystem::rename(p.state.session_directory / "session.pending", p.state.session_directory / "session.json");
        p.worker = std::thread([&p] { p.run(); });
        p.state.active = true;
        LOG_INFO("Data", "采集会话已启动：{}，样本上限 {}", p.session_id, config.max_samples);
        p.enabled.store(true, std::memory_order_release);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        try { error = exception.what(); std::lock_guard lock(impl_->mutex); impl_->state.active = false; impl_->state.error = error; } catch (...) {}
    } catch (...) { try { error = "采集启动失败"; } catch (...) {} }
    return false;
}
void Collector::stop() noexcept {
    impl_->enabled.store(false, std::memory_order_release);
    { std::lock_guard lock(impl_->mutex); impl_->state.active = false; impl_->state.manual_pending = false; impl_->stopping = true; }
    impl_->ready.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
void Collector::set_paused(bool paused) noexcept {
    std::lock_guard lock(impl_->mutex);
    if (!paused && (!impl_->state.error.empty() || impl_->accepted >= impl_->config.max_samples)) return;
    impl_->state.paused = paused;
}
void Collector::request_sample() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state.active && !impl_->state.paused) impl_->state.manual_pending = true;
}
Snapshot Collector::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->state;
    result.dropped += impl_->contention_drops.load();
    return result;
}
void Collector::offer(const CapturedFrame& frame, std::span<const Detection> detections, DetectionStatus status, std::uint64_t generation) noexcept {
    auto& p = *impl_;
    if (!p.enabled.load(std::memory_order_acquire)) return;
    std::unique_lock lock(p.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++p.contention_drops; return; }
    if (!p.state.active || p.state.paused) return;
    try {
        if (p.generation_bound && generation != p.bound_generation) { p.fail("模型代次已变化，请结束并重新开启采集会话"); return; }
        p.bound_generation = generation; p.generation_bound = true;
        if (frame.storage != CapturedFrameStorage::CPU_BGR) { p.fail("当前 GPU-only 画面不支持采集；未执行 GPU 回读"); return; }
        if (frame.bgr.empty() || frame.bgr.type() != CV_8UC3 || frame.width != frame.bgr.cols || frame.height != frame.bgr.rows ||
            !std::isfinite(frame.roi_x) || !std::isfinite(frame.roi_y) || !std::isfinite(frame.source_pixels_per_pixel_x) || !std::isfinite(frame.source_pixels_per_pixel_y)) { p.fail("采集帧尺寸或几何非法"); return; }
        const auto pixel_bytes = static_cast<std::uint64_t>(frame.width) * frame.height * 3;
        if (pixel_bytes > p.slots.front().pixels.size()) { p.fail("单帧超过独立缓存槽容量"); return; }
        if (detections.size() > 1024) { ++p.state.dropped; return; }
        const auto now = Clock::now();
        const bool manual = p.state.manual_pending;
        if (!manual && p.last_check != Clock::time_point{} && now - p.last_check < std::chrono::milliseconds(p.config.interval_ms)) return;
        p.last_check = now;
        std::array<unsigned char, 768> thumbnail{};
        std::size_t offset = 0;
        double difference = 0;
        for (int y = 0; y < 16; ++y) {
            const auto* row = frame.bgr.ptr<unsigned char>(std::min(frame.height - 1, y * frame.height / 16));
            for (int x = 0; x < 16; ++x) for (int c = 0; c < 3; ++c) {
                const auto value = row[std::min(frame.width - 1, x * frame.width / 16) * 3 + c];
                thumbnail[offset] = value;
                difference += std::abs(static_cast<int>(value) - p.previous[offset++]);
            }
        }
        bool geometry_changed = detections.size() != p.previous_count;
        bool uncertain = false;
        for (std::size_t i = 0; i < detections.size(); ++i) {
            const auto& d = detections[i];
            if (status == DetectionStatus::SUCCESS && (!std::isfinite(d.x1) || !std::isfinite(d.y1) || !std::isfinite(d.x2) || !std::isfinite(d.y2) || !std::isfinite(d.confidence) || d.x1 < 0 || d.y1 < 0 || d.x2 > frame.width || d.y2 > frame.height || d.x1 >= d.x2 || d.y1 >= d.y2 || d.confidence < 0 || d.confidence > 1 || d.class_id < 0 || static_cast<std::size_t>(d.class_id) >= p.config.class_names.size())) { p.fail("预标注类别或坐标不符合当前数据规范"); return; }
            uncertain |= d.confidence < p.config.uncertain_confidence;
            if (i < p.previous_count) { const auto& old = p.previous_detections[i]; geometry_changed |= d.class_id != old.class_id || std::abs(d.x1-old.x1) > frame.width * 0.02f || std::abs(d.y1-old.y1) > frame.height * 0.02f || std::abs(d.x2-old.x2) > frame.width * 0.02f || std::abs(d.y2-old.y2) > frame.height * 0.02f; }
        }
        const bool novel = !p.have_previous || geometry_changed || difference / (768.0 * 255.0) >= p.config.novelty_threshold;
        const bool exploration = now >= p.next_exploration;
        if (!manual && !novel && !exploration) { ++p.state.duplicates; return; }
        if (p.queue_size == p.queue.size()) { ++p.state.dropped; return; }
        auto found = std::find_if(p.slots.begin(), p.slots.end(), [](const auto& slot) { return !slot.occupied; });
        if (found == p.slots.end()) { ++p.state.dropped; return; }
        auto& slot = *found;
        slot.detections.clear();
        if (status == DetectionStatus::SUCCESS) slot.detections.assign(detections.begin(), detections.end());
        for (int y = 0; y < frame.height; ++y) std::memcpy(slot.pixels.data() + static_cast<std::size_t>(y) * frame.width * 3, frame.bgr.ptr(y), static_cast<std::size_t>(frame.width) * 3);
        slot.width = frame.width; slot.height = frame.height; slot.timing = frame.timing;
        slot.roi_x = frame.roi_x; slot.roi_y = frame.roi_y;
        slot.source_width = frame.source_width; slot.source_height = frame.source_height;
        slot.encoded_width = frame.encoded_width; slot.encoded_height = frame.encoded_height;
        slot.scale_x = frame.source_pixels_per_pixel_x; slot.scale_y = frame.source_pixels_per_pixel_y;
        slot.status = status; slot.generation = generation; slot.id = ++p.accepted;
        slot.reason = manual ? "manual" : exploration ? "exploration" : uncertain ? "uncertain" : "novel";
        slot.occupied = true;
        p.queue[(p.queue_head + p.queue_size) % p.queue.size()] = static_cast<std::size_t>(found - p.slots.begin());
        ++p.queue_size; ++p.state.queued;
        p.state.manual_pending = false;
        p.previous = thumbnail;
        p.previous_count = detections.size();
        std::copy(detections.begin(), detections.end(), p.previous_detections.begin());
        p.have_previous = true;
        if (exploration) p.next_exploration = now + std::chrono::milliseconds(std::uniform_int_distribution<std::int64_t>(std::max(1, p.config.exploration_interval_ms / 2), static_cast<std::int64_t>(p.config.exploration_interval_ms) + p.config.exploration_interval_ms / 2)(p.random));
        if (p.accepted >= p.config.max_samples) p.fail("采集数量预算已满");
        p.ready.notify_one();
    } catch (...) { p.fail("采集候选处理失败"); ++p.state.dropped; }
}
}
