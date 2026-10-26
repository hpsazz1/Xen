#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#include "grenade_web/server.h"
#include "grenade_obs_page.h"
#include "movement/timeline.h"
#include "debug_session/debug_session.h"
#include "grenade_alignment/grenade_alignment.h"
#include "weapon/weapon.h"
#include "log/log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>

namespace grenade_web {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaximumHeader = 8192;
constexpr std::size_t kMaximumBody = 1024;
constexpr std::size_t kMaximumResponse = 1024 * 1024;
constexpr std::size_t kMaximumCatalogEntries = 512;
constexpr std::size_t kMaximumPlanBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumCatalogBytes = 32 * 1024 * 1024;
constexpr auto kCatalogTimeout = std::chrono::milliseconds(750);
constexpr auto kRequestTimeout = std::chrono::seconds(2);
constexpr auto kCaptureHeartbeat = std::chrono::seconds(1);
constexpr std::size_t kMaximumCapturePlan = 1024 * 1024;
std::atomic<std::uint64_t> service_sequence{0};

struct ScopedSocket {
    SOCKET value = INVALID_SOCKET;
    ~ScopedSocket() { if (value != INVALID_SOCKET) closesocket(value); }
};
bool nonblocking(SOCKET socket) {
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}
bool private_address(unsigned long network_address) noexcept {
    const auto ip = ntohl(network_address);
    return (ip & 0xff000000UL) == 0x7f000000UL || (ip & 0xff000000UL) == 0x0a000000UL ||
        (ip & 0xfff00000UL) == 0xac100000UL || (ip & 0xffff0000UL) == 0xc0a80000UL;
}
std::vector<std::string> local_urls(const Config& config, std::uint16_t port) {
    const auto url = [port](const std::string& address) { return "http://" + address + ":" + std::to_string(port) + "/"; };
    if (config.bind_address != "0.0.0.0") return {url(config.bind_address)};
    std::vector<std::pair<bool,std::string>> candidates;
    ULONG size = 16384;
    std::vector<unsigned char> bytes(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
    auto status = GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        bytes.resize(size); adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());
        status = GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&size);
    }
    if (status == NO_ERROR) for (auto* item = adapters; item; item = item->Next) {
        if (item->OperStatus != IfOperStatusUp || item->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        bool gateway = false;
        for (auto* entry=item->FirstGatewayAddress;entry;entry=entry->Next)
            if (entry->Address.lpSockaddr && entry->Address.lpSockaddr->sa_family==AF_INET &&
                reinterpret_cast<const sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr.s_addr != INADDR_ANY) gateway=true;
        for (auto* entry=item->FirstUnicastAddress;entry;entry=entry->Next) {
            if (!entry->Address.lpSockaddr || entry->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* address=reinterpret_cast<const sockaddr_in*>(entry->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (private_address(address->sin_addr.s_addr) && inet_ntop(AF_INET,&address->sin_addr,text,sizeof(text)))
                candidates.emplace_back(!gateway,url(text));
        }
    }
    std::sort(candidates.begin(),candidates.end());
    std::vector<std::string> result;
    for (const auto& [unused,address] : candidates)
        if (std::find(result.begin(),result.end(),address)==result.end()) result.push_back(address);
    result.push_back(url("127.0.0.1"));
    return result;
}
std::string plan_team(const std::string& name) {
    const auto title = " " + name;
    for (const auto* side : {"CT", "T"}) {
        const std::string token = std::string(" ") + side;
        const auto start = title.find(token);
        if (start == std::string::npos) continue;
        const auto suffix = title.substr(start + token.size());
        if ((!suffix.empty() && suffix.front() >= '0' && suffix.front() <= '9') || suffix.starts_with("来源")) return side;
    }
    return {};
}
class CatalogLimit final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
std::string sha256(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0)
        throw std::runtime_error("方案摘要初始化失败");
    struct Close { BCRYPT_ALG_HANDLE value; ~Close() { BCryptCloseAlgorithmProvider(value,0); } } close{algorithm};
    std::array<unsigned char,32> digest{};
    if (BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                   static_cast<ULONG>(bytes.size()),digest.data(),static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("方案摘要计算失败");
    std::string result;
    for (const auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}
bool f10_conflicts(const AppConfig& config) {
    const auto contains = [](const auto& keys) { return std::find(keys.begin(),keys.end(),0x79) != keys.end(); };
    const auto& key = config.keyboard;
    if (contains(key.aim_hold_virtual_keys) || contains(key.emergency_virtual_keys) ||
        contains(key.runtime_toggle_virtual_keys) || contains(key.anomaly_mark_virtual_keys) ||
        (key.debug_test_enabled && contains(key.debug_test_virtual_keys))) return true;
    if (key.grenade_assist_enabled && (contains(key.grenade_menu_virtual_keys) || contains(key.grenade_align_virtual_keys) ||
        contains(key.grenade_throw_virtual_keys) || contains(key.grenade_cancel_virtual_keys))) return true;
    return (config.trigger.enabled && config.trigger.fire_enabled && config.trigger.hold_virtual_key == 0x79) ||
        (config.recoil.enabled && config.recoil.hold_virtual_key == 0x79) ||
        (config.auto_stop.enabled && (config.auto_stop.activation_virtual_key == 0x79 || contains(config.auto_stop.release_virtual_keys))) ||
        (config.movement.enabled && ((config.movement.spin_enabled && config.movement.spin_trigger == movement::Trigger::KEY &&
            config.movement.spin_virtual_key == 0x79) || (config.movement.large_enabled &&
            config.movement.large_trigger == movement::Trigger::KEY && config.movement.large_virtual_key == 0x79)));
}
// 复用正式 Plan 解析；库文件格式/重复字段/注释规则与正式库保持一致。
// 网页额外限制总扫描量，并在分块读取及 JSON 事件间协作停止。
template<class Check>
movement::timeline::Plan read_catalog_plan(const std::filesystem::path& path, std::size_t& total, Check&& check) {
    check();
    const auto declared = std::filesystem::file_size(path);
    if (declared > kMaximumPlanBytes) throw CatalogLimit("单个道具方案超过16MiB");
    if (declared > kMaximumCatalogBytes - total) throw CatalogLimit("道具清单累计输入超过32MiB");
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("无法读取道具方案");
    std::string text;
    while (file) {
        check();
        char bytes[16 * 1024];
        const auto limit = std::min({sizeof(bytes),kMaximumPlanBytes - text.size() + 1,kMaximumCatalogBytes - total + 1});
        file.read(bytes,static_cast<std::streamsize>(limit));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > kMaximumPlanBytes - text.size()) throw CatalogLimit("单个道具方案超过16MiB");
        if (count > kMaximumCatalogBytes - total) throw CatalogLimit("道具清单累计输入超过32MiB");
        total += count; text.append(bytes,count);
        check();
    }
    if (file.bad()) throw std::runtime_error("道具方案读取失败");
    std::vector<std::set<std::string>> keys;
    const auto document = Json::parse(text,[&](int depth, Json::parse_event_t event, Json& value) {
        check();
        if (depth > 32) throw std::runtime_error("方案嵌套过深");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::runtime_error("方案包含重复字段");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    },true,true);
    check();
    movement::timeline::Plan plan;
    std::string error;
    const bool valid = movement::timeline::parse_plan(document,plan,error);
    check();
    if (!valid) throw std::runtime_error(error);
    return plan;
}
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
std::string decode_plan_id(std::string_view value) {
    std::string result;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 :
        c >= 'A' && c <= 'F' ? c-'A'+10 : -1; };
    for (std::size_t i=0;i<value.size();++i) {
        if (value[i] != '%') result += value[i];
        else {
            if (i+2 >= value.size() || hex(value[i+1]) < 0 || hex(value[i+2]) < 0) return {};
            result += static_cast<char>((hex(value[i+1]) << 4) | hex(value[i+2])); i += 2;
        }
    }
    if (result.empty() || result.size() > 160 || result.find_first_of("/\\:") != std::string::npos ||
        std::any_of(result.begin(),result.end(),[](unsigned char c){return c<32 || c==127;})) return {};
    return result;
}
std::string read_resource(const std::filesystem::path& path, std::size_t maximum, Clock::time_point deadline) {
    const auto size = std::filesystem::file_size(path);
    if (!size || size > maximum) throw std::runtime_error("参考资源为空或过大");
    std::ifstream file(path,std::ios::binary);
    std::string bytes;
    while (file && bytes.size() < size) {
        if (Clock::now() >= deadline) throw std::runtime_error("读取参考资源超时");
        char buffer[16384]; file.read(buffer,std::min<std::size_t>(sizeof(buffer),size-bytes.size()));
        bytes.append(buffer,static_cast<std::size_t>(file.gcount()));
    }
    if (bytes.size() != size || file.bad()) throw std::runtime_error("参考资源读取失败");
    return bytes;
}
// 固定 JSON 选择入口；拒绝分块、重复长度与管线请求，保持单请求单连接。
// 返回 0 表示尚未收齐，其他值为 HTTP 状态。
int request_target(std::string_view request, std::string& method, std::string& target, std::string& body) {
    const auto end = request.find("\r\n\r\n");
    if (end == std::string_view::npos) return request.size() > kMaximumHeader ? 431 : 0;
    if (end + 4 > kMaximumHeader) return 431;
    const auto line_end = request.find("\r\n");
    const auto line = request.substr(0, line_end);
    const auto first = line.find(' '), last = line.rfind(' ');
    if (first == std::string_view::npos || first == last || line.find(' ', first + 1) != last) return 400;
    if (line.substr(last + 1) != "HTTP/1.1" && line.substr(last + 1) != "HTTP/1.0") return 400;
    method = line.substr(0, first);
    if (method != "GET" && method != "POST") return 405;
    const auto path = line.substr(first + 1, last - first - 1);
    if (path.empty() || path.size() > 1024 || path.front() != '/') return 400;
    bool length_seen = false;
    std::size_t length = 0;
    std::string content_type, host, origin;
    for (auto begin = line_end + 2; begin < end; ) {
        const auto next = request.find("\r\n", begin);
        const auto entry = request.substr(begin, next - begin);
        const auto colon = entry.find(':');
        if (colon == std::string_view::npos || colon == 0 || entry.front() == ' ' || entry.front() == '\t') return 400;
        std::string name(entry.substr(0, colon));
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const auto value = trim(entry.substr(colon + 1));
        if (name == "transfer-encoding" || name == "expect") return 400;
        if (name == "content-length") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (length_seen || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length > kMaximumBody) return 400;
            length_seen = true;
        }
        if (name == "content-type") content_type = value;
        if (name == "host") host = value;
        if (name == "origin") origin = value;
        begin = next + 2;
    }
    target = path;
    if (method == "POST" && target != "/select.json") return 405;
    if (method == "GET" && length != 0) return 400;
    if (method == "POST" && (!length_seen || length == 0 || content_type != "application/json" ||
        (!origin.empty() && origin != "http://" + host))) return 400;
    if (request.size() < end + 4 + length) return 0;
    if (request.size() != end + 4 + length) return 400;
    body = request.substr(end + 4);
    return 200;
}
}

struct Server::Impl {
    mutable std::mutex mutex;
    Snapshot state;
    Config config;
    std::shared_ptr<weapon::GsiReceiver> gsi;
    std::atomic<bool> stopping{false};
    std::thread worker;
    SOCKET listener = INVALID_SOCKET;
    bool winsock = false;
    std::string prefix;
    std::uint64_t generation = 0;
    std::optional<std::tuple<bool, std::string, weapon::Team, std::uint64_t>> identity;
    weapon::MapSnapshot current;
    std::string catalog;
    Clock::time_point next_catalog{};
    std::shared_ptr<const debug_session::Snapshot> capture_selection;
    Clock::time_point capture_received{};
    bool capture_available = false;
    std::string capture_reason;
    std::uint64_t capture_revision = 0, capture_generation = 0;
    std::optional<std::tuple<std::uint64_t,std::uint64_t,bool,std::string>> capture_identity;
    std::optional<SelectionRequest> selection_request;
    std::uint64_t selection_sequence = 0;
    bool selection_delivered = false;
    std::string selection_state = "idle", selection_reason, selection_context;
    Clock::time_point selection_deadline{};
    std::map<std::string,std::pair<std::string,std::string>> reference_cache;

    // 调用方持有mutex；普通心跳和手机选择完成共用同一快照/版本发布规则。
    void publish_capture_locked(std::shared_ptr<const debug_session::Snapshot> selection,
                                bool available, const std::string& reason, Clock::time_point now) {
        const auto& old = capture_selection;
        const bool changed = capture_available != available || capture_reason != reason ||
            now >= capture_received + kCaptureHeartbeat || static_cast<bool>(old) != static_cast<bool>(selection) ||
            (old && selection && old != selection &&
                (old->generation != selection->generation || old->state != selection->state ||
                 old->busy != selection->busy || old->physical != selection->physical ||
                 old->cleanup_unknown != selection->cleanup_unknown || old->movement_recording != selection->movement_recording ||
                 old->movement_test_enabled != selection->movement_test_enabled || old->movement_test_mode != selection->movement_test_mode ||
                 old->grenade_selected_path != selection->grenade_selected_path ||
                 old->grenade_loaded_revision != selection->grenade_loaded_revision ||
                 old->grenade_current != selection->grenade_current || old->grenade_baseline != selection->grenade_baseline));
        if (changed) {
            if (capture_revision == std::numeric_limits<std::uint64_t>::max()) {
                capture_selection.reset(); capture_available = false;
                capture_reason = "采集状态代际已耗尽"; return;
            }
            ++capture_revision;
        }
        capture_selection = std::move(selection); capture_received = now;
        capture_available = available; capture_reason = reason;
    }

    void update_context() {
        current = gsi->map_snapshot();
        const auto now = Clock::now();
        if (!current.valid || current.map_name.empty() || now < current.received_at || now >= current.valid_until) {
            current.valid = false; current.map_name.clear(); current.local_team = weapon::Team::UNKNOWN;
        }
        const auto next = std::make_tuple(current.valid, current.map_name, current.local_team, current.context_epoch);
        if (!identity || next != *identity) {
            if (generation == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("网页状态代际已耗尽");
            ++generation; identity = next;
            std::lock_guard lock(mutex);
            selection_context = prefix + "-" + std::to_string(generation);
            if (selection_request) {
                selection_state = "failed";
                selection_reason = "地图或阵营已改变，请重新选择";
            }
        }
        std::lock_guard lock(mutex);
        if (selection_state == "pending" && now >= selection_deadline) {
            selection_state = "failed"; selection_reason = "方案载入超时，请重新选择";
        }
    }
    std::string selection_json() {
        update_context();
        std::lock_guard lock(mutex);
        Json result{{"state",selection_state},{"reason",selection_reason},
            {"request_id",selection_request ? std::to_string(selection_request->id) : ""},
            {"plan_id",selection_request ? selection_request->plan_id : ""},
            {"generation",selection_request ? selection_request->generation : selection_context},
            {"actual_plan_id",""},{"guidance",{{"state","hidden"},{"plan_id",""},{"message",""}}}};
        const auto& selected = capture_selection;
        if (selected && current.valid && Clock::now() < capture_received + kCaptureHeartbeat &&
            selected->movement_test_enabled && selected->movement_test_mode == debug_session::Mode::GRENADE_TEST &&
            selected->grenade_current.value("map",std::string{}) == current.map_name) {
            const auto id = std::filesystem::u8path(selected->grenade_selected_path).filename().u8string();
            result["actual_plan_id"] = std::string(id.begin(),id.end());
            result["guidance"] = {{"state",selected->grenade_guidance_state},
                {"plan_id",selected->grenade_guidance_plan_id},{"message",selected->grenade_guidance_message}};
        }
        return result.dump();
    }
    std::filesystem::path catalog_path(const std::string& id, Clock::time_point deadline) {
        if (id.empty() || !refresh_catalog(deadline)) throw std::runtime_error("道具清单暂不可用");
        update_context();
        const auto entries = Json::parse(catalog);
        if (!current.valid || std::none_of(entries.begin(),entries.end(),[&](const auto& item) {
            return item["id"] == id && item["map"] == current.map_name && item["team"] == weapon::team_name(current.local_team);
        })) throw std::runtime_error("当前地图与阵营中没有此方案");
        const auto root = std::filesystem::canonical(std::filesystem::u8path(config.plans_root));
        const auto path = std::filesystem::canonical(root / std::filesystem::u8path(id));
        if (path.parent_path() != root || path.extension() != ".json") throw std::runtime_error("方案文件位置无效");
        return path;
    }
    std::pair<std::string,std::string> reference_resource(const std::string& id, const std::string& kind,
                                                        Clock::time_point deadline, const std::string& expected_revision = {}) {
        const auto path = catalog_path(id,deadline);
        const auto root = std::filesystem::canonical(path.parent_path().parent_path() / "references");
        const auto directory = std::filesystem::canonical(root / path.stem());
        if (directory.parent_path() != root) throw std::runtime_error("参考资源位置无效");
        std::string identity = id;
        for (const auto& file : {path,directory/"reference.json",directory/"standing.png",directory/"aim.png"}) {
            identity += ":" + std::to_string(std::filesystem::last_write_time(file).time_since_epoch().count()) +
                ":" + std::to_string(std::filesystem::file_size(file));
        }
        const auto cached = reference_cache.find(id);
        if (kind == "manifest" && cached != reference_cache.end() && identity == cached->second.first)
            return {"application/json; charset=utf-8",cached->second.second};
        const auto manifest_bytes = read_resource(directory / "reference.json",1024*1024,deadline);
        const auto manifest = Json::parse(manifest_bytes);
        const auto plan = Json::parse(read_resource(path,1024*1024,deadline),nullptr,true,true);
        const auto reference = grenade_alignment::load_reference(path,plan);
        if (!expected_revision.empty() && reference.revision != expected_revision)
            throw std::runtime_error("参考图版本已更新，请重新读取标记");
        if (read_resource(directory / "reference.json",1024*1024,deadline) != manifest_bytes)
            throw std::runtime_error("参考资源已更新，请重试");
        if (kind == "manifest") {
            Json value{{"ready",true},{"plan_id",id},{"revision",reference.revision},
                {"standing_width",reference.standing_image.cols},{"standing_height",reference.standing_image.rows},
                {"geometry",manifest.at("geometry")},{"target_x",reference.target.x},{"target_y",reference.target.y},
                {"standing_note",manifest.value("standing_note",std::string{})}};
            if (manifest.contains("standing_marker")) value["standing_marker"] = manifest["standing_marker"];
            if (reference_cache.size() >= 32 && !reference_cache.contains(id)) reference_cache.clear();
            auto metadata=value.dump(); reference_cache[id]={identity,metadata};
            return {"application/json; charset=utf-8",std::move(metadata)};
        }
        const auto image_path = std::filesystem::canonical(directory / (kind + ".png"));
        if (image_path.parent_path() != directory) throw std::runtime_error("参考图片位置无效");
        auto bytes = read_resource(image_path,64*1024*1024,deadline);
        if (sha256(bytes) != manifest.at("image_sha256").at(kind + "_image").get<std::string>())
            throw std::runtime_error("参考图片已更新，请重试");
        return {"image/png",std::move(bytes)};
    }
    std::pair<int,std::string> select_plan(const std::string& body, Clock::time_point deadline) {
        update_context();
        const auto document = Json::parse(body,nullptr,false);
        const auto reject = [](const char* reason) {
            return std::pair{409,Json{{"state","failed"},{"reason",reason}}.dump()};
        };
        if (!document.is_object() || !document.contains("plan_id") || !document["plan_id"].is_string() ||
            !document.contains("generation") || !document["generation"].is_string())
            return reject("方案选择格式无效");
        const auto plan_id = document["plan_id"].get<std::string>();
        if (plan_id.empty() || plan_id.size() > 160 || plan_id.find_first_of("/\\:\0",0,4) != std::string::npos)
            return reject("方案标识无效");
        if (!refresh_catalog(deadline)) return reject("道具清单暂不可用");
        update_context();
        if (!current.valid || current.local_team == weapon::Team::UNKNOWN ||
            document["generation"] != prefix + "-" + std::to_string(generation))
            return reject("地图或阵营已改变，请重新选择");
        const auto entries = Json::parse(catalog);
        const auto found = std::find_if(entries.begin(),entries.end(),[&](const auto& item) {
            return item["id"] == plan_id && item["map"] == current.map_name &&
                item["team"] == weapon::team_name(current.local_team);
        });
        if (found == entries.end()) return reject("当前地图与阵营中没有此方案");
        const auto path = catalog_path(plan_id,deadline).u8string();
        std::lock_guard lock(mutex);
        if (selection_state == "pending") return reject("正在载入方案，请稍候");
        if (selection_sequence == std::numeric_limits<std::uint64_t>::max()) return reject("方案选择序号已耗尽");
        selection_request = SelectionRequest{++selection_sequence,current.context_epoch,plan_id,
            std::string(path.begin(),path.end()),current.map_name,weapon::team_name(current.local_team),selection_context};
        selection_state = "pending"; selection_reason.clear(); selection_delivered = false;
        selection_deadline = Clock::now() + std::chrono::seconds(8);
        return {202,Json{{"state","pending"},{"request_id",std::to_string(selection_request->id)},
            {"plan_id",plan_id},{"generation",selection_context}}.dump()};
    }
    bool wait_socket(SOCKET socket, bool writing, Clock::time_point deadline) {
        while (!stopping && Clock::now() < deadline) {
            update_context();
            fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
            timeval timeout{0, 10000};
            const auto result = select(0, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &timeout);
            if (result == SOCKET_ERROR) {
                if (socket == listener && !stopping) throw std::runtime_error("网页监听等待失败");
                return false;
            }
            if (result > 0) return true;
        }
        return false;
    }
    std::string context_json() {
        update_context();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(current.valid_until - Clock::now()).count();
        const bool valid = current.valid && remaining > 0;
        return Json{{"valid",valid},{"map",valid ? current.map_name : std::string{}},
            {"team",valid && current.local_team != weapon::Team::UNKNOWN ? weapon::team_name(current.local_team) : ""},
            {"epoch",std::to_string(current.context_epoch)},{"generation",prefix + "-" + std::to_string(generation)},
            {"remaining_ms",valid ? std::min<std::int64_t>(remaining,10000) : 0}}.dump();
    }
    std::string capture_context_json(Clock::time_point request_deadline) {
        update_context();
        std::shared_ptr<const debug_session::Snapshot> selected;
        Clock::time_point received;
        std::uint64_t revision;
        bool available;
        std::string reason;
        {
            std::lock_guard lock(mutex);
            selected = capture_selection; received = capture_received; revision = capture_revision;
            available = capture_available; reason = capture_reason;
        }
        Json result{{"ready",false},{"reason",""},{"map",""},{"team",""},{"generation",""},
            {"remaining_ms",0},{"plan_id",""},{"plan",Json::object()},{"plan_sha256",""}};
        try {
            const auto initial_generation = generation;
            const auto check = [&] {
                update_context();
                const auto now = Clock::now();
                if (stopping || now >= request_deadline) throw std::runtime_error("采集方案读取已停止或超时");
                if (!selected || received == Clock::time_point{} || now < received || now >= received + kCaptureHeartbeat)
                    throw std::runtime_error("辅机界面状态未就绪或已过期");
                if (!current.valid || current.local_team == weapon::Team::UNKNOWN || generation != initial_generation)
                    throw std::runtime_error("游戏地图或本地阵营未就绪");
                std::lock_guard lock(mutex);
                if (revision != capture_revision) throw std::runtime_error("采集期间当前方案或状态已改变");
            };
            check();
            if (!available) throw std::runtime_error(reason.empty() ? "辅机当前不允许制作采集" : reason);
            // physical记录上次任务种类，成功试投后仍可能为true；只拒绝在途输出与清理未知。
            if (selected->busy || selected->cleanup_unknown || selected->movement_recording ||
                selected->state == debug_session::State::RUNNING || selected->state == debug_session::State::STOPPING)
                throw std::runtime_error("请先结束当前运行或录制，再按F10采集");
            if (!selected->movement_test_enabled || selected->movement_test_mode != debug_session::Mode::GRENADE_TEST)
                throw std::runtime_error("请先在辅机启用当前道具方案测试");
            if (selected->grenade_selected_path.empty() || !selected->grenade_current.is_object() ||
                selected->grenade_current != selected->grenade_baseline)
                throw std::runtime_error("当前道具尚未保存或存在未保存修改");
            const auto root = std::filesystem::canonical(std::filesystem::u8path(config.plans_root));
            const auto path = std::filesystem::u8path(selected->grenade_selected_path);
            if (path.extension() != ".json" || std::filesystem::is_symlink(path) ||
                std::filesystem::canonical(path).parent_path() != root ||
                std::filesystem::absolute(path).lexically_normal().parent_path() != root)
                throw std::runtime_error("当前道具不在正式方案库中");
            check();
            if (std::filesystem::file_size(path) > kMaximumCapturePlan) throw std::runtime_error("采集方案超过1MiB");
            std::ifstream input(path,std::ios::binary);
            if (!input) throw std::runtime_error("当前道具文件不可读");
            std::string bytes;
            while (input) {
                check();
                char buffer[16384];
                input.read(buffer,std::min<std::size_t>(sizeof(buffer),kMaximumCapturePlan - bytes.size() + 1));
                bytes.append(buffer,static_cast<std::size_t>(input.gcount()));
                if (bytes.size() > kMaximumCapturePlan) throw std::runtime_error("采集方案超过1MiB");
            }
            if (input.bad()) throw std::runtime_error("当前道具文件读取失败");
            std::vector<std::set<std::string>> keys;
            const auto plan = Json::parse(bytes,[&](int depth, Json::parse_event_t event, Json& value) {
                check();
                if (depth > 32) throw std::runtime_error("道具方案嵌套过深");
                if (event == Json::parse_event_t::object_start) keys.emplace_back();
                if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    throw std::runtime_error("道具方案字段重复");
                if (event == Json::parse_event_t::object_end) keys.pop_back();
                return true;
            },true,true);
            movement::timeline::Plan parsed, expected;
            std::string error;
            if (!movement::timeline::parse_plan(plan,parsed,error) ||
                !movement::timeline::parse_plan(selected->grenade_current,expected,error) ||
                movement::timeline::plan_json(parsed) != movement::timeline::plan_json(expected))
                throw std::runtime_error("磁盘道具方案与已启用方案不一致，请重新载入");
            if (parsed.map != current.map_name || plan_team(parsed.name) != weapon::team_name(current.local_team))
                throw std::runtime_error("当前方案与游戏地图或本地阵营不匹配");
            const auto name = path.filename().u8string();
            const std::string plan_id(name.begin(),name.end());
            if (plan_id.size() > 160 || plan_id.find_first_of("/\\:") != std::string::npos)
                throw std::runtime_error("当前方案文件名不能用于采集");
            const auto hash = sha256(bytes);
            check();
            const auto until = std::min(current.valid_until,received + kCaptureHeartbeat);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("采集上下文已经过期");
            result.update({{"ready",true},{"map",current.map_name},{"team",weapon::team_name(current.local_team)},
                {"remaining_ms",remaining},{"plan_id",plan_id},{"plan",plan},{"plan_sha256",hash}});
        } catch (const std::exception&) {
            // 错误正文不回传系统路径、原始JSON或配置；诊断只使用受控中文状态。
            try { throw; }
            catch (const std::filesystem::filesystem_error&) { result["reason"] = "当前方案文件不存在或不可读"; }
            catch (const Json::exception&) { result["reason"] = "当前方案文件格式无效"; }
            catch (const std::exception& error) { result["reason"] = error.what(); }
        }
        const auto next = std::make_tuple(revision,generation,result["ready"].get<bool>(),
            result["ready"].get<bool>() ? result["plan_sha256"].get<std::string>() : result["reason"].get<std::string>());
        if (!capture_identity || next != *capture_identity) {
            if (capture_generation == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("采集代际已耗尽");
            ++capture_generation; capture_identity = next;
        }
        result["generation"] = prefix + "-capture-" + std::to_string(capture_generation);
        auto encoded = result.dump();
        if (encoded.size() > kMaximumResponse) return Json{{"ready",false},{"reason","采集方案响应超过大小限制"},
            {"map",""},{"team",""},{"generation",result["generation"]},{"remaining_ms",0},
            {"plan_id",""},{"plan",Json::object()},{"plan_sha256",""}}.dump();
        return encoded;
    }
    bool refresh_catalog(Clock::time_point request_deadline) {
        if (Clock::now() < next_catalog) return !catalog.empty();
        next_catalog = Clock::now() + std::chrono::seconds(2);
        try {
            const auto deadline = std::min(request_deadline,Clock::now() + kCatalogTimeout);
            auto next_context = Clock::time_point{};
            const auto check = [&] {
                if (stopping) throw CatalogLimit("道具清单读取已取消");
                const auto now = Clock::now();
                if (now >= deadline) throw CatalogLimit("道具清单读取超过时间预算");
                if (now >= next_context) { update_context(); next_context = now + std::chrono::milliseconds(10); }
            };
            std::vector<std::pair<std::string,std::string>> entries;
            std::size_t visited = 0, total = 0, response_bytes = 2;
            const auto root = std::filesystem::u8path(config.plans_root);
            check();
            if (std::filesystem::exists(root)) for (const auto& entry : std::filesystem::directory_iterator(root)) {
                check();
                if (++visited > kMaximumCatalogEntries) throw CatalogLimit("道具目录条目超过512项");
                // 与正式库相同：只读本层 .json，忽略临时文件、子目录和符号链接。
                if (entry.path().extension() != ".json") continue;
                try {
                    if (entry.is_symlink() || !entry.is_regular_file()) continue;
                    const auto plan = read_catalog_plan(entry.path(),total,check);
                    const auto filename = entry.path().filename().u8string();
                    const std::string id(filename.begin(),filename.end());
                    auto encoded = Json{{"id",id},{"name",plan.name},{"map",plan.map},{"team",plan_team(plan.name)}}.dump();
                    check();
                    response_bytes += encoded.size() + (entries.empty() ? 0 : 1);
                    if (response_bytes > kMaximumResponse) throw CatalogLimit("网页道具清单超过响应上限");
                    auto sort_key = plan.name;
                    if (!plan.map.empty()) sort_key += " / " + plan.map;
                    if (!plan.location.empty()) sort_key += " / " + plan.location;
                    sort_key += " [方案库/" + id + "]";
                    entries.emplace_back(std::move(sort_key),std::move(encoded));
                } catch (const CatalogLimit&) { throw; }
                catch (const std::exception&) { /* 单个坏文件不阻断正式方案列表。 */ }
            }
            check();
            std::sort(entries.begin(),entries.end(),[](const auto& a, const auto& b) { return a.first < b.first; });
            std::string encoded = "[";
            for (const auto& entry : entries) {
                check();
                if (encoded.size() > 1) encoded += ',';
                encoded += entry.second;
            }
            encoded += ']';
            check();
            catalog = std::move(encoded);
            std::lock_guard lock(mutex); state.error.clear();
            return true;
        } catch (const std::exception& error) {
            catalog.clear();
            std::lock_guard lock(mutex); state.error = "道具清单读取失败：" + std::string(error.what());
            if (!stopping) LOG_WARN("grenade_web", "{}", state.error);
            return false;
        }
    }
    void respond(SOCKET client, int status, std::string_view content_type, std::string_view body, Clock::time_point deadline) {
        const auto reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 409 ? "Conflict" :
            status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" :
            status == 431 ? "Request Header Fields Too Large" : status == 503 ? "Service Unavailable" : "Bad Request";
        const auto response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " +
            std::string(content_type) + "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" +
            (status == 405 ? "Allow: GET, POST\r\n" : "") + "\r\n" + std::string(body);
        std::size_t sent = 0;
        while (sent < response.size() && wait_socket(client, true, deadline)) {
            const auto count = ::send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (count <= 0) return;
            sent += static_cast<std::size_t>(count);
        }
    }
    void handle(SOCKET client) {
        const auto deadline = Clock::now() + kRequestTimeout;
        std::string request;
        while (wait_socket(client, false, deadline)) {
            char buffer[2048];
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (count <= 0) return;
            request.append(buffer, static_cast<std::size_t>(count));
            std::string method, target, body;
            const auto status = request_target(request,method,target,body);
            if (status == 0) continue;
            if (status != 200) respond(client,status,"text/plain; charset=utf-8","请求格式或入口不支持",deadline);
            else if (method == "POST") {
                try {
                    const auto [code,result] = select_plan(body,deadline);
                    respond(client,code,"application/json; charset=utf-8",result,deadline);
                } catch (const std::exception&) {
                    respond(client,409,"application/json; charset=utf-8",Json{{"state","failed"},
                        {"reason","方案文件已改变，请刷新清单后重选"}}.dump(),deadline);
                }
            }
            else if (target == "/") respond(client,404,"text/plain; charset=utf-8","手机选择入口已移除，请在主机OBS中按小键盘右键选择道具。",deadline);
            else if (target == "/obs-menu") respond(client,200,"text/html; charset=utf-8",kObsMenuPage,deadline);
            else if (target == "/context.json") respond(client,200,"application/json; charset=utf-8",context_json(),deadline);
            else if (target == "/selection.json") respond(client,200,"application/json; charset=utf-8",selection_json(),deadline);
            else if (target.starts_with("/reference.json?plan_id=") || target.starts_with("/reference/standing.png?plan_id=") ||
                     target.starts_with("/reference/aim.png?plan_id=")) {
                try {
                    const auto query = std::string_view(target).substr(target.find("?plan_id=") + 9);
                    const auto separator = query.find('&');
                    std::string expected_revision;
                    if (separator != std::string_view::npos) {
                        const auto revision = query.substr(separator);
                        if (!revision.starts_with("&revision=") || revision.size() != 74 ||
                            revision.substr(10).find_first_not_of("0123456789abcdef") != std::string_view::npos)
                            throw std::runtime_error("参考资源参数无效");
                        expected_revision = revision.substr(10);
                    }
                    const auto id = decode_plan_id(query.substr(0,separator));
                    const auto kind = target.starts_with("/reference.json") ? "manifest" :
                        target.starts_with("/reference/standing") ? "standing" : "aim";
                    const auto [type,bytes] = reference_resource(id,kind,deadline,expected_revision);
                    respond(client,200,type,bytes,deadline);
                } catch (const std::exception&) {
                    respond(client,404,"application/json; charset=utf-8",Json{{"ready",false},
                        {"reason","这套方案的参考图尚未就绪，请在正确站位和瞄角按F10采集"}}.dump(),deadline);
                }
            }
            else if (target == "/capture-context.json") respond(client,200,"application/json; charset=utf-8",capture_context_json(deadline),deadline);
            else if (target == "/catalog.json") {
                if (refresh_catalog(deadline)) respond(client,200,"application/json; charset=utf-8",catalog,deadline);
                else respond(client,503,"text/plain; charset=utf-8","道具清单读取失败",deadline);
            } else respond(client,404,"text/plain; charset=utf-8","页面不存在",deadline);
            return;
        }
    }
    void run() noexcept {
        try {
            while (!stopping) {
                if (!wait_socket(listener, false, Clock::now() + std::chrono::milliseconds(20))) continue;
                sockaddr_in peer{}; int size = sizeof(peer);
                ScopedSocket client{accept(listener,reinterpret_cast<sockaddr*>(&peer),&size)};
                if (client.value == INVALID_SOCKET) {
                    const auto error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK || error == WSAECONNRESET) continue;
                    throw std::runtime_error("网页监听失败");
                }
                if (!private_address(peer.sin_addr.s_addr) || !nonblocking(client.value)) continue;
                handle(client.value);
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex); state.error = error.what();
            LOG_ERROR("grenade_web", "网页服务已停止：{}", state.error);
        } catch (...) {
            std::lock_guard lock(mutex); state.error = "网页服务发生未知异常";
        }
        std::lock_guard lock(mutex); state.running = false;
    }
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }
bool Server::start(const Config& config, std::shared_ptr<weapon::GsiReceiver> gsi) noexcept {
    stop();
    try {
        Log::register_module("grenade_web",LogLevel::INFO);
        if (!gsi || config.plans_root.empty()) throw std::runtime_error("网页缺少共享GSI或正式方案库路径");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.port);
        if (inet_pton(AF_INET,config.bind_address.c_str(),&address.sin_addr) != 1 ||
            (address.sin_addr.s_addr != INADDR_ANY && !private_address(address.sin_addr.s_addr)))
            throw std::runtime_error("网页仅允许绑定本机或局域网IPv4地址");
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2,2),&data) != 0) throw std::runtime_error("网页网络初始化失败");
        impl_->winsock = true;
        impl_->listener = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        BOOL exclusive = TRUE;
        if (impl_->listener == INVALID_SOCKET ||
            setsockopt(impl_->listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)) != 0 ||
            bind(impl_->listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) != 0 ||
            listen(impl_->listener,4) != 0 || !nonblocking(impl_->listener)) throw std::runtime_error("网页端口绑定失败");
        int size = sizeof(address);
        if (getsockname(impl_->listener,reinterpret_cast<sockaddr*>(&address),&size) != 0) throw std::runtime_error("网页端口读取失败");
        impl_->config = config; impl_->gsi = std::move(gsi);
        impl_->prefix = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++service_sequence) + "-" +
            std::to_string(Clock::now().time_since_epoch().count());
        impl_->identity.reset(); impl_->generation = 0; impl_->catalog.clear(); impl_->next_catalog = {};
        impl_->reference_cache.clear();
        impl_->capture_identity.reset(); impl_->capture_generation = 0;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->state = {true,ntohs(address.sin_port),{},local_urls(config,ntohs(address.sin_port))};
            impl_->capture_selection.reset(); impl_->capture_received = {}; impl_->capture_available = false;
            impl_->capture_reason.clear(); impl_->capture_revision = 0;
            impl_->selection_request.reset(); impl_->selection_sequence = 0; impl_->selection_delivered = false;
            impl_->selection_state = "idle"; impl_->selection_reason.clear(); impl_->selection_context.clear();
        }
        impl_->stopping = false;
        impl_->worker = std::thread([this] { impl_->run(); });
        LOG_INFO("grenade_web", "道具网页已启动，端口={}", ntohs(address.sin_port));
        return true;
    } catch (const std::exception& error) {
        stop(); std::lock_guard lock(impl_->mutex); impl_->state.error = error.what(); return false;
    } catch (...) {
        stop(); std::lock_guard lock(impl_->mutex); impl_->state.error = "网页服务启动异常"; return false;
    }
}
void Server::publish_capture(std::shared_ptr<const debug_session::Snapshot> selection,
                            const AppConfig& controls, bool available, const std::string& reason) noexcept {
    try {
        const auto conflict = f10_conflicts(controls);
        const auto message = conflict ? "F10已被辅机控制快捷键占用，请先调整该绑定" : reason;
        available = available && !conflict;
        const auto now = Clock::now();
        std::lock_guard lock(impl_->mutex);
        impl_->publish_capture_locked(std::move(selection),available,message,now);
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        impl_->capture_selection.reset(); impl_->capture_available = false;
        impl_->capture_received = {};
    }
}
std::optional<SelectionRequest> Server::poll_selection() noexcept {
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->selection_state != "pending" || impl_->selection_delivered ||
            Clock::now() >= impl_->selection_deadline) return {};
        impl_->selection_delivered = true;
        return impl_->selection_request;
    } catch (...) { return {}; }
}
bool Server::selection_current(const SelectionRequest& request) const noexcept {
    try {
        if (!impl_->gsi) return false;
        const auto map = impl_->gsi->map_snapshot();
        const auto now = Clock::now();
        std::lock_guard lock(impl_->mutex);
        return !impl_->stopping && impl_->selection_state == "pending" && impl_->selection_request &&
            impl_->selection_request->id == request.id && impl_->selection_context == request.generation &&
            now < impl_->selection_deadline && map.valid && now >= map.received_at && now < map.valid_until &&
            map.map_name == request.map && map.context_epoch == request.map_epoch &&
            weapon::team_name(map.local_team) == request.team;
    } catch (...) { return false; }
}
void Server::finish_selection(const SelectionRequest& request,
                              std::shared_ptr<const debug_session::Snapshot> selection,
                              const std::string& reason) noexcept {
    try {
        const auto valid = selection_current(request);
        std::lock_guard lock(impl_->mutex);
        if (!impl_->selection_request || impl_->selection_request->id != request.id ||
            impl_->selection_request->generation != request.generation || impl_->selection_state != "pending") return;
        const bool ok = selection && valid && impl_->selection_context == request.generation;
        if (ok) impl_->publish_capture_locked(std::move(selection),impl_->capture_available,
                                             impl_->capture_reason,Clock::now());
        impl_->selection_state = ok ? "selected" : "failed";
        impl_->selection_reason = ok ? "" : !valid ? "地图、阵营或请求已改变，请重新选择" :
            reason.empty() ? "当前状态不允许切换方案" : reason.substr(0,512);
    } catch (...) {}
}
void Server::stop() noexcept {
    impl_->stopping = true;
    if (impl_->worker.joinable()) impl_->worker.join();
    // 工作线程已退出，关闭监听不会与 accept/select 竞争或复用句柄。
    if (impl_->listener != INVALID_SOCKET) closesocket(impl_->listener);
    impl_->listener = INVALID_SOCKET;
    if (impl_->winsock) WSACleanup();
    impl_->winsock = false; impl_->gsi.reset();
    std::lock_guard lock(impl_->mutex); impl_->state.running = false;
    impl_->capture_selection.reset(); impl_->capture_available = false; impl_->capture_received = {};
    impl_->selection_request.reset(); impl_->selection_state = "idle"; impl_->selection_context.clear();
}
Snapshot Server::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
}
