#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#include "lineup/control_ipc.h"
#include "lineup/action_internal.h"
#include "weapon/weapon.h"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>

namespace lineup::control {
namespace {
using Json = nlohmann::json;
constexpr DWORD kMaxMessage = 16384;
constexpr auto kTtl = std::chrono::milliseconds(350);
struct Header { std::uint32_t magic = 0x584C4331, version = 1, size = 0, reserved = 0; std::uint64_t epoch = 0, sequence = 0, tick = 0; };
static_assert(sizeof(Header) == 40);
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
std::wstring logon_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(raw, TokenGroups, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(raw, TokenGroups, data.data(), size, &size)) return {};
    const auto groups = reinterpret_cast<TOKEN_GROUPS *>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid)) return {};
        std::wstring result(sid); LocalFree(sid); return result;
    }
    return {};
}
std::wstring endpoint(const std::string &key, const std::wstring &sid) {
    // 稳定名称散列仅用于隔离；权限由登录 SID ACL 实施，不把散列当认证。
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : key) { hash ^= c; hash *= 1099511628211ull; }
    return L"\\\\.\\pipe\\Xen.LineupControl.v1." + sid + L"." + std::to_wstring(hash);
}
bool complete(HANDLE pipe, HANDLE stop, OVERLAPPED &op, BOOL immediate, DWORD &bytes, DWORD timeout) {
    if (immediate) return GetOverlappedResult(pipe, &op, &bytes, FALSE) != FALSE;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE events[]{stop, op.hEvent};
    if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1)
        return GetOverlappedResult(pipe, &op, &bytes, FALSE) != FALSE;
    CancelIoEx(pipe, &op);
    GetOverlappedResult(pipe, &op, &bytes, TRUE); // 取消完成后才允许销毁缓冲区。
    return false;
}
bool transfer(HANDLE pipe, HANDLE stop, void *buffer, DWORD size, bool writing, DWORD timeout = 500) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) return false;
    auto *data = static_cast<unsigned char *>(buffer);
    DWORD offset = 0;
    while (offset < size) {
        OVERLAPPED op{}; op.hEvent = event.value; ResetEvent(event.value);
        DWORD bytes = 0;
        BOOL ok = writing ? WriteFile(pipe, data + offset, size - offset, nullptr, &op)
                          : ReadFile(pipe, data + offset, size - offset, nullptr, &op);
        if (!complete(pipe, stop, op, ok, bytes, timeout) || !bytes) return false;
        offset += bytes;
    }
    return true;
}
bool send(HANDLE pipe, HANDLE stop, Header header, const Json &value) {
    auto text = value.dump();
    if (text.size() > kMaxMessage) return false;
    header.size = static_cast<DWORD>(text.size()); header.tick = GetTickCount64();
    return transfer(pipe, stop, &header, sizeof(header), true) &&
           transfer(pipe, stop, text.data(), header.size, true);
}
bool receive(HANDLE pipe, HANDLE stop, Header &header, Json &value, DWORD timeout = 500) {
    if (!transfer(pipe, stop, &header, sizeof(header), false, timeout) || header.magic != 0x584C4331 ||
        header.version != 1 || header.reserved || !header.size || header.size > kMaxMessage) return false;
    std::string text(header.size, '\0');
    if (!transfer(pipe, stop, text.data(), header.size, false)) return false;
    // 有界长度与层级；解析失败关闭连接，不保留上一请求。
    int depth = 0;
    value = Json::parse(text, [&](int level, Json::parse_event_t, Json &) { depth = std::max(depth, level); return level <= 32; }, false);
    return !value.is_discarded() && depth <= 32 && value.is_object();
}
Json geometry_json(const detail::ExecutionGeometry &g) {
    return {{"width",g.width},{"height",g.height},{"source_width",g.source_width},{"source_height",g.source_height},
            {"encoded_width",g.encoded_width},{"encoded_height",g.encoded_height},{"roi_x",g.roi_x},{"roi_y",g.roi_y},
            {"scale_x",g.scale_x},{"scale_y",g.scale_y},{"mapping_verified",g.mapping_verified}};
}
bool valid_identity(const detail::ExecutionIdentity &i) {
    for (auto s : {&i.recipe_id,&i.reference_id,&i.source_id,&i.session_id})
        if (s->empty() || s->size() > 256 || s->find('\0') != std::string::npos) return false;
    const auto &g = i.geometry;
    return i.recipe_version && i.selection_generation && g.width >= 64 && g.height >= 64 && g.width <= 8192 && g.height <= 8192 &&
        g.encoded_width >= g.width && g.encoded_height >= g.height && g.encoded_width <= 32768 && g.encoded_height <= 32768 &&
        g.source_width >= 0 && g.source_height >= 0 && g.source_width <= 32768 && g.source_height <= 32768 &&
        std::isfinite(g.roi_x) && std::isfinite(g.roi_y) && g.roi_x >= 0 && g.roi_y >= 0 &&
        std::isfinite(g.scale_x) && std::isfinite(g.scale_y) && g.scale_x > 0 && g.scale_y > 0 &&
        ((!g.mapping_verified && !g.source_width && !g.source_height) ||
         (g.source_width > 0 && g.source_height > 0 && g.roi_x + g.width * g.scale_x <= g.source_width + .01 &&
          g.roi_y + g.height * g.scale_y <= g.source_height + .01));
}
bool valid_request(const Request &r) {
    if (r.mode == Mode::CANCEL) return true;
    if (r.mode != Mode::LOCATE && r.mode != Mode::OBSERVATION) return false;
    const auto &o = r.observation;
    return valid_identity(o.identity) && r.reference_version && o.sequence && std::isfinite(o.error_x) && std::isfinite(o.error_y) &&
        std::abs(o.error_x) <= 32768 && std::abs(o.error_y) <= 32768 && detail::inspect_action(r.throw_action).valid;
}
Json encode(const Request &r) {
    if (!valid_request(r)) throw std::runtime_error("invalid_request");
    if (r.mode == Mode::CANCEL) return {{"mode","cancel"}};
    const auto &o = r.observation; const auto &i = o.identity;
    auto now = Clock::now();
    const auto age = [&](Clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count(); };
    Json j = {{"mode",r.mode == Mode::LOCATE ? "locate" : "observation"},
        {"recipe_id",i.recipe_id},{"reference_id",i.reference_id},{"source_id",i.source_id},{"session_id",i.session_id},
        {"recipe_version",i.recipe_version},{"reference_version",r.reference_version},{"selection_generation",i.selection_generation},
        {"geometry",geometry_json(i.geometry)},{"sequence",o.sequence},{"capture_age_ns",o.captured_at == Clock::time_point{} ? Json(nullptr) : Json(age(o.captured_at))},
        {"source_age_ns",o.source_at ? Json(age(*o.source_at)) : Json(nullptr)},
        {"source_uncertainty_ms",o.source_uncertainty ? Json(o.source_uncertainty->count()) : Json(nullptr)},
        {"error_x",o.error_x},{"error_y",o.error_y},{"valid",o.valid},{"throw_action",r.throw_action}};
    return j;
}
Request decode(const Json &j, std::uint64_t sent_tick) {
    Request r;
    const auto mode = j.at("mode").get<std::string>();
    if (mode == "cancel") return r;
    if (mode != "locate" && mode != "observation") throw std::runtime_error("invalid_mode");
    for (const auto *field : {"recipe_version", "reference_version", "selection_generation", "sequence"}) {
        const auto &number = j.at(field);
        if (!number.is_number_integer() || (number.is_number_integer() && !number.is_number_unsigned() && number.get<std::int64_t>() <= 0))
            throw std::runtime_error("invalid_identity_counter");
    }
    r.mode = mode == "locate" ? Mode::LOCATE : Mode::OBSERVATION;
    auto &o = r.observation; auto &i = o.identity; auto &g = i.geometry;
    i.recipe_id = j.at("recipe_id"); i.reference_id = j.at("reference_id"); i.source_id = j.at("source_id"); i.session_id = j.at("session_id");
    i.recipe_version = j.at("recipe_version"); r.reference_version = j.at("reference_version"); i.selection_generation = j.at("selection_generation");
    const auto &v = j.at("geometry");
    for (const auto *field : {"width", "height", "source_width", "source_height", "encoded_width", "encoded_height"}) {
        const auto &number = v.at(field);
        if (!number.is_number_integer() || number.get<double>() < 0 || number.get<double>() > 32768)
            throw std::runtime_error("invalid_geometry_integer");
    }
    g.width = v.at("width"); g.height = v.at("height"); g.source_width = v.at("source_width"); g.source_height = v.at("source_height");
    g.encoded_width = v.at("encoded_width"); g.encoded_height = v.at("encoded_height");
    g.roi_x = v.at("roi_x"); g.roi_y = v.at("roi_y"); g.scale_x = v.at("scale_x"); g.scale_y = v.at("scale_y"); g.mapping_verified = v.at("mapping_verified");
    o.sequence = j.at("sequence"); o.error_x = j.at("error_x"); o.error_y = j.at("error_y"); o.valid = j.at("valid"); r.throw_action = j.at("throw_action");
    const auto tick = GetTickCount64();
    if (sent_tick > tick || tick - sent_tick > 350) throw std::runtime_error("expired_packet");
    const auto now = Clock::now();
    const auto transit = std::chrono::milliseconds(tick - sent_tick + 2);
    const auto reconstruct = [&](const Json &age) {
        if (!age.is_number_integer()) throw std::runtime_error("invalid_frame_time");
        const auto ns = age.get<std::int64_t>();
        if (ns < -350000000 || ns > 10000000000ll) throw std::runtime_error("invalid_frame_time");
        return now - transit - std::chrono::nanoseconds(ns);
    };
    if (!j.at("capture_age_ns").is_null()) o.captured_at = reconstruct(j.at("capture_age_ns"));
    if (!j.at("source_age_ns").is_null()) o.source_at = reconstruct(j.at("source_age_ns"));
    if (!j.at("source_uncertainty_ms").is_null()) {
        if (!j.at("source_uncertainty_ms").is_number_integer()) throw std::runtime_error("invalid_uncertainty");
        auto uncertainty = j.at("source_uncertainty_ms").get<std::int64_t>();
        if (uncertainty < 0 || uncertainty > 350) throw std::runtime_error("invalid_uncertainty");
        o.source_uncertainty = std::chrono::milliseconds(uncertainty + 2);
    }
    if (!valid_request(r)) throw std::runtime_error("invalid_request");
    return r;
}
Json unavailable(const char *reason) { return {{"available",false},{"reason",reason}}; }
}

std::string channel_key(const weapon::GsiConfig &c) {
    return c.bind_address + "|" + std::to_string(c.port) + "|" + c.allowed_peer_ipv4 + "|" +
        std::to_string(c.ttl_ms) + "|" + std::to_string(c.max_clock_skew_ms);
}
struct Server::Impl {
    std::mutex mutex;
    Snapshot latest;
    Json status = unavailable("runtime_status_unavailable");
    HANDLE stop = nullptr, pipe = INVALID_HANDLE_VALUE;
    std::thread worker;
    std::uint64_t epoch = 0, last_message = 0, selection_seen = 0;
    std::wstring name, sid;
    void revoke(const char *reason, bool connected) {
        const auto connection = latest.connection_epoch;
        latest = {}; latest.connected = connected; latest.connection_epoch = connection; latest.reason = reason;
    }
    void expire() {
        if (latest.available && Clock::now() >= latest.valid_until) revoke("expired", latest.connected);
    }
    bool accept(const Request &r, const Header &h) {
        std::lock_guard lock(mutex);
        expire();
        if (h.epoch != epoch || h.sequence <= last_message) { revoke("protocol_sequence_invalid", false); return false; }
        last_message = h.sequence;
        if (r.mode == Mode::CANCEL) { revoke("cancelled", true); latest.message_sequence = h.sequence; return true; }
        if (r.mode == Mode::LOCATE) {
            if (r.observation.identity.selection_generation <= selection_seen) { revoke("locate_replay_rejected", true); return true; }
            selection_seen = r.observation.identity.selection_generation;
            latest.locate_sequence = h.sequence;
        } else if (!latest.available || r.observation.identity != latest.request.observation.identity ||
                   r.reference_version != latest.request.reference_version || r.throw_action != latest.request.throw_action ||
                   r.observation.sequence <= latest.request.observation.sequence) {
            revoke("observation_unbound_or_repeated", true); return true;
        }
        latest.connected = latest.available = true; latest.request = r;
        latest.connection_epoch = epoch; latest.message_sequence = h.sequence;
        latest.received_at = Clock::now();
        const auto transport = GetTickCount64() - h.tick;
        latest.valid_until = latest.received_at + kTtl - std::chrono::milliseconds(std::min<std::uint64_t>(350, transport));
        latest.reason = "ready";
        return true;
    }
    HANDLE create() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        // 只授予读/写数据及必要标准权限，不授予 FILE_CREATE_PIPE_INSTANCE。
        const auto sddl = L"D:P(A;;0x12019b;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        HANDLE result = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kMaxMessage, kMaxMessage, 0, &security);
        LocalFree(descriptor); return result;
    }
    void run() noexcept {
        try {
            while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
                Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!event.value) break;
                OVERLAPPED op{}; op.hEvent = event.value; DWORD bytes = 0;
                BOOL immediate = ConnectNamedPipe(pipe, &op);
                bool ready = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
                if (!ready) ready = complete(pipe, stop, op, immediate, bytes, INFINITE);
                if (!ready) break;
                ++epoch; if (!epoch) ++epoch;
                {
                    std::lock_guard lock(mutex);
                    latest = {}; latest.connected = true; latest.connection_epoch = epoch;
                    latest.reason = "awaiting_explicit_locate"; last_message = selection_seen = 0;
                    status = unavailable("awaiting_runtime_status");
                }
                Header hello; hello.epoch = epoch;
                ready = send(pipe, stop, hello, {{"kind","hello"}});
                while (ready && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
                    try {
                    Header h; Json payload;
                    if (!receive(pipe, stop, h, payload, INFINITE)) break;
                    const auto now_tick = GetTickCount64();
                    if (h.tick > now_tick || now_tick - h.tick > 350) break;
                    if (payload.value("mode", "") == "status") {
                        std::lock_guard lock(mutex);
                        if (h.epoch != epoch || h.sequence <= last_message) break;
                        last_message = h.sequence; expire();
                    } else if (!accept(decode(payload,h.tick), h)) break;
                    Json reply;
                    { std::lock_guard lock(mutex); expire(); reply = {{"kind","ack"},{"status",status},{"request_available",latest.available},{"reason",latest.reason}}; }
                    Header ack; ack.epoch = epoch; ack.sequence = h.sequence;
                    ready = send(pipe, stop, ack, reply);
                    } catch (...) { break; }
                }
                { std::lock_guard lock(mutex); revoke("disconnected",false); }
                DisconnectNamedPipe(pipe);
            }
        } catch (...) { std::lock_guard lock(mutex); revoke("protocol_error",false); }
        DisconnectNamedPipe(pipe);
    }
};
Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }
bool Server::start(const std::string &key) noexcept {
    stop();
    try {
        if (key.empty() || key.size() > 1024) return false;
        impl_->sid = logon_sid(); if (impl_->sid.empty()) return false;
        impl_->name = endpoint(key,impl_->sid);
        impl_->stop = CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!impl_->stop) return false;
        impl_->pipe = impl_->create(); if (impl_->pipe == INVALID_HANDLE_VALUE) { stop(); return false; }
        LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
        impl_->epoch = static_cast<std::uint64_t>(counter.QuadPart) ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        impl_->worker = std::thread([this] { impl_->run(); }); return true;
    } catch (...) { stop(); return false; }
}
void Server::stop() noexcept {
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->pipe != INVALID_HANDLE_VALUE) CloseHandle(impl_->pipe);
    if (impl_->stop) CloseHandle(impl_->stop);
    impl_->pipe = INVALID_HANDLE_VALUE; impl_->stop = nullptr;
    std::lock_guard lock(impl_->mutex); impl_->revoke("stopped",false); impl_->status = unavailable("runtime_status_unavailable");
}
Snapshot Server::snapshot() noexcept {
    try { std::lock_guard lock(impl_->mutex); impl_->expire(); return impl_->latest; }
    catch (...) { return {}; }
}
void Server::publish_status(const Json &status) noexcept {
    try {
        if (!status.is_object() || status.dump().size() > 8192) return;
        std::lock_guard lock(impl_->mutex); impl_->status = status;
    } catch (...) {}
}
struct Client::Impl {
    mutable std::mutex mutex;
    HANDLE stop = nullptr, wake = nullptr;
    std::thread worker;
    std::wstring name;
    bool online = false;
    std::optional<Request> pending;
    Json status = unavailable("disconnected");
    Clock::time_point pending_at{}, status_at{};
    void disconnected() { std::lock_guard lock(mutex); online = false; pending.reset(); status = unavailable("disconnected"); }
    void run() noexcept {
        try {
            while (WaitForSingleObject(stop,0) == WAIT_TIMEOUT) {
                // 最小数据权限：不申请 GENERIC_WRITE，避免额外创建实例权限。
                Handle pipe(CreateFileW(name.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                    0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,nullptr));
                if (pipe.value == INVALID_HANDLE_VALUE) { WaitForSingleObject(stop,100); continue; }
                Header hello; Json payload;
                if (!receive(pipe.value,stop,hello,payload) || payload.value("kind","") != "hello" || !hello.epoch || hello.sequence) {
                    WaitForSingleObject(stop,100); continue;
                }
                std::uint64_t sequence = 0;
                { std::lock_guard lock(mutex); pending.reset(); online = true; status = unavailable("runtime_status_unavailable"); }
                while (WaitForSingleObject(stop,0) == WAIT_TIMEOUT) {
                    HANDLE events[]{stop,wake};
                    if (WaitForMultipleObjects(2,events,FALSE,50) == WAIT_OBJECT_0) break;
                    Json request = {{"mode","status"}};
                    {
                        std::lock_guard lock(mutex);
                        if (pending && Clock::now() - pending_at < kTtl) request = encode(*pending);
                        pending.reset();
                    }
                    Header h; h.epoch = hello.epoch; h.sequence = ++sequence;
                    if (!send(pipe.value,stop,h,request)) break;
                    Header ack; Json reply;
                    if (!receive(pipe.value,stop,ack,reply) || ack.epoch != hello.epoch || ack.sequence != sequence || reply.value("kind","") != "ack") break;
                    { std::lock_guard lock(mutex); status = reply.at("status"); status["ipc_request_available"] = reply.at("request_available"); status["ipc_reason"] = reply.at("reason"); status_at = Clock::now(); }
                }
                disconnected();
            }
        } catch (...) { disconnected(); }
        disconnected();
    }
};
Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { stop(); }
bool Client::start(const std::string &key) noexcept {
    stop();
    try {
        if (key.empty() || key.size() > 1024) return false;
        auto sid = logon_sid(); if (sid.empty()) return false;
        impl_->name = endpoint(key,sid); impl_->stop = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        impl_->wake = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!impl_->stop || !impl_->wake) { stop(); return false; }
        impl_->worker = std::thread([this]{ impl_->run(); }); return true;
    } catch (...) { stop(); return false; }
}
void Client::stop() noexcept {
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->stop) CloseHandle(impl_->stop);
    if (impl_->wake) CloseHandle(impl_->wake);
    impl_->stop = impl_->wake = nullptr; impl_->disconnected();
}
bool Client::connected() const noexcept { std::lock_guard lock(impl_->mutex); return impl_->online; }
bool Client::publish(const Request &request) noexcept {
    try {
        Json message = encode(request); if (message.dump().size() > kMaxMessage) return false;
        std::lock_guard lock(impl_->mutex);
        if (!impl_->online) return false;
        auto latest = request;
        if (impl_->pending && impl_->pending->mode == Mode::LOCATE && latest.mode == Mode::OBSERVATION &&
            impl_->pending->observation.identity == latest.observation.identity &&
            impl_->pending->reference_version == latest.reference_version && impl_->pending->throw_action == latest.throw_action)
            latest.mode = Mode::LOCATE;
        impl_->pending = std::move(latest); impl_->pending_at = Clock::now();
        SetEvent(impl_->wake); return true;
    } catch (...) { return false; }
}
Json Client::status() const noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->online || Clock::now() - impl_->status_at >= kTtl) return unavailable("disconnected_or_stale");
        return impl_->status;
    } catch (...) { return unavailable("status_error"); }
}
}
