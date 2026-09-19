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
void follow_recoil_tests(){
 auto r=request();r.mode=WallCaptureRequest::Mode::FOLLOW_RECOIL;r.follow_recoil_mouse_invariance_confirmed=true;
 cv::Mat gray(240,260,CV_8UC1);cv::RNG rng(843);rng.fill(gray,cv::RNG::UNIFORM,40,220);
 cv::GaussianBlur(gray,gray,{5,5},1.0);cv::Mat wall;cv::cvtColor(gray,wall,cv::COLOR_GRAY2BGR);
 const auto frame=[&](cv::Point shift,cv::Point q){cv::Mat result;
  const cv::Mat transform=(cv::Mat_<double>(2,3)<<1,0,shift.x,0,1,shift.y);
  cv::warpAffine(wall,result,transform,wall.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
  cv::rectangle(result,cv::Rect(q.x-1,q.y-1,3,3),cv::Scalar(255,0,255),-1);return result;};
 auto first=frame({0,0},{150,100});auto second=frame({2,-3},{160,85});
 auto result=analyze_wall_capture({{0,first},{40,second}},r);
 expect(result.valid&&result.candidate.has_value(),"Follow Recoil原图生成候选");
 if(!result.valid)std::cerr<<result.message<<'\n';
 if(result.candidate)expect(std::abs(result.candidate->points.back().x_counts-4)<0.1&&std::abs(result.candidate->points.back().y_counts-4)<0.1,"Follow Recoil使用(q-q0)-b及正确H符号");
 const auto common=analyze_wall_capture({{0,first},{40,frame({2,-3},{152,97})}},r);
 expect(common.valid&&std::abs(common.candidate->points.back().x_counts)<0.1&&std::abs(common.candidate->points.back().y_counts)<0.1,"准星背景共同位移不能重复计入");
 const auto opposite=analyze_wall_capture({{0,first},{40,frame({-2,3},{140,115})}},r);
 expect(opposite.valid&&std::abs(opposite.candidate->points.back().x_counts+4)<0.1&&std::abs(opposite.candidate->points.back().y_counts+4)<0.1,"准星两个方向均正确");
 auto bad=r;bad.follow_recoil_mouse_invariance_confirmed=false;expect(!analyze_wall_capture({{0,first},{40,second}},bad).valid,"缺少Jq校核不能推导");
 cv::Point2d point;std::string error;
 auto duplicate=second.clone();cv::rectangle(duplicate,{200,180,3,3},{255,0,255},-1);
 expect(!locate_follow_recoil_crosshair(duplicate,point,error),"多个洋红点拒绝质心");
 expect(!locate_follow_recoil_crosshair(frame({0,0},{1,100}),point,error),"裁边准星拒绝");
 auto cross=wall.clone();cv::line(cross,{145,100},{155,100},{255,0,255},1);cv::line(cross,{150,95},{150,105},{255,0,255},1);
 expect(locate_follow_recoil_crosshair(cross,point,error)&&point==cv::Point2d(150,100),"完整短十字可定位");
 cross.at<cv::Vec3b>(95,150)={50,50,50};expect(!locate_follow_recoil_crosshair(cross,point,error),"缺臂拒绝");
 expect(!analyze_wall_capture({{0,first},{40,wall}},r).valid,"缺测帧不能沿用旧中心");
 bad=r;bad.image.registration_roi={120,70,64,64};expect(!analyze_wall_capture({{0,first},{40,second}},bad).valid,"背景ROI排除准星");
 auto path=std::filesystem::temp_directory_path()/("follow-recoil-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
 WallCaptureReport loaded;expect(save_wall_report(path,result,error)&&load_wall_report(path,loaded,error)&&loaded.mode==r.mode&&loaded.observations.size()==1&&loaded.observations[0].crosshair_center==cv::Point2d(160,85),"Follow Recoil模式与q/b报告可重载");
 std::filesystem::remove(path);
 if(result.candidate){
  auto base=*result.candidate;for(auto& p:base.points){p.x_counts=0;p.y_counts=0;}
  std::vector<WallCaptureReport> fit(3,result),holdout(2,result);
  for(int i=0;i<5;++i)(i<3?fit[i]:holdout[i-3]).candidate->source.sha256=std::string(64,static_cast<char>('a'+i));
  WallOptimizationRequest optimization;optimization.measurements_confirmed=true;
  const auto optimized=optimize_wall_trials(base,fit,holdout,optimization);
  expect(optimized.valid&&optimized.candidate&&std::abs(optimized.candidate->points.back().x_counts-4)<0.1&&std::abs(optimized.candidate->points.back().y_counts-4)<0.1,"Follow Recoil优化正向追加已转换counts，不能再次反号");
  holdout[0].mode=WallCaptureRequest::Mode::CAMERA_MOTION;
  expect(!optimize_wall_trials(base,fit,holdout,optimization).valid,"优化组拒绝混合camera和follow模式");
  Trial metadata,trial;metadata.id="id";metadata.content_hash="hash";metadata.firing_id="fire";metadata.source_run="run";
  metadata.executed_profile_revision="revision";metadata.environment_fingerprint=r.environment_fingerprint;
  metadata.completed=metadata.independent_recoil=metadata.timing_valid=metadata.reference_confirmed=true;
  metadata.measurement_ms=40;metadata.receipts.push_back({1,1,0,0,0,ReceiptState::ACKNOWLEDGED});
  expect(wall_measurement_to_trial(result,0,true,metadata,trial,error)&&std::abs(trial.residual[0]-8)<0.2&&std::abs(trial.residual[1]+12)<0.2&&trial.measurement_source=="follow_recoil_display_geometry_confirmed_endpoint_v1","端点Trial保存e而非反号counts，单独标识观测来源");
 }
}
void extension_replay(){
 const auto directory=std::filesystem::path(__FILE__).parent_path()/"fixtures/recoil_extension";
 std::ifstream input(directory/"base.json");nlohmann::json document;input>>document;
 RecoilProfile base;std::string error;
 expect(load_recoil_profile(document.dump(),base,error),"真实执行基线必须有效");
 std::vector<WallCaptureReport> fit(3),holdout(2);
 for(int i=0;i<5;++i){auto& report=i<3?fit[i]:holdout[i-3];
  const auto name=i<3?"fit-"+std::to_string(i+1):"holdout-"+std::to_string(i-2);
  expect(load_wall_report(directory/(name+".json"),report,error),"真实五组测量必须可重载");
 }
 WallOptimizationRequest request;request.measurements_confirmed=true;request.locked_prefix_ms=base.points.back().time_ms;
 const auto result=optimize_wall_trials(base,fit,holdout,request);
 expect(result.valid&&result.candidate,"真实三拟合两验证必须生成候选");
 if(!result.candidate){std::cerr<<result.message<<'\n';return;}
 const auto& candidate=*result.candidate;
 const auto boundary=base.points.back();const auto end=candidate.points.back();
 for(const auto& point:base.points){const auto actual=sample_recoil_profile(candidate,point.time_ms);
  expect(actual.x_counts==point.x_counts&&actual.y_counts==point.y_counts,"真实锁前段全部节点保持");}
 for(double time=0;time<boundary.time_ms;time+=0.5){
  const auto old=sample_recoil_profile(base,time),current=sample_recoil_profile(candidate,time);
  expect(old.x_counts==current.x_counts&&old.y_counts==current.y_counts,"真实锁前段插值保持");}
 const auto tail=sample_recoil_profile(candidate,550);
 expect(end.y_counts-boundary.y_counts>40,"真实新增段不能被微调预算截成净5counts");
 expect(end.y_counts-tail.y_counts>10,"真实后半段仍须建立纵向补偿，不能延长平台");
 double before=0,after=0;
 for(const auto& report:holdout)for(double time=boundary.time_ms+1;time<=end.time_ms;time+=1){
  const auto residual=sample_recoil_profile(*report.candidate,time);
  const auto current=sample_recoil_profile(candidate,time);
  before+=std::abs(residual.y_counts);
  after+=std::abs(residual.y_counts-(current.y_counts-boundary.y_counts));
 }
 expect(after<before*0.5,"真实验证组新增段预测Y残差至少减半");
 std::cout<<"extension_replay end_ms="<<end.time_ms<<" added_y="<<end.y_counts-boundary.y_counts
  <<" tail_y="<<end.y_counts-tail.y_counts<<" holdout_y_ratio="<<after/before<<'\n';
}
}
int main(int argc,char** argv){
 if(argc==2&&std::string(argv[1])=="--extension-replay"){extension_replay();return failures?1:0;}
 if(argc==3&&std::string(argv[1])=="--registration-replay"){
  const std::filesystem::path path=argv[2];std::ifstream input(path);nlohmann::json report;input>>report;
  const auto before=cv::imread((path.parent_path()/report.at("frames").at(0).at("file").get<std::string>()).string());
  nlohmann::json rows=nlohmann::json::array();
  const auto inspect=[&](const std::string& file){
   const auto after=cv::imread((path.parent_path()/file).string());
   const auto r=detail::register_wall(before,after,{40,40,80,80});
   rows.push_back({{"file",file},{"failure",r.failure},{"shift",{r.shift.x,r.shift.y}},
    {"response",r.response},{"residual",r.residual},{"raw_residual",r.raw_residual},{"method",detail::kWallRegistration},
    {"search_policy",detail::kWallSearchPolicy},{"search_region",{r.search_region.x,r.search_region.y,r.search_region.width,r.search_region.height}},
    {"peak_separation",r.peak_separation}});
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

 follow_recoil_tests();
 extension_replay();
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
 expect(large.valid&&large.candidate,"大于64像素且有真实图像支撑时可配准");
 if(large.candidate)expect(std::abs(large.candidate->points.back().x_counts+35)<0.2,"大范围camera补偿尺度正确");
 const cv::Mat boundary_transform=(cv::Mat_<double>(2,3)<<1,0,130,0,1,0);
 cv::warpAffine(wide,wide_shift,boundary_transform,wide.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
 expect(analyze_wall_capture({{0,wide},{40,wide_shift}},wide_request).valid,"真实支撑内130像素平移不得被ROI30%经验界误拒绝");
 const cv::Mat unsupported_transform=(cv::Mat_<double>(2,3)<<1,0,330,0,1,0);
 cv::warpAffine(wide,wide_shift,unsupported_transform,wide.size(),cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
 expect(!analyze_wall_capture({{0,wide},{40,wide_shift}},wide_request).valid,"超出中点真实支撑的平移仍须拒绝，不能补造图像");
 // 真实失败帧必须通过生产分析接口复现，不用无射击标定帧代替。
 const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures/recoil_registration";
 cv::Mat supported_base(320,320,CV_8UC1);cv::RNG support_random(817);
 support_random.fill(supported_base,cv::RNG::UNIFORM,20,230);cv::GaussianBlur(supported_base,supported_base,{5,5},1.0);
 for(const auto shift:std::vector<cv::Point2d>{{-39,0},{77,0},{0,-39},{0,77},{30,-30},{-30,30}}){
  cv::Mat shifted;const cv::Mat warp=(cv::Mat_<double>(2,3)<<1,0,shift.x,0,1,shift.y);
  cv::warpAffine(supported_base,shifted,warp,supported_base.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT);
  const auto registration=detail::register_wall(supported_base,shifted,{40,40,80,80});
  expect(registration.failure.empty()&&cv::norm(registration.shift-shift)<0.3,"真实搜索支撑内正负两轴及边缘平移必须恢复同一对应");
 }
 for(const auto shift:std::vector<cv::Point2d>{{-41,0},{79,0},{0,-41},{0,79}}){
  cv::Mat shifted;const cv::Mat warp=(cv::Mat_<double>(2,3)<<1,0,shift.x,0,1,shift.y);
  cv::warpAffine(supported_base,shifted,warp,supported_base.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT);
  expect(!detail::register_wall(supported_base,shifted,{40,40,80,80}).failure.empty(),"真实模板或中点halo支撑外不得接收外推");
 }
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
 const auto stage_reference=cv::imread((fixtures/"reference-stage.png").string());
 const auto stage_shot=cv::imread((fixtures/"shot-stage.png").string());
 expect(analyze_wall_capture({{0,stage_reference},{120,stage_shot}},shot_request).valid,
  "分段实采失败帧必须在原残差门槛内完成亚像素求精");
 for(int i=0;i<3;++i){
  const auto range_reference=cv::imread((fixtures/("reference-range-"+std::to_string(i)+".png")).string());
  const auto range_shot=cv::imread((fixtures/("shot-range-"+std::to_string(i)+".png")).string());
  const auto range_result=analyze_wall_capture({{0,range_reference},{120,range_shot}},shot_request);
  expect(range_result.valid,"三发/五发真实图像中仍有完整背景对应，不应因固定24像素搜索域中断");
  if(range_result.valid)expect(range_result.observations.back().center.y>185&&range_result.observations.back().center.y<189,
   "扩大到真实支撑后仍须匹配同一文字，不能换到重复峰");
  const auto bounded=detail::register_wall(range_reference,range_shot,{40,40,80,80});
  expect(bounded.search_region==cv::Rect(0,0,198,198),"默认搜索必须来自完整模板与中点halo真实支撑交集");
  const auto calibration_search=detail::register_wall(range_reference,range_shot,{40,40,80,80},{64,64});
  expect(calibration_search.search_region==cv::Rect(0,0,184,184),"标定显式64像素搜索域及已有v3映射须保持兼容");
 }
 const auto text_reference=cv::imread((fixtures/"reference-text.png").string());
 const auto text_shot=cv::imread((fixtures/"shot-text.png").string());
 const auto text_result=analyze_wall_capture({{0,text_reference},{120,text_shot}},shot_request);
 expect(text_result.valid,"真实高对比文字的亚像素采样差异不得误判为背景失配");
 if(text_result.valid)expect(std::abs(text_result.observations.back().center.x-161.4)<0.6&&
  std::abs(text_result.observations.back().center.y-157.4)<0.6,"文字采样残差修复必须保留几何对应位置");
 auto text_occluded=text_shot.clone();text_occluded(cv::Rect(60,60,20,20)).setTo(cv::Scalar::all(128));
 const auto text_blocked=detail::register_wall(text_reference,text_occluded,{40,40,80,80});
 expect(text_blocked.response>=0.5&&text_blocked.peak_separation>=0.1&&
  text_blocked.failure=="inconsistent_registration","滤波残差仍必须拒绝相关性正常的局部遮挡");
 const auto halo=detail::register_wall(text_reference,text_reference,{0,40,80,80});
 expect(halo.failure=="invalid_registration_geometry","残差滤波不得借图像边界补造真实halo");
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
  // 边界残差不为零，且锁定点早于旧末端：旧域仍有限微调，新域仅建立相对增量。
  auto staged_base=base;staged_base.points={{0,0,0},{20,1,2},{40,2,3}};
  auto staged_fit=fit,staged_holdout=holdout;
  for(auto* reports:{&staged_fit,&staged_holdout})for(auto& report:*reports)
   report.candidate->points={{0,0,0},{20,-15,30},{40,-20,40},{60,-40,70},{80,-60,100}};
  tune.locked_prefix_ms=20;
  const auto staged=optimize_wall_trials(staged_base,staged_fit,staged_holdout,tune);
  expect(staged.valid&&staged.candidate,"旧段微调与新增段建立可同时存在");
  if(staged.candidate){
   const auto joint=sample_recoil_profile(*staged.candidate,40),end=staged.candidate->points.back();
   expect(joint.x_counts==-3&&joint.y_counts==8,"未锁定旧末端两轴仍严格服从5counts预算");
   expect(end.x_counts-joint.x_counts==-40&&end.y_counts-joint.y_counts==60,"新段以旧末端残差为锚建立相对增量");
   const auto near=sample_recoil_profile(*staged.candidate,40+1e-7);
   expect(std::abs(near.x_counts-joint.x_counts)<1e-5&&std::abs(near.y_counts-joint.y_counts)<1e-5,"连接处连续且无边界残差阶跃");
  }
  auto opposed=staged_holdout;
  for(auto& report:opposed)report.candidate->points={{0,0,0},{20,15,-30},{40,20,-40},{80,60,-100}};
  expect(!optimize_wall_trials(staged_base,staged_fit,opposed,tune).valid,"独立组方向相反必须拒绝新增段");
  // 早段改善不能掩盖新增域尾端反向。
  for(auto& report:opposed)report.candidate->points={{0,0,0},{20,-15,30},{40,-20,40},{60,-40,70},{80,60,-100}};
  expect(!optimize_wall_trials(staged_base,staged_fit,opposed,tune).valid,"新增末端恶化必须拒绝，不能由早段平均掩盖");
  for(auto& report:opposed)report.candidate->points={{0,0,0},{20,-15,30},{40,-20,40},{60,-40,70},{70,-50,-10000},{80,-60,100}};
  expect(!optimize_wall_trials(staged_base,staged_fit,opposed,tune).valid,"留出独有内部节点的反向残差不能漏评");
  tune.locked_prefix_ms=0;
  holdout[0].candidate->source.sha256=fit[0].candidate->source.sha256;expect(!optimize_wall_trials(base,fit,holdout,tune).valid,"留出不允许重复拟合来源");
 }
 std::filesystem::remove_all(directory);
 return failures?1:0;
}
