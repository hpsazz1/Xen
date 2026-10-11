// 直接编译生产实现以验证内部publish，不复制发布逻辑，也不向生产接口添加故障开关。
// 同时调用真实SessionArchive生命周期；静态库仅提供DebugReport等未定义依赖。
#include "../Xen/debug/session_archive.cpp"
#include <codecvt>
#include <cstring>
#include <iostream>
#include <iterator>
#include <locale>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture read open");
    return {std::istreambuf_iterator<char>(input), {}};
}
Json json_file(const std::filesystem::path& path) { return Json::parse(bytes(path)); }
void wait_file(const std::filesystem::path& path) {
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(path) && Clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(std::filesystem::is_regular_file(path), "fixture published file timeout");
}
std::string failed_publish(const std::filesystem::path& path, const Json& value) {
    try { publish(path, value); }
    catch (const std::exception& error) { return error.what(); }
    throw std::runtime_error("publish unexpectedly succeeded");
}
struct StreamFault {
    int close_at = 0;
    bool write = false;
    std::atomic<int> closes{0}, write_failures{0};
};
// 使用真实filebuf转换失败，write/flush与close故障可区分。只影响本测试进程；
// 生命周期用例在worker空闲后安装，join完成后恢复，不与其他归档/写线程并行。
class FaultCodecvt final : public std::codecvt<char, char, std::mbstate_t> {
public:
    explicit FaultCodecvt(StreamFault& fault) : fault_(fault) {}
protected:
    result do_out(state_type&, const char* from, const char* end, const char*& next,
                  char* to, char* limit, char*& to_next) const override {
        if (fault_.write) { next=from; to_next=to; ++fault_.write_failures; return error; }
        return copy(from,end,next,to,limit,to_next);
    }
    result do_in(state_type&, const char* from, const char* end, const char*& next,
                 char* to, char* limit, char*& to_next) const override {
        return copy(from,end,next,to,limit,to_next);
    }
    result do_unshift(state_type&, char* to, char*, char*& next) const override {
        next=to; return ++fault_.closes==fault_.close_at ? error : ok;
    }
    bool do_always_noconv() const noexcept override { return false; }
    int do_encoding() const noexcept override { return 1; }
    int do_max_length() const noexcept override { return 1; }
    int do_length(state_type&, const char* from, const char* end, std::size_t max) const override {
        return static_cast<int>(std::min(static_cast<std::size_t>(end-from),max));
    }
private:
    static result copy(const char* from, const char* end, const char*& next,
                       char* to, char* limit, char*& to_next) {
        const auto n=std::min(end-from,limit-to);
        std::memcpy(to,from,static_cast<std::size_t>(n)); next=from+n; to_next=to+n;
        return next==end ? ok : partial;
    }
    StreamFault& fault_;
};
struct ScopedStreamFault {
    explicit ScopedStreamFault(StreamFault& fault)
        : old(std::locale::global(std::locale(std::locale(),new FaultCodecvt(fault)))) {}
    ~ScopedStreamFault() { std::locale::global(old); }
    std::locale old;
};
class HeldFile {
public:
    HeldFile(const std::filesystem::path& path, DWORD sharing)
        : handle_(CreateFileW(path.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)) {
        require(handle_!=INVALID_HANDLE_VALUE,"fixture file lock");
    }
    ~HeldFile() { CloseHandle(handle_); }
    HeldFile(const HeldFile&)=delete;
    HeldFile& operator=(const HeldFile&)=delete;
private:
    HANDLE handle_;
};
void start_archive(SessionArchive& archive, const std::filesystem::path& directory) {
    SessionArchiveConfig config; config.directory=directory.string();
    config.segment_interval=std::chrono::seconds(30);
    config.report_config.session_id="ordinary-publication-fixture";
    std::string error; require(archive.start(config,error),"archive start");
    wait_file(directory/"manifest.json");
}
void queue_marker(SessionArchive& archive, const std::string& label) {
    bool accepted=false;
    for (int i=0;i<100&&!accepted;++i) {
        accepted=archive.mark(label);
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(accepted,"marker accepted");
}
void mark_archive(SessionArchive& archive, const std::filesystem::path& directory) {
    queue_marker(archive,"synthetic ordinary marker"); wait_file(directory/"marker-1.json");
}
void failed_archive(const SessionArchive& archive, const std::filesystem::path& directory,
                    std::uint64_t markers) {
    const auto status=archive.status(); const auto manifest=json_file(directory/"manifest.json");
    require(!status.active && !status.last_error.empty(),"failure must remain visible after stop");
    require(status.accepted_samples==0 && status.written_samples==0 && status.dropped_samples==0,
            "marker-only fixture cannot invent sample counts");
    require(manifest["closed"]==true && manifest["complete"]==false &&
            manifest["last_error"]==status.last_error,"recovered final manifest must truthfully report failure");
    std::cout << "  marker_count=" << status.marker_count << " expected_published=" << markers << '\n';
    require(status.marker_count==markers && manifest["marker_count"]==markers,
            "marker count must equal successfully published markers");
}
void publisher_case(const std::filesystem::path& root, const std::string& kind) {
    std::filesystem::create_directory(root);
    const auto path=root/"manifest.json", temp=root/"manifest.json.tmp";
    const Json old={{"revision",1},{"closed",false}}, next={{"revision",2},{"closed",true}};
    publish(path,old); const auto original=bytes(path);
    if (kind=="normal") publish(path,next);
    else if (kind=="write" || kind=="close") {
        StreamFault fault; fault.write=kind=="write"; fault.close_at=kind=="close"?1:0;
        std::string error;
        { ScopedStreamFault scope(fault); error=failed_publish(path,next); }
        require(kind=="write" ? fault.write_failures>0 : fault.closes==1 && fault.write_failures==0,
                "selected actual stream failure must occur");
        require(error.find(kind=="write"?"写入":"关闭")!=std::string::npos,"correct stream failure stage");
        require(bytes(path)==original,"stream failure must preserve previous formal bytes");
        if (kind=="close") require(json_file(temp)==next,"close failed after complete JSON write/flush");
        publish(path,next);
    } else if (kind=="replace") {
        { HeldFile held(path,FILE_SHARE_READ|FILE_SHARE_WRITE); failed_publish(path,next); }
        require(bytes(path)==original && json_file(temp)==next,"real sharing failure preserves old file and staged JSON");
        publish(path,next);
    } else {
        std::filesystem::create_directory(temp); failed_publish(path,next);
        require(bytes(path)==original && std::filesystem::is_directory(temp),"open failure preserves old and obstruction");
        std::filesystem::rename(temp,root/"preserved-obstruction"); publish(path,next);
    }
    require(json_file(path)==next && !std::filesystem::exists(temp),"successful publish/retry has exact JSON and no pending file");
}
void marker_failure(const std::filesystem::path& root, bool replacement) {
    SessionArchive archive; start_archive(archive,root);
    const auto marker=root/"marker-1.json";
    if (replacement) {
        const Json sentinel={{"fixture","existing marker sentinel"}}; publish(marker,sentinel);
        const auto original=bytes(marker);
        { HeldFile held(marker,FILE_SHARE_READ|FILE_SHARE_WRITE);
          queue_marker(archive,"fails replacement"); archive.stop(); }
        require(bytes(marker)==original,"failed marker replacement preserves existing bytes");
    } else {
        std::filesystem::create_directory(root/"marker-1.json.tmp");
        queue_marker(archive,"fails open"); archive.stop();
        require(!std::filesystem::exists(marker),"failed marker write publishes no formal marker");
    }
    failed_archive(archive,root,0);
}
void marker_close_failure(const std::filesystem::path& root) {
    SessionArchive archive; start_archive(archive,root); mark_archive(archive,root);
    const auto marker=root/"marker-1.json"; const auto original=bytes(marker);
    StreamFault fault; fault.close_at=1;
    { ScopedStreamFault scope(fault); archive.stop(); }
    require(fault.closes>=2 && fault.write_failures==0,"marker close fails; final manifest retry uses real streams");
    require(bytes(marker)==original && json_file(marker)["closed"]==false,"failed seal preserves unclosed marker bytes");
    failed_archive(archive,root,1);
}
void marker_read_failure(const std::filesystem::path& root) {
    SessionArchive archive; start_archive(archive,root); mark_archive(archive,root);
    const auto marker=root/"marker-1.json"; const auto original=bytes(marker);
    { HeldFile held(marker,FILE_SHARE_DELETE); archive.stop(); }
    require(bytes(marker)==original,"reread failure cannot rewrite the original marker");
    failed_archive(archive,root,1);
    const auto stopped=bytes(root/"manifest.json"); archive.stop();
    require(bytes(root/"manifest.json")==stopped && bytes(marker)==original,"repeated stop is idempotent, not an undocumented retry");
}
void manifest_failure(const std::filesystem::path& root, bool close_once) {
    SessionArchive archive; start_archive(archive,root);
    const auto path=root/"manifest.json"; const auto original=bytes(path);
    StreamFault fault; fault.close_at=close_once?1:0; fault.write=!close_once;
    { ScopedStreamFault scope(fault); archive.stop(); }
    if (close_once) {
        require(fault.closes==2 && fault.write_failures==0,"final manifest close fails once and actual internal retry succeeds");
        failed_archive(archive,root,0);
        require(!std::filesystem::exists(root/"manifest.json.tmp"),"successful internal retry consumes staged file");
    } else {
        require(fault.write_failures>=2,"both final manifest write attempts fail");
        require(bytes(path)==original && json_file(path)["closed"]==false,"persistent write failure retains exact prior open manifest");
        require(!archive.status().active && !archive.status().last_error.empty(),"persistent write failure is visible in live status");
    }
}
void manifest_replace_failure(const std::filesystem::path& root) {
    SessionArchive archive; start_archive(archive,root);
    const auto path=root/"manifest.json"; const auto original=bytes(path);
    { HeldFile held(path,FILE_SHARE_READ|FILE_SHARE_WRITE); archive.stop(); }
    require(bytes(path)==original && json_file(path)["closed"]==false,"two failed replacements preserve prior manifest");
    require(!archive.status().active && !archive.status().last_error.empty(),"replacement failure remains visible");
    const auto staged=json_file(root/"manifest.json.tmp");
    require(staged["complete"]==false && staged["last_error"]==archive.status().last_error,"retry candidate includes the actual failure");
    // 单独验证同一个生产publish重试；stop并不承诺重放已失败的后台任务。
    publish(path,staged); require(json_file(path)==staged,"publisher retry preserves exact failure metadata");
}
void normal_marker(const std::filesystem::path& root) {
    SessionArchive archive; start_archive(archive,root); mark_archive(archive,root); archive.stop();
    const auto manifest=json_file(root/"manifest.json"), marker=json_file(root/"marker-1.json");
    require(manifest["complete"]==true && manifest["closed"]==true && manifest["marker_count"]==1 &&
            marker["closed"]==true && archive.status().marker_count==1,"normal marker and manifest agree");
}
}

int main(int argc, char** argv) {
    const auto root=argc==2 ? std::filesystem::u8path(argv[1]) : std::filesystem::temp_directory_path()/
        ("xen-archive-publication-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    try {
        require(!std::filesystem::exists(root),"fixture root must be new");
        std::filesystem::create_directories(root);
        int passed=0,failed=0;
        auto test=[&](const std::string& name, auto run) {
            try { run(root/name); ++passed; std::cout << "PASS " << name << '\n'; }
            catch (const std::exception& error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
        };
        for (const std::string kind : {"normal","write","close","replace","open"})
            test("publisher-"+kind,[&](const auto& path){publisher_case(path,kind);});
        test("marker-write-failure",[](const auto& path){marker_failure(path,false);});
        test("marker-replace-failure",[](const auto& path){marker_failure(path,true);});
        test("marker-close-failure",marker_close_failure);
        test("marker-reread-failure",marker_read_failure);
        test("manifest-close-once",[](const auto& path){manifest_failure(path,true);});
        test("manifest-write-persistent",[](const auto& path){manifest_failure(path,false);});
        test("manifest-replace-persistent",manifest_replace_failure);
        test("normal-marker",normal_marker);
        std::cout << "archive publication: " << passed << " scenarios passed, " << failed << " failed\n"
                  << "fixture root: " << root << '\n';
        return failed?1:0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
