#ifndef XEN_LINEUP_H
#define XEN_LINEUP_H
#include "capture/capture.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace lineup {
using Clock = std::chrono::steady_clock;
struct FrameIdentity {
    std::string session_id;
    std::uint64_t sequence = 0;
};
enum class ReferenceMode { FULL_FRAME, ROI };
enum class Status { VALID, NOT_FOUND, UNRELIABLE, EXPIRED, INVALID_FRAME, NO_REFERENCE, ERROR };
const char *status_name(Status status) noexcept;
struct Observation {
    Status status = Status::NO_REFERENCE;
    std::string reason;
    std::string reference_id;
    FrameIdentity frame;
    std::optional<cv::Point2d> aim;
    std::optional<cv::Point2d> source_aim;
    int matches = 0;
    int inliers = 0;
    double inlier_ratio = 0;
    double coverage = 0;
    double residual_px = 0;
    double local_correlation = 0;
    double processing_ms = 0;
    double local_age_ms = 0;
    std::optional<double> source_age_ms;
};
struct ReferenceInfo {
    std::string id;
    std::string label;
    FrameIdentity frame;
    cv::Point2d aim;
    cv::Size image_size;
    cv::Size source_size;
};
// 实例仅由计算线程串行使用；录制和定位都不修改传入原图。
class PreparedFrame {
    friend class Engine;
    cv::Mat gray_image_, descriptors_;
    std::vector<cv::KeyPoint> points_;
    FrameIdentity identity_;
    ReferenceMode mode_ = ReferenceMode::FULL_FRAME;
    const unsigned char *pixels_ = nullptr;
  public:
    double feature_ms = 0;
};
class Engine {
  public:
    Engine();
    ~Engine();
    Engine(Engine &&) noexcept;
    Engine &operator=(Engine &&) noexcept;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    static bool validate_frame(const CapturedFrame &frame, const std::string &session,
                               Clock::time_point now, std::string &error,
                               ReferenceMode mode = ReferenceMode::FULL_FRAME) noexcept;
    bool create_reference(const CapturedFrame &frame, const std::string &session,
                          const std::string &id, const std::string &label, cv::Point2d aim,
                          const cv::Mat &static_mask, Clock::time_point now,
                          std::string &error, ReferenceMode mode = ReferenceMode::FULL_FRAME) noexcept;
    bool annotate_reference(cv::Point2d aim, const cv::Mat &static_mask, const std::string &new_id,
                            std::string &error) noexcept;
    // 原子发布 root/id，拒绝覆盖。加载失败会清除旧参考，避免旧结果冒充新选择。
    bool save_reference(const std::filesystem::path &root, std::string &error) const noexcept;
    bool load_reference(const std::filesystem::path &directory, std::string &error) noexcept;
    Observation locate(const CapturedFrame &frame, const std::string &session,
                       Clock::time_point now, ReferenceMode mode = ReferenceMode::FULL_FRAME) noexcept;
    static PreparedFrame prepare(const CapturedFrame &frame, const std::string &session,
                                 ReferenceMode mode = ReferenceMode::FULL_FRAME);
    Observation locate(const CapturedFrame &frame, const std::string &session,
                       Clock::time_point now, const PreparedFrame &prepared,
                       ReferenceMode mode = ReferenceMode::FULL_FRAME) noexcept;
    std::size_t memory_bytes() const noexcept;
    std::optional<ReferenceInfo> reference_info() const;
    cv::Mat reference_image() const;
    void clear() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Single-worker cache; published references are immutable. Explicit clear after catalog edits.
class ReferenceCache {
  public:
    struct Stats { std::size_t entries = 0, bytes = 0, hits = 0, misses = 0, evictions = 0; };
    explicit ReferenceCache(std::size_t max_entries = 32, std::size_t max_bytes = 128 * 1024 * 1024);
    ~ReferenceCache();
    Engine *get(const std::filesystem::path &directory, std::string &error);
    void clear();
    Stats stats() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lineup
#endif
