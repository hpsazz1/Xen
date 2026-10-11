#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "capture/capture.h"
#include <algorithm>
#include <array>
#include <codecvt>
#include <cstring>
#include <fstream>
#include <locale>
#include <sstream>
#include <string_view>
#include <thread>

namespace {
PHANDLER_ROUTINE test_handler = nullptr;
bool cancel_in_wait = false;
bool cancel_in_grab = false;
bool return_no_frame = false;
bool return_bad_frame = false;
int created = 0, opened = 0, grabbed = 0, closed = 0;
BOOL WINAPI test_register_handler(PHANDLER_ROUTINE handler, BOOL add) {
    test_handler = add ? handler : nullptr;
    return TRUE;
}
BOOL WINAPI test_codepage(UINT) { return TRUE; }
std::chrono::nanoseconds test_wait(std::chrono::steady_clock::time_point) {
    if (cancel_in_wait && test_handler) test_handler(CTRL_C_EVENT);
    return std::chrono::nanoseconds::zero();
}
class SyntheticCapture final : public ICapture {
public:
    explicit SyntheticCapture(CaptureConfig config) : config_(config) {}
    bool open() noexcept override { ++opened; return true; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        ++grabbed;
        if (cancel_in_grab && test_handler) test_handler(CTRL_C_EVENT);
        if (return_no_frame) return CaptureStatus::NO_FRAME;
        try {
            frame.bgr = cv::Mat(config_.roi_height, config_.roi_width + (return_bad_frame ? 1 : 0), CV_8UC3,
                                cv::Scalar(17, 39, 61));
            frame.width = frame.bgr.cols; frame.height = frame.bgr.rows;
            frame.roi_x = config_.roi_x; frame.roi_y = config_.roi_y;
            frame.timing.sequence = grabbed;
            frame.timing.captured_at = std::chrono::steady_clock::now();
            return CaptureStatus::FRAME;
        } catch (...) { return CaptureStatus::FAILURE; }
    }
    void close() noexcept override { ++closed; }
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return "synthetic capture"; }
private:
    CaptureConfig config_;
};
std::unique_ptr<ICapture> test_create_capture(const CaptureConfig& config) noexcept {
    ++created;
    return std::make_unique<SyntheticCapture>(config);
}
}

// 包含真实CLI；只替换采集factory、控制台注册和等待，绝不创建DXGI/NDI采集器。
// 标准头已先包含；sleep_until替换仍调用标准sleep_for(0)，不声明std扩展。
#define create_capture test_create_capture
#define SetConsoleCtrlHandler test_register_handler
#define SetConsoleOutputCP test_codepage
#define sleep_until(deadline) sleep_for(test_wait(deadline))
#define wmain auto_stop_capture_fixture_main
#include "../Xen/auto_stop_capture/main.cpp"
#undef wmain
#undef sleep_until
#undef SetConsoleOutputCP
#undef SetConsoleCtrlHandler
#undef create_capture

namespace {
struct CloseState { int fail_at = 0; int calls = 0; };
class CloseErrorCodecvt final : public std::codecvt<char, char, std::mbstate_t> {
public:
    explicit CloseErrorCodecvt(CloseState& state) : state_(state) {}
protected:
    result do_out(state_type& state, const char* from, const char* end, const char*& next,
                  char* to, char* limit, char*& to_next) const override {
        const auto size = std::min(end - from, limit - to);
        const auto prefix_bytes = std::min(static_cast<std::size_t>(size), prefix_.size() - prefix_size_);
        std::memcpy(prefix_.data() + prefix_size_, from, prefix_bytes);
        prefix_size_ += prefix_bytes;
        std::memcpy(to, from, static_cast<std::size_t>(size));
        next = from + size; to_next = to + size;
        return next == end ? ok : partial;
    }
    result do_in(state_type& state, const char* from, const char* end, const char*& next,
                 char* to, char* limit, char*& to_next) const override {
        return do_out(state, from, end, next, to, limit, to_next);
    }
    result do_unshift(state_type& state, char* to, char*, char*& next) const override {
        next = to;
        // 只选CSV/summary及控制流；PNG编码流不计入报告关闭序号。
        const std::string_view prefix(prefix_.data(), prefix_size_);
        const bool report = prefix.starts_with("attempt,") || prefix.starts_with("status=") ||
            prefix.starts_with("synthetic");
        prefix_size_ = 0;
        if (!report) return ok;
        return ++state_.calls == state_.fail_at ? error : ok;
    }
    bool do_always_noconv() const noexcept override { return false; }
    int do_encoding() const noexcept override { return 1; }
    int do_max_length() const noexcept override { return 1; }
    int do_length(state_type&, const char* from, const char* end, std::size_t max) const override {
        return static_cast<int>(std::min(static_cast<std::size_t>(end - from), max));
    }
private:
    CloseState& state_;
    mutable std::array<char, 9> prefix_{};
    mutable std::size_t prefix_size_ = 0;
};
struct ScopedCloseError {
    explicit ScopedCloseError(CloseState& state)
        : previous(std::locale::global(std::locale(std::locale(), new CloseErrorCodecvt(state)))) {}
    ~ScopedCloseError() { std::locale::global(previous); }
    std::locale previous;
};
int failures = 0;
void check(bool condition, const std::string& name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition) ++failures;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
int run(const std::filesystem::path& path) {
    stopped.store(false); // 模拟独立CLI进程的初态；不扩大生产多次wmain合同。
    created = opened = grabbed = closed = 0;
    std::vector<std::wstring> args{L"fixture", L"--output", path.native(),
        L"--seconds", L"1", L"--fps", L"4", L"--roi-x", L"0", L"--roi-y", L"0",
        L"--roi-width", L"3", L"--roi-height", L"2"};
    std::vector<wchar_t*> pointers;
    for (auto& value : args) pointers.push_back(value.data());
    const int result = auto_stop_capture_fixture_main(static_cast<int>(pointers.size()), pointers.data());
    check(created == 1 && opened == 1 && closed == 1 && test_handler == nullptr,
          "only synthetic capture opened/closed; handler released");
    return result;
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-auto-stop-finalize-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    {
        CloseState state{1};
        ScopedCloseError scope(state);
        std::ofstream output(root / "close-control.txt");
        output << "synthetic";
        output.flush();
        check(output.good() && state.calls == 0, "write/flush succeed before close");
        output.close();
        check(output.fail() && state.calls == 1, "actual filebuf close reports injected failure");
    }
    for (int fail_at : {0, 1, 2}) {
        CloseState state{fail_at};
        const auto path = root / ("close-" + std::to_string(fail_at));
        int result;
        {
            ScopedCloseError scope(state);
            result = run(path);
        }
        check(result == (fail_at ? 1 : 0), "CLI reports selected close " + std::to_string(fail_at));
        check(state.calls >= (fail_at ? fail_at : 2), "selected real filebuf close reached");
        if (fail_at == 1) check(!std::filesystem::exists(path / "summary.txt"),
                                "failed CSV prevents completed summary");
        check(run(root / ("recovery-" + std::to_string(fail_at))) == 0,
              "healthy new directory recovers");
    }
    const auto unicode = root / L"中文-\U0001F680";
    check(run(unicode) == 0, "Unicode output completes synthetic PNG/CSV/summary");
    const auto png = read(unicode / "frame_0.png");
    const std::vector<unsigned char> encoded(png.begin(), png.end());
    const auto decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
    check(!decoded.empty() && cv::norm(decoded,
              cv::Mat(2, 3, CV_8UC3, cv::Scalar(17, 39, 61)), cv::NORM_INF) == 0,
          "Unicode PNG preserves every synthetic pixel");
    cancel_in_wait = true;
    check(run(root / "cancel-wait") == 1 && grabbed == 0,
          "cancel delivered during wait prevents the next grab");
    check(read(root / "cancel-wait" / "summary.txt").find("status=INTERRUPTED") != std::string::npos,
          "cancel with no returned frames remains interrupted, not failed");
    cancel_in_wait = false;
    cancel_in_grab = true;
    const auto interrupted = root / "cancel-after-frame";
    check(run(interrupted) == 1 && grabbed == 1 &&
              read(interrupted / "summary.txt").find("status=INTERRUPTED") != std::string::npos &&
              std::filesystem::is_regular_file(interrupted / "frame_0.png"),
          "cancel after a returned frame preserves it and reports interrupted");
    cancel_in_grab = false;
    return_no_frame = true;
    const auto empty = root / "no-frames";
    check(run(empty) == 1 && read(empty / "summary.txt").find("status=FAILED") != std::string::npos,
          "uncancelled no-frame run remains failed");
    return_no_frame = false;
    cancel_in_grab = return_bad_frame = true;
    const auto bad = root / "error-with-cancel";
    check(run(bad) == 1 && read(bad / "summary.txt").find("status=FAILED") != std::string::npos,
          "real frame validation error remains failed despite cancellation");
    cancel_in_grab = return_bad_frame = false;
    std::cout << "Fixture directory: " << root.string() << '\n';
    return failures ? 1 : 0;
}
