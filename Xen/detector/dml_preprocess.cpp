#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "detector/dml_preprocess.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#ifdef ERROR
#undef ERROR
#endif

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace detector::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaximumRegisteredTextures = 8;
constexpr UINT kThreadGroupSize = 16;

constexpr char kBgraToChwShader[] = R"(
Texture2D<float4> source_texture : register(t0);
RWStructuredBuffer<float> output_tensor : register(u0);

cbuffer Dimensions : register(b0) {
    uint image_width;
    uint image_height;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= image_width || id.y >= image_height) return;
    const uint index = id.y * image_width + id.x;
    const uint plane = image_width * image_height;
    const float4 bgra = source_texture.Load(int3(id.xy, 0));
    // B8G8R8A8 的 SRV 在 shader 中按逻辑 RGBA 返回，底层字节重排由格式完成。
    output_tensor[index] = bgra.x;
    output_tensor[plane + index] = bgra.y;
    output_tensor[plane * 2 + index] = bgra.z;
}
)";

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES value{};
    value.Type = type;
    value.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    value.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    value.CreationNodeMask = 1;
    value.VisibleNodeMask = 1;
    return value;
}

D3D12_RESOURCE_DESC buffer_desc(
        std::uint64_t bytes,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
    D3D12_RESOURCE_DESC value{};
    value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    value.Alignment = 0;
    value.Width = bytes;
    value.Height = 1;
    value.DepthOrArraySize = 1;
    value.MipLevels = 1;
    value.Format = DXGI_FORMAT_UNKNOWN;
    value.SampleDesc.Count = 1;
    value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    value.Flags = flags;
    return value;
}

std::string hresult_text(const char* operation, HRESULT hr) {
    char buffer[96]{};
    std::snprintf(
        buffer, sizeof(buffer), "%s (HRESULT=0x%08lX)",
        operation, static_cast<unsigned long>(hr));
    return buffer;
}

} // namespace

struct DmlPreprocessor::Impl {
    struct TextureEntry {
        void* key = nullptr;
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12DescriptorHeap> descriptors;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
    };

    const OrtDmlApi* dml_api = nullptr;
    ComPtr<ID3D12CommandQueue> command_queue;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12Resource> input_buffer;
    void* dml_allocation = nullptr;
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    ComPtr<ID3D12Resource> timestamp_readback;
    ComPtr<ID3D12CommandAllocator> start_allocator;
    ComPtr<ID3D12GraphicsCommandList> start_command_list;
    ComPtr<ID3D12Fence> shared_fence;
    void* shared_fence_handle = nullptr;
    std::vector<TextureEntry> textures;
    std::uint64_t timestamp_frequency = 0;
    std::size_t tensor_byte_count = 0;
    int device_id = 0;
    int width = 0;
    int height = 0;
    bool timing_pending = false;
    std::string error;

    ~Impl() noexcept {
        wait_idle();
        textures.clear();
        start_command_list.Reset();
        start_allocator.Reset();
        timestamp_readback.Reset();
        timestamp_heap.Reset();
        shared_fence.Reset();
        if (dml_allocation && dml_api) {
            OrtStatus* status = dml_api->FreeGPUAllocation(dml_allocation);
            if (status) Ort::GetApi().ReleaseStatus(status);
            dml_allocation = nullptr;
        }
        input_buffer.Reset();
        pipeline_state.Reset();
        root_signature.Reset();
        device.Reset();
        command_queue.Reset();
    }

    bool fail(std::string message) noexcept {
        try {
            error = std::move(message);
        } catch (...) {
        }
        return false;
    }

    bool fail_hresult(const char* operation, HRESULT hr) noexcept {
        try {
            return fail(hresult_text(operation, hr));
        } catch (...) {
            return false;
        }
    }

    void wait_idle() noexcept {
        if (!device || !command_queue) return;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            return;
        }
        HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle) return;
        constexpr std::uint64_t kFenceValue = 1;
        if (SUCCEEDED(command_queue->Signal(fence.Get(), kFenceValue)) &&
            fence->GetCompletedValue() < kFenceValue &&
            SUCCEEDED(fence->SetEventOnCompletion(kFenceValue, event_handle))) {
            WaitForSingleObject(event_handle, 5000);
        }
        CloseHandle(event_handle);
    }

    TextureEntry* find_texture(void* key) noexcept {
        for (auto& entry : textures) {
            if (entry.key == key) return &entry;
        }
        return nullptr;
    }

    bool create_pipeline() noexcept {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> shader_errors;
        HRESULT hr = D3DCompile(
            kBgraToChwShader, std::strlen(kBgraToChwShader),
            "XenDmlBgraToChw", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &shader_errors);
        if (FAILED(hr)) {
            std::string detail = "编译 D3D12 BGRA→CHW shader 失败";
            if (shader_errors && shader_errors->GetBufferPointer()) {
                detail.append(": ").append(
                    static_cast<const char*>(shader_errors->GetBufferPointer()),
                    shader_errors->GetBufferSize());
            }
            return fail(std::move(detail));
        }

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.Num32BitValues = 2;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC signature_desc{};
        signature_desc.NumParameters = 3;
        signature_desc.pParameters = parameters;
        signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> signature_errors;
        hr = D3D12SerializeRootSignature(
            &signature_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &signature, &signature_errors);
        if (FAILED(hr)) {
            return fail_hresult("序列化 D3D12 root signature 失败", hr);
        }
        hr = device->CreateRootSignature(
            0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&root_signature));
        if (FAILED(hr)) {
            return fail_hresult("创建 D3D12 root signature 失败", hr);
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
        pipeline_desc.pRootSignature = root_signature.Get();
        pipeline_desc.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline_desc.CS.BytecodeLength = shader->GetBufferSize();
        hr = device->CreateComputePipelineState(
            &pipeline_desc, IID_PPV_ARGS(&pipeline_state));
        if (FAILED(hr)) {
            return fail_hresult("创建 D3D12 compute pipeline 失败", hr);
        }
        return true;
    }

    bool create_timing_resources(D3D12_COMMAND_LIST_TYPE list_type) noexcept {
        D3D12_QUERY_HEAP_DESC query_desc{};
        query_desc.Count = 2;
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        HRESULT hr = device->CreateQueryHeap(
            &query_desc, IID_PPV_ARGS(&timestamp_heap));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 时间戳堆失败", hr);

        const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const auto readback_desc = buffer_desc(sizeof(std::uint64_t) * 2);
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&timestamp_readback));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 时间戳回读缓冲失败", hr);

        hr = device->CreateCommandAllocator(
            list_type, IID_PPV_ARGS(&start_allocator));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 起始命令分配器失败", hr);
        hr = device->CreateCommandList(
            0, list_type, start_allocator.Get(), nullptr,
            IID_PPV_ARGS(&start_command_list));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 起始命令列表失败", hr);
        start_command_list->EndQuery(
            timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        hr = start_command_list->Close();
        if (FAILED(hr)) return fail_hresult("关闭 D3D12 起始命令列表失败", hr);
        return true;
    }

    bool open_shared_fence(void* handle) noexcept {
        if (!handle) return fail("D3D11/DirectML 共享 fence handle 为空");
        if (shared_fence && shared_fence_handle == handle) return true;
        if (shared_fence) {
            return fail("同一 DirectML 会话不能混用多个 Capture 共享 fence");
        }
        HRESULT hr = device->OpenSharedHandle(
            static_cast<HANDLE>(handle), IID_PPV_ARGS(&shared_fence));
        if (FAILED(hr)) return fail_hresult("D3D12 打开 D3D11 共享 fence 失败", hr);
        shared_fence_handle = handle;
        return true;
    }

    bool create_texture_entry(void* d3d11_texture) noexcept {
        if (!d3d11_texture) return fail("D3D11 纹理为空");
        if (textures.size() >= kMaximumRegisteredTextures) {
            return fail("D3D11/DirectML 纹理缓存超过 8 个固定槽");
        }

        auto* texture11 = static_cast<ID3D11Texture2D*>(d3d11_texture);
        D3D11_TEXTURE2D_DESC desc11{};
        texture11->GetDesc(&desc11);
        if (desc11.Width != static_cast<UINT>(width) ||
            desc11.Height != static_cast<UINT>(height) ||
            desc11.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc11.MipLevels != 1 || desc11.ArraySize != 1 ||
            desc11.SampleDesc.Count != 1 ||
            (desc11.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0) {
            return fail("D3D11/DirectML 纹理必须是固定尺寸、单样本、shared NT handle 的 BGRA8");
        }

        ComPtr<IDXGIResource1> dxgi_resource;
        HRESULT hr = texture11->QueryInterface(IID_PPV_ARGS(&dxgi_resource));
        if (FAILED(hr)) return fail_hresult("获取 IDXGIResource1 失败", hr);
        HANDLE texture_handle = nullptr;
        hr = dxgi_resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &texture_handle);
        if (FAILED(hr)) return fail_hresult("创建 D3D11 纹理 shared NT handle 失败", hr);

        TextureEntry entry;
        entry.key = d3d11_texture;
        hr = device->OpenSharedHandle(
            texture_handle, IID_PPV_ARGS(&entry.texture));
        CloseHandle(texture_handle);
        if (FAILED(hr)) return fail_hresult("D3D12 打开 D3D11 共享纹理失败", hr);

        const D3D12_RESOURCE_DESC desc12 = entry.texture->GetDesc();
        if (desc12.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc12.Width != static_cast<UINT64>(width) ||
            desc12.Height != static_cast<UINT>(height) ||
            desc12.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            return fail("D3D12 打开后的共享纹理描述不符合 BGRA8 固定输入契约");
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 2;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(
            &heap_desc, IID_PPV_ARGS(&entry.descriptors));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 描述符堆失败", hr);

        const UINT increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu =
            entry.descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(entry.texture.Get(), &srv_desc, srv_cpu);

        D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu = srv_cpu;
        uav_cpu.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.NumElements = static_cast<UINT>(
            static_cast<std::uint64_t>(width) * height * 3);
        uav_desc.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(
            input_buffer.Get(), nullptr, &uav_desc, uav_cpu);

        const D3D12_COMMAND_LIST_TYPE list_type = command_queue->GetDesc().Type;
        hr = device->CreateCommandAllocator(
            list_type, IID_PPV_ARGS(&entry.allocator));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 预处理命令分配器失败", hr);
        hr = device->CreateCommandList(
            0, list_type, entry.allocator.Get(), pipeline_state.Get(),
            IID_PPV_ARGS(&entry.command_list));
        if (FAILED(hr)) return fail_hresult("创建 D3D12 预处理命令列表失败", hr);

        ID3D12DescriptorHeap* heaps[] = {entry.descriptors.Get()};
        entry.command_list->SetDescriptorHeaps(1, heaps);
        entry.command_list->SetComputeRootSignature(root_signature.Get());
        entry.command_list->SetPipelineState(pipeline_state.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu =
            entry.descriptors->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = srv_gpu;
        uav_gpu.ptr += increment;
        entry.command_list->SetComputeRootDescriptorTable(0, srv_gpu);
        entry.command_list->SetComputeRootDescriptorTable(1, uav_gpu);
        const UINT dimensions[] = {
            static_cast<UINT>(width), static_cast<UINT>(height)};
        entry.command_list->SetComputeRoot32BitConstants(
            2, 2, dimensions, 0);

        D3D12_RESOURCE_BARRIER to_read{};
        to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_read.Transition.pResource = entry.texture.Get();
        to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        entry.command_list->ResourceBarrier(1, &to_read);
        entry.command_list->Dispatch(
            (static_cast<UINT>(width) + kThreadGroupSize - 1) / kThreadGroupSize,
            (static_cast<UINT>(height) + kThreadGroupSize - 1) / kThreadGroupSize,
            1);

        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = input_buffer.Get();
        barriers[1] = to_read;
        barriers[1].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        entry.command_list->ResourceBarrier(2, barriers);
        entry.command_list->EndQuery(
            timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        entry.command_list->ResolveQueryData(
            timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            0, 2, timestamp_readback.Get(), 0);
        hr = entry.command_list->Close();
        if (FAILED(hr)) return fail_hresult("关闭 D3D12 预处理命令列表失败", hr);

        textures.push_back(std::move(entry));
        return true;
    }
};

DmlPreprocessor::DmlPreprocessor() : impl_(std::make_unique<Impl>()) {}
DmlPreprocessor::~DmlPreprocessor() = default;

bool DmlPreprocessor::init(
        const OrtDmlApi* dml_api,
        ID3D12CommandQueue* command_queue,
        int device_id,
        int width,
        int height) noexcept {
    if (!impl_ || !dml_api || !command_queue || device_id < 0 ||
        width <= 0 || height <= 0) {
        return false;
    }
    try {
        const std::uint64_t elements =
            static_cast<std::uint64_t>(width) *
            static_cast<std::uint64_t>(height) * 3;
        if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
            elements > std::numeric_limits<UINT>::max()) {
            return impl_->fail("DirectML 输入张量尺寸溢出");
        }

        impl_->dml_api = dml_api;
        impl_->command_queue = command_queue;
        impl_->device_id = device_id;
        impl_->width = width;
        impl_->height = height;
        impl_->tensor_byte_count =
            static_cast<std::size_t>(elements) * sizeof(float);
        HRESULT hr = command_queue->GetDevice(IID_PPV_ARGS(&impl_->device));
        if (FAILED(hr)) return impl_->fail_hresult("读取 DML D3D12 设备失败", hr);
        const D3D12_COMMAND_LIST_TYPE list_type = command_queue->GetDesc().Type;
        if (list_type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            list_type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            return impl_->fail("DirectML command queue 不是 DIRECT/COMPUTE 类型");
        }
        hr = command_queue->GetTimestampFrequency(&impl_->timestamp_frequency);
        if (FAILED(hr) || impl_->timestamp_frequency == 0) {
            return impl_->fail_hresult("读取 DML queue 时间戳频率失败", hr);
        }

        const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        const auto input_desc = buffer_desc(
            impl_->tensor_byte_count,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = impl_->device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &input_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&impl_->input_buffer));
        if (FAILED(hr)) return impl_->fail_hresult("创建 DirectML 输入缓冲失败", hr);

        Ort::ThrowOnError(dml_api->CreateGPUAllocationFromD3DResource(
            impl_->input_buffer.Get(), &impl_->dml_allocation));
        if (!impl_->dml_allocation) {
            return impl_->fail("ORT 未返回 DirectML allocation wrapper");
        }
        if (!impl_->create_pipeline() ||
            !impl_->create_timing_resources(list_type)) {
            return false;
        }
        impl_->error.clear();
        return true;
    } catch (const std::exception& e) {
        return impl_->fail(std::string("初始化 DirectML GPU 前处理失败: ") + e.what());
    } catch (...) {
        return impl_->fail("初始化 DirectML GPU 前处理失败: 未知异常");
    }
}

bool DmlPreprocessor::prepare(
        void* d3d11_texture,
        void* shared_fence_handle) noexcept {
    if (!impl_ || !impl_->device || !impl_->dml_allocation) return false;
    try {
        if (!impl_->open_shared_fence(shared_fence_handle)) return false;
        if (impl_->find_texture(d3d11_texture)) return true;
        return impl_->create_texture_entry(d3d11_texture);
    } catch (const std::exception& e) {
        return impl_->fail(std::string("预备 D3D11/DirectML 纹理失败: ") + e.what());
    } catch (...) {
        return impl_->fail("预备 D3D11/DirectML 纹理失败: 未知异常");
    }
}

bool DmlPreprocessor::enqueue(
        void* d3d11_texture,
        void* shared_fence_handle,
        std::uint64_t fence_value,
        std::mutex& submission_mutex) noexcept {
    if (!impl_ || fence_value == 0 || impl_->timing_pending ||
        !prepare(d3d11_texture, shared_fence_handle)) {
        return false;
    }
    auto* entry = impl_->find_texture(d3d11_texture);
    if (!entry || !impl_->shared_fence) return false;
    try {
        std::lock_guard<std::mutex> lock(submission_mutex);
        ID3D12CommandList* start_lists[] = {impl_->start_command_list.Get()};
        impl_->command_queue->ExecuteCommandLists(1, start_lists);
        HRESULT hr = impl_->command_queue->Wait(
            impl_->shared_fence.Get(), fence_value);
        if (FAILED(hr)) return impl_->fail_hresult("DML queue 等待 D3D11 fence 失败", hr);
        ID3D12CommandList* process_lists[] = {entry->command_list.Get()};
        impl_->command_queue->ExecuteCommandLists(1, process_lists);
        impl_->timing_pending = true;
        return true;
    } catch (...) {
        return impl_->fail("提交 D3D11/DirectML GPU 前处理失败");
    }
}

bool DmlPreprocessor::complete_timing(double& elapsed_ms) noexcept {
    elapsed_ms = 0.0;
    if (!impl_ || !impl_->timing_pending || !impl_->timestamp_readback ||
        impl_->timestamp_frequency == 0) {
        return false;
    }
    impl_->timing_pending = false;
    std::uint64_t* values = nullptr;
    D3D12_RANGE read_range{0, sizeof(std::uint64_t) * 2};
    const HRESULT hr = impl_->timestamp_readback->Map(
        0, &read_range, reinterpret_cast<void**>(&values));
    if (FAILED(hr) || !values) {
        return impl_->fail_hresult("读取 D3D11/DirectML GPU 时间戳失败", hr);
    }
    const std::uint64_t started = values[0];
    const std::uint64_t finished = values[1];
    D3D12_RANGE written_range{0, 0};
    impl_->timestamp_readback->Unmap(0, &written_range);
    if (finished < started) {
        return impl_->fail("D3D11/DirectML GPU 时间戳顺序非法");
    }
    elapsed_ms = static_cast<double>(finished - started) * 1000.0 /
                 static_cast<double>(impl_->timestamp_frequency);
    return true;
}

void DmlPreprocessor::abandon_pending() noexcept {
    if (!impl_) return;
    impl_->wait_idle();
    impl_->timing_pending = false;
}

void* DmlPreprocessor::dml_allocation() const noexcept {
    return impl_ ? impl_->dml_allocation : nullptr;
}

std::size_t DmlPreprocessor::tensor_bytes() const noexcept {
    return impl_ ? impl_->tensor_byte_count : 0;
}

const std::string& DmlPreprocessor::last_error() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->error : empty;
}

} // namespace detector::detail
