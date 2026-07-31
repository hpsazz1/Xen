#ifndef DETECTOR_CUDA_PREPROCESS_H
#define DETECTOR_CUDA_PREPROCESS_H

#include <cstddef>
#include <memory>

#include <cuda_runtime_api.h>
#include <opencv2/core.hpp>

namespace detector::detail {

/// TensorRT CUDA Graph 前处理工作区。
///
/// 模型尺寸固定后，页锁定主机缓冲、uint8 设备缓冲和目标 float 设备地址都必须
/// 跨帧稳定。stage() 只覆盖已有主机缓冲，enqueue_*() 只在调用方提供的同一
/// CUDA stream 上排队，不执行隐式同步。
class CudaPreprocessor {
public:
    CudaPreprocessor();
    ~CudaPreprocessor();

    CudaPreprocessor(const CudaPreprocessor&) = delete;
    CudaPreprocessor& operator=(const CudaPreprocessor&) = delete;
    CudaPreprocessor(CudaPreprocessor&&) noexcept;
    CudaPreprocessor& operator=(CudaPreprocessor&&) noexcept;

    bool init(int device_id, int width, int height) noexcept;
    bool stage(const cv::Mat& bgr_image) noexcept;
    bool enqueue_upload(cudaStream_t stream) noexcept;
    bool enqueue_convert(float* device_output,
                         cudaStream_t stream) noexcept;

    bool ready() const noexcept;
    std::size_t upload_bytes() const noexcept;
    cudaError_t last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detector::detail

#endif // DETECTOR_CUDA_PREPROCESS_H
