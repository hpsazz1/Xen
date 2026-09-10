#include "recoil/recoil_store.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {
using Json=nlohmann::json;
bool id(const std::string& value) {
    return !value.empty()&&value.size()<=128&&std::all_of(value.begin(),value.end(),[](unsigned char c){
        return c>='a'&&c<='z'||c>='A'&&c<='Z'||c>='0'&&c<='9'||c=='_'||c=='-';
    });
}
bool file_name(const std::string& file) {
    return file.size()>5&&file.ends_with(".json")&&file!="active.json"&&id(file.substr(0,file.size()-5));
}
bool reparse(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(path);
#endif
}
std::string read(const std::filesystem::path& path,std::uintmax_t maximum) {
    if(reparse(path)||!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>maximum)
        throw std::runtime_error("文件类型或大小不符合曲线库限制");
    std::ifstream stream(path,std::ios::binary);
    if(!stream)throw std::runtime_error("无法读取曲线文件");
    std::string text(static_cast<std::size_t>(maximum)+1,'\0');
    stream.read(text.data(),static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if(text.size()>maximum||stream.bad())throw std::runtime_error("读取曲线文件失败或越界");
    return text;
}
Json index(const std::filesystem::path& directory) {
    const auto path=directory/"active.json";
    if(!std::filesystem::exists(path))return Json{{"schema_version",1},{"active",Json::object()}};
    auto value=Json::parse(read(path,65536));
    if(value.at("schema_version")!=1||!value.at("active").is_object()||value.at("active").size()>512)
        throw std::runtime_error("活动索引schema或数量无效");
    for(auto it=value["active"].begin();it!=value["active"].end();++it) {
        if(!id(it.key())||!file_name(it.value().at("file").get<std::string>())||
            (!it.value().value("previous",std::string{}).empty()&&!file_name(it.value()["previous"].get<std::string>())))
            throw std::runtime_error("活动索引含无效文件引用");
    }
    return value;
}
void write(const std::filesystem::path& destination,const std::string& text,bool replace) {
    static std::atomic<std::uint64_t> sequence{0};
    auto temp=destination;
    temp+=L".tmp-"+std::to_wstring(++sequence)+L"-"+std::to_wstring(RecoilClock::now().time_since_epoch().count());
#ifdef _WIN32
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("无法创建临时曲线文件");
    DWORD written=0;
    const bool success=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(handle);
    CloseHandle(handle);
    if(!success||!MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))) {
        DeleteFileW(temp.c_str());throw std::runtime_error("原子保存失败或版本已存在");
    }
#else
    if(!replace&&std::filesystem::exists(destination))throw std::runtime_error("版本已存在");
    {std::ofstream stream(temp,std::ios::binary);stream<<text;if(!stream)throw std::runtime_error("写入失败");}
    std::filesystem::rename(temp,destination);
#endif
}
struct IndexLock {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit IndexLock(const std::filesystem::path& directory) {
        handle=CreateFileW((directory/"active.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("活动索引正在更新，请重试");
    }
    ~IndexLock(){CloseHandle(handle);}
#else
    explicit IndexLock(const std::filesystem::path&){}
#endif
};
bool calibrated(const RecoilProfile& profile) {
    return profile.state==RecoilProfileState::CALIBRATED||profile.state==RecoilProfileState::ACCEPTED;
}
void report(std::string& error,const std::exception& e)noexcept{try{error=e.what();}catch(...){}}
}
RecoilStore::RecoilStore(std::filesystem::path directory):directory_(std::filesystem::absolute(directory).lexically_normal()){}
bool RecoilStore::load(const std::string& file,RecoilProfile& profile,std::string& error) const noexcept {
    try {
        if(!file_name(file)||reparse(directory_))throw std::runtime_error("曲线文件名或目录无效");
        return load_recoil_profile(read(directory_/file,16*1024*1024),profile,error);
    }catch(const std::exception& e){report(error,e);return false;}catch(...){return false;}
}
bool RecoilStore::list(std::vector<RecoilStoredProfile>& output,std::string& error) const noexcept {
    try {
        std::vector<RecoilStoredProfile> profiles;
        if(!std::filesystem::exists(directory_)){output={};error.clear();return true;}
        if(reparse(directory_))throw std::runtime_error("不允许重解析曲线目录");
        std::uintmax_t total=0;std::size_t entries=0;
        for(const auto& entry:std::filesystem::directory_iterator(directory_)) {
            if(++entries>1024)throw std::runtime_error("曲线目录项超过1024");
            const auto file=entry.path().filename().string();
            if(file=="active.json"||entry.path().extension()!=".json")continue;
            if(profiles.size()>=512)throw std::runtime_error("曲线数量超过512");
            total+=entry.file_size();if(total>64*1024*1024)throw std::runtime_error("曲线总大小超过64MiB");
            RecoilProfile p;if(!load(file,p,error))return false;
            profiles.push_back({file,std::make_shared<const RecoilProfile>(std::move(p))});
        }
        std::sort(profiles.begin(),profiles.end(),[](const auto& a,const auto& b){return a.file<b.file;});
        output=std::move(profiles);error.clear();return true;
    }catch(const std::exception& e){report(error,e);return false;}catch(...){return false;}
}
bool RecoilStore::save_new(const RecoilProfile& input,std::string& file,std::string& error,bool preserve) const noexcept {
    try {
        if(reparse(directory_)||!id(input.id))throw std::runtime_error("目录或profile id无效");
        RecoilProfile profile=input;
        std::vector<RecoilStoredProfile> known;if(!list(known,error))return false;
        std::uint64_t maximum=0;
        for(const auto& item:known)if(item.profile->id==profile.id)maximum=std::max(maximum,item.profile->revision);
        if(maximum==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("曲线revision已耗尽");
        profile.revision=std::max(profile.revision,maximum+1);
        if(!preserve){profile.state=RecoilProfileState::SCHEMA_VALID;profile.calibration.evidence.clear();profile.phase_tolerance_ms.reset();profile.recovery_ms.reset();}
        else if(!calibrated(profile))throw std::runtime_error("明确校准保存必须带CALIBRATED或ACCEPTED及完整证据");
        const auto text=serialize_recoil_profile(profile);
        if(text.size()>16*1024*1024)throw std::runtime_error("保存曲线超过16MiB");
        const auto name=profile.id+"-r"+std::to_string(profile.revision)+".json";
        if(!file_name(name))throw std::runtime_error("保存版本文件名过长");
        std::filesystem::create_directories(directory_);
        write(directory_/name,text,false);file=name;error.clear();return true;
    }catch(const std::exception& e){report(error,e);return false;}catch(...){return false;}
}
bool RecoilStore::set_active(const std::string& weapon,const std::string& file,std::string& error) const noexcept {
    try {
        RecoilProfile profile;if(!load(file,profile,error))return false;
        if(!id(weapon)||profile.weapon_id!=weapon||!calibrated(profile))throw std::runtime_error("活动引用必须对应本武器的已校准曲线");
        IndexLock lock(directory_);auto value=index(directory_);
        auto previous=value["active"].contains(weapon)?value["active"][weapon]["file"].get<std::string>():std::string{};
        if(previous==file){error.clear();return true;}
        value["active"][weapon]={{"file",file},{"previous",previous}};
        if(value.dump(2).size()>65536)throw std::runtime_error("活动索引大小超过限制");
        write(directory_/"active.json",value.dump(2),true);error.clear();return true;
    }catch(const std::exception& e){report(error,e);return false;}catch(...){return false;}
}
bool RecoilStore::rollback(const std::string& weapon,std::string& error) const noexcept {
    try {
        IndexLock lock(directory_);auto value=index(directory_);
        if(!value["active"].contains(weapon))throw std::runtime_error("没有可回退的活动版本");
        const auto current=value["active"][weapon]["file"].get<std::string>();
        const auto previous=value["active"][weapon].value("previous",std::string{});
        RecoilProfile profile;if(!load(previous,profile,error))return false;
        if(profile.weapon_id!=weapon||!calibrated(profile))throw std::runtime_error("回退版本不满足校准身份");
        value["active"][weapon]={{"file",previous},{"previous",current}};
        write(directory_/"active.json",value.dump(2),true);error.clear();return true;
    }catch(const std::exception& e){report(error,e);return false;}catch(...){return false;}
}
std::shared_ptr<const RecoilProfile> RecoilStore::resolve(const RecoilConfig& config,const std::string& weapon,std::string& error) const noexcept {
    try {
        if(reparse(directory_)||!id(weapon)||config.game_build.empty()||config.input_path.empty()||config.conditions.empty()||
            !std::isfinite(config.sensitivity)||config.sensitivity<=0)throw std::runtime_error("曲线匹配条件未知");
        std::string file;
        if(config.use_trial)file=config.trial_file;
        else {const auto value=index(directory_);if(!value["active"].contains(weapon))throw std::runtime_error("武器未明确选择活动曲线");file=value["active"][weapon]["file"].get<std::string>();}
        RecoilProfile p;if(!load(file,p,error))return {};
        if(!calibrated(p)||p.weapon_id!=weapon||p.fire_mode!=config.fire_mode||p.calibration.game_build!=config.game_build||
            p.calibration.input_path!=config.input_path||p.calibration.conditions!=config.conditions||
            !p.calibration.sensitivity||*p.calibration.sensitivity!=config.sensitivity)
            throw std::runtime_error("曲线校准状态、模式或输入条件不精确匹配");
        error.clear();return std::make_shared<const RecoilProfile>(std::move(p));
    }catch(const std::exception& e){report(error,e);return {};}catch(...){return {};}
}
