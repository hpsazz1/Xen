#include "runtime/aim_frame_internal.h"
#include "log/log.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
double median(std::vector<double> values) {
    check(values.size() == 20, "尾段必须包含全部20帧，不能筛掉无预测帧");
    std::sort(values.begin(), values.end()); return values[values.size()/2];
}
struct Summary { double lead, observer; unsigned consumed, rejected; };
Summary replay(int scene, int width_mode, std::ostream* csv) {
    cv::Mat canvas(320, 400, CV_8UC3);
    cv::RNG random(74521); random.fill(canvas, cv::RNG::UNIFORM, 0, 255);
    runtime::detail::RuntimeObservationClock clock;
    runtime::detail::CameraMotionEstimator estimator;
    AimConfig config;
    config.min_confirmed_hits = 1;
    config.enable_prediction = config.enable_delay_compensation = true;
    config.control_delay_ms = 15; config.max_delay_compensation_ms = 44;
    config.max_delay_compensation_percent = 15; config.max_prediction_lead_percent = 35;
    config.counts_per_pixel_x = config.counts_per_pixel_y = .5f;
    config.soft_zone_radius_percent = 0; config.acquisition_range_percent = 100;
    Aim aim(config);
    const auto begin = Clock::now() - 2s;
    AimFrame previous;
    std::uint64_t identity = 0;
    std::vector<double> leads, observers;
    unsigned consumed = 0, rejected = 0;
    float previous_center = 0;
    double previous_lead = 0;
    // n按1起编号，I2第51张侵入右块，第70张退出，第71张形成第二个干净pair。
    for (int n = 1; n <= 80; ++n) {
        const bool varying = width_mode == 1 || (width_mode == 2 && n > 40);
        const float width = 20 + (varying ? n%2 : 0);
        const int camera = scene == 1 ? n : 0;
        const float center = scene == 0 ? 128 + .75f*n : scene == 1 ? 190 - .25f*n :
            160 + .75f*(n <= 60 ? n : 120-n);
        const Detection detection{center-width/2,141.1f,center+width/2,195.1f,.95f,0};
        CapturedFrame captured;
        captured.width = captured.height = captured.source_width = captured.source_height = 320;
        captured.bgr = canvas(cv::Rect(camera,0,320,320)).clone();
        const int left = static_cast<int>(std::floor(detection.x1));
        const int right = static_cast<int>(std::ceil(detection.x2));
        cv::rectangle(captured.bgr, cv::Rect(left,141,right-left,55), cv::Scalar(20,160,230), cv::FILLED);
        captured.timing.sequence = n;
        captured.timing.captured_at = begin + std::chrono::nanoseconds(static_cast<long long>(n-1)*1000000000/240);
        auto prepared = runtime::detail::prepare_aim_frame(captured,{detection},clock,estimator,true);
        check(prepared.frame.control_at >= captured.timing.captured_at && prepared.background_motion_ms >= 0,
              "真实组装入口必须在图像估计后取控制时刻");
        // 与既有生产组帧回归一致，离线重放显式原时间；不修改估计器输出或资格。
        prepared.frame.control_at = captured.timing.captured_at + 3ms;
        if (prepared.reset_aim) aim.reset();
        const auto& frame = prepared.frame;
        const auto& bg = frame.background_motion_x;
        const bool foreground = scene == 2 && n >= 51 && n <= 70;
        if (n == 1) check(bg.status == AimBackgroundMotionStatus::WARMING, "首张真实图像必须WARMING，不能制造有效零背景");
        else if (foreground) check(bg.status == AimBackgroundMotionStatus::FOREGROUND,
                                   "当前或上一张含遮挡时必须拒绝；首张退出仍不能恢复");
        else {
            check(bg.status == AimBackgroundMotionStatus::VALID && bg.usable_patch_count == 2 && bg.min_response >= .5f,
                  "真实双块估计必须通过纹理与响应质量，不能手填VALID");
            const double expected_dx = scene == 1 ? -1 : 0;
            check(std::abs(bg.dx_roi_pixels-expected_dx) < .1, "真实裁图的背景位移符号和幅度必须匹配独立相机真值");
        }
        const auto result = aim.process(frame);
        if (!identity && result.has_target) identity = result.target.track_id;
        check(result.status == AimStatus::SUCCESS && result.has_target && identity != 0 &&
              result.target.track_id == identity && result.target.state == TrackState::CONFIRMED &&
              !result.target.predicted && result.target.matched_observation_valid,
              "图像链每帧必须同一CONFIRMED实测目标，不能借丢失身份通过");
        const double lead = result.target.prediction_aim_x-result.target.base_aim_x;
        if (n > 1 && !foreground) {
            ++consumed;
            check(result.control.background_motion_use_x == AimBackgroundMotionUse::CONSUMED,
                  "生产Aim必须实际消费估计器的合格位移");
            check(std::abs(result.control.observer_camera_motion_x_source_pixels-bg.dx_roi_pixels)<.001,
                  "估计位移必须以正确source单位到达实际控制诊断");
            if (scene < 2) {
                const double dt = std::chrono::duration<double>(frame.captured_at-previous.captured_at).count();
                const double world_velocity = (center-previous_center-bg.dx_roi_pixels)/dt;
                check(std::abs(world_velocity/180-1)<.14, "真实背景补偿后源世界速度必须接近180px/s独立真值");
            }
        } else if (foreground) {
            ++rejected;
            // 资格立即撤销，但公开参考遵守既有1.5对角线/秒回收，不能强迫瞬时跳回。
            const double dt = std::chrono::duration<double>(frame.captured_at-previous.captured_at).count();
            const double budget = std::hypot(result.target.x2-result.target.x1,result.target.y2-result.target.y1)*1.5*dt;
            check(result.control.background_motion_use_x != AimBackgroundMotionUse::CONSUMED &&
                  lead >= -.001 && lead <= previous_lead+.001 && previous_lead-lead <= budget+.001,
                  "污染背景不得消费；已公开偏移必须单调且在既有速度预算内回收");
            if (previous_lead>.01) check(lead<previous_lead-.001, "污染期间不能冻结或增加旧前探");
            if (n>=64) check(std::abs(lead)<.01 && !result.target.lead_active,
                             "持续污染超过回收窗口后必须归零，不能沿用旧世界速度");
            if (n==51 || n==70) std::cout << "I2 rejection n=" << n << " lead=" << lead << '\n';
        }
        if (n > 60) {
            leads.push_back(lead);
            observers.push_back(result.control.observer_target_velocity_x_counts_per_second);
            if (scene < 2) check(result.target.lead_active && lead >= 3.24,
                                 "静背景或相机运动尾20帧每帧都须覆盖真实速度的一次18ms时域");
        }
        if (csv) *csv << scene << ',' << width_mode << ',' << n << ',' << center << ',' << width << ','
            << static_cast<int>(bg.status) << ',' << bg.usable_patch_count << ',' << bg.min_response << ','
            << bg.dx_roi_pixels << ',' << static_cast<int>(result.control.background_motion_use_x) << ','
            << result.control.observer_target_velocity_x_counts_per_second << ',' << lead << ',' << result.target.track_id << '\n';
        check(aim.record_backend_completed_command(frame.sequence,frame.control_at,0,0), "图像回放逐帧明确零执行，不发送设备命令");
        previous = frame; previous_center = center; previous_lead = lead;
    }
    check(consumed == (scene == 2 ? 59 : 79) && rejected == (scene == 2 ? 20 : 0),
          "必须覆盖全部有效和遮挡帧，不能筛选失败帧");
    const Summary result{median(leads),median(observers),consumed,rejected};
    std::cout << "I" << scene << " width=" << width_mode << " leadP50=" << result.lead
              << " observerP50=" << result.observer << " consumed=" << consumed << " rejected=" << rejected << '\n';
    return result;
}
}
int main(int argc, char** argv) try {
    LogConfig config; config.enable_console = config.enable_file = false; Log::init(config);
    std::ofstream csv;
    if (argc == 2) { csv.open(argv[1]); check(csv.good(),"CSV无法创建");
        csv << "scene,width_mode,frame,center,width,status,patches,response,dx,background_use,observer_counts_s,lead,track_id\n"; }
    Summary baseline{};
    for (int scene = 0; scene < 3; ++scene) {
        const auto fixed = replay(scene,0,csv.is_open()?&csv:nullptr);
        for (int mode : {1,2}) {
            const auto alternate = replay(scene,mode,csv.is_open()?&csv:nullptr);
            if (scene < 2) check(std::abs(fixed.lead-alternate.lead)<.25,
                                 "真实图像输入只改变框宽不应持续压低预测幅度");
        }
        if (scene == 0) baseline = fixed;
        if (scene == 1) check(fixed.observer > 0 && std::abs(fixed.observer/baseline.observer-1)<.14,
                             "相机使图像目标向左时，实际控制观察器仍恢复与静背景同向同量的世界运动");
    }
    Log::shutdown(); return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; Log::shutdown(); return 1; }
