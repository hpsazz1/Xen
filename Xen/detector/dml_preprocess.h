#ifndef DETECTOR_DML_PREPROCESS_H
#define DETECTOR_DML_PREPROCESS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct ID3D12CommandQueue;
struct OrtDmlApi;

namespace detector::detail {

// D3D11 共享 BGRA8 纹理到 DirectML float32 CHW 输入缓冲的固定工作区。
// 所有 D3D12 对象、描述符和命令列表均在启动期创建；热路径只提交已录制命令。
class DmlPreprocessor {
public:
    DmlPreprocessor();
    ~DmlPreprocessor();

    DmlPreprocessor(const DmlPreprocessor&) = delete;
    DmlPreprocessor& operator=(const DmlPreprocessor&) = delete;

    bool init(
        const OrtDmlApi* dml_api,
        ID3D12CommandQueue* command_queue,
        int device_id,
        int width,
        int height) noexcept;

    // 必须在 Capture 业务线程启动前调用。texture 必须是带 shared NT handle
    // 的 BGRA8 D3D11 纹理；shared_fence_handle 必须来自同一 D3D11 设备。
    bool prepare(
        void* d3d11_texture,
        void* shared_fence_handle) noexcept;

    // 提交顺序为时间戳起点 → GPU fence wait → BGRA8→CHW compute。
    // 同一实例不可并发调用；Session::Run 返回后必须调用 complete_timing()。
    bool enqueue(
        void* d3d11_texture,
        void* shared_fence_handle,
        std::uint64_t fence_value,
        std::mutex& submission_mutex) noexcept;
    bool complete_timing(double& elapsed_ms) noexcept;
    // ORT Run 中途失败时等待当前 queue 收敛并清除 pending 状态，避免下一帧
    // 误用尚未完成的时间戳或永久拒绝提交。
    void abandon_pending() noexcept;

    void* dml_allocation() const noexcept;
    std::size_t tensor_bytes() const noexcept;
    const std::string& last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detector::detail

#endif // DETECTOR_DML_PREPROCESS_H
