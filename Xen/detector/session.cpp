#include "detector/session.h"

#include "log/log.h"

#include <algorithm>
#include <filesystem>
#include <exception>
#include <memory>
#include <string>

#ifndef XEN_HAS_CUDA_RUNTIME
#define XEN_HAS_CUDA_RUNTIME 0
#endif
#if XEN_HAS_CUDA_RUNTIME
#include <cuda_runtime_api.h>
#endif

#if defined(_WIN32) && __has_include(<dml_provider_factory.h>)
#include <dml_provider_factory.h>
// DirectML 间接包含的 Windows 头会定义 ERROR 宏，导致后续日志宏中的
// LogLevel::ERROR 被预处理器改写。Provider 头读取完毕后立即清除此污染。
#ifdef ERROR
#undef ERROR
#endif
#define XEN_HAS_DIRECTML_FACTORY 1
#else
#define XEN_HAS_DIRECTML_FACTORY 0
#endif

namespace detector::detail {
namespace {

bool provider_available(const std::vector<std::string>& providers,
                        const char* name) {
    for (const auto& provider : providers) {
        if (provider == name) return true;
    }
    return false;
}

} // namespace

struct Session::Impl {
    // ORT C++ 对象本身是 RAII 包装器。这里使用 unique_ptr 允许 reset() 和失败后
    // 保持空状态，同时彻底移除手写 new/delete 与重复 load 时的泄漏风险。
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::MemoryInfo> memory;
    Ort::SessionOptions opts;
    Ort::RunOptions run_opts;

    std::vector<Ort::AllocatedStringPtr> input_names_allocated;
    std::vector<Ort::AllocatedStringPtr> output_names_allocated;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
    std::vector<int64_t> input_shape;
    ONNXTensorElementDataType input_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::unique_ptr<Ort::MemoryInfo> device_memory;
    std::unique_ptr<Ort::Allocator> device_allocator;
    Ort::Value device_input_value{nullptr};
    std::vector<Ort::Value> device_output_values;
    std::vector<Ort::Value> output_values;
    std::unique_ptr<Ort::IoBinding> io_binding;
    int device_id = 0;
    bool use_cuda_graph = false;
    std::string active_provider = "CPUExecutionProvider";

    void clear_execution_buffers() noexcept {
        // I/O Binding 先释放对设备 OrtValue 的引用；设备 Tensor 必须在其分配器
        // 和 Session 仍存活时销毁，避免重复 load 或失败回滚形成悬空 allocator。
        io_binding.reset();
        output_values.clear();
        device_output_values.clear();
        device_input_value = Ort::Value{nullptr};
        device_allocator.reset();
        device_memory.reset();
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
Session::Session(Session&&) = default;
Session& Session::operator=(Session&&) = default;

bool Session::init_env(const DetectorConfig& cfg) {
    try {
        const auto level = cfg.enable_graph_opt
            ? ORT_LOGGING_LEVEL_WARNING
            : ORT_LOGGING_LEVEL_ERROR;
        impl_->env = std::make_unique<Ort::Env>(level, "detector");
        impl_->env->DisableTelemetryEvents();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("detector", "ORT 环境初始化失败: {}", e.what());
        impl_->env.reset();
        return false;
    } catch (...) {
        LOG_ERROR("detector", "ORT 环境初始化失败: 未知异常");
        impl_->env.reset();
        return false;
    }
}

bool Session::setup_options(const DetectorConfig& cfg) {
    if (!impl_->env || cfg.device_id < 0) return false;

    try {
        auto& options = impl_->opts;
        if (cfg.intra_threads > 0) options.SetIntraOpNumThreads(cfg.intra_threads);
        if (cfg.inter_threads > 0) options.SetInterOpNumThreads(cfg.inter_threads);
        options.SetGraphOptimizationLevel(
            cfg.enable_graph_opt ? ORT_ENABLE_ALL : ORT_DISABLE_ALL);

        const auto providers = Ort::GetAvailableProviders();
        const bool has_cuda = provider_available(
            providers, "CUDAExecutionProvider");
        const bool has_tensorrt = provider_available(
            providers, "TensorrtExecutionProvider");
        const bool has_directml = provider_available(
            providers, "DmlExecutionProvider");

        const auto append_cuda = [&]() {
            OrtCUDAProviderOptionsV2* raw_options = nullptr;
            Ort::ThrowOnError(
                Ort::GetApi().CreateCUDAProviderOptions(&raw_options));
            const auto release = [](OrtCUDAProviderOptionsV2* value) {
                if (value) Ort::GetApi().ReleaseCUDAProviderOptions(value);
            };
            std::unique_ptr<OrtCUDAProviderOptionsV2, decltype(release)>
                cuda_options(raw_options, release);

            const std::string device_id = std::to_string(cfg.device_id);
            const char* keys[] = {"device_id"};
            const char* values[] = {device_id.c_str()};
            Ort::ThrowOnError(Ort::GetApi().UpdateCUDAProviderOptions(
                cuda_options.get(), keys, values, 1));
            options.AppendExecutionProvider_CUDA_V2(*cuda_options);
        };

        if (cfg.backend == BackendType::TENSORRT) {
            if (has_tensorrt) {
                OrtTensorRTProviderOptionsV2* raw_options = nullptr;
                Ort::ThrowOnError(
                    Ort::GetApi().CreateTensorRTProviderOptions(&raw_options));
                const auto release = [](OrtTensorRTProviderOptionsV2* value) {
                    if (value) Ort::GetApi().ReleaseTensorRTProviderOptions(value);
                };
                std::unique_ptr<OrtTensorRTProviderOptionsV2, decltype(release)>
                    tensorrt_options(raw_options, release);

                std::string cache_path;
                if (cfg.enable_trt_engine_cache ||
                    cfg.enable_trt_timing_cache) {
                    const auto requested_path =
                        std::filesystem::u8path(cfg.trt_cache_path);
                    std::error_code error;
                    std::filesystem::create_directories(
                        requested_path, error);
                    if (error ||
                        !std::filesystem::is_directory(requested_path)) {
                        LOG_ERROR("detector", "TensorRT 缓存目录创建失败: {}",
                                  cfg.trt_cache_path);
                        return false;
                    }
                    // Provider V2 接口接收窄字符串。absolute() 同时固定相对路径
                    // 的解析位置，避免运行期间工作目录变化后读写不同缓存目录。
                    cache_path = std::filesystem::absolute(
                        requested_path).string();
                }

                const std::string device_id = std::to_string(cfg.device_id);
                const char* keys[] = {
                    "device_id",
                    "trt_fp16_enable",
                    "trt_engine_cache_enable",
                    "trt_engine_cache_path",
                    "trt_timing_cache_enable",
                    "trt_timing_cache_path",
                    "trt_cuda_graph_enable",
                };
                const char* values[] = {
                    device_id.c_str(),
                    cfg.enable_fp16 ? "1" : "0",
                    cfg.enable_trt_engine_cache ? "1" : "0",
                    cache_path.c_str(),
                    cfg.enable_trt_timing_cache ? "1" : "0",
                    cache_path.c_str(),
                    cfg.enable_trt_cuda_graph ? "1" : "0",
                };
                Ort::ThrowOnError(Ort::GetApi().UpdateTensorRTProviderOptions(
                    tensorrt_options.get(), keys, values,
                    sizeof(keys) / sizeof(keys[0])));
                options.AppendExecutionProvider_TensorRT_V2(*tensorrt_options);
                impl_->active_provider = "TensorrtExecutionProvider";
                impl_->device_id = cfg.device_id;
                impl_->use_cuda_graph = cfg.enable_trt_cuda_graph;
                if (!cache_path.empty()) {
                    LOG_INFO("detector", "TensorRT 缓存目录: {}", cache_path);
                }
                LOG_INFO("detector", "TensorRT CUDA Graph: {}",
                         cfg.enable_trt_cuda_graph ? "启用" : "关闭");
            }

            // TensorRT 官方建议继续注册 CUDA，让 TRT 不支持的节点落到 CUDA，
            // 最后才由 ORT 内置 CPU Provider 兜底。
            if (has_cuda) {
                append_cuda();
                if (!has_tensorrt) {
                    impl_->active_provider = "CUDAExecutionProvider";
                    LOG_WARN("detector", "TensorRT EP 不可用，降级到 CUDA EP");
                }
            } else if (!has_tensorrt) {
                impl_->active_provider = "CPUExecutionProvider";
                LOG_WARN("detector", "TensorRT/CUDA EP 均不可用，降级到 CPU EP");
            }
            return true;
        }

        if (cfg.backend == BackendType::CUDA) {
            if (has_cuda) {
                append_cuda();
                impl_->active_provider = "CUDAExecutionProvider";
            } else {
                impl_->active_provider = "CPUExecutionProvider";
                LOG_WARN("detector", "CUDA EP 不可用，降级到 CPU EP");
            }
            return true;
        }

        if (cfg.backend == BackendType::DIRECTML) {
            // DirectML 不支持 memory pattern 和并行执行；缺少任一设置都会在
            // 创建 Session 时失败或产生不受支持的运行方式。
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
#if XEN_HAS_DIRECTML_FACTORY
            if (has_directml) {
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
                    options, cfg.device_id));
                impl_->active_provider = "DmlExecutionProvider";
            } else {
                LOG_ERROR("detector", "DirectML EP 不可用，拒绝静默降级到 CPU EP");
                return false;
            }
#else
            (void)has_directml;
            LOG_ERROR("detector", "当前 ORT SDK 不含 DirectML 工厂，无法执行 DML 推理");
            return false;
#endif
            return true;
        }

        impl_->active_provider = "CPUExecutionProvider";
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("detector", "ORT EP 配置失败: {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("detector", "ORT EP 配置失败: 未知异常");
        return false;
    }
}

bool Session::load(const std::string& path) {
    if (!impl_->env || path.empty()) return false;

    try {
        impl_->clear_execution_buffers();
        impl_->session.reset();
        const std::filesystem::path model_path =
            std::filesystem::u8path(path);
        if (!std::filesystem::is_regular_file(model_path)) {
            LOG_ERROR("detector", "模型文件不存在: {}", path);
            return false;
        }

        impl_->session = std::make_unique<Ort::Session>(
            *impl_->env, model_path.c_str(), impl_->opts);

        const size_t input_count = impl_->session->GetInputCount();
        const size_t output_count = impl_->session->GetOutputCount();
        if (input_count != 1 || output_count == 0) {
            LOG_ERROR("detector", "仅支持单输入且至少单输出模型，实际 inputs={}, outputs={}",
                      input_count, output_count);
            impl_->clear_execution_buffers();
            impl_->session.reset();
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        impl_->input_names_allocated.clear();
        impl_->input_names.clear();
        impl_->output_names_allocated.clear();
        impl_->output_names.clear();
        impl_->input_names_allocated.reserve(input_count);
        impl_->input_names.reserve(input_count);
        impl_->output_names_allocated.reserve(output_count);
        impl_->output_names.reserve(output_count);

        for (size_t i = 0; i < input_count; ++i) {
            impl_->input_names_allocated.push_back(
                impl_->session->GetInputNameAllocated(i, allocator));
            impl_->input_names.push_back(
                impl_->input_names_allocated.back().get());
        }
        for (size_t i = 0; i < output_count; ++i) {
            impl_->output_names_allocated.push_back(
                impl_->session->GetOutputNameAllocated(i, allocator));
            impl_->output_names.push_back(
                impl_->output_names_allocated.back().get());
        }

        const auto tensor_info = impl_->session->GetInputTypeInfo(0)
            .GetTensorTypeAndShapeInfo();
        impl_->input_shape = tensor_info.GetShape();
        impl_->input_type = tensor_info.GetElementType();

        impl_->output_values.clear();
        impl_->device_output_values.clear();
        if (impl_->use_cuda_graph) {
            // CUDA Graph 只重放设备侧工作。输入必须在每帧捕获外显式复制到稳定
            // 的 CUDA 缓冲区，输出也必须从固定设备地址复制回 CPU；只固定 CPU
            // OrtValue 会导致重放首次输入，产生“耗时很低但结果恒定”的错误。
            const bool static_input = !impl_->input_shape.empty() &&
                std::all_of(impl_->input_shape.begin(), impl_->input_shape.end(),
                            [](int64_t dimension) { return dimension > 0; });
            if (!static_input) {
                LOG_ERROR("detector", "TensorRT CUDA Graph 要求静态输入形状");
                impl_->clear_execution_buffers();
                impl_->session.reset();
                return false;
            }

#if XEN_HAS_CUDA_RUNTIME
            const cudaError_t set_device_result =
                cudaSetDevice(impl_->device_id);
            if (set_device_result != cudaSuccess) {
                LOG_ERROR("detector", "CUDA 设备选择失败: {}",
                          cudaGetErrorString(set_device_result));
                impl_->clear_execution_buffers();
                impl_->session.reset();
                return false;
            }
#else
            LOG_ERROR("detector", "当前构建未链接 CUDA Runtime，无法启用 CUDA Graph");
            impl_->clear_execution_buffers();
            impl_->session.reset();
            return false;
#endif

            impl_->device_memory = std::make_unique<Ort::MemoryInfo>(
                "Cuda", OrtDeviceAllocator, impl_->device_id,
                OrtMemTypeDefault);
            impl_->device_allocator = std::make_unique<Ort::Allocator>(
                *impl_->session, *impl_->device_memory);
            impl_->device_input_value = Ort::Value::CreateTensor(
                *impl_->device_allocator,
                impl_->input_shape.data(), impl_->input_shape.size(),
                impl_->input_type);

            impl_->output_values.reserve(output_count);
            impl_->device_output_values.reserve(output_count);
            for (size_t i = 0; i < output_count; ++i) {
                const auto output_info = impl_->session->GetOutputTypeInfo(i)
                    .GetTensorTypeAndShapeInfo();
                const auto output_shape = output_info.GetShape();
                const bool static_shape = !output_shape.empty() &&
                    std::all_of(output_shape.begin(), output_shape.end(),
                                [](int64_t dimension) { return dimension > 0; });
                if (!static_shape) {
                    LOG_ERROR("detector",
                              "TensorRT CUDA Graph 要求静态输出形状，输出索引={}", i);
                    impl_->clear_execution_buffers();
                    impl_->session.reset();
                    return false;
                }
                impl_->output_values.push_back(Ort::Value::CreateTensor(
                    allocator, output_shape.data(), output_shape.size(),
                    output_info.GetElementType()));
                impl_->device_output_values.push_back(Ort::Value::CreateTensor(
                    *impl_->device_allocator,
                    output_shape.data(), output_shape.size(),
                    output_info.GetElementType()));
            }

            impl_->io_binding = std::make_unique<Ort::IoBinding>(*impl_->session);
            impl_->io_binding->BindInput(
                impl_->input_names[0], impl_->device_input_value);
            for (size_t i = 0; i < output_count; ++i) {
                impl_->io_binding->BindOutput(
                    impl_->output_names[i], impl_->device_output_values[i]);
            }
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("detector", "模型加载失败: {}", e.what());
        impl_->clear_execution_buffers();
        impl_->session.reset();
        return false;
    } catch (...) {
        LOG_ERROR("detector", "模型加载失败: 未知异常");
        impl_->clear_execution_buffers();
        impl_->session.reset();
        return false;
    }
}

const std::vector<Ort::Value>* Session::run(Ort::Value& input) {
    if (!impl_->session) return nullptr;
    try {
        if (impl_->use_cuda_graph) {
#if XEN_HAS_CUDA_RUNTIME
            const cudaError_t set_device_result =
                cudaSetDevice(impl_->device_id);
            if (set_device_result != cudaSuccess) {
                LOG_DEBUG("detector", "CUDA 设备选择失败: {}",
                          cudaGetErrorString(set_device_result));
                return nullptr;
            }

            const cudaError_t input_copy_result = cudaMemcpy(
                impl_->device_input_value.GetTensorMutableRawData(),
                input.GetTensorRawData(), input.GetTensorSizeInBytes(),
                cudaMemcpyHostToDevice);
            if (input_copy_result != cudaSuccess) {
                LOG_DEBUG("detector", "CUDA 输入复制失败: {}",
                          cudaGetErrorString(input_copy_result));
                return nullptr;
            }

            impl_->session->Run(impl_->run_opts, *impl_->io_binding);

            for (size_t i = 0; i < impl_->output_values.size(); ++i) {
                const cudaError_t output_copy_result = cudaMemcpy(
                    impl_->output_values[i].GetTensorMutableRawData(),
                    impl_->device_output_values[i].GetTensorRawData(),
                    impl_->output_values[i].GetTensorSizeInBytes(),
                    cudaMemcpyDeviceToHost);
                if (output_copy_result != cudaSuccess) {
                    LOG_DEBUG("detector", "CUDA 输出复制失败: {}",
                              cudaGetErrorString(output_copy_result));
                    return nullptr;
                }
            }
#else
            return nullptr;
#endif
        } else {
            impl_->output_values = impl_->session->Run(
                impl_->run_opts,
                impl_->input_names.data(), &input, 1,
                impl_->output_names.data(), impl_->output_names.size());
        }
        return &impl_->output_values;
    } catch (const std::exception& e) {
        // run() 只负责把异常转成空结果；热路径不打印 INFO 及以上日志。
        LOG_DEBUG("detector", "ORT 推理失败: {}", e.what());
        return nullptr;
    } catch (...) {
        LOG_DEBUG("detector", "ORT 推理失败: 未知异常");
        return nullptr;
    }
}

size_t Session::num_inputs() const {
    return impl_->input_names.size();
}

std::vector<int64_t> Session::input_shape() const {
    return impl_->input_shape;
}

ONNXTensorElementDataType Session::input_type() const {
    return impl_->input_type;
}

size_t Session::num_outputs() const {
    return impl_->output_names.size();
}

const std::vector<const char*>& Session::output_names() const {
    return impl_->output_names;
}

Ort::MemoryInfo* Session::memory_info() {
    if (!impl_->memory) {
        try {
            impl_->memory = std::make_unique<Ort::MemoryInfo>(
                Ort::MemoryInfo::CreateCpu(
                    OrtArenaAllocator, OrtMemTypeDefault));
        } catch (...) {
            return nullptr;
        }
    }
    return impl_->memory.get();
}

std::string Session::metadata_value(const char* key) const {
    if (!impl_->session || !key || *key == '\0') return {};
    try {
        Ort::AllocatorWithDefaultOptions allocator;
        auto metadata = impl_->session->GetModelMetadata();
        auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator);
        return value ? std::string(value.get()) : std::string{};
    } catch (...) {
        return {};
    }
}

const std::string& Session::active_provider() const noexcept {
    return impl_->active_provider;
}

} // namespace detector::detail
