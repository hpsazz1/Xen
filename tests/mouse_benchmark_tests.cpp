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
    explicit FakeKmboxDevice(std::vector<AckMode> responses)
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
        const DWORD receive_timeout_ms = 1500;
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
    options.warmup_pairs = 1;
    options.sample_pairs = 2;
    options.dx_counts = 7;
    options.dy_counts = -3;
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
               options.dx_counts == 4 && options.dy_counts == -2,
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

    const std::vector<std::wstring_view> valid_makcu{
        L"--backend", L"makcu", L"--report", L"makcu.json",
        L"--makcu-port", L"COM8",
        L"--makcu-baud-rate", L"4000000",
        L"--connect-timeout-ms", L"700",
        L"--command-timeout-ms", L"80",
        L"--allow-physical-output",
        L"--confirm-physical-output",
        L"XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT"};
    expect(parse_mouse_benchmark_options(
               valid_makcu, options, error) ==
               MouseBenchmarkParseStatus::READY &&
               options.mouse.backend == MouseBackend::MAKCU &&
               options.mouse.makcu_port == "COM8" &&
               options.mouse.makcu_baud_rate == 4000000 &&
               options.mouse.makcu_connect_timeout_ms == 700 &&
               options.mouse.makcu_command_timeout_ms == 80,
           "完整 MAKCU 设备参数应解析到独立 MouseConfig 字段: " + error);

    auto makcu_with_kmbox = valid_makcu;
    makcu_with_kmbox.insert(
        makcu_with_kmbox.end() - 3,
        {L"--kmbox-ip", L"127.0.0.1"});
    expect(parse_mouse_benchmark_options(
               makcu_with_kmbox, options, error) ==
               MouseBenchmarkParseStatus::INVALID,
           "MAKCU 后端必须拒绝 KMBOX 专属参数");
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
    FakeKmboxDevice device(std::vector<AckMode>(9, AckMode::VALID));
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
    expect(packets.size() == 9,
           "握手、首条补偿、预热和正式样本应产生九个数据报");
    if (packets.size() == 9) {
        expect(packets.front().size() == kHeaderBytes &&
                   read_u32_le(packets.front().data() + 12) ==
                       kConnectCommand,
               "首包必须是 KMBOX 连接握手");
        for (std::size_t index = 1; index < packets.size(); ++index) {
            const int direction = index % 2U == 1U ? 1 : -1;
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
    expect(report.find("\"complete\": true") != std::string::npos &&
               report.find("A1B2C3D4") == std::string::npos &&
               report.find("\"successful_commands\": 8") !=
                   std::string::npos &&
               report.find("\"formal_successful_commands\": 4") !=
                   std::string::npos,
           "报告必须完整、保留成功计数且不泄露 KMBOX UUID");
    expect(!write_mouse_benchmark_report(options, result, error),
           "既有报告目标必须拒绝覆盖");
    std::filesystem::remove(report_path, ignored);
}

void test_failed_command_does_not_publish() {
    FakeKmboxDevice device(
        {AckMode::VALID, AckMode::WRONG_SEQUENCE});
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
               !result.complete && !std::filesystem::exists(report_path),
           "失败样本必须单独计数且不得发布半份报告");
    expect(device.packets().size() == 2,
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
           "MAKCU 合法合成结果应发布 schema 1 报告: " + error);
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
                   std::string::npos,
           "MAKCU 报告必须固化非敏感端点、波特率和超时配置");
    std::filesystem::remove(report_path, ignored);
}

} // namespace

int main() {
    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "Mouse Benchmark 测试无法初始化 Winsock\n";
        return 1;
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
