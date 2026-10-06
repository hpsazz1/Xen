#include "lineup/calibration_internal.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <vector>
namespace lineup::detail {
namespace {
bool compute_file_sha256(const std::filesystem::path& path,
                         std::string& value,
                         std::string& error) {
    value.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "无法读取 SHA-256 输入: " + path.string();
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD returned = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    auto cleanup = [&]() noexcept {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    const auto succeeded = [](NTSTATUS status) noexcept {
        return status >= 0;
    };
    if (!succeeded(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
        !succeeded(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
            &returned, 0)) ||
        !succeeded(BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
            &returned, 0))) {
        cleanup();
        error = "初始化 SHA-256 失败";
        return false;
    }
    object.resize(object_size);
    digest.resize(hash_size);
    if (!succeeded(BCryptCreateHash(
            algorithm, &hash, object.data(), object_size,
            nullptr, 0, 0))) {
        cleanup();
        error = "创建 SHA-256 状态失败";
        return false;
    }

    // Windows 可执行文件默认线程栈通常不足以再容纳 1 MiB 局部数组；哈希
    // 缓冲属于流式工作区，放到堆上可保持固定内存上限且不改变证据字节。
    std::vector<char> buffer(1024 * 1024);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && !succeeded(BCryptHashData(
                hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0))) {
            cleanup();
            error = "计算 SHA-256 失败";
            return false;
        }
    }
    if (!stream.eof() || !succeeded(BCryptFinishHash(
            hash, digest.data(), hash_size, 0))) {
        cleanup();
        error = "完成 SHA-256 失败";
        return false;
    }
    cleanup();

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const UCHAR byte : digest) {
        text << std::setw(2) << static_cast<unsigned int>(byte);
    }
    value = text.str();
    return true;
}
bool finite_range(double v, double low, double high) { return std::isfinite(v) && v >= low && v <= high; }
ExecutionGeometry geometry_from(const nlohmann::json &j) {
    ExecutionGeometry g;
    g.width=j.at("width"); g.height=j.at("height"); g.source_width=j.at("source_width"); g.source_height=j.at("source_height");
    g.encoded_width=j.at("encoded_width"); g.encoded_height=j.at("encoded_height");
    g.roi_x=j.at("roi_x"); g.roi_y=j.at("roi_y"); g.scale_x=j.at("scale_x"); g.scale_y=j.at("scale_y");
    g.mapping_verified=j.at("mapping_verified"); return g;
}
}
CalibrationReadResult read_calibration(const std::filesystem::path &path, const std::string &source_id,
    const ExecutionGeometry &geometry, const std::string &backend, const std::string &context_identity) noexcept {
    CalibrationReadResult result;
    try {
        const auto require=[](bool ok,const char *reason) { if(!ok) throw std::runtime_error(reason); };
        require(!path.empty() && !context_identity.empty(),"calibration_not_configured");
        require(std::filesystem::file_size(path)<=1024*1024,"calibration_file_too_large");
        std::ifstream input(path,std::ios::binary); nlohmann::json data; input>>data;
        require(data.at("schema")==1 && data.at("origin")=="measured" && data.at("reviewed")==true,
            "calibration_not_reviewed_measurement");
        require(data.at("source_id")==source_id && data.at("backend")==backend && data.at("context_identity")==context_identity,
            "calibration_binding_mismatch");
        const auto g=geometry_from(data.at("geometry"));
        require(g==geometry && g.mapping_verified && g.width>0 && g.height>0 && g.source_width>0 && g.source_height>0 &&
            finite_range(g.scale_x,0.001,1000) && finite_range(g.scale_y,0.001,1000),"calibration_geometry_mismatch");
        const auto id=data.at("id").get<std::string>(); require(!id.empty() && id.size()<=128,"calibration_id_invalid");
        const auto &samples=data.at("samples"); require(samples.is_array() && samples.size()>=8 && samples.size()<=128,"calibration_samples_invalid");
        double sums[2]={}, squares[2]={}; int positive[2]={}, negative[2]={};
        std::vector<std::pair<int,double>> gains;
        double maximum_delay=0; std::set<std::string> pairs;
        const auto directory=std::filesystem::weakly_canonical(path.parent_path());
        for(const auto &sample:samples) {
            const auto axis=sample.at("axis").get<std::string>(); require(axis=="x" || axis=="y","calibration_axis_invalid");
            const int a=axis=="x"?0:1;
            const double counts=sample.at("counts"), delta=sample.at("delta_error_pixels"), cross=sample.at("cross_error_pixels"), delay=sample.at("observed_delay_ms");
            require(finite_range(std::abs(counts),1,100) && std::trunc(counts)==counts && finite_range(std::abs(delta),2,100) &&
                std::isfinite(cross) && std::abs(cross)<=std::abs(delta)*0.1 && finite_range(delay,0,1000),"calibration_sample_out_of_range");
            // 两个方向分别至少两次，避免单向或偶然小位移拟合。
            (counts>0?positive[a]:negative[a])++;
            const auto before=sample.at("before_sha256").get<std::string>(), after=sample.at("after_sha256").get<std::string>();
            require(before!=after && pairs.insert(before+after).second,"calibration_duplicate_frame_pair");
            for(const auto prefix:{"before","after"}) {
                const auto name=sample.at(std::string(prefix)+"_image").get<std::string>();
                auto file=std::filesystem::weakly_canonical(directory/std::filesystem::u8path(name));
                auto relative=file.lexically_relative(directory);
                require(!relative.empty() && !relative.is_absolute() && *relative.begin()!=L"..","calibration_evidence_outside_bundle");
                require(std::filesystem::file_size(file)<=32*1024*1024,"calibration_evidence_too_large");
                std::string hash,error; require(compute_file_sha256(file,hash,error),"calibration_evidence_unreadable");
                require(hash==sample.at(std::string(prefix)+"_sha256").get<std::string>(),"calibration_evidence_hash_mismatch");
            }
            sums[a]+=counts*delta; squares[a]+=delta*delta; gains.emplace_back(a,counts/delta);
            maximum_delay=std::max(maximum_delay,delay);
        }
        double gain[2];
        for(int a=0;a<2;++a) {
            require(positive[a]>=2 && negative[a]>=2 && squares[a]>0,"calibration_axis_coverage_missing");
            gain[a]=sums[a]/squares[a]; require(finite_range(std::abs(gain[a]),0.01,20),"calibration_gain_invalid");
        }
        for(const auto &[a,value]:gains) require(std::abs(value-gain[a])<=std::abs(gain[a])*0.15,"calibration_inconsistent_samples");
        const double delay=data.at("observation_delay_ms");
        require(finite_range(delay,maximum_delay,1000),"calibration_delay_understates_samples");
        result.calibration={id,source_id,g,true,gain[0],gain[1],std::chrono::milliseconds(static_cast<long long>(std::ceil(delay)))};
        result.valid=true; result.reason="measured_evidence_loaded_game_conditions_require_match";
    } catch(const std::exception &error) { result.reason=error.what(); }
      catch(...) { result.reason="calibration_read_failed"; }
    return result;
}
}
