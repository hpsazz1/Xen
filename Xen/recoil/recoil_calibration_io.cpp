#include "recoil/recoil_calibration_io.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <climits>

namespace {
using Json=nlohmann::json;
constexpr std::uintmax_t max_bytes=16*1024*1024;
void safe_path(const std::filesystem::path& path) {
    auto at=std::filesystem::absolute(path).lexically_normal();
    while(!at.empty()) {
        const auto flags=GetFileAttributesW(at.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES && (flags&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("校准路径不接受重解析点");
        const auto parent=at.parent_path();if(parent==at)break;at=parent;
    }
}
std::string read(const std::filesystem::path& path) {
    safe_path(path);
    if(std::filesystem::file_size(path)>max_bytes)throw std::runtime_error("校准文件超过16MiB");
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("无法读取校准文件");
    std::string result((std::istreambuf_iterator<char>(input)),{});
    if(result.size()>max_bytes || input.bad())throw std::runtime_error("校准文件读取不完整");
    return result;
}
void create_file(const std::filesystem::path& path,const std::string& bytes) {
    safe_path(path);
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("文件已存在或无法创建；拒绝覆盖会话");
    DWORD written=0;
    const bool ok=bytes.size()<=max_bytes && WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) &&
        written==bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(!ok)throw std::runtime_error("校准文件写入失败；保留不完整目录供检查");
}
Json parse(const std::string& text) {
    int max_depth=0;
    auto value=Json::parse(text,[&](int depth,Json::parse_event_t,const Json&){max_depth=std::max(max_depth,depth);return depth<=32;});
    if(max_depth>32 || !value.is_object())throw std::runtime_error("校准JSON结构无效");
    return value;
}
int integer(const Json& value,const char* key) {
    const auto& item=value.at(key);
    if(!item.is_number_integer() || (item.is_number_unsigned()&&item.get<std::uint64_t>()>INT_MAX))throw std::runtime_error("校准整数字段越界");
    const auto number=item.get<std::int64_t>();
    if(number<INT_MIN||number>INT_MAX)throw std::runtime_error("校准整数字段越界");
    return static_cast<int>(number);
}
Json environment_json(const RecoilCalibrationEnvironment& e) {
    return {{"weapon_id",e.weapon_id},{"game_build",e.game_build},{"input_path",e.input_path},{"conditions",e.conditions},{"sensitivity",e.sensitivity}};
}
RecoilCalibrationEnvironment environment(const Json& j) {
    return {j.at("weapon_id").get<std::string>(),j.at("game_build").get<std::string>(),j.at("input_path").get<std::string>(),
        j.at("conditions").get<std::string>(),j.at("sensitivity").get<double>()};
}
Json limits_json(const RecoilCalibrationLimits& l) {
    return {{"max_firing_sessions",l.max_firing_sessions},{"max_session_duration_ms",l.max_session_duration_ms},
        {"max_firing_duration_ms",l.max_firing_duration_ms},{"max_sent_l1_counts",l.max_sent_l1_counts},
        {"max_command_l1_counts",l.max_command_l1_counts},{"rolling_window_ms",l.rolling_window_ms},
        {"rolling_window_counts",l.rolling_window_counts},{"command_phase_budget_ms",l.command_phase_budget_ms}};
}
RecoilCalibrationLimits limits(const Json& j) {
    RecoilCalibrationLimits l;
    const auto sessions=integer(j,"max_firing_sessions");
    if(sessions!=1)throw std::runtime_error("B1仅允许一次弹序");
    l.max_firing_sessions=1;l.max_session_duration_ms=integer(j,"max_session_duration_ms");
    l.max_firing_duration_ms=integer(j,"max_firing_duration_ms");
    if(!j.at("max_sent_l1_counts").is_number_unsigned())throw std::runtime_error("累计counts必须为正整数");
    l.max_sent_l1_counts=j.at("max_sent_l1_counts").get<std::uint64_t>();
    l.max_command_l1_counts=integer(j,"max_command_l1_counts");l.rolling_window_ms=integer(j,"rolling_window_ms");
    l.rolling_window_counts=j.at("rolling_window_counts").get<double>();
    l.command_phase_budget_ms=j.at("command_phase_budget_ms").get<double>();return l;
}
Json manifest_json(const RecoilCalibrationManifest& m) {
    return {{"schema_version",m.schema_version},{"session_id",m.session_id},{"profile_file_sha256",m.profile_file_sha256},
        {"profile_semantic_sha256",m.profile_semantic_sha256},{"environment_fingerprint",m.environment_fingerprint},
        {"config_binding_sha256",m.config_binding_sha256},{"environment",environment_json(m.environment)},
        {"limits",limits_json(m.limits)},{"hold_virtual_key",m.hold_virtual_key},{"cancel_virtual_key",m.cancel_virtual_key}};
}
RecoilCalibrationManifest manifest(const Json& j) {
    RecoilCalibrationManifest m;
    if(integer(j,"schema_version")!=1)throw std::runtime_error("不支持的校准schema");
    m.session_id=j.at("session_id");m.profile_file_sha256=j.at("profile_file_sha256");
    m.profile_semantic_sha256=j.at("profile_semantic_sha256");m.environment_fingerprint=j.at("environment_fingerprint");
    m.config_binding_sha256=j.at("config_binding_sha256");m.environment=environment(j.at("environment"));m.limits=limits(j.at("limits"));
    m.hold_virtual_key=integer(j,"hold_virtual_key");m.cancel_virtual_key=integer(j,"cancel_virtual_key");return m;
}
std::string quote(const std::filesystem::path& p) {
    const auto raw=p.string();
    if(raw.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("命令路径含换行");
    std::string result="'";for(char c:raw){result+=c;if(c=='\'')result+='\'';}return result+"'";
}
std::string configuration(const std::filesystem::path& path,AppConfig& config) {
    const auto before=read(path);
    std::string error;
    if(!load_app_config(path.string(),config,error))throw std::runtime_error("校准配置无法载入；请先在设置中保存有效配置");
    if(config.mouse.backend!=MouseBackend::KMBOX_NET || !config.source_context.enabled || !config.gsi.enabled)
        throw std::runtime_error("校准要求KMBOX NET、源端焦点和GSI配置均已启用");
    if(read(path)!=before)throw std::runtime_error("配置在读取期间变化；请重新准备");
    return recoil_calibration_sha256(before);
}
std::string new_id() {
    unsigned char bytes[16]{};
    if(BCryptGenRandom(nullptr,bytes,sizeof bytes,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("无法创建校准身份");
    return "cal-"+recoil_calibration_sha256(std::string_view(reinterpret_cast<char*>(bytes),sizeof bytes)).substr(0,32);
}
}
bool prepare_recoil_calibration(const RecoilCalibrationPrepareRequest& request,RecoilCalibrationPrepared& out,std::string& error) noexcept {
    try {
        auto directory=std::filesystem::absolute(request.output_directory).lexically_normal();
        if(request.output_directory.empty()||request.executable_path.empty())throw std::runtime_error("校准目录与前台工具路径必须显式提供");
        safe_path(directory);
        const auto profile_text=read(request.profile_path);
        auto profile=std::make_shared<RecoilProfile>();
        if(!load_recoil_profile(profile_text,*profile,error))return false;
        safe_path(request.executable_path);
        if(!std::filesystem::is_regular_file(request.executable_path))throw std::runtime_error("校准前台工具不存在，请先完成对应构建");
        AppConfig config;const auto config_hash=configuration(request.config_path,config);
        RecoilCalibrationManifest m;
        m.session_id=new_id();m.profile_file_sha256=recoil_calibration_sha256(profile_text);
        m.profile_semantic_sha256=recoil_calibration_sha256(serialize_recoil_profile(*profile));
        m.config_binding_sha256=config_hash;m.environment=request.environment;
        m.environment_fingerprint=recoil_calibration_environment_fingerprint(m.environment);m.limits=request.limits;
        m.hold_virtual_key=request.hold_virtual_key;m.cancel_virtual_key=request.cancel_virtual_key;
        if(!validate_recoil_calibration_manifest(m,*profile,error))return false;
        auto document=manifest_json(m);
        document["config_path"]=std::filesystem::absolute(request.config_path).lexically_normal().string();
        document["executable_path"]=std::filesystem::absolute(request.executable_path).lexically_normal().string();
        const auto text=document.dump(2);
        const auto digest=recoil_calibration_sha256(text);
        const auto confirmation="CALIBRATE-"+digest;
        const auto command="& "+quote(std::filesystem::path(document["executable_path"].get<std::string>()))+
            " run "+quote(directory)+" --allow-physical-output --confirm '"+confirmation+"'";
        if(!std::filesystem::create_directory(directory))throw std::runtime_error("校准目录已存在；拒绝覆盖");
        create_file(directory/"profile.json",profile_text);
        create_file(directory/"manifest.json",text);
        create_file(directory/"TASK.md","# 单次弹道校准\n\n状态：PREPARED_NOT_LAUNCHED。仅由用户在前台运行以下完整命令。\n\n"
            "原曲线为SCHEMA_VALID；操作员声明环境不等于已测量。一次授权只允许一次人工左键弹序，需完整释放后按住保持键并按左键。\n\n"
            "```powershell\n"+command+"\n```\n\n超时、取消或故障后不可续跑；重新准备新会话。软件结束不会升级校准状态或发布活动版本。\n");
        create_file(directory/"PREPARED",digest);
        out={m,std::move(profile),std::move(config),directory,digest,confirmation,command};error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}catch(...){error="校准准备异常";return false;}
}
bool load_recoil_calibration_prepared(const std::filesystem::path& path,RecoilCalibrationPrepared& out,std::string& error) noexcept {
    try {
        const auto directory=std::filesystem::absolute(path).lexically_normal();
        const auto text=read(directory/"manifest.json");const auto digest=recoil_calibration_sha256(text);
        if(read(directory/"PREPARED")!=digest)throw std::runtime_error("校准准备未完成或manifest已变化");
        const auto document=parse(text);auto m=manifest(document);
        const auto profile_text=read(directory/"profile.json");
        if(recoil_calibration_sha256(profile_text)!=m.profile_file_sha256)throw std::runtime_error("候选快照已变化");
        auto profile=std::make_shared<RecoilProfile>();
        if(!load_recoil_profile(profile_text,*profile,error)||!validate_recoil_calibration_manifest(m,*profile,error))return false;
        const std::filesystem::path config_path=document.at("config_path").get<std::string>();
        AppConfig config;
        if(configuration(config_path,config)!=m.config_binding_sha256)throw std::runtime_error("已保存配置变化，必须重新准备会话");
        const auto confirmation="CALIBRATE-"+digest;
        const auto command="& "+quote(std::filesystem::path(document.at("executable_path").get<std::string>()))+
            " run "+quote(directory)+" --allow-physical-output --confirm '"+confirmation+"'";
        out={m,std::move(profile),std::move(config),directory,digest,confirmation,command};error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}catch(...){error="校准读取异常";return false;}
}
bool load_recoil_calibration_request(const std::filesystem::path& path,RecoilCalibrationPrepareRequest& out,std::string& error) noexcept {
    try {
        const auto j=parse(read(path));
        out.environment=environment(j.at("environment"));out.limits=limits(j.at("limits"));
        out.hold_virtual_key=integer(j,"hold_virtual_key");out.cancel_virtual_key=integer(j,"cancel_virtual_key");error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}catch(...){error="校准请求读取异常";return false;}
}
bool verify_recoil_calibration_launch(const std::filesystem::path& path,bool allow,const std::string& confirmation,
    RecoilCalibrationPrepared& out,std::string& error) noexcept {
    if(!allow || confirmation.empty()){error="必须由用户提供允许物理输出标志及绑定确认串";return false;}
    if(!load_recoil_calibration_prepared(path,out,error))return false;
    if(confirmation!=out.confirmation){error="校准确认串与当前会话不匹配";return false;}
    try {if(std::filesystem::exists(out.directory/"CONSUMED")){error="校准会话已经消费，禁止重放";return false;}}
    catch(...){error="无法核验会话消费状态";return false;}
    return true;
}
bool consume_recoil_calibration_session(const RecoilCalibrationPrepared& prepared,std::string& error) noexcept {
    try {create_file(prepared.directory/"CONSUMED",prepared.manifest_sha256);error.clear();return true;}
    catch(const std::exception& e){error=e.what();return false;}catch(...){error="会话消费失败";return false;}
}
bool write_recoil_calibration_result(const RecoilCalibrationPrepared& prepared,const std::string& termination,
    const RecoilCalibrationBudgetSnapshot& budget,const std::string& archive_status,std::string& error) noexcept {
    try {
        Json result={{"schema",1},{"session_id",prepared.manifest.session_id},{"manifest_sha256",prepared.manifest_sha256},
            {"manifest",manifest_json(prepared.manifest)},{"profile",Json::parse(serialize_recoil_profile(*prepared.profile))},
            {"termination",termination},{"archive_status",archive_status},{"physical_acceptance",nullptr},{"calibration_result",nullptr},
            {"environment_evidence","OPERATOR_DECLARED"},{"phase_budget_source","CALIBRATION_SESSION_BUDGET"},
            {"budget",{{"firing_sessions",budget.firing_sessions},{"sent_l1_counts",budget.sent_l1_counts},
                {"terminal",budget.terminal},{"end",RecoilCalibrationEndName(budget.end)}}}};
        create_file(prepared.directory/"result.json.tmp",result.dump(2));
        if(!MoveFileExW((prepared.directory/"result.json.tmp").c_str(),(prepared.directory/"result.json").c_str(),MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("校准报告无法原子发布");
        error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}catch(...){error="校准报告写入异常";return false;}
}
