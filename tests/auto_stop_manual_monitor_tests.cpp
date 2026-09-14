#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "auto_stop_probe/manual_recording_internal.h"
#include <atomic>
#include <condition_variable>
#include <iostream>

namespace {
using namespace auto_stop_probe_detail;
constexpr std::uint32_t kConnect=0xaf3c2828U, kMonitor=0x27388020U;
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint32_t read_word(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1])<<8 | std::uint32_t(p[2])<<16 | std::uint32_t(p[3])<<24;
}
// 仿照既有kmbox_button_tests的回环SDK响应；记录全部命令而非只筛输入。
class LoopbackMonitor {
public:
    LoopbackMonitor() {
        socket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        require(socket_!=INVALID_SOCKET,"回环socket创建失败");
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"回环bind失败");
        int size=sizeof(address);
        require(getsockname(socket_,reinterpret_cast<sockaddr*>(&address),&size)==0,"回环端口读取失败");
        port=ntohs(address.sin_port);
        DWORD timeout=20;
        require(setsockopt(socket_,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))==0,
            "回环超时配置失败");
        worker_=std::thread([this] { run(); });
    }
    ~LoopbackMonitor() { stop_=true; if(worker_.joinable()) worker_.join(); if(socket_!=INVALID_SOCKET) closesocket(socket_); }
    void report(const std::array<unsigned char,20>& raw) {
        std::lock_guard lock(mutex_);
        require(destination_.sin_port!=0,"监听目标尚未登记");
        require(sendto(socket_,reinterpret_cast<const char*>(raw.data()),20,0,
            reinterpret_cast<sockaddr*>(&destination_),sizeof(destination_))==20,"回环报告发送失败");
    }
    struct Packet { std::uint32_t command, argument; int size; };
    std::vector<Packet> packets() { std::lock_guard lock(mutex_); return packets_; }
    bool await_disabled() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock,std::chrono::seconds(1),[&] {
            return !packets_.empty() && packets_.back().command==kMonitor && packets_.back().argument==0;
        });
    }
    int port=0;
private:
    void run() {
        while(!stop_) {
            std::array<unsigned char,1024> bytes{};
            sockaddr_in source{}; int size=sizeof(source);
            const int received=recvfrom(socket_,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,
                reinterpret_cast<sockaddr*>(&source),&size);
            if(received<16) continue;
            const auto command=read_word(bytes.data()+12), argument=read_word(bytes.data()+4);
            {
                std::lock_guard lock(mutex_);
                packets_.push_back({command,argument,received});
                if(command==kMonitor && argument!=0) {
                    destination_=source; destination_.sin_port=htons(static_cast<unsigned short>(argument));
                }
            }
            changed_.notify_all();
            sendto(socket_,reinterpret_cast<char*>(bytes.data()),16,0,reinterpret_cast<sockaddr*>(&source),size);
        }
    }
    SOCKET socket_=INVALID_SOCKET;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    sockaddr_in destination_{};
    std::vector<Packet> packets_;
};

void run_test() {
    LoopbackMonitor fake;
    MouseConfig original; original.backend=MouseBackend::KMBOX_NET;
    original.allow_send_input=true; original.kmbox_ip="127.0.0.1";
    original.kmbox_port=fake.port; original.kmbox_uuid="12345678";
    original.kmbox_connect_timeout_ms=1000; original.kmbox_command_timeout_ms=25;
    const auto cfg=manual_monitor_config(original);
    require(original.allow_send_input && !cfg.allow_send_input,"录制配置必须固定false且不改原始配置");
    std::shared_ptr<IMouseController> device=MouseDeviceFactory::create(cfg,MouseOutputOwnerScope::CURRENT_PROCESS_TEST);
    require(device && device->open(),"回环监听必须成功");
    require(device->status()==MouseStatus::DISABLED,"监听成功仍无软件输入授权");
    require(device->set_input_report_subscription(true),"订阅原始报告失败");
    require(!device->move({4,-8}).succeeded,"误调用移动必须失败");
    for(bool down : {true,false}) {
        const auto receipt=device->set_left_button(down);
        require(receipt.disposition==ButtonDisposition::REJECTED && !receipt.datagram_sent,"误调用左键边沿必须零发送");
    }
    for(std::uint8_t mask : {std::uint8_t(2),std::uint8_t(0)}) {
        const auto receipt=device->set_wasd_keyboard(mask);
        require(receipt.disposition==KeyboardDisposition::REJECTED && !receipt.datagram_sent,"误调用软件键必须零发送");
    }
    for(bool masked : {true,false}) {
        const auto receipt=device->set_wasd_mask(2,masked);
        require(receipt.disposition==KeyboardDisposition::REJECTED && !receipt.datagram_sent,"误调用屏蔽必须零发送");
    }
    require(!device->cleanup_wasd_keyboard().datagram_sent,"空债务清理不得发键");
    auto source=std::make_unique<runtime::detail::InputTrainingSource>(device);
    std::array<unsigned char,20> raw{}; raw[1]=1; raw[2]=0xff; raw[3]=0xff; raw[10]=0x04;
    fake.report(raw);
    input_training::ReadBatch batch;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    do {
        batch=source->read();
        if(!batch.events.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    require(batch.events.size()==1,"原始报告必须经过生产source可读");
    const auto& event=batch.events.front();
    require(event.raw_report==raw && event.left_down && event.held_mask==2 && event.datagram_size==20 && event.state_valid,
        "归档源保留同包鼠标/键盘/原始字节");
    require(!event.motion_valid && !event.physical_motion_verified,"原始XY不得自动升级为物理位移");
    // 等待第二条真实接收事实，再freeze；独立source游标尚未消费尾up。
    raw[1]=0; fake.report(raw);
    InputSnapshot state;
    const auto tail_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    do { device->poll_input(state); if(state.sequence>=2) break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while(std::chrono::steady_clock::now()<tail_deadline);
    require(state.sequence>=2,"尾up未进入接收水位");
    require(device->freeze_input_reports(),"冻结失败");
    batch=source->read();
    require(batch.events.size()==1 && !batch.events[0].left_down,"冻结后source必须读到尾up");
    require(source->read().events.empty(),"排空后必须合法空批");
    source.reset(); device->close();
    require(device->set_input_report_subscription(false),"lease释放后仍可冻结");
    require(fake.await_disabled(),"关闭必须发送monitor disable");
    const auto packets=fake.packets();
    require(packets.size()==3,"所有误调用/订阅/read/close合计只能三个管理命令");
    require(packets[0].command==kConnect && packets[1].command==kMonitor && packets[1].argument!=0 &&
        packets[2].command==kMonitor && packets[2].argument==0,"管理命令顺序必须connect/enable/disable");
    for(const auto& packet:packets) require(packet.size==16 && (packet.command==kConnect || packet.command==kMonitor),
        "录制协议白名单不得出现键鼠或屏蔽命令");
}
}
int main() {
    WSADATA data{};
    if(WSAStartup(MAKEWORD(2,2),&data)!=0) return 2;
    int result=0;
    try { run_test(); std::cout<<"人工录制回环命令白名单与原始source通过\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; result=1; }
    WSACleanup(); return result;
}
