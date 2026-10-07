#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include "weapon/context_sharing_internal.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
using namespace std::chrono_literals;
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
template<class F> bool wait(F f) {
    auto end = weapon::Clock::now() + 3s;
    while (weapon::Clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(20ms); }
    return false;
}
std::wstring endpoint(const weapon::GsiConfig& c) {
    HANDLE token = nullptr; OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD size = 0; GetTokenInformation(token, TokenGroups, nullptr, 0, &size);
    std::vector<unsigned char> data(size); GetTokenInformation(token, TokenGroups, data.data(), size, &size); CloseHandle(token);
    auto groups = reinterpret_cast<TOKEN_GROUPS*>(data.data());
    for (DWORD i=0; i<groups->GroupCount; ++i) if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID) {
        LPWSTR sid=nullptr; ConvertSidToStringSidW(groups->Groups[i].Sid,&sid);
        std::wstring name=L"\\\\.\\pipe\\Xen.GsiContext.v2."+std::wstring(sid)+L".127.0.0.1_"+std::to_wstring(c.port)+L"__2500_5000";
        LocalFree(sid); return name;
    }
    return {};
}
int main() {
    weapon::GsiConfig config;
    config.enabled = true; config.port = static_cast<std::uint16_t>(20000 + GetCurrentProcessId() % 20000);
    weapon::WeaponSnapshot source;
    source.valid = source.context_valid = source.identity_match = source.player_playing = true;
    source.map_name = "de_dust2"; source.local_team = weapon::Team::T;
    source.player_id = "must-not-cross"; source.canonical_id = "ak47"; source.ammo_clip = 30;
    source.control_safety_epoch = 123; source.source_epoch = 42;
    source.received_at = weapon::Clock::now(); source.valid_until = source.received_at + 2s;
    std::mutex mutex;
    weapon::detail::LineupLocateEvent locate{1, weapon::Clock::now()};
    weapon::detail::ContextPublisher publisher;
    const auto sample = [&] { std::lock_guard lock(mutex); return source; };
    const auto sample_locate = [&] { std::lock_guard lock(mutex); return locate; };
    expect(publisher.start(config, sample, sample_locate), "publisher start");
    const auto name = endpoint(config);
    expect(wait([&] {
        HANDLE h=CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(h!=INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
        return GetLastError()==ERROR_ACCESS_DENIED;
    }), "client write access denied");
    weapon::GsiContextReader reader; reader.configure(config);
    weapon::WeaponSnapshot observed;
    expect(wait([&] { observed = reader.snapshot(); return observed.context_valid; }), "projected context");
    expect(!observed.valid && observed.player_id.empty() && observed.canonical_id.empty() && !observed.ammo_clip &&
        observed.control_safety_epoch == 0 && observed.source_epoch == 0, "control/private fields must stay absent");
    // 重启不等读者关闭旧实例：新发布者须自动恢复。
    expect(!reader.consume_lineup_locate(), "first connection must baseline old locate event");
    { std::lock_guard lock(mutex); locate = {2, weapon::Clock::now()}; }
    expect(wait([&] { reader.snapshot(); return reader.consume_lineup_locate() == 2; }), "new edge preserves exact sequence");
    expect(!reader.consume_lineup_locate(), "event consumed once");
    std::this_thread::sleep_for(100ms); reader.snapshot();
    expect(!reader.consume_lineup_locate(), "heartbeat must not replay event");
    { std::lock_guard lock(mutex); locate = {3, weapon::Clock::now() - 1s}; }
    std::this_thread::sleep_for(100ms); reader.snapshot();
    expect(!reader.consume_lineup_locate(), "late event must not execute");
    { std::lock_guard lock(mutex); locate = {4, weapon::Clock::now() + 1s}; }
    std::this_thread::sleep_for(100ms); reader.snapshot();
    expect(!reader.consume_lineup_locate(), "future event must not execute");
    publisher.stop();
    { std::lock_guard lock(mutex); locate = {5, weapon::Clock::now()}; }
    expect(publisher.start(config, sample, sample_locate), "immediate restart");
    expect(!reader.snapshot().context_valid, "disconnect invalidates");
    expect(wait([&] { return reader.snapshot().context_valid; }), "restart recovers without configuration changes");
    expect(!reader.consume_lineup_locate(), "reconnect must not replay earlier edge");
    { std::lock_guard lock(mutex); source.context_valid = false; }
    expect(wait([&] { return !reader.snapshot().context_valid; }), "invalid source clears context");
    { std::lock_guard lock(mutex); locate = {6, weapon::Clock::now()}; }
    expect(wait([&] { reader.snapshot(); return reader.consume_lineup_locate() == 6; }), "invalid GSI preserves manual locate sequence");
    // 慢读者不读管道；发布者停止仍有界，不等待客户端 drain。
    std::this_thread::sleep_for(1500ms);
    const auto begin = weapon::Clock::now(); publisher.stop();
    expect(weapon::Clock::now() - begin < 1s, "slow reader must not block stop");
    expect(!reader.snapshot().context_valid, "stopped publisher cannot be fresh");
    expect(publisher.start(config, sample), "no-reader start");
    const auto begin2 = weapon::Clock::now(); publisher.stop();
    expect(weapon::Clock::now() - begin2 < 1s, "no-reader connect cancellation bounded");
    reader.configure(config);
    // 模拟不兼容/截断发布者。仅本测试端点，不触碰任何真实进程。
    for (DWORD length : {8UL,328UL}) {
        HANDLE raw=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_OUTBOUND|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,512,512,0,nullptr);
        expect(raw!=INVALID_HANDLE_VALUE,"fixture pipe creation");
        if(raw==INVALID_HANDLE_VALUE) break;
        reader.snapshot();
        ConnectNamedPipe(raw,nullptr);
        unsigned char packet[328]{}; DWORD sent=0;
        expect(WriteFile(raw,packet,length,&sent,nullptr)!=FALSE,"fixture packet write");
        expect(!reader.snapshot().context_valid,"unknown protocol or truncated frame rejected");
        DisconnectNamedPipe(raw);CloseHandle(raw);reader.configure(config);
    }
    config.enabled = false;
    reader.configure(config);
    { std::lock_guard lock(mutex); locate = {7, weapon::Clock::now()}; }
    expect(publisher.start(config, sample, sample_locate), "disabled HTTP GSI still allows local channel");
    reader.snapshot();
    expect(wait([&] { reader.snapshot(); return reader.last_error().empty(); }), "disabled GSI channel connected");
    expect(!reader.consume_lineup_locate(), "disabled GSI connection also baselines");
    { std::lock_guard lock(mutex); locate = {8, weapon::Clock::now()}; }
    expect(wait([&] { reader.snapshot(); return reader.consume_lineup_locate() == 8; }), "disabled GSI preserves exact locate sequence");
    // 已取入但尚未消费的事件也受原按下时间限制，不能因轮询刷新 TTL。
    { std::lock_guard lock(mutex); locate = {9, weapon::Clock::now()}; }
    std::this_thread::sleep_for(100ms); reader.snapshot();
    std::this_thread::sleep_for(400ms);
    expect(!reader.consume_lineup_locate(), "unconsumed event expires without replay");
    publisher.stop();
    return failures ? 1 : 0;
}
