#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "recoil_tuner/recoil_tuner.h"
#include "recoil/recoil.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <numeric>
#include <utility>

namespace {
thread_local bool measuring = false;
thread_local std::size_t allocation_count = 0, allocation_bytes = 0;
}
void* operator new(std::size_t size) {
    for (;;) {
        if (auto* result = std::malloc(size ? size : 1)) {
            if (measuring) { ++allocation_count; allocation_bytes += size; }
            return result;
        }
        if (const auto handler = std::get_new_handler()) handler();
        else throw std::bad_alloc();
    }
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace recoil_tuner;
using Json = nlohmann::json;
Dataset fixture(std::size_t points, double tolerance) {
    Dataset d; d.phase_tolerance_ms=tolerance;
    d.environment_fingerprint="synthetic-game-input-conditions-v1";d.base_profile_revision="1";
    for(std::size_t i=0;i<points;++i) {
        const double time=100.0*i/(points-1);
        d.base_curve.push_back({time,0,time/10});
    }
    for(int i=0;i<5;++i) {
        Trial t;const auto suffix=std::to_string(i);
        t.id="trial-"+suffix;t.content_hash="hash-"+suffix;t.firing_id="firing-"+suffix;t.source_run="run-"+suffix;
        t.environment_fingerprint=d.environment_fingerprint;t.executed_profile_revision="1";t.measurement_source="synthetic-known-response";
        t.use=i<3?TrialUse::FIT:TrialUse::HOLDOUT;
        t.completed=t.independent_recoil=t.timing_valid=t.reference_confirmed=true;
        t.measurement_ms=110;t.timing_uncertainty_ms=0.1;
        t.residual={4.0+(i-2)*0.02,-3.0+(i-2)*0.01};t.noise={0.1,0.1};t.executed_counts={0,10};
        for(int k=1;k<=10;++k)t.receipts.push_back({static_cast<std::uint64_t>(k),k*10.0,k*10.0,0,1,ReceiptState::ACKNOWLEDGED});
        d.trials.push_back(std::move(t));
    }
    for(int i=0;i<3;++i) {
        ResponseExperiment e;const auto suffix=std::to_string(i);
        e.id="experiment-"+suffix;e.environment_fingerprint=d.environment_fingerprint;
        e.baseline_run="cal-base-"+suffix;e.changed_run="cal-changed-"+suffix;
        e.baseline_hash="cal-base-hash-"+suffix;e.changed_hash="cal-change-hash-"+suffix;
        e.acknowledged=e.timing_valid=true;e.noise=0.1;
        e.delta_counts=i==0?std::array<double,2>{10,0}:i==1?std::array<double,2>{0,10}:std::array<double,2>{10,10};
        e.delta_residual={2*e.delta_counts[0],3*e.delta_counts[1]};d.response_experiments.push_back(std::move(e));
    }
    return d;
}
Json metrics_json(const Metrics& m) {
    return {m.robust_center_error,m.p95_error,m.worst_error,m.mean_absolute_axis};
}
Json report_json(const Report& r) {
    Json j={{"status",static_cast<int>(r.status)},{"messages",r.messages},
        {"consumed_holdout_trial_ids",r.consumed_holdout_trial_ids},{"response_condition",r.response_condition},
        {"response_matrix",r.response_matrix},{"response_available",r.response_available},
        {"predictions_available",r.predictions_available},{"fit_before",metrics_json(r.fit_before)},
        {"fit_predicted",metrics_json(r.fit_predicted)},{"holdout_before",metrics_json(r.holdout_before)},
        {"holdout_predicted",metrics_json(r.holdout_predicted)},{"candidate",nullptr}};
    if(r.candidate) {
        const auto& c=*r.candidate;Json points=Json::array();
        for(const auto& p:c.points)points.push_back({p.time_ms,p.x_counts,p.y_counts});
        j["candidate"]={{"schema_version",c.schema_version},{"revision",c.revision},{"parent_revision",c.parent_revision},
            {"environment_fingerprint",c.environment_fingerprint},{"generation",c.generation},
            {"correction_counts",c.correction_counts},{"points",points},{"fit_trial_ids",c.fit_trial_ids},
            {"consumed_holdout_trial_ids",c.consumed_holdout_trial_ids},{"evidence_hashes",c.evidence_hashes},
            {"software_validated",c.software_validated}};
    }
    return j;
}
bool validate(const std::vector<CurvePoint>& points,std::string& error) {
    RecoilProfile p;p.id="synthetic";p.weapon_id="synthetic-weapon";p.state=RecoilProfileState::SCHEMA_VALID;
    for(const auto& v:points)p.points.push_back({v.time_ms,v.x_counts,v.y_counts});
    RecoilProfile out;return compile_recoil_profile(p,{},out,error);
}
}
void publish_new_json(const std::filesystem::path& output,const std::string& payload) {
    const auto temporary=output.parent_path()/(L".recoil-tuner-"+std::to_wstring(GetCurrentProcessId())+
        L"-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count())+L".incoming");
    const auto handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create exclusive benchmark temporary");
    bool closed=false;
    try {
        std::size_t offset=0;
        while(offset<payload.size()) {
            const auto size=static_cast<DWORD>(std::min<std::size_t>(65536,payload.size()-offset));DWORD written=0;
            if(!WriteFile(handle,payload.data()+offset,size,&written,nullptr)||written!=size)
                throw std::runtime_error("benchmark write failed");
            offset+=written;
        }
        if(!FlushFileBuffers(handle))throw std::runtime_error("benchmark flush failed");
        if(!CloseHandle(handle))throw std::runtime_error("benchmark close failed");
        closed=true;
        if(!MoveFileW(temporary.c_str(),output.c_str()))throw std::runtime_error("benchmark publication failed; existing output preserved");
    } catch(...) {
        if(!closed)CloseHandle(handle);
        DeleteFileW(temporary.c_str());throw;
    }
}
int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"output JSON path required\n";return 2;}
    constexpr int warmup=2,repeats=11;
    Json output={{"fixture","deterministic synthetic production optimize; no device output"},
        {"allocation_scope","C++ ordinary new/new[] calls only; excludes OpenCV malloc/aligned allocation"},
        {"fixture_seed",0},{"warmup",warmup},{"repeats",repeats},{"cases",Json::array()}};
    int total_failures=0;
    for(const auto [points,tolerance]:{std::pair<std::size_t,double>{3,5},{1000,5},{10000,5},{30000,20}}) {
        const auto d=fixture(points,tolerance);Request request;request.candidate_revision="2";
        for(int i=0;i<warmup;++i)optimize(d,request,validate);
        std::vector<double> times;Json oracle;std::size_t max_allocations=0,max_bytes=0;int failures=0;
        for(int i=0;i<repeats;++i) {
            allocation_count=allocation_bytes=0;measuring=true;
            const auto start=std::chrono::steady_clock::now();const auto report=optimize(d,request,validate);
            const auto end=std::chrono::steady_clock::now();measuring=false;
            max_allocations=std::max(max_allocations,allocation_count);max_bytes=std::max(max_bytes,allocation_bytes);
            times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
            const auto serialized=report_json(report);
            if(i==0)oracle=serialized;
            if(serialized.dump()!=oracle.dump()||report.status!=Status::CANDIDATE_VALIDATED)++failures;
        }
        total_failures+=failures;
        std::sort(times.begin(),times.end());
        const auto percentile=[&](double p){return times[static_cast<std::size_t>(std::ceil(p*times.size()))-1];};
        output["cases"].push_back({{"points",points},{"phase_tolerance_ms",tolerance},{"trials",d.trials.size()},
            {"receipts_per_trial",10},{"mean_ms",std::accumulate(times.begin(),times.end(),0.0)/times.size()},
            {"p50_ms",percentile(.5)},{"p95_ms",percentile(.95)},{"p99_ms",percentile(.99)},
            {"max_ms",times.back()},{"max_cpp_allocations",max_allocations},{"max_cpp_allocation_bytes",max_bytes},
            {"failures",failures},{"oracle",oracle}});
        std::cout<<"points="<<points<<" p50_ms="<<percentile(.5)<<" allocations="<<max_allocations<<" bytes="<<max_bytes<<" failures="<<failures<<'\n';
    }
    try {publish_new_json(std::filesystem::u8path(argv[1]),output.dump(2)+"\n");}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    return total_failures?1:0;
}
