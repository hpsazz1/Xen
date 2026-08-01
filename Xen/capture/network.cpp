#include "capture/network_internal.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace capture::detail {

NetworkLatestFramePool::NetworkLatestFramePool() {
    for (auto& slot : pool_) {
        slot = std::make_shared<NetworkDecodedFrame>();
    }
}

std::shared_ptr<NetworkDecodedFrame>
NetworkLatestFramePool::acquire_write() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& slot : pool_) {
            // pool_ 自身持有一个引用；消费者仍持有的槽不得覆写。
            if (slot != latest_ && slot.use_count() == 1) return slot;
        }
    } catch (...) {
    }
    return nullptr;
}

void NetworkLatestFramePool::publish(
    const std::shared_ptr<NetworkDecodedFrame>& frame) noexcept {
    if (!frame || frame->bgr.empty() || frame->timing.sequence == 0) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ && latest_->timing.sequence != consumed_sequence_) {
            ++dropped_frames_;
        }
        frame->timing.source_dropped_frames = dropped_frames_;
        latest_ = frame;
    } catch (...) {
    }
}

bool NetworkLatestFramePool::take_latest(
    std::uint64_t last_sequence,
    CapturedFrame& frame) noexcept {
    try {
        std::shared_ptr<NetworkDecodedFrame> latest;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latest_ || latest_->timing.sequence == last_sequence) {
                return false;
            }
            latest_->timing.source_dropped_frames = dropped_frames_;
            latest = latest_;
            consumed_sequence_ = latest_->timing.sequence;
        }

        // 先释放旧视图，再归还旧槽引用，避免生产者覆写仍在使用的 Mat。
        frame.bgr.release();
        frame.bgr_storage.reset();
        frame.native_storage.reset();
        frame.native_synchronization.reset();
        frame.native_fence.reset();
        frame.native_fence_value = 0;
        frame.bgr_storage = std::shared_ptr<const cv::Mat>(latest, &latest->bgr);
        frame.bgr = *frame.bgr_storage;
        frame.storage = CapturedFrameStorage::CPU_BGR;
        frame.width = frame.bgr.cols;
        frame.height = frame.bgr.rows;
        frame.timing = latest->timing;
        frame.roi_x = latest->roi_x;
        frame.roi_y = latest->roi_y;
        frame.source_width = latest->source_width;
        frame.source_height = latest->source_height;
        frame.encoded_width = latest->encoded_width;
        frame.encoded_height = latest->encoded_height;
        frame.source_pixels_per_pixel_x = latest->source_pixels_per_pixel_x;
        frame.source_pixels_per_pixel_y = latest->source_pixels_per_pixel_y;
        return true;
    } catch (...) {
        return false;
    }
}

void NetworkLatestFramePool::record_drop() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dropped_frames_;
    } catch (...) {
    }
}

std::uint64_t NetworkLatestFramePool::dropped_frames() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_frames_;
    } catch (...) {
        return 0;
    }
}

void NetworkLatestFramePool::reset() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset();
        consumed_sequence_ = 0;
        dropped_frames_ = 0;
        for (const auto& slot : pool_) {
            if (slot.use_count() != 1) continue;
            slot->timing = {};
            slot->roi_x = 0.0;
            slot->roi_y = 0.0;
            slot->source_width = 0;
            slot->source_height = 0;
            slot->encoded_width = 0;
            slot->encoded_height = 0;
            slot->source_pixels_per_pixel_x = 1.0;
            slot->source_pixels_per_pixel_y = 1.0;
        }
    } catch (...) {
    }
}

namespace {

constexpr int kMaxSourceDimension = 16384;

void skip_space(std::string_view value, std::size_t& position) noexcept {
    while (position < value.size() &&
           std::isspace(static_cast<unsigned char>(value[position]))) {
        ++position;
    }
}

bool parse_nonnegative_int(std::string_view value, int& output) noexcept {
    if (value.empty()) return false;
    int parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed < 0) {
        return false;
    }
    output = parsed;
    return true;
}

bool known_attribute(std::string_view key) noexcept {
    return key == "version" || key == "source_width" ||
           key == "source_height" || key == "roi_x" || key == "roi_y" ||
           key == "roi_width" || key == "roi_height";
}

} // namespace

bool parse_xen_frame_metadata(std::string_view metadata,
                              XenFrameMetadata& parsed) noexcept {
    try {
        XenFrameMetadata result;
        bool has_version = false;
        bool has_source_width = false;
        bool has_source_height = false;
        bool has_roi_x = false;
        bool has_roi_y = false;
        bool has_roi_width = false;
        bool has_roi_height = false;
        std::size_t position = 0;
        skip_space(metadata, position);
        if (metadata.substr(position, 4) != "<xen") return false;
        position += 4;
        if (position < metadata.size() &&
            !std::isspace(static_cast<unsigned char>(metadata[position])) &&
            metadata[position] != '/' && metadata[position] != '>') {
            return false;
        }

        while (true) {
            skip_space(metadata, position);
            if (position + 2 <= metadata.size() &&
                metadata.substr(position, 2) == "/>") {
                position += 2;
                break;
            }
            if (position >= metadata.size() || metadata[position] == '>') {
                return false;
            }
            const std::size_t key_begin = position;
            while (position < metadata.size() &&
                   (std::isalnum(static_cast<unsigned char>(metadata[position])) ||
                    metadata[position] == '_')) {
                ++position;
            }
            const std::string_view key = metadata.substr(
                key_begin, position - key_begin);
            if (!known_attribute(key) || key.empty()) return false;
            skip_space(metadata, position);
            if (position >= metadata.size() || metadata[position] != '=') {
                return false;
            }
            ++position;
            skip_space(metadata, position);
            if (position >= metadata.size() ||
                (metadata[position] != '\'' && metadata[position] != '"')) {
                return false;
            }
            const char quote = metadata[position++];
            const std::size_t value_begin = position;
            while (position < metadata.size() && metadata[position] != quote) {
                if (metadata[position] == '<') return false;
                ++position;
            }
            if (position >= metadata.size()) return false;
            const std::string_view value = metadata.substr(
                value_begin, position - value_begin);
            ++position;

            int parsed_value = 0;
            if (key == "version") {
                if (has_version || value != "1") return false;
                has_version = true;
            } else {
                if (!parse_nonnegative_int(value, parsed_value)) return false;
                if (key == "source_width") {
                    if (has_source_width) return false;
                    result.source_width = parsed_value;
                    has_source_width = true;
                } else if (key == "source_height") {
                    if (has_source_height) return false;
                    result.source_height = parsed_value;
                    has_source_height = true;
                } else if (key == "roi_x") {
                    if (has_roi_x) return false;
                    result.roi_x = parsed_value;
                    has_roi_x = true;
                } else if (key == "roi_y") {
                    if (has_roi_y) return false;
                    result.roi_y = parsed_value;
                    has_roi_y = true;
                } else if (key == "roi_width") {
                    if (has_roi_width) return false;
                    result.roi_width = parsed_value;
                    has_roi_width = true;
                } else if (key == "roi_height") {
                    if (has_roi_height) return false;
                    result.roi_height = parsed_value;
                    has_roi_height = true;
                }
            }
        }
        skip_space(metadata, position);
        if (position != metadata.size() || !has_version ||
            !has_source_width || !has_source_height || !has_roi_x ||
            !has_roi_y || !has_roi_width || !has_roi_height ||
            result.source_width <= 0 || result.source_height <= 0 ||
            result.source_width > kMaxSourceDimension ||
            result.source_height > kMaxSourceDimension ||
            result.roi_x < 0 || result.roi_y < 0 ||
            result.roi_width <= 0 || result.roi_height <= 0 ||
            result.roi_x > result.source_width ||
            result.roi_y > result.source_height ||
            result.roi_width > result.source_width - result.roi_x ||
            result.roi_height > result.source_height - result.roi_y) {
            return false;
        }
        parsed = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool resolve_network_frame_geometry(
    const NetworkGeometryConfig& config,
    int encoded_width,
    int encoded_height,
    NetworkFrameGeometry& geometry,
    const XenFrameMetadata* metadata) noexcept {
    try {
        if (encoded_width <= 0 || encoded_height <= 0 ||
            config.roi_width <= 0 || config.roi_height <= 0) {
            return false;
        }

        NetworkFrameGeometry resolved;
        resolved.encoded_width = encoded_width;
        resolved.encoded_height = encoded_height;
        if (metadata && config.roi_width == metadata->roi_width &&
            config.roi_height == metadata->roi_height &&
            ((encoded_width == metadata->roi_width &&
              encoded_height == metadata->roi_height) ||
             (encoded_width == metadata->source_width &&
              encoded_height == metadata->source_height))) {
            resolved.source_width = metadata->source_width;
            resolved.source_height = metadata->source_height;
            resolved.source_roi_x = metadata->roi_x;
            resolved.source_roi_y = metadata->roi_y;
            resolved.source_pixels_per_pixel_x = 1.0;
            resolved.source_pixels_per_pixel_y = 1.0;
            if (encoded_width == metadata->roi_width &&
                encoded_height == metadata->roi_height) {
                resolved.decoded_roi_x = 0;
                resolved.decoded_roi_y = 0;
                resolved.decoded_roi_width = encoded_width;
                resolved.decoded_roi_height = encoded_height;
            } else {
                resolved.decoded_roi_x = metadata->roi_x;
                resolved.decoded_roi_y = metadata->roi_y;
                resolved.decoded_roi_width = metadata->roi_width;
                resolved.decoded_roi_height = metadata->roi_height;
            }
            geometry = resolved;
            return true;
        }

        if (config.center_roi) {
            resolved.decoded_roi_width =
                std::min(config.roi_width, encoded_width);
            resolved.decoded_roi_height =
                std::min(config.roi_height, encoded_height);
            resolved.decoded_roi_x =
                (encoded_width - resolved.decoded_roi_width) / 2;
            resolved.decoded_roi_y =
                (encoded_height - resolved.decoded_roi_height) / 2;
        } else {
            resolved.decoded_roi_x = config.roi_x;
            resolved.decoded_roi_y = config.roi_y;
            resolved.decoded_roi_width = config.roi_width;
            resolved.decoded_roi_height = config.roi_height;
        }
        if (resolved.decoded_roi_x < 0 || resolved.decoded_roi_y < 0 ||
            resolved.decoded_roi_width <= 0 ||
            resolved.decoded_roi_height <= 0 ||
            resolved.decoded_roi_x + resolved.decoded_roi_width > encoded_width ||
            resolved.decoded_roi_y + resolved.decoded_roi_height > encoded_height) {
            return false;
        }

        switch (config.layout) {
            case NetworkFrameLayout::FULL_FRAME_1_TO_1:
                if (config.source_width != 0 || config.source_height != 0) {
                    return false;
                }
                resolved.source_width = encoded_width;
                resolved.source_height = encoded_height;
                resolved.source_roi_x = resolved.decoded_roi_x;
                resolved.source_roi_y = resolved.decoded_roi_y;
                break;
            case NetworkFrameLayout::FULL_FRAME_SCALED:
                if (config.source_width <= 0 || config.source_height <= 0) {
                    return false;
                }
                resolved.source_width = config.source_width;
                resolved.source_height = config.source_height;
                resolved.source_pixels_per_pixel_x =
                    static_cast<double>(resolved.source_width) / encoded_width;
                resolved.source_pixels_per_pixel_y =
                    static_cast<double>(resolved.source_height) / encoded_height;
                resolved.source_roi_x = resolved.decoded_roi_x *
                    resolved.source_pixels_per_pixel_x;
                resolved.source_roi_y = resolved.decoded_roi_y *
                    resolved.source_pixels_per_pixel_y;
                break;
            case NetworkFrameLayout::CENTER_CROP_1_TO_1:
                if (config.source_width < encoded_width ||
                    config.source_height < encoded_height ||
                    config.source_width <= 0 || config.source_height <= 0) {
                    return false;
                }
                resolved.source_width = config.source_width;
                resolved.source_height = config.source_height;
                resolved.source_roi_x =
                    (resolved.source_width - encoded_width) / 2 +
                    resolved.decoded_roi_x;
                resolved.source_roi_y =
                    (resolved.source_height - encoded_height) / 2 +
                    resolved.decoded_roi_y;
                break;
            default:
                return false;
        }
        geometry = resolved;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace capture::detail
