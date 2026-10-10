#include "aim/aim.h"
#include "log/log.h"
#include "recoil/motion_ledger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; }
}
auto milliseconds(double value) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(value));
}
struct Event { Clock::time_point control, completed, effect; int dx, dy; };
struct Summary {
    int samples=0, inventory_errors=0, transition_samples=0, transition_errors=0;
    double preview_error=0, median_error=0;
};

// O是取像，L是交付，C是Aim控制，B是后端完成，E是独立plant实际应用。
// 每帧间隔18ms且C=O+14ms，确保下一C严格晚于上一B（最大K15ms）。
Summary replay(double backend_ms, bool total_delay, bool prediction, bool step, std::ostream* csv,
               bool external = false) {
    // 本实验显式固定已校准的相机响应，避免将增益失配混入时间起点变量。
    // plant与oracle只使用此物理参数和自己的事件表，不读取Aim估计速度/库存。
    constexpr double plant_pixels_per_count = .5215;
    AimConfig config;
    config.min_confirmed_hits=1;
    config.enable_delay_compensation=true;
    config.enable_prediction=prediction;
    config.control_delay_ms=static_cast<float>(15+(total_delay ? backend_ms : 0));
    config.max_delay_compensation_ms=80;
    config.max_delay_compensation_percent=50;
    config.max_prediction_lead_percent=50;
    config.counts_per_pixel_x=config.counts_per_pixel_y=.5f;
    config.acquisition_range_percent=100;
    config.max_counts_per_frame=50;
    Aim aim(config);
    const auto begin=Clock::time_point{}+30s;
    MotionLedger ledger;
    ledger.reset(config.max_counts_per_frame,16,begin-1s);
    std::vector<Event> events;
    std::vector<double> errors;
    Summary summary;
    AimFrame previous;
    double previous_camera=0;
    std::uint64_t identity=0;
    for (int n=0;n<220;++n) {
        const auto observation=begin+18ms*n;
        const auto delivered=observation+4ms;
        const auto control=observation+14ms;
        const double current_backend=step && n<100 ? 5 : backend_ms;
        const auto completed=control+milliseconds(current_backend);
        const auto effect=completed+15ms;
        double camera=0, unseen=0;
        for (const auto& event:events) {
            if (event.effect<=observation) camera+=event.dx*plant_pixels_per_count;
            if (event.completed<=control && event.effect>observation) unseen+=event.dx;
        }
        const double world=176+500*std::chrono::duration<double>(observation-begin).count();
        AimFrame frame;
        frame.sequence=n+1;
        frame.captured_at=observation; frame.control_at=control;
        frame.roi_width=frame.roi_height=320;
        frame.control_center_x=frame.control_center_y=160;
        frame.lock_active=true; frame.observation_epoch=1;
        if(external) frame.external_motion=ledger.snapshot(control);
        const float center=static_cast<float>(world-camera);
        frame.detections={{center-40,141.1f,center+40,195.1f,.95f,0}};
        if (n>0) frame.background_motion_x={AimBackgroundMotionStatus::VALID,
            previous.sequence,frame.sequence,previous.captured_at,observation,1,
            static_cast<float>(-(camera-previous_camera)),1,0,2};
        const auto result=aim.process(frame);
        if (!identity && result.has_target) identity=result.target.track_id;
        expect(result.status==AimStatus::SUCCESS && result.has_target && identity!=0 &&
            result.target.track_id==identity && result.target.state==TrackState::CONFIRMED &&
            result.target.matched_observation_valid && !result.target.predicted,
            "延迟实验逐帧保持同一真实匹配目标");
        if(n>0) expect(result.control.background_motion_use_x==AimBackgroundMotionUse::CONSUMED,
            "延迟实验必须消费真实同源背景");
        const double expected_preview=500/plant_pixels_per_count*
            std::chrono::duration<double>(effect-observation).count();
        if (step && n >= 100 && n <= 105) {
            // 固定D30而阶跃前实际C到E20：仍在窗口内的旧命令存在已知配置失配。
            // 明确验证过渡段，不能只用末段稳态掩盖两帧多计，也不声称自动适配K。
            double configured_unseen = 0;
            for (const auto& event : events)
                if (event.completed <= control && event.control + milliseconds(config.control_delay_ms) > observation)
                    configured_unseen += event.dx;
            expect(result.control.residual_role_x, "阶跃过渡段必须实际消费执行误差角色");
            expect(std::abs(result.control.execution_unseen_command_x_counts-configured_unseen)<.001,
                   "阶跃过渡也必须遵循已配置C+D，不能回到B+D");
            expect(std::abs(result.control.execution_world_preview_x_counts-expected_preview)<.15,
                   "阶跃后的新预览每帧均覆盖当前真实时域");
            const double excess = configured_unseen-unseen;
            expect(n < 102 ? excess > 0 : std::abs(excess)<.001,
                   "固定D的旧段失配只在前两帧过渡保留，之后物理库存一致");
            ++summary.transition_samples;
            if(std::abs(result.control.execution_unseen_command_x_counts-unseen)>.001) ++summary.transition_errors;
            std::cout << "transition external=" << external << " prediction=" << prediction << " frame=" << n
                      << " physical_unseen=" << unseen << " configured_unseen=" << configured_unseen
                      << " aim_unseen=" << result.control.execution_unseen_command_x_counts << '\n';
        }
        if(n>=140) {
            expect(result.control.residual_role_x,"统计窗口必须实际进入执行误差角色");
            ++summary.samples;
            if(std::abs(result.control.execution_unseen_command_x_counts-unseen)>.001) ++summary.inventory_errors;
            summary.preview_error+=std::abs(result.control.execution_world_preview_x_counts-expected_preview);
            errors.push_back(std::abs(center-160));
        }
        if(csv) *csv<<backend_ms<<','<<total_delay<<','<<prediction<<','<<step<<','<<n<<','
            <<std::chrono::duration<double,std::milli>(observation-begin).count()<<','
            <<std::chrono::duration<double,std::milli>(delivered-begin).count()<<','
            <<std::chrono::duration<double,std::milli>(control-begin).count()<<','
            <<std::chrono::duration<double,std::milli>(completed-begin).count()<<','
            <<std::chrono::duration<double,std::milli>(effect-begin).count()<<','
            <<expected_preview<<','<<result.control.execution_world_preview_x_counts<<','
            <<unseen<<','<<result.control.execution_unseen_command_x_counts<<','<<center-160<<'\n';
        const int dx=result.has_command ? result.command.dx_counts : 0;
        const int dy=result.has_command ? result.command.dy_counts : 0;
        expect(dy==0,"固定Y不应产生纵向输出");
        expect(aim.record_backend_completed_command(frame.sequence,completed,external?0:dx,external?0:dy),
            "确认本分支实际归属的命令，外部事件不双计入自身环");
        if(external) {
            MouseMoveReceipt receipt; receipt.succeeded=true; receipt.backend_completed_at=completed;
            expect(ledger.record({dx,dy},receipt,true,control),"真实共享账本记录外部控制起点及完成回执");
        }
        events.push_back({control,completed,effect,dx,dy});
        previous=frame; previous_camera=camera;
    }
    expect(summary.samples==80,"独立oracle统计必须非空");
    if (step) expect(summary.transition_samples==6 && summary.transition_errors==2,
                     "阶跃验收须明确六个过渡帧中的两个物理配置失配，不能只看稳态");
    summary.preview_error/=summary.samples;
    std::sort(errors.begin(),errors.end()); summary.median_error=errors[errors.size()/2];
    std::cout<<"K="<<backend_ms<<" D="<<config.control_delay_ms<<" prediction="<<prediction<<" step="<<step<<" external="<<external
             <<" preview_MAE="<<summary.preview_error<<" inventory_mismatch="<<summary.inventory_errors
             <<"/"<<summary.samples<<" error_P50="<<summary.median_error<<'\n';
    return summary;
}
}
int main(int argc,char**argv) {
    LogConfig logging; logging.enable_console=logging.enable_file=logging.enable_debug_file=false; Log::init(logging);
    std::ofstream csv;
    if(argc==2) { csv.open(argv[1]); if(!csv) return 2;
        csv<<"backend_ms,total_delay,prediction,step,frame,O_ms,L_ms,C_ms,B_ms,E_ms,true_preview,aim_preview,true_unseen,aim_unseen,source_error\n"; }
    for (bool prediction:{false,true}) for (double backend:{.1,5.,15.}) {
        const auto residual=replay(backend,false,prediction,false,csv.is_open()?&csv:nullptr);
        const auto total=replay(backend,true,prediction,false,csv.is_open()?&csv:nullptr);
        if(backend>=5) expect(residual.preview_error>.9,"只配B到E的负控必须暴露未覆盖的C到B时间");
        expect(total.preview_error<.15,"C到E的配置必须覆盖真实世界预览");
        expect(total.inventory_errors==0,"C到E的同一配置必须正确扣除已经进入观测的命令");
    }
    const auto stepped=replay(15,true,false,true,csv.is_open()?&csv:nullptr);
    expect(stepped.preview_error<.15 && stepped.inventory_errors==0,"后端耗时阶跃后的匹配配置稳态仍须成立");
    for(bool prediction:{false,true}) for(double backend:{5.,15.}) {
        const auto outside=replay(backend,true,prediction,false,nullptr,true);
        expect(outside.preview_error<.15 && outside.inventory_errors==0,
            "真实共享账本的外部命令也必须使用同一控制起点，不能仅修正自身命令");
    }
    for(bool prediction:{false,true}) {
        const auto outside_step=replay(15,true,prediction,true,nullptr,true);
        expect(outside_step.preview_error<.15 && outside_step.inventory_errors==0,
               "外部账本阶跃须同时验证过渡段和匹配配置稳态");
    }
    Log::shutdown();
    return failures ? 1:0;
}
