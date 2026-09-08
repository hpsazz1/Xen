#include "runtime/aim_frame_internal.h"
#include "config/config.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Options {
    std::string mode, input, config, model, report;
    std::size_t warmup = 100;
};

std::uint64_t integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number >= 0) return static_cast<std::uint64_t>(number);
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (!text.empty() && parsed.ec == std::errc{} &&
            parsed.ptr == text.data() + text.size()) return result;
    }
    throw std::runtime_error("需要非负整数或完整十进制整数字符串，禁止浮点时间戳");
}

Clock::time_point time_point(const Json& value) {
    const auto number = integer(value);
    if (number == 0 || number > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("观测/控制时间越界或为零");
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(static_cast<std::int64_t>(number))));
}

std::string ns(Clock::time_point value) {
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count());
}

double finite_number(const Json& value) {
    if (!value.is_number()) throw std::runtime_error("几何必须为JSON数值");
    const double result = value.get<double>();
    if (!std::isfinite(result)) throw std::runtime_error("几何含非有限值");
    return result;
}

int positive_size(const Json& value) {
    const auto result = integer(value);
    if (result == 0 || result > 65536)
        throw std::runtime_error("图像尺寸无效");
    return static_cast<int>(result);
}

double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

Options parse(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) throw std::runtime_error("选项缺少值");
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--mode") result.mode = value;
        else if (key == "--input") result.input = value;
        else if (key == "--config") result.config = value;
        else if (key == "--model") result.model = value;
        else if (key == "--report") result.report = value;
        else if (key == "--warmup") result.warmup = integer(Json(value));
        else throw std::runtime_error("未知选项：" + key);
    }
    if ((result.mode != "estimator-only" && result.mode != "trt-ab") ||
        result.input.empty() || result.config.empty() || result.report.empty())
        throw std::runtime_error("用法：--mode estimator-only|trt-ab --input JSON --config INI --report JSON [--model ONNX] [--warmup 100]");
    if (fs::exists(fs::u8path(result.report)))
        throw std::runtime_error("报告已存在，拒绝覆盖");
    return result;
}

struct InputRow {
    CapturedFrame captured;
    std::vector<Detection> detections;
    Clock::time_point observation_at, control_at, backend_at;
    bool locked = false;
    bool explicit_reset = false;
    double center_x = 0, center_y = 0;
    std::string png_path;
};

std::vector<InputRow> read_input(const std::string& path) {
    std::ifstream stream(fs::u8path(path));
    if (!stream) throw std::runtime_error("无法读取输入JSON");
    Json input; stream >> input;
    if (integer(input.at("schema")) != 1 || !input.at("rows").is_array())
        throw std::runtime_error("输入schema必须为1且rows必须为数组");
    std::vector<InputRow> rows;
    std::uint64_t previous_sequence = 0;
    for (const auto& source : input.at("rows")) {
        InputRow row;
        auto& captured = row.captured;
        captured.timing.sequence = integer(source.at("sequence"));
        if (captured.timing.sequence == 0 || captured.timing.sequence <= previous_sequence)
            throw std::runtime_error("原sequence必须严格递增，缺口可保留");
        previous_sequence = captured.timing.sequence;
        captured.timing.captured_at = time_point(source.at("capture_ns"));
        row.observation_at = time_point(source.at("observation_ns"));
        row.control_at = time_point(source.at("control_ns"));
        row.backend_at = time_point(source.at("backend_ns"));
        if (row.control_at < row.observation_at || row.backend_at < row.control_at)
            throw std::runtime_error("原时间关系不合法");
        row.locked = source.at("lock_active").get<bool>();
        row.explicit_reset = source.at("runtime_observation_clock_reset").get<bool>();
        row.center_x = finite_number(source.at("center_x"));
        row.center_y = finite_number(source.at("center_y"));
        const auto& geometry = source.at("captured_geometry");
        captured.width = positive_size(geometry.at("width"));
        captured.height = positive_size(geometry.at("height"));
        captured.roi_x = finite_number(geometry.at("roi_x"));
        captured.roi_y = finite_number(geometry.at("roi_y"));
        captured.source_width = positive_size(geometry.at("source_width"));
        captured.source_height = positive_size(geometry.at("source_height"));
        captured.encoded_width = positive_size(geometry.at("encoded_width"));
        captured.encoded_height = positive_size(geometry.at("encoded_height"));
        captured.source_pixels_per_pixel_x = finite_number(geometry.at("scale_x"));
        captured.source_pixels_per_pixel_y = finite_number(geometry.at("scale_y"));
        if (captured.source_pixels_per_pixel_x <= 0 || captured.source_pixels_per_pixel_y <= 0)
            throw std::runtime_error("原ROI比例必须为正");
        captured.timing.source_time_timing_valid = geometry.at("source_time_valid").get<bool>();
        captured.timing.source_clock_session_id = integer(geometry.at("source_session_id"));
        const auto basis = geometry.at("source_time_basis").get<std::string>();
        if (basis == "NDI_SDK_SUBMISSION")
            captured.timing.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
        else if (basis != "UNAVAILABLE") throw std::runtime_error("未知原source_time_basis");
        if (captured.timing.source_time_timing_valid) {
            if (captured.timing.source_clock_session_id == 0)
                throw std::runtime_error("有效源时间缺少session");
            captured.timing.source_time_at = row.observation_at;
        } else if (captured.timing.captured_at != row.observation_at) {
            throw std::runtime_error("source无效时原观测时间必须等于原capture时间");
        }
        for (const auto& detection : source.at("detections")) {
            const auto& box = detection.at("box");
            if (!box.is_array() || box.size() != 4 ||
                !detection.at("class_id").is_number_integer())
                throw std::runtime_error("检测框/类别输入无效");
            Detection d;
            d.x1 = static_cast<float>(finite_number(box[0]));
            d.y1 = static_cast<float>(finite_number(box[1]));
            d.x2 = static_cast<float>(finite_number(box[2]));
            d.y2 = static_cast<float>(finite_number(box[3]));
            d.confidence = static_cast<float>(finite_number(detection.at("confidence")));
            d.class_id = detection.at("class_id").get<int>();
            if (!std::isfinite(d.x1) || !std::isfinite(d.y1) ||
                !std::isfinite(d.x2) || !std::isfinite(d.y2) ||
                !(d.x2 > d.x1 && d.y2 > d.y1) || d.confidence < 0 || d.confidence > 1)
                throw std::runtime_error("检测框面积/置信度无效");
            row.detections.push_back(d);
        }
        if (!source.at("png_path").is_null()) {
            row.png_path = source.at("png_path").get<std::string>();
            const auto png = fs::u8path(row.png_path);
            if (!png.is_absolute()) throw std::runtime_error("PNG必须为绝对路径");
            std::ifstream file(png, std::ios::binary);
            if (!file) throw std::runtime_error("PNG缺失：" + row.png_path);
            const std::vector<unsigned char> bytes(
                (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            captured.bgr = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (captured.bgr.empty() || captured.bgr.type() != CV_8UC3 ||
                captured.bgr.cols != captured.width || captured.bgr.rows != captured.height)
                throw std::runtime_error("PNG与原ROI几何不一致：" + row.png_path);
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) throw std::runtime_error("没有输入行");
    return rows;
}

Json distribution(std::vector<double> values) {
    if (values.empty()) return {{"samples", 0}};
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto i = static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1;
        return values[std::min(i, values.size() - 1)];
    };
    return {{"samples", values.size()}, {"mean_ms", std::accumulate(values.begin(), values.end(), 0.0) / values.size()},
        {"p50_ms", percentile(.50)}, {"p95_ms", percentile(.95)},
        {"p99_ms", percentile(.99)}, {"max_ms", values.back()}};
}

Json motion_json(const AimBackgroundMotionX& motion) {
    return {{"status", AimBackgroundMotionStatusName(motion.status)},
        {"previous_sequence", std::to_string(motion.previous_sequence)},
        {"sequence", std::to_string(motion.sequence)},
        {"previous_captured_ns", ns(motion.previous_captured_at)},
        {"captured_ns", ns(motion.captured_at)},
        {"observation_epoch", std::to_string(motion.observation_epoch)},
        {"dx_roi_pixels", motion.dx_roi_pixels}, {"min_response", motion.min_response},
        {"disagreement_roi_pixels", motion.disagreement_roi_pixels},
        {"usable_patch_count", motion.usable_patch_count}};
}

Json profile_json(const InferenceProfile& p) {
    return {{"status", DetectionStatusName(p.status)}, {"total_ms", p.total_ms},
        {"preprocess_ms", p.preprocess_ms}, {"inference_ms", p.inference_ms},
        {"postprocess_ms", p.postprocess_ms}, {"h2d_ms", p.h2d_ms},
        {"gpu_preprocess_ms", p.gpu_preprocess_ms}, {"execution_ms", p.execution_ms},
        {"d2h_ms", p.d2h_ms}, {"explicit_device_copy", p.explicit_device_copy},
        {"gpu_preprocess", p.gpu_preprocess}, {"input_upload_bytes", p.input_upload_bytes},
        {"input_device_copy_bytes", p.input_device_copy_bytes}};
}

void check_profile(const Detector& detector, const InferenceProfile& p) {
    if (detector.backend_name() != "TensorrtExecutionProvider" ||
        p.status != DetectionStatus::SUCCESS || !p.explicit_device_copy ||
        !p.gpu_preprocess || p.input_device_copy_bytes != 0 ||
        p.input_upload_bytes != static_cast<std::uint64_t>(detector.input_width()) *
            static_cast<std::uint64_t>(detector.input_height()) * 3U)
        throw std::runtime_error("真实TensorRT/Graph/GPU前处理路径验证失败");
}

Json run_pass(const std::vector<InputRow>& rows, const AimConfig& config,
              Detector* detector, bool measure, std::size_t warmup) {
    Aim aim(config);
    runtime::detail::CameraMotionEstimator estimator;
    runtime::detail::RuntimeObservationClock observation_clock;
    Json output = Json::array();
    std::vector<double> combined_times, detector_times, assembly_times, camera_times, aim_times;
    std::size_t image_index = 0;
    for (const auto& row : rows) {
        const bool has_image = !row.captured.bgr.empty();
        if (detector && !has_image) {
            // 真Detector模式缺原图时不拿旧检测伪装成真实推理。
            aim.reset(); estimator.reset();
            observation_clock = {};
            output.push_back({{"sequence", std::to_string(row.captured.timing.sequence)},
                {"status", "SKIPPED_MISSING_IMAGE"}});
            continue;
        }
        if (row.explicit_reset) { aim.reset(); estimator.reset(); observation_clock = {}; }
        const auto started = Clock::now();
        InferenceProfile profile;
        auto detections = detector ? detector->detect(row.captured.bgr) : row.detections;
        const auto detected = Clock::now();
        if (detector) { profile = detector->profile(); check_profile(*detector, profile); }
        auto prepared = runtime::detail::prepare_aim_frame(row.captured,
            std::move(detections), observation_clock, estimator, row.locked, measure);
        if (std::fabs(prepared.frame.control_center_x - row.center_x) > 1e-4 ||
            std::fabs(prepared.frame.control_center_y - row.center_y) > 1e-4 ||
            prepared.frame.captured_at != row.observation_at)
            throw std::runtime_error("生产组装seam与原几何/观测身份不一致");
        // 仅离线诊断覆盖：原控制时间保持因果，wall成本单独记录。
        // 生产Runtime仍使用seam在背景估计完成后取得的now。
        prepared.frame.control_at = row.control_at;
        const auto assembled = Clock::now();
        if (prepared.reset_aim) aim.reset();
        const auto result = aim.process(prepared.frame);
        bool confirmation_ok = true;
        if (result.has_command) confirmation_ok = aim.record_backend_completed_command(
            prepared.frame.sequence, row.backend_at, row.locked ? result.command.dx_counts : 0,
            row.locked ? result.command.dy_counts : 0);
        const auto finished = Clock::now();
        if (!confirmation_ok) throw std::runtime_error("本程序请求的软件确认失败");
        const bool measured = has_image && image_index++ >= warmup;
        const auto& c = result.control;
        const auto& t = result.target;
        const double total_ms = milliseconds(started, finished);
        const double detect_ms = milliseconds(started, detected);
        const double assembly_ms = milliseconds(detected, assembled);
        const double aim_ms = milliseconds(assembled, finished);
        if (measured) {
            combined_times.push_back(total_ms); detector_times.push_back(detect_ms);
            assembly_times.push_back(assembly_ms); camera_times.push_back(prepared.background_motion_ms);
            aim_times.push_back(aim_ms);
        }
        output.push_back({{"sequence", std::to_string(prepared.frame.sequence)},
            {"png_path", row.png_path}, {"has_image", has_image}, {"measured", measured},
            {"observation_ns", ns(prepared.frame.captured_at)}, {"control_ns", ns(prepared.frame.control_at)},
            {"reset_aim", prepared.reset_aim}, {"frame_epoch", std::to_string(prepared.frame.observation_epoch)},
            {"background", motion_json(prepared.frame.background_motion_x)},
            {"background_motion_ms", prepared.background_motion_ms}, {"wall_combined_ms", total_ms},
            {"wall_detector_ms", detect_ms}, {"wall_assembly_ms", assembly_ms}, {"wall_aim_ms", aim_ms},
            {"detector_profile", detector ? profile_json(profile) : Json(nullptr)},
            {"detection_count", prepared.frame.detections.size()},
            {"status", AimStatusName(result.status)}, {"has_target", result.has_target},
            {"has_command", result.has_command}, {"confirmation_ok", confirmation_ok},
            {"control_evaluated", c.evaluated},
            {"command", {result.command.dx_counts, result.command.dy_counts}},
            {"track_id", t.track_id}, {"predicted", t.predicted},
            {"base", {t.base_aim_x, t.base_aim_y}}, {"box", {t.x1, t.y1, t.x2, t.y2}},
            {"velocity", {t.velocity_x, t.velocity_y}},
            {"delay_compensated", {t.delay_compensated_aim_x, t.delay_compensated_aim_y}},
            {"delay_compensation", {t.delay_compensation_x, t.delay_compensation_y}},
            {"delay_compensation_ms", {t.delay_compensation_ms_x, t.delay_compensation_ms_y}},
            {"final_aim", {t.aim_x, t.aim_y}},
            {"background_use", AimBackgroundMotionUseName(c.background_motion_use_x)},
            {"observer_camera_source_pixels", c.observer_camera_motion_x_source_pixels},
            {"observer_velocity_counts_per_second", c.observer_target_velocity_x_counts_per_second},
            {"proportional", c.proportional_x_counts}, {"integral", c.feedforward_x_counts},
            {"desired_before", c.desired_before_reverse_x_counts},
            {"phase", c.observer_phase_command_x_counts},
            {"residual", c.residual_before_quantization_x_counts},
            {"raw_left", c.reverse_translation_raw_left_x_roi_pixels},
            {"raw_right", c.reverse_translation_raw_right_x_roi_pixels},
            {"filtered", c.filtered_x_counts}, {"model_add", c.modelled_response_x_counts},
            {"shaped", c.shaped_x_counts}, {"delayed", c.delayed_command_x_counts},
            {"pending_net", c.pending_net_x_counts}, {"pending_abs", c.pending_absolute_x_counts}});
    }
    if (detector && combined_times.empty()) throw std::runtime_error("warmup后没有真实图像样本");
    return {{"background_enabled", measure}, {"rows", std::move(output)},
        {"wall_combined", distribution(combined_times)}, {"wall_detector", distribution(detector_times)},
        {"wall_assembly", distribution(assembly_times)}, {"camera", distribution(camera_times)},
        {"wall_aim", distribution(aim_times)}};
}
} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        AppConfig config; std::string error;
        if (!load_app_config(options.config, config, error)) throw std::runtime_error(error);
        if (!options.model.empty()) config.detector.model_path = options.model;
        const auto rows = read_input(options.input);
        LogConfig log_config; log_config.enable_file = false;
        log_config.enable_debug_file = false; log_config.enable_ringbuf = false;
        Log::init(log_config);
        struct Shutdown { ~Shutdown() { Log::shutdown(); } } shutdown;
        std::unique_ptr<Detector> detector;
        Json graph_evidence = nullptr;
        if (options.mode == "trt-ab") {
            if (config.detector.backend != BackendType::TENSORRT ||
                !config.detector.enable_trt_cuda_graph || !config.detector.enable_gpu_preprocess)
                throw std::runtime_error("trt-ab要求正式配置已启用TensorRT/Graph/GPU前处理");
            auto diagnostic_config = config.detector;
            diagnostic_config.enable_output_fingerprint = true;
            diagnostic_config.enable_ort_profiling = true;
            const auto report_path = fs::u8path(options.report);
            if (!report_path.parent_path().empty())
                fs::create_directories(report_path.parent_path());
            diagnostic_config.ort_profile_prefix = options.report + ".provider";
            Detector diagnostic(diagnostic_config);
            if (!diagnostic.load()) throw std::runtime_error("真实模型诊断加载失败");
            const auto first = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return !r.captured.bgr.empty(); });
            const auto last = std::find_if(rows.rbegin(), rows.rend(), [](const auto& r) { return !r.captured.bgr.empty(); });
            if (first == rows.end() || last == rows.rend()) throw std::runtime_error("没有原PNG");
            diagnostic.detect(first->captured.bgr); const auto a = diagnostic.profile();
            diagnostic.detect(last->captured.bgr); const auto b = diagnostic.profile();
            check_profile(diagnostic, a); check_profile(diagnostic, b);
            if (a.output_fingerprint == b.output_fingerprint)
                throw std::runtime_error("首末真实图输出指纹相同，无法证明Graph处理变化输入");
            graph_evidence = {{"first_sequence", std::to_string(first->captured.timing.sequence)},
                {"last_sequence", std::to_string(last->captured.timing.sequence)},
                {"first_output_fingerprint", std::to_string(a.output_fingerprint)},
                {"last_output_fingerprint", std::to_string(b.output_fingerprint)},
                {"first_profile", profile_json(a)}, {"last_profile", profile_json(b)}};
            std::string provider_path;
            if (!diagnostic.end_profiling(provider_path) ||
                !fs::is_regular_file(fs::u8path(provider_path)))
                throw std::runtime_error("独立ORT诊断未产生Provider profile");
            Json provider_events;
            std::ifstream provider_file(fs::u8path(provider_path));
            provider_file >> provider_events;
            if (!provider_events.is_array()) throw std::runtime_error("ORT profile不是事件数组");
            Json provider_counts = Json::object();
            for (const auto& event : provider_events) {
                if (!event.contains("args") || !event["args"].is_object() ||
                    !event["args"].contains("provider") ||
                    !event["args"]["provider"].is_string()) continue;
                const auto provider = event["args"]["provider"].get<std::string>();
                if (provider.empty()) continue;
                provider_counts[provider] = provider_counts.value(provider, 0) + 1;
            }
            if (provider_counts.value("TensorrtExecutionProvider", 0) == 0 ||
                provider_counts.value("CPUExecutionProvider", 0) != 0)
                throw std::runtime_error("Provider trace未确认TRT或出现CPU回退");
            graph_evidence["provider_profile_path"] = provider_path;
            graph_evidence["provider_event_counts"] = provider_counts;
            diagnostic.reset();
            config.detector.enable_output_fingerprint = false;
            config.detector.enable_ort_profiling = false;
            detector = std::make_unique<Detector>(config.detector);
            if (!detector->load()) throw std::runtime_error("真实性能Session加载失败");
        }
        std::uint64_t image_bytes = 0;
        for (const auto& row : rows) image_bytes += row.captured.bgr.total() * row.captured.bgr.elemSize();
        Json report = {{"schema", 1}, {"mode", options.mode}, {"input", options.input},
            {"config", options.config}, {"model", config.detector.model_path},
            {"physical_output", false}, {"runtime_capture_or_mouse_constructed", false},
            {"control_time_mode", "original_replay_metadata"},
            {"software_confirmation", "own_request_at_original_backend_timestamp_not_physical_replay"},
            {"queue_and_source_to_control_admission", "NOT_MEASURED"},
            {"opencv_threads", cv::getNumThreads()}, {"preloaded_image_bytes", image_bytes},
            {"input_rows", rows.size()}, {"warmup_images_per_pass", options.warmup},
            {"graph_changed_input_evidence", graph_evidence}, {"passes", Json::array()}};
        const std::string order = detector ? "ABBA" : "AB";
        for (char label : order) {
            auto pass = run_pass(rows, config.aim, detector.get(), label == 'B', options.warmup);
            pass["label"] = std::string(1, label); report["passes"].push_back(std::move(pass));
        }
        const auto destination = fs::u8path(options.report);
        if (!destination.parent_path().empty()) fs::create_directories(destination.parent_path());
        auto temporary = destination;
        temporary += ".tmp";
        if (fs::exists(temporary)) throw std::runtime_error("临时报告已存在，拒绝覆盖");
        std::ofstream file(temporary);
        if (!file) throw std::runtime_error("无法写报告");
        file << std::setw(2) << report << '\n'; file.close();
        if (!file) throw std::runtime_error("报告写入失败");
        fs::rename(temporary, destination);
        std::cout << "完成，无物理输出；rows=" << rows.size() << "，passes=" << order << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "离线背景输入验证失败：" << error.what() << '\n';
        return 1;
    }
}
