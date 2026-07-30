#include "detector/detector.h"

#include "detector/postprocess.h"
#include "detector/preprocess.h"
#include "detector/session.h"
#include "log/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace {

bool valid_config(const DetectorConfig& config) noexcept {
    const bool dimensions_are_pair =
        (config.input_width == 0) == (config.input_height == 0);
    const bool trt_cache_path_valid =
        config.backend != BackendType::TENSORRT ||
        (!config.enable_trt_engine_cache &&
         !config.enable_trt_timing_cache) ||
        !config.trt_cache_path.empty();
    return !config.model_path.empty() && config.device_id >= 0 &&
           dimensions_are_pair && trt_cache_path_valid &&
           config.input_width >= 0 &&
           config.input_height >= 0 && config.intra_threads >= 0 &&
           config.inter_threads >= 0 &&
           std::isfinite(config.conf_threshold) &&
           config.conf_threshold >= 0.0f && config.conf_threshold <= 1.0f &&
           std::isfinite(config.nms_threshold) &&
           config.nms_threshold >= 0.0f && config.nms_threshold <= 1.0f &&
           config.top_k > 0;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(
                ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        });
    return value;
}

std::optional<bool> parse_bool_metadata(const std::string& value) {
    const std::string normalized = lowercase_ascii(value);
    if (normalized == "true" || normalized == "1") return true;
    if (normalized == "false" || normalized == "0") return false;
    return std::nullopt;
}

const char* output_format_name(OutputFormat format) noexcept {
    switch (format) {
        case OutputFormat::AUTO: return "AUTO";
        case OutputFormat::CHANNEL_FIRST: return "CHANNEL_FIRST";
        case OutputFormat::ANCHOR_FIRST_OBJECTNESS:
            return "ANCHOR_FIRST_OBJECTNESS";
        case OutputFormat::END_TO_END: return "END_TO_END";
    }
    return "UNKNOWN";
}

} // namespace

struct Detector::Impl {
    detector::detail::Session session;
    bool loaded = false;
    int64_t channels = 3;
    int64_t height = 0;
    int64_t width = 0;
    std::string active_provider = "CPUExecutionProvider";
    std::optional<bool> metadata_end_to_end;
    std::optional<OutputFormat> resolved_output_format;

    bool load(const DetectorConfig& config) {
        if (!valid_config(config)) {
            LOG_ERROR("detector", "DetectorConfig 参数非法");
            return false;
        }

        LOG_INFO("detector", "加载模型: {}, 请求后端={}",
                 config.model_path, BackendName(config.backend));

        if (!session.init_env(config) ||
            !session.setup_options(config) ||
            !session.load(config.model_path)) {
            return false;
        }

        if (session.input_type() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            LOG_ERROR("detector", "仅支持 float32 输入张量，实际类型={}",
                      static_cast<int>(session.input_type()));
            return false;
        }

        const auto shape = session.input_shape();
        if (shape.size() != 4 ||
            (shape[0] != 1 && shape[0] != -1) ||
            (shape[1] != 3 && shape[1] != -1)) {
            LOG_ERROR("detector", "仅支持单 batch NCHW 三通道输入，rank={}",
                      shape.size());
            return false;
        }

        channels = shape[1] > 0 ? shape[1] : 3;
        height = shape[2] > 0 ? shape[2] : config.input_height;
        width = shape[3] > 0 ? shape[3] : config.input_width;
        if (height <= 0 || width <= 0) {
            LOG_ERROR("detector", "模型高宽为动态维度时必须显式设置 input_width/input_height");
            return false;
        }

        // 静态模型不允许配置值与模型形状冲突，避免调用方误以为 Detector
        // 会在不改变 ONNX 图的情况下强行覆盖固定输入尺寸。
        if ((config.input_height > 0 && shape[2] > 0 &&
             config.input_height != shape[2]) ||
            (config.input_width > 0 && shape[3] > 0 &&
             config.input_width != shape[3])) {
            LOG_ERROR("detector", "配置输入尺寸与 ONNX 静态输入尺寸不一致");
            return false;
        }

        const std::string task = lowercase_ascii(session.metadata_value("task"));
        if (!task.empty() && task != "detect") {
            LOG_ERROR("detector", "当前模块仅支持 detect 任务，模型 task={}", task);
            return false;
        }
        metadata_end_to_end = parse_bool_metadata(
            session.metadata_value("end2end"));

        // Detect 标准导出应只有一个输出。多输出通常表示分割原型、姿态附加
        // 张量或未合并 head；宁可拒绝，也不能静默套用错误解码器。
        if (session.num_outputs() != 1) {
            LOG_ERROR("detector", "Detect 仅支持单输出模型，实际 outputs={}",
                      session.num_outputs());
            return false;
        }

        active_provider = session.active_provider();
        loaded = true;
        LOG_INFO("detector", "模型加载完成: {}x{}, provider={}",
                 width, height, active_provider);
        return true;
    }

    std::vector<Detection> run(const cv::Mat& bgr_image,
                               const DetectorConfig& config,
                               InferenceProfile& profile) {
        using clock = std::chrono::steady_clock;
        using milliseconds = std::chrono::duration<double, std::milli>;

        const auto start = clock::now();
        cv::Mat blob;
        detector::detail::LetterBoxInfo letterbox_info;
        if (!detector::detail::letterbox(
                bgr_image, blob,
                static_cast<int>(width), static_cast<int>(height),
                letterbox_info)) {
            return {};
        }
        const auto preprocessed = clock::now();

        const std::vector<int64_t> input_shape = {
            1, channels, height, width};
        const size_t element_count = static_cast<size_t>(channels) *
            static_cast<size_t>(height) * static_cast<size_t>(width);
        Ort::MemoryInfo* memory = session.memory_info();
        if (!memory) return {};

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            *memory, blob.ptr<float>(), element_count,
            input_shape.data(), input_shape.size());
        auto outputs = session.run(input_tensor);
        const auto inferred = clock::now();
        if (outputs.size() != 1 || !outputs[0].IsTensor()) return {};

        const auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            return {};
        }
        const auto output_shape = output_info.GetShape();

        OutputFormat requested = config.output_format;
        if (requested == OutputFormat::AUTO && metadata_end_to_end.has_value()) {
            if (*metadata_end_to_end) {
                requested = OutputFormat::END_TO_END;
            } else if (output_shape.size() == 3 && output_shape[2] == 6 &&
                       output_shape[1] > 1024) {
                requested = OutputFormat::ANCHOR_FIRST_OBJECTNESS;
            }
        }

        OutputFormat resolved = OutputFormat::AUTO;
        if (!detector::detail::resolve_output_format(
                output_shape, requested, resolved)) {
            return {};
        }
        if (resolved_output_format.has_value() &&
            *resolved_output_format != resolved) {
            // 同一会话输出契约发生变化意味着模型或动态输出不符合约定。
            return {};
        }
        resolved_output_format = resolved;

        std::vector<Detection> detections;
        if (!detector::detail::decode_output(
                outputs[0].GetTensorData<float>(), output_shape, resolved,
                config.conf_threshold, detections)) {
            return {};
        }

        detector::detail::scale_detections(detections, letterbox_info);
        if (resolved != OutputFormat::END_TO_END) {
            detector::detail::nms(
                detections, config.nms_threshold, config.top_k);
        } else if (static_cast<int>(detections.size()) > config.top_k) {
            std::partial_sort(
                detections.begin(), detections.begin() + config.top_k,
                detections.end(),
                [](const Detection& a, const Detection& b) {
                    return a.confidence > b.confidence;
                });
            detections.resize(static_cast<size_t>(config.top_k));
        }
        const auto finished = clock::now();

        profile.preprocess_ms =
            std::chrono::duration_cast<milliseconds>(preprocessed - start).count();
        profile.inference_ms =
            std::chrono::duration_cast<milliseconds>(inferred - preprocessed).count();
        profile.postprocess_ms =
            std::chrono::duration_cast<milliseconds>(finished - inferred).count();
        profile.total_ms =
            std::chrono::duration_cast<milliseconds>(finished - start).count();

        LOG_TRACE("detector",
                  "format={}, pre={:.2f}ms infer={:.2f}ms post={:.2f}ms total={:.2f}ms det={}",
                  output_format_name(resolved), profile.preprocess_ms,
                  profile.inference_ms, profile.postprocess_ms,
                  profile.total_ms, detections.size());
        return detections;
    }
};

const char* BackendName(BackendType backend) noexcept {
    switch (backend) {
        case BackendType::CUDA: return "CUDAExecutionProvider";
        case BackendType::TENSORRT: return "TensorrtExecutionProvider";
        case BackendType::DIRECTML: return "DmlExecutionProvider";
        case BackendType::CPU: return "CPUExecutionProvider";
    }
    return "CPUExecutionProvider";
}

Detector::Detector(const DetectorConfig& config)
    : impl_(std::make_unique<Impl>()), config_(config) {}

Detector::~Detector() = default;

bool Detector::load() {
    try {
        auto candidate = std::make_unique<Impl>();
        if (!candidate->load(config_)) return false;
        impl_ = std::move(candidate);
        profile_ = {};
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("detector", "Detector::load() 失败: {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("detector", "Detector::load() 失败: 未知异常");
        return false;
    }
}

bool Detector::loaded() const noexcept {
    return impl_ && impl_->loaded;
}

void Detector::reset() {
    impl_ = std::make_unique<Impl>();
    profile_ = {};
}

std::vector<Detection> Detector::detect(const cv::Mat& bgr_image) {
    if (!loaded() || bgr_image.empty() || bgr_image.type() != CV_8UC3) {
        return {};
    }

    try {
        InferenceProfile current_profile;
        auto detections = impl_->run(bgr_image, config_, current_profile);
        profile_ = current_profile;
        return detections;
    } catch (const std::exception& e) {
        // detect() 是推理热路径，只允许编译期可移除的 DEBUG/TRACE。
        LOG_DEBUG("detector", "Detector::detect() 失败: {}", e.what());
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::detect() 失败: 未知异常");
        return {};
    }
}

std::vector<std::vector<Detection>> Detector::detect_batch(
        const std::vector<cv::Mat>& bgr_images) {
    std::vector<std::vector<Detection>> results;
    results.reserve(bgr_images.size());
    for (const auto& image : bgr_images) {
        results.push_back(detect(image));
    }
    return results;
}

std::unique_ptr<Detector> Detector::clone() const {
    auto clone = std::make_unique<Detector>(config_);
    return clone->load() ? std::move(clone) : nullptr;
}

const InferenceProfile& Detector::profile() const noexcept {
    return profile_;
}

std::string Detector::backend_name() const {
    return loaded() ? impl_->active_provider : BackendName(config_.backend);
}

int Detector::input_width() const noexcept {
    return impl_ ? static_cast<int>(impl_->width) : 0;
}

int Detector::input_height() const noexcept {
    return impl_ ? static_cast<int>(impl_->height) : 0;
}

std::vector<std::string> Detector::available_providers() {
    try {
        return Ort::GetAvailableProviders();
    } catch (...) {
        return {};
    }
}
