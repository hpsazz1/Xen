#ifndef XEN_DATA_COLLECTION_H
#define XEN_DATA_COLLECTION_H

#include <capture/capture.h>
#include <detector/detector.h>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace data_collection {
struct Config {
    std::filesystem::path root_directory;
    std::vector<std::string> class_names;
    std::string model_path;
    std::uint64_t max_samples = 300;
    // PNG 与样本 JSON 的写入预算；不含固定 session.json 和文件系统元数据。
    std::uint64_t max_bytes = 1024ULL * 1024 * 1024;
    int interval_ms = 1000;
    float novelty_threshold = 0.04f;
    int exploration_interval_ms = 10000;
    std::size_t queue_capacity = 8;
    std::uint64_t buffer_bytes = 32ULL * 1024 * 1024;
    float uncertain_confidence = 0.5f;
    std::uint64_t exploration_seed = 0; // 0 在启动时生成并记录实际种子。
};
struct Snapshot {
    bool active = false;
    bool paused = false;
    bool manual_pending = false;
    std::uint64_t saved = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicates = 0;
    std::size_t queued = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path session_directory;
    std::string error;
};
// start/stop 由单一生命周期调用方串行调用；offer 只有一个生产者。
// 原图只复制到本模块预分配槽，后台不保留 Capture 的引用。
class Collector {
public:
    Collector();
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    bool start(const Config& config, std::string& error) noexcept;
    void stop() noexcept;
    void set_paused(bool paused) noexcept;
    void request_sample() noexcept;
    void offer(const CapturedFrame& frame, std::span<const Detection> detections,
               DetectionStatus status, std::uint64_t detector_generation) noexcept;
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
