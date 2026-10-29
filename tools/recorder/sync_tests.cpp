// SPDX-License-Identifier: GPL-2.0-or-later
#include "sync_log.h"
#include <iostream>
#include <stdexcept>
using namespace xen::recorder;
void require(bool condition,const char* description) { if (!condition) throw std::runtime_error(description); }
int main() {
    try {
        // 人工已知主辅偏移+100ns、单程10ns：不能把服务处理耗时算作网络不确定度。
        const auto mapping = clock_mapping(1000,1035,1110,1125);
        require(mapping.at("offset_aux_minus_source_ns")=="100","网络映射偏移错误");
        require(mapping.at("network_uncertainty_ns")=="10","服务处理时间未排除");
        const auto odd = clock_mapping(1000,1036,1110,1125);
        require(odd.at("offset_rounding_ns")=="1" && odd.at("network_uncertainty_ns")=="11","奇数往返未保留量化上界");
        bool rejected = false;
        try { clock_mapping(1000,1001,1100,1120); } catch (...) { rejected = true; }
        require(rejected,"负网络耗时应拒绝");
        EncodedMarker marker; marker.id="SYNC-test"; marker.video_pts=180; marker.timebase_num=1; marker.timebase_den=120;
        require(marker_record(marker).at("video_seconds").is_null(),"未匹配标记不能宣称视频时间");
        marker.matched=true;
        require(marker_record(marker).at("video_seconds")==1.5,"PTS有理数单位错误");
        marker.render_frame_time_ns=101; marker.encoder_frame_time_ns=77; marker.timing_mode="gpu_previous_completed";
        const auto record=marker_record(marker);
        require(record.at("render_frame_time_ns")=="101" && record.at("encoder_frame_time_ns")=="77" &&
            record.at("timing_mode")=="gpu_previous_completed","内容CTS与编码候选CTS不能混为一域");
        std::cout << "同步映射与未匹配语义通过\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
