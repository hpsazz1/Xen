#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "detector/detector.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
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
    D3D11TextureFrame frame;

    bool init(int cuda_device, int width, int height) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumerated = factory->EnumAdapters1(index, &candidate);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) return false;
            int mapped_device = -1;
            if (cudaD3D11GetDevice(&mapped_device, candidate.Get()) ==
                    cudaSuccess &&
                mapped_device == cuda_device) {
                adapter = std::move(candidate);
                break;
            }
        }
        if (!adapter) return false;

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
        if (FAILED(created)) return false;

        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(
                &description, nullptr, &texture))) {
            return false;
        }

        ID3D11Texture2D* owned = texture.Detach();
        try {
            frame.resource = std::shared_ptr<void>(
                owned, [](void* value) noexcept {
                    if (value) {
                        static_cast<ID3D11Texture2D*>(value)->Release();
                    }
                });
        } catch (...) {
            owned->Release();
            return false;
        }
        frame.width = width;
        frame.height = height;
        frame.synchronization = std::make_shared<std::mutex>();
        return true;
    }

    void upload(const cv::Scalar& bgr) {
        // UpdateSubresource 接受 BGRA；alpha 固定 255，不参与 Detector 输入。
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(frame.width) * frame.height * 4U);
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4U) {
            pixels[offset] = static_cast<std::uint8_t>(bgr[0]);
            pixels[offset + 1] = static_cast<std::uint8_t>(bgr[1]);
            pixels[offset + 2] = static_cast<std::uint8_t>(bgr[2]);
            pixels[offset + 3] = 255U;
        }
        context->UpdateSubresource(
            static_cast<ID3D11Texture2D*>(frame.resource.get()), 0,
            nullptr, pixels.data(), static_cast<UINT>(frame.width * 4), 0);
    }
};

void run_test(const std::string& model_path,
              const std::string& cache_path) {
    DetectorConfig config;
    config.model_path = model_path;
    config.backend = BackendType::TENSORRT;
    config.enable_fp16 = true;
    config.enable_trt_cuda_graph = true;
    config.enable_gpu_preprocess = true;
    config.enable_output_fingerprint = true;
    config.trt_cache_path = cache_path;

    Detector detector(config);
    expect(detector.load(), "TensorRT Detector 应成功加载真实模型");
    if (!detector.loaded()) return;
    expect(detector.backend_name() == "TensorrtExecutionProvider" &&
               detector.d3d11_interop_supported(),
           "测试必须实际运行 TensorRT CUDA Graph 互操作能力");
    if (!detector.d3d11_interop_supported()) return;

    D3D11Fixture fixture;
    expect(fixture.init(
               config.device_id, detector.input_width(),
               detector.input_height()),
           "应在与 CUDA device 对应的 D3D11 适配器上创建 BGRA8 纹理");
    if (!fixture.frame.resource) return;
    const bool prepared = detector.prepare_d3d11(fixture.frame);
    expect(prepared,
           "D3D11 纹理必须在首次图形写入前完成 CUDA 注册");
    if (!prepared) return;

    const cv::Scalar first_color(16, 80, 224);
    const cv::Scalar second_color(224, 48, 12);
    const cv::Mat first_cpu(
        detector.input_height(), detector.input_width(), CV_8UC3,
        first_color);
    const cv::Mat second_cpu(
        detector.input_height(), detector.input_width(), CV_8UC3,
        second_color);

    const auto first_cpu_detections = detector.detect(first_cpu);
    const auto first_cpu_profile = detector.profile();
    fixture.upload(first_color);
    const auto first_gpu_detections = detector.detect_d3d11(fixture.frame);
    const auto first_gpu_profile = detector.profile();

    fixture.upload(second_color);
    const auto second_gpu_detections = detector.detect_d3d11(fixture.frame);
    const auto second_gpu_profile = detector.profile();
    const auto second_cpu_detections = detector.detect(second_cpu);
    const auto second_cpu_profile = detector.profile();

    expect(first_cpu_profile.status == DetectionStatus::SUCCESS &&
               first_gpu_profile.status == DetectionStatus::SUCCESS &&
               second_gpu_profile.status == DetectionStatus::SUCCESS &&
               second_cpu_profile.status == DetectionStatus::SUCCESS,
           "CPU BGR 与连续变化 D3D11 纹理都必须完整推理成功");
    expect(first_gpu_profile.d3d11_cuda_interop &&
               first_gpu_profile.h2d_ms == 0.0 &&
               first_gpu_profile.input_upload_bytes == 0 &&
               first_gpu_profile.input_device_copy_bytes ==
                   static_cast<std::uint64_t>(detector.input_width()) *
                       detector.input_height() * 4U,
           "互操作 profile 必须证明零 host upload 和 BGRA 设备复制字节数");
    expect(first_cpu_profile.output_fingerprint ==
               first_gpu_profile.output_fingerprint &&
               second_cpu_profile.output_fingerprint ==
               second_gpu_profile.output_fingerprint,
           "同色 CPU BGR 与 D3D11 BGRA 输入必须生成相同原始输出指纹");
    expect(first_gpu_profile.output_fingerprint !=
               second_gpu_profile.output_fingerprint,
           "同一注册纹理连续写入不同内容时输出必须随输入变化");
    expect(same_detections(first_cpu_detections, first_gpu_detections) &&
               same_detections(second_cpu_detections, second_gpu_detections),
           "CPU 与 D3D11 互操作检测结果必须在约定容差内一致");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        std::cerr << "用法: d3d11_cuda_interop_tests <model.onnx> [cache]\n";
        return 2;
    }
    const std::string cache_path = argc >= 3 && argv[2]
        ? argv[2] : "cache/tensorrt/d3d11-interop-test";
    run_test(argv[1], cache_path);
    if (failures != 0) {
        std::cerr << "D3D11/CUDA 互操作测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "D3D11/CUDA 互操作测试全部通过。\n";
    return 0;
}
