#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace study_fixture {
enum class Scenario { SUCCESS, STOP_BEFORE, QUALITY, HARD_ACTIVE, VALIDATION_FAILURE, HASH_DRIFT };
Scenario scenario = Scenario::SUCCESS;
constexpr LONGLONG kInitialQpc = 1000000000000000;
LONGLONG clock = kInitialQpc, due_at = 0;
unsigned waits = 0, batches = 0, reads_after_wait = 0;
std::uint64_t guard_ns = 300000;
std::filesystem::path directory;

BOOL WINAPI query_counter(LARGE_INTEGER* value) {
    const bool active_jump = scenario == Scenario::HARD_ACTIVE && waits == 1 && reads_after_wait == 1;
    clock += active_jump ? 3501 : 1;
    if (waits) ++reads_after_wait;
    value->QuadPart = clock;
    return TRUE;
}
BOOL WINAPI query_frequency(LARGE_INTEGER* value) { value->QuadPart = 10000000; return TRUE; }
HANDLE WINAPI create_timer(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) {
    const unsigned index = batches++;
    constexpr std::uint64_t guards[]{300000, 325000, 350000};
    guard_ns = index < 30 ? guards[(index / 3 + index % 3) % 3] :
        (scenario == Scenario::QUALITY ? 325000 : 300000);
    // 只借真实 Event 的句柄生命周期；从不创建或武装真实 timer。
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
BOOL WINAPI set_timer(HANDLE, const LARGE_INTEGER* due, LONG period,
                     PTIMERAPCROUTINE callback, LPVOID, BOOL resume) {
    if (due->QuadPart >= 0 || period != 0 || callback || resume)
        throw std::runtime_error("fixture 只允许负相对due的无回调one-shot");
    due_at = clock - due->QuadPart;
    return TRUE;
}
DWORD WINAPI wait_multiple(DWORD count, const HANDLE*, BOOL all, DWORD timeout) {
    if (count != 2 || all || timeout == 0 || timeout > 1000)
        throw std::runtime_error("fixture 发现study Wait合同漂移");
    ++waits;
    reads_after_wait = 0;
    const bool late = (scenario == Scenario::QUALITY && waits == 5) ||
        (scenario == Scenario::VALIDATION_FAILURE && waits == 1264);
    // due_base与Set前QPC相差1tick；再扣除后续Wait/marker两次QPC，得到精确176100ns。
    clock = late ? due_at + static_cast<LONGLONG>(guard_ns / 100) - 1 + 1759 : due_at;
    if (scenario == Scenario::HASH_DRIFT && waits == 1261)
        std::ofstream(directory / "candidate.json", std::ios::binary | std::ios::app) << '\n';
    return WAIT_OBJECT_0 + 1;
}
DWORD WINAPI wait_single(HANDLE, DWORD timeout) {
    if (timeout != 0) throw std::runtime_error("fixture 不允许真实或无限等待");
    return scenario == Scenario::STOP_BEFORE ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
} // namespace study_fixture

// Windows声明先加载，宏只替换本测试TU中生产源码的调用，不修改导入声明或生产二进制。
#define QueryPerformanceCounter study_fixture::query_counter
#define QueryPerformanceFrequency study_fixture::query_frequency
#define CreateWaitableTimerExW study_fixture::create_timer
#define SetWaitableTimer study_fixture::set_timer
#define WaitForMultipleObjects study_fixture::wait_multiple
#define WaitForSingleObject study_fixture::wait_single
#define wmain xen_unused_wmain
#include "../Xen/mouse_effect_probe_composite_seal/main.cpp"
#undef wmain
#undef WaitForSingleObject
#undef WaitForMultipleObjects
#undef SetWaitableTimer
#undef CreateWaitableTimerExW
#undef QueryPerformanceFrequency
#undef QueryPerformanceCounter

namespace {
using nlohmann::json;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
json read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return json::parse(input);
}
void verify_hashes(const std::filesystem::path& path, const json& result) {
    for (const auto* name : {"protocol", "characterization", "candidate", "validation"}) {
        const auto key = std::string(name) + "_file_sha256";
        if (!result.at(key).is_null()) {
            std::string hash, error, content;
            check(read_file(path / (std::string(name) + ".json"), content, error, kMaximumJsonBytes) &&
                  sha256_text(content, hash), "真实文件hash回读失败");
            check(result.at(key) == hash, "实际文件hash与封存引用不一致");
        }
    }
}
void run_case(const std::filesystem::path& root, const char* name, study_fixture::Scenario mode) {
    using S = study_fixture::Scenario;
    study_fixture::scenario = mode;
    study_fixture::clock = study_fixture::kInitialQpc;
    study_fixture::due_at = 0;
    study_fixture::waits = study_fixture::batches = study_fixture::reads_after_wait = 0;
    study_fixture::directory = root / name;
    Options options;
    options.mode = Mode::STUDY_SCHEDULER;
    options.diagnostic_output = study_fixture::directory;
    std::string error;
    const auto code = run_scheduler_study(options, error);
    const auto result = read(options.diagnostic_output / "result.json");
    const auto raw = read(options.diagnostic_output / "characterization.json");
    check(result.at("physical_dispatch_count") == 0 && result.at("formal_preflight_published") == false &&
          result.at("final_plan_published") == false, "编排不得发布正式输出或物理dispatch");
    if (mode != S::HASH_DRIFT) verify_hashes(options.diagnostic_output, result);
    if (mode == S::STOP_BEFORE || mode == S::HARD_ACTIVE) {
        check(code == 3 && result.at("status") == "ABORTED" && raw.at("blocks").size() == 1 &&
              !std::filesystem::exists(options.diagnostic_output / "candidate.json"), "早停必须保存partial且不产生candidate");
        const auto& batch = raw.at("blocks")[0].at("diagnostic");
        check(batch.at("completed_event_count") == 0, "硬停止不得标记采样完整");
        if (mode == S::STOP_BEFORE)
            check(study_fixture::waits == 0 && batch.at("events").empty(), "前置stop必须保留空trace");
        else
            check(study_fixture::waits == 1 && batch.at("events")[0].at("active_wait_ns") == 350100,
                  "active350100ns应在第一次事件立即停止");
        return;
    }
    check(raw.at("status") == "COMPLETE" && raw.at("blocks").size() == 30 &&
          std::filesystem::file_size(options.diagnostic_output / "characterization.json") > 2U * 1024U * 1024U,
          "必须实际发布和回读超过2MiB的完整characterization");
    const auto candidate = read(options.diagnostic_output / "candidate.json");
    const auto validation = read(options.diagnostic_output / "validation.json");
    check(validation.at("blocks")[0].at("diagnostic").at("anchor_qpc") >
          validation.at("candidate_frozen_qpc"), "验证采样必须晚于候选冻结");
    if (mode == S::VALIDATION_FAILURE) {
        check(code == 3 && result.at("status") == "VALIDATION_REJECTED" &&
              candidate.at("selected_guard_ns") == 300000 && study_fixture::waits == 1264 &&
              validation.at("blocks").size() == 1 &&
              validation.at("blocks")[0].at("diagnostic").at("completed_event_count") == 3,
              "验证首失败应保留partial，不重新选择或执行替补");
    } else if (mode == S::HASH_DRIFT) {
        std::string actual_hash, hash_error;
        check(file_sha256(options.diagnostic_output / "candidate.json", actual_hash, hash_error) &&
              result.at("candidate_file_sha256") != actual_hash && study_fixture::waits == 1680 &&
              code == 3 && result.at("status") == "ABORTED", "真实candidate文件hash漂移必须拒绝");
    } else {
        check(code == 0 && result.at("status") == "STUDY_VALIDATED" && study_fixture::waits == 1680 &&
              validation.at("blocks").size() == 10, "固定30+10批应完整运行生产编排");
        check(candidate.at("selected_guard_ns") == (mode == S::QUALITY ? 325000 : 300000), "唯一候选必须符合实际样本");
        if (mode == S::QUALITY) {
            const auto& batch = raw.at("blocks")[0].at("diagnostic");
            check(batch.at("completed_event_count") == 42 && batch.at("events")[4].at("completed") == true &&
                  batch.at("events")[4].at("deadline_lateness_ns") == 176100 &&
                  candidate.at("guard_results")[0].at("quality_failure_count") == 1,
                  "176100ns质量失败必须保留且继续采满，原guard被淘汰");
        }
    }
}
} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() /
            (L"xen-scheduler-study-driver-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        check(std::filesystem::create_directory(root), "fixture新目录创建失败");
        std::wcout << L"确定性编排证据保留目录: " << root.wstring() << L'\n';
        using S = study_fixture::Scenario;
        run_case(root, "success", S::SUCCESS);
        run_case(root, "stop-before", S::STOP_BEFORE);
        run_case(root, "quality-tail", S::QUALITY);
        run_case(root, "hard-active", S::HARD_ACTIVE);
        run_case(root, "validation-failure", S::VALIDATION_FAILURE);
        run_case(root, "hash-drift", S::HASH_DRIFT);
        std::cout << "scheduler study 生产编排确定性测试全部通过；无真实计时测量。\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[失败] " << error.what() << '\n';
        return 1;
    }
}
