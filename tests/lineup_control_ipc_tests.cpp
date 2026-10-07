#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include "lineup/control_ipc.h"
#include "weapon/weapon.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using namespace lineup;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class F> bool until(F f, int milliseconds = 3000) {
    auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (Clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(5ms); }
    return false;
}
control::Request request(std::uint64_t generation = 1, std::uint64_t sequence = 1) {
    control::Request r;
    r.mode = control::Mode::LOCATE; r.reference_version = 1;
    r.trigger_sequence = 100;
    auto &o = r.observation;
    o.identity.recipe_id = "recipe"; o.identity.reference_id = "reference";
    o.identity.source_id = "synthetic-only"; o.identity.session_id = "fixture";
    o.identity.recipe_version = 2; o.identity.selection_generation = generation;
    o.identity.geometry = {320,320,1920,1080,320,320,800,380,1,1,true};
    o.sequence = sequence; o.captured_at = Clock::now() - 5ms;
    o.source_at = Clock::now() - 7ms; o.source_uncertainty = 1ms;
    o.error_x = 2; o.error_y = -3; o.valid = true;
    r.throw_action = {{"schema",1},{"type","phases"},{"phases",nlohmann::json::array({
        {{"buttons",nlohmann::json::array({"left"})},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",100}},
        {{"buttons",nlohmann::json::array()},{"movement",nlohmann::json::array()},{"jump",false},{"duration_ms",0}}})}};
    return r;
}
int child(const std::string &key) {
    control::Client c; check(c.start(key),"child start");
    check(until([&]{return c.connected();}),"child connect");
    check(c.publish(request(100,1)),"child publish");
    check(until([&]{return c.status().value("ipc_request_available",false);}),"child ack");
    std::this_thread::sleep_for(150ms); c.stop(); return 0;
}
void malformed_peer(const std::string &key) {
    HANDLE token = nullptr;
    check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token) != FALSE,"test token");
    DWORD size = 0; GetTokenInformation(token,TokenGroups,nullptr,0,&size);
    std::vector<unsigned char> bytes(size);
    const bool token_ok = GetTokenInformation(token,TokenGroups,bytes.data(),size,&size) != FALSE;
    CloseHandle(token); check(token_ok,"test logon groups");
    const auto groups = reinterpret_cast<TOKEN_GROUPS *>(bytes.data());
    std::wstring sid;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        LPWSTR text = nullptr;
        check(ConvertSidToStringSidW(groups->Groups[i].Sid,&text) != FALSE,"test SID");
        sid = text; LocalFree(text); break;
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : key) { hash ^= c; hash *= 1099511628211ull; }
    const auto name = L"\\\\.\\pipe\\Xen.LineupControl.v1." + sid + L"." + std::to_wstring(hash);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    check(until([&]{pipe = CreateFileW(name.c_str(),FILE_READ_DATA | FILE_WRITE_DATA,0,nullptr,OPEN_EXISTING,0,nullptr); return pipe != INVALID_HANDLE_VALUE;}),"malformed peer connect");
    struct Header { std::uint32_t magic = 0x584C4331, version = 1, size = 16385, reserved = 0; std::uint64_t epoch = 0, sequence = 1, tick = GetTickCount64(); };
    static_assert(sizeof(Header) == 40);
    Header oversized; DWORD written = 0;
    const bool sent = WriteFile(pipe,&oversized,sizeof(oversized),&written,nullptr) != FALSE;
    std::this_thread::sleep_for(40ms);
    DWORD available = 0;
    const bool rejected = !PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr);
    CloseHandle(pipe); check(sent && written == sizeof(oversized),"oversized envelope send");
    check(rejected,"oversized wire envelope disconnected");
}
}
int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--child") return child(argv[2]);
        const auto key = "test-only-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        weapon::GsiConfig config;
        check(!control::channel_key(config).empty(),"existing configuration helper");
        control::Server server, duplicate; control::Client client;
        check(!client.publish(request()),"unconnected publish must fail");
        check(server.start(key),"server start");
        check(!duplicate.start(key),"first instance exclusivity");
        check(client.start(key),"client start");
        check(until([&]{return client.connected();}),"connect");
        check(!server.snapshot().available,"connect must not locate");
        server.publish_status({{"available",true},{"reason","fake_controller_ready"}});
        check(until([&]{return client.status().value("reason","") == "fake_controller_ready";}),"status reverse channel");
        auto r = request();
        check(client.publish(r),"locate publish");
        check(until([&]{return server.snapshot().available;}),"locate accepted");
        auto first = server.snapshot();
        check(first.request.observation.identity == r.observation.identity,"identity roundtrip");
        check(first.request.reference_version == 1 && first.request.throw_action == r.throw_action,"action and revision roundtrip");
        check(first.request.trigger_sequence == r.trigger_sequence,"local trigger sequence roundtrip");
        check(first.request.observation.captured_at <= Clock::now() - 5ms,"no underreported capture age");
        check(first.request.observation.source_uncertainty->count() >= 3,"mapping uncertainty conservative");
        auto observe = request(1,2); observe.mode = control::Mode::OBSERVATION;
        check(client.publish(observe),"observation publish");
        check(until([&]{return server.snapshot().request.observation.sequence == 2;}),"observation accepted");
        check(server.snapshot().locate_sequence == first.locate_sequence,"observation never invents locate edge");
        auto changed_trigger = observe; ++changed_trigger.trigger_sequence; ++changed_trigger.observation.sequence;
        check(client.publish(changed_trigger),"changed trigger publish");
        check(until([&]{return !server.snapshot().available;}),"changed local trigger revokes binding");
        r = request(2, 4); check(client.publish(r),"fresh locate after changed trigger");
        check(until([&]{return server.snapshot().available;}),"fresh binding accepted");
        observe = request(2, 5); observe.mode = control::Mode::OBSERVATION;
        check(client.publish(observe),"fresh observation publish");
        check(until([&]{return server.snapshot().request.observation.sequence == 5;}),"fresh observation accepted");
        check(client.publish(observe),"duplicate frame publish");
        check(until([&]{return !server.snapshot().available;}),"duplicate frame revokes");
        observe.observation.sequence = 3;
        check(client.publish(observe),"unbound observation publish");
        std::this_thread::sleep_for(40ms);
        check(!server.snapshot().available,"observation cannot revive revoked selection");
        check(client.publish(request(3,6)),"new explicit selection");
        check(until([&]{return server.snapshot().available;}),"new selection accepted");
        std::this_thread::sleep_for(400ms);
        check(!server.snapshot().available,"status heartbeats cannot extend command lifetime");
        check(client.publish(request(3,7)),"replayed locate publish");
        std::this_thread::sleep_for(40ms);
        check(!server.snapshot().available,"same generation cannot replay after expiry");
        auto unknown = request(4,8); unknown.trigger_sequence = 0; unknown.observation.source_at.reset(); unknown.observation.source_uncertainty.reset();
        check(client.publish(unknown),"unknown source input");
        check(until([&]{return server.snapshot().available;}),"unknown remains typed unknown");
        check(!server.snapshot().request.observation.source_at && !server.snapshot().request.observation.source_uncertainty,"no invented source time");
        check(server.snapshot().request.trigger_sequence == 0,"web locate retains absent local trigger");
        auto invalid = request(4,7); invalid.mode = static_cast<control::Mode>(99);
        check(!client.publish(invalid),"no throw or arbitrary command mode");
        invalid = request(4,7); invalid.observation.error_x = std::numeric_limits<double>::quiet_NaN();
        check(!client.publish(invalid),"nonfinite data rejected");
        const auto old_epoch = server.snapshot().connection_epoch;
        client.stop(); check(until([&]{return !server.snapshot().connected;}),"disconnect revokes");
        check(client.start(key),"restart client"); check(until([&]{return client.connected();}),"reconnect");
        check(server.snapshot().connection_epoch != old_epoch && !server.snapshot().available,"new connection epoch no replay");
        auto after = request(5,8); after.mode = control::Mode::OBSERVATION;
        check(client.publish(after),"post-reconnect observation"); std::this_thread::sleep_for(40ms);
        check(!server.snapshot().available,"post-reconnect observations cannot locate");
        check(client.publish(request(6,9)),"post-reconnect explicit locate");
        check(until([&]{return server.snapshot().available;}),"new locate after reconnect");
        control::Request cancel; check(client.publish(cancel),"cancel");
        check(until([&]{return !server.snapshot().available && server.snapshot().reason == "cancelled";}),"cancel immediate revoke");
        client.stop(); check(until([&]{return !server.snapshot().connected;}),"client stopped");
        malformed_peer(key);
        check(!server.snapshot().available,"oversized wire frame must not retain command");

        // 第二进程仅运行此夹具，无设备工厂或真实输入。
        char executable[MAX_PATH]{}; GetModuleFileNameA(nullptr,executable,MAX_PATH);
        std::string command = std::string("\"") + executable + "\" --child " + key;
        STARTUPINFOA startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        check(CreateProcessA(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process) != FALSE,"test child launch");
        const bool child_observed = until([&]{auto s = server.snapshot(); return s.available && s.request.observation.identity.selection_generation == 100;});
        const DWORD waited = WaitForSingleObject(process.hProcess,5000); DWORD exit_code = 1;
        GetExitCodeProcess(process.hProcess,&exit_code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        check(child_observed && waited == WAIT_OBJECT_0 && exit_code == 0,"cross-process typed roundtrip");
        check(until([&]{return !server.snapshot().connected && !server.snapshot().available;}),"child exit revokes");
        const auto before = Clock::now(); server.stop();
        check(Clock::now() - before < 1s,"idle overlapped shutdown bounded");
        std::cout << "lineup_control_ipc_tests passed (software-only, no device)\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
