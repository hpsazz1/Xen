#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#include "grenade_web/server.h"
#include "weapon/weapon.h"
#include "movement/timeline.h"
#include "debug_session/debug_session.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <array>
#include <opencv2/imgcodecs.hpp>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
struct Winsock {
    bool ready = false;
    Winsock() { WSADATA data{}; ready = WSAStartup(MAKEWORD(2,2),&data) == 0; }
    ~Winsock() { if (ready) WSACleanup(); }
};
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
SOCKET connect_to(std::uint16_t port) {
    const auto value = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (value == INVALID_SOCKET || connect(value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0) {
        if (value != INVALID_SOCKET) closesocket(value);
        return INVALID_SOCKET;
    }
    DWORD timeout = 2500;
    setsockopt(value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    setsockopt(value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    return value;
}
std::uint16_t unused_port() {
    Socket reservation{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (reservation.value == INVALID_SOCKET || bind(reservation.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0) return 0;
    int size = sizeof(address);
    if (getsockname(reservation.value,reinterpret_cast<sockaddr*>(&address),&size) != 0) return 0;
    return ntohs(address.sin_port);
}
struct Response {
    int status = 0;
    std::string headers, body;
    Json json() const { return Json::parse(body,nullptr,false); }
};
Response http_exchange(std::uint16_t port, const std::string& request) {
    Socket client{connect_to(port)};
    if (client.value == INVALID_SOCKET) return {};
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto count = send(client.value,request.data() + sent,static_cast<int>(request.size() - sent),0);
        if (count <= 0) return {};
        sent += static_cast<std::size_t>(count);
    }
    std::string raw;
    for (;;) {
        char bytes[8192];
        const auto count = recv(client.value,bytes,sizeof(bytes),0);
        if (count == 0) break;
        if (count < 0 || raw.size() + count > 2 * 1024 * 1024) return {};
        raw.append(bytes,static_cast<std::size_t>(count));
    }
    const auto boundary = raw.find("\r\n\r\n");
    if (boundary == std::string::npos || !raw.starts_with("HTTP/1.1 ")) return {};
    Response response;
    const auto result = std::from_chars(raw.data()+9,raw.data()+12,response.status);
    if (result.ec != std::errc{}) return {};
    response.headers = raw.substr(0,boundary);
    response.body = raw.substr(boundary+4);
    return response;
}
Response get(std::uint16_t port, const std::string& target) {
    return http_exchange(port,"GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
}
Response select_plan(std::uint16_t port, const std::string& plan_id, const std::string& generation) {
    const auto body=Json{{"plan_id",plan_id},{"generation",generation}}.dump();
    return http_exchange(port,"POST /select.json HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body);
}
std::string image_hash(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("测试摘要创建失败");
    std::array<unsigned char,32> digest{};
    const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);
    if (status<0) throw std::runtime_error("测试摘要计算失败");
    std::string result;
    for (const auto byte:digest) {result+="0123456789abcdef"[byte>>4];result+="0123456789abcdef"[byte&15];}
    return result;
}
std::uint64_t utc_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
bool post_context(std::uint16_t port, std::uint64_t timestamp, const std::string& map,
                  const std::string& team, bool local = true, int health = 100) {
    const Json document = {{"provider",{{"appid",730},{"steamid","76561198000000000"},{"timestamp",timestamp}}},
        {"map",{{"name",map},{"phase","live"}}},
        {"player",{{"steamid",local ? "76561198000000000" : "76561198000000001"},{"activity","playing"},
            {"team",team},{"state",{{"health",health}}},
            {"weapons",{{"weapon_0",{{"name","weapon_smokegrenade"},{"type","Grenade"},{"state","active"}}}}}}}};
    const auto body = document.dump();
    return http_exchange(port,"POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body).status == 200;
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(),value.end()};
}
void write_plan(const std::filesystem::path& path, const std::string& name, const std::string& map) {
    movement::timeline::Plan plan;
    plan.name = name; plan.map = map; plan.voice_command = "";
    plan.actions = {{movement::timeline::ActionKind::KEY,movement::timeline::Key::SPACE,0,100,25}};
    std::ofstream file(path,std::ios::binary);
    file << movement::timeline::plan_json(plan).dump();
    if (!file) throw std::runtime_error("测试方案写入失败");
}
void selection_tests(const std::filesystem::path& root) {
    const auto base=root/"mobile-selection", plans=base/"plans", resources=base/"references"/"t";
    std::filesystem::create_directories(plans); std::filesystem::create_directories(resources);
    const auto path=plans/"t.json";
    write_plan(path,"T1 VIP烟","de_mirage");
    write_plan(plans/"ct.json","CT1 B烟","de_mirage");
    Json plan; {std::ifstream file(path);file>>plan;}
    cv::imwrite((resources/"standing.png").string(),cv::Mat(360,640,CV_8UC3,cv::Scalar(10,20,30)));
    cv::imwrite((resources/"aim.png").string(),cv::Mat(320,320,CV_8UC3,cv::Scalar(30,20,10)));
    Json reference{{"schema_version",1},{"plan_id","t.json"},{"reference_id","t"},{"manual_throw_verified",true},
        {"plan",plan},{"standing_image","standing.png"},{"aim_image","aim.png"},{"standing_note","站在T1"},
        {"standing_marker",{{"x",0.2},{"y",0.4},{"label","T1"}}},
        {"geometry",{{"source_width",320},{"source_height",320},{"roi_x",0},{"roi_y",0},
            {"roi_width",320},{"roi_height",320},{"scale_x",1},{"scale_y",1}}},
        {"image_sha256",{{"standing_image",image_hash(resources/"standing.png")},{"aim_image",image_hash(resources/"aim.png")}}}};
    {std::ofstream file(resources/"reference.json");file<<reference.dump();}
    auto gsi=std::make_shared<weapon::GsiReceiver>();
    weapon::GsiConfig gsi_config;gsi_config.enabled=true;gsi_config.port=unused_port();gsi_config.ttl_ms=3000;
    expect(gsi->start(gsi_config),"手机选择夹具启动共享GSI");
    grenade_web::Server server;grenade_web::Config config;config.bind_address="127.0.0.1";config.port=0;config.plans_root=utf8(plans);
    expect(server.start(config,gsi),"手机选择使用生产HTTP服务");const auto port=server.snapshot().port;
    const auto timestamp=utc_seconds();
    expect(post_context(gsi_config.port,timestamp,"de_mirage","T"),"手机选择建立T阵营");
    const auto generation=get(port,"/context.json").json().value("generation",std::string{});
    expect(select_plan(port,"../t.json",generation).status==409 && select_plan(port,"ct.json",generation).status==409,
        "选择不能跳目录或串阵营");
    const auto pending=select_plan(port,"t.json",generation);
    expect(pending.status==202 && pending.json().value("state",std::string{})=="pending","HTTP提交只进入待处理，不冒称选中");
    const auto request=server.poll_selection();
    expect(request && request->plan_id=="t.json" && std::filesystem::u8path(request->plan_path)==path &&
        server.selection_current(*request) && !server.poll_selection(),"App只收到一次可信库路径请求");
    expect(select_plan(port,"t.json",generation).status==409,"在途请求不排队");
    if (request) server.finish_selection(*request,true);
    expect(get(port,"/selection.json").json().value("state",std::string{})=="selected","仅App完成后发布已选中");
    debug_session::Snapshot selected;selected.movement_test_enabled=true;selected.movement_test_mode=debug_session::Mode::GRENADE_TEST;
    selected.grenade_selected_path=utf8(path);selected.grenade_current=plan;selected.grenade_baseline=plan;
    selected.grenade_guidance_state="aligning";selected.grenade_guidance_plan_id="t.json";selected.grenade_guidance_message="正在对齐";
    server.publish_capture(std::make_shared<const debug_session::Snapshot>(selected),AppConfig{},true);
    const auto guidance=get(port,"/selection.json").json();
    expect(guidance.value("actual_plan_id",std::string{})=="t.json" && guidance["guidance"]["state"]=="aligning",
        "手机读取实际Session选择和对齐状态，不需要主机GUI");
    const auto info=get(port,"/reference.json?plan_id=t.json");
    expect(info.status==200 && info.json().value("standing_width",0)==640 && info.json()["geometry"]["source_width"]==320 &&
        info.json()["standing_marker"]["x"]==0.2,"参考图复用生产契约，站位与瞄点尺寸独立且输出标记");
    expect(get(port,"/reference/standing.png?plan_id=t.json").headers.find("Content-Type: image/png")!=std::string::npos &&
        get(port,"/reference/aim.png?plan_id=t.json&revision="+info.json().value("revision",std::string{})).status==200,
        "双图固定入口返回PNG并支持版本参数");
    reference["standing_note"]="新采集站位";
    {std::ofstream file(resources/"reference.json");file<<reference.dump();}
    const auto refreshed=get(port,"/reference.json?plan_id=t.json").json();
    expect(refreshed.value("revision",std::string{})!=info.json().value("revision",std::string{}) &&
        refreshed.value("standing_note",std::string{})=="新采集站位","F10替换资源后轮询发现新版，无需重新选择方案");
    expect(get(port,"/reference/aim.png?plan_id=t.json&revision="+info.json().value("revision",std::string{})).status==404 &&
        get(port,"/reference/aim.png?plan_id=t.json&revision="+refreshed.value("revision",std::string{})).status==200,
        "图片与标记使用同一资源版本，旧版本地址不返回新图");
    expect(get(port,"/reference.json?plan_id=%2e%2e%2ft.json").status==404 &&
        get(port,"/reference/config.ini?plan_id=t.json").status==404,"参考图片路由不能读任意路径");
    {std::ofstream file(resources/"aim.png",std::ios::app|std::ios::binary);file<<'x';}
    expect(get(port,"/reference/aim.png?plan_id=t.json").status==404,"原图变化不得继续显示未核对旧资源");
    expect(select_plan(port,"t.json",generation).status==202,"前次完成后接受新选择");
    const auto old=server.poll_selection();
    expect(post_context(gsi_config.port,timestamp+1,"de_mirage","CT"),"在途选择时换边");
    expect(old && !server.selection_current(*old),"App提交前立即观察GSI换边");
    get(port,"/context.json");
    if(old)server.finish_selection(*old,true);
    expect(get(port,"/selection.json").json().value("state",std::string{})=="failed","旧回执不能在换边后变成成功");
    expect(post_context(gsi_config.port,timestamp+2,"de_mirage","T"),"重新回到T阵营");
    const auto restored=get(port,"/context.json").json().value("generation",std::string{});
    expect(restored!=generation && select_plan(port,"t.json",generation).status==409,"T到CT再T不能复活旧请求");
    expect(select_plan(port,"t.json",restored).status==202,"新页面身份重新选择");
    const auto current=server.poll_selection();if(old)server.finish_selection(*old,false,"过期回执");
    expect(current && server.selection_current(*current),"旧完成回执不覆盖新在途请求");
    if(current)server.finish_selection(*current,false,"尚有未保存修改");
    expect(get(port,"/selection.json").json().value("reason",std::string{})=="尚有未保存修改","手机显示App返回的选择失败原因");
    server.stop();gsi->stop();
    for(const auto* name:{"standing.png","aim.png","reference.json"})std::filesystem::remove(resources/name);
    std::filesystem::remove(resources);std::filesystem::remove(base/"references");
    std::filesystem::remove(path);std::filesystem::remove(plans/"ct.json");std::filesystem::remove(plans);std::filesystem::remove(base);
}
void capture_context_tests(const std::filesystem::path& root) {
    const auto directory = root / "capture-context";
    std::filesystem::create_directory(directory);
    const auto path = directory / "selected.json";
    const std::string raw = R"({"schema_version":3,"name":"T1 VIP","map":"de_mirage","location":"","notes":"","voice_command":"","actions":[{"type":"key","key":"SPACE","start_ms":0,"duration_ms":100}],"mask":{"enabled":false,"wasd":0,"start_ms":-1,"duration_ms":0}})";
    { std::ofstream file(path,std::ios::binary); file << raw; }
    auto gsi = std::make_shared<weapon::GsiReceiver>();
    weapon::GsiConfig gsi_config; gsi_config.enabled = true; gsi_config.port = unused_port(); gsi_config.ttl_ms = 700;
    expect(gsi->start(gsi_config),"F10只读上下文使用独立回环GSI夹具");
    grenade_web::Server server;
    grenade_web::Config config; config.bind_address = "127.0.0.1"; config.port = 0; config.plans_root = utf8(directory);
    expect(server.start(config,gsi),"F10只读上下文服务启动");
    const auto port = server.snapshot().port;
    AppConfig controls;
    debug_session::Snapshot selected;
    selected.movement_test_enabled = true; selected.movement_test_mode = debug_session::Mode::GRENADE_TEST;
    selected.grenade_selected_path = utf8(path); selected.grenade_current = Json::parse(raw);
    selected.grenade_baseline = selected.grenade_current;
    const auto publish = [&](bool available = true) {
        server.publish_capture(std::make_shared<const debug_session::Snapshot>(selected),controls,available,"Runtime尚未停止");
    };
    const auto read = [&] { return get(port,"/capture-context.json").json(); };
    const auto rejected = [&] {
        const auto value = read();
        expect(value.is_object() && !value.value("ready",true) && value.value("remaining_ms",1)==0 &&
            value["plan"].is_object() && value["plan"].empty() && value.value("plan_id",std::string("old")).empty() &&
            value.value("plan_sha256",std::string("old")).empty() && !value.value("reason",std::string{}).empty(),
            "未就绪的F10上下文清空方案和摘要并给出原因");
        return value;
    };
    rejected();
    publish(); rejected();
    const auto timestamp = utc_seconds();
    expect(post_context(gsi_config.port,timestamp,"de_mirage","T"),"F10夹具建立有效本地GSI");
    const auto ready = read();
    expect(ready.value("ready",false) && ready.value("plan_id",std::string{})=="selected.json" &&
        ready["plan"]==Json::parse(raw) && ready.value("plan_sha256",std::string{})==
            "8bfc1ece2235cf8cea469335b9ee07126792aac7b822b36972defe94450b24ac" &&
        ready.value("remaining_ms",0)>0 && ready.value("remaining_ms",701)<=700 && ready.size()==9,
        "F10返回实际启用方案的原JSON和SHA，不暴露路径或其他Session字段");
    publish();
    expect(read()["generation"]==ready["generation"],"相同选择与心跳刷新不制造新采集身份");
    selected.grenade_current["name"] = "T1 未保存"; publish(); rejected();
    selected.grenade_current = selected.grenade_baseline; publish();
    expect(read().value("ready",false) && read()["generation"]!=ready["generation"],"脏草稿恢复不能复活旧采集身份");
    selected.busy = true; selected.physical = true; selected.state = debug_session::State::RUNNING; publish(); rejected();
    selected.busy = false; selected.state = debug_session::State::COMPLETED; publish();
    expect(read().value("ready",false),"试投成功且已清理后允许F10，physical历史属性不阻断采图");
    selected.cleanup_unknown = true; publish(); rejected(); selected.cleanup_unknown = false;
    selected.movement_test_enabled = false; publish(); rejected(); selected.movement_test_enabled = true;
    selected.movement_test_mode = debug_session::Mode::MOVEMENT_TEST; publish(); rejected();
    selected.movement_test_mode = debug_session::Mode::GRENADE_TEST;
    publish(false); rejected();
    controls.keyboard.debug_test_enabled = true; controls.keyboard.debug_test_virtual_keys = {0x79}; publish();
    expect(rejected().value("reason",std::string{}).find("F10")!=std::string::npos,"设备侧自定义F10与采集热键冲突时拒绝");
    controls.keyboard.debug_test_enabled = false; publish();
    expect(read().value("ready",false),"未启用的F10草稿绑定不占用采集键");
    controls.trigger.enabled = true; controls.trigger.fire_enabled = true; controls.trigger.hold_virtual_key = 0x79; publish(); rejected();
    controls.trigger.enabled = false; publish();
    const auto before_bytes_change = read();
    { std::ofstream file(path,std::ios::binary | std::ios::app); file << '\n'; }
    const auto bytes_changed = read();
    expect(bytes_changed.value("ready",false) && bytes_changed["plan"]==ready["plan"] &&
        bytes_changed["plan_sha256"]!=before_bytes_change["plan_sha256"] &&
        bytes_changed["generation"]!=before_bytes_change["generation"],"源文件仅格式变化仍更新原始字节摘要与采集身份");
    write_plan(path,"T1 已变更","de_mirage"); rejected();
    { std::ofstream file(path,std::ios::binary); file << raw; }
    selected.grenade_selected_path = utf8(root / "t.json"); publish(); rejected();
    selected.grenade_selected_path = utf8(path); publish();
    expect(post_context(gsi_config.port,timestamp+1,"de_nuke","T"),"F10换图输入成功"); rejected();
    expect(post_context(gsi_config.port,timestamp+2,"de_mirage","T"),"F10地图恢复输入成功"); publish();
    expect(read().value("ready",false),"地图恢复后重新取得当前采集上下文");
    std::this_thread::sleep_for(1050ms);
    expect(post_context(gsi_config.port,timestamp+3,"de_mirage","T"),"只刷新GSI而不续App心跳");
    expect(rejected().value("reason",std::string{}).find("界面状态")!=std::string::npos,"App心跳过期时有效GSI不能续命旧选择");
    publish(); expect(read().value("ready",false),"App新心跳恢复当前选择");
    std::this_thread::sleep_for(750ms); publish(); rejected();
    gsi->stop(); publish(); rejected();
    server.stop();
    expect(server.start(config,gsi),"F10服务对象可以重启");
    expect(!get(server.snapshot().port,"/capture-context.json").json().value("ready",true),"服务重启不继承旧App选择心跳");
    server.stop();
    std::filesystem::remove(path); std::filesystem::remove(directory);
}
void catalog_limits(const std::filesystem::path& root, const std::shared_ptr<weapon::GsiReceiver>& gsi) {
    const auto directory = root / "catalog-limits";
    std::filesystem::create_directory(directory);
    grenade_web::Config config; config.bind_address = "127.0.0.1"; config.port = 0; config.plans_root = utf8(directory);
    grenade_web::Server server;
    const auto rejected = [&](const char* reason) {
        expect(server.start(config,gsi),"预算用例启动同一生产网页服务");
        const auto begin = Clock::now();
        const auto result = get(server.snapshot().port,"/catalog.json");
        expect(result.status == 503 && Clock::now()-begin < 2s &&
            server.snapshot().error.find(reason) != std::string::npos,"超预算清单快速拒绝且保留明确原因");
        expect(get(server.snapshot().port,"/context.json").status == 200,"超预算只影响清单，状态接口仍可用");
        const auto before_stop = Clock::now();
        server.stop();
        expect(!server.snapshot().running && Clock::now()-before_stop < 500ms,"超预算扫描后仍可正常停止");
    };
    // 连不参与解析的条目也计入遍历预算，防止大量临时文件拖住退出。
    for (int i=0;i<513;++i) { std::ofstream file(directory / (std::to_string(i) + ".tmp")); }
    rejected("512");
    for (int i=0;i<513;++i) std::filesystem::remove(directory / (std::to_string(i) + ".tmp"));
    const auto oversized = directory / "oversized.json";
    { std::ofstream file(oversized,std::ios::binary); }
    std::filesystem::resize_file(oversized,16 * 1024 * 1024 + 1);
    rejected("16MiB");
    std::filesystem::remove(oversized);
    // 各文件均小于单文件上限，坏JSON也不能绕过累计读取预算。
    for (int i=0;i<3;++i) {
        const auto path = directory / (std::to_string(i) + ".json");
        { std::ofstream file(path,std::ios::binary); }
        std::filesystem::resize_file(path,12 * 1024 * 1024);
    }
    rejected("32MiB");
    for (int i=0;i<3;++i) std::filesystem::remove(directory / (std::to_string(i) + ".json"));
    write_plan(directory / "valid.json","Mirage T1 有效方案","de_mirage");
    { std::ofstream file(directory / "bad.json"); file << "{\"name\":\"a\",\"name\":\"b\"}"; }
    expect(server.start(config,gsi),"预算失败后可重启并重新读取已修复的目录");
    const auto restored = get(server.snapshot().port,"/catalog.json").json();
    expect(restored.is_array() && restored.size()==1 && restored[0].value("id",std::string{})=="valid.json",
        "有效方案仍用生产解析器读取，重复字段坏文件跳过");
    server.stop();
    std::filesystem::remove(directory / "valid.json");
    std::filesystem::remove(directory / "bad.json");
    std::filesystem::remove(directory);
}
int preview(int argc, char** argv) {
    grenade_web::Config config; config.bind_address = "127.0.0.1"; config.port = 0;
    std::string root;
    int seconds = 0;
    for (int i=1;i+1<argc;i+=2) {
        const std::string option = argv[i];
        if (option == "--preview-root") root = argv[i+1];
        else {
            int value = 0; const std::string text = argv[i+1];
            const auto parsed = std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) return 2;
            if (option == "--preview-port" && value >= 0 && value <= 65535) config.port = static_cast<std::uint16_t>(value);
            else if (option == "--preview-seconds" && value > 0 && value <= 600) seconds = value;
            else return 2;
        }
    }
    if (argc % 2 != 1 || root.empty() || !seconds || !std::filesystem::is_directory(std::filesystem::u8path(root))) return 2;
    config.plans_root = root;
    auto gsi = std::make_shared<weapon::GsiReceiver>();
    weapon::GsiConfig gsi_config; gsi_config.enabled = true; gsi_config.port = unused_port();
    if (!gsi->start(gsi_config)) return 2;
    grenade_web::Server server;
    if (!server.start(config,gsi)) return 2;
    std::cout << "本机合成GSI只读预览；非真实游戏或设备验收。http://127.0.0.1:" << server.snapshot().port << "/\n" << std::flush;
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        if (!post_context(gsi_config.port,utc_seconds(),"de_mirage","T")) return 2;
        std::this_thread::sleep_for(500ms);
    }
    server.stop(); gsi->stop();
    return 0;
}
}

int main(int argc, char** argv) {
    Winsock winsock;
    if (!winsock.ready) return 1;
    if (argc > 1 && std::string(argv[1]).starts_with("--")) return preview(argc,argv);
    const auto base = argc > 1 ? std::filesystem::u8path(argv[1]) :
        std::filesystem::path("temp/XEN-GRENADE-ALIGN-20261026-001/web-fixtures");
    const auto root = std::filesystem::absolute(base / ("run-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()))).lexically_normal();
    std::filesystem::create_directories(root);
    write_plan(root / "t.json","Mirage T1 VIP R2","de_mirage");
    write_plan(root / "ct.json","Mirage CT来源5 B小烟","de_mirage");
    write_plan(root / "unknown.json","未标阵营","de_nuke");
    { std::ofstream file(root / "bad.json"); file << "{}"; }
    auto gsi = std::make_shared<weapon::GsiReceiver>();
    weapon::GsiConfig gsi_config; gsi_config.enabled = true; gsi_config.port = unused_port(); gsi_config.ttl_ms = 700;
    expect(gsi_config.port && gsi->start(gsi_config),"生产GSI回环接收器启动");
    grenade_web::Server server;
    grenade_web::Config config; config.bind_address = "127.0.0.1"; config.port = 0; config.plans_root = utf8(root);
    expect(server.start(config,gsi),"注入共享GSI启动网页，不初始化Runtime或设备");
    const auto started = server.snapshot();
    if (!started.running || !started.port) return 1;
    expect(started.urls == std::vector<std::string>{"http://127.0.0.1:" + std::to_string(started.port) + "/"},"实际端口和可用网址可查询");
    const auto page = get(started.port,"/");
    expect(page.status == 200 && page.body.find("<html") != std::string::npos &&
        page.headers.find("Cache-Control: no-store") != std::string::npos &&
        page.headers.find("Content-Length: " + std::to_string(page.body.size())) != std::string::npos,
        "内嵌页面GET成功，响应无缓存且长度按实际字节计算");
    const auto waiting = get(started.port,"/context.json").json();
    expect(waiting.is_object() && !waiting.value("valid",true) && waiting.value("map",std::string("old")).empty() &&
        waiting.value("team",std::string("old")).empty() && waiting.value("remaining_ms",1)==0,"无GSI时明确等待，不猜地图或阵营");
    const auto catalog = get(started.port,"/catalog.json").json();
    expect(catalog.is_array() && catalog.size()==3,"正式库包含空语音口令方案，坏文件不进入清单");
    if (catalog.is_array()) for (const auto& item : catalog) {
        expect(item.size()==4 && item.contains("id") && item.contains("name") && item.contains("map") && item.contains("team"),
            "清单只有白名单显示字段，不提供文件路径或动作内容");
        const auto id = item.value("id",std::string{});
        expect(item.value("team",std::string("bad")) == (id=="t.json" ? "T" : id=="ct.json" ? "CT" : ""),
            "方案阵营只由名称明确来源编号识别");
    }
    const auto timestamp = utc_seconds();
    expect(post_context(gsi_config.port,timestamp,"de_mirage","T"),"实际回环HTTP向共享GSI发送本地T状态");
    const auto t = get(started.port,"/context.json").json();
    expect(t.is_object() && t.value("valid",false) && t.value("map",std::string{})=="de_mirage" &&
        t.value("team",std::string{})=="T" && t.value("remaining_ms",0)>0 && t.value("remaining_ms",701)<=700,
        "网页直接读取共享GSI的地图、阵营和源剩余期限");
    std::this_thread::sleep_for(25ms);
    const auto later = get(started.port,"/context.json").json();
    expect(later.is_object() && later.value("remaining_ms",701)<t.value("remaining_ms",0),"网页读取不延长GSI有效期");
    expect(post_context(gsi_config.port,timestamp+1,"de_mirage","CT",true,0),"本地死亡且新秒换边输入成功");
    const auto ct = get(started.port,"/context.json").json();
    expect(ct.is_object() && ct.value("team",std::string{})=="CT" && ct.value("epoch",std::string{})==t.value("epoch",std::string("old")) &&
        ct.value("generation",std::string{})!=t.value("generation",std::string{}) && !gsi->snapshot().valid,
        "死亡仍显示己方阵营，换边更新展示代际且不改变武器有效性");
    expect(post_context(gsi_config.port,timestamp+2,"de_mirage","T"),"新秒恢复T输入成功");
    std::this_thread::sleep_for(25ms);
    const auto restored = get(started.port,"/context.json").json();
    expect(restored.is_object() && restored.value("team",std::string{})=="T" &&
        restored.value("generation",std::string{})!=t.value("generation",std::string{}),"T到CT再T不能恢复旧详情连续性");
    expect(post_context(gsi_config.port,timestamp+3,"de_mirage","CT",false),"旁观目标输入已被接收");
    const auto spectator = get(started.port,"/context.json").json();
    expect(spectator.is_object() && !spectator.value("valid",true) && spectator.value("team",std::string("old")).empty(),
        "身份不符不采用旁观目标阵营");
    expect(post_context(gsi_config.port,timestamp+3,"de_mirage","T"),"本地身份恢复");
    std::this_thread::sleep_for(750ms);
    const auto expired = get(started.port,"/context.json").json();
    expect(expired.is_object() && !expired.value("valid",true) && expired.value("remaining_ms",1)==0 &&
        expired.value("map",std::string("old")).empty() && expired.value("team",std::string("old")).empty(),"源TTL到期清空地图和阵营");
    expect(http_exchange(started.port,"POST /context.json HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n").status==405,
        "非选择接口不接受POST");
    expect(get(started.port,"/../config.ini").status==404 && get(started.port,"/catalog.json?path=../config.ini").status==404,
        "任意路径和查询参数不能读取文件");
    expect(http_exchange(started.port,"GET /context.json HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n\r\nx").status==400,
        "只读GET不接受请求体");
    expect(http_exchange(started.port,"GET / HTTP/1.1\r\nX-Fill: " + std::string(9000,'x') + "\r\n\r\n").status==431,
        "超限请求头有界拒绝");
    Socket slow{connect_to(started.port)};
    const std::string incomplete = "GET /context.json HTTP/1.1\r\nHost: ";
    send(slow.value,incomplete.data(),static_cast<int>(incomplete.size()),0);
    std::this_thread::sleep_for(20ms);
    const auto before_stop = Clock::now(); server.stop();
    expect(!server.snapshot().running && Clock::now()-before_stop < 500ms,"停止打断未完成请求并回收工作线程");
    expect(server.start(config,gsi),"同一网页对象可以重新启动");
    const auto restarted = get(server.snapshot().port,"/context.json").json();
    expect(restarted.is_object() && restarted.value("generation",std::string{})!=expired.value("generation",std::string{}),
        "服务重启生成新的展示身份");
    server.stop();
    expect(post_context(gsi_config.port,timestamp+4,"de_mirage","T") && gsi->map_snapshot().valid,"网页停止不关闭共享GSI");
    catalog_limits(root,gsi);
    capture_context_tests(root);
    selection_tests(root);
    gsi->stop();
    auto invalid = config; invalid.bind_address = "203.0.113.8";
    expect(!server.start(invalid,gsi) && !server.snapshot().error.empty(),"公开地址绑定在创建监听前拒绝");
    expect(!server.start(config,nullptr),"缺少共享GSI不能启动隐藏接收器");
    for (const auto* file : {"t.json","ct.json","unknown.json","bad.json"}) std::filesystem::remove(root / file);
    std::filesystem::remove(root);
    return failures ? 1 : 0;
}
