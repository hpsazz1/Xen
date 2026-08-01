#ifndef OVERLAY_INTERNAL_H
#define OVERLAY_INTERNAL_H

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace overlay::detail {

// Overlay 只保留固定数量的标量诊断样本。覆盖最旧值时不分配内存，也不反压 Runtime。
template <std::size_t Capacity>
class MetricHistory {
    static_assert(Capacity > 0);

public:
    void push(float value) noexcept {
        values_[write_index_] = value;
        write_index_ = (write_index_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
    }

    void clear() noexcept {
        size_ = 0;
        write_index_ = 0;
    }

    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    float at(std::size_t index) const noexcept {
        if (index >= size_) return 0.0f;
        const std::size_t oldest = size_ == Capacity ? write_index_ : 0;
        return values_[(oldest + index) % Capacity];
    }

    float latest() const noexcept {
        return empty() ? 0.0f : at(size_ - 1);
    }

    float maximum(float floor) const noexcept {
        float result = floor;
        for (std::size_t index = 0; index < size_; ++index) {
            if (at(index) > result) result = at(index);
        }
        return result;
    }

private:
    std::array<float, Capacity> values_{};
    std::size_t size_ = 0;
    std::size_t write_index_ = 0;
};

struct PreviewPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct PreviewRect {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct PreviewSize {
    float width = 0.0f;
    float height = 0.0f;
};

inline PreviewSize fit_preview_size(int width,
                                    int height,
                                    float maximum_width,
                                    float maximum_height) noexcept {
    if (width <= 0 || height <= 0 || maximum_width <= 0.0f ||
        maximum_height <= 0.0f || !std::isfinite(maximum_width) ||
        !std::isfinite(maximum_height)) {
        return {};
    }
    const float scale = std::min({
        1.0f,
        maximum_width / static_cast<float>(width),
        maximum_height / static_cast<float>(height)});
    return {
        static_cast<float>(width) * scale,
        static_cast<float>(height) * scale};
}

inline PreviewPoint map_preview_point(float x,
                                      float y,
                                      float scale_x,
                                      float scale_y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        scale_x <= 0.0f || scale_y <= 0.0f) {
        return {};
    }
    return {x * scale_x, y * scale_y};
}

inline PreviewRect map_preview_rect(float x1,
                                    float y1,
                                    float x2,
                                    float y2,
                                    float scale_x,
                                    float scale_y,
                                    float width,
                                    float height) noexcept {
    const PreviewPoint first =
        map_preview_point(x1, y1, scale_x, scale_y);
    const PreviewPoint second =
        map_preview_point(x2, y2, scale_x, scale_y);
    const float minimum_x = std::clamp(
        std::min(first.x, second.x), 0.0f, std::max(0.0f, width));
    const float minimum_y = std::clamp(
        std::min(first.y, second.y), 0.0f, std::max(0.0f, height));
    const float maximum_x = std::clamp(
        std::max(first.x, second.x), 0.0f, std::max(0.0f, width));
    const float maximum_y = std::clamp(
        std::max(first.y, second.y), 0.0f, std::max(0.0f, height));
    return {minimum_x, minimum_y, maximum_x, maximum_y};
}

} // namespace overlay::detail

#endif // OVERLAY_INTERNAL_H
