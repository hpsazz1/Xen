#include "detector/detector.h"

#include "detector/postprocess.h"
#include "detector/preprocess.h"
#include "detector/session.h"
#include "log/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {

enum class ModelTask {
    DETECT,
    SEGMENT,
    POSE,
    OBB,
};

enum class RequestedOutput {
    BOXES,
    SEGMENTATION,
    POSE,
    OBB,
};

bool valid_config(const DetectorConfig& config) noexcept {
    const bool dimensions_are_pair =
        (config.input_width == 0) == (config.input_height == 0);
    const bool trt_cache_path_valid =
        config.backend != BackendType::TENSORRT ||
        (!config.enable_trt_engine_cache &&
         !config.enable_trt_timing_cache) ||
        !config.trt_cache_path.empty();
    const bool profiling_path_valid =
        !config.enable_ort_profiling || !config.ort_profile_prefix.empty();
    return !config.model_path.empty() && config.device_id >= 0 &&
           dimensions_are_pair && trt_cache_path_valid &&
           profiling_path_valid &&
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

bool parse_positive_int_pair(const std::string& value,
                             int64_t& first,
                             int64_t& second) noexcept {
    first = 0;
    second = 0;
    size_t index = 0;
    const auto skip_spaces = [&]() {
        while (index < value.size() &&
               (value[index] == ' ' || value[index] == '\t' ||
                value[index] == '\r' || value[index] == '\n')) {
            ++index;
        }
    };
    const auto parse_positive = [&](int64_t& output) {
        output = 0;
        bool has_digit = false;
        while (index < value.size() && value[index] >= '0' &&
               value[index] <= '9') {
            const int digit = value[index] - '0';
            if (output >
                (std::numeric_limits<int64_t>::max() - digit) / 10) {
                return false;
            }
            output = output * 10 + digit;
            has_digit = true;
            ++index;
        }
        return has_digit && output > 0;
    };

    skip_spaces();
    if (index >= value.size() ||
        (value[index] != '[' && value[index] != '(')) {
        return false;
    }
    const char closing = value[index++] == '[' ? ']' : ')';
    skip_spaces();
    if (!parse_positive(first)) return false;
    skip_spaces();
    if (index >= value.size() || value[index++] != ',') return false;
    skip_spaces();
    if (!parse_positive(second)) return false;
    skip_spaces();
    if (index >= value.size() || value[index++] != closing) return false;
    skip_spaces();
    return index == value.size();
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

OutputFormat requested_output_format(
        const DetectorConfig& config,
        const std::optional<bool>& metadata_end_to_end,
        const std::vector<int64_t>& output_shape) noexcept {
    if (config.output_format != OutputFormat::AUTO) {
        return config.output_format;
    }
    if (!metadata_end_to_end.has_value()) return OutputFormat::AUTO;
    if (*metadata_end_to_end) return OutputFormat::END_TO_END;

    // 单类别 raw head 与端到端输出均为六列。metadata 明确为 false 时
    // 必须尊重模型声明，不能再根据候选数量猜测。
    if (output_shape.size() == 3 && output_shape[2] == 6) {
        return OutputFormat::ANCHOR_FIRST_OBJECTNESS;
    }
    return OutputFormat::AUTO;
}

bool valid_declared_prediction_shape(
        const std::vector<int64_t>& shape) noexcept {
    if (shape.size() != 3 || (shape[0] != 1 && shape[0] != -1)) {
        return false;
    }
    return std::all_of(shape.begin() + 1, shape.end(),
        [](int64_t dimension) {
            return dimension == -1 || dimension > 0;
        });
}

bool valid_declared_prototype_shape(
        const std::vector<int64_t>& shape) noexcept {
    if (shape.size() != 4 || (shape[0] != 1 && shape[0] != -1)) {
        return false;
    }
    return std::all_of(shape.begin() + 1, shape.end(),
        [](int64_t dimension) {
            return dimension == -1 || dimension > 0;
        });
}

bool static_shape(const std::vector<int64_t>& shape) noexcept {
    return !shape.empty() && std::all_of(
        shape.begin(), shape.end(),
        [](int64_t dimension) { return dimension > 0; });
}

std::uint64_t fnv1a_64_update(std::uint64_t hash,
                              const void* data,
                              size_t byte_count) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < byte_count; ++index) {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kPrime;
    }
    return hash;
}

std::uint64_t output_fingerprint(
        const std::vector<Ort::Value>& outputs) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    std::uint64_t hash = kOffsetBasis;
    for (const Ort::Value& output : outputs) {
        const size_t bytes = output.GetTensorSizeInBytes();
        hash = fnv1a_64_update(hash, &bytes, sizeof(bytes));
        hash = fnv1a_64_update(hash, output.GetTensorRawData(), bytes);
    }
    return hash;
}

struct TaskRunResult {
    // detect 与 segment 共用 segmentation.detections 作为轻量框载体；
    // pose 独立保存关键点布局，避免公有结果类型彼此耦合。
    SegmentationResult segmentation;
    PoseResult pose;
    ObbResult obb;
};

} // namespace

struct Detector::Impl {
    detector::detail::Session session;
    bool loaded = false;
    int64_t channels = 3;
    int64_t height = 0;
    int64_t width = 0;
    std::string active_provider = "CPUExecutionProvider";
    ModelTask model_task = ModelTask::DETECT;
    size_t prediction_output_index = 0;
    size_t prototype_output_index = 0;
    std::optional<bool> metadata_end_to_end;
    std::optional<OutputFormat> resolved_output_format;
    std::optional<detector::detail::SegmentationContract>
        resolved_segmentation_contract;
    std::optional<detector::detail::PoseContract>
        resolved_pose_contract;
    std::optional<detector::detail::ObbContract>
        resolved_obb_contract;
    int64_t pose_keypoint_count = 0;
    int64_t pose_keypoint_dimensions = 0;
    std::array<int64_t, 4> input_shape{1, 3, 0, 0};
    size_t input_element_count = 0;
    cv::Mat input_blob;
    cv::Mat prepared_bgr;
    cv::Mat resize_buffer;
    std::vector<Detection> candidate_detections;
    std::vector<detector::detail::SegmentationCandidate>
        segmentation_candidates;
    std::vector<detector::detail::SegmentationCandidate>
        selected_segmentations;
    std::vector<detector::detail::PoseCandidate> pose_candidates;
    std::vector<detector::detail::PoseCandidate> selected_poses;
    std::vector<detector::detail::ObbCandidate> obb_candidates;
    std::vector<detector::detail::ObbCandidate> selected_obbs;
    std::vector<float> mask_logits;
    std::vector<std::uint8_t> mask_input;
    std::vector<unsigned char> nms_suppressed;
    mutable std::mutex profile_mutex;
    InferenceProfile last_profile;

    void set_profile(const InferenceProfile& profile) noexcept {
        try {
            std::lock_guard<std::mutex> lock(profile_mutex);
            last_profile = profile;
        } catch (...) {
            // profile 是观测信息；锁异常不得终止推理进程。
        }
    }

    InferenceProfile profile() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(profile_mutex);
            return last_profile;
        } catch (...) {
            return {};
        }
    }

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

        const auto max_int = static_cast<int64_t>(
            std::numeric_limits<int>::max());
        if (height > max_int || width > max_int ||
            height > max_int / width || height * width > max_int / 3) {
            LOG_ERROR("detector", "模型输入尺寸过大: {}x{}", width, height);
            return false;
        }

        input_shape = {1, channels, height, width};
        input_element_count = static_cast<size_t>(channels) *
            static_cast<size_t>(height) * static_cast<size_t>(width);

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
        if (!task.empty() && task != "detect" && task != "segment" &&
            task != "pose" && task != "obb") {
            LOG_ERROR("detector", "不支持的模型任务: task={}", task);
            return false;
        }
        if (task == "segment" ||
            (task.empty() && session.num_outputs() == 2)) {
            model_task = ModelTask::SEGMENT;
        } else if (task == "pose") {
            model_task = ModelTask::POSE;
        } else if (task == "obb") {
            model_task = ModelTask::OBB;
        } else {
            model_task = ModelTask::DETECT;
        }
        metadata_end_to_end = parse_bool_metadata(
            session.metadata_value("end2end"));

        if (model_task == ModelTask::DETECT) {
            // Detect 标准导出应只有一个输出。姿态、OBB 或未识别的多输出 head
            // 继续严格拒绝，不能静默套用检测解码器。
            if (session.num_outputs() != 1) {
                LOG_ERROR("detector", "Detect 仅支持单输出模型，实际 outputs={}",
                          session.num_outputs());
                return false;
            }
            if (session.output_type(0) !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                LOG_ERROR("detector", "仅支持 float32 输出张量，实际类型={}",
                          static_cast<int>(session.output_type(0)));
                return false;
            }
            const auto declared_output_shape = session.output_shape(0);
            if (!valid_declared_prediction_shape(declared_output_shape)) {
                LOG_ERROR("detector", "模型输出必须是单 batch 三维张量，rank={}",
                          declared_output_shape.size());
                return false;
            }
            if (static_shape(declared_output_shape)) {
                const OutputFormat requested = requested_output_format(
                    config, metadata_end_to_end, declared_output_shape);
                OutputFormat resolved = OutputFormat::AUTO;
                if (!detector::detail::resolve_output_format(
                        declared_output_shape, requested, resolved)) {
                    LOG_ERROR(
                        "detector",
                        "无法确定模型输出契约，请显式设置 output_format");
                    return false;
                }
                resolved_output_format = resolved;
            }
        } else if (model_task == ModelTask::SEGMENT) {
            if (session.num_outputs() != 2) {
                LOG_ERROR(
                    "detector",
                    "Segment 仅支持检测/系数与原型双输出，实际 outputs={}",
                    session.num_outputs());
                return false;
            }
            if (metadata_end_to_end.value_or(false) ||
                (config.output_format != OutputFormat::AUTO &&
                 config.output_format != OutputFormat::CHANNEL_FIRST)) {
                LOG_ERROR(
                    "detector",
                    "Segment 当前仅支持非 end-to-end 的 channel-first raw head");
                return false;
            }

            std::optional<size_t> prediction_index;
            std::optional<size_t> prototype_index;
            for (size_t index = 0; index < session.num_outputs(); ++index) {
                if (session.output_type(index) !=
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    LOG_ERROR(
                        "detector", "Segment 输出必须全部为 float32，索引={}",
                        index);
                    return false;
                }
                const auto shape = session.output_shape(index);
                if (shape.size() == 3 && !prediction_index.has_value()) {
                    prediction_index = index;
                } else if (shape.size() == 4 &&
                           !prototype_index.has_value()) {
                    prototype_index = index;
                } else {
                    LOG_ERROR(
                        "detector",
                        "Segment 输出必须恰好包含一个三维 prediction 和一个四维 prototype");
                    return false;
                }
            }
            if (!prediction_index.has_value() ||
                !prototype_index.has_value()) {
                LOG_ERROR("detector", "Segment 双输出 rank 契约不完整");
                return false;
            }
            prediction_output_index = *prediction_index;
            prototype_output_index = *prototype_index;
            const auto prediction_shape =
                session.output_shape(prediction_output_index);
            const auto prototype_shape =
                session.output_shape(prototype_output_index);
            if (!valid_declared_prediction_shape(prediction_shape) ||
                !valid_declared_prototype_shape(prototype_shape)) {
                LOG_ERROR("detector", "Segment 声明输出 shape 非法");
                return false;
            }
            if (static_shape(prediction_shape) &&
                static_shape(prototype_shape)) {
                detector::detail::SegmentationContract contract;
                if (!detector::detail::resolve_segmentation_contract(
                        prediction_shape, prototype_shape,
                        config.output_format, contract)) {
                    LOG_ERROR("detector", "无法确定 Segment 输出契约");
                    return false;
                }
                resolved_segmentation_contract = contract;
            }
        } else if (model_task == ModelTask::POSE) {
            if (session.num_outputs() != 1) {
                LOG_ERROR(
                    "detector",
                    "Pose 仅支持单输出 raw head，实际 outputs={}",
                    session.num_outputs());
                return false;
            }
            if (session.output_type(0) !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                LOG_ERROR("detector", "Pose 输出必须为 float32");
                return false;
            }
            if (metadata_end_to_end.value_or(false) ||
                (config.output_format != OutputFormat::AUTO &&
                 config.output_format != OutputFormat::CHANNEL_FIRST)) {
                LOG_ERROR(
                    "detector",
                    "Pose 当前仅支持非 end-to-end 的 channel-first raw head");
                return false;
            }
            if (!parse_positive_int_pair(
                    session.metadata_value("kpt_shape"),
                    pose_keypoint_count, pose_keypoint_dimensions) ||
                (pose_keypoint_dimensions != 2 &&
                 pose_keypoint_dimensions != 3)) {
                LOG_ERROR(
                    "detector",
                    "Pose metadata 必须包含 kpt_shape=[K,2|3]");
                return false;
            }
            const auto declared_output_shape = session.output_shape(0);
            if (!valid_declared_prediction_shape(declared_output_shape)) {
                LOG_ERROR("detector", "Pose 输出必须是单 batch 三维张量");
                return false;
            }
            if (static_shape(declared_output_shape)) {
                detector::detail::PoseContract contract;
                if (!detector::detail::resolve_pose_contract(
                        declared_output_shape, config.output_format,
                        pose_keypoint_count, pose_keypoint_dimensions,
                        contract)) {
                    LOG_ERROR("detector", "无法确定 Pose 输出契约");
                    return false;
                }
                resolved_pose_contract = contract;
            }
        } else {
            if (session.num_outputs() != 1) {
                LOG_ERROR(
                    "detector",
                    "OBB 仅支持单输出 raw head，实际 outputs={}",
                    session.num_outputs());
                return false;
            }
            if (session.output_type(0) !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                LOG_ERROR("detector", "OBB 输出必须为 float32");
                return false;
            }
            if (metadata_end_to_end.value_or(false) ||
                (config.output_format != OutputFormat::AUTO &&
                 config.output_format != OutputFormat::CHANNEL_FIRST)) {
                LOG_ERROR(
                    "detector",
                    "OBB 当前仅支持非 end-to-end 的 channel-first raw head");
                return false;
            }
            const auto declared_output_shape = session.output_shape(0);
            if (!valid_declared_prediction_shape(declared_output_shape)) {
                LOG_ERROR("detector", "OBB 输出必须是单 batch 三维张量");
                return false;
            }
            if (static_shape(declared_output_shape)) {
                detector::detail::ObbContract contract;
                if (!detector::detail::resolve_obb_contract(
                        declared_output_shape, config.output_format,
                        contract)) {
                    LOG_ERROR("detector", "无法确定 OBB 输出契约");
                    return false;
                }
                resolved_obb_contract = contract;
            }
        }

        active_provider = session.active_provider();
        loaded = true;
        const char* task_name = model_task == ModelTask::SEGMENT
            ? "segment"
            : (model_task == ModelTask::POSE
                ? "pose"
                : (model_task == ModelTask::OBB ? "obb" : "detect"));
        LOG_INFO("detector", "模型加载完成: {}x{}, task={}, provider={}",
                 width, height, task_name, active_provider);
        return true;
    }

    TaskRunResult finish_run(
            const std::vector<Ort::Value>* outputs,
            const detector::detail::SessionRunProfile& session_profile,
            const detector::detail::LetterBoxInfo& letterbox_info,
            const DetectorConfig& config,
            std::chrono::steady_clock::time_point started,
            std::chrono::steady_clock::time_point preprocessed,
            std::chrono::steady_clock::time_point inferred,
            RequestedOutput requested_output,
            InferenceProfile& profile) {
        using milliseconds = std::chrono::duration<double, std::milli>;

        TaskRunResult result;

        profile.preprocess_ms = std::chrono::duration_cast<milliseconds>(
            preprocessed - started).count();
        profile.inference_ms = std::chrono::duration_cast<milliseconds>(
            inferred - preprocessed).count();
        profile.h2d_ms = session_profile.h2d_ms;
        profile.d3d11_to_cuda_ms = session_profile.d3d11_to_cuda_ms;
        profile.d3d11_to_directml_ms =
            session_profile.d3d11_to_directml_ms;
        profile.gpu_preprocess_ms = session_profile.gpu_preprocess_ms;
        profile.execution_ms = session_profile.execution_ms;
        profile.d2h_ms = session_profile.d2h_ms;
        profile.explicit_device_copy = session_profile.explicit_device_copy;
        profile.gpu_preprocess = session_profile.gpu_preprocess;
        profile.d3d11_cuda_interop = session_profile.d3d11_cuda_interop;
        profile.d3d11_directml_interop =
            session_profile.d3d11_directml_interop;
        profile.input_upload_bytes = session_profile.input_upload_bytes;
        profile.input_device_copy_bytes =
            session_profile.input_device_copy_bytes;
        if (!outputs) {
            profile.status = DetectionStatus::INFERENCE_FAILED;
            return result;
        }
        const size_t expected_outputs =
            model_task == ModelTask::SEGMENT ? 2U : 1U;
        if (outputs->size() != expected_outputs ||
            !std::all_of(outputs->begin(), outputs->end(),
                [](const Ort::Value& value) { return value.IsTensor(); })) {
            profile.status = DetectionStatus::INVALID_OUTPUT;
            return result;
        }
        if (config.enable_output_fingerprint) {
            profile.output_fingerprint = output_fingerprint(*outputs);
        }

        OutputFormat resolved = OutputFormat::CHANNEL_FIRST;
        if (model_task == ModelTask::DETECT) {
            if (requested_output != RequestedOutput::BOXES) {
                profile.status = DetectionStatus::UNSUPPORTED_TASK;
                return result;
            }
            const auto output_info =
                (*outputs)[0].GetTensorTypeAndShapeInfo();
            if (output_info.GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto output_shape = output_info.GetShape();
            const OutputFormat requested = requested_output_format(
                config, metadata_end_to_end, output_shape);
            if (!detector::detail::resolve_output_format(
                    output_shape, requested, resolved) ||
                (resolved_output_format.has_value() &&
                 *resolved_output_format != resolved)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            resolved_output_format = resolved;

            candidate_detections.clear();
            if (!detector::detail::decode_output(
                    (*outputs)[0].GetTensorData<float>(), output_shape,
                    resolved, config.conf_threshold,
                    candidate_detections)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            if (!detector::detail::finalize_detections(
                    candidate_detections, resolved,
                    config.nms_threshold, config.top_k,
                    letterbox_info, result.segmentation.detections,
                    nms_suppressed)) {
                profile.status = DetectionStatus::POSTPROCESS_FAILED;
                return {};
            }
        } else if (model_task == ModelTask::SEGMENT) {
            if (requested_output != RequestedOutput::BOXES &&
                requested_output != RequestedOutput::SEGMENTATION) {
                profile.status = DetectionStatus::UNSUPPORTED_TASK;
                return result;
            }
            const Ort::Value& prediction =
                (*outputs)[prediction_output_index];
            const Ort::Value& prototype =
                (*outputs)[prototype_output_index];
            const auto prediction_info =
                prediction.GetTensorTypeAndShapeInfo();
            const auto prototype_info =
                prototype.GetTensorTypeAndShapeInfo();
            if (prediction_info.GetElementType() !=
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                prototype_info.GetElementType() !=
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto prediction_shape = prediction_info.GetShape();
            const auto prototype_shape = prototype_info.GetShape();
            detector::detail::SegmentationContract contract;
            if (!detector::detail::resolve_segmentation_contract(
                    prediction_shape, prototype_shape,
                    config.output_format, contract)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto same_contract = [](
                    const detector::detail::SegmentationContract& left,
                    const detector::detail::SegmentationContract& right) {
                return left.class_count == right.class_count &&
                       left.mask_channels == right.mask_channels &&
                       left.anchors == right.anchors &&
                       left.prototype_height == right.prototype_height &&
                       left.prototype_width == right.prototype_width;
            };
            if (resolved_segmentation_contract.has_value() &&
                !same_contract(*resolved_segmentation_contract, contract)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            resolved_segmentation_contract = contract;

            const float* prediction_data =
                prediction.GetTensorData<float>();
            const float* prototype_data = prototype.GetTensorData<float>();
            segmentation_candidates.clear();
            if (!detector::detail::decode_segmentation_output(
                    prediction_data, prediction_shape, contract,
                    config.conf_threshold, segmentation_candidates)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            if (!detector::detail::finalize_segmentations(
                    segmentation_candidates, config.nms_threshold,
                    config.top_k, letterbox_info, prediction_data,
                    prediction_shape, prototype_data, prototype_shape,
                    contract,
                    requested_output == RequestedOutput::SEGMENTATION,
                    result.segmentation,
                    selected_segmentations, nms_suppressed,
                    mask_logits, mask_input)) {
                profile.status = DetectionStatus::POSTPROCESS_FAILED;
                return {};
            }
        } else if (model_task == ModelTask::POSE) {
            if (requested_output != RequestedOutput::BOXES &&
                requested_output != RequestedOutput::POSE) {
                profile.status = DetectionStatus::UNSUPPORTED_TASK;
                return result;
            }
            const Ort::Value& prediction = (*outputs)[0];
            const auto prediction_info =
                prediction.GetTensorTypeAndShapeInfo();
            if (prediction_info.GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto prediction_shape = prediction_info.GetShape();
            detector::detail::PoseContract contract;
            if (!detector::detail::resolve_pose_contract(
                    prediction_shape, config.output_format,
                    pose_keypoint_count, pose_keypoint_dimensions,
                    contract)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto same_contract = [](
                    const detector::detail::PoseContract& left,
                    const detector::detail::PoseContract& right) {
                return left.class_count == right.class_count &&
                       left.keypoint_count == right.keypoint_count &&
                       left.keypoint_dimensions ==
                           right.keypoint_dimensions &&
                       left.anchors == right.anchors;
            };
            if (resolved_pose_contract.has_value() &&
                !same_contract(*resolved_pose_contract, contract)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            resolved_pose_contract = contract;

            const float* prediction_data =
                prediction.GetTensorData<float>();
            pose_candidates.clear();
            if (!detector::detail::decode_pose_output(
                    prediction_data, prediction_shape, contract,
                    config.conf_threshold, pose_candidates)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            if (!detector::detail::finalize_poses(
                    pose_candidates, config.nms_threshold, config.top_k,
                    letterbox_info, prediction_data, prediction_shape,
                    contract,
                    requested_output == RequestedOutput::POSE,
                    result.pose, selected_poses, nms_suppressed)) {
                profile.status = DetectionStatus::POSTPROCESS_FAILED;
                return {};
            }
        } else {
            if (requested_output != RequestedOutput::BOXES &&
                requested_output != RequestedOutput::OBB) {
                profile.status = DetectionStatus::UNSUPPORTED_TASK;
                return result;
            }
            const Ort::Value& prediction = (*outputs)[0];
            const auto prediction_info =
                prediction.GetTensorTypeAndShapeInfo();
            if (prediction_info.GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            const auto prediction_shape = prediction_info.GetShape();
            detector::detail::ObbContract contract;
            if (!detector::detail::resolve_obb_contract(
                    prediction_shape, config.output_format, contract)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            if (resolved_obb_contract.has_value() &&
                (resolved_obb_contract->class_count !=
                     contract.class_count ||
                 resolved_obb_contract->anchors != contract.anchors)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            resolved_obb_contract = contract;

            obb_candidates.clear();
            if (!detector::detail::decode_obb_output(
                    prediction.GetTensorData<float>(), prediction_shape,
                    contract, config.conf_threshold, obb_candidates)) {
                profile.status = DetectionStatus::INVALID_OUTPUT;
                return result;
            }
            if (!detector::detail::finalize_obbs(
                    obb_candidates, config.nms_threshold, config.top_k,
                    letterbox_info,
                    requested_output == RequestedOutput::OBB,
                    result.obb, selected_obbs, nms_suppressed)) {
                profile.status = DetectionStatus::POSTPROCESS_FAILED;
                return {};
            }
        }
        const auto finished = std::chrono::steady_clock::now();
        profile.postprocess_ms = std::chrono::duration_cast<milliseconds>(
            finished - inferred).count();
        profile.total_ms = std::chrono::duration_cast<milliseconds>(
            finished - started).count();
        profile.status = DetectionStatus::SUCCESS;

        const size_t detection_count = model_task == ModelTask::POSE
            ? result.pose.detections.size()
            : (model_task == ModelTask::OBB
                ? result.obb.detections.size()
                : result.segmentation.detections.size());
        LOG_TRACE(
            "detector",
            "format={}, pre={:.2f}ms infer={:.2f}ms h2d={:.2f}ms d3d11_cuda={:.2f}ms d3d11_dml={:.2f}ms gpu_pre={:.2f}ms exec={:.2f}ms d2h={:.2f}ms post={:.2f}ms total={:.2f}ms upload={}B device_copy={}B det={}",
            output_format_name(resolved), profile.preprocess_ms,
            profile.inference_ms, profile.h2d_ms,
            profile.d3d11_to_cuda_ms,
            profile.d3d11_to_directml_ms,
            profile.gpu_preprocess_ms,
            profile.execution_ms, profile.d2h_ms,
            profile.postprocess_ms, profile.total_ms,
            profile.input_upload_bytes, profile.input_device_copy_bytes,
            detection_count);
        return result;
    }

    TaskRunResult run(const cv::Mat& bgr_image,
                      const DetectorConfig& config,
                      RequestedOutput requested_output,
                      InferenceProfile& profile) {
        using clock = std::chrono::steady_clock;
        using milliseconds = std::chrono::duration<double, std::milli>;

        const auto start = clock::now();
        detector::detail::LetterBoxInfo letterbox_info;
        const bool use_gpu_preprocess = session.gpu_preprocess_enabled();
        if (use_gpu_preprocess) {
            if (!detector::detail::letterbox_bgr_reuse(
                    bgr_image, prepared_bgr, resize_buffer,
                    static_cast<int>(width), static_cast<int>(height),
                    letterbox_info) ||
                !session.stage_gpu_input(prepared_bgr)) {
                profile.status = DetectionStatus::PREPROCESS_FAILED;
                return {};
            }
        } else if (!detector::detail::letterbox_reuse(
                       bgr_image, input_blob, resize_buffer,
                       static_cast<int>(width), static_cast<int>(height),
                       letterbox_info)) {
            profile.status = DetectionStatus::PREPROCESS_FAILED;
            return {};
        }
        const auto preprocessed = clock::now();

        detector::detail::SessionRunProfile session_profile;
        const std::vector<Ort::Value>* outputs = nullptr;
        if (use_gpu_preprocess) {
            outputs = session.run_gpu_preprocessed(session_profile);
        } else {
            Ort::MemoryInfo* memory = session.memory_info();
            if (!memory) {
                profile.status = DetectionStatus::INFERENCE_FAILED;
                return {};
            }
            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                *memory, input_blob.ptr<float>(), input_element_count,
                input_shape.data(), input_shape.size());
            outputs = session.run(input_tensor, session_profile);
        }
        const auto inferred = clock::now();
        return finish_run(
            outputs, session_profile, letterbox_info, config,
            start, preprocessed, inferred, requested_output, profile);
    }

    TaskRunResult run_d3d11(
            const D3D11TextureFrame& frame,
            const DetectorConfig& config,
            RequestedOutput requested_output,
            InferenceProfile& profile) {
        using clock = std::chrono::steady_clock;

        const auto started = clock::now();
        detector::detail::LetterBoxInfo letterbox_info;
        letterbox_info.scale = 1.0f;
        letterbox_info.orig_w = frame.width;
        letterbox_info.orig_h = frame.height;
        letterbox_info.target_w = static_cast<int>(width);
        letterbox_info.target_h = static_cast<int>(height);
        const auto preprocessed = clock::now();

        detector::detail::SessionRunProfile session_profile;
        const auto* outputs = session.run_d3d11_preprocessed(
            frame.resource.get(), frame.width, frame.height,
            *frame.synchronization,
            frame.shared_fence.get(), frame.fence_value,
            session_profile);
        const auto inferred = clock::now();
        return finish_run(
            outputs, session_profile, letterbox_info, config,
            started, preprocessed, inferred, requested_output, profile);
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
}

std::vector<Detection> Detector::detect(const cv::Mat& bgr_image) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run(
            bgr_image, config_, RequestedOutput::BOXES, current_profile);
        impl_->set_profile(current_profile);
        if (impl_->model_task == ModelTask::POSE) {
            return std::move(result.pose.detections);
        }
        if (impl_->model_task == ModelTask::OBB) {
            return std::move(result.obb.detections);
        }
        return std::move(result.segmentation.detections);
    } catch (const std::exception& e) {
        // detect() 是推理热路径，只允许编译期可移除的 DEBUG/TRACE。
        LOG_DEBUG("detector", "Detector::detect() 失败: {}", e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::detect() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

SegmentationResult Detector::segment(const cv::Mat& bgr_image) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!segmentation_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run(
            bgr_image, config_, RequestedOutput::SEGMENTATION,
            current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.segmentation);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::segment() 失败: {}", e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::segment() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

std::vector<Detection> Detector::detect_d3d11(
        const D3D11TextureFrame& frame) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    const bool directml_frame =
        impl_->active_provider == "DmlExecutionProvider";
    if (!frame.resource || !frame.synchronization ||
        (directml_frame &&
         (!frame.shared_fence || frame.fence_value == 0)) ||
        frame.width <= 0 || frame.height <= 0 ||
        frame.width != input_width() || frame.height != input_height()) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }
    if (!d3d11_interop_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run_d3d11(
            frame, config_, RequestedOutput::BOXES, current_profile);
        impl_->set_profile(current_profile);
        if (impl_->model_task == ModelTask::POSE) {
            return std::move(result.pose.detections);
        }
        if (impl_->model_task == ModelTask::OBB) {
            return std::move(result.obb.detections);
        }
        return std::move(result.segmentation.detections);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::detect_d3d11() 失败: {}",
                  e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::detect_d3d11() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

SegmentationResult Detector::segment_d3d11(
        const D3D11TextureFrame& frame) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!segmentation_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    const bool directml_frame =
        impl_->active_provider == "DmlExecutionProvider";
    if (!frame.resource || !frame.synchronization ||
        (directml_frame &&
         (!frame.shared_fence || frame.fence_value == 0)) ||
        frame.width <= 0 || frame.height <= 0 ||
        frame.width != input_width() || frame.height != input_height()) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }
    if (!d3d11_interop_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run_d3d11(
            frame, config_, RequestedOutput::SEGMENTATION,
            current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.segmentation);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::segment_d3d11() 失败: {}",
                  e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG(
            "detector", "Detector::segment_d3d11() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

bool Detector::segmentation_supported() const noexcept {
    return loaded() && impl_->model_task == ModelTask::SEGMENT;
}

PoseResult Detector::pose(const cv::Mat& bgr_image) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!pose_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run(
            bgr_image, config_, RequestedOutput::POSE, current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.pose);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::pose() 失败: {}", e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::pose() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

PoseResult Detector::pose_d3d11(const D3D11TextureFrame& frame) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!pose_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    const bool directml_frame =
        impl_->active_provider == "DmlExecutionProvider";
    if (!frame.resource || !frame.synchronization ||
        (directml_frame &&
         (!frame.shared_fence || frame.fence_value == 0)) ||
        frame.width <= 0 || frame.height <= 0 ||
        frame.width != input_width() || frame.height != input_height()) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }
    if (!d3d11_interop_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run_d3d11(
            frame, config_, RequestedOutput::POSE, current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.pose);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::pose_d3d11() 失败: {}",
                  e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::pose_d3d11() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

bool Detector::pose_supported() const noexcept {
    return loaded() && impl_->model_task == ModelTask::POSE;
}

ObbResult Detector::obb(const cv::Mat& bgr_image) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!obb_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run(
            bgr_image, config_, RequestedOutput::OBB, current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.obb);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::obb() 失败: {}", e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::obb() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

ObbResult Detector::obb_d3d11(const D3D11TextureFrame& frame) {
    InferenceProfile current_profile;
    if (!loaded()) {
        current_profile.status = DetectionStatus::NOT_LOADED;
        if (impl_) impl_->set_profile(current_profile);
        return {};
    }
    if (!obb_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_TASK;
        impl_->set_profile(current_profile);
        return {};
    }
    const bool directml_frame =
        impl_->active_provider == "DmlExecutionProvider";
    if (!frame.resource || !frame.synchronization ||
        (directml_frame &&
         (!frame.shared_fence || frame.fence_value == 0)) ||
        frame.width <= 0 || frame.height <= 0 ||
        frame.width != input_width() || frame.height != input_height()) {
        current_profile.status = DetectionStatus::INVALID_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }
    if (!d3d11_interop_supported()) {
        current_profile.status = DetectionStatus::UNSUPPORTED_INPUT;
        impl_->set_profile(current_profile);
        return {};
    }

    try {
        auto result = impl_->run_d3d11(
            frame, config_, RequestedOutput::OBB, current_profile);
        impl_->set_profile(current_profile);
        return std::move(result.obb);
    } catch (const std::exception& e) {
        LOG_DEBUG("detector", "Detector::obb_d3d11() 失败: {}",
                  e.what());
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    } catch (...) {
        LOG_DEBUG("detector", "Detector::obb_d3d11() 失败: 未知异常");
        current_profile.status = DetectionStatus::INFERENCE_FAILED;
        impl_->set_profile(current_profile);
        return {};
    }
}

bool Detector::obb_supported() const noexcept {
    return loaded() && impl_->model_task == ModelTask::OBB;
}

bool Detector::d3d11_interop_supported() const noexcept {
    return loaded() && impl_->session.d3d11_interop_enabled();
}

bool Detector::prepare_d3d11(
        const D3D11TextureFrame& frame) noexcept {
    if (!d3d11_interop_supported() || !frame.resource ||
        !frame.synchronization ||
        frame.width != input_width() || frame.height != input_height()) {
        return false;
    }
    try {
        return impl_->session.prepare_d3d11_preprocessed(
            frame.resource.get(), frame.width, frame.height,
            *frame.synchronization, frame.shared_fence.get());
    } catch (...) {
        return false;
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

InferenceProfile Detector::profile() const noexcept {
    return impl_ ? impl_->profile() : InferenceProfile{};
}

bool Detector::end_profiling(std::string& profile_path) noexcept {
    profile_path.clear();
    return loaded() && impl_->session.end_profiling(profile_path);
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
