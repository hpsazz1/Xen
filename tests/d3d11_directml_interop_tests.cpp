#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "detector/detector.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool same_detections(const std::vector<Detection>& left,
                     const std::vector<Detection>& right) {
    if (left.size() != right.size()) return false;
    constexpr float kTolerance = 1e-4f;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.class_id != b.class_id ||
            std::abs(a.x1 - b.x1) > kTolerance ||
            std::abs(a.y1 - b.y1) > kTolerance ||
            std::abs(a.x2 - b.x2) > kTolerance ||
            std::abs(a.y2 - b.y2) > kTolerance ||
            std::abs(a.confidence - b.confidence) > kTolerance) {
            return false;
        }
    }
    return true;
}

struct D3D11Fixture {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    D3D11TextureFrame frame;

    bool failed(const char* operation, HRESULT result) {
        std::cerr << operation << " HRESULT=0x" << std::hex
                  << static_cast<unsigned long>(result) << std::dec << '\n';
        return false;
    }

    bool init(int adapter_index, int width, int height) {
        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return failed("CreateDXGIFactory1", result);
        result = factory->EnumAdapters1(
            static_cast<UINT>(adapter_index), &adapter);
        if (FAILED(result)) return failed("EnumAdapters1", result);

        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selected{};
        HRESULT created = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &device, &selected, &context);
        if (created == E_INVALIDARG) {
            created = D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &levels[1], 1, D3D11_SDK_VERSION,
                &device, &selected, &context);
        }
        if (FAILED(created)) return failed("D3D11CreateDevice", created);
        result = device.As(&device5);
        if (FAILED(result)) return failed("ID3D11Device5", result);
        result = context.As(&context4);
        if (FAILED(result)) return failed("ID3D11DeviceContext4", result);
        result = device5->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        if (FAILED(result)) return failed("CreateFence", result);

        HANDLE fence_handle = nullptr;
        result = fence->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &fence_handle);
        if (FAILED(result) || !fence_handle) {
            return failed("Fence CreateSharedHandle", result);
        }
        try {
            frame.shared_fence = std::shared_ptr<void>(
                fence_handle, [](void* value) noexcept {
                    if (value) CloseHandle(static_cast<HANDLE>(value));
                });
        } catch (...) {
            CloseHandle(fence_handle);
            return false;
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
            D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        result = device->CreateTexture2D(
            &description, nullptr, &texture);
        if (FAILED(result)) return failed("CreateTexture2D", result);

        ID3D11Texture2D* owned = texture.Detach();
        try {
            frame.resource = std::shared_ptr<void>(
                owned, [](void* value) noexcept {
                    if (value) {
                        static_cast<ID3D11Texture2D*>(value)->Release();
                    }
                });
            frame.synchronization = std::make_shared<std::mutex>();
        } catch (...) {
            owned->Release();
            return false;
        }
        frame.width = width;
        frame.height = height;
        return true;
    }

    bool upload(const cv::Scalar& bgr) {
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(frame.width) * frame.height * 4U);
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4U) {
            pixels[offset] = static_cast<std::uint8_t>(bgr[0]);
            pixels[offset + 1] = static_cast<std::uint8_t>(bgr[1]);
            pixels[offset + 2] = static_cast<std::uint8_t>(bgr[2]);
            pixels[offset + 3] = 255U;
        }
        std::lock_guard<std::mutex> lock(*frame.synchronization);
        context->UpdateSubresource(
            static_cast<ID3D11Texture2D*>(frame.resource.get()), 0,
            nullptr, pixels.data(), static_cast<UINT>(frame.width * 4), 0);
        const std::uint64_t next_value = frame.fence_value + 1;
        if (FAILED(context4->Signal(fence.Get(), next_value))) return false;
        context->Flush();
        frame.fence_value = next_value;
        return true;
    }
};

void run_test(const std::string& model_path) {
    DetectorConfig config;
    config.model_path = model_path;
    config.backend = BackendType::DIRECTML;
    config.enable_output_fingerprint = true;

    Detector detector(config);
    expect(detector.load(), "DirectML Detector 应成功加载真实模型");
    if (!detector.loaded()) return;
    expect(detector.backend_name() == "DmlExecutionProvider" &&
               detector.d3d11_interop_supported(),
           "测试必须实际运行严格 DirectML D3D11 输入能力");
    if (!detector.d3d11_interop_supported()) return;

    D3D11Fixture fixture;
    expect(fixture.init(
               config.device_id, detector.input_width(),
               detector.input_height()),
           "应在 DirectML adapter 上创建 shared NT BGRA8 纹理和 fence");
    if (!fixture.frame.resource || !fixture.frame.shared_fence) return;
    expect(detector.prepare_d3d11(fixture.frame),
           "D3D11 纹理必须在业务线程启动前由 D3D12 打开并预录制命令");
    if (failures != 0) return;

    // 0/255 在 CPU 与 UNORM shader 中都精确表示，既能做原始输出指纹等值
    // 校验，又避免中间色的浮点归一化舍入差异制造伪失败。
    const cv::Scalar first_color(0, 0, 255);
    const cv::Scalar second_color(255, 0, 0);
    const cv::Mat first_cpu(
        detector.input_height(), detector.input_width(), CV_8UC3,
        first_color);
    const cv::Mat second_cpu(
        detector.input_height(), detector.input_width(), CV_8UC3,
        second_color);

    const auto first_cpu_detections = detector.detect(first_cpu);
    const auto first_cpu_profile = detector.profile();
    expect(fixture.upload(first_color), "第一张 D3D11 纹理写入和 Signal 应成功");
    const auto first_gpu_detections = detector.detect_d3d11(fixture.frame);
    const auto first_gpu_profile = detector.profile();

    expect(fixture.upload(second_color), "第二张 D3D11 纹理写入和 Signal 应成功");
    const auto second_gpu_detections = detector.detect_d3d11(fixture.frame);
    const auto second_gpu_profile = detector.profile();
    const auto second_cpu_detections = detector.detect(second_cpu);
    const auto second_cpu_profile = detector.profile();

    expect(first_cpu_profile.status == DetectionStatus::SUCCESS &&
               first_gpu_profile.status == DetectionStatus::SUCCESS &&
               second_gpu_profile.status == DetectionStatus::SUCCESS &&
               second_cpu_profile.status == DetectionStatus::SUCCESS,
           "CPU BGR 与连续变化 D3D11 纹理都必须完整推理成功");
    expect(first_gpu_profile.d3d11_directml_interop &&
               !first_gpu_profile.d3d11_cuda_interop &&
               first_gpu_profile.h2d_ms == 0.0 &&
               first_gpu_profile.input_upload_bytes == 0 &&
               first_gpu_profile.input_device_copy_bytes == 0 &&
               first_gpu_profile.d3d11_to_directml_ms >= 0.0,
           "DirectML profile 必须证明零 host upload、零中间设备复制和共享链路计时");
    expect(first_cpu_profile.output_fingerprint ==
               first_gpu_profile.output_fingerprint &&
               second_cpu_profile.output_fingerprint ==
               second_gpu_profile.output_fingerprint,
           "同色 CPU BGR 与 D3D11 BGRA 输入必须生成相同原始输出指纹");
    expect(first_gpu_profile.output_fingerprint !=
               second_gpu_profile.output_fingerprint,
           "同一共享纹理连续写入不同内容时输出必须随输入变化");
    expect(same_detections(first_cpu_detections, first_gpu_detections) &&
               same_detections(second_cpu_detections, second_gpu_detections),
           "CPU 与 D3D11/DirectML 检测结果必须在约定容差内一致");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        std::cerr << "用法: d3d11_directml_interop_tests <model.onnx>\n";
        return 2;
    }
    run_test(argv[1]);
    if (failures != 0) {
        std::cerr << "D3D11/DirectML 互操作测试失败数: "
                  << failures << '\n';
        return 1;
    }
    std::cout << "D3D11/DirectML 互操作测试全部通过。\n";
    return 0;
}
