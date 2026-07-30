#include "detector/session.h"
#include <cstdio>
#include <memory>
#include <string>

namespace detector::detail {

struct Session::Impl {
    Ort::Env*             env      = nullptr;
    Ort::Session*         session  = nullptr;
    Ort::MemoryInfo*      memory   = nullptr;
    Ort::SessionOptions   opts;
    Ort::RunOptions       run_opts;

    std::vector<Ort::AllocatedStringPtr> in_names_alloc;
    std::vector<Ort::AllocatedStringPtr> out_names_alloc;
    std::vector<const char*>  in_names;
    std::vector<const char*>  out_names;
    std::vector<int64_t>      in_shape;

    ~Impl() {
        delete session;
        delete memory;
        delete env;
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
Session::Session(Session&&) = default;
Session& Session::operator=(Session&&) = default;

bool Session::init_env(const DetectorConfig& cfg) {
    try {
        auto level = cfg.enable_graph_opt
            ? ORT_LOGGING_LEVEL_WARNING : ORT_LOGGING_LEVEL_ERROR;
        impl_->env = new Ort::Env(level, "detector");
        impl_->env->DisableTelemetryEvents();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Detector] Env init failed: %s\n", e.what());
        return false;
    }
}

void Session::setup_options(const DetectorConfig& cfg) {
    auto& opts = impl_->opts;

    if (cfg.intra_threads > 0) opts.SetIntraOpNumThreads(cfg.intra_threads);
    if (cfg.inter_threads > 0) opts.SetInterOpNumThreads(cfg.inter_threads);

    opts.SetGraphOptimizationLevel(
        cfg.enable_graph_opt
            ? ORT_ENABLE_ALL
            : ORT_DISABLE_ALL);

    // ── CUDA EP ──
    if (cfg.backend == BackendType::CUDA) {
        auto providers = Ort::GetAvailableProviders();
        bool has_cuda = false;
        for (auto& p : providers)
            if (p.find("CUDA") != std::string::npos) { has_cuda = true; break; }

        if (has_cuda) {
            OrtCUDAProviderOptionsV2* opt = nullptr;
            Ort::GetApi().CreateCUDAProviderOptions(&opt);
            auto guard = std::unique_ptr<OrtCUDAProviderOptionsV2,
                decltype(&Ort::GetApi().ReleaseCUDAProviderOptions)>(
                    opt, [](auto* p) { Ort::GetApi().ReleaseCUDAProviderOptions(p); });
            Ort::GetApi().UpdateCUDAProviderOptions(
                opt, "device_id", std::to_string(cfg.device_id).c_str());
            opts.AppendExecutionProvider_CUDA_V2(*opt);
        } else {
            std::fprintf(stderr, "[Detector] CUDA not available, fallback to CPU\n");
        }
    }

    // ── TensorRT EP ──
    if (cfg.backend == BackendType::TENSORRT) {
        OrtTensorRTProviderOptionsV2* opt = nullptr;
        Ort::GetApi().CreateTensorRTProviderOptions(&opt);
        auto guard = std::unique_ptr<OrtTensorRTProviderOptionsV2,
            decltype(&Ort::GetApi().ReleaseTensorRTProviderOptions)>(
                opt, [](auto* p) { Ort::GetApi().ReleaseTensorRTProviderOptions(p); });
        Ort::GetApi().UpdateTensorRTProviderOptions(
            opt, "device_id", std::to_string(cfg.device_id).c_str());
        if (cfg.enable_fp16)
            Ort::GetApi().UpdateTensorRTProviderOptions(opt, "trt_fp16_enable", "1");
        opts.AppendExecutionProvider_TensorRT_V2(*opt);
    }

    // ── DirectML EP ──
    if (cfg.backend == BackendType::DIRECTML) {
        OrtDmlProviderOptions dml{};
        dml.device_id = cfg.device_id;
        opts.AppendExecutionProvider_DML(dml);
    }
}

bool Session::load(const std::string& path) {
    try {
        impl_->session = new Ort::Session(*impl_->env, path.c_str(), impl_->opts);

        auto alloc = Ort::AllocatorWithDefaultOptions();
        size_t nin  = impl_->session->GetInputCount();
        size_t nout = impl_->session->GetOutputCount();

        impl_->in_names_alloc.resize(nin);
        impl_->in_names.resize(nin);
        impl_->out_names_alloc.resize(nout);
        impl_->out_names.resize(nout);

        for (size_t i = 0; i < nin; ++i) {
            impl_->in_names_alloc[i] = impl_->session->GetInputNameAllocated(i, alloc);
            impl_->in_names[i] = impl_->in_names_alloc[i].get();
        }
        for (size_t i = 0; i < nout; ++i) {
            impl_->out_names_alloc[i] = impl_->session->GetOutputNameAllocated(i, alloc);
            impl_->out_names[i] = impl_->out_names_alloc[i].get();
        }

        auto type_info = impl_->session->GetInputTypeInfo(0);
        impl_->in_shape = type_info.GetTensorTypeAndShapeInfo().GetShape();
        if (impl_->in_shape[0] == -1) impl_->in_shape[0] = 1; // dynamic batch

        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Detector] Load model failed: %s\n", e.what());
        return false;
    }
}

std::vector<Ort::Value> Session::run(Ort::Value& input) {
    try {
        return impl_->session->Run(
            impl_->run_opts,
            impl_->in_names.data(), &input, 1,
            impl_->out_names.data(), impl_->out_names.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Detector] Inference failed: %s\n", e.what());
        return {};
    }
}

std::vector<int64_t> Session::input_shape() const { return impl_->in_shape; }
size_t Session::num_outputs() const { return impl_->out_names.size(); }
const std::vector<const char*>& Session::output_names() const { return impl_->out_names; }

Ort::MemoryInfo* Session::memory_info() {
    if (!impl_->memory)
        impl_->memory = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
    return impl_->memory;
}

} // namespace detector::detail
