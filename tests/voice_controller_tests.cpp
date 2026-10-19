#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "voice_switch/controller.h"
#include "voice_switch/voice_switch.h"
#include "debug_session/debug_session.h"
#include "debug_session/movement_library_internal.h"
#include "movement/timeline.h"
#include "keyboard/keyboard.h"
#include "runtime/runtime.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
namespace {
using Json = nlohmann::json;
class FakeDevice final : public IMouseController {
public:
    int outputs = 0;
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++outputs; return {}; }
    MouseMoveReceipt scroll_wheel(int) noexcept override { ++outputs; return {}; }
    ButtonReceipt set_left_button(bool) noexcept override { ++outputs; return {}; }
    ButtonReceipt set_right_button(bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_space_key(bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_mask(std::uint8_t,bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++outputs; return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool poll_input(InputSnapshot& state) noexcept override { state = {}; state.state_valid = true; state.status = InputMonitorStatus::READY; return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
};
int failures = 0;
void expect(bool ok, const char* text) { if (!ok) { ++failures; std::cerr << text << '\n'; } }
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("xen-voice-controller-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto original_cwd = std::filesystem::current_path();
    std::filesystem::current_path(root);
    movement::timeline::Plan plan; plan.name = "跳台"; plan.map = "de_dust2"; plan.voice_command = "跳台";
    plan.actions = {{movement::timeline::ActionKind::KEY,movement::timeline::Key::SPACE,0,100,25}};
    const auto root_u8 = (root / "cache" / "movement" / "plans").u8string(); const std::string library(root_u8.begin(),root_u8.end());
    const auto saved_path = debug_session::detail::save_movement_plan(movement::timeline::plan_json(plan),"",false,library);
    debug_session::Session session; debug_session::Context context;
    context.map.valid = true; context.map.map_name = "de_dust2"; context.map.context_epoch = 1;
    context.runtime_idle = true; context.input_recording_idle = true; context.cleanup_known = true;
    debug_session::Request request; request.mode = debug_session::Mode::MOVEMENT_TEST; request.movement_library_root = library;
    expect(session.dispatch(debug_session::Action::LIST_MOVEMENT_PLANS,request,context),"生产Session刷新目录");
    for (int i=0;i<400 && session.busy();++i) { session.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    session.poll(); expect(session.snapshot()->movement_saved_plans.size()==1,"语音候选来自已保存方案");
    voice_switch::Server server; voice_switch::Config config; config.port = 0;
    if (!server.start(config)) return 1;
    // App 持有接收器，Runtime 的停止和析构均不能关闭其监听端口。
    auto shared_gsi = std::make_shared<weapon::GsiReceiver>();
    weapon::GsiConfig gsi_config; gsi_config.enabled = true;
    SOCKET reservation = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in reserved{}; reserved.sin_family = AF_INET; reserved.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int reserved_size = sizeof(reserved);
    bind(reservation,reinterpret_cast<sockaddr*>(&reserved),sizeof(reserved));
    getsockname(reservation,reinterpret_cast<sockaddr*>(&reserved),&reserved_size);
    gsi_config.port = ntohs(reserved.sin_port); closesocket(reservation);
    expect(shared_gsi->start(gsi_config),"App共享GSI启动");
    auto post_map = [&](const std::string& name) {
        SOCKET sender = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port=htons(gsi_config.port);
        endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(connect(sender,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint)) != 0) { closesocket(sender); return false; }
        DWORD timeout=2000;setsockopt(sender,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        const auto timestamp=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Json value={{"provider",{{"appid",730},{"steamid","76561198000000000"},{"timestamp",timestamp}}},
                    {"player",{{"steamid","76561198000000000"},{"activity","textinput"}}},{"map",{{"name",name}}}};
        const auto body=value.dump();
        const auto request="POST / HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        ::send(sender,request.data(),static_cast<int>(request.size()),0);char response[256];
        const int received=recv(sender,response,sizeof(response),0);closesocket(sender);
        return received>0 && shared_gsi->map_snapshot().valid && shared_gsi->map_snapshot().map_name==name;
    };
    { Runtime consumer(shared_gsi); consumer.stop(); expect(post_map("de_dust2"),"Runtime停止后共享地图仍接收真实HTTP"); }
    expect(post_map("de_inferno"),"Runtime析构后共享地图仍接收真实HTTP");
    shared_gsi->stop();
    SOCKET client = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_port=htons(server.snapshot().port); inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
    if (connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0) return 1;
    u_long nonblocking = 1; ioctlsocket(client,FIONBIO,&nonblocking);
    voice_switch::Controller controller(server,session);
    KeyboardConfig keyboard; keyboard.voice_virtual_keys={'V'};
    KeyboardPollResult input; input.input_healthy=true; input.capture_state_valid=true;
    bool blocked=false,capture=false;
    auto tick = [&] { session.poll(); session.update_map(context.map); controller.tick(context,input,keyboard,blocked,capture); input.events.clear(); };
    std::string buffer;
    auto receive = [&](const std::string& type) {
        for (int i=0;i<400;++i) {
            tick(); char chunk[4096]; const int size=recv(client,chunk,sizeof(chunk),0);
            if(size>0)buffer.append(chunk,static_cast<std::size_t>(size));
            for (;;) { const auto n=buffer.find('\n'); if(n==std::string::npos)break;
                auto value=Json::parse(buffer.substr(0,n)); buffer.erase(0,n+1);
                if(value.value("type",std::string{})==type)return value;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return Json{};
    };
    auto send = [&](Json value) { const auto line=value.dump()+"\n"; ::send(client,line.data(),static_cast<int>(line.size()),0); };
    const auto catalog=receive("catalog"); expect(catalog.is_object() && catalog["entries"].size()==1,"连接同步当前地图候选");
    if(!catalog.is_object()) {closesocket(client);return 1;}
    const auto revision=catalog["revision"];
    send({{"type","ready"},{"epoch",1},{"revision",revision}});
    for(int i=0;i<10;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    input.capture_virtual_keys['V']=true; input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,true}};tick();
    const auto begin=receive("begin"); expect(begin.is_object(),"就绪后新按下开始唯一轮次");
    if(!begin.is_object()){closesocket(client);return 1;}
    input.capture_virtual_keys['V']=false;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,false}};tick();
    expect(receive("end").value("id",0ULL)==begin["id"].get<std::uint64_t>(),"松开提交同一轮次");
    send({{"type","select"},{"id",begin["id"]},{"map","de_dust2"},{"epoch",1},{"revision",revision},{"plan_id",catalog["entries"][0]["id"]}});
    const auto result=receive("result");expect(result.is_object()&&!result.value("ok",true),"未启用测试明确拒绝且不隐式启用");
    expect(!session.snapshot()->movement_test_enabled,"语音没有重新武装测试");
    auto fake = std::make_shared<FakeDevice>(); context.device = fake;
    context.config.mouse.backend = MouseBackend::KMBOX_NET; context.config.mouse.allow_send_input = true;
    request.load_path = saved_path;
    expect(session.dispatch(debug_session::Action::LOAD_PLAN,request,context),"手动载入建立干净基线");
    for(int i=0;i<400 && session.busy();++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    request.plan_text = movement::timeline::plan_json(plan).dump();
    expect(session.dispatch(debug_session::Action::ENABLE_MOVEMENT_TEST,request,context),"显式启用FakeDevice测试许可");
    for(int round=0;round<2;++round) {
        input.capture_virtual_keys['V']=false;tick();
        input.capture_virtual_keys['V']=true;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,true}};tick();
        const auto successful_begin=receive("begin");expect(successful_begin.is_object(),"干净方案可以开始语音轮次");
        if(!successful_begin.is_object())break;
        expect(successful_begin["revision"]==revision,"手动载入与自身语音载入不推进候选版本");
        input.capture_virtual_keys['V']=false;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,false}};tick();
        expect(receive("end").value("id",0ULL)==successful_begin["id"].get<std::uint64_t>(),"成功轮次松开身份一致");
        send({{"type","select"},{"id",successful_begin["id"]},{"map","de_dust2"},{"epoch",1},{"revision",revision},{"plan_id",catalog["entries"][0]["id"]}});
        const auto success=receive("result");
        expect(success.is_object()&&success.value("ok",false)&&success["plan_id"]==catalog["entries"][0]["id"],"生产TCP到Session异步提交回执成功且身份匹配");
        const auto selected=session.snapshot();
        expect(selected->movement_voice_selected && selected->movement_voice_epoch==1 && selected->repeat_ready &&
               selected->movement_selected_path==saved_path && selected->movement_current==selected->movement_baseline &&
               selected->movement_current==movement::timeline::plan_json(plan),"成功回执前草稿基线及下一次模板已更新");
        expect(fake->outputs==0,"选择与准备模板不输出任何FakeDevice输入");
    }
    tick();input.capture_virtual_keys['V']=true;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,true}};tick();
    const auto second=receive("begin");expect(second.is_object(),"完成结果后新按下可开始下一轮");
    capture=true;tick();expect(receive("cancel").is_object(),"进入捕获主动取消而非伪造松开");
    capture=false;tick();
    input.capture_virtual_keys['V']=false;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,false}};tick();
    context.map.context_epoch=2;context.map.map_name="de_inferno";
    const auto changed=receive("catalog");expect(changed.is_object()&&changed["entries"].empty(),"换图不回退全库");
    expect(session.snapshot()->movement_test_enabled && !session.snapshot()->repeat_ready, "换图撤销语音模板但保留显式测试许可");
    expect(fake->outputs==0,"完整语音交互未执行设备动作");
    controller.cancel();closesocket(client);server.stop();
    session.request_shutdown();session.poll();
    std::filesystem::current_path(original_cwd);
    std::filesystem::remove_all(root);
    return failures==0?0:1;
}
