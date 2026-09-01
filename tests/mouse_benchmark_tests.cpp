#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include "log/log.h"
#include "mouse_benchmark/mouse_benchmark.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kConnectCommand = 0xaf3c2828U;
constexpr std::uint32_t kMouseMoveCommand = 0xaede7345U;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMovePacketBytes = 72;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool close_to(double first, double second) noexcept {
    return std::abs(first - second) <= 1e-9;
}

std::uint32_t read_u32_le(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

void write_u32_le(std::uint8_t* output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

enum class AckMode {
    VALID,
    WRONG_SEQUENCE,
};

class FakeKmboxDevice {
public:
    explicit FakeKmboxDevice(
            std::vector<AckMode> responses,
            DWORD receive_timeout_ms = 1500)
        : responses_(std::move(responses)) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        int address_size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                        &address_size) == SOCKET_ERROR) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        port_ = ntohs(address.sin_port);
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receive_timeout_ms),
                   sizeof(receive_timeout_ms));
        worker_ = std::thread([this] { run(); });
    }

    ~FakeKmboxDevice() {
        finish();
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }

    FakeKmboxDevice(const FakeKmboxDevice&) = delete;
    FakeKmboxDevice& operator=(const FakeKmboxDevice&) = delete;

    bool valid() const noexcept { return socket_ != INVALID_SOCKET; }
    int port() const noexcept { return port_; }

    void finish() {
        if (worker_.joinable()) worker_.join();
    }

    std::vector<std::vector<std::uint8_t>> packets() const {
        std::lock_guard<std::mutex> lock(packets_mutex_);
        return packets_;
    }

private:
    void run() noexcept {
        for (const AckMode mode : responses_) {
            std::array<std::uint8_t, 128> buffer{};
            sockaddr_in client{};
            int client_size = sizeof(client);
            const int received = recvfrom(
                socket_, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&client), &client_size);
            if (received == SOCKET_ERROR) return;
            try {
                std::lock_guard<std::mutex> lock(packets_mutex_);
                packets_.emplace_back(buffer.begin(),
                                      buffer.begin() + received);
            } catch (...) {
                return;
            }
            if (received < static_cast<int>(kHeaderBytes)) continue;
            std::array<std::uint8_t, kHeaderBytes> ack{};
            std::copy_n(buffer.begin(), kHeaderBytes, ack.begin());
            if (mode == AckMode::WRONG_SEQUENCE) {
                write_u32_le(ack.data() + 8,
                             read_u32_le(ack.data() + 8) + 1U);
            }
            sendto(socket_, reinterpret_cast<const char*>(ack.data()),
                   static_cast<int>(ack.size()), 0,
                   reinterpret_cast<const sockaddr*>(&client), client_size);
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
    int port_ = 0;
    std::vector<AckMode> responses_;
    std::thread worker_;
    mutable std::mutex packets_mutex_;
    std::vector<std::vector<std::uint8_t>> packets_;
};

std::filesystem::path unique_report_path(const wchar_t* name) {
    return std::filesystem::temp_directory_path() /
        (std::wstring(L"xen-mouse-benchmark-") + name + L"-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L".json");
}

MouseBenchmarkOptions make_kmbox_options(
        int port, const std::filesystem::path& report_path) {
    MouseBenchmarkOptions options;
    options.backend_explicit = true;
    options.mouse.backend = MouseBackend::KMBOX_NET;
    options.mouse.kmbox_ip = "127.0.0.1";
    options.mouse.kmbox_port = port;
    options.mouse.kmbox_uuid = "A1B2C3D4";
    options.mouse.kmbox_connect_timeout_ms = 80;
    options.mouse.kmbox_command_timeout_ms = 60;
    options.report_path = report_path.string();
    options.run_uuid = "11111111-2222-4333-8444-555555555555";
    options.warmup_pairs = 1;
    options.sample_pairs = 2;
    options.dx_counts = 7;
    options.dy_counts = -3;
    options.owner_scope = MouseOutputOwnerScope::CURRENT_PROCESS_TEST;
    options.peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::LOOPBACK_UDP_FAKE;
    options.allow_physical_output = true;
    options.physical_output_confirmed = true;
    return options;
}

void test_parse_and_safety_gate() {
    const std::vector<std::wstring_view> valid{
        L"--backend", L"win32",
        L"--report", L"mouse.json",
        L"--warmup-pairs", L"0",
        L"--sample-pairs", L"2",
        L"--dx-counts", L"4",
        L"--dy-counts", L"-2",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    MouseBenchmarkOptions options;
    std::string error;
    expect(parse_mouse_benchmark_options(valid, options, error) ==
               MouseBenchmarkParseStatus::READY,
           "双重授权和完整 Win32 参数应解析成功: " + error);
    expect(options.mouse.backend == MouseBackend::WIN32_SEND_INPUT &&
               options.sample_pairs == 2 && options.warmup_pairs == 0 &&
               options.dx_counts == 4 && options.dy_counts == -2 &&
               options.peer_test_boundary ==
                   MouseBenchmarkPeerTestBoundary::LOCAL_OS_API,
           "解析结果必须保留后端、样本和位移");

    std::vector<std::wstring_view> missing_confirmation = valid;
    missing_confirmation.resize(missing_confirmation.size() - 2U);
    expect(parse_mouse_benchmark_options(
               missing_confirmation, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "缺少固定确认令牌必须拒绝物理输出");
    const std::vector<std::wstring_view> invalid_negative{
        L"--backend", L"win32", L"--report", L"mouse.json",
        L"--dx-counts", L"-32768",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    expect(parse_mouse_benchmark_options(
               invalid_negative,
               options, error) == MouseBenchmarkParseStatus::INVALID,
           "无法成对取反的 -32768 counts 必须拒绝");
    const std::vector<std::wstring_view> help{L"--help"};
    expect(parse_mouse_benchmark_options(
               help, options, error) ==
               MouseBenchmarkParseStatus::HELP,
           "--help 不应触发授权校验");
    const std::vector<std::wstring_view> win32_with_kmbox_timeout{
        L"--backend", L"win32", L"--report", L"mouse.json",
        L"--command-timeout-ms", L"10",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    expect(parse_mouse_benchmark_options(
               win32_with_kmbox_timeout, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "Win32 后端必须拒绝 KMBOX 专属超时参数");

    const std::vector<std::wstring_view> makcu_without_boundary{
        L"--backend", L"makcu", L"--report", L"makcu.json",
        L"--makcu-port", L"COM8",
        L"--makcu-baud-rate", L"4000000",
        L"--connect-timeout-ms", L"700",
        L"--command-timeout-ms", L"80",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    expect(parse_mouse_benchmark_options(
               makcu_without_boundary, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "MAKCU CLI 未显式声明 execution boundary 必须拒绝");

    auto valid_makcu = makcu_without_boundary;
    valid_makcu.insert(
        valid_makcu.end() - 3,
        {L"--peer-test-boundary",
         L"configured_external_device_peer"});
    expect(parse_mouse_benchmark_options(
               valid_makcu, options, error) ==
               MouseBenchmarkParseStatus::READY &&
               options.mouse.backend == MouseBackend::MAKCU &&
               options.mouse.makcu_port == "COM8" &&
               options.mouse.makcu_baud_rate == 4000000 &&
               options.mouse.makcu_connect_timeout_ms == 700 &&
               options.mouse.makcu_command_timeout_ms == 80,
           "完整 MAKCU 设备参数应解析到独立 MouseConfig 字段: " + error);

    auto slow_makcu = makcu_without_boundary;
    slow_makcu.insert(
        slow_makcu.end() - 3,
        {L"--peer-test-boundary",
         L"configured_external_device_peer"});
    slow_makcu[7] = L"115200";
    expect(parse_mouse_benchmark_options(
               slow_makcu, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "MAKCU 基准必须拒绝不支持物理 streaming 的 115200");

    auto makcu_with_kmbox = valid_makcu;
    makcu_with_kmbox.insert(
        makcu_with_kmbox.end() - 3,
        {L"--kmbox-ip", L"127.0.0.1"});
    expect(parse_mouse_benchmark_options(
               makcu_with_kmbox, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "MAKCU 后端必须拒绝 KMBOX 专属参数");

    const std::vector<std::wstring_view> kmbox_without_boundary{
        L"--backend", L"kmbox_net", L"--report", L"kmbox.json",
        L"--kmbox-ip", L"192.0.2.1",
        L"--kmbox-port", L"12345",
        L"--kmbox-uuid", L"A1B2C3D4",
        L"--connect-timeout-ms", L"80",
        L"--command-timeout-ms", L"60",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    expect(parse_mouse_benchmark_options(
               kmbox_without_boundary, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "KMBOX CLI 未显式声明 execution boundary 必须拒绝");

    auto external_kmbox = kmbox_without_boundary;
    external_kmbox.insert(
        external_kmbox.end() - 3,
        {L"--peer-test-boundary",
         L"configured_external_device_peer"});
    expect(parse_mouse_benchmark_options(
               external_kmbox, options, error) ==
               MouseBenchmarkParseStatus::READY &&
               options.peer_test_boundary == MouseBenchmarkPeerTestBoundary::
                   CONFIGURED_EXTERNAL_DEVICE_PEER,
           "KMBOX 外部设备必须显式声明 configured external peer: " + error);

    auto loopback_without_boundary = kmbox_without_boundary;
    loopback_without_boundary[5] = L"127.0.0.1";
    expect(parse_mouse_benchmark_options(
               loopback_without_boundary, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "127/8 fake CLI 未显式声明 loopback test boundary 必须拒绝");

    auto loopback_kmbox = loopback_without_boundary;
    loopback_kmbox.insert(
        loopback_kmbox.end() - 3,
        {L"--peer-test-boundary", L"loopback_udp_fake"});
    expect(parse_mouse_benchmark_options(
               loopback_kmbox, options, error) ==
               MouseBenchmarkParseStatus::READY &&
               options.peer_test_boundary ==
                   MouseBenchmarkPeerTestBoundary::LOOPBACK_UDP_FAKE,
           "127/8 fake 必须允许显式 loopback test boundary: " + error);

    auto forged_loopback = make_kmbox_options(
        12345, unique_report_path(L"forged-loopback"));
    forged_loopback.mouse.kmbox_ip = "192.0.2.1";
    expect(!validate_mouse_benchmark_options(forged_loopback, error),
           "loopback fake 声明必须拒绝非 127/8 的外部设备 endpoint");

    auto forged_external = make_kmbox_options(
        12345, unique_report_path(L"forged-external"));
    forged_external.peer_test_boundary = MouseBenchmarkPeerTestBoundary::
        CONFIGURED_EXTERNAL_DEVICE_PEER;
    expect(!validate_mouse_benchmark_options(forged_external, error),
           "127/8 endpoint 必须拒绝 external peer 声明");
}

void test_summary() {
    const std::array<double, 4> values{1.0, 2.0, 3.0, 4.0};
    const auto summary = summarize_mouse_benchmark_timings(values);
    expect(close_to(summary.mean_ms, 2.5) &&
               close_to(summary.p50_ms, 2.5) &&
               close_to(summary.p95_ms, 3.85) &&
               close_to(summary.p99_ms, 3.97) &&
               close_to(summary.max_ms, 4.0),
           "统计必须使用线性插值并输出 mean/P50/P95/P99/max");
}

void test_successful_kmbox_run_and_report() {
    FakeKmboxDevice device(std::vector<AckMode>(10, AckMode::VALID));
    expect(device.valid(), "成功基准假 KMBOX 必须创建成功");
    if (!device.valid()) return;
    const auto report_path = unique_report_path(L"success");
    std::error_code ignored;
    std::filesystem::remove(report_path, ignored);
    auto options = make_kmbox_options(device.port(), report_path);
    MouseBenchmarkResult result;
    std::string error;
    expect(run_mouse_benchmark(options, result, error),
           "本机假 KMBOX 正式路径应成功: " + error);
    device.finish();
    expect(result.complete && result.failed_commands == 0 &&
               result.successful_commands == 8 &&
               result.formal_successful_commands == 4 &&
               result.final_status == MouseStatus::READY &&
               result.samples.size() == 4,
           "正式样本必须是两组四条成功命令且最终状态 READY");
    expect(std::filesystem::is_regular_file(report_path),
           "成功运行必须原子发布 JSON 报告");

    const auto packets = device.packets();
    expect(packets.size() == 10,
           "握手、monitor、首条补偿、预热和正式样本应产生十个数据报");
    if (packets.size() == 10) {
        expect(packets.front().size() == kHeaderBytes &&
                   read_u32_le(packets.front().data() + 12) ==
                       kConnectCommand,
               "首包必须是 KMBOX 连接握手");
        for (std::size_t index = 2; index < packets.size(); ++index) {
            const int direction = (index - 2U) % 2U == 0U ? 1 : -1;
            expect(packets[index].size() == kMovePacketBytes &&
                       read_u32_le(packets[index].data() + 12) ==
                           kMouseMoveCommand &&
                       static_cast<std::int32_t>(read_u32_le(
                           packets[index].data() + 20)) == 7 * direction &&
                       static_cast<std::int32_t>(read_u32_le(
                           packets[index].data() + 24)) == -3 * direction,
                   "所有物理命令必须严格正反成对");
        }
    }

    std::ifstream input(report_path, std::ios::binary);
    const std::string report((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    expect(report.find("\"schema\": 2") != std::string::npos &&
               report.find(
                   "\"run_uuid\": "
                   "\"11111111-2222-4333-8444-555555555555\"") !=
                   std::string::npos &&
               report.find("\"complete\": true") != std::string::npos &&
               report.find("A1B2C3D4") == std::string::npos &&
               report.find(
                   "\"completion_semantic\": "
                   "\"kmbox_matched_udp_protocol_ack\"") !=
                   std::string::npos &&
               report.find(
                   "\"peer_test_boundary\": "
                   "\"loopback_udp_fake\"") != std::string::npos &&
               report.find("\"protocol_ack_observed\": true") !=
                   std::string::npos &&
               report.find("\"physical_effect_observed\": false") !=
                   std::string::npos &&
               report.find(
                   "\"physical_effect_observation_method\": \"none\"") !=
                   std::string::npos &&
               report.find(
                   "\"aggregation_key\": "
                   "\"kmbox_net|kmbox_matched_udp_protocol_ack|"
                   "loopback_udp_fake|none\"") != std::string::npos &&
               report.find("\"successful_commands\": 8") !=
                   std::string::npos &&
               report.find("\"formal_successful_commands\": 4") !=
                   std::string::npos,
           "报告必须版本化自证 KMBOX ACK、loopback fake、无 effect，"
           "并保留成功计数且不泄露 KMBOX UUID");
    expect(!write_mouse_benchmark_report(options, result, error),
           "既有报告目标必须拒绝覆盖");
    std::filesystem::remove(report_path, ignored);
}

void test_failed_command_does_not_publish() {
    FakeKmboxDevice device(
        {AckMode::VALID, AckMode::VALID, AckMode::WRONG_SEQUENCE});
    expect(device.valid(), "失败基准假 KMBOX 必须创建成功");
    if (!device.valid()) return;
    const auto report_path = unique_report_path(L"failure");
    std::error_code ignored;
    std::filesystem::remove(report_path, ignored);
    auto options = make_kmbox_options(device.port(), report_path);
    MouseBenchmarkResult result;
    std::string error;
    expect(!run_mouse_benchmark(options, result, error),
           "首条命令 ACK 错误必须使基准失败");
    device.finish();
    expect(result.failed_commands == 1 &&
               result.final_status == MouseStatus::INVALID_RESPONSE &&
               !result.complete && !result.protocol_ack_observed &&
               !std::filesystem::exists(report_path),
           "首条无合法 move receipt 时不得声称观察到 protocol ACK，"
           "且不得发布半份报告");
    expect(device.packets().size() == 3,
           "首条失败后必须立即停止，不发送补偿、预热或正式命令");
}

void test_makcu_report_contract_without_physical_output() {
    MouseBenchmarkOptions options;
    options.backend_explicit = true;
    options.mouse.backend = MouseBackend::MAKCU;
    options.mouse.makcu_port = "COM8";
    options.mouse.makcu_baud_rate = 4000000;
    options.mouse.makcu_connect_timeout_ms = 700;
    options.mouse.makcu_command_timeout_ms = 80;
    options.run_uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    options.peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::IN_MEMORY_SERIAL_FAKE;
    options.warmup_pairs = 0;
    options.sample_pairs = 1;
    options.dx_counts = 2;
    options.dy_counts = -1;
    options.allow_physical_output = true;
    options.physical_output_confirmed = true;
    const auto report_path = unique_report_path(L"makcu-contract");
    options.report_path = report_path.string();
    std::error_code ignored;
    std::filesystem::remove(report_path, ignored);

    MouseBenchmarkResult result;
    result.complete = true;
    result.run_uuid = options.run_uuid;
    result.completion_semantic = MouseBenchmarkCompletionSemantic::
        MAKCU_MATCHED_SERIAL_DEVICE_STATUS_ACK;
    result.protocol_ack_observed = true;
    result.physical_effect_observed = false;
    result.successful_commands = 4;
    result.formal_successful_commands = 2;
    result.final_status = MouseStatus::READY;
    result.command_latency = {0.2, 0.1, 0.3, 0.4, 0.5};
    result.samples = {
        {1, 1, 1, 2, -1, 0.1},
        {2, 1, -1, -2, 1, 0.2},
    };
    std::string error;
    expect(write_mouse_benchmark_report(options, result, error),
           "MAKCU 合法 in-memory fake 结果应发布 schema 2 报告: " + error);
    std::ifstream input(report_path, std::ios::binary);
    const std::string report((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    expect(report.find("\"backend\": \"makcu\"") != std::string::npos &&
               report.find("\"endpoint\": \"COM8\"") !=
                   std::string::npos &&
               report.find("\"baud_rate\": 4000000") !=
                   std::string::npos &&
               report.find("\"connect_timeout_ms\": 700") !=
                   std::string::npos &&
               report.find("\"command_timeout_ms\": 80") !=
                   std::string::npos &&
               report.find(
                   "\"completion_semantic\": "
                   "\"makcu_matched_serial_device_status_ack\"") !=
                   std::string::npos &&
               report.find(
                   "\"peer_test_boundary\": "
                   "\"in_memory_serial_fake\"") != std::string::npos &&
               report.find("\"physical_effect_observed\": false") !=
                   std::string::npos,
           "MAKCU 报告必须固化配置及 in-memory ACK/effect 来源");
    std::filesystem::remove(report_path, ignored);

    const auto external_boundary_path =
        unique_report_path(L"makcu-external-boundary");
    options.report_path = external_boundary_path.string();
    options.run_uuid = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    options.peer_test_boundary = MouseBenchmarkPeerTestBoundary::
        CONFIGURED_EXTERNAL_DEVICE_PEER;
    result.run_uuid = options.run_uuid;
    expect(write_mouse_benchmark_report(options, result, error),
           "同一 COM endpoint 必须允许由公有 seam 显式声明 external peer: " +
               error);
    std::ifstream external_input(external_boundary_path, std::ios::binary);
    const std::string external_report(
        (std::istreambuf_iterator<char>(external_input)),
        std::istreambuf_iterator<char>());
    expect(external_report.find("\"endpoint\": \"COM8\"") !=
                   std::string::npos &&
               external_report.find(
                   "\"peer_test_boundary\": "
                   "\"configured_external_device_peer\"") !=
                   std::string::npos,
           "writer 必须保留显式边界，不得从相同 endpoint 推断 fake/real");
    external_input.close();
    std::filesystem::remove(external_boundary_path, ignored);

    const auto invalid_semantic_path =
        unique_report_path(L"makcu-invalid-semantic");
    options.report_path = invalid_semantic_path.string();
    options.peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::IN_MEMORY_SERIAL_FAKE;
    result.completion_semantic =
        MouseBenchmarkCompletionSemantic::KMBOX_MATCHED_UDP_PROTOCOL_ACK;
    expect(!write_mouse_benchmark_report(options, result, error) &&
               !std::filesystem::exists(invalid_semantic_path),
           "writer 必须拒绝 backend 与 completion semantic 不匹配");
    result.completion_semantic = MouseBenchmarkCompletionSemantic::
        MAKCU_MATCHED_SERIAL_DEVICE_STATUS_ACK;

    const auto invalid_boundary_path =
        unique_report_path(L"makcu-invalid-boundary");
    options.report_path = invalid_boundary_path.string();
    options.peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::LOOPBACK_UDP_FAKE;
    expect(!write_mouse_benchmark_report(options, result, error) &&
               !std::filesystem::exists(invalid_boundary_path),
           "writer 必须拒绝 MAKCU 与 loopback UDP fake 的非法组合");

    const auto claimed_effect_path =
        unique_report_path(L"makcu-claimed-effect");
    options.report_path = claimed_effect_path.string();
    options.peer_test_boundary =
        MouseBenchmarkPeerTestBoundary::IN_MEMORY_SERIAL_FAKE;
    result.physical_effect_observed = true;
    expect(!write_mouse_benchmark_report(options, result, error) &&
               !std::filesystem::exists(claimed_effect_path),
           "没有独立 observer 的 writer 必须拒绝 physical effect 声明");
}

void test_open_failure_and_first_move_timeout_do_not_observe_ack() {
    std::error_code ignored;

    FakeKmboxDevice open_failure_device({AckMode::WRONG_SEQUENCE});
    expect(open_failure_device.valid(), "打开失败假 KMBOX 必须创建成功");
    if (open_failure_device.valid()) {
        const auto report_path = unique_report_path(L"open-failure-ack");
        std::filesystem::remove(report_path, ignored);
        auto options = make_kmbox_options(
            open_failure_device.port(), report_path);
        MouseBenchmarkResult result;
        std::string error;
        expect(!run_mouse_benchmark(options, result, error),
               "连接阶段无匹配 ACK 必须使基准失败");
        open_failure_device.finish();
        expect(!result.protocol_ack_observed &&
                   !std::filesystem::exists(report_path),
               "打开失败前没有合法 move receipt，protocol ACK 必须保持 false");
    }

    FakeKmboxDevice first_move_timeout_device(
        {AckMode::VALID, AckMode::VALID});
    expect(first_move_timeout_device.valid(),
           "首条超时假 KMBOX 必须创建成功");
    if (first_move_timeout_device.valid()) {
        const auto report_path = unique_report_path(L"first-move-timeout-ack");
        std::filesystem::remove(report_path, ignored);
        auto options = make_kmbox_options(
            first_move_timeout_device.port(), report_path);
        MouseBenchmarkResult result;
        std::string error;
        expect(!run_mouse_benchmark(options, result, error),
               "首条 move ACK 超时必须使基准失败");
        first_move_timeout_device.finish();
        expect(result.failed_commands == 1 &&
                   !result.protocol_ack_observed &&
                   !std::filesystem::exists(report_path),
               "握手/monitor ACK 不能冒充 move receipt，首条超时必须保持 false");
    }
}

void test_run_rejects_mouse_output_owner_conflict_before_device_open() {
    MouseOutputOwnerLease blocker;
    std::string error;
    expect(blocker.acquire(MouseOutputOwnerScope::CURRENT_PROCESS_TEST,
                           "mouse-benchmark-test-blocker", error),
           "benchmark owner 冲突测试必须先持有隔离 lease: " + error);
    const auto report_path = unique_report_path(L"owner-conflict");
    std::error_code ignored;
    std::filesystem::remove(report_path, ignored);
    auto options = make_kmbox_options(12345, report_path);
    MouseBenchmarkResult result;
    expect(!run_mouse_benchmark(options, result, error) &&
               error.find("owner") != std::string::npos &&
               result.successful_commands == 0 &&
               !std::filesystem::exists(report_path),
           "benchmark 必须在打开设备和发送命令前拒绝 Mouse owner 冲突");
    blocker.release();
}

int serve_script_kmbox_fake() {
    // 正式脚本专项只连接本机 UDP peer；六个响应覆盖握手、monitor、首组和正式组。
    // 子进程先发布 ready port，随后允许 wrapper 完成文件/部署快照；单元
    // fake 仍保留 1500 ms 默认预算，只有跨进程专项扩展存活窗口。
    FakeKmboxDevice device(
        std::vector<AckMode>(6, AckMode::VALID), 15000);
    if (!device.valid()) return 2;
    std::cout << "PORT=" << device.port() << std::endl;
    device.finish();
    return device.packets().size() == 6U ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[]) {
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Mouse Benchmark 测试无法初始化 Winsock\n";
        return 1;
    }
    if (argc == 2 &&
        std::string_view(argv[1]) == "--serve-script-kmbox-fake") {
        const int result = serve_script_kmbox_fake();
        WSACleanup();
        return result;
    }
    LogConfig log_config;
    log_config.enable_console = false;
    log_config.enable_file = false;
    log_config.enable_ringbuf = false;
    Log::init(log_config);

    test_parse_and_safety_gate();
    test_summary();
    test_successful_kmbox_run_and_report();
    test_failed_command_does_not_publish();
    test_open_failure_and_first_move_timeout_do_not_observe_ack();
    test_run_rejects_mouse_output_owner_conflict_before_device_open();
    test_makcu_report_contract_without_physical_output();

    Log::shutdown();
    WSACleanup();
    if (failures != 0) {
        std::cerr << "Mouse Benchmark 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Mouse Benchmark 测试全部通过。\n";
    return 0;
}
