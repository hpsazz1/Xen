#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "grenade_web/server.h"
#include "weapon/weapon.h"
#include "movement/timeline.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

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
        "网页不提供POST或控制接口");
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
    gsi->stop();
    auto invalid = config; invalid.bind_address = "203.0.113.8";
    expect(!server.start(invalid,gsi) && !server.snapshot().error.empty(),"公开地址绑定在创建监听前拒绝");
    expect(!server.start(config,nullptr),"缺少共享GSI不能启动隐藏接收器");
    for (const auto* file : {"t.json","ct.json","unknown.json","bad.json"}) std::filesystem::remove(root / file);
    std::filesystem::remove(root);
    return failures ? 1 : 0;
}
