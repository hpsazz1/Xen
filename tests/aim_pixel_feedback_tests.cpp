#include "runtime/aim_frame_internal.h"
#include "log/log.h"
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int kFrames = 320;
constexpr double kPixelsPerCount = .5215; // 固定fixture响应，不是实际设备校准结论。
constexpr double kConfiguredDelayMs = 15;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Clock::duration ms(double value) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(value));
}
double milliseconds(Clock::time_point time, Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(time - begin).count();
}
// 真值只依赖虚拟时钟：右移112帧、停48帧、左移112帧、再停48帧。
double world(double seconds) {
    const double forward = std::clamp(seconds, 0.0, 112.0 / 240);
    const double reverse = std::clamp(seconds - 160.0 / 240, 0.0, 112.0 / 240);
    return 132 + 120 * (forward - reverse);
}
struct Mode {
    const char* name;
    bool output, jitter, step;
    double observation_age_ms = 3;
    bool drop_window = false;
    double world_offset = 0;
};
constexpr std::array<Mode, 5> kModes{{
    {"zero_output", false, false, false}, {"correct", true, false, false},
    {"feedback_jitter", true, true, false}, {"physical_delay_step", true, false, true},
    {"step_and_jitter", true, true, true}}};
struct Event {
    Clock::time_point issued, completed, effect;
    int dx;
};
double camera_at(const std::vector<Event>& events, Clock::time_point time) {
    double camera = 0;
    for (const auto& event : events) if (event.effect <= time) camera += event.dx * kPixelsPerCount;
    return camera;
}
struct Row {
    int dx;
    double camera, lead, unseen, preview;
    int requested;
};

std::vector<Row> replay(const cv::Mat& texture, Mode mode, bool prediction, std::ostream* csv,
                        bool nominal_lead_contract = true) {
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_delay_compensation = true;
    config.enable_prediction = prediction;
    config.control_delay_ms = static_cast<float>(kConfiguredDelayMs);
    config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15;
    config.max_prediction_lead_percent = 35;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .5f;
    config.soft_zone_radius_percent = 0;
    config.acquisition_range_percent = 100;
    Aim aim(config);
    runtime::detail::RuntimeObservationClock clock;
    runtime::detail::CameraMotionEstimator estimator;
    const auto begin = Clock::time_point{} + 30s;
    std::vector<Event> events;
    std::vector<Row> rows;
    std::uint64_t identity = 0;
    double previous_camera = 0;
    unsigned consumed = 0, residual = 0, nonzero = 0;
    double max_inventory_error = 0, max_background_error = 0;
    for (int n = 0; n < kFrames; ++n) {
        const auto observation = begin + std::chrono::nanoseconds(static_cast<long long>(n) * 1000000000 / 240);
        const auto control = observation + ms(mode.observation_age_ms);
        const double backend_ms = mode.jitter ? std::array{.1, 1., 3., .3}[n % 4] : .1;
        const auto completed = control + ms(backend_ms);
        const double actual_delay_ms = mode.step && n >= 80 ? 27 : 15;
        const auto effect = control + ms(actual_delay_ms);
        if (!events.empty()) require(events.back().completed < control,
            "同步回执必须先于下帧控制；本矩阵不模拟阻塞超帧或latest-only丢帧");
        const double time_s = std::chrono::duration<double>(observation - begin).count();
        const double position = world(time_s) + mode.world_offset;
        const double camera = camera_at(events, observation);
        const double center = position - camera;
        require(center > 122 && center < 198, "合成目标必须留在两背景块之间，避免把夹具越界当预测缺陷");
        require(std::abs(camera) < 240, "独立plant超出合成纹理边界");
        CapturedFrame captured;
        captured.width = captured.height = captured.source_width = captured.source_height = 320;
        cv::getRectSubPix(texture, {320, 320}, {static_cast<float>(512 + camera), 159.5f}, captured.bgr);
        const Detection detection{static_cast<float>(center - 10), 141.1f,
                                  static_cast<float>(center + 10), 195.1f, .95f, 0};
        cv::rectangle(captured.bgr, cv::Rect(static_cast<int>(std::floor(center - 10)), 141, 21, 55),
                      cv::Scalar(20, 160, 230), cv::FILLED);
        captured.timing.sequence = n + 1;
        captured.timing.captured_at = observation;
        auto prepared = runtime::detail::prepare_aim_frame(captured, {detection}, clock, estimator, true);
        // 只替代时间；不修改生产估计器的位移、质量、资格或世代。
        prepared.frame.control_at = control;
        if (prepared.reset_aim) aim.reset();
        const auto& frame = prepared.frame;
        const auto& background = frame.background_motion_x;
        const auto result = aim.process(frame);
        if (!identity && result.has_target) identity = result.target.track_id;
        require(result.status == AimStatus::SUCCESS && result.has_target && identity != 0 &&
                result.target.track_id == identity && result.target.state == TrackState::CONFIRMED &&
                result.target.matched_observation_valid && !result.target.predicted,
                "全部帧须保留同一CONFIRMED实测目标，不能过滤失配帧");
        const auto& diagnostic = result.control;
        const double lead = result.target.prediction_aim_x - result.target.base_aim_x;
        const double world_delta = world(std::chrono::duration<double>(effect - begin).count()) + mode.world_offset - position;
        if (nominal_lead_contract && prediction && !mode.step && ((n >= 48 && n <= 111) || (n >= 224 && n <= 267))) {
            const double direction = n < 112 ? 1 : -1;
            require(direction * (lead - world_delta) >= -.05,
                    "固定匹配延迟的稳定前探须覆盖独立世界位移，不能长期不足");
        }
        if ((n >= 144 && n <= 159) || n >= 304)
            require(std::abs(lead) < .01, "两段停止恢复尾窗须撤回公开前探");
        const double truth_background = -(camera - previous_camera);
        double physical_unseen = 0, configured_unseen = 0;
        for (const auto& event : events) if (event.completed <= control) {
            if (event.effect > observation) physical_unseen += event.dx;
            if (event.issued + ms(kConfiguredDelayMs) > observation) configured_unseen += event.dx;
        }
        if (diagnostic.background_motion_use_x == AimBackgroundMotionUse::CONSUMED) {
            ++consumed;
            max_background_error = std::max(max_background_error,
                std::abs(background.dx_roi_pixels - truth_background));
        }
        if (diagnostic.residual_role_x) {
            ++residual;
            max_inventory_error = std::max(max_inventory_error,
                std::abs(diagnostic.execution_unseen_command_x_counts - configured_unseen));
        }
        const int requested = result.has_command ? result.command.dx_counts : 0;
        const int dy = result.has_command ? result.command.dy_counts : 0;
        require(dy == 0, "固定Y真值不可产生纵向输出");
        const bool permission = mode.output && !(mode.drop_window && n >= 64 && n <= 71);
        const int applied = runtime::detail::aim_frame_dispatch_allowed(frame, permission) ? requested : 0;
        if (applied) ++nonzero;
        // 使用Runtime实际调用的公开回执入口。零回执对应plant确实零输出，不谎报已执行命令。
        require(aim.record_backend_completed_command(frame.sequence, completed, applied, 0),
                "生产回执入口必须接受本帧真实应用值");
        events.push_back({control, completed, effect, applied});
        if (csv) *csv << mode.name << ',' << prediction << ',' << n << ','
            << milliseconds(observation, begin) << ',' << milliseconds(control, begin) << ','
            << milliseconds(completed, begin) << ',' << milliseconds(effect, begin) << ','
            << position << ',' << camera << ',' << center - 160 << ',' << requested << ',' << applied << ','
            << static_cast<int>(background.status) << ',' << static_cast<int>(diagnostic.background_motion_use_x) << ','
            << background.usable_patch_count << ',' << background.min_response << ','
            << truth_background << ',' << background.dx_roi_pixels << ','
            << diagnostic.observer_target_velocity_x_counts_per_second * kPixelsPerCount << ','
            << world_delta << ',' << lead << ',' << result.target.aim_x - result.target.base_aim_x << ','
            << result.target.delay_compensated_aim_x - result.target.base_aim_x << ','
            << result.target.lead_active << ',' << diagnostic.residual_role_x << ','
            << physical_unseen << ',' << configured_unseen << ',' << diagnostic.execution_unseen_command_x_counts << ','
            << diagnostic.execution_world_preview_x_counts << ',' << result.target.track_id << '\n';
        rows.push_back({applied, camera, lead, diagnostic.execution_unseen_command_x_counts,
                       diagnostic.execution_world_preview_x_counts, requested});
        previous_camera = camera;
    }
    require(rows.size() == kFrames && consumed >= 300 && residual >= 240,
            "完整矩阵必须有足够实际背景消费及库存角色，不能以空样本通过");
    require(!mode.output || nonzero >= 100, "非零反馈矩阵必须实际驱动内存plant");
    require(max_background_error < .6, "真实像素估计须符合独立渲染真值的预登记0.6px容差");
    require(max_inventory_error < .001, "库存须遵守C+D配置合同，不能按B+D记账");
    std::cout << mode.name << " prediction=" << prediction << " frames=" << rows.size()
              << " consumed=" << consumed << " residual=" << residual << " nonzero=" << nonzero
              << " background_max_px=" << max_background_error
              << " inventory_max_counts=" << max_inventory_error << '\n';
    return rows;
}

void same_feedback_result(const std::vector<Row>& baseline, const std::vector<Row>& jittered) {
    require(baseline.size() == jittered.size(), "反馈对照不能缺帧");
    for (std::size_t n = 0; n < baseline.size(); ++n) {
        const auto& a = baseline[n]; const auto& b = jittered[n];
        require(a.dx == b.dx && std::abs(a.camera - b.camera) < 1e-9 && std::abs(a.lead - b.lead) < 1e-9 &&
                std::abs(a.unseen - b.unseen) < 1e-9 && std::abs(a.preview - b.preview) < 1e-9,
                "B变化但均在下次C前确认且实际E不变，不应改变像素/提前量/命令库存");
    }
}

void sensitivity(const cv::Mat& texture, std::ostream* csv) {
    const auto reference = replay(texture, {"reference", true, false, false}, true, csv);
    const auto dropped = replay(texture, {"permission_drop", true, false, false, 3, true}, true, csv, false);
    const auto aged = replay(texture, {"frame_age_9ms", true, false, false, 9}, true, csv, false);
    const auto aligned = replay(texture, {"initially_aligned", true, false, false, 3, false, 28}, true, csv, false);
    const auto first_pixel_change = [&](const auto& changed) {
        for (int n = 0; n < kFrames; ++n)
            if (std::abs(reference[n].camera - changed[n].camera) > 1e-9) return n;
        return kFrames;
    };
    const int drop_pixel_change = first_pixel_change(dropped);
    const int age_pixel_change = first_pixel_change(aged);
    int feedback_before_pixels = 0, later_request_changes = 0, age_before_pixels = 0, alignment_changes = 0;
    for (int n = 0; n < kFrames; ++n) {
        if (n >= 64 && n <= 71) require(dropped[n].dx == 0, "撤销许可的窗口必须真实零应用/零回执");
        if (n > 64 && n < drop_pixel_change &&
            std::abs(reference[n].unseen - dropped[n].unseen) > .001) ++feedback_before_pixels;
        if (n > 64 && reference[n].requested != dropped[n].requested) ++later_request_changes;
        if (n > 0 && n < age_pixel_change &&
            std::abs(reference[n].preview - aged[n].preview) > .001) ++age_before_pixels;
        if (reference[n].requested != aligned[n].requested) ++alignment_changes;
    }
    require(reference[64].dx != 0, "回执扰动必须击中真实非零命令，不能用零命令证明敏感性");
    require(feedback_before_pixels > 0 && later_request_changes > 0,
            "零回执必须在像素改变前影响库存，并改变后续生产请求；不能仅检查fixture强制的零输出");
    require(age_before_pixels > 0 && alignment_changes > 0,
            "独立帧龄与初始位置扰动必须影响实际生产计算，不能把测试不敏感称为鲁棒");
    std::cout << "sensitivity: feedback_before_pixel_change=" << feedback_before_pixels
              << " first_drop_pixel_change=" << drop_pixel_change
              << " later_request_changes=" << later_request_changes
              << " age_before_pixel_change=" << age_before_pixels
              << " first_age_pixel_change=" << age_pixel_change
              << " initial_alignment_request_changes=" << alignment_changes << '\n';
}
} // namespace

int main(int argc, char** argv) try {
    const bool sensitivity_only = argc >= 2 && std::string(argv[1]) == "--sensitivity";
    const int output_argument = sensitivity_only ? 2 : 1;
    require(argc <= output_argument + 1, "用法：aim_pixel_feedback_tests [--sensitivity] [新证据目录]");
    LogConfig logging; logging.enable_console = logging.enable_file = logging.enable_debug_file = false;
    Log::init(logging);
    cv::setNumThreads(1);
    cv::ocl::setUseOpenCL(false);
    cv::Mat texture(320, 1024, CV_8UC3);
    cv::RNG random(74521); random.fill(texture, cv::RNG::UNIFORM, 0, 255);
    std::ofstream csv;
    std::filesystem::path csv_path;
    if (argc > output_argument) {
        const auto root = std::filesystem::u8path(argv[output_argument]);
        require(!std::filesystem::exists(root), "证据目录必须全新，禁止覆盖已有文件");
        require(std::filesystem::create_directories(root), "无法创建证据目录");
        std::ofstream pixels(root / "texture.bgr", std::ios::binary);
        pixels.write(reinterpret_cast<const char*>(texture.data), static_cast<std::streamsize>(texture.total() * texture.elemSize()));
        pixels.close(); require(pixels.good(), "合成纹理写入失败");
        csv_path = root / "frames.csv";
        csv.open(root / "frames.csv.tmp"); require(csv.good(), "无法创建逐帧CSV");
        csv << std::setprecision(17)
            << "mode,prediction,frame,O_ms,C_ms,B_ms,E_ms,world_px,camera_px,source_error_px,requested_dx,applied_dx,"
               "background_status,background_use,patches,response,true_background_px,estimated_background_px,"
               "estimated_world_velocity_px_s,true_world_advance_px,prediction_lead_px,total_reference_offset_px,"
               "delay_offset_px,lead_active,residual_role,true_unseen_counts,configured_unseen_counts,aim_unseen_counts,"
               "world_preview_counts,track_id\n";
    }
    if (sensitivity_only) sensitivity(texture, csv.is_open() ? &csv : nullptr);
    else for (const bool prediction : {false, true}) {
        std::array<std::vector<Row>, 5> results;
        for (std::size_t i = 0; i < kModes.size(); ++i)
            results[i] = replay(texture, kModes[i], prediction, csv.is_open() ? &csv : nullptr);
        same_feedback_result(results[1], results[2]);
        same_feedback_result(results[3], results[4]);
    }
    if (csv.is_open()) {
        csv.close(); require(csv.good(), "逐帧CSV关闭失败");
        std::filesystem::rename(csv_path.parent_path() / "frames.csv.tmp", csv_path);
    }
    std::cout << (sensitivity_only ? "pixel feedback sensitivity: 4 cases, 1280 frames, passed\n" :
                                    "pixel feedback: 10 cases, 3200 frames, passed\n");
    Log::shutdown(); return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; Log::shutdown(); return 1;
}
