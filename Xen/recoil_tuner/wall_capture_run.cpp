#include "recoil_tuner/wall_capture_run.h"
#include "recoil_tuner/wall_registration_internal.h"
#include "recoil/recoil_calibration.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace recoil_tuner {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double milliseconds(Clock::time_point value, Clock::time_point origin) {
    return std::chrono::duration<double, std::milli>(value - origin).count();
}
std::int64_t nanoseconds(Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}
struct StoredFrame { CapturedFrame frame; Clock::time_point received; cv::Size input_size; };
Json geometry_json(const StoredFrame& saved) {
    return {{"processing", "wall_processing_v1_max960x540_area"},
        {"registration", detail::kWallRegistration},
        {"input_size", {saved.input_size.width, saved.input_size.height}},
        {"processing_size", {saved.frame.bgr.cols, saved.frame.bgr.rows}},
        {"source_size", {saved.frame.source_width, saved.frame.source_height}},
        {"source_pixels_per_pixel", {saved.frame.source_pixels_per_pixel_x, saved.frame.source_pixels_per_pixel_y}},
        {"roi_origin", {saved.frame.roi_x, saved.frame.roi_y}},
        {"processing_scale", {static_cast<double>(saved.frame.bgr.cols) / saved.input_size.width,
            static_cast<double>(saved.frame.bgr.rows) / saved.input_size.height}}};
}
struct CaptureBuffer {
    std::mutex mutex;
    std::vector<StoredFrame> frames;
    StoredFrame latest;
    std::uint64_t bytes = 0;
    std::string error;
    std::string recovery_action;
    Json registration_failure;
    StoredFrame registration_failure_frame;
    bool recording = false;
    cv::Mat reference;
    cv::Size reference_input_size;
    double reference_source_scale_x = 0, reference_source_scale_y = 0;
    Json reference_geometry;
    Clock::time_point last_saved{};
    std::uint64_t intentionally_unsaved = 0;
};
void write_json(const std::filesystem::path& path, const Json& value) {
    auto temporary = path; temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    file << value.dump(2); file.close();
    if (!file) throw std::runtime_error("采集报告写入失败");
    std::filesystem::rename(temporary, path);
}
bool pressed(const InputSnapshot& input, int key) { return key > 0 && key < 256 && input.virtual_keys[key]; }
}

WallRunResult run_wall_capture(const WallRunRequest& request, std::shared_ptr<IMouseController> device,
        const std::atomic<bool>& canceled, const std::function<void(const std::string&)>& progress) noexcept {
    WallRunResult result;
    const bool measurement_required=request.mode!=WallRunMode::TEST||request.measurement_required;
    result.report = {{"schema_version", 1}, {"completed", false}, {"cleanup_unknown", false},
        {"physically_accepted", false}, {"measurement_required",measurement_required},
        {"frames", Json::array()}, {"calibration", Json::array()}};
    std::unique_ptr<ICapture> capture;
    std::jthread capture_thread;
    CaptureBuffer buffer;
    bool left_may_be_down = false, worker_ready = false, directory_owned = false;
    const auto origin = Clock::now();
    std::string termination = "FAILED";
    auto notify = [&](const std::string& message) { if (progress) progress(message); };
    auto release = [&] {
        if (worker_ready && request.on_firing_stopped) {
            worker_ready = false;
            try { request.on_firing_stopped(); } catch (...) { result.cleanup_unknown = true; }
        }
        if (left_may_be_down && device) {
            result.report["firing"]["release_call_started_ns"] = nanoseconds(Clock::now());
            bool clean = false;
            for (int attempt = 0; attempt < 3 && !clean; ++attempt) {
                const auto receipt = device->set_left_button(false);
                clean = receipt.disposition == ButtonDisposition::ACKNOWLEDGED && !receipt.cleanup_required && !device->left_button_cleanup_required();
            }
            result.report["release_acknowledged"] = clean;
            if (!clean) result.cleanup_unknown = true;
            left_may_be_down = false;
        }
    };
    auto stop_capture = [&] {
        if (capture_thread.joinable()) { capture_thread.request_stop(); capture_thread.join(); }
        if (capture) capture->close();
    };
    try {
        if (!device || !device->output_owner_exclusive() || !request.context_valid ||
            request.duration_ms < 100 || request.duration_ms > 15000 || request.weapon_id.empty() ||
            request.target_shots < 0 || request.target_shots > 100 ||
            (request.target_shots > 0 && !request.observed_ammo_delta) ||
            !std::isfinite(request.sensitivity) || request.sensitivity <= 0 ||
            request.trigger_virtual_key <= 0 || request.trigger_virtual_key >= 256 ||
            request.cancel_virtual_key <= 0 || request.cancel_virtual_key >= 256 ||
            request.trigger_virtual_key == request.cancel_virtual_key || request.output_directory.empty())
            throw std::runtime_error("采集参数、用户按键或设备独占状态无效");
        if (request.mode != WallRunMode::CALIBRATE && (!device->supports_left_button() ||
            device->left_button_faulted() || device->left_button_cleanup_required()))
            throw std::runtime_error("左键状态未清理或设备不支持有界射击");
        if (std::filesystem::exists(request.output_directory)) throw std::runtime_error("采集目录已存在，拒绝覆盖");
        std::filesystem::create_directories(request.output_directory.parent_path());
        if (!std::filesystem::create_directory(request.output_directory)) throw std::runtime_error("无法独占创建采集目录");
        directory_owned = true;
        if(measurement_required){
        std::filesystem::create_directory(request.output_directory / "frames");
        auto config = request.capture;
        config.enable_d3d11_cuda_interop = false; config.enable_d3d11_directml_interop = false;
        capture = request.capture_factory ? request.capture_factory(config) : create_capture(config);
        if (!capture || !capture->open()) throw std::runtime_error("无法打开画面采集源");
        capture_thread = std::jthread([&](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    CapturedFrame frame;
                    const auto status = capture->grab(frame);
                    const auto received = Clock::now();
                    if (status == CaptureStatus::NO_FRAME || status == CaptureStatus::READY) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
                    }
                    if (status != CaptureStatus::FRAME || frame.bgr.empty()) {
                        std::lock_guard lock(buffer.mutex); buffer.error = "画面采集丢失或图像无效"; break;
                    }
                    const auto input_size = frame.bgr.size();
                    const auto input_bytes = static_cast<std::uint64_t>(frame.bgr.total()) * frame.bgr.elemSize();
                    if (input_bytes > 64ULL * 1024 * 1024) {
                        std::lock_guard lock(buffer.mutex); buffer.error = "单帧超过64 MiB采集上限"; break;
                    }
                    // 标定和测试使用同一分析图像空间，完整保存缩放事实，不冒称原分辨率原图。
                    if (frame.bgr.channels() == 4) {
                        cv::Mat bgr; cv::cvtColor(frame.bgr, bgr, cv::COLOR_BGRA2BGR); frame.bgr = std::move(bgr);
                    }
                    const double scale = std::min({1.0, 960.0 / input_size.width, 540.0 / input_size.height});
                    if (scale < 1) {
                        cv::Mat reduced;
                        cv::resize(frame.bgr, reduced, {std::max(1, static_cast<int>(std::floor(input_size.width * scale))),
                            std::max(1, static_cast<int>(std::floor(input_size.height * scale)))}, 0, 0, cv::INTER_AREA);
                        frame.bgr = std::move(reduced);
                        frame.source_pixels_per_pixel_x *= static_cast<double>(input_size.width) / frame.bgr.cols;
                        frame.source_pixels_per_pixel_y *= static_cast<double>(input_size.height) / frame.bgr.rows;
                    } else frame.bgr = frame.bgr.clone();
                    frame.width = frame.bgr.cols; frame.height = frame.bgr.rows; frame.bgr_storage.reset();
                    const auto bytes = static_cast<std::uint64_t>(frame.bgr.total()) * frame.bgr.elemSize();
                    std::lock_guard lock(buffer.mutex);
                    buffer.latest = {frame, received, input_size};
                    if (buffer.recording) {
                        if (input_size != buffer.reference_input_size ||
                            frame.source_pixels_per_pixel_x != buffer.reference_source_scale_x ||
                            frame.source_pixels_per_pixel_y != buffer.reference_source_scale_y ||
                            geometry_json(buffer.latest) != buffer.reference_geometry) {
                                buffer.recovery_action = "recalibrate";
                                buffer.error = "采集源几何或缩放发生变化，请重新标定"; break;
                        }
                        if (request.mode != WallRunMode::CALIBRATE && !buffer.reference.empty()) {
                            auto roi = request.registration_roi;
                            if (roi.width <= 0 || roi.height <= 0) roi = {frame.bgr.cols / 8, frame.bgr.rows / 8,
                                frame.bgr.cols / 4, frame.bgr.rows / 4};
                            if (frame.bgr.size() != buffer.reference.size() || roi.width < 32 || roi.height < 32 ||
                                (roi & cv::Rect(0, 0, frame.bgr.cols, frame.bgr.rows)) != roi) {
                                buffer.recovery_action = "recalibrate";
                                buffer.error = "采集区域或图像几何发生变化，请重新标定"; break;
                            }
                            const auto registration=detail::register_wall(buffer.reference,frame.bgr,roi);
                            const auto shift=registration.shift;
                            const auto response=registration.response;
                            const auto& reason=registration.failure;
                            if(reason=="insufficient_texture")
                                buffer.error="背景纹理不足，已停止本组；请换有清晰细节的固定靶面，重新标定后采集";
                            else if(reason=="registration_range_exceeded")
                                buffer.error="背景位移接近观测范围边缘，已停止本组；请重新对准并减少本阶段发数";
                            else if(!reason.empty())
                                buffer.error="背景匹配不可靠，已停止本组；请重新对准固定靶面并标定";
                            if (!reason.empty()) {
                                buffer.recovery_action = reason == "registration_range_exceeded" ? "reduce_shots" : "recalibrate";
                                const auto number = [](double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); };
                                buffer.registration_failure = {{"reason",reason},{"sequence",frame.timing.sequence},
                                    {"texture_stddev",number(registration.texture_stddev)},{"response",number(response)},
                                    {"shift_pixels",{number(shift.x),number(shift.y)}},{"roi",{roi.x,roi.y,roi.width,roi.height}},
                                    {"method",detail::kWallRegistration},{"midpoint_residual",number(registration.residual)},
                                    {"template_score",number(registration.template_score)},
                                    {"peak_separation",number(registration.peak_separation)}};
                                // 首枪可能早于30Hz归档间隔失败；保留已拥有的本帧，释放后独立落盘，不进入训练序列。
                                buffer.registration_failure_frame = buffer.latest;
                                break;
                            }
                        }
                        // 原始源帧仍逐帧做在线范围检查；落盘最多30 Hz，保存实际时间而非合成等间隔。
                        if (buffer.last_saved != Clock::time_point{} && received - buffer.last_saved < std::chrono::microseconds(33334)) {
                            ++buffer.intentionally_unsaved; continue;
                        }
                        if (buffer.frames.size() >= 1200 || buffer.bytes + bytes > 768ULL * 1024 * 1024) {
                            buffer.error = "达到本组图像内存/帧数上限，已停止射击"; break;
                        }
                        buffer.bytes += bytes; buffer.frames.push_back({std::move(frame), received, input_size});
                        buffer.last_saved = received;
                    }
                }
            } catch (...) { std::lock_guard lock(buffer.mutex); buffer.error = "图像线程异常"; }
        });
        }
        std::string readiness_reason="等待键鼠释放";
        auto input_valid = [&](bool armed) {
            if (canceled.load()) throw std::runtime_error("用户取消");
            InputSnapshot input;
            if (!device->poll_input(input) || !input.state_valid || input.status != InputMonitorStatus::READY)
                throw std::runtime_error("原始输入状态不可用");
            if (pressed(input, request.cancel_virtual_key)) throw std::runtime_error("用户取消键");
            const bool released = !pressed(input, 1) && !pressed(input, 'W') && !pressed(input, 'A') &&
                !pressed(input, 'S') && !pressed(input, 'D') && !pressed(input, request.trigger_virtual_key);
            if (armed && !released) throw std::runtime_error("检测到人工移动、左键或测试键，已停止本组");
            const bool context = request.context_valid();
            const auto context_reason=!context&&request.context_block_reason?request.context_block_reason():"源焦点或武器上下文失效";
            if (armed && !context) throw std::runtime_error(context_reason);
            readiness_reason=!released?"等待松开测试键、左键和WASD":!context?context_reason:"等待连续就绪";
            if(measurement_required){
                std::lock_guard lock(buffer.mutex);
                if (!buffer.error.empty()) throw std::runtime_error(buffer.error);
                if (armed && (buffer.latest.frame.bgr.empty() || Clock::now() - buffer.latest.received > std::chrono::milliseconds(500)))
                    throw std::runtime_error("图像超过500 ms未更新");
            }
            return released && context;
        };
        notify("请松开测试键、左键和移动键，保持游戏前台；准备好后自动执行本组");
        Clock::time_point ready_since{};
        std::string reported_reason;
        for (;;) {
            const auto now = Clock::now();
            if (now - origin > std::chrono::seconds(15)) throw std::runtime_error("准备超时："+readiness_reason);
            bool ready = input_valid(false);
            if(measurement_required){
                std::lock_guard lock(buffer.mutex);
                const bool image_ready=!buffer.latest.frame.bgr.empty()&&now-buffer.latest.received<std::chrono::milliseconds(200);
                if(ready&&!image_ready)readiness_reason="等待有效图像（尚无画面或图像已过期）";
                ready=ready&&image_ready;
            }
            result.report["readiness_blocker"]=readiness_reason;
            if(reported_reason!=readiness_reason){reported_reason=readiness_reason;notify(readiness_reason);}
            if (!ready) ready_since = {};
            else if (ready_since == Clock::time_point{}) ready_since = now;
            else if (now - ready_since >= std::chrono::milliseconds(100)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(measurement_required){
            std::lock_guard lock(buffer.mutex);
            result.report["geometry"] = geometry_json(buffer.latest);
            if (request.geometry_valid && !request.geometry_valid(result.report["geometry"])) {
                buffer.recovery_action = "recalibrate";
                throw std::runtime_error("当前实际画面几何与标定不一致，请重新标定");
            }
            buffer.frames.push_back(buffer.latest);
            buffer.reference = buffer.latest.frame.bgr;
            buffer.reference_input_size = buffer.latest.input_size;
            buffer.reference_source_scale_x = buffer.latest.frame.source_pixels_per_pixel_x;
            buffer.reference_source_scale_y = buffer.latest.frame.source_pixels_per_pixel_y;
            buffer.reference_geometry = result.report["geometry"];
            buffer.last_saved = buffer.latest.received;
            buffer.bytes = buffer.latest.frame.bgr.total() * buffer.latest.frame.bgr.elemSize();
            buffer.recording = true;
        }
        auto wait_checked = [&](std::chrono::milliseconds duration) {
            const auto until = Clock::now() + duration;
            while (Clock::now() < until) { input_valid(true); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        };
        if (request.mode == WallRunMode::CALIBRATE) {
            notify("正在进行一次X/Y画面标定，不射击；请勿移动鼠标");
            const std::array<MouseMoveCommand, 4> commands{{{12, 0}, {-12, 0}, {0, 12}, {0, -12}}};
            for (const auto& command : commands) {
                input_valid(true);
                StoredFrame before;
                { std::lock_guard lock(buffer.mutex); before = buffer.latest; }
                const auto started = Clock::now();
                const auto receipt = device->move(command);
                if (!receipt.succeeded || !receipt.protocol_ack_received) throw std::runtime_error("标定位移未收到已知ACK，停止本组");
                wait_checked(std::chrono::milliseconds(450));
                StoredFrame after;
                { std::lock_guard lock(buffer.mutex); after = buffer.latest; }
                auto roi = request.registration_roi;
                if (roi.width <= 0 || roi.height <= 0) roi = {before.frame.bgr.cols / 8, before.frame.bgr.rows / 8,
                    before.frame.bgr.cols / 4, before.frame.bgr.rows / 4};
                if ((roi & cv::Rect(0, 0, before.frame.bgr.cols, before.frame.bgr.rows)) != roi || roi.width < 32 || roi.height < 32 ||
                    before.frame.bgr.size() != after.frame.bgr.size() || after.received <= receipt.backend_completed_at)
                    throw std::runtime_error("标定图像区域、几何或时间无效");
                const auto registration=detail::register_wall(before.frame.bgr,after.frame.bgr,roi,{64,64});
                const auto delta=registration.shift;
                const auto response=registration.response;
                if(!registration.failure.empty() || cv::norm(delta)>64)
                    throw std::runtime_error("墙面纹理或标定配准不足，请选择有纹理的固定墙面");
                const auto id = std::to_string(result.calibration.size() + 1);
                result.calibration.push_back({{static_cast<double>(command.dx_counts), static_cast<double>(command.dy_counts)},
                    {delta.x, delta.y}, true, (request.output_directory / ("calibration-" + id)).string()});
                result.report["calibration"].push_back({{"counts", {command.dx_counts, command.dy_counts}}, {"pixel_delta", {delta.x, delta.y}},
                    {"acknowledged", true}, {"evidence_id", result.calibration.back().evidence_id}, {"call_started_ns", nanoseconds(started)},
                    {"completed_ns", nanoseconds(receipt.backend_completed_at)}, {"response", response},
                    {"before_sequence", before.frame.timing.sequence}, {"after_sequence", after.frame.timing.sequence}});
            }
        } else {
            if (request.on_ready) { worker_ready = true; if (!request.on_ready()) throw std::runtime_error("候选测试调度器准备失败"); }
            input_valid(true);
            notify(measurement_required?"正在执行一次有界扫射并采集图像，取消键可立即停止":"正在验证已有曲线，不采集画面；取消键可立即停止");
            const auto started = Clock::now();
            left_may_be_down = true;
            const auto receipt = device->set_left_button(true);
            result.report["firing"] = {{"call_started_ns", nanoseconds(started)}, {"completed_ns", nanoseconds(receipt.backend_completed_at)},
                {"acknowledged", receipt.disposition == ButtonDisposition::ACKNOWLEDGED}, {"duration_ms", request.duration_ms}, {"actual_shot_time_known", false}};
            // 已确认DOWN仍有待UP的清理债务，这是正常状态，不能误判失败。
            if (receipt.disposition != ButtonDisposition::ACKNOWLEDGED) throw std::runtime_error("射击命令未收到明确ACK");
            if (request.on_firing_started && !request.on_firing_started(started, receipt)) throw std::runtime_error("候选执行未接受射击信号");
            const auto until = Clock::now() + std::chrono::milliseconds(request.duration_ms);
            std::optional<int> observed;
            bool target_reached = false;
            while (Clock::now() < until) {
                input_valid(true);
                if (request.observed_ammo_delta) observed = request.observed_ammo_delta();
                if (request.target_shots > 0 && observed && *observed >= request.target_shots) { target_reached = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            release();
            if (result.cleanup_unknown) throw std::runtime_error("左键清理结果未知");
            wait_checked(std::chrono::milliseconds(300));
            if (request.observed_ammo_delta) observed = request.observed_ammo_delta();
            const int overshoot = observed && request.target_shots > 0 ? std::max(0, *observed - request.target_shots) : 0;
            result.report["requested_shots"] = request.target_shots;
            result.report["observed_ammo_delta"] = observed ? Json(*observed) : Json(nullptr);
            result.report["overshoot"] = overshoot;
            result.report["target_reached"] = target_reached;
            result.report["exact_shot_count_verified"] = false;
            result.report["training_eligible"] = measurement_required && (request.target_shots == 0 || (target_reached && overshoot == 0));
        }
        release(); stop_capture();
        if (!buffer.error.empty()) throw std::runtime_error(buffer.error);
        result.completed = !result.cleanup_unknown;
        termination = "COMPLETED"; result.message = measurement_required?"本组采集完成，图像与执行记录已保存":"已有曲线验证完成，执行记录已保存；请人工观察效果";
    } catch (const std::exception& error) { result.message = error.what(); release(); stop_capture(); }
      catch (...) { result.message = "采集异常，已停止本组"; release(); stop_capture(); }
    try {
        notify(measurement_required?"射击已停止，正在保存本组图像和时间证据":"射击已停止，正在保存本组执行记录");
        result.report["completed"] = result.completed;
        if (!result.completed && request.mode == WallRunMode::CALIBRATE) buffer.recovery_action = "recalibrate";
        if (!buffer.recovery_action.empty()) result.report["recovery_action"] = buffer.recovery_action;
        if (!buffer.registration_failure.is_null()) result.report["registration_failure"] = buffer.registration_failure;
        if (!result.completed) result.report["training_eligible"] = false;
        result.report["cleanup_unknown"] = result.cleanup_unknown;
        result.report["termination"] = termination;
        result.report["message"] = result.message;
        result.report["mode"] = request.mode == WallRunMode::CALIBRATE ? "calibrate" : request.mode == WallRunMode::TEST ? "test" : "capture";
        result.report["weapon_id"] = request.weapon_id; result.report["sensitivity"] = request.sensitivity;
        result.report["environment_fingerprint"] = request.environment_fingerprint;
        result.report["capture_sampling"] = {{"maximum_saved_hz", 30}, {"intentionally_unsaved_frames", buffer.intentionally_unsaved},
            {"timing_is_fixed_interval", false}, {"saved_image_bytes", buffer.bytes}};
        result.report["image_processing"] = "wall_processing_v1_max960x540_area";
        if (!buffer.frames.empty()) {
            const auto frame_origin = buffer.frames.front().received;
            for (std::size_t index = 0; index < buffer.frames.size(); ++index) {
                const auto& saved = buffer.frames[index]; const auto& timing = saved.frame.timing;
                const auto name = "frames/frame-" + std::to_string(index) + ".png";
                std::vector<unsigned char> bytes;
                if (!cv::imencode(".png", saved.frame.bgr, bytes, {cv::IMWRITE_PNG_COMPRESSION, 1})) throw std::runtime_error("图像编码失败");
                std::ofstream file(request.output_directory / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); file.close();
                if (!file) throw std::runtime_error("图像写入失败");
                const auto hash = recoil_calibration_sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                const double time = milliseconds(saved.received, frame_origin);
                result.frames.push_back({time, saved.frame.bgr});
                result.report["frames"].push_back({{"file", name}, {"sha256", hash}, {"time_ms", time}, {"received_ns", nanoseconds(saved.received)},
                    {"sequence", timing.sequence}, {"source_sequence", timing.source_sequence}, {"source_sequence_valid", timing.source_sequence_valid},
                    {"source_time_valid", timing.source_time_timing_valid}, {"source_time_ns", timing.source_time_timing_valid ? Json(nanoseconds(timing.source_time_at)) : Json(nullptr)},
                    {"source_uncertainty_ms", timing.source_time_timing_valid ? Json(timing.source_clock_uncertainty_ms) : Json(nullptr)},
                    {"source_time_basis", SourceTimeBasisName(timing.source_time_basis)}, {"source_dropped_frames", timing.source_dropped_frames},
                    {"transport_dropped_frames", timing.transport_dropped_frames}, {"temporal_basis", "receive_time_only"},
                    {"input_size", {saved.input_size.width, saved.input_size.height}}, {"processing_size", {saved.frame.bgr.cols, saved.frame.bgr.rows}},
                    {"processing_scale", {static_cast<double>(saved.frame.bgr.cols) / saved.input_size.width,
                        static_cast<double>(saved.frame.bgr.rows) / saved.input_size.height}},
                    {"source_pixels_per_pixel", {saved.frame.source_pixels_per_pixel_x, saved.frame.source_pixels_per_pixel_y}},
                    {"roi_origin", {saved.frame.roi_x, saved.frame.roi_y}}});
            }
        }
        if (directory_owned) {
            if (!buffer.registration_failure_frame.frame.bgr.empty()) {
                const auto& failed = buffer.registration_failure_frame;
                std::vector<unsigned char> bytes;
                if (!cv::imencode(".png", failed.frame.bgr, bytes, {cv::IMWRITE_PNG_COMPRESSION, 1}))
                    throw std::runtime_error("失配诊断图像编码失败");
                const std::string name = "registration-failure.png";
                std::ofstream file(request.output_directory / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); file.close();
                if (!file) throw std::runtime_error("失配诊断图像写入失败");
                auto& failure = result.report["registration_failure"];
                failure["image_file"] = name; failure["reference_file"] = "frames/frame-0.png";
                failure["received_ns"] = nanoseconds(failed.received);
                failure["sha256"] = recoil_calibration_sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            }
            result.report["source_hash"] = recoil_calibration_sha256(result.report.dump());
            if (request.mode == WallRunMode::CALIBRATE && result.completed) {
                result.report["raw_calibration_path"] = (request.output_directory / "raw-calibration.json").string();
                write_json(request.output_directory / "raw-calibration.json", result.report);
            }
            result.report["capture_path"] = (request.output_directory / "run.json").string();
            write_json(request.output_directory / "run.json", result.report);
        }
    } catch (...) {
        result.completed = false; result.message = "采集图像或报告保存不完整，不能用于生成曲线";
        result.report["completed"] = false; result.report["message"] = result.message;
        result.report["training_eligible"] = false;
    }
    return result;
}
}
