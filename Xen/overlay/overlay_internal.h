#ifndef OVERLAY_INTERNAL_H
#define OVERLAY_INTERNAL_H

#include <array>
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

} // namespace overlay::detail

#endif // OVERLAY_INTERNAL_H
