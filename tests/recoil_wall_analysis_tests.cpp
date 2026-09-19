#include "recoil_tuner/wall_capture_analysis.h"
#include "recoil_tuner/wall_registration_internal.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <nlohmann/json.hpp>
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
int main(int argc,char** argv){
 if(argc==3&&std::string(argv[1])=="--registration-replay"){
  const std::filesystem::path path=argv[2];std::ifstream input(path);nlohmann::json report;input>>report;
  const auto before=cv::imread((path.parent_path()/report.at("frames").at(0).at("file").get<std::string>()).string());
  nlohmann::json rows=nlohmann::json::array();
  const auto inspect=[&](const std::string& file){
   const auto after=cv::imread((path.parent_path()/file).string());
   const auto r=detail::register_wall(before,after,{40,40,80,80});
   rows.push_back({{"file",file},{"failure",r.failure},{"shift",{r.shift.x,r.shift.y}},
    {"response",r.response},{"residual",r.residual},{"peak_separation",r.peak_separation}});
  };
  for(std::size_t i=1;i<report.at("frames").size();++i)inspect(report.at("frames").at(i).at("file"));
  if(report.contains("registration_failure")&&report.at("registration_failure").contains("image_file"))
   inspect(report.at("registration_failure").at("image_file"));
  std::cout<<rows.dump(2)<<'\n';return 0;
 }
 if(argc==2&&std::string(argv[1])=="--registration-benchmark"){
  const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures/recoil_registration";
  const auto a=cv::imread((fixtures/"reference.png").string()),b=cv::imread((fixtures/"shot-b.png").string());
  if(a.empty()||b.empty())return 1;
  for(bool legacy:{true,false}){
   std::vector<double> times;int rejected=0;
   for(int i=-20;i<200;++i){
    const auto start=std::chrono::steady_clock::now();bool valid=false;
    if(legacy){
     cv::Mat af,bf,window;cv::cvtColor(a(cv::Rect(40,40,80,80)),af,cv::COLOR_BGR2GRAY);
     cv::cvtColor(b(cv::Rect(40,40,80,80)),bf,cv::COLOR_BGR2GRAY);
     af.convertTo(af,CV_64F);bf.convertTo(bf,CV_64F);cv::createHanningWindow(window,af.size(),CV_64F);
     double response=0;const auto shift=cv::phaseCorrelate(af,bf,window,&response);
     valid=response>=0.5&&std::abs(shift.x)<=24&&std::abs(shift.y)<=24;
    }else valid=detail::register_wall(a,b,{40,40,80,80}).failure.empty();
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(i>=0){times.push_back(elapsed);if(!valid)++rejected;}
   }
   std::sort(times.begin(),times.end());
   std::cout<<(legacy?"legacy":"bounded")<<" n=200 rejected="<<rejected<<" mean_ms="
    <<std::accumulate(times.begin(),times.end(),0.0)/times.size()<<" p50="<<times[99]
    <<" p95="<<times[189]<<" p99="<<times[197]<<" max="<<times.back()<<'\n';
  }
  return 0;
 }

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
 // 真实失败帧必须通过生产分析接口复现，不用无射击标定帧代替。
 const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures/recoil_registration";
 const auto shot_reference=cv::imread((fixtures/"reference.png").string());
 auto shot_request=camera_request;shot_request.image.registration_roi={40,40,80,80};
 shot_request.image.reference={160,160};
 for(const auto* name:{"shot-a.png","shot-b.png"}){
  const auto shot=cv::imread((fixtures/name).string());
  const auto reference=std::string(name)=="shot-a.png"?cv::imread((fixtures/"reference-a.png").string()):shot_reference;
  auto observed=analyze_wall_capture({{0,reference},{120,shot}},shot_request);
  if(!observed.valid)std::cerr<<name<<": "<<observed.message<<'\n';
  expect(observed.valid,"真实开枪帧在原ROI30%范围内必须能匹配对应背景");
  if(observed.valid)expect(std::abs(observed.observations.back().center.x-163.5)<0.6&&
   std::abs(observed.observations.back().center.y-183.3)<0.6,"真实图像对应文字位移必须一致，不能仅提高响应");
 }
 const auto preceding=cv::imread((fixtures/"shot-before-a.png").string());
 const auto preceding_reference=cv::imread((fixtures/"reference-a.png").string());
 expect(analyze_wall_capture({{0,preceding_reference},{100,preceding}},shot_request).valid,
  "真实失败前段图也须通过，不能只修最后一帧的相关响应");
 // 对错误对应保持拒绝：高频多解、无关图像、局部遮挡和真实边界。
 cv::Mat periodic(320,320,CV_8UC1);
 for(int y=0;y<periodic.rows;++y)for(int x=0;x<periodic.cols;++x)
  periodic.at<unsigned char>(y,x)=static_cast<unsigned char>(((x/4+y/4)%2)*180+30);
 expect(!analyze_wall_capture({{0,periodic},{40,periodic.clone()}},shot_request).valid,"周期多解不能因相位响应高而放行");
 cv::Mat tiled(320,320,CV_8UC1),tile(16,16,CV_32F);cv::RNG tiled_random(33);
 tiled_random.fill(tile,cv::RNG::UNIFORM,-30,30);
 for(int y=0;y<tiled.rows;++y){const double row_noise=tiled_random.gaussian(2);
  for(int x=0;x<tiled.cols;++x)tiled.at<unsigned char>(y,x)=cv::saturate_cast<unsigned char>(
   tile.at<float>(y%16,x%16)+0.7*x+row_noise+tiled_random.gaussian(0.1)+30);}
 expect(!analyze_wall_capture({{0,tiled},{40,tiled.clone()}},shot_request).valid,
  "最强竞争峰残差不通过也不能漏掉后续满足残差的周期异解");
 cv::Mat unrelated(camera_base.size(),camera_base.type());camera_random.fill(unrelated,cv::RNG::UNIFORM,0,255);
 expect(!analyze_wall_capture({{0,camera_base},{40,unrelated}},camera_request).valid,"独立随机画面不得作为背景平移");
 auto occluded=moved.clone();occluded(camera_request.image.registration_roi).setTo(128);
 expect(!analyze_wall_capture({{0,camera_base},{40,occluded}},camera_request).valid,"背景被遮挡必须停止");
 auto edge_request=camera_request;edge_request.image.registration_roi={0,0,64,64};
 expect(!analyze_wall_capture({{0,camera_base},{40,moved}},edge_request).valid,"中点两侧插值不得借边缘复制补造内容");
 const auto unmodified=shot_reference.clone();
 (void)analyze_wall_capture({{0,shot_reference},{40,shot_reference}},shot_request);
 expect(cv::norm(shot_reference,unmodified,cv::NORM_INF)==0,"配准不得原地修改输入图像");
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
