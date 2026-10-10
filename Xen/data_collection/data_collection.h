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
class Collector;
namespace detail {
struct LifecycleAdapter;
std::unique_ptr<Collector> make_collector_with_lifecycle_adapter(LifecycleAdapter adapter);
}
struct Config {
    std::filesystem::path root_directory;
    std::vector<std::string> class_names;
    std::string model_path;
    // 连续保存到用户结束或真实写入失败；只约束瞬时缓存和队列。
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
    bool draining = false;
    std::uint64_t saved = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t filtered = 0;
    std::size_t queued = 0;
    std::uint64_t bytes = 0;
    std::filesystem::path session_directory;
    std::string error;
};
// start/stop 串行拥有线程句柄；多个 stop 调用等待同一次回收。offer 只有一个生产者。
// 原图只复制到本模块预分配槽，后台不保留 Capture 的引用。
class Collector {
public:
    Collector();
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    bool start(const Config& config, std::string& error) noexcept;
    // UI 只封口；poll_stop 仅在写入线程已完成时尝试回收，不等待写盘。
    void request_stop() noexcept;
    void poll_stop() noexcept;
    // 同步停止供 Runtime/析构使用，返回时已完成排空与回收。
    void stop() noexcept;
    void set_paused(bool paused) noexcept;
    void request_sample() noexcept;
    // automatic_allowed 只限制自动采集；人工请求仍可保存用于诊断。
    void offer(const CapturedFrame& frame, std::span<const Detection> detections,
               DetectionStatus status, std::uint64_t detector_generation,
               bool automatic_allowed = true) noexcept;
    Snapshot snapshot() const;
private:
    friend std::unique_ptr<Collector> detail::make_collector_with_lifecycle_adapter(detail::LifecycleAdapter);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
