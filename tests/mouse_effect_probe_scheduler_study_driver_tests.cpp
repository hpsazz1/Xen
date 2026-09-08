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
enum class Scenario { SUCCESS, STOP_BEFORE, QUALITY, HARD_ACTIVE, VALIDATION_FAILURE, HASH_DRIFT,
                      ACTIVE_EQUAL, WAIT_FAILURE, TIMEOUT };
Scenario scenario = Scenario::SUCCESS;
bool resource_mode = false;
bool formal_mode = false;
std::uint64_t formal_guard_ns = 300000;
constexpr LONGLONG kInitialQpc = 1000000000000000;
LONGLONG clock = kInitialQpc, due_at = 0;
unsigned waits = 0, batches = 0, reads_after_wait = 0;
std::uint64_t guard_ns = 300000;
std::filesystem::path directory;

BOOL WINAPI query_counter(LARGE_INTEGER* value) {
    const bool active_jump = scenario == Scenario::HARD_ACTIVE && waits == 1 && reads_after_wait == 1;
    const bool exact_active = resource_mode && scenario == Scenario::ACTIVE_EQUAL &&
        waits == 1 && reads_after_wait == 1;
    clock += active_jump ? (resource_mode ? 10001 : 3501) : exact_active ? 9999 : 1;
    if (waits) ++reads_after_wait;
    value->QuadPart = clock;
    return TRUE;
}
BOOL WINAPI query_frequency(LARGE_INTEGER* value) { value->QuadPart = 10000000; return TRUE; }
HANDLE WINAPI create_timer(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) {
    const unsigned index = batches++;
    constexpr std::uint64_t guards[]{300000, 325000, 350000};
    guard_ns = formal_mode ? formal_guard_ns : resource_mode ? 1000000 : index < 30 ? guards[(index / 3 + index % 3) % 3] :
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
    if (scenario == Scenario::WAIT_FAILURE) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    if (scenario == Scenario::TIMEOUT) { clock += 300000000; return WAIT_OBJECT_0 + 1; }
    const bool late = (scenario == Scenario::QUALITY && waits == 5) ||
        (scenario == Scenario::VALIDATION_FAILURE && waits == (resource_mode ? 424U : 1264U));
    // due_base与Set前QPC相差1tick；再扣除后续Wait/marker两次QPC，得到精确176100ns。
    clock = late ? due_at + static_cast<LONGLONG>(guard_ns / 100) - 1 + (resource_mode ? 1838 : 1759) : due_at;
    if (scenario == Scenario::HASH_DRIFT && waits == (resource_mode ? 421U : 1261U))
        std::ofstream(directory / "candidate.json", std::ios::binary | std::ios::app) << '\n';
    return WAIT_OBJECT_0 + 1;
}
DWORD WINAPI wait_single(HANDLE, DWORD timeout) {
    if (formal_mode && timeout == INFINITE) {
        const auto result = wait_multiple(2, nullptr, FALSE, 1000);
        return result == WAIT_OBJECT_0 + 1 ? WAIT_OBJECT_0 : result;
    }
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
    study_fixture::resource_mode = false;
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

void run_resource_case(const std::filesystem::path& root, const char* name,
                       study_fixture::Scenario scenario) {
    using S = study_fixture::Scenario;
    study_fixture::resource_mode = true;
    study_fixture::scenario = scenario;
    study_fixture::clock = study_fixture::kInitialQpc;
    study_fixture::due_at = 0;
    study_fixture::waits = study_fixture::batches = study_fixture::reads_after_wait = 0;
    study_fixture::directory = root / name;
    auto path = study_fixture::directory.wstring();
    wchar_t executable[] = L"fixture";
    wchar_t mode[] = L"--study-scheduler-resource";
    wchar_t* arguments[]{executable, mode, path.data()};
    Options options;
    std::string error;
    check(parse_options(3, arguments, options, error), "新 resource CLI 必须路由独立 study mode");
    const auto code = run_scheduler_study(options, error);
    const auto result = read(study_fixture::directory / "result.json");
    const auto raw = read(study_fixture::directory / "characterization.json");
    check(raw.at("protocol").at("schema_version") == 2 &&
          raw.at("protocol").at("policy_id") == "scheduler-resource-1ms-v1" &&
          raw.at("protocol").at("guard_grid_ns") == json::array({1000000}) &&
          raw.at("protocol").at("campaign_max_batches") == 20 &&
          raw.at("protocol").at("campaign_max_active_wait_ns") == 840000000 &&
          result.at("policy_id") == "scheduler-resource-1ms-v1",
          "resource 必须冻结唯一 1ms 与 20批840ms 派生预算身份");
    check(result.at("physical_dispatch_count") == 0 && result.at("formal_preflight_published") == false &&
          result.at("final_plan_published") == false, "resource 不得封存正式产物或触发物理输出");
    if (scenario != S::HASH_DRIFT) verify_hashes(study_fixture::directory, result);
    if (scenario == S::HARD_ACTIVE || scenario == S::STOP_BEFORE ||
        scenario == S::WAIT_FAILURE || scenario == S::TIMEOUT) {
        check(code == 3 && result.at("status") == "ABORTED" &&
              raw.at("blocks").size() == 1 && !std::filesystem::exists(study_fixture::directory / "candidate.json"),
              "resource 硬预算/停止/API/30s超时必须立即中止无候选");
        const auto& batch = raw.at("blocks")[0].at("diagnostic");
        check(batch.at("completed_event_count") == 0, "resource 硬失败不得标记事件完成");
        if (scenario == S::HARD_ACTIVE)
            check(batch.at("events")[0].at("active_wait_ns") == 1000100 && study_fixture::waits == 1,
                  "1000100ns active 必须在首事件立即拒绝");
        return;
    }
    check(raw.at("status") == "COMPLETE" && raw.at("blocks").size() == 10,
          "resource characterization 必须完整10批");
    const auto candidate = read(study_fixture::directory / "candidate.json");
    if (scenario == S::QUALITY) {
        check(code == 3 && result.at("status") == "NO_CANDIDATE" &&
              candidate.at("selected_guard_ns").is_null() && study_fixture::waits == 420 &&
              raw.at("blocks")[0].at("diagnostic").at("events")[4].at("deadline_lateness_ns") == 184000 &&
              raw.at("blocks").back().at("diagnostic").at("completed_event_count") == 42 &&
              !std::filesystem::exists(study_fixture::directory / "validation.json"),
              "184us质量失败必须保留10x42尾部并整候选拒绝，无替补验证");
        return;
    }
    const auto validation = read(study_fixture::directory / "validation.json");
    check(candidate.at("selected_guard_ns") == 1000000 &&
          validation.at("blocks")[0].at("diagnostic").at("anchor_qpc") > validation.at("candidate_frozen_qpc"),
          "resource 验证必须发生在唯一候选真实文件hash冻结之后");
    if (scenario == S::VALIDATION_FAILURE) {
        check(code == 3 && result.at("status") == "VALIDATION_REJECTED" &&
              study_fixture::waits == 424 && validation.at("blocks").size() == 1 &&
              validation.at("blocks")[0].at("diagnostic").at("completed_event_count") == 3,
              "resource 验证首失败保留partial，不能换候选");
    } else if (scenario == S::HASH_DRIFT) {
        check(code == 3 && result.at("status") == "ABORTED" && study_fixture::waits == 840,
              "resource candidate文件漂移必须拒绝");
    } else {
        check(code == 0 && result.at("status") == "STUDY_VALIDATED" &&
              study_fixture::waits == 840 && validation.at("blocks").size() == 10,
              "resource 必须执行且仅执行10+10批");
        if (scenario == S::ACTIVE_EQUAL)
            check(raw.at("blocks")[0].at("diagnostic").at("events")[0].at("active_wait_ns") == 1000000,
                  "active恰好1ms边界应通过，不能采用大于等于拒绝");
    }
}

void test_formal_seed_scheduler_binding(const std::filesystem::path& root) {
    const auto directory = root / "formal-seed-policy";
    check(std::filesystem::create_directory(directory), "formal seed fixture目录创建失败");
    const json legacy{{"timer_mode", "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL"},
        {"active_guard_ns", 300000}, {"max_wake_lateness_ns", 150000},
        {"max_event_interval_width_ns", 100000},
        {"max_active_wait_ns_per_event", 350000}, {"max_active_wait_ns_total", 14700000}};
    const json resource{{"timer_mode", "HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1"},
        {"active_guard_ns", 1000000}, {"max_wake_lateness_ns", 150000},
        {"max_event_interval_width_ns", 100000},
        {"max_active_wait_ns_per_event", 1000000}, {"max_active_wait_ns_total", 42000000}};
    Options options;
    options.sequence = directory / "sequence.json";
    options.plan_seed = directory / "seed.json";
    options.run_uuid = "79ca21ff-6eff-427d-96b3-3a651805222d";
    options.activation_epoch = 1788813319325ULL;
    const auto validate = [&](const json& request, const json& scheduler) {
        const json sequence{{"schema", 7}, {"profile", "physical_b_composite_phase_calibration"},
            {"sequence_sha256", std::string(64, 'a')}, {"request", request}};
        const auto sequence_text = sequence.dump();
        std::ofstream(options.sequence, std::ios::binary) << sequence_text;
        std::string file_hash, semantic_hash;
        check(sha256_text(sequence_text, file_hash), "fixture sequence hash失败");
        json seed{{"status", "AWAITING_AUXILIARY_PREFLIGHT"}, {"run_uuid", options.run_uuid},
            {"activation_epoch", options.activation_epoch}, {"frozen_at_utc_unix_ns", nullptr},
            {"physical_output_capability", false}, {"physical_dispatch_count", 0},
            {"sequence_binding", {{"sequence_file_sha256", file_hash},
                                  {"sequence_semantic_sha256", std::string(64, 'a')}}},
            {"scheduler_policy", scheduler}};
        seed["scheduler_policy"]["preflight_file_sha256"] = nullptr;
        check(sha256_text(seed.dump(), semantic_hash), "fixture seed hash失败");
        seed["plan_seed_semantic_sha256"] = semantic_hash;
        std::ofstream(options.plan_seed, std::ios::binary) << seed.dump();
        json parsed;
        std::string sequence_semantic, error;
        return validate_seed(options, parsed, sequence_semantic, error);
    };
    check(validate(legacy, legacy), "旧合法seed仍须接受");
    check(validate(resource, resource), "新1ms正式seed与sequence完整tuple须接受");
    check(!validate(resource, legacy) && !validate(legacy, resource), "新旧seed/sequence交叉混配必须在timer前拒绝");
    for (const auto* key : {"active_guard_ns", "max_wake_lateness_ns", "max_event_interval_width_ns",
                           "max_active_wait_ns_per_event", "max_active_wait_ns_total"}) {
        auto changed = resource;
        changed[key] = changed[key].get<std::uint64_t>() + 1;
        check(!validate(changed, changed), "自洽但偏离固定tuple的预算必须拒绝");
    }
    auto unknown = resource;
    unknown["timer_mode"] = "UNKNOWN_TIMER";
    check(!validate(unknown, unknown), "未知timer策略不能按大预算兜底");
    auto non_integer = resource;
    non_integer["active_guard_ns"] = 1000000.0;
    check(!validate(non_integer, non_integer), "正式tuple不能把float隐式转换为整数");
}

void test_formal_preflight_consumes_selected_policy() {
    using Policy = mouse_effect_probe::CompositePhaseSchedulerPolicy;
    using S = study_fixture::Scenario;
    const auto run = [&](Policy policy, S scenario) {
        study_fixture::formal_mode = true;
        study_fixture::resource_mode = policy == Policy::ACTIVE_1MS_V1;
        study_fixture::formal_guard_ns = policy == Policy::ACTIVE_1MS_V1 ? 1000000 : 300000;
        study_fixture::scenario = scenario;
        study_fixture::clock = study_fixture::kInitialQpc;
        study_fixture::due_at = 0;
        study_fixture::waits = study_fixture::batches = study_fixture::reads_after_wait = 0;
        Options options;
        options.run_uuid = "formal-fixture";
        json report;
        std::string error;
        const auto passed = run_preflight(options, std::string(64, 'b'), std::string(64, 'a'),
            report, error, nullptr, nullptr, policy);
        study_fixture::formal_mode = false;
        return std::pair{passed, report};
    };
    const auto [legacy_ok, legacy] = run(Policy::LEGACY, S::SUCCESS);
    check(legacy_ok && legacy.at("timer_mode") == "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL" &&
          legacy.at("active_guard_ns") == 300000 && legacy.at("max_active_wait_ns_total") == 14700000,
          "正式旧默认预算与报告必须保留");
    const auto [resource_ok, resource] = run(Policy::ACTIVE_1MS_V1, S::SUCCESS);
    check(resource_ok && resource.at("timer_mode") == "HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1" &&
          resource.at("active_guard_ns") == 1000000 && resource.at("max_active_wait_ns_total") == 42000000 &&
          resource.at("max_active_wait_ns_per_event") == 1000000 && resource.at("events").size() == 42 &&
          resource.at("observed_active_wait_total_ns").get<std::uint64_t>() > 14700000 &&
          resource.at("events")[0].at("active_wait_ns").get<std::uint64_t>() > 350000,
          "正式1ms必须在真实生产等待分支消费新预算并报告完整42项");
    const auto [late_ok, late] = run(Policy::ACTIVE_1MS_V1, S::QUALITY);
    check(!late_ok && late.is_null() && study_fixture::waits == 5,
          "正式路径184us仍须当场拒绝，不保留study质量尾部语义");
    const auto [hard_ok, hard] = run(Policy::ACTIVE_1MS_V1, S::HARD_ACTIVE);
    check(!hard_ok && hard.is_null() && study_fixture::waits == 1,
          "正式1ms单事件超限必须立即拒绝");
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
        run_resource_case(root, "resource-success", S::SUCCESS);
        run_resource_case(root, "resource-quality", S::QUALITY);
        run_resource_case(root, "resource-hard", S::HARD_ACTIVE);
        run_resource_case(root, "resource-equal", S::ACTIVE_EQUAL);
        run_resource_case(root, "resource-stop", S::STOP_BEFORE);
        run_resource_case(root, "resource-wait-error", S::WAIT_FAILURE);
        run_resource_case(root, "resource-timeout", S::TIMEOUT);
        run_resource_case(root, "resource-validation", S::VALIDATION_FAILURE);
        run_resource_case(root, "resource-hash", S::HASH_DRIFT);
        test_formal_seed_scheduler_binding(root);
        test_formal_preflight_consumes_selected_policy();
        std::cout << "scheduler study 生产编排确定性测试全部通过；无真实计时测量。\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[失败] " << error.what() << '\n';
        return 1;
    }
}
