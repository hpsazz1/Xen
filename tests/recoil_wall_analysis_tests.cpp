#include "recoil_tuner/wall_capture_analysis.h"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <cmath>
#include <chrono>
using namespace recoil_tuner;
namespace {
int failures=0;
void expect(bool ok,const char* message){if(!ok){++failures;std::cerr<<message<<'\n';}}
WallCaptureRequest request(){
 WallCaptureRequest r;r.environment_fingerprint="synthetic";r.profile_id="wall";r.weapon_id="ak47";r.source_hash=std::string(64,'a');r.sensitivity=1;
 r.image.reference={150,100};r.image.reference_confirmed=true;r.image.registration_roi={10,10,64,64};r.image.measurement_roi={90,40,130,150};
 r.calibration={{{10,0},{20,0},true,"x"},{{0,10},{0,-30},true,"y"}};return r;
}
std::vector<WallFrame> frames(){
 cv::Mat base(240,260,CV_8UC1,cv::Scalar(200));cv::RNG random(42);random.fill(base(cv::Rect(10,10,64,64)),cv::RNG::UNIFORM,40,220);
 auto a=base.clone();cv::circle(a,{160,85},2,cv::Scalar(0),-1);auto b=a.clone();cv::circle(b,{170,70},2,cv::Scalar(0),-1);
 return {{0,base},{20,base.clone()},{40,a},{60,a.clone()},{80,b}};
}
}
int main(){
 auto r=request();auto f=frames();auto result=analyze_wall_capture(f,r);
 expect(result.valid&&result.candidate.has_value(),"无既有曲线也能生成候选");
 if(result.candidate){auto& p=*result.candidate;expect(p.state==RecoilProfileState::SCHEMA_VALID&&!p.phase_tolerance_ms&&!p.recovery_ms&&p.calibration.evidence.empty(),"不伪造校准声明");
 expect(std::abs(p.points[1].x_counts-5)<0.05&&std::abs(p.points[1].y_counts-5)<0.05,"背景响应符号及两轴尺度");}
 expect(result.observations.size()==2&&result.observations[0].earliest_ms==20&&result.observations[0].first_visible_ms==40&&!result.shot_timing_available,"首次可见区间不等于射击时间");
 auto bad=r;bad.calibration[1].counts={20,0};expect(!analyze_wall_capture(f,bad).valid,"拒绝单轴标定");
 bad=r;bad.calibration[0].acknowledged=false;expect(!analyze_wall_capture(f,bad).valid,"拒绝无ACK标定");
 auto ambiguous=f;ambiguous[2].image=f[2].image.clone();cv::circle(ambiguous[2].image,{190,150},2,cv::Scalar(0),-1);expect(!analyze_wall_capture(ambiguous,r).valid,"同帧多新点拒绝归属");
 auto time=f;time[2].time_ms=20;expect(!analyze_wall_capture(time,r).valid,"拒绝非递增帧时间");
 auto flat=f;for(auto& frame:flat)frame.image=cv::Mat(240,260,CV_8UC1,cv::Scalar(200));expect(!analyze_wall_capture(flat,r).valid,"拒绝无纹理背景");
 Trial out,meta;std::string error;expect(!wall_measurement_to_trial(result,0,false,meta,out,error),"未确认测量不能直接变成Trial");
 auto camera_request=r;camera_request.mode=WallCaptureRequest::Mode::CAMERA_MOTION;
 // 配准图像在ROI外继续有纹理，避免把人为方块边界当作墙面相位信号。
 cv::Mat camera_base(240,260,CV_8UC1);cv::RNG camera_random(42);camera_random.fill(camera_base,cv::RNG::UNIFORM,40,220);
 cv::GaussianBlur(camera_base,camera_base,{5,5},1.0);
 cv::Mat moved;const cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,2,0,1,-3);
 cv::warpAffine(camera_base,moved,transform,camera_base.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
 auto camera=analyze_wall_capture({{0,camera_base},{40,moved}},camera_request);
 if(!camera.valid)std::cerr<<camera.message<<'\n';
 expect(camera.valid,"静态背景位移可生成视角补偿候选");
 if(camera.candidate)expect(std::abs(camera.candidate->points.back().x_counts+1)<0.1&&std::abs(camera.candidate->points.back().y_counts+1)<0.1,"camera必须抵消两轴背景位移而非弹孔偏移符号");
 cv::Mat wide(700,900,CV_8UC1);cv::RNG wide_random(71);wide_random.fill(wide,cv::RNG::UNIFORM,20,230);
 cv::GaussianBlur(wide,wide,{5,5},1.0);
 auto wide_request=camera_request;wide_request.image.registration_roi={150,150,400,400};
 wide_request.image.measurement_roi={}; // 视角测量与弹孔区域无关。
 wide_request.image.reference={450,350};
 cv::Mat wide_shift;const cv::Mat wide_transform=(cv::Mat_<double>(2,3)<<1,0,70,0,1,0);
 cv::warpAffine(wide,wide_shift,wide_transform,wide.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
 auto large=analyze_wall_capture({{0,wide},{40,wide_shift}},wide_request);
 expect(large.valid&&large.candidate,"大于64像素但仍在背景ROI30%内可配准");
 if(large.candidate)expect(std::abs(large.candidate->points.back().x_counts+35)<0.2,"大范围camera补偿尺度正确");
 const cv::Mat boundary_transform=(cv::Mat_<double>(2,3)<<1,0,130,0,1,0);
 cv::warpAffine(wide,wide_shift,boundary_transform,wide.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
 expect(!analyze_wall_capture({{0,wide},{40,wide_shift}},wide_request).valid,"超过ROI30%边缘拒绝外推");
 const auto directory=std::filesystem::temp_directory_path()/("xen-wall-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(directory);
 expect(save_wall_report(directory/"measurement.json",result,error),"报告独立保存");
 WallCaptureReport restored;expect(load_wall_report(directory/"measurement.json",restored,error)&&restored.valid&&restored.observations.size()==2,"报告重载保留测量来源及时域");
 expect(!save_wall_report(directory/"measurement.json",result,error),"不得覆盖原始测量");
 if(result.candidate){
  RecoilProfile base=*result.candidate;base.points={{0,0,0},{40,2,3}};
  std::vector<WallCaptureReport> fit,holdout;
  for(int i=0;i<5;++i){auto sample=result;sample.candidate->source.sha256=std::string(64,static_cast<char>('a'+i));(i<3?fit:holdout).push_back(sample);}
  WallOptimizationRequest tune;tune.measurements_confirmed=true;tune.locked_prefix_ms=40;
  auto optimized=optimize_wall_trials(base,fit,holdout,tune);
  expect(optimized.valid&&optimized.candidate&&optimized.candidate->points.back().time_ms==80,"后五发阶段可延长旧曲线时域");
  if(optimized.candidate){for(double time:{0.,10.,20.,39.,40.}){auto old=sample_recoil_profile(base,time),current=sample_recoil_profile(*optimized.candidate,time);expect(old.x_counts==current.x_counts&&old.y_counts==current.y_counts,"锁定前段节点与插值完全保持");}}
  std::vector<WallTrial> trials;for(int i=0;i<5;++i){WallTrial t;t.measurement=i<3?fit[i]:holdout[i-3];t.source_run="run-"+std::to_string(i);t.executed_profile_json=serialize_recoil_profile(base);t.confirmed=true;t.use=i<3?TrialUse::FIT:TrialUse::HOLDOUT;trials.push_back(t);}
  auto recorded=optimize_wall_trials_recorded(base,trials,tune,directory/"usage");expect(recorded.valid,"完整基线绑定与持久登记后可优化");
  expect(!optimize_wall_trials_recorded(base,trials,tune,directory/"usage").valid,"持久拒绝重新使用留出或拟合来源");
  tune.locked_prefix_ms=0;auto full=optimize_wall_trials(base,fit,holdout,tune);expect(full.valid&&full.candidate&&full.candidate->points[1].x_counts!=base.points[1].x_counts,"整段微调模式允许优化原前段");
  auto longer_base=base;longer_base.points.push_back({100,7,8});
  auto overlap=optimize_wall_trials(longer_base,fit,holdout,tune);
  expect(overlap.valid&&overlap.candidate,"稍短采集仍可优化共同观测区间");
  if(overlap.candidate){for(double time:{80.,85.,90.,100.}){const auto old=sample_recoil_profile(longer_base,time),current=sample_recoil_profile(*overlap.candidate,time);expect(std::abs(old.x_counts-current.x_counts)<1e-12&&std::abs(old.y_counts-current.y_counts)<1e-12,"未覆盖旧尾保持原值与插值，不外推测量");}}
  tune.locked_prefix_ms=80;expect(!optimize_wall_trials(longer_base,fit,holdout,tune).valid,"共同观测没超过前段时要求更长采集");tune.locked_prefix_ms=0;
  holdout[0].candidate->source.sha256=fit[0].candidate->source.sha256;expect(!optimize_wall_trials(base,fit,holdout,tune).valid,"留出不允许重复拟合来源");
 }
 std::filesystem::remove_all(directory);
 return failures?1:0;
}
