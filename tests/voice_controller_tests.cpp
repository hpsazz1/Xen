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
    const auto guidance=receive("guidance");
    expect(guidance.is_object() && guidance.value("state",std::string{})=="hidden", "首次连接独立同步隐藏引导状态");
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
    plan.name="测试烟"; plan.voice_command="测试烟"; plan.map="de_inferno";
    const auto grenade_u8=(root / "cache" / "grenade" / "plans").u8string();
    const std::string grenade_library(grenade_u8.begin(),grenade_u8.end());
    const auto grenade_path=debug_session::detail::save_movement_plan(movement::timeline::plan_json(plan),"",false,grenade_library);
    request.mode=debug_session::Mode::GRENADE_TEST; request.movement_library_root=grenade_library; request.load_path=grenade_path;
    expect(session.dispatch(debug_session::Action::LOAD_PLAN,request,context),"载入道具建立独立干净草稿");
    for(int i=0;i<400 && session.busy();++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    request.plan_text=movement::timeline::plan_json(plan).dump();
    expect(session.dispatch(debug_session::Action::ENABLE_MOVEMENT_TEST,request,context),"显式将执行域切换到道具");
    auto grenade_catalog=receive("catalog");
    if(grenade_catalog.is_object() && grenade_catalog["entries"].empty()) grenade_catalog=receive("catalog");
    expect(grenade_catalog.is_object() && grenade_catalog["entries"].size()==1 &&
        grenade_catalog["entries"][0]["command"]=="测试烟", "道具执行域只发布当前地图道具口令");
    if(grenade_catalog.is_object() && !grenade_catalog["entries"].empty()) {
        send({{"type","ready"},{"epoch",2},{"revision",grenade_catalog["revision"]}});
        input.capture_virtual_keys['V']=false;
        for(int i=0;i<10;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        input.capture_virtual_keys['V']=true;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,true}};tick();
        const auto voice_begin=receive("begin"); expect(voice_begin.is_object(),"道具目录就绪后可开始语音");
        if(voice_begin.is_object()) {
            input.capture_virtual_keys['V']=false;input.events={{KeyboardEventType::VOICE_HOLD_CHANGED,false}};tick();
            expect(receive("end").is_object(),"道具语音松开结束收音");
            send({{"type","select"},{"id",voice_begin["id"]},{"map","de_inferno"},{"epoch",2},
                {"revision",grenade_catalog["revision"]},{"plan_id",grenade_catalog["entries"][0]["id"]}});
            const auto selected_grenade=receive("result");
            expect(selected_grenade.is_object() && selected_grenade.value("ok",false),"道具语音复用Session异步选择成功");
            expect(session.snapshot()->grenade_selected_path==grenade_path &&
                session.snapshot()->grenade_current==movement::timeline::plan_json(plan),"语音结果只更新道具草稿");
            expect(fake->outputs==0,"选择道具不执行动作");
        }
    }
    plan.name="测试 T1 窗烟";plan.voice_command.clear();
    const auto menu_path=debug_session::detail::save_movement_plan(movement::timeline::plan_json(plan),"",false,grenade_library);
    expect(session.dispatch(debug_session::Action::LIST_MOVEMENT_PLANS,request,context),"刷新空语音口令道具清单");
    const auto menu_catalog=receive("catalog");
    expect(menu_catalog.is_object() && menu_catalog["entries"].size()==2 && menu_catalog.value("domain",std::string{})=="grenade",
        "完整道具清单保留无语音口令的方案");
    if(menu_catalog.is_object()) {
        std::string empty_command_id;
        for(const auto& entry:menu_catalog["entries"]) if(entry["command"]=="") {
            empty_command_id=entry["id"].get<std::string>();
            expect(entry["team"]=="T" && entry["name"]==plan.name,"阵营只来自名称中明确的来源编号");
        }
        keyboard.voice_virtual_keys.clear();
        send({{"type","ready"},{"epoch",2},{"revision",menu_catalog["revision"]},{"voice_enabled",false}});
        for(int i=0;i<10;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        input.events={{KeyboardEventType::GRENADE_MENU,true}};tick();
        const auto shown=receive("catalog_show");
        expect(shown.is_object() && shown["revision"]==menu_catalog["revision"],"独立菜单键无需语音键或模型即可打开清单");
        send({{"type","catalog_select"},{"id",1},{"map","de_inferno"},{"epoch",2},
            {"revision",menu_catalog["revision"]},{"plan_id",empty_command_id}});
        send({{"type","fail"},{"id",1},{"reason","迟到语音消息"}});
        const auto menu_result=receive("catalog_result");
        expect(menu_result.is_object() && menu_result.value("ok",false) && menu_result["plan_id"]==empty_command_id,
            "无口令道具可独立点选，迟到同编号语音消息不干扰菜单请求");
        expect(session.snapshot()->grenade_selected_path==menu_path,"菜单异步选择复用正式道具Session");
        const auto loaded=session.snapshot()->grenade_loaded_revision;
        send({{"type","catalog_select"},{"id",1},{"map","de_inferno"},{"epoch",2},
            {"revision",menu_catalog["revision"]},{"plan_id",empty_command_id}});
        for(int i=0;i<20;++i){tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        expect(session.snapshot()->grenade_loaded_revision==loaded,"重复菜单请求不再次选择或排队");
        send({{"type","catalog_select"},{"id",2},{"map","de_inferno"},{"epoch",1},
            {"revision",menu_catalog["revision"]},{"plan_id",empty_command_id}});
        const auto stale_result=receive("catalog_result");
        expect(stale_result.is_object() && !stale_result.value("ok",true),"旧地图菜单请求明确拒绝");
        expect(fake->outputs==0,"完整菜单与语音选择不输出物理动作");
    }
    controller.cancel();closesocket(client);server.stop();
    session.request_shutdown();session.poll();
    std::filesystem::current_path(original_cwd);
    std::filesystem::remove_all(root);
    return failures==0?0:1;
}
