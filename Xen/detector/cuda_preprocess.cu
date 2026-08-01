#include "detector/cuda_preprocess.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>

#include <array>
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

__global__ void bgra_u8_to_rgb_chw_f32_kernel(
        const unsigned char* source,
        float* destination,
        int width,
        int height) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const std::size_t pixel =
        static_cast<std::size_t>(y) * width + x;
    const std::size_t source_index = pixel * 4U;
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
    struct RegisteredResource {
        void* texture = nullptr;
        cudaGraphicsResource_t resource = nullptr;
    };

    // Runtime 当前只有三槽；保留八项固定上限便于独立 Detector 测试，同时
    // 禁止热路径因新纹理地址扩容或触发逐帧堆分配。
    static constexpr std::size_t kMaximumRegisteredResources = 8;

    unsigned char* host_input = nullptr;
    unsigned char* device_input = nullptr;
    std::size_t byte_count = 0;
    std::size_t bgra_byte_count = 0;
    int width = 0;
    int height = 0;
    int device_id = 0;
    cudaError_t last_error = cudaSuccess;
    std::array<RegisteredResource, kMaximumRegisteredResources>
        registered_resources{};
    std::size_t registered_count = 0;

    ~Impl() noexcept {
        reset();
    }

    void reset() noexcept {
        if (device_input || host_input || registered_count > 0) {
            cudaSetDevice(device_id);
        }
        // 注册会持有 D3D11 资源引用，必须在 Session/设备销毁前逐项注销。
        for (std::size_t index = 0; index < registered_count; ++index) {
            if (registered_resources[index].resource) {
                cudaGraphicsUnregisterResource(
                    registered_resources[index].resource);
            }
            registered_resources[index] = {};
        }
        registered_count = 0;
        if (device_input) {
            cudaFree(device_input);
            device_input = nullptr;
        }
        if (host_input) {
            cudaFreeHost(host_input);
            host_input = nullptr;
        }
        byte_count = 0;
        bgra_byte_count = 0;
        width = 0;
        height = 0;
    }

    cudaGraphicsResource_t registered_resource(
            void* texture) const noexcept {
        for (std::size_t index = 0; index < registered_count; ++index) {
            if (registered_resources[index].texture == texture) {
                return registered_resources[index].resource;
            }
        }
        return nullptr;
    }

    bool d3d11_device_matches(ID3D11Texture2D* texture) noexcept {
        ID3D11Device* d3d_device = nullptr;
        texture->GetDevice(&d3d_device);
        if (!d3d_device) {
            last_error = cudaErrorInvalidDevice;
            return false;
        }

        constexpr unsigned int kMaximumDevices = 8;
        unsigned int count = 0;
        int devices[kMaximumDevices]{};
        last_error = cudaD3D11GetDevices(
            &count, devices, kMaximumDevices, d3d_device,
            cudaD3D11DeviceListAll);
        d3d_device->Release();
        if (last_error != cudaSuccess || count == 0 ||
            count > kMaximumDevices) {
            return false;
        }
        for (unsigned int index = 0; index < count; ++index) {
            if (devices[index] == device_id) return true;
        }
        last_error = cudaErrorInvalidDevice;
        return false;
    }

    cudaGraphicsResource_t register_d3d11_resource(
            void* raw_texture) noexcept {
        if (!raw_texture ||
            registered_count == kMaximumRegisteredResources) {
            last_error = cudaErrorInvalidResourceHandle;
            return nullptr;
        }
        auto* texture = static_cast<ID3D11Texture2D*>(raw_texture);
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Width != static_cast<UINT>(width) ||
            description.Height != static_cast<UINT>(height) ||
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            description.MipLevels != 1 || description.ArraySize != 1 ||
            !d3d11_device_matches(texture)) {
            if (last_error == cudaSuccess) {
                last_error = cudaErrorInvalidValue;
            }
            return nullptr;
        }

        cudaGraphicsResource_t resource = nullptr;
        last_error = cudaGraphicsD3D11RegisterResource(
            &resource, texture, cudaGraphicsRegisterFlagsNone);
        if (last_error != cudaSuccess) return nullptr;
        registered_resources[registered_count++] = {
            raw_texture, resource};
        return resource;
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
    impl_->bgra_byte_count = static_cast<std::size_t>(width) * height * 4U;

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
        reinterpret_cast<void**>(&impl_->device_input),
        impl_->bgra_byte_count);
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

bool CudaPreprocessor::register_d3d11_bgra(
        void* d3d11_texture,
        int width,
        int height,
        std::mutex& synchronization) noexcept {
    if (!ready() || !d3d11_texture ||
        width != impl_->width || height != impl_->height) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }
    std::lock_guard<std::mutex> lock(synchronization);
    if (impl_->registered_resource(d3d11_texture)) return true;
    return impl_->register_d3d11_resource(d3d11_texture) != nullptr;
}

bool CudaPreprocessor::enqueue_d3d11_bgra(
        void* d3d11_texture,
        int width,
        int height,
        std::mutex& synchronization,
        cudaStream_t stream) noexcept {
    if (!ready() || !d3d11_texture || !stream ||
        width != impl_->width || height != impl_->height) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }

    // D3D11 immediate context 与 legacy CUDA graphics interop 都会建立隐式
    // 设备同步。锁住 host-side 提交窗口，禁止另一个线程把 D3D copy 插入
    // map 与 unmap 之间或插入“copy 已调用但尚未 Flush”的窗口。
    std::lock_guard<std::mutex> lock(synchronization);
    cudaGraphicsResource_t resource =
        impl_->registered_resource(d3d11_texture);
    if (!resource) return false;

    impl_->last_error = cudaGraphicsMapResources(1, &resource, stream);
    if (impl_->last_error != cudaSuccess) return false;

    cudaArray_t mapped_array = nullptr;
    impl_->last_error = cudaGraphicsSubResourceGetMappedArray(
        &mapped_array, resource, 0, 0);
    if (impl_->last_error == cudaSuccess) {
        const std::size_t row_bytes =
            static_cast<std::size_t>(impl_->width) * 4U;
        impl_->last_error = cudaMemcpy2DFromArrayAsync(
            impl_->device_input, row_bytes, mapped_array, 0, 0,
            row_bytes, static_cast<std::size_t>(impl_->height),
            cudaMemcpyDeviceToDevice, stream);
    }

    const cudaError_t work_error = impl_->last_error;
    const cudaError_t unmap_error =
        cudaGraphicsUnmapResources(1, &resource, stream);
    impl_->last_error = work_error != cudaSuccess
        ? work_error : unmap_error;
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

bool CudaPreprocessor::enqueue_convert_bgra(
        float* device_output, cudaStream_t stream) noexcept {
    if (!ready() || !device_output || !stream) {
        if (impl_) impl_->last_error = cudaErrorInvalidValue;
        return false;
    }

    constexpr dim3 block(16, 16);
    const dim3 grid(
        (impl_->width + block.x - 1) / block.x,
        (impl_->height + block.y - 1) / block.y);
    bgra_u8_to_rgb_chw_f32_kernel<<<grid, block, 0, stream>>>(
        impl_->device_input, device_output,
        impl_->width, impl_->height);
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

std::size_t CudaPreprocessor::d3d11_copy_bytes() const noexcept {
    return impl_ ? impl_->bgra_byte_count : 0;
}

cudaError_t CudaPreprocessor::last_error() const noexcept {
    return impl_ ? impl_->last_error : cudaErrorInvalidValue;
}

} // namespace detector::detail
