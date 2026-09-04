#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
    REPORT_SEMANTIC_SHA256,
    VERIFY_REPORT,
};

struct Options {
    Mode mode = Mode::PREFLIGHT_AND_SEAL;
    std::filesystem::path report;
    std::filesystem::path plan_seed;
    std::filesystem::path sequence;
    std::filesystem::path preflight_output;
    std::filesystem::path plan_output;
    std::string run_uuid;
    std::uint64_t activation_epoch = 0;
};

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

bool query_qpc(std::int64_t& value) {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart <= 0) {
        return false;
    }
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
                   nlohmann::json& document, std::string& error) {
    LARGE_INTEGER frequency_value{};
    if (!QueryPerformanceFrequency(&frequency_value) ||
        frequency_value.QuadPart <= 0) {
        error = "QPC frequency 不可用";
        return false;
    }
    const auto frequency = frequency_value.QuadPart;
    Handle timer(CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS));
    if (!timer) {
        error = "高分辨率 one-shot timer 不可用";
        return false;
    }
    std::int64_t interval_ticks = 0;
    std::int64_t guard_ticks = 0;
    std::int64_t anchor = 0;
    if (!ns_to_ticks(kPreflightIntervalNs, frequency, interval_ticks) ||
        !ns_to_ticks(kActiveGuardNs, frequency, guard_ticks) ||
        !query_qpc(anchor)) {
        error = "preflight QPC 单位转换失败";
        return false;
    }
    nlohmann::json events = nlohmann::json::array();
    std::uint64_t total_active_ns = 0;
    std::uint64_t maximum_lateness_ns = 0;
    std::uint64_t maximum_marker_width_ns = 0;
    for (std::size_t index = 0; index < kPreflightEventCount; ++index) {
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
        std::int64_t now = 0;
        if (!query_qpc(now) || now >= active_target) {
            error = "preflight coarse deadline 已迟到";
            return false;
        }
        LARGE_INTEGER due{};
        if (!relative_due(active_target - now, frequency, due) ||
            !SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer.get(), INFINITE) != WAIT_OBJECT_0 ||
            !query_qpc(now)) {
            error = "preflight one-shot wait 失败";
            return false;
        }
        const auto active_enter = now;
        std::uint64_t reads = 0;
        while (now < deadline) {
            ++reads;
            if (!query_qpc(now)) {
                error = "preflight active wait QPC 失败";
                return false;
            }
            const auto active_ns = ticks_to_ns_ceil(
                now - active_enter, frequency);
            if (active_ns > kMaxActiveWaitPerEventNs) {
                error = "preflight 单事件 active wait 超限";
                return false;
            }
        }
        std::int64_t marker_before = 0;
        std::int64_t marker_after = 0;
        if (!query_qpc(marker_before) || !query_qpc(marker_after) ||
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
        if (lateness > kMaxWakeLatenessNs ||
            marker_width > kMaxEventIntervalWidthNs ||
            active_ns > kMaxActiveWaitPerEventNs ||
            total_active_ns > kMaxActiveWaitTotalNs - active_ns) {
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
               "--report-semantic-sha256 <report-json>\n"
            << "XenMouseEffectProbeCompositeSeal "
               "--verify-report <report-json>\n";
        return error.empty() ? 0 : 2;
    }
    try {
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
