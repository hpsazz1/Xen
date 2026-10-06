#include "lineup/calibration_internal.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using Json=nlohmann::json;
using namespace lineup::detail;
void check(bool value,const char *why) { if(!value) throw std::runtime_error(why); }
std::string sha256(const std::string &value) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    auto cleanup=[&](){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);};
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"test sha open");
        DWORD size=0,bytes=0;check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&bytes,0)>=0,"test sha property");
        std::vector<UCHAR> object(size),digest(32);
        check(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0,"test sha create");
        check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),0)>=0,"test sha data");
        check(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"test sha finish");
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto byte:digest)out<<std::setw(2)<<static_cast<unsigned>(byte);
        cleanup();return out.str();
    } catch(...) {cleanup();throw;}
}
int main() {
    // 受控假测量只写测试临时目录；reader接受只验证格式/数学/哈希，不是生产实测证据。
    const auto root=std::filesystem::temp_directory_path()/("xen-calibration-reader-test-"+std::to_string(lineup::Clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(root);
        const auto path=root/"calibration.json";
        ExecutionGeometry geometry;geometry.width=geometry.height=geometry.encoded_width=geometry.encoded_height=320;
        geometry.source_width=1920;geometry.source_height=1080;geometry.roi_x=800;geometry.roi_y=380;geometry.scale_x=geometry.scale_y=1;geometry.mapping_verified=true;
        Json data={{"schema",1},{"id","TEST-ONLY-NOT-FOR-PRODUCTION"},{"origin","measured"},{"reviewed",true},
            {"source_id","fixture-source"},{"backend","fixture-backend"},{"context_identity","test-only"},{"observation_delay_ms",20},
            {"geometry",{{"width",320},{"height",320},{"source_width",1920},{"source_height",1080},{"encoded_width",320},{"encoded_height",320},{"roi_x",800},{"roi_y",380},{"scale_x",1},{"scale_y",1},{"mapping_verified",true}}},
            {"samples",Json::array()}};
        for(int i=0;i<8;++i) {
            const int counts=(i%4==0?10:i%4==1?20:i%4==2?-10:-20);
            Json sample={{"axis",i<4?"x":"y"},{"counts",counts},{"delta_error_pixels",-2*counts},{"cross_error_pixels",0},{"observed_delay_ms",10+i}};
            for(auto prefix:{"before","after"}) {
                auto name=std::string(prefix)+std::to_string(i)+".bin";
                auto bytes=std::string("TEST-ONLY-SYNTHETIC-")+prefix+std::to_string(i);
                std::ofstream(root/name,std::ios::binary)<<bytes;
                sample[std::string(prefix)+"_image"]=name;sample[std::string(prefix)+"_sha256"]=sha256(bytes);
            }
            data["samples"].push_back(sample);
        }
        const auto read=[&](const Json &value){std::ofstream(path,std::ios::binary)<<value.dump(2);return read_calibration(path,"fixture-source",geometry,"fixture-backend","test-only");};
        check(!read_calibration({},"fixture-source",geometry,"fixture-backend","test-only").valid,"empty path rejected");
        check(!read(Json::object()).valid,"empty document rejected");
        auto changed=data;changed["origin"]="simulation";check(read(changed).reason=="calibration_not_reviewed_measurement","simulation rejected");
        changed=data;changed["reviewed"]=false;check(!read(changed).valid,"unreviewed rejected");
        for(auto field:{"source_id","backend","context_identity"}) {changed=data;changed[field]="mismatch";check(read(changed).reason=="calibration_binding_mismatch","identity mismatch");}
        changed=data;changed["geometry"]["width"]=640;check(read(changed).reason=="calibration_geometry_mismatch","geometry mismatch");
        changed=data;changed["samples"][0]["before_sha256"]=std::string(64,'0');check(read(changed).reason=="calibration_evidence_hash_mismatch","hash mismatch");
        changed=data;for(auto &sample:changed["samples"])sample["axis"]="x";check(read(changed).reason=="calibration_axis_coverage_missing","both axes required");
        changed=data;changed["samples"][0]["delta_error_pixels"]=-10;check(read(changed).reason=="calibration_inconsistent_samples","outlier rejected");
        changed=data;changed["observation_delay_ms"]=1;check(read(changed).reason=="calibration_delay_understates_samples","delay underestimate rejected");
        changed=data;changed["samples"][0]["cross_error_pixels"]=10;check(read(changed).reason=="calibration_sample_out_of_range","cross axis rejected");
        changed=data;changed["samples"][1]=changed["samples"][0];check(read(changed).reason=="calibration_duplicate_frame_pair","repeated evidence rejected");
        changed=data;changed["samples"][0]["before_image"]="../outside.bin";check(read(changed).reason=="calibration_evidence_outside_bundle","path escape rejected");
        auto valid=read(data);check(valid.valid && valid.calibration.validated,"controlled final valid fixture");
        check(valid.calibration.counts_per_local_pixel_x==-0.5 && valid.calibration.counts_per_local_pixel_y==-0.5,"known signed gains");
        check(valid.calibration.observation_delay==std::chrono::milliseconds(20),"measured delay loaded");
        std::ofstream(root/"before0.bin",std::ios::binary)<<"mutated-test-only";
        check(read(data).reason=="calibration_evidence_hash_mismatch","original changed after packaging detected");
        std::cout<<"calibration reader simulation/identity/hash/coverage/outlier/delay/valid tests passed\n";
        return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
