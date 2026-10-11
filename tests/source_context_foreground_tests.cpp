#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace foreground_fixture {
struct State {
    HWND initial_window = reinterpret_cast<HWND>(1);
    HWND final_window = initial_window;
    DWORD initial_process = 101;
    DWORD initial_thread = 201;
    DWORD final_process = initial_process;
    DWORD final_thread = initial_thread;
    bool open_ok = true;
    bool query_ok = true;
    std::wstring path = L"C:\\fixture\\game.exe";
    int foreground_calls = 0;
    int owner_calls = 0;
    int opened = 0;
    int closed = 0;
    bool invalid_call = false;
};
State state;
const HANDLE kProcess = reinterpret_cast<HANDLE>(3);

HWND WINAPI foreground() {
    return state.foreground_calls++ == 0 ? state.initial_window : state.final_window;
}
DWORD WINAPI owner(HWND window, LPDWORD process) {
    state.invalid_call |= window != state.initial_window || !process;
    const bool first = state.owner_calls++ == 0;
    const DWORD thread = first ? state.initial_thread : state.final_thread;
    // Win32 失败时不改输出；生产代码必须同时检查返回值。
    if (thread && process) *process = first ? state.initial_process : state.final_process;
    return thread;
}
HANDLE WINAPI open(DWORD access, BOOL inherit, DWORD process) {
    state.invalid_call |= access != PROCESS_QUERY_LIMITED_INFORMATION || inherit ||
                          process != state.initial_process;
    if (!state.open_ok) return nullptr;
    ++state.opened;
    return kProcess;
}
BOOL WINAPI query(HANDLE process, DWORD flags, LPWSTR path, PDWORD size) {
    state.invalid_call |= process != kProcess || flags || state.closed || !path || !size;
    if (!state.query_ok || !size || state.path.size() >= *size) return FALSE;
    std::copy(state.path.begin(), state.path.end(), path);
    path[state.path.size()] = L'\0';
    *size = static_cast<DWORD>(state.path.size());
    return TRUE;
}
BOOL WINAPI close(HANDLE process) {
    state.invalid_call |= process != kProcess || state.closed != 0;
    ++state.closed;
    return TRUE;
}
} // namespace foreground_fixture

// 编译同一生产实现，只替换前台查询边界；本测试不启动网络/窗口/真实进程查询。
#define GetForegroundWindow foreground_fixture::foreground
#define GetWindowThreadProcessId foreground_fixture::owner
#define OpenProcess foreground_fixture::open
#define QueryFullProcessImageNameW foreground_fixture::query
#define CloseHandle foreground_fixture::close
#include "../Xen/source_context/source_context.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef OpenProcess
#undef QueryFullProcessImageNameW
#undef CloseHandle

namespace {
int failures = 0;
int cases = 0;
template <typename Configure>
void check(const char* name, source_context::detail::Focus expected, Configure configure) {
    using namespace foreground_fixture;
    state = {};
    configure(state);
    const auto result = source_context::foreground_focus("game.exe");
    ++cases;
    if (result != expected) {
        std::cerr << "FAIL " << name << ": focus=" << static_cast<int>(result)
                  << " expected=" << static_cast<int>(expected) << '\n';
        ++failures;
    }
    if (state.invalid_call || state.closed != state.opened) {
        std::cerr << "FAIL " << name << ": invalid call or process handle ownership\n";
        ++failures;
    }
}
} // namespace

int main() {
    using source_context::detail::Focus;
    check("stable foreground", Focus::Foreground, [](auto&) {});
    check("case insensitive basename", Focus::Foreground,
          [](auto& s) { s.path = L"C:/fixture/GAME.EXE"; });
    check("different process", Focus::Background,
          [](auto& s) { s.path = L"C:\\fixture\\other.exe"; });
    check("substring denied", Focus::Background,
          [](auto& s) { s.path = L"C:\\fixture\\other-game.exe"; });
    check("non ASCII denied", Focus::Background,
          [](auto& s) { s.path = L"C:\\fixture\\\u6e38\u620f.exe"; });
    check("oversized basename denied", Focus::Background,
          [](auto& s) { s.path = L"C:\\fixture\\" + std::wstring(64, L'a') + L".exe"; });
    check("no foreground", Focus::Unknown, [](auto& s) { s.initial_window = nullptr; });
    check("initial owner failed", Focus::Unknown, [](auto& s) { s.initial_thread = 0; });
    check("initial PID zero", Focus::Unknown, [](auto& s) { s.initial_process = 0; });
    check("open denied", Focus::Unknown, [](auto& s) { s.open_ok = false; });
    check("image query failed", Focus::Unknown, [](auto& s) { s.query_ok = false; });
    check("foreground changed", Focus::Unknown,
          [](auto& s) { s.final_window = reinterpret_cast<HWND>(2); });
    check("same HWND different PID", Focus::Unknown, [](auto& s) { ++s.final_process; });
    check("same HWND different TID", Focus::Unknown, [](auto& s) { ++s.final_thread; });
    check("final owner failed", Focus::Unknown, [](auto& s) { s.final_thread = 0; });
    check("final PID zero", Focus::Unknown, [](auto& s) { s.final_process = 0; });
    check("healthy observation after failures", Focus::Foreground, [](auto&) {});
    // 将生产查询结果送入真实报文和证据接受器，确认UNKNOWN撤销许可且恢复不抹除断点。
    using namespace source_context::detail;
    using namespace std::chrono_literals;
    Evidence evidence;
    Message response;
    response.response = true;
    response.session = 10;
    response.process_name = "game.exe";
    const std::string token(32, 'a'); // 纯内存测试向量，不是部署鉴权值。
    const auto start = Clock::time_point{} + 1s;
    std::uint64_t revoked_epoch = 0;
    for (int step = 0; step < 3; ++step) {
        foreground_fixture::state = {};
        if (step == 1) ++foreground_fixture::state.final_process;
        response.focus = source_context::foreground_focus("game.exe");
        response.sequence = static_cast<std::uint64_t>(step + 1);
        response.nonce[0] = static_cast<std::uint8_t>(step + 1);
        const auto sent = start + step * 10ms;
        evidence.begin(response.nonce, sent);
        Packet packet{};
        Message parsed;
        const bool accepted = encode(response, token, packet) && decode(packet, token, parsed) &&
            evidence.accept(parsed, "game.exe", sent + 1ms, 100);
        const auto snapshot = evidence.snapshot(sent + 1ms);
        const bool healthy = step != 1;
        const bool expected_epoch = step == 0 ? snapshot.control_safety_epoch == 0 :
            step == 1 ? snapshot.control_safety_epoch > 0 :
                        snapshot.control_safety_epoch == revoked_epoch;
        if (!accepted || snapshot.available != healthy || snapshot.focused != healthy ||
            !expected_epoch || foreground_fixture::state.invalid_call ||
            foreground_fixture::state.closed != foreground_fixture::state.opened) {
            std::cerr << "FAIL query/protocol/evidence step " << step << '\n';
            ++failures;
        }
        if (step == 1) revoked_epoch = snapshot.control_safety_epoch;
    }
    std::cout << cases << " foreground fixture cases, " << failures << " failures\n";
    return failures ? 1 : 0;
}
