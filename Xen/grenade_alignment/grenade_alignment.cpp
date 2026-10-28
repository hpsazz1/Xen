#include "grenade_alignment/grenade_alignment_internal.h"
#include "log/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <opencv2/imgproc.hpp>

namespace grenade_alignment {
namespace {
using Clock = detail::Clock;
using namespace std::chrono_literals;
constexpr double kSearchRatio = 0.20;
constexpr double kAgreementRatio = 1.5 / 320.0;
// 精确瞄点不能在仍有一像素以上残差时结束；以ROI比例统一主动细调与投前复核。
constexpr double kAlignedRatio = 0.35 / 320.0;
constexpr auto kStableDuration = 80ms;
constexpr auto kFrameAge = 200ms;
constexpr auto kNoFrameDuration = 350ms;
constexpr auto kRunDuration = 5s;

struct Match {
    bool valid = false;
    cv::Point2d shift{};
    double score = 0;
    int patches = 0;
    std::string error;
};
struct PatchMatch { cv::Point2d shift, center; double score = 0; };
bool near(double a, double b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 1e-6; }

bool geometry_matches(const Reference& reference, const CapturedFrame& frame) {
    const auto& g = reference.geometry;
    return frame.storage == CapturedFrameStorage::CPU_BGR &&
        frame.bgr.type() == CV_8UC3 && !frame.bgr.empty() &&
        frame.width == g.roi_width && frame.height == g.roi_height &&
        frame.bgr.cols == frame.width && frame.bgr.rows == frame.height &&
        frame.source_width == g.source_width && frame.source_height == g.source_height &&
        near(frame.roi_x, g.roi_x) && near(frame.roi_y, g.roi_y) &&
        near(frame.source_pixels_per_pixel_x, g.scale_x) &&
        near(frame.source_pixels_per_pixel_y, g.scale_y);
}
cv::Mat reference_roi(const Reference& reference) {
    const auto& g = reference.geometry;
    if (g.roi_width != 320 || g.roi_height != 320 || g.source_width <= 0 || g.source_height <= 0 ||
        reference.aim_image.empty() || reference.aim_image.type() != CV_8UC3 ||
        reference.aim_image.cols != g.source_width || reference.aim_image.rows != g.source_height ||
        !std::isfinite(g.roi_x) || !std::isfinite(g.roi_y) ||
        !std::isfinite(g.scale_x) || !std::isfinite(g.scale_y) ||
        g.roi_x < 0 || g.roi_y < 0 || g.scale_x <= 0 || g.scale_y <= 0 ||
        g.roi_x + g.roi_width * g.scale_x > g.source_width ||
        g.roi_y + g.roi_height * g.scale_y > g.source_height)
        throw std::runtime_error("参考图或ROI映射不合法");
    cv::Mat map_x(g.roi_height, g.roi_width, CV_32F), map_y(map_x.size(), CV_32F);
    for (int y = 0; y < g.roi_height; ++y)
        for (int x = 0; x < g.roi_width; ++x) {
            map_x.at<float>(y, x) = static_cast<float>(g.roi_x + x * g.scale_x);
            map_y.at<float>(y, x) = static_cast<float>(g.roi_y + y * g.scale_y);
        }
    cv::Mat roi, gray;
    cv::remap(reference.aim_image, roi, map_x, map_y, cv::INTER_LINEAR);
    cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
    // 与实时图采用同一带宽，避免半像素视角变化的插值把真实相关峰压低。
    // 不降低匹配分数或第二峰歧义门；固定纹理和重复图案仍由后续质量判据决定。
    cv::GaussianBlur(gray, gray, {5,5}, 1.0);
    return gray;
}
bool safe_patch(const cv::Rect& patch, const cv::Point2d& center, int side) {
    // 准星固定在屏幕上，不能作为地图纹理；避开底部HUD/手臂常驻区域。
    const int margin = side / 20;
    const cv::Rect crosshair(static_cast<int>(center.x) - margin,
                            static_cast<int>(center.y) - margin, margin * 2 + 1, margin * 2 + 1);
    return patch.x >= 0 && patch.y >= 0 && patch.x + patch.width <= side &&
        patch.y + patch.height <= side * 7 / 8 && (patch & crosshair).area() == 0;
}
double subpixel(const cv::Mat& scores, const cv::Point& at, bool horizontal) {
    const int x = at.x, y = at.y;
    if ((horizontal && (x == 0 || x + 1 >= scores.cols)) ||
        (!horizontal && (y == 0 || y + 1 >= scores.rows))) return 0;
    const double a = scores.at<float>(y - !horizontal, x - horizontal);
    const double b = scores.at<float>(y, x);
    const double c = scores.at<float>(y + !horizontal, x + horizontal);
    const double denominator = a - 2 * b + c;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || denominator >= -1e-6) return 0;
    return std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
}
Match combine_matches(const std::vector<PatchMatch>& matches, int side) {
    if (matches.size() < 2) return {false,{},0,0,"共同纹理不足或匹配存在歧义，请靠近参考瞄点"};
    std::vector<double> xs, ys;
    for (const auto& value : matches) { xs.push_back(value.shift.x); ys.push_back(value.shift.y); }
    std::sort(xs.begin(),xs.end()); std::sort(ys.begin(),ys.end());
    const auto median = [](const auto& values) { return (values[(values.size()-1)/2] + values[values.size()/2])*0.5; };
    const cv::Point2d middle(median(xs),median(ys));
    Match result; result.score = 1;
    std::vector<cv::Point2d> centers;
    for (const auto& value : matches)
        if (std::abs(value.shift.x-middle.x) <= side*kAgreementRatio &&
            std::abs(value.shift.y-middle.y) <= side*kAgreementRatio) {
            result.shift += value.shift; result.score = std::min(result.score,value.score);
            centers.push_back(value.center); ++result.patches;
        }
    bool distributed = false;
    for (const auto& a : centers) for (const auto& b : centers)
        if (cv::norm(a-b) >= side*0.25) distributed = true;
    if (result.patches < 2 || !distributed)
        return {false,{},0,0,"不同区域的匹配位移不一致，请确认站位和画面"};
    result.shift *= 1.0/result.patches;
    result.valid = std::abs(result.shift.x) < side*kSearchRatio && std::abs(result.shift.y) < side*kSearchRatio;
    if (!result.valid) result.error = "瞄点偏移超过本次局部对齐范围";
    return result;
}
Match match(const cv::Mat& reference, const cv::Mat& current_bgr, cv::Point2d center) {
    cv::Mat current;
    cv::cvtColor(current_bgr, current, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(current, current, {5,5}, 1.0);
    const int side = reference.cols, width = side / 5, search_radius = static_cast<int>(side * kSearchRatio);
    const std::array<cv::Point, 6> origins{{{24,24}, {128,24}, {232,24}, {24,120}, {232,120}, {24,208}}};
    std::vector<PatchMatch> matches;
    const auto match_patch = [&](const cv::Rect& patch) {
        if (!safe_patch(patch, center, side)) return;
        cv::Scalar mean, deviation;
        cv::meanStdDev(reference(patch), mean, deviation);
        if (deviation[0] < 6.0) return;
        const int min_x = std::max(0, patch.x - search_radius), min_y = std::max(0, patch.y - search_radius);
        const int max_x = std::min(side - width, patch.x + search_radius);
        const int max_y = std::min(side - width, patch.y + search_radius);
        const cv::Rect search(min_x, min_y, max_x - min_x + width, max_y - min_y + width);
        cv::Mat scores;
        cv::matchTemplate(current(search), reference(patch), scores, cv::TM_CCOEFF_NORMED);
        double best = -1, second = -1;
        cv::Point best_at;
        for (int y = 0; y < scores.rows; ++y)
            for (int x = 0; x < scores.cols; ++x) {
                const double value = scores.at<float>(y, x);
                if (std::isfinite(value) && value > best &&
                    safe_patch({min_x+x, min_y+y, width, width}, center, side)) {
                    best = value; best_at = {x,y};
                }
            }
        if (best < 0.82) return;
        for (int y = 0; y < scores.rows; ++y)
            for (int x = 0; x < scores.cols; ++x) {
                if (std::abs(x-best_at.x) <= 3 && std::abs(y-best_at.y) <= 3) continue;
                const double value = scores.at<float>(y,x);
                if (std::isfinite(value) && safe_patch({min_x+x,min_y+y,width,width},center,side))
                    second = std::max(second,value);
            }
        if (best - second < 0.06) return;
        matches.push_back({{min_x + best_at.x - patch.x + subpixel(scores,best_at,true),
                            min_y + best_at.y - patch.y + subpixel(scores,best_at,false)},
                           {patch.x + width*0.5, patch.y + width*0.5},best});
    };
    for (const auto origin : origins)
        match_patch({origin.x * side / 320, origin.y * side / 320, width, width});
    const auto fixed = combine_matches(matches,side);
    if (fixed.valid) return fixed;

    // 天空等稀疏场景的细节可能恰好落在固定块之间。仅在原取样不能定位时补查网格，
    // 继续使用同一相关峰、二维位移和精度要求；相邻重叠块不能重复计作独立区域。
    const int step=width/2, last_y=side*7/8-width;
    for (int y=0;;y=std::min(y+step,last_y)) {
        for (int x=0;x+width<=side;x+=step) {
            const cv::Rect patch(x,y,width,width);
            const cv::Point2d patch_center(x+width*0.5,y+width*0.5);
            if (std::any_of(matches.begin(),matches.end(),[&](const auto& value) {
                const auto delta=value.center-patch_center;
                return cv::norm(delta)<side*0.25 || (std::abs(delta.x)<width && std::abs(delta.y)<width);
            })) continue;
            match_patch(patch);
        }
        if(y==last_y) break;
    }
    return combine_matches(matches,side);
}
struct CaptureCloser {
    ICapture* capture = nullptr;
    ~CaptureCloser() { if (capture) capture->close(); }
};
class SharedCapture final : public ICapture {
public:
    explicit SharedCapture(std::function<CaptureStatus(CapturedFrame&)> grab):grab_(std::move(grab)) {}
    bool open() noexcept override { return true; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        try { return grab_(frame); } catch (...) { return CaptureStatus::FAILURE; }
    }
    void close() noexcept override {}
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return "Runtime画面借用已结束"; }
private:
    std::function<CaptureStatus(CapturedFrame&)> grab_;
};
}

Result detail::run_with_dependencies(const Reference& reference, const AppConfig& config,
    std::shared_ptr<IMouseController> device, std::atomic_bool& canceled, bool verify_only,
    std::function<bool()> permitted, const Dependencies& dependencies) noexcept {
    Result result;
    Clock::time_point started{};
    std::uint64_t frames = 0, commands = 0;
    bool physical_motion_observed = false;
    try {
        if (!dependencies.create_capture || !dependencies.now || !dependencies.sleep || !dependencies.source)
            throw std::runtime_error("对齐运行依赖不完整");
        started = dependencies.now();
        const auto deadline = started + kRunDuration;
        result.evidence = {{"reference_revision",reference.revision},{"verify_only",verify_only},
            {"samples",nlohmann::json::array()}};
        if (!device || !device->output_owner_exclusive() || device->status() != MouseStatus::READY ||
            config.mouse.backend != MouseBackend::KMBOX_NET || !config.mouse.allow_send_input)
            throw std::runtime_error("对齐需要现有独占KMBOX设备及有效输出许可");
        if (!verify_only && !permitted) throw std::runtime_error("缺少对齐任务输出许可回调");
        if (!std::isfinite(config.aim.counts_per_pixel_x) || config.aim.counts_per_pixel_x <= 0 ||
            !std::isfinite(config.aim.counts_per_pixel_y) || config.aim.counts_per_pixel_y <= 0 ||
            !std::isfinite(config.aim.max_counts_per_frame) || config.aim.max_counts_per_frame < 1 ||
            !std::isfinite(config.aim.control_delay_ms) || config.aim.control_delay_ms < 0 ||
            config.aim.control_delay_ms > 1000 || config.mouse.kmbox_command_timeout_ms <= 0 ||
            config.mouse.kmbox_command_timeout_ms > 1000)
            throw std::runtime_error("对齐所需的像素换算、限幅或延时参数无效");
        const auto reference_gray = reference_roi(reference);
        const auto& g = reference.geometry;
        const cv::Point2d center((g.source_width*0.5-g.roi_x)/g.scale_x,
                                 (g.source_height*0.5-g.roi_y)/g.scale_y);
        if (!std::isfinite(reference.target.x) || !std::isfinite(reference.target.y) ||
            center.x < 0 || center.y < 0 || center.x >= 320 || center.y >= 320 ||
            reference.target.x < 0 || reference.target.y < 0 || reference.target.x >= 320 || reference.target.y >= 320)
            throw std::runtime_error("当前ROI没有包含有效视轴或参考目标点");
        source_context::SourceContextSnapshot initial_source;
        InputReportCursor motion_cursor;
        InputReportBatch baseline;
        const bool reports_available = device->read_movement_reports(motion_cursor,baseline) && baseline.subscribed;
        if (reports_available) motion_cursor = {baseline.epoch,baseline.final_sequence};
        const auto check_permission = [&]() {
            if (canceled.load() || (permitted && !permitted())) {
                result.canceled = true; throw std::runtime_error("对齐已取消或方案许可已撤回");
            }
            InputSnapshot input;
            if (!device->poll_input(input) || !input.state_valid || !device->output_owner_exclusive() ||
                device->status() != MouseStatus::READY)
                throw std::runtime_error("对齐期间设备输入失效");
            for (int key : config.keyboard.emergency_virtual_keys)
                if (key > 0 && key < 256 && input.virtual_keys[static_cast<std::size_t>(key)]) {
                    result.canceled = true;result.emergency_stop=true;
                    throw std::runtime_error("急停键取消对齐");
                }
            if (config.keyboard.grenade_assist_enabled)
                for (int key : config.keyboard.grenade_cancel_virtual_keys)
                    if (key>0 && key<256 && input.virtual_keys[static_cast<std::size_t>(key)]) {
                        result.canceled=true;throw std::runtime_error("道具取消键已停止本次对齐");
                    }
            for (int key : {0x57,0x41,0x53,0x44,0x20,0x01,0x02})
                if (input.virtual_keys[static_cast<std::size_t>(key)]) {
                    result.canceled = true; throw std::runtime_error("检测到手动移动或按钮输入，对齐已取消");
                }
            if (reports_available) {
                InputReportBatch events;
                if (!device->read_movement_reports(motion_cursor,events) || !events.subscribed || events.gap || events.dropped_count)
                    throw std::runtime_error("物理鼠标监听发生缺口，对齐已停止");
                for (std::size_t index=0;index<events.count;++index) {
                    const auto& event=events.events[index];
                    if (event.physical_source_verified && event.motion_semantics==InputMotionSemantics::RELATIVE_COUNTS) {
                        physical_motion_observed = true;
                        if (event.raw_x || event.raw_y || event.raw_wheel || event.wasd_mask || event.mouse_buttons) {
                            result.canceled=true; throw std::runtime_error("手动键鼠输入取消对齐");
                        }
                    }
                }
            }
            const auto source = dependencies.source();
            if (!source.available || !source.focused || source.session_id==0)
                throw std::runtime_error("源端游戏未聚焦或焦点状态已失效");
            if (initial_source.session_id == 0) initial_source=source;
            if (source.session_id != initial_source.session_id ||
                source.control_safety_epoch != initial_source.control_safety_epoch)
                throw std::runtime_error("源端焦点会话已变化，对齐已停止");
        };
        // 焦点旁路连接可异步建立；只在尚未采图输出的起点短时等候。
        while (!dependencies.source().available && dependencies.now() < started+500ms && !canceled.load())
            dependencies.sleep(5ms);
        check_permission();
        auto capture_config=config.capture;
        if (capture_config.enable_d3d11_cuda_interop || capture_config.enable_d3d11_directml_interop)
            throw std::runtime_error("道具对齐需要CPU图像，当前GPU专用采集配置不适用");
        // NDI发现和首次连接是异步过程；沿用源配置，由整次任务期限限制等待。
        capture_config.ndi_receive_timeout_ms=std::min(capture_config.ndi_receive_timeout_ms,50);
        capture_config.acquire_timeout_ms=std::min(capture_config.acquire_timeout_ms,50);
        capture_config.udp_read_timeout_ms=std::min(capture_config.udp_read_timeout_ms,50);
        auto capture=dependencies.create_capture(capture_config);
        if (!capture) throw std::runtime_error("无法创建道具采集器");
        CaptureCloser closer{capture.get()};
        if (!capture->open()) throw std::runtime_error("道具采集启动失败："+capture->last_error());
        std::uint64_t last_sequence=0, receiver_generation=0, source_session=0;
        std::int64_t last_source_timestamp=0;
        auto last_source_time=Clock::time_point{}, last_fresh=dependencies.now(), stable_since=Clock::time_point{};
        auto observe_after=started;
        double previous_error=std::numeric_limits<double>::infinity();
        double best_error=std::numeric_limits<double>::infinity();
        auto last_progress=dependencies.now();
        int stable_frames=0;
        while (dependencies.now()<deadline) {
            check_permission();
            CapturedFrame frame;
            const auto status=capture->grab(frame);
            if (status!=CaptureStatus::FRAME && status!=CaptureStatus::NO_FRAME && status!=CaptureStatus::READY)
                throw std::runtime_error("道具采集断流或失败："+capture->last_error());
            const auto now=dependencies.now();
            if (status!=CaptureStatus::FRAME || frame.timing.sequence<=last_sequence) {
                if (last_sequence && now-last_fresh>kNoFrameDuration)
                    throw std::runtime_error("未收到新的道具画面，对齐已停止");
                dependencies.sleep(2ms); continue;
            }
            if (!geometry_matches(reference,frame)) throw std::runtime_error("当前画面与参考图的ROI或分辨率不一致");
            const auto& timing=frame.timing;
            if (frames && timing.receiver_generation!=receiver_generation)
                throw std::runtime_error("采集器已重连，对齐已停止");
            receiver_generation=timing.receiver_generation;
            last_sequence=timing.sequence;
            const bool network=config.capture.backend!=CaptureBackend::DESKTOP_DUPLICATION;
            if (network && (!timing.source_time_timing_valid || timing.source_clock_status!=SourceClockStatus::VALID ||
                !timing.source_timestamp_valid || timing.source_clock_session_id==0 ||
                !std::isfinite(timing.source_clock_uncertainty_ms) || timing.source_clock_uncertainty_ms<0 ||
                timing.source_clock_uncertainty_ms>50)) {
                // 每个短时NDI接收器的mapper需要三次真实时钟交换。暖机只等候，
                // 不产生匹配/稳定帧/鼠标命令；一旦消费过有效帧则不允许降级重暖。
                if (!frames && now<deadline && !config.capture.ndi_clock_sync_url.empty() &&
                    (timing.source_clock_status==SourceClockStatus::WARMING ||
                     timing.source_clock_status==SourceClockStatus::UNSYNCHRONIZED)) {
                    last_fresh=now;dependencies.sleep(5ms);continue;
                }
                throw std::runtime_error("网络画面缺少可信源时钟，请先恢复现有NDI时钟同步");
            }
            const auto observed_at=network?timing.source_time_at:timing.captured_at;
            const auto uncertainty=network?std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double,std::milli>(timing.source_clock_uncertainty_ms)):Clock::duration::zero();
            if (timing.captured_at==Clock::time_point{} || timing.captured_at>now || now-timing.captured_at>kFrameAge ||
                observed_at==Clock::time_point{} || observed_at>now || now-observed_at+uncertainty>kFrameAge)
                throw std::runtime_error("道具画面已陈旧，不能用于鼠标输出");
            if (network && source_session && source_session!=timing.source_clock_session_id)
                throw std::runtime_error("画面源时钟会话已变化");
            source_session=timing.source_clock_session_id;
            if (observed_at<=last_source_time || observed_at-uncertainty<=observe_after ||
                (network && timing.source_timestamp<=last_source_timestamp)) {
                if (now-last_fresh>kNoFrameDuration) throw std::runtime_error("源画面没有推进，对齐已停止");
                dependencies.sleep(2ms);continue;
            }
            last_source_time=observed_at;last_source_timestamp=timing.source_timestamp;last_fresh=now;++frames;
            const auto measured=match(reference_gray,frame.bgr,center);
            if (!measured.valid) throw std::runtime_error(measured.error);
            const auto error=reference.target+measured.shift-center;
            const double normalized=std::max(std::abs(error.x)/320.0,std::abs(error.y)/320.0);
            auto& samples=result.evidence["samples"];
            if (samples.size()<128) samples.push_back({{"sequence",timing.sequence},{"error_x",error.x},
                {"error_y",error.y},{"error_roi_ratio",normalized},{"score",measured.score},
                {"patches",measured.patches},{"source_timing_valid",timing.source_time_timing_valid}});
            check_permission();
            if (dependencies.now()>=deadline) break;
            if (normalized<=kAlignedRatio) {
                if (stable_frames++==0) stable_since=observed_at;
                if (stable_frames>=3 && observed_at-stable_since>=kStableDuration) {
                    result.aligned=true;result.verified_at=observed_at;break;
                }
                dependencies.sleep(2ms);continue;
            }
            stable_frames=0;stable_since={};
            if (verify_only) throw std::runtime_error("画面已离开参考瞄点，请再次按上键对齐");
            if (normalized>previous_error+0.025)
                throw std::runtime_error("对齐误差反向增大，请核对鼠标换算或手动移动");
            previous_error=normalized;
            if (normalized<best_error-0.5/320.0) {best_error=normalized;last_progress=now;}
            if (commands && now-last_progress>500ms)
                throw std::runtime_error("鼠标输出后未观察到持续接近瞄点，已停止");
            if (commands>=48) throw std::runtime_error("本次对齐已达到步数上限");
            const auto remaining=deadline-dependencies.now();
            if (remaining<std::chrono::milliseconds(config.mouse.kmbox_command_timeout_ms+100)) break;
            double dx=error.x*g.scale_x*config.aim.counts_per_pixel_x*0.5;
            double dy=error.y*g.scale_y*config.aim.counts_per_pixel_y*0.5;
            const double limit=std::floor(config.aim.max_counts_per_frame);
            const double norm=std::hypot(dx,dy);
            if (norm>limit) { dx*=limit/norm;dy*=limit/norm; }
            if (!std::isfinite(dx) || !std::isfinite(dy) ||
                std::abs(dx)>std::numeric_limits<int>::max()-1.0 ||
                std::abs(dy)>std::numeric_limits<int>::max()-1.0)
                throw std::runtime_error("对齐鼠标换算超出整数范围");
            int ix=static_cast<int>(std::round(dx)),iy=static_cast<int>(std::round(dy));
            if (ix==0 && iy==0) {
                if (std::abs(error.x)>=std::abs(error.y)) ix=error.x>0?1:-1;
                else iy=error.y>0?1:-1;
            }
            while (std::hypot(ix,iy)>limit) {
                if (std::abs(ix)>=std::abs(iy)) ix+=ix>0?-1:1;
                else iy+=iy>0?-1:1;
            }
            check_permission();
            const auto receipt=device->move({ix,iy});++commands;
            if (!receipt.succeeded) throw std::runtime_error("对齐鼠标命令未确认，已停止重试");
            // ACK不是屏幕效果；后续只能消费命令完成及配置响应延时之后的新源帧。
            observe_after=dependencies.now()+std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double,std::milli>(config.aim.control_delay_ms));
            dependencies.sleep(2ms);
        }
        if (!result.aligned) result.error="本次对齐超时，请靠近参考点后重试";
    } catch (const cv::Exception&) { result.error="道具图像匹配失败，未继续输出"; }
      catch (const std::exception& error) { result.error=error.what(); }
      catch (...) { result.error="道具对齐发生未知错误，已停止"; }
    try {
        result.evidence["aligned"]=result.aligned;result.evidence["canceled"]=result.canceled;
        result.evidence["emergency_stop"]=result.emergency_stop;
        result.evidence["error"]=result.error;result.evidence["frames"]=frames;
        result.evidence["commands"]=commands;
        if (result.aligned) result.evidence["verified_at_steady_ns"]=
            std::chrono::duration_cast<std::chrono::nanoseconds>(result.verified_at.time_since_epoch()).count();
        result.evidence["physical_motion_monitor_verified"]=physical_motion_observed;
        if (dependencies.now) result.evidence["elapsed_ms"]=std::chrono::duration<double,std::milli>(dependencies.now()-started).count();
    } catch (...) {}
    return result;
}

Result run(const Reference& reference, const AppConfig& config,
           std::shared_ptr<IMouseController> device,std::atomic_bool& canceled,
           bool verify_only,std::function<bool()> permitted,SharedStream stream) noexcept {
    try {
        Log::register_module("grenade_alignment",LogLevel::INFO);
        if (stream.grab && stream.source) {
            detail::Dependencies dependencies{
                [&](const CaptureConfig&){return std::make_unique<SharedCapture>(stream.grab);},
                []{return Clock::now();},
                [](std::chrono::milliseconds duration){std::this_thread::sleep_for(duration);},
                stream.source};
            return detail::run_with_dependencies(reference,config,std::move(device),canceled,
                verify_only,std::move(permitted),dependencies);
        }
        source_context::SourceContextClient source;
        auto source_config=config.source_context;
        char* token=nullptr;std::size_t length=0;
        if (_dupenv_s(&token,&length,"XEN_SOURCE_CONTEXT_TOKEN")==0 && token) {
            source_config.token=token;std::free(token);
        }
        if (!source_config.enabled || !source.start(source_config))
            return {false,false,"源端游戏焦点接收器未配置或启动失败",nlohmann::json::object()};
        detail::Dependencies dependencies{
            [](const CaptureConfig& value){return create_capture(value);},
            []{return Clock::now();},
            [](std::chrono::milliseconds duration){std::this_thread::sleep_for(duration);},
            [&]{return source.snapshot();}};
        auto result=detail::run_with_dependencies(reference,config,std::move(device),canceled,
            verify_only,std::move(permitted),dependencies);
        LOG_INFO("grenade_alignment","道具对齐结束 aligned={} canceled={} verify_only={}",
            result.aligned,result.canceled,verify_only);
        return result;
    } catch (...) { return {false,false,"道具对齐初始化失败",nlohmann::json::object()}; }
}
}
