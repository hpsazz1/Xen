#include "detector/cuda_preprocess.h"

#include <cuda_runtime.h>

#include <cstring>
#include <limits>
#include <utility>

namespace detector::detail {
namespace {

__global__ void bgr_u8_to_rgb_chw_f32_kernel(
        const unsigned char* source,
        float* destination,
        int width,
        int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const std::size_t pixel =
        static_cast<std::size_t>(y) * width + x;
    const std::size_t source_index = pixel * 3U;
    const std::size_t plane_size =
        static_cast<std::size_t>(width) * height;
    constexpr float kNormalize = 1.0f / 255.0f;

    destination[pixel] =
        static_cast<float>(source[source_index + 2U]) * kNormalize;
    destination[plane_size + pixel] =
        static_cast<float>(source[source_index + 1U]) * kNormalize;
    destination[plane_size * 2U + pixel] =
        static_cast<float>(source[source_index]) * kNormalize;
}

} // namespace

struct CudaPreprocessor::Impl {
    unsigned char* host_input = nullptr;
    unsigned char* device_input = nullptr;
    std::size_t byte_count = 0;
    int width = 0;
    int height = 0;
    int device_id = 0;
    cudaError_t last_error = cudaSuccess;

    ~Impl() noexcept {
        reset();
    }

    void reset() noexcept {
        if (device_input || host_input) {
            cudaSetDevice(device_id);
        }
        if (device_input) {
            cudaFree(device_input);
            device_input = nullptr;
        }
        if (host_input) {
            cudaFreeHost(host_input);
            host_input = nullptr;
        }
        byte_count = 0;
        width = 0;
        height = 0;
    }
};

CudaPreprocessor::CudaPreprocessor()
    : impl_(std::make_unique<Impl>()) {}

CudaPreprocessor::~CudaPreprocessor() = default;
CudaPreprocessor::CudaPreprocessor(CudaPreprocessor&&) noexcept = default;
CudaPreprocessor& CudaPreprocessor::operator=(
    CudaPreprocessor&&) noexcept = default;

bool CudaPreprocessor::init(
        int device_id, int width, int height) noexcept {
    if (!impl_ || device_id < 0 || width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    if (static_cast<std::size_t>(width) >
        max_size / static_cast<std::size_t>(height) / 3U) {
        return false;
    }

    impl_->reset();
    impl_->device_id = device_id;
    impl_->width = width;
    impl_->height = height;
    impl_->byte_count = static_cast<std::size_t>(width) * height * 3U;

    impl_->last_error = cudaSetDevice(device_id);
    if (impl_->last_error != cudaSuccess) {
        impl_->reset();
        return false;
    }
    impl_->last_error = cudaMallocHost(
        reinterpret_cast<void**>(&impl_->host_input), impl_->byte_count);
    if (impl_->last_error != cudaSuccess) {
        impl_->reset();
        return false;
    }
    impl_->last_error = cudaMalloc(
        reinterpret_cast<void**>(&impl_->device_input), impl_->byte_count);
    if (impl_->last_error != cudaSuccess) {
        impl_->reset();
        return false;
    }
    return true;
}

bool CudaPreprocessor::stage(const cv::Mat& bgr_image) noexcept {
    if (!ready() || bgr_image.empty() || bgr_image.type() != CV_8UC3 ||
        bgr_image.cols != impl_->width || bgr_image.rows != impl_->height) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }

    const std::size_t row_bytes =
        static_cast<std::size_t>(impl_->width) * 3U;
    if (bgr_image.isContinuous() && bgr_image.step == row_bytes) {
        std::memcpy(impl_->host_input, bgr_image.ptr(), impl_->byte_count);
        return true;
    }
    for (int row = 0; row < impl_->height; ++row) {
        std::memcpy(
            impl_->host_input + static_cast<std::size_t>(row) * row_bytes,
            bgr_image.ptr(row), row_bytes);
    }
    return true;
}

bool CudaPreprocessor::enqueue_upload(cudaStream_t stream) noexcept {
    if (!ready() || !stream) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }
    impl_->last_error = cudaMemcpyAsync(
        impl_->device_input, impl_->host_input, impl_->byte_count,
        cudaMemcpyHostToDevice, stream);
    return impl_->last_error == cudaSuccess;
}

bool CudaPreprocessor::enqueue_convert(
        float* device_output, cudaStream_t stream) noexcept {
    if (!ready() || !device_output || !stream) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }

    constexpr dim3 block(16, 16);
    const dim3 grid(
        (impl_->width + block.x - 1) / block.x,
        (impl_->height + block.y - 1) / block.y);
    bgr_u8_to_rgb_chw_f32_kernel<<<grid, block, 0, stream>>>(
        impl_->device_input, device_output, impl_->width, impl_->height);
    impl_->last_error = cudaPeekAtLastError();
    return impl_->last_error == cudaSuccess;
}

bool CudaPreprocessor::ready() const noexcept {
    return impl_ && impl_->host_input && impl_->device_input &&
           impl_->byte_count > 0;
}

std::size_t CudaPreprocessor::upload_bytes() const noexcept {
    return impl_ ? impl_->byte_count : 0;
}

cudaError_t CudaPreprocessor::last_error() const noexcept {
    return impl_ ? impl_->last_error : cudaErrorInvalidValue;
}

} // namespace detector::detail
