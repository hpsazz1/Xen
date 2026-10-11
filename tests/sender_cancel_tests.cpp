#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "capture/capture.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace {
BOOL WINAPI console_control_handler(DWORD) noexcept;
int cancel_at = 0, created = 0, opened = 0, grabbed = 0, closed = 0;
DWORD control_event = CTRL_C_EVENT;
CaptureStatus selected_status = CaptureStatus::FRAME;
class SyntheticCapture final : public ICapture {
public:
    bool open() noexcept override { ++opened; return true; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        ++grabbed;
        // 第一帧以后跨过本测试的1微秒采样间隔，避免反例被pacer偶然跳过。
        if (grabbed > 1) Sleep(1);
        if (grabbed == cancel_at) {
            console_control_handler(control_event);
            if (selected_status != CaptureStatus::FRAME) return selected_status;
        }
        try {
            frame.bgr = cv::Mat(16, 16, CV_8UC3, cv::Scalar(17, 39, 61));
            frame.width = frame.height = 16;
            frame.source_width = frame.source_height = 16;
            frame.encoded_width = frame.encoded_height = 16;
            frame.timing.captured_at = std::chrono::steady_clock::now();
            return CaptureStatus::FRAME;
        } catch (...) { return CaptureStatus::FAILURE; }
    }
    void close() noexcept override { ++closed; }
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return "合成采集器"; }
};
std::unique_ptr<ICapture> synthetic_factory(const CaptureConfig&) noexcept {
    ++created;
    return std::make_unique<SyntheticCapture>();
}
}

// 执行生产run_sender和控制处理器；仅替换采集factory。绝不执行wmain或真实DXGI。
#define create_capture synthetic_factory
#define wmain sender_unused_fixture_main
#include "../Xen/sender/main.cpp"
#undef wmain
#undef create_capture

namespace {
int failures = 0;
void check(bool ok, const std::string& name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}
class LoopbackReceiver {
public:
    LoopbackReceiver() {
        handle_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (handle_ == INVALID_SOCKET ||
            bind(handle_, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
            throw std::runtime_error("本机UDP绑定失败");
        }
        int size = sizeof(address);
        if (getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &size)) {
            throw std::runtime_error("本机UDP端口读取失败");
        }
        url = "udp://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        DWORD timeout = 50;
        if (setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout))) {
            throw std::runtime_error("本机UDP超时设置失败");
        }
    }
    ~LoopbackReceiver() { closesocket(handle_); }
    int drain() {
        std::array<char, 65536> packet{};
        int count = 0;
        while (recv(handle_, packet.data(), static_cast<int>(packet.size()), 0) > 0) ++count;
        check(WSAGetLastError() == WSAETIMEDOUT, "UDP接收以超时结束");
        return count;
    }
    std::string url;
private:
    SOCKET handle_ = INVALID_SOCKET;
};
void run_case(const std::filesystem::path& root, const std::string& name,
              int cancel, CaptureStatus status, bool report_enabled, int expected_frames) {
    LoopbackReceiver receiver;
    cancel_at = cancel;
    selected_status = status;
    created = opened = grabbed = closed = 0;
    stop_requested.store(false); // 每次模拟独立CLI初态，不增加多次wmain合同。
    SenderOptions options;
    options.destination_url = receiver.url;
    options.roi_width = options.roi_height = 16;
    options.fps = 1'000'000;
    options.max_frames = cancel == 1 ? 1 : 2;
    options.max_seconds = 1;
    const auto path = root / (name + ".json");
    if (report_enabled) options.report_path = path.string();
    const int result = run_sender(options);
    const bool capture_failed = cancel && status == CaptureStatus::FAILURE;
    const int expected_exit = capture_failed ? 4 :
        (report_enabled && expected_frames == 0 ? 6 : 0);
    check(result == expected_exit, name + " 退出码");
    check(created == 1 && opened == 1 && closed == 1 &&
          grabbed == (cancel ? cancel : 2), name + " 合成采集生命周期");
    const int packets = receiver.drain();
    check(expected_frames == 0 ? packets == 0 : packets > 0, name + " 实际UDP有无发送");
    if (report_enabled && expected_exit == 0) {
        std::ifstream input(path);
        const auto report = nlohmann::json::parse(input, nullptr, false);
        check(!report.is_discarded(), name + " 报告可解析");
        if (!report.is_discarded()) {
            check(report["stats"]["frames_sent"] == expected_frames &&
                  report["samples"].size() == static_cast<std::size_t>(expected_frames),
                  name + " 不把取消返回帧计为成功样本");
            check(report["stop_reason"] == (cancel ? "signal" : "frame_limit"),
                  name + " 保留实际停止原因");
            check(report["stats"]["datagrams_sent"] == packets, name + " 报告与实际UDP一致");
        }
    } else if (report_enabled) {
        check(!std::filesystem::exists(path), name + " 不发布错误或零帧完整报告");
    }
}
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-sender-cancel-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    LogConfig config;
    config.enable_console = config.enable_file = config.enable_ringbuf = false;
    Log::init(config);
    for (const DWORD event : {CTRL_C_EVENT, CTRL_BREAK_EVENT}) {
        control_event = event;
        const auto prefix = std::to_string(event);
        run_case(root, prefix + "-first", 1, CaptureStatus::FRAME, false, 0);
        run_case(root, prefix + "-second", 2, CaptureStatus::FRAME, true, 1);
        run_case(root, prefix + "-recovery", 0, CaptureStatus::FRAME, true, 2);
    }
    run_case(root, "no-frame", 1, CaptureStatus::NO_FRAME, false, 0);
    run_case(root, "error-with-cancel", 1, CaptureStatus::FAILURE, true, 0);
    run_case(root, "zero-report", 1, CaptureStatus::FRAME, true, 0);
    run_case(root, "final-recovery", 0, CaptureStatus::FRAME, true, 2);
    Log::shutdown();
    WSACleanup();
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file()) std::filesystem::remove(entry.path());
    }
    std::filesystem::remove(root);
    return failures == 0 ? 0 : 1;
}
