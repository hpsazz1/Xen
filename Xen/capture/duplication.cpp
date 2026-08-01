#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "capture/capture.h"

#include "log/log.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#include <opencv2/imgproc.hpp>

namespace capture::detail {
namespace {

using Microsoft::WRL::ComPtr;

class DesktopDuplicationCapture final : public ICapture {
public:
    explicit DesktopDuplicationCapture(const CaptureConfig& config)
        : config_(config) {}

    ~DesktopDuplicationCapture() override { close(); }

    bool open() noexcept override {
        try {
            close();
            Log::register_module("capture", LogLevel::INFO);
            if (!valid_config()) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "Desktop Duplication 配置非法");
            }

            ComPtr<IDXGIFactory1> factory;
            HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (FAILED(hr)) return fail_hresult("创建 DXGI Factory 失败", hr);

            hr = factory->EnumAdapters1(
                static_cast<UINT>(config_.adapter_index), &adapter_);
            if (FAILED(hr)) return fail_hresult("找不到指定显示适配器", hr);

            hr = adapter_->EnumOutputs(
                static_cast<UINT>(config_.output_index), &output_);
            if (FAILED(hr)) return fail_hresult("找不到指定显示输出", hr);

            DXGI_OUTPUT_DESC output_desc{};
            hr = output_->GetDesc(&output_desc);
            if (FAILED(hr)) return fail_hresult("读取显示输出信息失败", hr);
            if (output_desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
                output_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
                return fail(CaptureStatus::UNSUPPORTED,
                            "首版 Desktop Duplication 不支持旋转显示器");
            }

            source_width_ = output_desc.DesktopCoordinates.right -
                            output_desc.DesktopCoordinates.left;
            source_height_ = output_desc.DesktopCoordinates.bottom -
                             output_desc.DesktopCoordinates.top;
            if (!resolve_roi()) {
                return fail(CaptureStatus::INVALID_CONFIG,
                            "采集 ROI 超出显示输出范围");
            }

            constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
            };
            D3D_FEATURE_LEVEL selected_level{};
            hr = D3D11CreateDevice(
                adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)),
                D3D11_SDK_VERSION, &device_, &selected_level, &context_);
            if (hr == E_INVALIDARG) {
                hr = D3D11CreateDevice(
                    adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    &kFeatureLevels[1], 1, D3D11_SDK_VERSION,
                    &device_, &selected_level, &context_);
            }
            if (FAILED(hr)) return fail_hresult("创建 D3D11 设备失败", hr);

            ComPtr<IDXGIOutput1> output1;
            hr = output_.As(&output1);
            if (FAILED(hr)) return fail_hresult("显示输出不支持 DXGI 1.2", hr);
            hr = output1->DuplicateOutput(device_.Get(), &duplication_);
            if (FAILED(hr)) return fail_hresult("创建 Desktop Duplication 失败", hr);

            if (gpu_interop_enabled()) {
                interop_synchronization_ = std::make_shared<std::mutex>();
                if (config_.enable_d3d11_directml_interop &&
                    !create_directml_fence()) {
                    return false;
                }
            } else {
                D3D11_TEXTURE2D_DESC staging_desc{};
                staging_desc.Width = static_cast<UINT>(roi_width_);
                staging_desc.Height = static_cast<UINT>(roi_height_);
                staging_desc.MipLevels = 1;
                staging_desc.ArraySize = 1;
                staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                staging_desc.SampleDesc.Count = 1;
                staging_desc.Usage = D3D11_USAGE_STAGING;
                staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = device_->CreateTexture2D(
                    &staging_desc, nullptr, &staging_);
                if (FAILED(hr)) {
                    return fail_hresult("创建 ROI staging 纹理失败", hr);
                }
            }

            sequence_ = 0;
            set_error({});
            status_.store(CaptureStatus::READY, std::memory_order_release);
            LOG_INFO("capture", "Desktop Duplication 已打开: source={}x{}, roi=({}, {}) {}x{}, storage={}",
                     source_width_, source_height_, roi_x_, roi_y_,
                     roi_width_, roi_height_,
                     config_.enable_d3d11_cuda_interop
                         ? "D3D11_BGRA8/CUDA"
                         : (config_.enable_d3d11_directml_interop
                                ? "D3D11_BGRA8/DirectML" : "CPU_BGR"));
            return true;
        } catch (...) {
            return fail(CaptureStatus::FAILURE,
                        "打开 Desktop Duplication 时发生未知异常");
        }
    }

    bool prepare_frame(CapturedFrame& frame) noexcept override {
        if (!gpu_interop_enabled()) return true;
        if (!device_) {
            return fail(CaptureStatus::CLOSED,
                        "D3D11 GPU 帧槽预创建时 Capture 尚未打开");
        }
        if (!reusable_gpu_texture(frame) && !create_gpu_texture(frame)) {
            return false;
        }
        frame.bgr.release();
        frame.bgr_storage.reset();
        frame.native_synchronization = interop_synchronization_;
        frame.native_fence = config_.enable_d3d11_directml_interop
            ? interop_fence_handle_ : std::shared_ptr<void>{};
        frame.native_fence_value = 0;
        frame.storage = gpu_storage();
        frame.width = roi_width_;
        frame.height = roi_height_;
        return true;
    }

    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        if (!duplication_ || !context_ ||
            (!gpu_interop_enabled() && !staging_)) {
            return CaptureStatus::CLOSED;
        }

        const auto started = std::chrono::steady_clock::now();
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};
        const HRESULT acquire_hr = duplication_->AcquireNextFrame(
            static_cast<UINT>(config_.acquire_timeout_ms), &info, &resource);
        if (acquire_hr == DXGI_ERROR_WAIT_TIMEOUT) {
            status_.store(CaptureStatus::NO_FRAME, std::memory_order_release);
            return CaptureStatus::NO_FRAME;
        }
        if (acquire_hr == DXGI_ERROR_ACCESS_LOST) {
            fail(CaptureStatus::ACCESS_LOST,
                 "Desktop Duplication 访问丢失，需要重建采集会话");
            return CaptureStatus::ACCESS_LOST;
        }
        if (FAILED(acquire_hr)) {
            fail_hresult("Desktop Duplication 取帧失败", acquire_hr);
            return CaptureStatus::FAILURE;
        }
        struct FrameRelease {
            IDXGIOutputDuplication* duplication = nullptr;
            ~FrameRelease() {
                if (duplication) duplication->ReleaseFrame();
            }
        } release{duplication_.Get()};

        try {
            ComPtr<ID3D11Texture2D> desktop_texture;
            HRESULT hr = resource.As(&desktop_texture);
            if (FAILED(hr)) {
                fail_hresult("桌面资源不是 D3D11 纹理", hr);
                return CaptureStatus::FAILURE;
            }

            const D3D11_BOX source_box{
                static_cast<UINT>(roi_x_),
                static_cast<UINT>(roi_y_),
                0,
                static_cast<UINT>(roi_x_ + roi_width_),
                static_cast<UINT>(roi_y_ + roi_height_),
                1,
            };
            if (gpu_interop_enabled()) {
                ID3D11Texture2D* slot_texture =
                    reusable_gpu_texture(frame);
                if (!slot_texture) {
                    fail(CaptureStatus::FAILURE,
                         "D3D11 GPU 帧槽未在 Runtime 启动期预创建");
                    return CaptureStatus::FAILURE;
                }
                if (!frame.native_synchronization ||
                    frame.native_synchronization !=
                        interop_synchronization_) {
                    fail(CaptureStatus::FAILURE,
                         "D3D11 GPU 帧槽缺少当前设备的提交锁");
                    return CaptureStatus::FAILURE;
                }
                {
                    std::lock_guard<std::mutex> lock(
                        *frame.native_synchronization);
                    context_->CopySubresourceRegion(
                        slot_texture, 0, 0, 0, 0,
                        desktop_texture.Get(), 0, &source_box);
                    if (config_.enable_d3d11_directml_interop) {
                        if (!context4_ || !interop_fence_ ||
                            !interop_fence_handle_ ||
                            interop_fence_value_ ==
                                std::numeric_limits<std::uint64_t>::max()) {
                            fail(CaptureStatus::FAILURE,
                                 "D3D11/DirectML 共享 fence 状态非法");
                            return CaptureStatus::FAILURE;
                        }
                        const HRESULT signal_hr = context4_->Signal(
                            interop_fence_.Get(), ++interop_fence_value_);
                        if (FAILED(signal_hr)) {
                            fail_hresult("D3D11 共享 fence Signal 失败",
                                         signal_hr);
                            return CaptureStatus::FAILURE;
                        }
                    }
                    // Flush 只提交而不等待。与 CUDA map/unmap 共用提交锁，
                    // 防止另一个线程在 copy 与 Flush 之间插入隐式同步。
                    context_->Flush();
                }
                frame.bgr.release();
                frame.bgr_storage.reset();
                frame.native_fence = config_.enable_d3d11_directml_interop
                    ? interop_fence_handle_ : std::shared_ptr<void>{};
                frame.native_fence_value =
                    config_.enable_d3d11_directml_interop
                        ? interop_fence_value_ : 0;
                frame.storage = gpu_storage();
                frame.width = roi_width_;
                frame.height = roi_height_;
            } else {
                context_->CopySubresourceRegion(
                    staging_.Get(), 0, 0, 0, 0,
                    desktop_texture.Get(), 0, &source_box);

                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = context_->Map(
                    staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (FAILED(hr)) {
                    fail_hresult("映射 ROI staging 纹理失败", hr);
                    return CaptureStatus::FAILURE;
                }
                struct Unmap {
                    ID3D11DeviceContext* context = nullptr;
                    ID3D11Texture2D* texture = nullptr;
                    ~Unmap() {
                        if (context && texture) context->Unmap(texture, 0);
                    }
                } unmap{context_.Get(), staging_.Get()};

                cv::Mat bgra(roi_height_, roi_width_, CV_8UC4,
                             mapped.pData, mapped.RowPitch);
                // write_slot 可能来自上一次 UDP Runtime；先释放其别名视图和
                // 所有权，再恢复为 Desktop Duplication 自有可写 Mat 缓冲。
                if (frame.bgr_storage) {
                    frame.bgr.release();
                    frame.bgr_storage.reset();
                }
                frame.native_storage.reset();
                frame.native_synchronization.reset();
                frame.native_fence.reset();
                frame.native_fence_value = 0;
                frame.bgr.create(roi_height_, roi_width_, CV_8UC3);
                cv::cvtColor(bgra, frame.bgr, cv::COLOR_BGRA2BGR);
                frame.storage = CapturedFrameStorage::CPU_BGR;
                frame.width = roi_width_;
                frame.height = roi_height_;
            }

            const auto finished = std::chrono::steady_clock::now();
            frame.timing.sequence = ++sequence_;
            frame.timing.captured_at = finished;
            frame.timing.capture_ms =
                std::chrono::duration<double, std::milli>(finished - started).count();
            frame.timing.source_dropped_frames = 0;
            frame.roi_x = roi_x_;
            frame.roi_y = roi_y_;
            frame.source_width = source_width_;
            frame.source_height = source_height_;
            frame.encoded_width = source_width_;
            frame.encoded_height = source_height_;
            frame.source_pixels_per_pixel_x = 1.0;
            frame.source_pixels_per_pixel_y = 1.0;
            status_.store(CaptureStatus::FRAME, std::memory_order_release);
            return CaptureStatus::FRAME;
        } catch (...) {
            fail(CaptureStatus::FAILURE,
                 "转换 Desktop Duplication 帧时发生未知异常");
            return CaptureStatus::FAILURE;
        }
    }

    void close() noexcept override {
        staging_.Reset();
        duplication_.Reset();
        context4_.Reset();
        interop_fence_.Reset();
        interop_fence_handle_.reset();
        interop_fence_value_ = 0;
        context_.Reset();
        device5_.Reset();
        device_.Reset();
        interop_synchronization_.reset();
        output_.Reset();
        adapter_.Reset();
        status_.store(CaptureStatus::CLOSED, std::memory_order_release);
    }

    CaptureStatus status() const noexcept override {
        return status_.load(std::memory_order_acquire);
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return last_error_;
    }

private:
    ID3D11Texture2D* reusable_gpu_texture(
            const CapturedFrame& frame) const noexcept {
        if (frame.storage != gpu_storage() ||
            !frame.native_storage ||
            frame.native_synchronization != interop_synchronization_ ||
            !device_) {
            return nullptr;
        }
        auto* texture = static_cast<ID3D11Texture2D*>(
            frame.native_storage.get());
        ID3D11Device* owner = nullptr;
        texture->GetDevice(&owner);
        const bool same_device = owner == device_.Get();
        if (owner) owner->Release();
        if (!same_device) return nullptr;

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        const bool directml_contract =
            !config_.enable_d3d11_directml_interop ||
            ((description.MiscFlags &
              D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0 &&
             (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
             frame.native_fence == interop_fence_handle_);
        return description.Width == static_cast<UINT>(roi_width_) &&
               description.Height == static_cast<UINT>(roi_height_) &&
               description.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
               directml_contract
            ? texture : nullptr;
    }

    bool create_gpu_texture(CapturedFrame& frame) noexcept {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(roi_width_);
        description.Height = static_cast<UINT>(roi_height_);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        if (config_.enable_d3d11_directml_interop) {
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            // SHARED + SHARED_NTHANDLE 生成可由 D3D12 打开的 NT handle 资源；
            // 不启用 keyed mutex，跨 D3D11/D3D12 排序统一由显式 shared fence 完成。
            description.MiscFlags =
                D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                D3D11_RESOURCE_MISC_SHARED;
        }

        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = device_->CreateTexture2D(
            &description, nullptr, &texture);
        if (FAILED(hr)) {
            return fail_hresult("创建 D3D11 GPU ROI 纹理失败", hr);
        }
        ID3D11Texture2D* owned = texture.Detach();
        try {
            frame.native_storage = std::shared_ptr<void>(
                owned, [](void* value) noexcept {
                    if (value) {
                        static_cast<ID3D11Texture2D*>(value)->Release();
                    }
                });
        } catch (...) {
            owned->Release();
            return fail(CaptureStatus::FAILURE,
                        "创建 D3D11 GPU ROI 纹理所有权失败");
        }
        frame.native_synchronization = interop_synchronization_;
        frame.native_fence = config_.enable_d3d11_directml_interop
            ? interop_fence_handle_ : std::shared_ptr<void>{};
        frame.native_fence_value = 0;
        frame.storage = gpu_storage();
        return true;
    }

    bool create_directml_fence() noexcept {
        HRESULT hr = device_.As(&device5_);
        if (FAILED(hr)) {
            return fail_hresult("D3D11 设备不支持共享 fence", hr);
        }
        hr = context_.As(&context4_);
        if (FAILED(hr)) {
            return fail_hresult("D3D11 immediate context 不支持共享 fence", hr);
        }
        hr = device5_->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&interop_fence_));
        if (FAILED(hr)) {
            return fail_hresult("创建 D3D11 共享 fence 失败", hr);
        }
        HANDLE handle = nullptr;
        hr = interop_fence_->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr) || !handle) {
            return fail_hresult("创建 D3D11 fence shared handle 失败", hr);
        }
        try {
            interop_fence_handle_ = std::shared_ptr<void>(
                handle, [](void* value) noexcept {
                    if (value) CloseHandle(static_cast<HANDLE>(value));
                });
        } catch (...) {
            CloseHandle(handle);
            return fail(CaptureStatus::FAILURE,
                        "创建 D3D11 fence handle 所有权失败");
        }
        interop_fence_value_ = 0;
        return true;
    }

    bool gpu_interop_enabled() const noexcept {
        return config_.enable_d3d11_cuda_interop ||
               config_.enable_d3d11_directml_interop;
    }

    CapturedFrameStorage gpu_storage() const noexcept {
        return config_.enable_d3d11_directml_interop
            ? CapturedFrameStorage::D3D11_BGRA8_DIRECTML
            : CapturedFrameStorage::D3D11_BGRA8;
    }

    bool valid_config() const noexcept {
        return config_.adapter_index >= 0 && config_.output_index >= 0 &&
               config_.roi_width > 0 && config_.roi_height > 0 &&
               config_.acquire_timeout_ms >= 0 &&
               !(config_.enable_d3d11_cuda_interop &&
                 config_.enable_d3d11_directml_interop);
    }

    bool resolve_roi() noexcept {
        roi_width_ = std::min(config_.roi_width, source_width_);
        roi_height_ = std::min(config_.roi_height, source_height_);
        if (config_.center_roi) {
            roi_x_ = (source_width_ - roi_width_) / 2;
            roi_y_ = (source_height_ - roi_height_) / 2;
        } else {
            roi_x_ = config_.roi_x;
            roi_y_ = config_.roi_y;
        }
        return roi_x_ >= 0 && roi_y_ >= 0 && roi_width_ > 0 &&
               roi_height_ > 0 && roi_x_ + roi_width_ <= source_width_ &&
               roi_y_ + roi_height_ <= source_height_;
    }

    bool fail(CaptureStatus status, const std::string& message) noexcept {
        set_error(message);
        status_.store(status, std::memory_order_release);
        LOG_ERROR("capture", "{}", message);
        return false;
    }

    bool fail_hresult(const char* operation, HRESULT hr) noexcept {
        return fail(CaptureStatus::FAILURE,
                    std::string(operation) + "，HRESULT=" +
                    std::to_string(static_cast<long>(hr)));
    }

    void set_error(const std::string& message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = message;
        } catch (...) {
        }
    }

    CaptureConfig config_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Device5> device5_;
    ComPtr<ID3D11DeviceContext4> context4_;
    ComPtr<ID3D11Fence> interop_fence_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    std::shared_ptr<std::mutex> interop_synchronization_;
    std::shared_ptr<void> interop_fence_handle_;
    std::atomic<CaptureStatus> status_{CaptureStatus::CLOSED};
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::uint64_t sequence_ = 0;
    std::uint64_t interop_fence_value_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    int roi_x_ = 0;
    int roi_y_ = 0;
    int roi_width_ = 0;
    int roi_height_ = 0;
};

} // namespace

std::unique_ptr<ICapture> create_desktop_duplication_capture(
        const CaptureConfig& config) noexcept {
    try {
        return std::make_unique<DesktopDuplicationCapture>(config);
    } catch (...) {
        return nullptr;
    }
}

} // namespace capture::detail
