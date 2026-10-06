#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include "weapon/context_sharing_internal.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <thread>
#include <vector>
#include <utility>

namespace weapon {
namespace {
// 固定协议只投影筛选上下文与人工定位边沿；绝不传递控制许可。
struct Packet {
    std::uint32_t magic = 0x58474331, version = 2;
    std::uint64_t sequence = 0, published = 0, received = 0, until = 0, context_epoch = 0;
    std::uint32_t valid = 0, team = 0;
    char map[256]{};
    std::uint64_t locate_sequence = 0, locate_tick = 0;
};
static_assert(sizeof(Packet) == 328);
std::wstring logon_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const bool ok = GetTokenInformation(token, TokenGroups, buffer.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!ok) return {};
    const auto groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        LPWSTR value = nullptr;
        if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &value)) return {};
        std::wstring result(value); LocalFree(value); return result;
    }
    return {};
}
std::wstring pipe_name(const GsiConfig& c, const std::wstring& sid) {
    auto key = c.bind_address + "_" + std::to_string(c.port) + "_" + c.allowed_peer_ipv4 +
        "_" + std::to_string(c.ttl_ms) + "_" + std::to_string(c.max_clock_skew_ms);
    return L"\\\\.\\pipe\\Xen.GsiContext.v2." + sid + L"." + std::wstring(key.begin(), key.end());
}
bool valid_channel_config(GsiConfig config) {
    // IPC 地址沿用现有 GSI 配置；关闭 HTTP 接收不关闭人工定位事件。
    config.enabled = true;
    return valid_config(config);
}
Packet project(const WeaponSnapshot& s, std::uint64_t sequence, detail::LineupLocateEvent locate) {
    Packet p;
    p.sequence = sequence;
    p.context_epoch = s.context_epoch;
    const auto now = Clock::now();
    p.published = GetTickCount64();
    p.locate_sequence = locate.sequence;
    const auto locate_age = std::chrono::duration_cast<std::chrono::milliseconds>(now - locate.at).count();
    if (locate.sequence && locate.at != Clock::time_point{} && locate_age >= 0 &&
        static_cast<std::uint64_t>(locate_age) <= p.published)
        p.locate_tick = p.published - locate_age;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - s.received_at).count();
    p.received = age >= 0 && static_cast<std::uint64_t>(age) <= p.published ? p.published - age : 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(s.valid_until - now).count();
    const auto map = canonical_map_id(s.map_name);
    if (s.context_valid && s.identity_match && s.player_playing && remaining > 20 &&
        (s.local_team == Team::T || s.local_team == Team::CT) && !map.empty() && map.size() < sizeof(p.map)) {
        // tick 精度余量保守扣除；重复心跳不延长接收器原 TTL。
        p.valid = 1; p.team = static_cast<std::uint32_t>(s.local_team);
        p.until = p.published + remaining - 20;
        std::memcpy(p.map, map.data(), map.size());
    }
    return p;
}
}
namespace detail {
struct ContextPublisher::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE, stop_event = nullptr;
    std::thread worker;
    std::function<WeaponSnapshot()> sample;
    std::function<LineupLocateEvent()> locate;
    std::wstring name, sid;
    HANDLE create_pipe() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto sddl = L"D:P(A;;GR;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        auto handle = CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(Packet), sizeof(Packet), 0, &security);
        LocalFree(descriptor);
        return handle;
    }
    bool complete(OVERLAPPED& op, BOOL immediate, DWORD& transferred, DWORD timeout) {
        if (immediate) return GetOverlappedResult(pipe, &op, &transferred, FALSE) != FALSE;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE events[]{stop_event, op.hEvent};
        if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1)
            return GetOverlappedResult(pipe, &op, &transferred, FALSE) != FALSE;
        CancelIoEx(pipe, &op);
        GetOverlappedResult(pipe, &op, &transferred, TRUE);
        return false;
    }
    void run() noexcept {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return;
        std::uint64_t sequence = 0;
        try {
            while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
                // 旧进程退出后读端可能尚未关闭旧实例；异步重试，不拖住 Runtime 启动。
                if (pipe == INVALID_HANDLE_VALUE) {
                    pipe = create_pipe();
                    if (pipe == INVALID_HANDLE_VALUE) {
                        WaitForSingleObject(stop_event, 200);
                        continue;
                    }
                }
                OVERLAPPED op{}; op.hEvent = event; ResetEvent(event);
                DWORD bytes = 0;
                BOOL connected = ConnectNamedPipe(pipe, &op);
                bool ready = !connected && GetLastError() == ERROR_PIPE_CONNECTED;
                if (!ready) ready = complete(op, connected, bytes, INFINITE);
                while (ready && WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
                    const auto packet = project(sample(), ++sequence, locate ? locate() : LineupLocateEvent{});
                    op = {}; op.hEvent = event; ResetEvent(event);
                    const BOOL written = WriteFile(pipe, &packet, sizeof(packet), nullptr, &op);
                    ready = complete(op, written, bytes, 100) && bytes == sizeof(packet);
                    if (WaitForSingleObject(stop_event, 50) != WAIT_TIMEOUT) break;
                }
                // 不 FlushFileBuffers：慢读者/退出的助手不得阻塞发布者。
                DisconnectNamedPipe(pipe);
            }
        } catch (...) { DisconnectNamedPipe(pipe); }
        CloseHandle(event);
    }
};
ContextPublisher::ContextPublisher() : impl_(std::make_unique<Impl>()) {}
ContextPublisher::~ContextPublisher() { stop(); }
bool ContextPublisher::start(const GsiConfig& config, std::function<WeaponSnapshot()> sample,
                             std::function<LineupLocateEvent()> locate) noexcept {
    stop();
    try {
        const auto sid = logon_sid();
        if (sid.empty() || !valid_channel_config(config)) return false;
        // 当前登录 SID 仅获读权限；不使用默认 Everyone/Anonymous ACL。
        impl_->name = pipe_name(config, sid); impl_->sid = sid;
        impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!impl_->stop_event) { stop(); return false; }
        impl_->sample = std::move(sample);
        impl_->locate = std::move(locate);
        impl_->worker = std::thread([this] { impl_->run(); });
        return true;
    } catch (...) { stop(); return false; }
}
void ContextPublisher::stop() noexcept {
    if (impl_->stop_event) SetEvent(impl_->stop_event);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->pipe != INVALID_HANDLE_VALUE) CloseHandle(impl_->pipe);
    if (impl_->stop_event) CloseHandle(impl_->stop_event);
    impl_->pipe = INVALID_HANDLE_VALUE; impl_->stop_event = nullptr;
}
}
struct GsiContextReader::Impl {
    GsiConfig config;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    Packet packet{};
    std::uint64_t sequence = 0;
    std::string error;
    bool locate_baselined = false;
    std::uint64_t locate_seen = 0, locate_pending_until = 0;
    void close() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; packet = {}; sequence = 0; locate_baselined = false; locate_seen = 0; locate_pending_until = 0; }
};
GsiContextReader::GsiContextReader() : impl_(std::make_unique<Impl>()) {}
GsiContextReader::~GsiContextReader() { impl_->close(); }
void GsiContextReader::configure(const GsiConfig& config) { impl_->close(); impl_->config = config; }
WeaponSnapshot GsiContextReader::snapshot() {
    WeaponSnapshot result;
    auto& i = *impl_;
    try {
        if (!valid_channel_config(i.config)) { result.status = Status::DISABLED; return result; }
        if (i.pipe == INVALID_HANDLE_VALUE) {
            const auto sid = logon_sid();
            if (sid.empty()) return result;
            i.pipe = CreateFileW(pipe_name(i.config, sid).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            i.error = "Runtime 只读上下文不可用；请使用支持 v2 共享的新版本并在同一登录会话运行，或手动筛选";
            // 重连先发布一次未知，避免重启后相同地图/阵营保留旧锁。
            return result;
        }
        for (int count = 0; count < 16; ++count) {
            DWORD available = 0, read = 0;
            if (!PeekNamedPipe(i.pipe, nullptr, 0, nullptr, &available, nullptr)) { i.close(); return result; }
            if (available < sizeof(Packet)) break;
            Packet next;
            if (!ReadFile(i.pipe, &next, sizeof(next), &read, nullptr) || read != sizeof(next) ||
                next.magic != 0x58474331 || next.version != 2 || next.sequence <= i.sequence ||
                next.valid > 1 || !std::memchr(next.map, 0, sizeof(next.map))) { i.close(); return result; }
            i.packet = next; i.sequence = next.sequence;
        }
        const auto& p = i.packet;
        const auto tick = GetTickCount64();
        if (!i.sequence || p.published > tick || tick - p.published > 350) { i.close(); return result; }
        i.error.clear();
        if (!i.locate_baselined) {
            i.locate_seen = p.locate_sequence;
            i.locate_baselined = true;
        } else if (p.locate_sequence != i.locate_seen) {
            // 单调边沿只保留最新一次；迟到、回退或未来 tick 一律不执行。
            if (p.locate_sequence > i.locate_seen && p.locate_tick &&
                p.locate_tick <= tick && tick - p.locate_tick <= 350)
                i.locate_pending_until = p.locate_tick + 350;
            else i.locate_pending_until = 0;
            i.locate_seen = p.locate_sequence;
        }
        if (!i.config.enabled) { result.status = Status::DISABLED; return result; }
        if (!p.valid || p.until <= tick || p.until - tick > static_cast<std::uint64_t>(i.config.ttl_ms) ||
            p.received > tick || (p.team != static_cast<unsigned>(Team::CT) && p.team != static_cast<unsigned>(Team::T))) {
            result.status = Status::EXPIRED; return result;
        }
        const auto map = canonical_map_id(p.map);
        if (map.empty() || map != p.map) { i.close(); return result; }
        result.context_epoch = p.context_epoch;
        result.context_valid = result.identity_match = result.player_playing = true;
        result.map_name = map; result.local_team = static_cast<Team>(p.team);
        result.received_at = Clock::now() - std::chrono::milliseconds(tick - p.received);
        result.valid_until = Clock::now() + std::chrono::milliseconds(std::min<std::uint64_t>(p.until - tick, 350 - (tick - p.published)));
        result.status = Status::READY;
        // valid、武器、弹药、玩家ID与控制代次保留默认无效值。
        return result;
    } catch (...) { i.close(); return {}; }
}
std::string GsiContextReader::last_error() const { return impl_->error; }
bool GsiContextReader::consume_lineup_locate() noexcept {
    const auto until = std::exchange(impl_->locate_pending_until, std::uint64_t{0});
    return until != 0 && GetTickCount64() <= until;
}
}
