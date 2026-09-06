#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>
#include "scheduler_study_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

constexpr std::uint64_t kActiveGuardNs = 300'000;
constexpr std::uint64_t kMaxWakeLatenessNs = 150'000;
constexpr std::uint64_t kMaxEventIntervalWidthNs = 100'000;
constexpr std::uint64_t kMaxActiveWaitPerEventNs = 350'000;
constexpr std::uint64_t kMaxActiveWaitTotalNs = 42U * 350'000U;
constexpr std::uint64_t kPreflightIntervalNs = 5'000'000;
constexpr std::size_t kPreflightEventCount = 42;
constexpr std::uintmax_t kMaximumJsonBytes = 16U * 1024U * 1024U;

enum class Mode {
    PREFLIGHT_AND_SEAL,
    DIAGNOSE_SCHEDULER,
    STUDY_SCHEDULER,
    REPORT_SEMANTIC_SHA256,
    VERIFY_REPORT,
};

struct Options {
    Mode mode = Mode::PREFLIGHT_AND_SEAL;
    std::filesystem::path report;
    std::filesystem::path diagnostic_output;
    std::filesystem::path plan_seed;
    std::filesystem::path sequence;
    std::filesystem::path preflight_output;
    std::filesystem::path plan_output;
    std::string run_uuid;
    std::uint64_t activation_epoch = 0;
};

// 固定容量标量记录只供独立诊断使用，避免在计时循环中构造 JSON。
struct TraceValue {
    std::int64_t value;
    bool valid;
};

struct Win32Failure {
    const char* api;
    DWORD code;
};

struct SchedulerEventTrace {
    TraceValue deadline;
    TraceValue coarse_target;
    TraceValue due_base;
    TraceValue relative_due;
    TraceValue set_before;
    TraceValue set_after;
    TraceValue set_result;
    TraceValue wait_result;
    TraceValue wait_return;
    TraceValue active_last;
    TraceValue marker_before;
    TraceValue marker_after;
    TraceValue active_reads;
    TraceValue lateness;
    TraceValue marker_width;
    TraceValue active_wait;
    std::uint64_t active_total_before;
    const char* last_stage;
    bool completed;
};

struct SchedulerTrace {
    std::array<SchedulerEventTrace, kPreflightEventCount> events;
    std::size_t reached_count;
    std::size_t completed_count;
    std::int64_t frequency;
    std::int64_t anchor;
    const char* stage;
    Win32Failure win32_failure;
};

// 只供独立 study 使用；正式封存不接收主机候选参数。
struct StudyRun {
    std::uint64_t guard_ns;
    bool characterize;
    HANDLE stop_event;
    std::int64_t deadline_qpc;
};

std::atomic<HANDLE> study_stop_event{nullptr};

BOOL WINAPI stop_study(DWORD signal) {
    const auto stop = study_stop_event.load();
    if ((signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) &&
        stop) {
        SetEvent(stop);
        return TRUE;
    }
    return FALSE;
}

static_assert(std::is_trivial_v<SchedulerTrace> &&
              std::is_standard_layout_v<SchedulerTrace>);

void record_value(TraceValue& field, std::int64_t value) noexcept {
    field.value = value;
    field.valid = true;
}

void record_win32_failure(Win32Failure* failure, const char* api) noexcept {
    // 必须紧接失败 API，先于其他 Win32 调用或诊断处理。
    if (failure) {
        const auto code = GetLastError();
        failure->code = code;
        failure->api = api;
    }
}

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
private:
    HANDLE value_ = nullptr;
};

bool valid_sha256(std::string_view value) noexcept {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
}

bool valid_uuid(std::string_view value) noexcept {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool sha256_bytes(std::span<const std::uint8_t> bytes,
                  std::string& output) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        DWORD object_size = 0;
        DWORD digest_size = 0;
        DWORD copied = 0;
        const auto ok = [](NTSTATUS status) { return status >= 0; };
        if (bytes.size() > std::numeric_limits<ULONG>::max() ||
            !ok(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !ok(BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                &copied, 0)) || copied != sizeof(object_size) ||
            !ok(BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                &copied, 0)) || copied != sizeof(digest_size) ||
            digest_size != 32) {
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        std::vector<std::uint8_t> object(object_size);
        std::array<std::uint8_t, 32> digest{};
        if (!ok(BCryptCreateHash(
                algorithm, &hash, object.data(), object_size,
                nullptr, 0, 0)) ||
            (!bytes.empty() && !ok(BCryptHashData(
                hash, const_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), 0))) ||
            !ok(BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        constexpr char hexadecimal[] = "0123456789abcdef";
        output.clear();
        output.reserve(64);
        for (const auto value : digest) {
            output.push_back(hexadecimal[value >> 4U]);
            output.push_back(hexadecimal[value & 0x0fU]);
        }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return true;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
}

bool sha256_text(std::string_view text, std::string& output) noexcept {
    return sha256_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
        output);
}

bool read_file(const std::filesystem::path& path, std::string& content,
               std::string& error,
               std::uintmax_t maximum_bytes = 2'097'152U) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error) ||
        filesystem_error ||
        std::filesystem::file_size(path, filesystem_error) > maximum_bytes ||
        filesystem_error) {
        error = "输入不是受限普通文件";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    content.assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
    if (content.empty() || (!input.good() && !input.eof())) {
        error = "输入文件为空或读取失败";
        return false;
    }
    return true;
}

bool file_sha256(const std::filesystem::path& path, std::string& output,
                 std::string& error) {
    std::string content;
    if (!read_file(path, content, error)) return false;
    if (!sha256_text(content, output)) {
        error = "文件 SHA-256 失败";
        return false;
    }
    return true;
}

bool parse_unsigned(std::wstring_view text, std::uint64_t& output) {
    std::string narrow;
    narrow.reserve(text.size());
    for (const auto character : text) {
        if (character < L'0' || character > L'9') return false;
        narrow.push_back(static_cast<char>(character));
    }
    const auto [end, status] = std::from_chars(
        narrow.data(), narrow.data() + narrow.size(), output);
    return status == std::errc{} && end == narrow.data() + narrow.size();
}

bool parse_options(int argc, wchar_t* argv[], Options& options,
                   std::string& error) {
    if (argc == 3 &&
        (std::wstring_view(argv[1]) == L"--diagnose-scheduler" ||
         std::wstring_view(argv[1]) == L"--study-scheduler")) {
        options.mode = std::wstring_view(argv[1]) == L"--study-scheduler"
            ? Mode::STUDY_SCHEDULER : Mode::DIAGNOSE_SCHEDULER;
        options.diagnostic_output = std::filesystem::path(argv[2]);
        std::error_code filesystem_error;
        if (options.diagnostic_output.empty() ||
            !options.diagnostic_output.is_absolute() ||
            std::filesystem::exists(options.diagnostic_output,
                                    filesystem_error) || filesystem_error) {
            error = "scheduler 诊断输出必须是绝对新路径，拒绝覆盖";
            return false;
        }
        return true;
    }
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--diagnose-scheduler" ||
            std::wstring_view(argv[index]) == L"--study-scheduler") {
            error = "scheduler 诊断模式不接受其他参数";
            return false;
        }
    }
    if (argc == 3 &&
        (std::wstring_view(argv[1]) == L"--report-semantic-sha256" ||
         std::wstring_view(argv[1]) == L"--verify-report")) {
        options.mode = std::wstring_view(argv[1]) ==
                L"--report-semantic-sha256"
            ? Mode::REPORT_SEMANTIC_SHA256 : Mode::VERIFY_REPORT;
        options.report = std::filesystem::path(argv[2]);
        if (options.report.empty() || !options.report.is_absolute()) {
            error = "report 路径必须是绝对路径";
            return false;
        }
        return true;
    }
    bool seen_seed = false, seen_sequence = false, seen_preflight = false;
    bool seen_plan = false, seen_uuid = false, seen_epoch = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h") {
            return false;
        }
        if (index + 1 >= argc) {
            error = "参数缺少值";
            return false;
        }
        const std::wstring_view value(argv[++index]);
        auto duplicate = [&](bool& seen) {
            if (seen) {
                error = "参数重复";
                return true;
            }
            seen = true;
            return false;
        };
        if (argument == L"--plan-seed") {
            if (duplicate(seen_seed)) return false;
            options.plan_seed = value;
        } else if (argument == L"--sequence") {
            if (duplicate(seen_sequence)) return false;
            options.sequence = value;
        } else if (argument == L"--preflight-output") {
            if (duplicate(seen_preflight)) return false;
            options.preflight_output = value;
        } else if (argument == L"--plan-output") {
            if (duplicate(seen_plan)) return false;
            options.plan_output = value;
        } else if (argument == L"--run-uuid") {
            if (duplicate(seen_uuid)) return false;
            const int required = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (required <= 0) break;
            options.run_uuid.resize(required);
            if (WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                    static_cast<int>(value.size()), options.run_uuid.data(),
                    required, nullptr, nullptr) != required) break;
        } else if (argument == L"--activation-epoch") {
            if (duplicate(seen_epoch)) return false;
            if (!parse_unsigned(value, options.activation_epoch)) break;
        } else {
            error = "未知参数";
            return false;
        }
    }
    if (!seen_seed || !seen_sequence || !seen_preflight || !seen_plan ||
        !seen_uuid || !seen_epoch || options.activation_epoch == 0 ||
        !valid_uuid(options.run_uuid) || options.plan_seed.empty() ||
        !options.plan_seed.is_absolute() || options.sequence.empty() ||
        !options.sequence.is_absolute() || options.preflight_output.empty() ||
        !options.preflight_output.is_absolute() || options.plan_output.empty() ||
        !options.plan_output.is_absolute()) {
        error = "缺少必填参数、重复参数或路径/Run identity 非法";
        return false;
    }
    if (std::filesystem::exists(options.preflight_output) ||
        std::filesystem::exists(options.plan_output)) {
        error = "输出已存在，拒绝覆盖";
        return false;
    }
    return true;
}

bool calculate_report_semantic(
        const std::filesystem::path& path, std::string& claimed,
        std::string& actual, std::string& error) {
    std::string content;
    if (!read_file(path, content, error, kMaximumJsonBytes)) return false;
    try {
        auto document = nlohmann::ordered_json::parse(content);
        if (!document.is_object()) {
            error = "command report 必须是 JSON object";
            return false;
        }
        claimed.clear();
        const auto iterator = document.find("report_sha256");
        if (iterator != document.end()) {
            if (!iterator->is_string()) {
                error = "command report 的 report_sha256 类型非法";
                return false;
            }
            claimed = iterator->get<std::string>();
            document.erase(iterator);
        }
        if (!sha256_text(document.dump(), actual)) {
            error = "command report semantic SHA-256 失败";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("command report JSON 无效: ") + exception.what();
        return false;
    }
}

std::uint64_t ticks_to_ns_ceil(std::int64_t ticks,
                               std::int64_t frequency) {
    if (ticks <= 0 || frequency <= 0) return 0;
    const auto value = static_cast<std::uint64_t>(ticks);
    const auto divisor = static_cast<std::uint64_t>(frequency);
    const auto whole = value / divisor;
    const auto remainder = value % divisor;
    return whole * 1'000'000'000ULL +
        (remainder * 1'000'000'000ULL) / divisor +
        ((remainder * 1'000'000'000ULL) % divisor == 0 ? 0U : 1U);
}

bool ns_to_ticks(std::uint64_t nanoseconds, std::int64_t frequency,
                 std::int64_t& ticks) {
    if (frequency <= 0) return false;
    const auto product = (nanoseconds % 1'000'000'000ULL) *
        static_cast<std::uint64_t>(frequency);
    const auto whole = (nanoseconds / 1'000'000'000ULL) *
        static_cast<std::uint64_t>(frequency);
    const auto partial = product / 1'000'000'000ULL +
        (product % 1'000'000'000ULL == 0 ? 0U : 1U);
    if (whole > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) - partial) return false;
    ticks = static_cast<std::int64_t>(whole + partial);
    return true;
}

bool relative_due(std::int64_t ticks, std::int64_t frequency,
                  LARGE_INTEGER& due) {
    const auto nanoseconds = ticks_to_ns_ceil(ticks, frequency);
    const auto units = nanoseconds / 100U +
        (nanoseconds % 100U == 0 ? 0U : 1U);
    if (ticks <= 0 || units == 0 ||
        units > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) return false;
    due.QuadPart = -static_cast<std::int64_t>(units);
    return true;
}

bool query_qpc(std::int64_t& value, Win32Failure* failure = nullptr) {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        record_win32_failure(failure, "QueryPerformanceCounter");
        return false;
    }
    if (counter.QuadPart <= 0) return false;
    value = counter.QuadPart;
    return true;
}

bool write_json_atomic(const std::filesystem::path& path,
                       const nlohmann::json& value,
                       std::string& error) {
    auto pending = path;
    pending += L".pending-" + std::to_wstring(GetCurrentProcessId());
    try {
        if (std::filesystem::exists(path) || std::filesystem::exists(pending)) {
            error = "JSON 输出已存在";
            return false;
        }
        std::error_code directory_error;
        std::filesystem::create_directories(
            path.parent_path(), directory_error);
        if (directory_error) {
            error = "JSON 输出目录创建失败";
            return false;
        }
        const auto content = value.dump(2) + "\n";
        std::ofstream output(pending, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(
            content.size()));
        output.flush();
        const bool written = output.good();
        output.close();
        if (!written || !MoveFileExW(
                pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = "JSON 原子发布失败";
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("JSON 发布异常: ") + exception.what();
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        return false;
    }
}

bool validate_seed(const Options& options, nlohmann::json& seed,
                   std::string& sequence_semantic, std::string& error) {
    std::string seed_content;
    std::string sequence_content;
    if (!read_file(options.plan_seed, seed_content, error) ||
        !read_file(options.sequence, sequence_content, error)) return false;
    try {
        seed = nlohmann::json::parse(seed_content);
        const auto sequence = nlohmann::json::parse(sequence_content);
        const auto claimed = seed.value(
            "plan_seed_semantic_sha256", std::string{});
        auto semantic_input = seed;
        semantic_input.erase("plan_seed_semantic_sha256");
        std::string actual;
        std::string sequence_file_hash;
        if (!valid_sha256(claimed) ||
            !sha256_text(semantic_input.dump(), actual) || actual != claimed ||
            seed.value("status", std::string{}) !=
                "AWAITING_AUXILIARY_PREFLIGHT" ||
            seed.value("run_uuid", std::string{}) != options.run_uuid ||
            seed.value("activation_epoch", std::uint64_t{0}) !=
                options.activation_epoch ||
            !seed["frozen_at_utc_unix_ns"].is_null() ||
            seed.value("physical_output_capability", true) ||
            seed.value("physical_dispatch_count", -1) != 0 ||
            sequence.value("schema", 0) != 7 ||
            sequence.value("profile", std::string{}) !=
                "physical_b_composite_phase_calibration" ||
            !file_sha256(options.sequence, sequence_file_hash, error) ||
            seed["sequence_binding"].value(
                "sequence_file_sha256", std::string{}) !=
                sequence_file_hash ||
            seed["sequence_binding"].value(
                "sequence_semantic_sha256", std::string{}) !=
                sequence.value("sequence_sha256", std::string{}) ||
            !seed["scheduler_policy"][
                "preflight_file_sha256"].is_null() ||
            seed["scheduler_policy"].value(
                "timer_mode", std::string{}) !=
                "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL" ||
            seed["scheduler_policy"].value(
                "active_guard_ns", std::uint64_t{0}) != kActiveGuardNs ||
            seed["scheduler_policy"].value(
                "max_wake_lateness_ns", std::uint64_t{0}) !=
                kMaxWakeLatenessNs ||
            seed["scheduler_policy"].value(
                "max_event_interval_width_ns", std::uint64_t{0}) !=
                kMaxEventIntervalWidthNs ||
            seed["scheduler_policy"].value(
                "max_active_wait_ns_per_event", std::uint64_t{0}) !=
                kMaxActiveWaitPerEventNs ||
            seed["scheduler_policy"].value(
                "max_active_wait_ns_total", std::uint64_t{0}) !=
                kMaxActiveWaitTotalNs) {
            if (error.empty()) error = "plan seed/sequence/scheduler 合同漂移";
            return false;
        }
        sequence_semantic = sequence["sequence_sha256"].get<std::string>();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("plan seed/sequence JSON 无效: ") +
            exception.what();
        return false;
    }
}

bool run_preflight(const Options& options, std::string_view seed_semantic,
                   std::string_view sequence_semantic,
                   nlohmann::json& document, std::string& error,
                   SchedulerTrace* trace = nullptr,
                   const StudyRun* study = nullptr) {
    auto* failure = trace ? &trace->win32_failure : nullptr;
    if (trace) trace->stage = "QPC_FREQUENCY";
    LARGE_INTEGER frequency_value{};
    if (!QueryPerformanceFrequency(&frequency_value)) {
        record_win32_failure(failure, "QueryPerformanceFrequency");
        error = "QPC frequency 不可用";
        return false;
    }
    if (frequency_value.QuadPart <= 0) {
        error = "QPC frequency 不可用";
        return false;
    }
    const auto frequency = frequency_value.QuadPart;
    if (trace) {
        trace->frequency = frequency;
        trace->stage = "CREATE_TIMER";
    }
    Handle timer(CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS));
    if (!timer) {
        record_win32_failure(failure, "CreateWaitableTimerExW");
        error = "高分辨率 one-shot timer 不可用";
        return false;
    }
    std::int64_t interval_ticks = 0;
    std::int64_t guard_ticks = 0;
    std::int64_t anchor = 0;
    if (trace) trace->stage = "QPC_SETUP";
    if (!ns_to_ticks(kPreflightIntervalNs, frequency, interval_ticks) ||
        !ns_to_ticks(study ? study->guard_ns : kActiveGuardNs,
                     frequency, guard_ticks) ||
        !query_qpc(anchor, failure)) {
        error = "preflight QPC 单位转换失败";
        return false;
    }
    if (trace) trace->anchor = anchor;
    nlohmann::json events = trace ? nlohmann::json{} : nlohmann::json::array();
    std::uint64_t total_active_ns = 0;
    std::uint64_t maximum_lateness_ns = 0;
    std::uint64_t maximum_marker_width_ns = 0;
    for (std::size_t index = 0; index < kPreflightEventCount; ++index) {
        auto* event = trace ? &trace->events[index] : nullptr;
        if (event) {
            trace->reached_count = index + 1U;
            trace->stage = event->last_stage = "DEADLINE";
            event->active_total_before = total_active_ns;
        }
        if (interval_ticks <= 0 ||
            static_cast<std::uint64_t>(index + 1U) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max() - anchor) /
                static_cast<std::uint64_t>(interval_ticks)) {
            error = "preflight absolute deadline 溢出";
            return false;
        }
        const auto deadline = anchor +
            static_cast<std::int64_t>(index + 1U) * interval_ticks;
        const auto active_target = deadline - guard_ticks;
        if (event) {
            record_value(event->deadline, deadline);
            record_value(event->coarse_target, active_target);
            trace->stage = event->last_stage = "DUE_BASE_QPC";
        }
        std::int64_t now = 0;
        const auto due_base_valid = query_qpc(now, failure);
        if (event && due_base_valid) record_value(event->due_base, now);
        if (study && due_base_valid && (now < anchor ||
                (index > 0 && now < trace->events[index - 1].marker_after.value))) {
            trace->stage = event->last_stage = "STUDY_QPC_ORDER";
            error = "study due-base QPC 倒退";
            return false;
        }
        if (study && (WaitForSingleObject(study->stop_event, 0) != WAIT_TIMEOUT ||
                      now >= study->deadline_qpc)) {
            trace->stage = event->last_stage = "STUDY_STOP_OR_TIMEOUT";
            error = "study 停止或整体经过时间超限";
            return false;
        }
        if (!due_base_valid || now >= active_target) {
            if (event && due_base_valid) {
                trace->stage = event->last_stage = "COARSE_DEADLINE";
            }
            error = "preflight coarse deadline 已迟到";
            return false;
        }
        LARGE_INTEGER due{};
        if (event) trace->stage = event->last_stage = "RELATIVE_DUE";
        if (!relative_due(active_target - now, frequency, due)) {
            error = "preflight one-shot wait 失败";
            return false;
        }
        if (event) {
            record_value(event->relative_due, due.QuadPart);
            trace->stage = event->last_stage = "SET_TIMER_BEFORE_QPC";
            std::int64_t observed = 0;
            if (!query_qpc(observed, failure)) {
                error = "诊断 SetWaitableTimer 前 QPC 失败";
                return false;
            }
            record_value(event->set_before, observed);
            if (study && observed < now) {
                error = "study Set 前 QPC 倒退";
                return false;
            }
            trace->stage = event->last_stage = "SET_TIMER";
            const auto set_result = SetWaitableTimer(
                timer.get(), &due, 0, nullptr, nullptr, FALSE);
            if (!set_result) record_win32_failure(failure, "SetWaitableTimer");
            record_value(event->set_result, set_result ? 1 : 0);
            if (!set_result) {
                error = "preflight one-shot wait 失败";
                return false;
            }
            trace->stage = event->last_stage = "SET_TIMER_AFTER_QPC";
            if (!query_qpc(observed, failure)) {
                error = "诊断 SetWaitableTimer 后 QPC 失败";
                return false;
            }
            record_value(event->set_after, observed);
            if (study && observed < event->set_before.value) {
                error = "study Set 后 QPC 倒退";
                return false;
            }
            trace->stage = event->last_stage = "WAIT";
            DWORD wait_result = 0;
            if (study) {
                const HANDLE handles[]{study->stop_event, timer.get()};
                const auto remaining_ns = ticks_to_ns_ceil(
                    study->deadline_qpc - observed, frequency);
                const auto remaining_ms = std::min<std::uint64_t>(1000,
                    remaining_ns / 1'000'000U + (remaining_ns % 1'000'000U != 0));
                wait_result = WaitForMultipleObjects(2, handles, FALSE,
                    static_cast<DWORD>(remaining_ms));
            } else {
                wait_result = WaitForSingleObject(timer.get(), INFINITE);
            }
            if (wait_result == WAIT_FAILED) {
                record_win32_failure(failure, study ? "WaitForMultipleObjects" : "WaitForSingleObject");
            }
            record_value(event->wait_result, wait_result);
            if (wait_result != (study ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0)) {
                error = "preflight one-shot wait 失败";
                return false;
            }
            trace->stage = event->last_stage = "WAIT_RETURN_QPC";
            if (!query_qpc(now, failure)) {
                error = "preflight one-shot wait 失败";
                return false;
            }
            record_value(event->wait_return, now);
            if (study && (now >= study->deadline_qpc || now < event->set_after.value)) {
                trace->stage = event->last_stage = now < event->set_after.value
                    ? "STUDY_QPC_ORDER" : "STUDY_TIMEOUT";
                error = "study Wait 返回 QPC 倒退或整体经过时间超限";
                return false;
            }
        } else if (
            !SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer.get(), INFINITE) != WAIT_OBJECT_0 ||
            !query_qpc(now)) {
            error = "preflight one-shot wait 失败";
            return false;
        }
        const auto active_enter = now;
        std::uint64_t reads = 0;
        if (event) {
            trace->stage = event->last_stage = "ACTIVE_WAIT";
            record_value(event->active_last, now);
            record_value(event->active_reads, 0);
        }
        while (now < deadline) {
            if (study && WaitForSingleObject(study->stop_event, 0) != WAIT_TIMEOUT) {
                record_value(event->active_last, now);
                record_value(event->active_reads, static_cast<std::int64_t>(reads));
                record_value(event->active_wait, static_cast<std::int64_t>(
                    ticks_to_ns_ceil(now - active_enter, frequency)));
                trace->stage = event->last_stage = "STUDY_STOP";
                error = "study 已停止";
                return false;
            }
            ++reads;
            const auto previous = now;
            if (!query_qpc(now, failure)) {
                if (event) {
                    record_value(event->active_last, now);
                    record_value(event->active_reads,
                                 static_cast<std::int64_t>(reads));
                    trace->stage = event->last_stage = "ACTIVE_WAIT_QPC";
                }
                error = "preflight active wait QPC 失败";
                return false;
            }
            const auto active_ns = ticks_to_ns_ceil(
                now - active_enter, frequency);
            if (study && now < previous) {
                record_value(event->active_last, now);
                record_value(event->active_reads, static_cast<std::int64_t>(reads));
                trace->stage = event->last_stage = "STUDY_QPC_ORDER";
                error = "study active QPC 倒退";
                return false;
            }
            if (active_ns > kMaxActiveWaitPerEventNs ||
                (study && (total_active_ns > kMaxActiveWaitTotalNs - active_ns ||
                           now >= study->deadline_qpc))) {
                if (event) {
                    record_value(event->active_last, now);
                    record_value(event->active_reads,
                                 static_cast<std::int64_t>(reads));
                    record_value(event->active_wait,
                                 static_cast<std::int64_t>(active_ns));
                    trace->stage = event->last_stage = "ACTIVE_WAIT_BUDGET";
                }
                error = "preflight 单事件 active wait 超限";
                return false;
            }
        }
        std::int64_t marker_before = 0;
        std::int64_t marker_after = 0;
        if (event) {
            record_value(event->active_last, now);
            record_value(event->active_reads, static_cast<std::int64_t>(reads));
            trace->stage = event->last_stage = "MARKER_BEFORE_QPC";
            if (!query_qpc(marker_before, failure)) {
                error = "preflight marker bracket 失败";
                return false;
            }
            record_value(event->marker_before, marker_before);
            if (study && marker_before < now) {
                error = "study marker 前 QPC 倒退";
                return false;
            }
            trace->stage = event->last_stage = "MARKER_AFTER_QPC";
            if (!query_qpc(marker_after, failure)) {
                error = "preflight marker bracket 失败";
                return false;
            }
            record_value(event->marker_after, marker_after);
            if (marker_after < marker_before) {
                trace->stage = event->last_stage = "MARKER_ORDER";
                error = "preflight marker bracket 失败";
                return false;
            }
        } else if (!query_qpc(marker_before) || !query_qpc(marker_after) ||
            marker_after < marker_before) {
            error = "preflight marker bracket 失败";
            return false;
        }
        const auto lateness = marker_before > deadline
            ? ticks_to_ns_ceil(marker_before - deadline, frequency) : 0U;
        const auto marker_width = ticks_to_ns_ceil(
            marker_after - marker_before, frequency);
        const auto active_ns = marker_before > active_enter
            ? ticks_to_ns_ceil(marker_before - active_enter, frequency) : 0U;
        if (event) {
            record_value(event->lateness, static_cast<std::int64_t>(lateness));
            record_value(event->marker_width,
                         static_cast<std::int64_t>(marker_width));
            record_value(event->active_wait,
                         static_cast<std::int64_t>(active_ns));
            trace->stage = event->last_stage = "EVENT_BUDGETS";
        }
        const bool budgets_failed = lateness > kMaxWakeLatenessNs ||
            marker_width > kMaxEventIntervalWidthNs ||
            active_ns > kMaxActiveWaitPerEventNs ||
            total_active_ns > kMaxActiveWaitTotalNs - active_ns;
        const auto study_disposition = study
            ? xen::scheduler_study::detail::classify_event(
                lateness, marker_width, active_ns, total_active_ns)
            : xen::scheduler_study::detail::EventDisposition::ACCEPT;
        if (budgets_failed && (!study || !study->characterize ||
            study_disposition == xen::scheduler_study::detail::EventDisposition::ABORT)) {
            error = "preflight lateness/width/total active budget 超限: event=" +
                std::to_string(index) + ", lateness_ns=" +
                std::to_string(lateness) + ", marker_width_ns=" +
                std::to_string(marker_width) + ", active_ns=" +
                std::to_string(active_ns) + ", active_total_before_ns=" +
                std::to_string(total_active_ns);
            return false;
        }
        total_active_ns += active_ns;
        maximum_lateness_ns = std::max(maximum_lateness_ns, lateness);
        maximum_marker_width_ns = std::max(
            maximum_marker_width_ns, marker_width);
        if (event) {
            event->completed = true;
            trace->stage = event->last_stage = "COMPLETE";
            ++trace->completed_count;
        } else {
            events.push_back({
            {"event_ordinal", index},
            {"deadline_qpc", deadline},
            {"active_enter_qpc", active_enter},
            {"marker_before_qpc", marker_before},
            {"marker_after_qpc", marker_after},
            {"active_read_count", reads},
            {"active_wait_ns", active_ns},
            {"deadline_lateness_ns", lateness},
            {"marker_width_ns", marker_width},
            });
        }
    }
    // 诊断跳过正式 preflight 的 JSON/semantic；原模式保留逐事件记账。
    if (trace) return true;
    document = {
        {"schema_version", 1},
        {"evidence_type",
         "mouse_effect_probe_b_composite_phase_scheduler_preflight"},
        {"status", "PASS"},
        {"physical_output_capability", false},
        {"physical_dispatch_count", 0},
        {"run_uuid", options.run_uuid},
        {"activation_epoch", options.activation_epoch},
        {"plan_seed_semantic_sha256", seed_semantic},
        {"sequence_semantic_sha256", sequence_semantic},
        {"scheduler_clock", {
            {"clock_kind", "WINDOWS_QPC"},
            {"clock_session_id", options.run_uuid + "-preflight-qpc"},
            {"frequency_hz", frequency},
            {"producer_process_id", GetCurrentProcessId()},
        }},
        {"timer_mode", "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL"},
        {"process_priority", "NORMAL"},
        {"thread_priority", "NORMAL"},
        {"cpu_affinity_used", false},
        {"time_begin_period_used", false},
        {"periodic_timer_used", false},
        {"event_count", kPreflightEventCount},
        {"preflight_interval_ns", kPreflightIntervalNs},
        {"active_guard_ns", kActiveGuardNs},
        {"max_wake_lateness_ns", kMaxWakeLatenessNs},
        {"max_event_interval_width_ns", kMaxEventIntervalWidthNs},
        {"max_active_wait_ns_per_event", kMaxActiveWaitPerEventNs},
        {"max_active_wait_ns_total", kMaxActiveWaitTotalNs},
        {"observed_max_wake_lateness_ns", maximum_lateness_ns},
        {"observed_max_marker_width_ns", maximum_marker_width_ns},
        {"observed_active_wait_total_ns", total_active_ns},
        {"events", std::move(events)},
    };
    std::string semantic;
    if (!sha256_text(document.dump(), semantic)) {
        error = "preflight semantic SHA-256 失败";
        return false;
    }
    document["preflight_semantic_sha256"] = semantic;
    return true;
}

nlohmann::json diagnostic_context() {
    const auto process_id = GetCurrentProcessId();
    const auto thread_id = GetCurrentThreadId();
    DWORD session_id = 0;
    const auto session_valid = ProcessIdToSessionId(process_id, &session_id);
    const auto session_error = session_valid ? 0U : GetLastError();
    const auto priority_class = GetPriorityClass(GetCurrentProcess());
    const auto priority_class_error = priority_class ? 0U : GetLastError();
    const auto thread_priority = GetThreadPriority(GetCurrentThread());
    const auto thread_priority_valid =
        thread_priority != THREAD_PRIORITY_ERROR_RETURN;
    const auto thread_priority_error = thread_priority_valid ? 0U : GetLastError();
    return {
        {"process_id", process_id},
        {"thread_id", thread_id},
        {"session_id", session_valid ? nlohmann::json(session_id) : nullptr},
        {"session_id_query_error", session_valid ? nullptr :
            nlohmann::json(session_error)},
        {"process_priority_class", priority_class ?
            nlohmann::json(priority_class) : nullptr},
        {"process_priority_class_query_error", priority_class ? nullptr :
            nlohmann::json(priority_class_error)},
        {"thread_priority", thread_priority_valid ?
            nlohmann::json(thread_priority) : nullptr},
        {"thread_priority_query_error", thread_priority_valid ? nullptr :
            nlohmann::json(thread_priority_error)},
        {"sampled_before_preflight", true},
    };
}

nlohmann::json diagnostic_document(
        const SchedulerTrace& trace, nlohmann::json context,
        bool measured, std::string_view error) {
    auto events = nlohmann::json::array();
    for (std::size_t index = 0; index < trace.reached_count; ++index) {
        const auto& event = trace.events[index];
        nlohmann::json value = {
            {"event_ordinal", index},
            {"completed", event.completed},
            {"last_stage", event.last_stage},
            {"active_total_before_ns", event.active_total_before},
            {"valid", nlohmann::json::object()},
        };
        const auto field = [&](const char* name, const TraceValue& observation) {
            value[name] = observation.valid ?
                nlohmann::json(observation.value) : nullptr;
            value["valid"][name] = observation.valid;
        };
        field("deadline_qpc", event.deadline);
        field("coarse_target_qpc", event.coarse_target);
        field("due_base_qpc", event.due_base);
        field("relative_due_100ns", event.relative_due);
        field("set_timer_before_qpc", event.set_before);
        field("set_timer_after_qpc", event.set_after);
        field("set_timer_result", event.set_result);
        field("wait_result", event.wait_result);
        field("wait_return_qpc", event.wait_return);
        field("active_enter_qpc", event.wait_return);
        field("active_last_qpc", event.active_last);
        field("marker_before_qpc", event.marker_before);
        field("marker_after_qpc", event.marker_after);
        field("active_read_count", event.active_reads);
        field("deadline_lateness_ns", event.lateness);
        field("marker_width_ns", event.marker_width);
        field("active_wait_ns", event.active_wait);
        events.push_back(std::move(value));
    }
    return {
        {"schema_version", 1},
        {"evidence_type", "mouse_effect_probe_scheduler_phase_diagnostic"},
        {"status", measured ? "MEASURED_WITHIN_BUDGETS" : "REJECTED"},
        {"diagnostic_only", true},
        {"instrumented", true},
        {"timing_perturbed", true},
        {"timing_loop_json_bookkeeping", false},
        {"normal_preflight_timing_loop_json_bookkeeping", true},
        {"additional_qpc_reads_per_completed_event", 2},
        {"physical_output_capability", false},
        {"physical_dispatch_count", 0},
        {"formal_preflight_published", false},
        {"final_plan_published", false},
        {"context", std::move(context)},
        {"clock_kind", "WINDOWS_QPC"},
        {"qpc_frequency_hz", trace.frequency > 0 ?
            nlohmann::json(trace.frequency) : nullptr},
        {"anchor_qpc", trace.anchor > 0 ? nlohmann::json(trace.anchor) : nullptr},
        {"timing_policy", {
            {"event_capacity", kPreflightEventCount},
            {"preflight_interval_ns", kPreflightIntervalNs},
            {"active_guard_ns", kActiveGuardNs},
            {"max_wake_lateness_ns", kMaxWakeLatenessNs},
            {"max_event_interval_width_ns", kMaxEventIntervalWidthNs},
            {"max_active_wait_ns_per_event", kMaxActiveWaitPerEventNs},
            {"max_active_wait_ns_total", kMaxActiveWaitTotalNs},
            {"timer_mode", "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL"},
        }},
        {"reached_event_count", trace.reached_count},
        {"completed_event_count", trace.completed_count},
        {"failure_event", trace.reached_count > trace.completed_count ?
            nlohmann::json(trace.reached_count - 1U) : nullptr},
        {"failure_stage", measured || !trace.stage ? nullptr : nlohmann::json(trace.stage)},
        {"failure_reason", error},
        {"win32_failure", trace.win32_failure.api ? nlohmann::json{
            {"api", trace.win32_failure.api},
            {"code", trace.win32_failure.code},
        } : nullptr},
        {"events", std::move(events)},
    };
}

int run_scheduler_study(const Options& options, std::string& error) {
    using xen::scheduler_study::detail::protocol;
    using xen::scheduler_study::detail::select_candidate;
    const auto policy = protocol();
    const auto directory = options.diagnostic_output;
    // 单一新目录同时承担消耗标记；中止后保留所有已取得记录，不接受续跑。
    if (!std::filesystem::create_directory(directory)) {
        std::cerr << "study 目录创建失败或已经存在\n";
        return 2;
    }
    const auto publish = [&](const char* name, const nlohmann::json& value) {
        if (!write_json_atomic(directory / name, value, error)) {
            throw std::runtime_error(error);
        }
        std::string written;
        if (!read_file(directory / name, written, error, kMaximumJsonBytes) ||
            written != value.dump(2) + "\n") {
            throw std::runtime_error("study 产物回读不一致");
        }
    };
    const auto hash_file = [&](const char* name) {
        std::string hash;
        std::string content;
        if (!read_file(directory / name, content, error, kMaximumJsonBytes) ||
            !sha256_text(content, hash)) {
            throw std::runtime_error(error);
        }
        return hash;
    };
    const auto base_context = diagnostic_context();
    if (base_context["process_priority_class"] != NORMAL_PRIORITY_CLASS ||
        base_context["thread_priority"] != THREAD_PRIORITY_NORMAL ||
        base_context["session_id"].is_null()) {
        publish("result.json", {{"status", "ABORTED"},
            {"reason", "实际 session/priority 不满足诊断协议"},
            {"physical_dispatch_count", 0}, {"final_plan_published", false}});
        return 3;
    }
    auto context = base_context;
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> computer{};
    DWORD computer_size = static_cast<DWORD>(computer.size());
    std::array<wchar_t, 32768> executable{};
    const auto executable_size = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    FILETIME created{}, exited{}, kernel{}, user{};
    LARGE_INTEGER frequency{};
    std::int64_t campaign_start = 0;
    if (!GetComputerNameW(computer.data(), &computer_size) ||
        executable_size == 0 || executable_size >= executable.size() ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
        !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !query_qpc(campaign_start)) {
        throw std::runtime_error("study 主机/进程/时钟身份不可用");
    }
    std::string executable_hash;
    if (!file_sha256(std::filesystem::path(executable.data()), executable_hash, error)) {
        throw std::runtime_error(error);
    }
    std::string computer_utf8;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        computer.data(), static_cast<int>(computer_size), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("study 主机名编码失败");
    computer_utf8.resize(size);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, computer.data(),
        static_cast<int>(computer_size), computer_utf8.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("study 主机名编码失败");
    }
    context["computer_name"] = computer_utf8;
    context["process_creation_filetime"] =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
    context["executable_sha256"] = executable_hash;
    context["qpc_frequency_hz"] = frequency.QuadPart;
    context["campaign_start_qpc"] = campaign_start;
    context["uptime_ms_before_campaign"] = GetTickCount64();
    context["same_process_same_boot_required"] = true;
    std::int64_t timeout_ticks = 0;
    if (!ns_to_ticks(policy["campaign_timeout_ns"].get<std::uint64_t>(),
                     frequency.QuadPart, timeout_ticks) ||
        campaign_start > std::numeric_limits<std::int64_t>::max() - timeout_ticks) {
        throw std::runtime_error("study 整体超时转换失败");
    }
    Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event) throw std::runtime_error("study stop event 创建失败");
    study_stop_event = stop_event.get();
    if (!SetConsoleCtrlHandler(stop_study, TRUE)) {
        study_stop_event = nullptr;
        throw std::runtime_error("study Ctrl+C handler 注册失败");
    }
    struct StopRegistration {
        ~StopRegistration() {
            SetConsoleCtrlHandler(stop_study, FALSE);
            study_stop_event = nullptr;
        }
    } registration;
    publish("protocol.json", {{"protocol", policy}, {"context", context},
        {"status", "FROZEN_BEFORE_MEASUREMENT"},
        {"diagnostic_only", true}, {"physical_output_capability", false},
        {"physical_dispatch_count", 0}, {"final_plan_published", false}});
    const auto protocol_hash = hash_file("protocol.json");
    const auto check_context = [&](std::int64_t* observed = nullptr) {
        std::int64_t now = 0;
        const auto current = diagnostic_context();
        const auto clock_valid = query_qpc(now);
        if (observed) *observed = now;
        return current == base_context && clock_valid && now >= campaign_start &&
            now < campaign_start + timeout_ticks &&
            WaitForSingleObject(stop_event.get(), 0) == WAIT_TIMEOUT;
    };
    const auto make_raw = [&](const char* type) {
        return nlohmann::json{{"schema_version", 1}, {"evidence_type", type},
            {"protocol", policy}, {"context", context}, {"status", "ABORTED"},
            {"protocol_file_sha256", protocol_hash},
            {"blocks", nlohmann::json::array()}, {"diagnostic_only", true},
            {"physical_output_capability", false}, {"physical_dispatch_count", 0},
            {"formal_preflight_published", false}, {"final_plan_published", false}};
    };
    std::uint64_t recorded_active_ns = 0;
    std::size_t attempted_batches = 0;
    const auto collect = [&](std::size_t index, std::size_t guard_index,
                              std::uint64_t guard, bool characterize) {
        SchedulerTrace trace{};
        std::string reason;
        nlohmann::json unused;
        const StudyRun run{guard, characterize, stop_event.get(), campaign_start + timeout_ticks};
        const auto before_valid = check_context();
        trace.stage = "STUDY_CONTEXT_BEFORE_BATCH";
        const auto measured = before_valid && run_preflight(options, {}, {}, unused, reason, &trace, &run);
        const auto after_valid = check_context();
        ++attempted_batches;
        for (std::size_t ordinal = 0; ordinal < trace.reached_count; ++ordinal) {
            const auto& event = trace.events[ordinal];
            if (event.active_wait.valid) recorded_active_ns += event.active_wait.value;
            else if (event.wait_return.valid && event.active_last.valid) {
                recorded_active_ns += ticks_to_ns_ceil(
                    event.active_last.value - event.wait_return.value, trace.frequency);
            }
        }
        if (!before_valid || !after_valid) reason = "study 身份漂移、停止或整体经过时间超限";
        auto diagnostic = diagnostic_document(trace, context, measured, reason);
        diagnostic["evidence_type"] = "mouse_effect_probe_scheduler_study_batch";
        diagnostic["study_phase"] = characterize ? "CHARACTERIZATION" : "VALIDATION";
        diagnostic["status"] = measured && after_valid ? "COMPLETE" : "ABORTED";
        diagnostic["timing_policy"]["active_guard_ns"] = guard;
        diagnostic["completed_means"] = "RAW_SAMPLE_COMPLETE_NOT_QUALITY_PASS";
        diagnostic["study_stop_polling"] = true;
        diagnostic["study_wait_timeout_ms"] = 1000;
        return nlohmann::json{{"round_index", index}, {"guard_index", guard_index},
            {"diagnostic", std::move(diagnostic)}};
    };
    auto characterization = make_raw("mouse_effect_probe_scheduler_study_characterization");
    const auto guards = policy["guard_grid_ns"].get<std::vector<std::uint64_t>>();
    bool complete = true;
    for (std::size_t round = 0; complete && round < policy["round_count"].get<std::size_t>(); ++round) {
        for (std::size_t slot = 0; complete && slot < guards.size(); ++slot) {
            const auto index = (round + slot) % guards.size();
            auto block = collect(round, index, guards[index], true);
            complete = block["diagnostic"]["status"] == "COMPLETE";
            characterization["blocks"].push_back(std::move(block));
        }
    }
    characterization["status"] = complete ? "COMPLETE" : "ABORTED";
    publish("characterization.json", characterization);
    const auto characterization_hash = hash_file("characterization.json");
    auto result = nlohmann::json{{"status", "ABORTED"}, {"diagnostic_only", true},
        {"physical_output_capability", false}, {"physical_dispatch_count", 0},
        {"formal_preflight_published", false}, {"final_plan_published", false},
        {"protocol_file_sha256", protocol_hash},
        {"characterization_file_sha256", characterization_hash},
        {"candidate_file_sha256", nullptr}, {"validation_file_sha256", nullptr}};
    const auto finish = [&](const char* status) {
        std::int64_t finished = 0;
        const auto final_status = check_context(&finished) ? status : "ABORTED";
        result["status"] = final_status;
        result["attempted_batch_count"] = attempted_batches;
        result["recorded_active_wait_ns"] = recorded_active_ns;
        result["campaign_max_active_wait_ns"] = policy["campaign_max_active_wait_ns"];
        if (finished > 0) {
            result["finished_qpc"] = finished;
            result["elapsed_ns"] = ticks_to_ns_ceil(finished - campaign_start, frequency.QuadPart);
        }
        result["context_after"] = diagnostic_context();
        publish("result.json", result);
        std::cout << "scheduler study: " << final_status << "; 只生成诊断证据，未封存正式 plan\n";
        return std::string_view(final_status) == "STUDY_VALIDATED" ? 0 : 3;
    };
    if (!complete || !check_context()) return finish("ABORTED");
    auto candidate = select_candidate(characterization);
    candidate["protocol_file_sha256"] = protocol_hash;
    candidate["characterization_file_sha256"] = characterization_hash;
    candidate["context"] = context;
    candidate["diagnostic_only"] = true;
    candidate["formal_preflight_published"] = false;
    candidate["final_plan_published"] = false;
    candidate["physical_output_capability"] = false;
    candidate["physical_dispatch_count"] = 0;
    publish("candidate.json", candidate);
    const auto candidate_hash = hash_file("candidate.json");
    result["candidate_file_sha256"] = candidate_hash;
    std::int64_t candidate_frozen_qpc = 0;
    if (!query_qpc(candidate_frozen_qpc)) return finish("ABORTED");
    result["candidate_frozen_qpc"] = candidate_frozen_qpc;
    if (!check_context() || hash_file("protocol.json") != protocol_hash ||
        hash_file("characterization.json") != characterization_hash) return finish("ABORTED");
    if (candidate["status"] == "NO_CANDIDATE") return finish("NO_CANDIDATE");
    const auto guard = candidate["selected_guard_ns"].get<std::uint64_t>();
    const auto found = std::find(guards.begin(), guards.end(), guard);
    if (found == guards.end()) throw std::runtime_error("study candidate 不在预注册集合中");
    auto validation = make_raw("mouse_effect_probe_scheduler_study_validation");
    validation["candidate_file_sha256"] = candidate_hash;
    validation["candidate_frozen_qpc"] = candidate_frozen_qpc;
    validation["characterization_file_sha256"] = characterization_hash;
    validation["not_used_for_candidate_selection"] = true;
    validation["statistical_independence_claimed"] = false;
    for (std::size_t round = 0; complete && round < policy["validation_batch_count"].get<std::size_t>(); ++round) {
        auto block = collect(round, static_cast<std::size_t>(found - guards.begin()), guard, false);
        complete = block["diagnostic"]["status"] == "COMPLETE";
        validation["blocks"].push_back(std::move(block));
    }
    validation["status"] = complete ? "COMPLETE" : "ABORTED";
    publish("validation.json", validation);
    result["validation_file_sha256"] = hash_file("validation.json");
    if (!check_context() || hash_file("protocol.json") != protocol_hash ||
        hash_file("characterization.json") != characterization_hash ||
        hash_file("candidate.json") != candidate_hash) return finish("ABORTED");
    return finish(complete ? "STUDY_VALIDATED" : "VALIDATION_REJECTED");
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        if (!error.empty()) std::cerr << "参数错误: " << error << "\n\n";
        std::cerr
            << "XenMouseEffectProbeCompositeSeal --plan-seed <json> "
               "--sequence <json> --preflight-output <new-json> "
               "--plan-output <new-json> --run-uuid <uuid> "
               "--activation-epoch <n>\n"
            << "XenMouseEffectProbeCompositeSeal "
               "--diagnose-scheduler <absolute-new-json>\n"
            << "XenMouseEffectProbeCompositeSeal "
               "--study-scheduler <absolute-new-directory>\n"
            << "XenMouseEffectProbeCompositeSeal "
               "--report-semantic-sha256 <report-json>\n"
            << "XenMouseEffectProbeCompositeSeal "
               "--verify-report <report-json>\n";
        return error.empty() ? 0 : 2;
    }
    try {
        if (options.mode == Mode::STUDY_SCHEDULER) {
            return run_scheduler_study(options, error);
        }
        if (options.mode == Mode::DIAGNOSE_SCHEDULER) {
            // 固定记录和实际元数据在 anchor 前准备；只生成独立诊断产物。
            SchedulerTrace trace{};
            auto context = diagnostic_context();
            nlohmann::json unused_preflight;
            const auto measured = run_preflight(
                options, {}, {}, unused_preflight, error, &trace);
            const auto diagnostic = diagnostic_document(
                trace, std::move(context), measured, error);
            std::string publish_error;
            if (!write_json_atomic(
                    options.diagnostic_output, diagnostic, publish_error)) {
                std::cerr << "scheduler 诊断发布失败: " << publish_error << '\n';
                return 4;
            }
            if (!measured) {
                std::cerr << "scheduler 诊断拒绝: " << error << '\n';
                return 3;
            }
            std::cout << "本次独立阶段测量满足预算；未封存正式 preflight/plan\n";
            return 0;
        }
        if (options.mode != Mode::PREFLIGHT_AND_SEAL) {
            std::string claimed;
            std::string actual;
            if (!calculate_report_semantic(
                    options.report, claimed, actual, error)) {
                std::cerr << "command report 校验失败: " << error << '\n';
                return 2;
            }
            if (options.mode == Mode::VERIFY_REPORT &&
                (!valid_sha256(claimed) || claimed != actual)) {
                std::cerr << "command report semantic SHA-256 漂移\n";
                return 2;
            }
            std::cout << actual << '\n';
            return 0;
        }
        nlohmann::json seed;
        std::string sequence_semantic;
        if (!validate_seed(
                options, seed, sequence_semantic, error)) {
            std::cerr << "seal 前置校验失败: " << error << '\n';
            return 2;
        }
        const auto seed_semantic = seed[
            "plan_seed_semantic_sha256"].get<std::string>();
        nlohmann::json preflight;
        if (!run_preflight(options, seed_semantic, sequence_semantic,
                           preflight, error)) {
            std::cerr << "scheduler preflight 失败: " << error << '\n';
            return 3;
        }
        if (!write_json_atomic(
                options.preflight_output, preflight, error)) {
            std::cerr << "preflight 发布失败: " << error << '\n';
            return 3;
        }
        std::string preflight_file_sha256;
        if (!file_sha256(options.preflight_output,
                         preflight_file_sha256, error)) {
            std::cerr << "preflight 回读失败: " << error << '\n';
            return 3;
        }
        seed.erase("plan_seed_semantic_sha256");
        seed["status"] = "FROZEN_BEFORE_CAPTURE";
        seed["frozen_at_utc_unix_ns"] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        seed["scheduler_policy"]["preflight_file_sha256"] =
            preflight_file_sha256;
        std::string plan_semantic;
        if (!sha256_text(seed.dump(), plan_semantic)) {
            std::cerr << "最终 plan semantic SHA-256 失败\n";
            return 4;
        }
        seed["plan_semantic_sha256"] = plan_semantic;
        if (!write_json_atomic(options.plan_output, seed, error)) {
            std::cerr << "最终 plan 发布失败: " << error << '\n';
            return 4;
        }
        std::string plan_file_sha256;
        if (!file_sha256(options.plan_output, plan_file_sha256, error)) {
            std::cerr << "最终 plan 回读失败: " << error << '\n';
            return 4;
        }
        std::cout << "scheduler preflight 与最终 plan 已封存: plan_sha256="
                  << plan_file_sha256 << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "composite seal 异常: " << exception.what() << '\n';
        return 5;
    }
}
