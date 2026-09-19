#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include "recoil_tuner/wall_capture_analysis.h"
#include "recoil_tuner/wall_registration_internal.h"
#include "recoil/recoil_calibration.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <limits>
#include <fstream>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

namespace recoil_tuner {
namespace {
bool finite(double x) { return std::isfinite(x); }
bool well_conditioned(const cv::Mat& m, double limit) {
    cv::Mat w; cv::SVD::compute(m, w);
    return w.total() == 2 && finite(w.at<double>(0)) && finite(w.at<double>(1)) &&
        w.at<double>(1) > 1e-9 && w.at<double>(0) / w.at<double>(1) <= limit;
}
bool camera_translation(const cv::Mat& before, const cv::Mat& after, const cv::Rect& roi,
    cv::Point2d& translation, std::string& error) {
    const auto registration=detail::register_wall(before,after,roi);
    translation=registration.shift;
    if(!registration.failure.empty()) { error="背景配准失败："+registration.failure; return false; }
    return true;
}
}
namespace detail {
WallRegistration register_wall(const cv::Mat& before, const cv::Mat& after,
        const cv::Rect& roi, cv::Size search_limit) {
    WallRegistration result;
    const auto reject=[&](const char* reason){result.failure=reason;return result;};
    if(before.empty() || before.size()!=after.size() || before.type()!=after.type() ||
        before.depth()!=CV_8U || (before.channels()!=1&&before.channels()!=3&&before.channels()!=4) ||
        before.total()>33554432 || roi.width<32 || roi.height<32 || roi.x<0 || roi.y<0 ||
        static_cast<std::int64_t>(roi.x)+roi.width>before.cols ||
        static_cast<std::int64_t>(roi.y)+roi.height>before.rows)
        return reject("invalid_registration_geometry");
    const auto gray=[](const cv::Mat& image){
        cv::Mat value;
        if(image.channels()==1)value=image;
        else cv::cvtColor(image,value,image.channels()==3?cv::COLOR_BGR2GRAY:cv::COLOR_BGRA2GRAY);
        return value;
    };
    const auto a=gray(before),b=gray(after);
    cv::Scalar mean,deviation;cv::meanStdDev(a(roi),mean,deviation);
    result.texture_stddev=deviation[0];
    if(result.texture_stddev<5)return reject("insufficient_texture");
    const double limit_x=search_limit.width>0?search_limit.width:roi.width*0.30;
    const double limit_y=search_limit.height>0?search_limit.height:roi.height*0.30;
    const int left=std::max(0,roi.x-static_cast<int>(std::ceil(limit_x)));
    const int top=std::max(0,roi.y-static_cast<int>(std::ceil(limit_y)));
    const int right=std::min(a.cols,roi.x+roi.width+static_cast<int>(std::ceil(limit_x)));
    const int bottom=std::min(a.rows,roi.y+roi.height+static_cast<int>(std::ceil(limit_y)));
    cv::Mat scores;
    cv::matchTemplate(b(cv::Rect(left,top,right-left,bottom-top)),a(roi),scores,cv::TM_CCOEFF_NORMED);
    cv::Point peak;double best_score=0;cv::minMaxLoc(scores,nullptr,&best_score,nullptr,&peak);
    // 5×5对应相位质心邻域；其外最强峰约束全部竞争位置，不只复核第二峰的残差。
    const auto neighborhood=cv::Rect(peak.x-2,peak.y-2,5,5)&cv::Rect(0,0,scores.cols,scores.rows);
    scores(neighborhood).setTo(-2);
    double second_score=-2;cv::minMaxLoc(scores,nullptr,&second_score);
    const double separation=best_score-second_score;
    // NCC分离度是独立歧义契约，不是phase response；0.1由真实帧和周期负例验证。
    if(second_score>-2 && separation<0.1){
        result.template_score=best_score;result.peak_separation=separation;
        return reject("ambiguous_registration");
    }
    cv::Mat af,window;a(roi).convertTo(af,CV_64F);
    cv::createHanningWindow(window,roi.size(),CV_64F);
    const auto evaluate=[&](cv::Point location){
        WallRegistration candidate;candidate.texture_stddev=result.texture_stddev;
        cv::Mat bf;b(cv::Rect(left+location.x,top+location.y,roi.width,roi.height)).convertTo(bf,CV_64F);
        // OpenCV加窗可能原地修改浮点输入；每个候选独立持有参考副本。
        auto reference=af.clone();
        const auto fine=cv::phaseCorrelate(reference,bf,window,&candidate.response);
        candidate.shift={left+location.x-roi.x+fine.x,top+location.y-roi.y+fine.y};
        if(!finite(candidate.shift.x)||!finite(candidate.shift.y)||!finite(candidate.response))
            candidate.failure="nonfinite_registration";
        else if(candidate.response<0.5)candidate.failure="unreliable_registration";
        else if(std::abs(candidate.shift.x)>limit_x || std::abs(candidate.shift.y)>limit_y)
            candidate.failure="registration_range_exceeded";
        if(!candidate.failure.empty())return candidate;
        const cv::Point2f center(roi.x+(roi.width-1)*0.5f,roi.y+(roi.height-1)*0.5f);
        const auto inside=[&](cv::Point2f c){
            return c.x-(roi.width-1)*0.5>=0 && c.y-(roi.height-1)*0.5>=0 &&
                c.x+(roi.width-1)*0.5<=a.cols-1 && c.y+(roi.height-1)*0.5<=a.rows-1;
        };
        const auto residual_at=[&](cv::Point2d shift){
            const cv::Point2f half(static_cast<float>(shift.x*0.5),static_cast<float>(shift.y*0.5));
            if(std::abs(shift.x)>limit_x || std::abs(shift.y)>limit_y ||
                !inside(center-half)||!inside(center+half) ||
                roi.x+shift.x<0 || roi.y+shift.y<0 ||
                roi.x+roi.width+shift.x>a.cols || roi.y+roi.height+shift.y>a.rows)
                return std::numeric_limits<double>::infinity();
            // 两图都在中点空间插值，避免仅模糊当前图再与锐利参考图比较。
            cv::Mat first,second,difference;
            cv::getRectSubPix(a,roi.size(),center-half,first);
            cv::getRectSubPix(b,roi.size(),center+half,second);
            cv::absdiff(first,second,difference);return cv::mean(difference)[0];
        };
        candidate.residual=residual_at(candidate.shift);
        if(!finite(candidate.residual)){
            candidate.failure="invalid_registration_geometry";return candidate;
        }
        // 相位质心是初值；在每轴不到半像素邻域内最小化同一残差，不放宽8的验收阈值。
        for(double step:{0.25,0.125,0.0625,0.03125,0.015625}){
            const auto origin=candidate.shift;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
                const cv::Point2d refined=origin+cv::Point2d(x*step,y*step);
                const double residual=residual_at(refined);
                if(residual<candidate.residual){candidate.shift=refined;candidate.residual=residual;}
            }
        }
        if(candidate.residual>8)candidate.failure="inconsistent_registration";
        return candidate;
    };
    result=evaluate(peak);
    result.template_score=best_score;result.peak_separation=separation;
    return result;
}
}
bool fit_wall_calibration(const std::vector<WallCalibrationSample>& samples,double max_condition,
        std::array<double,4>& pixel_response,std::string& error) noexcept {
    pixel_response={};
    try {
        const auto reject=[&](const char* message){error=message;return false;};
        if(samples.size()<2||samples.size()>1000||!finite(max_condition)||max_condition<1||max_condition>20)
            return reject("标定样本数量或条件数限制无效。");
        cv::Mat counts(static_cast<int>(samples.size()),2,CV_64F),pixels(counts.size(),CV_64F);
        std::set<std::string> evidence;
        for(int i=0;i<counts.rows;++i){
            const auto& sample=samples[static_cast<std::size_t>(i)];
            if(!sample.acknowledged||sample.evidence_id.empty()||!evidence.insert(sample.evidence_id).second)
                return reject("标定缺少独立已确认回执及证据标识。");
            for(int j=0;j<2;++j){
                if(!finite(sample.counts[j])||!finite(sample.pixel_delta[j]))return reject("标定数值无效。");
                counts.at<double>(i,j)=sample.counts[j];pixels.at<double>(i,j)=sample.pixel_delta[j];
            }
        }
        if(!well_conditioned(counts,max_condition))return reject("标定输入缺少独立两轴激励。");
        cv::Mat transpose_response;
        if(!cv::solve(counts,pixels,transpose_response,cv::DECOMP_SVD))return reject("标定解算失败。");
        cv::Mat response=transpose_response.t();
        if(!well_conditioned(response,max_condition))return reject("像素响应秩不足或病态，不能生成counts。");
        // 同一线性几何响应应解释正反位移。归一化残差不超过15%，不使用速度判据。
        const double signal=cv::norm(pixels,cv::NORM_L2);
        const double residual=cv::norm(counts*transpose_response-pixels,cv::NORM_L2);
        if(!finite(signal)||!finite(residual)||signal<=0||residual/signal>0.15)
            return reject("正反标定响应不一致，请保持固定墙面与鼠标静止后重新标定。");
        for(int i=0;i<4;++i)pixel_response[i]=response.at<double>(i/2,i%2);
        error.clear();return true;
    }catch(...){error="标定拟合失败。";return false;}
}
WallCaptureReport analyze_wall_capture(const std::vector<WallFrame>& frames, const WallCaptureRequest& request) noexcept {
    WallCaptureReport result;
    result.environment_fingerprint = request.environment_fingerprint;
    result.reference = request.image.reference;
    result.mode = request.mode;
    try {
        auto reject = [&](const char* text) { result.message = text; result.candidate.reset(); return result; };
        if (frames.size() < 2 || frames.size() > 10000 || frames.front().time_ms != 0 || request.calibration.size() < 2 ||
            request.calibration.size() > 1000 || request.environment_fingerprint.empty() || request.source_hash.size() != 64 ||
            !finite(request.sensitivity) || request.sensitivity <= 0 || !finite(request.max_condition) || request.max_condition < 1 ||
            !finite(request.matching_radius_pixels) || request.matching_radius_pixels <= 0 || request.matching_radius_pixels > 10)
            return reject("缺少有界时间序列、两轴标定或环境证据。");
        std::string fit_error;
        if(!fit_wall_calibration(request.calibration,request.max_condition,result.pixel_response,fit_error))return reject(fit_error.c_str());
        for(const auto& sample:request.calibration)result.calibration_evidence.push_back(sample.evidence_id);
        cv::Mat response=(cv::Mat_<double>(2,2)<<result.pixel_response[0],result.pixel_response[1],result.pixel_response[2],result.pixel_response[3]);
        const auto inverse = response.inv();
        RecoilProfile profile; profile.id = request.profile_id; profile.weapon_id = request.weapon_id;
        profile.state = RecoilProfileState::SCHEMA_VALID; profile.source.sha256 = request.source_hash;
        profile.source.source_unit = "registered_wall_pixels"; profile.source.conversion_revision = "wall_capture_v1";
        profile.calibration.sensitivity = request.sensitivity; profile.calibration.input_path = "kmbox_net";
        profile.points.push_back({0, 0, 0});
        for (std::size_t i = 1; i < frames.size(); ++i) {
            if (!finite(frames[i].time_ms) || frames[i].time_ms <= frames[i - 1].time_ms || frames[i].time_ms > 60000)
                return reject("帧时间必须有限、严格递增并以真实采集时间为准。");
            if (request.mode == WallCaptureRequest::Mode::CAMERA_MOTION) {
                cv::Point2d translation;std::string error;
                if (!camera_translation(frames.front().image,frames[i].image,request.image.registration_roi,translation,error))
                    return reject(error.c_str());
                WallObservation o; o.center = result.reference + translation;
                o.earliest_ms = frames[i - 1].time_ms; o.first_visible_ms = frames[i].time_ms;
                const cv::Mat displacement = (cv::Mat_<double>(2, 1) << translation.x, translation.y);
                const cv::Mat compensation = -inverse * displacement;
                o.cumulative_counts = {compensation.at<double>(0), compensation.at<double>(1)};
                result.observations.push_back(o);
                profile.points.push_back({o.first_visible_ms, o.cumulative_counts[0], o.cumulative_counts[1]});
                continue;
            }
            const auto measured = measure_image_pair(frames.front().image, frames[i].image, request.image);
            if (!measured.valid && !measured.empty_detection)
                return reject(measured.message.c_str());
            if (!measured.valid && !result.observations.empty()) return reject("已有暗斑消失或候选过多，时间归属不可靠。");
            if (!measured.valid) continue;
            std::vector<bool> matched(result.observations.size(), false);
            std::vector<cv::Point2d> added;
            for (const auto& center : measured.candidate_centers) {
                std::size_t matches = 0, index = 0;
                for (std::size_t k = 0; k < result.observations.size(); ++k)
                    if (cv::norm(center - result.observations[k].center) <= request.matching_radius_pixels) { ++matches; index = k; }
                if (matches > 1 || (matches == 1 && matched[index])) return reject("弹孔对应存在歧义。");
                if (matches == 1) matched[index] = true; else added.push_back(center);
            }
            if (std::find(matched.begin(), matched.end(), false) != matched.end()) return reject("已有弹孔消失或合并，不能确定首次出现顺序。");
            if (added.size() > 1) return reject("同一帧出现多个新暗斑，不能推断逐点顺序。");
            if (added.empty()) continue;
            WallObservation o; o.center = added.front(); o.earliest_ms = frames[i - 1].time_ms; o.first_visible_ms = frames[i].time_ms;
            const cv::Mat offset = (cv::Mat_<double>(2, 1) << o.center.x - request.image.reference.x, o.center.y - request.image.reference.y);
            const cv::Mat compensation = inverse * offset;
            o.cumulative_counts = {compensation.at<double>(0), compensation.at<double>(1)};
            result.observations.push_back(o);
            profile.points.push_back({o.first_visible_ms, o.cumulative_counts[0], o.cumulative_counts[1]});
        }
        std::string error;
        if (!validate_recoil_profile(profile, error)) return reject(error.c_str());
        result.candidate = std::move(profile); result.valid = true;
        result.message = request.mode == WallCaptureRequest::Mode::CAMERA_MOTION ?
            "已生成待核对的视角补偿候选，非弹着校准；不能把准星动画当作弹道。" :
            "已生成待人工核对的SCHEMA_VALID候选；节点为首次可见时间，不代表真实逐发时间或已校准。";
    } catch (...) { result.valid = false; result.candidate.reset(); result.message = "对墙图像分析失败。"; }
    return result;
}
bool wall_measurement_to_trial(const WallCaptureReport& report, std::size_t index, bool confirmed,
    const Trial& metadata, Trial& output, std::string& error) noexcept {
    try {
        if (!report.valid || index >= report.observations.size() || !confirmed || !metadata.reference_confirmed ||
            !metadata.completed || !metadata.independent_recoil || !metadata.timing_valid || metadata.id.empty() ||
            metadata.content_hash.empty() || metadata.firing_id.empty() || metadata.source_run.empty() ||
            metadata.executed_profile_revision.empty() || metadata.environment_fingerprint != report.environment_fingerprint ||
            !finite(metadata.measurement_ms) || !finite(metadata.timing_uncertainty_ms) || metadata.timing_uncertainty_ms < 0 ||
            metadata.receipts.empty()) { error = "缺少人工点确认、真实Run、独立时序或执行回执。"; return false; }
        const auto& o = report.observations[index];
        if (metadata.measurement_ms < o.first_visible_ms) { error = "端点测量时间早于暗斑首次可见时间。"; return false; }
        for (const auto& receipt : metadata.receipts) {
            if (receipt.state != ReceiptState::ACKNOWLEDGED || receipt.command_id == 0 ||
                !finite(receipt.completed_ms) || !finite(receipt.planned_ms) || receipt.completed_ms < receipt.planned_ms ||
                !finite(receipt.x_counts) || !finite(receipt.y_counts)) { error = "执行回执不完整或未确认。"; return false; }
        }
        Trial trial = metadata;
        trial.measurement_source = "wall_capture_confirmed_endpoint_v1";
        trial.residual = {o.center.x - report.reference.x, o.center.y - report.reference.y};
        output = std::move(trial); error.clear(); return true;
    } catch (...) { error = "测量转换失败。"; return false; }
}
WallCaptureReport optimize_wall_trials(const RecoilProfile& base, const std::vector<WallCaptureReport>& fit,
    const std::vector<WallCaptureReport>& holdout, const WallOptimizationRequest& request) noexcept {
    WallCaptureReport output;
    try {
        auto reject = [&](const char* reason) { output.message = reason; return output; };
        std::string error;
        if (!validate_recoil_profile(base, error) || fit.size() < 3 || holdout.size() < 2 || fit.size() + holdout.size() > 1000 ||
            !request.measurements_confirmed || !finite(request.max_axis_correction_counts) || request.max_axis_correction_counts <= 0 ||
            request.max_axis_correction_counts > 100 || !finite(request.min_relative_improvement) ||
            request.min_relative_improvement <= 0 || request.min_relative_improvement >= 1 ||
            !finite(request.locked_prefix_ms) || request.locked_prefix_ms < 0 || request.locked_prefix_ms > base.points.back().time_ms)
            return reject("需有效基线、至少三组拟合和两组独立验证、人工测量确认及有界修正预算。");
        output.environment_fingerprint = fit.front().environment_fingerprint;
        output.mode = fit.front().mode; output.pixel_response = fit.front().pixel_response;
        if (output.environment_fingerprint.empty()) return reject("缺少环境绑定。");
        cv::Mat response(2,2,CV_64F);
        for (int i = 0; i < 4; ++i) {
            if (!finite(output.pixel_response[i])) return reject("响应矩阵含非有限数值。");
            response.at<double>(i/2,i%2) = output.pixel_response[i];
        }
        if (!well_conditioned(response,20)) return reject("响应矩阵秩不足或病态。");
        std::set<std::string> hashes;
        for (const auto* collection : {&fit, &holdout}) for (const auto& r : *collection) {
            if (!r.valid || !r.candidate || r.mode != output.mode || r.environment_fingerprint != output.environment_fingerprint ||
                !validate_recoil_profile(*r.candidate, error) || r.candidate->source.sha256.empty() ||
                !hashes.insert(r.candidate->source.sha256).second || r.candidate->weapon_id != base.weapon_id ||
                r.candidate->calibration.sensitivity != base.calibration.sensitivity || r.pixel_response != output.pixel_response)
                return reject("采集重复、环境或标定不一致，或实际采集未覆盖基线时域。");
        }
        auto candidate = base; candidate.revision += 1; candidate.state = RecoilProfileState::SCHEMA_VALID;
        candidate.calibration.evidence.clear(); candidate.phase_tolerance_ms.reset(); candidate.recovery_ms.reset();
        double end_ms = 60000;
        for (const auto* collection : {&fit, &holdout}) for (const auto& r : *collection)
            end_ms = (std::min)(end_ms, r.candidate->points.back().time_ms);
        if (end_ms <= request.locked_prefix_ms) return reject("共同观测区间未超过已锁定前段，请延长本阶段采集。");
        const bool preserve_tail = end_ms < base.points.back().time_ms;
        std::set<double> grid;
        for (const auto& point : base.points) grid.insert(point.time_ms);
        grid.insert(request.locked_prefix_ms); grid.insert(end_ms);
        for (const auto& r : fit) for (const auto& point : r.candidate->points)
            if (point.time_ms > request.locked_prefix_ms && point.time_ms <= end_ms) grid.insert(point.time_ms);
        candidate.points.clear();
        for (const auto time : grid) { auto point = sample_recoil_profile(base,time); point.time_ms = time; candidate.points.push_back(point); }
        std::vector<std::array<double, 2>> corrections;
        for (auto& point : candidate.points) {
            std::array<double, 2> correction{};
            for (const auto& report : fit) {
                const auto residual = sample_recoil_profile(*report.candidate, point.time_ms);
                correction[0] += residual.x_counts / fit.size(); correction[1] += residual.y_counts / fit.size();
            }
            if (point.time_ms <= request.locked_prefix_ms || (preserve_tail && point.time_ms >= end_ms)) correction = {};
            for (auto& axis : correction) axis = std::clamp(axis, -request.max_axis_correction_counts, request.max_axis_correction_counts);
            point.x_counts += correction[0]; point.y_counts += correction[1]; corrections.push_back(correction);
        }
        double before = 0, after = 0;
        for (const auto& r : holdout) for (std::size_t i = 1; i < candidate.points.size(); ++i) {
            if (candidate.points[i].time_ms <= request.locked_prefix_ms || candidate.points[i].time_ms > end_ms) continue;
            const auto residual = sample_recoil_profile(*r.candidate, candidate.points[i].time_ms);
            const auto& h = r.pixel_response;
            const auto cost = [&](double x, double y) { return std::hypot(h[0] * x + h[1] * y, h[2] * x + h[3] * y); };
            before += cost(residual.x_counts, residual.y_counts);
            after += cost(residual.x_counts - corrections[i][0], residual.y_counts - corrections[i][1]);
        }
        if (before <= 1e-9 || after > before * (1 - request.min_relative_improvement))
            return reject("独立验证组的预测残差未改善，保留原曲线。");
        if (!validate_recoil_profile(candidate, error)) return reject(error.c_str());
        candidate.source.sha256 = recoil_calibration_sha256(nlohmann::json{{"base",serialize_recoil_profile(base)},
            {"measurement_hashes",hashes},{"locked_prefix_ms",request.locked_prefix_ms},{"common_end_ms",end_ms},
            {"max_axis_correction_counts",request.max_axis_correction_counts}}.dump());
        candidate.source.conversion_revision = "wall_time_series_optimization_v1";
        output.valid = true; output.candidate = std::move(candidate);
        output.message = "已用重复采集生成时域优化候选，独立组仅通过预测比较，仍需重新实测。";
        for (const auto& hash : hashes) output.calibration_evidence.push_back(hash);
    } catch (...) { output.valid = false; output.candidate.reset(); output.message = "时域优化失败。"; }
    return output;
}
bool save_wall_report(const std::filesystem::path& path, const WallCaptureReport& report, std::string& error) noexcept {
    try {
        if (std::filesystem::exists(path)) { error = "结果文件已存在，禁止覆盖原始证据。"; return false; }
        nlohmann::json j = {{"schema_version",1},{"valid",report.valid},{"message",report.message},
            {"mode",report.mode == WallCaptureRequest::Mode::CAMERA_MOTION ? "camera_motion" : "bullet_marks"},
            {"environment_fingerprint",report.environment_fingerprint},{"pixel_response",report.pixel_response},
            {"reference",{report.reference.x,report.reference.y}},{"calibration_evidence",report.calibration_evidence},
            {"requires_manual_confirmation",true},{"shot_timing_available",false},{"observations",nlohmann::json::array()}};
        if (report.candidate) j["candidate"] = nlohmann::json::parse(serialize_recoil_profile(*report.candidate));
        for (const auto& o : report.observations) j["observations"].push_back({{"center",{o.center.x,o.center.y}},
            {"earliest_ms",o.earliest_ms},{"first_visible_ms",o.first_visible_ms},{"cumulative_counts",o.cumulative_counts}});
        const auto text = j.dump(2);
        const HANDLE file = CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = "测量报告无法独占创建。"; return false; }
        DWORD written = 0;
        const bool okay = text.size() < MAXDWORD && WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) &&
            written == text.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!okay) { error = "测量报告写入失败。"; return false; } error.clear(); return true;
    } catch (...) { error = "测量报告保存失败。"; return false; }
}
WallCaptureReport optimize_wall_trials_recorded(const RecoilProfile& base, const std::vector<WallTrial>& trials,
    const WallOptimizationRequest& request, const std::filesystem::path& directory) noexcept {
    WallCaptureReport result;
    try {
        if (directory.empty() || trials.size() < 5 || trials.size() > 1000) { result.message = "缺少持久用途目录或独立采集组。"; return result; }
        std::vector<WallCaptureReport> fit, holdout;
        std::set<std::string> hashes, runs;
        const auto base_hash = recoil_calibration_sha256(serialize_recoil_profile(base));
        for (const auto& trial : trials) {
            RecoilProfile executed; std::string error;
            if (!trial.confirmed || trial.source_run.empty() || !trial.measurement.valid || !trial.measurement.candidate ||
                !runs.insert(trial.source_run).second || !hashes.insert(trial.measurement.candidate->source.sha256).second ||
                !load_recoil_profile(trial.executed_profile_json,executed,error) ||
                recoil_calibration_sha256(serialize_recoil_profile(executed)) != base_hash) {
                result.message = "采集未经确认、来源重复或实际执行曲线与优化基线不一致。"; return result;
            }
            (trial.use == TrialUse::FIT ? fit : holdout).push_back(trial.measurement);
        }
        if (fit.size() < 3 || holdout.size() < 2) { result.message = "至少需要三组拟合和两组独立验证。"; return result; }
        std::filesystem::create_directories(directory);
        // 原子目录占用在任何预测展示之前完成。中途失败保留占用，防止重试挑选留出结果。
        for (const auto& trial : trials) {
            const auto content = trial.measurement.candidate->source.sha256;
            for (const auto& identity : {std::string("content-") + recoil_calibration_sha256(content),
                std::string("run-") + recoil_calibration_sha256(trial.source_run)}) {
                const auto claim = directory / identity;
                if (!std::filesystem::create_directory(claim)) { result.message = "采集已用于先前优化，必须重新采集独立数据。"; return result; }
                std::ofstream file(claim / "usage.json",std::ios::binary);
                file << nlohmann::json({{"source_run",trial.source_run},{"content_hash",content},
                    {"executed_profile_hash",base_hash},{"use",trial.use == TrialUse::FIT ? "fit" : "holdout"}}).dump(2);
                file.flush(); if (!file) { result.message = "采集用途登记失败，未运行优化。"; return result; }
            }
        }
        return optimize_wall_trials(base,fit,holdout,request);
    } catch (...) { result.message = "持久用途登记失败，未展示预测结果。"; return result; }
}
bool load_wall_report(const std::filesystem::path& path, WallCaptureReport& report, std::string& error) noexcept {
    try {
        if (std::filesystem::file_size(path) > 16 * 1024 * 1024) { error = "测量报告过大。"; return false; }
        std::ifstream file(path, std::ios::binary); nlohmann::json j; file >> j;
        if (j.at("schema_version") != 1 || j.value("shot_timing_available",true) || !j.value("requires_manual_confirmation",false)) {
            error = "测量报告schema或证据声明无效。"; return false;
        }
        WallCaptureReport r; r.valid = j.at("valid").get<bool>(); r.message = j.at("message").get<std::string>();
        const auto mode = j.at("mode").get<std::string>();
        if (mode != "camera_motion" && mode != "bullet_marks") { error = "测量模式无效。"; return false; }
        r.mode = mode == "camera_motion" ? WallCaptureRequest::Mode::CAMERA_MOTION : WallCaptureRequest::Mode::BULLET_MARKS;
        r.environment_fingerprint = j.at("environment_fingerprint").get<std::string>();
        r.pixel_response = j.at("pixel_response").get<std::array<double,4>>();
        for (const auto v : r.pixel_response) if (!finite(v)) { error = "标定矩阵无效。"; return false; }
        r.reference = {j.at("reference").at(0).get<double>(),j.at("reference").at(1).get<double>()};
        if (!finite(r.reference.x) || !finite(r.reference.y)) { error = "参考锚点无效。"; return false; }
        r.calibration_evidence = j.at("calibration_evidence").get<std::vector<std::string>>();
        if (j.contains("candidate")) {
            RecoilProfile p; if (!load_recoil_profile(j.at("candidate").dump(),p,error)) return false;
            if (p.state != RecoilProfileState::SCHEMA_VALID || !p.calibration.evidence.empty() || p.phase_tolerance_ms || p.recovery_ms) {
                error = "图像候选不得声明已校准或真实相位。"; return false;
            }
            r.candidate = std::move(p);
        }
        double previous = -1;
        for (const auto& o : j.at("observations")) {
            WallObservation p; p.center = {o.at("center").at(0).get<double>(),o.at("center").at(1).get<double>()};
            p.earliest_ms = o.at("earliest_ms"); p.first_visible_ms = o.at("first_visible_ms");
            p.cumulative_counts = o.at("cumulative_counts").get<std::array<double,2>>();
            if (!finite(p.center.x) || !finite(p.center.y) || !finite(p.earliest_ms) || !finite(p.first_visible_ms) ||
                !finite(p.cumulative_counts[0]) || !finite(p.cumulative_counts[1]) || p.earliest_ms < 0 ||
                p.first_visible_ms <= p.earliest_ms || p.first_visible_ms <= previous || p.first_visible_ms > 60000 || r.observations.size() >= 10000) {
                error = "测量节点时间或数值无效。"; return false;
            }
            previous = p.first_visible_ms; r.observations.push_back(p);
        }
        if (r.valid && (!r.candidate || r.environment_fingerprint.empty())) { error = "有效报告缺少候选或环境。"; return false; }
        report = std::move(r); error.clear(); return true;
    } catch (...) { error = "测量报告读取失败。"; return false; }
}
}
