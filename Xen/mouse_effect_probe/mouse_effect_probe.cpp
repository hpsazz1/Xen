#include "mouse_effect_probe/mouse_effect_probe.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string_view>

namespace mouse_effect_probe {

namespace {

constexpr std::uint32_t kSequenceSchema = 1;
constexpr std::string_view kSparsePulseProfile = "sparse_pulse_a";
constexpr std::uint64_t kMaximumSequenceSamples = 1'000'000;
constexpr std::uintmax_t kMaximumSequenceFileBytes = 128U * 1024U * 1024U;
constexpr std::uint32_t kReportSchema = 1;
constexpr std::string_view kReportEvidenceType =
    "backend_completed_command_to_visible_background_response";
constexpr std::uintmax_t kMaximumReportFileBytes = 256U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumBindingFileBytes = 16U * 1024U * 1024U;

void set_error(std::string& output, std::string_view value) noexcept {
    try {
        output.assign(value);
    } catch (...) {
    }
}

std::int64_t steady_nanoseconds(
        std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        value.time_since_epoch()).count();
}

bool checked_add(std::uint64_t first,
                 std::uint64_t second,
                 std::uint64_t& output) noexcept {
    if (first > std::numeric_limits<std::uint64_t>::max() - second) {
        return false;
    }
    output = first + second;
    return true;
}

bool expected_sample_count(const SparsePulseSequenceRequest& request,
                           std::uint64_t& output) noexcept {
    if (request.baseline_sample_count == 0 ||
        request.response_sample_count == 0 ||
        request.guard_sample_count == 0) {
        return false;
    }
    std::uint64_t response_samples = 0;
    std::uint64_t guard_samples = 0;
    std::uint64_t total = 0;
    if (request.response_sample_count >
            std::numeric_limits<std::uint64_t>::max() / 4U ||
        request.guard_sample_count >
            std::numeric_limits<std::uint64_t>::max() / 3U) {
        return false;
    }
    response_samples = request.response_sample_count * 4U;
    guard_samples = request.guard_sample_count * 3U;
    if (!checked_add(request.baseline_sample_count, 4U, total) ||
        !checked_add(total, response_samples, total) ||
        !checked_add(total, guard_samples, total) ||
        total > kMaximumSequenceSamples) {
        return false;
    }
    output = total;
    return true;
}

void append_sample(MouseEffectProbeSequence& sequence,
                   std::uint64_t block_id,
                   ProbeSamplePhase phase,
                   int dx_counts) {
    ProbeSequenceSample sample;
    sample.sample_index = sequence.samples.size();
    sample.block_id = block_id;
    sample.phase = phase;
    sample.dx_counts = dx_counts;
    sample.dy_counts = 0;
    sequence.samples.push_back(sample);
}

void append_zeros(MouseEffectProbeSequence& sequence,
                  std::uint64_t block_id,
                  ProbeSamplePhase phase,
                  std::uint64_t count) {
    for (std::uint64_t index = 0; index < count; ++index) {
        append_sample(sequence, block_id, phase, 0);
    }
}

void append_sparse_block(MouseEffectProbeSequence& sequence,
                         std::uint64_t block_id,
                         int first_direction) {
    ProbeSequenceBlock block;
    block.block_id = block_id;
    block.first_sample_index = sequence.samples.size();
    block.first_pulse_dx_counts = first_direction;
    block.second_pulse_dx_counts = -first_direction;

    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  first_direction);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 sequence.request.response_sample_count);
    append_zeros(sequence, block_id, ProbeSamplePhase::GUARD,
                 sequence.request.guard_sample_count);
    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  -first_direction);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 sequence.request.response_sample_count);

    block.sample_count = sequence.samples.size() - block.first_sample_index;
    sequence.blocks.push_back(block);
}

bool build_sparse_sequence(const SparsePulseSequenceRequest& request,
                           MouseEffectProbeSequence& sequence,
                           std::string& error) {
    std::uint64_t sample_count = 0;
    if (!expected_sample_count(request, sample_count)) {
        set_error(error,
            "baseline/response/guard 必须为正且总样本不超过固定容量边界");
        return false;
    }
    sequence = {};
    sequence.schema = kSequenceSchema;
    sequence.profile = kSparsePulseProfile;
    sequence.request = request;
    sequence.samples.reserve(static_cast<std::size_t>(sample_count));
    sequence.blocks.reserve(2U);

    append_zeros(sequence, 0, ProbeSamplePhase::BASELINE,
                 request.baseline_sample_count);
    append_sparse_block(sequence, 1, 1);
    append_zeros(sequence, 0, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_sparse_block(sequence, 2, -1);

    std::int64_t prefix = 0;
    std::uint64_t maximum = 0;
    for (const auto& sample : sequence.samples) {
        prefix += sample.dx_counts;
        const auto absolute = prefix < 0
            ? static_cast<std::uint64_t>(-prefix)
            : static_cast<std::uint64_t>(prefix);
        maximum = std::max(maximum, absolute);
    }
    sequence.net_x_counts = prefix;
    sequence.max_abs_prefix_x_counts = maximum;
    error.clear();
    return true;
}

nlohmann::ordered_json canonical_payload(
        const MouseEffectProbeSequence& sequence) {
    nlohmann::ordered_json blocks = nlohmann::ordered_json::array();
    for (const auto& block : sequence.blocks) {
        blocks.push_back({
            {"block_id", block.block_id},
            {"first_sample_index", block.first_sample_index},
            {"sample_count", block.sample_count},
            {"first_pulse_dx_counts", block.first_pulse_dx_counts},
            {"second_pulse_dx_counts", block.second_pulse_dx_counts},
        });
    }
    nlohmann::ordered_json samples = nlohmann::ordered_json::array();
    for (const auto& sample : sequence.samples) {
        samples.push_back({
            {"sample_index", sample.sample_index},
            {"block_id", sample.block_id},
            {"phase", probe_sample_phase_name(sample.phase)},
            {"dx_counts", sample.dx_counts},
            {"dy_counts", sample.dy_counts},
        });
    }
    return {
        {"schema", sequence.schema},
        {"profile", sequence.profile},
        {"request", {
            {"baseline_sample_count",
             sequence.request.baseline_sample_count},
            {"response_sample_count",
             sequence.request.response_sample_count},
            {"guard_sample_count", sequence.request.guard_sample_count},
        }},
        {"blocks", std::move(blocks)},
        {"samples", std::move(samples)},
        {"summary", {
            {"net_x_counts", sequence.net_x_counts},
            {"max_abs_prefix_x_counts",
             sequence.max_abs_prefix_x_counts},
        }},
    };
}

bool sha256(std::string_view input,
            std::string& output,
            std::string& error) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        if (input.size() > std::numeric_limits<ULONG>::max()) {
            set_error(error, "SHA-256 输入超过 Windows CNG 单次容量边界");
            return false;
        }
        DWORD object_bytes = 0;
        DWORD digest_bytes = 0;
        DWORD copied = 0;
        const auto succeeded = [](NTSTATUS status) noexcept {
            return status >= 0;
        };
        if (!succeeded(BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
            !succeeded(BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_bytes),
                sizeof(object_bytes), &copied, 0)) ||
            copied != sizeof(object_bytes) || object_bytes == 0 ||
            !succeeded(BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_bytes),
                sizeof(digest_bytes), &copied, 0)) ||
            copied != sizeof(digest_bytes) || digest_bytes != 32U) {
            set_error(error, "无法初始化 Windows CNG SHA-256");
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        std::vector<std::uint8_t> object(object_bytes);
        std::array<std::uint8_t, 32> digest{};
        if (!succeeded(BCryptCreateHash(
                algorithm, &hash, object.data(), object_bytes,
                nullptr, 0, 0)) ||
            (!input.empty() && !succeeded(BCryptHashData(
                hash,
                reinterpret_cast<PUCHAR>(
                    const_cast<char*>(input.data())),
                static_cast<ULONG>(input.size()), 0))) ||
            !succeeded(BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            set_error(error, "计算序列 SHA-256 失败");
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return false;
        }
        constexpr char hexadecimal[] = "0123456789abcdef";
        output.clear();
        output.reserve(digest.size() * 2U);
        for (const auto byte : digest) {
            output.push_back(hexadecimal[byte >> 4U]);
            output.push_back(hexadecimal[byte & 0x0fU]);
        }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error.clear();
        return true;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        set_error(error, "计算序列 SHA-256 时发生未知异常");
        return false;
    }
}

bool same_request(const SparsePulseSequenceRequest& first,
                  const SparsePulseSequenceRequest& second) noexcept {
    return first.baseline_sample_count == second.baseline_sample_count &&
           first.response_sample_count == second.response_sample_count &&
           first.guard_sample_count == second.guard_sample_count;
}

bool same_block(const ProbeSequenceBlock& first,
                const ProbeSequenceBlock& second) noexcept {
    return first.block_id == second.block_id &&
           first.first_sample_index == second.first_sample_index &&
           first.sample_count == second.sample_count &&
           first.first_pulse_dx_counts == second.first_pulse_dx_counts &&
           first.second_pulse_dx_counts == second.second_pulse_dx_counts;
}

bool same_sample(const ProbeSequenceSample& first,
                 const ProbeSequenceSample& second) noexcept {
    return first.sample_index == second.sample_index &&
           first.block_id == second.block_id &&
           first.phase == second.phase &&
           first.dx_counts == second.dx_counts &&
           first.dy_counts == second.dy_counts;
}

bool has_exact_keys(const nlohmann::ordered_json& value,
                    std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
        return value.contains(std::string(key));
    });
}

bool read_u64(const nlohmann::ordered_json& object,
              const char* key,
              std::uint64_t& output) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) return false;
    output = value.get<std::uint64_t>();
    return true;
}

bool read_i64(const nlohmann::ordered_json& object,
              const char* key,
              std::int64_t& output) {
    const auto& value = object.at(key);
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        output = static_cast<std::int64_t>(unsigned_value);
        return true;
    }
    if (!value.is_number_integer()) return false;
    output = value.get<std::int64_t>();
    return true;
}

bool read_int(const nlohmann::ordered_json& object,
              const char* key,
              int& output) {
    std::int64_t value = 0;
    if (!read_i64(object, key, value) ||
        value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

bool parse_phase(std::string_view value, ProbeSamplePhase& output) noexcept {
    if (value == "baseline") {
        output = ProbeSamplePhase::BASELINE;
    } else if (value == "pulse") {
        output = ProbeSamplePhase::PULSE;
    } else if (value == "response") {
        output = ProbeSamplePhase::RESPONSE;
    } else if (value == "guard") {
        output = ProbeSamplePhase::GUARD;
    } else {
        return false;
    }
    return true;
}

bool valid_run_uuid(std::string_view value) noexcept {
    if (value.size() != 36U) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') return false;
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool parse_document(const nlohmann::ordered_json& document,
                    MouseEffectProbeSequence& sequence,
                    std::string& error) {
    if (!has_exact_keys(document,
            {"schema", "profile", "request", "blocks", "samples",
             "summary", "sequence_sha256"}) ||
        !document.at("schema").is_number_unsigned() ||
        !document.at("profile").is_string() ||
        !document.at("sequence_sha256").is_string()) {
        set_error(error, "序列根对象字段集合或类型非法");
        return false;
    }
    const auto schema = document.at("schema").get<std::uint64_t>();
    if (schema > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "序列 schema 超出 uint32 边界");
        return false;
    }
    MouseEffectProbeSequence candidate;
    candidate.schema = static_cast<std::uint32_t>(schema);
    candidate.profile = document.at("profile").get<std::string>();
    candidate.sequence_sha256 =
        document.at("sequence_sha256").get<std::string>();

    const auto& request = document.at("request");
    if (!has_exact_keys(request,
            {"baseline_sample_count", "response_sample_count",
             "guard_sample_count"}) ||
        !read_u64(request, "baseline_sample_count",
                  candidate.request.baseline_sample_count) ||
        !read_u64(request, "response_sample_count",
                  candidate.request.response_sample_count) ||
        !read_u64(request, "guard_sample_count",
                  candidate.request.guard_sample_count)) {
        set_error(error, "序列 request 字段集合或类型非法");
        return false;
    }

    const auto& blocks = document.at("blocks");
    if (!blocks.is_array() || blocks.size() > 2U) {
        set_error(error, "序列 blocks 必须是固定容量数组");
        return false;
    }
    candidate.blocks.reserve(blocks.size());
    for (const auto& value : blocks) {
        if (!has_exact_keys(value,
                {"block_id", "first_sample_index", "sample_count",
                 "first_pulse_dx_counts", "second_pulse_dx_counts"})) {
            set_error(error, "序列 block 字段集合非法");
            return false;
        }
        ProbeSequenceBlock block;
        if (!read_u64(value, "block_id", block.block_id) ||
            !read_u64(value, "first_sample_index",
                      block.first_sample_index) ||
            !read_u64(value, "sample_count", block.sample_count) ||
            !read_int(value, "first_pulse_dx_counts",
                      block.first_pulse_dx_counts) ||
            !read_int(value, "second_pulse_dx_counts",
                      block.second_pulse_dx_counts)) {
            set_error(error, "序列 block 字段类型或数值非法");
            return false;
        }
        candidate.blocks.push_back(block);
    }

    const auto& samples = document.at("samples");
    if (!samples.is_array() || samples.size() > kMaximumSequenceSamples) {
        set_error(error, "序列 samples 必须是固定容量数组");
        return false;
    }
    candidate.samples.reserve(samples.size());
    for (const auto& value : samples) {
        if (!has_exact_keys(value,
                {"sample_index", "block_id", "phase", "dx_counts",
                 "dy_counts"}) ||
            !value.at("phase").is_string()) {
            set_error(error, "序列 sample 字段集合或 phase 类型非法");
            return false;
        }
        ProbeSequenceSample sample;
        const auto phase = value.at("phase").get<std::string>();
        if (!read_u64(value, "sample_index", sample.sample_index) ||
            !read_u64(value, "block_id", sample.block_id) ||
            !parse_phase(phase, sample.phase) ||
            !read_int(value, "dx_counts", sample.dx_counts) ||
            !read_int(value, "dy_counts", sample.dy_counts)) {
            set_error(error, "序列 sample 字段类型或数值非法");
            return false;
        }
        candidate.samples.push_back(sample);
    }

    const auto& summary = document.at("summary");
    if (!has_exact_keys(summary,
            {"net_x_counts", "max_abs_prefix_x_counts"}) ||
        !read_i64(summary, "net_x_counts", candidate.net_x_counts) ||
        !read_u64(summary, "max_abs_prefix_x_counts",
                  candidate.max_abs_prefix_x_counts)) {
        set_error(error, "序列 summary 字段集合或类型非法");
        return false;
    }
    if (!validate_mouse_effect_probe_sequence(candidate, error)) return false;
    sequence = std::move(candidate);
    error.clear();
    return true;
}

} // namespace

const char* probe_sample_phase_name(ProbeSamplePhase phase) noexcept {
    switch (phase) {
        case ProbeSamplePhase::BASELINE: return "baseline";
        case ProbeSamplePhase::PULSE: return "pulse";
        case ProbeSamplePhase::RESPONSE: return "response";
        case ProbeSamplePhase::GUARD: return "guard";
    }
    return "unknown";
}

bool make_sparse_pulse_sequence(
        const SparsePulseSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_sparse_sequence(request, candidate, error)) return false;
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) return false;
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error, std::string("生成稀疏脉冲序列异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成稀疏脉冲序列时发生未知异常");
        return false;
    }
}

bool validate_mouse_effect_probe_sequence(
        const MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence expected;
        if (!make_sparse_pulse_sequence(sequence.request, expected, error)) {
            return false;
        }
        if (sequence.schema != expected.schema ||
            sequence.profile != expected.profile ||
            !same_request(sequence.request, expected.request) ||
            sequence.net_x_counts != expected.net_x_counts ||
            sequence.max_abs_prefix_x_counts !=
                expected.max_abs_prefix_x_counts ||
            sequence.blocks.size() != expected.blocks.size() ||
            sequence.samples.size() != expected.samples.size()) {
            set_error(error, "稀疏脉冲序列结构或汇总不符合固定合同");
            return false;
        }
        for (std::size_t index = 0; index < sequence.blocks.size(); ++index) {
            if (!same_block(sequence.blocks[index], expected.blocks[index])) {
                set_error(error, "稀疏脉冲 block 边界或方向非法");
                return false;
            }
        }
        for (std::size_t index = 0; index < sequence.samples.size(); ++index) {
            if (!same_sample(sequence.samples[index], expected.samples[index])) {
                set_error(error,
                    "稀疏脉冲 sample 顺序、相位或 X/Y 位移非法");
                return false;
            }
        }
        if (sequence.sequence_sha256 != expected.sequence_sha256) {
            set_error(error, "稀疏脉冲规范语义 SHA-256 不匹配");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "校验稀疏脉冲序列时发生未知异常");
        return false;
    }
}

bool write_mouse_effect_probe_sequence(
        const std::filesystem::path& path,
        const MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    std::filesystem::path temporary_path;
    try {
        if (!validate_mouse_effect_probe_sequence(sequence, error)) {
            return false;
        }
        if (path.empty()) {
            set_error(error, "序列发布路径不能为空");
            return false;
        }
        const auto final_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(final_path)) {
            set_error(error, "序列发布目标已存在，拒绝覆盖");
            return false;
        }
        const auto parent = final_path.parent_path();
        std::error_code directory_error;
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error ||
            (!parent.empty() && !std::filesystem::is_directory(parent))) {
            set_error(error, "序列发布目录创建失败");
            return false;
        }
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        if (std::filesystem::exists(temporary_path)) {
            set_error(error, "序列临时发布目标已存在，拒绝覆盖");
            temporary_path.clear();
            return false;
        }
        auto document = canonical_payload(sequence);
        document["sequence_sha256"] = sequence.sequence_sha256;
        const std::string content = document.dump(2) + '\n';
        std::ofstream output(
            temporary_path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(
            content.size()));
        output.flush();
        const bool written = output.good();
        output.close();
        if (!written) {
            set_error(error, "序列临时文件写入失败");
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            set_error(error, "序列原子发布失败，Win32Error=" +
                             std::to_string(GetLastError()));
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        temporary_path.clear();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("发布序列异常: ") + exception.what());
    } catch (...) {
        set_error(error, "发布序列时发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    return false;
}

bool read_mouse_effect_probe_sequence(
        const std::filesystem::path& path,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    sequence = {};
    try {
        std::error_code filesystem_error;
        if (path.empty() || !std::filesystem::is_regular_file(
                path, filesystem_error) || filesystem_error) {
            set_error(error, "序列输入不是可读普通文件");
            return false;
        }
        const auto bytes = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error || bytes == 0 ||
            bytes > kMaximumSequenceFileBytes) {
            set_error(error, "序列输入为空或超过固定文件容量边界");
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开序列输入文件");
            return false;
        }
        const std::string content((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            set_error(error, "读取序列输入文件失败");
            return false;
        }
        const auto document = nlohmann::ordered_json::parse(content);
        MouseEffectProbeSequence candidate;
        if (!parse_document(document, candidate, error)) return false;
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        set_error(error, std::string("解析序列异常: ") + exception.what());
        return false;
    } catch (...) {
        set_error(error, "解析序列时发生未知异常");
        return false;
    }
}

class MouseEffectProbeExecutor::Impl {
public:
    ProbeExecutionOptions options;
    MouseEffectProbeSequence sequence;
    std::shared_ptr<IMouseController> mouse;
    ProbeExecutionResult result;
    std::optional<std::uint64_t> last_source_frame_sequence;
    std::optional<std::int64_t> last_source_timestamp;
    std::string source_clock_session_id;

    void close_mouse() noexcept {
        if (mouse && mouse->status() != MouseStatus::CLOSED) mouse->close();
    }

    void stop(ProbeStopReason reason) noexcept {
        result.state = ProbeExecutionState::STOPPED;
        result.stop_reason = reason;
        result.complete = false;
        close_mouse();
    }

    void complete() noexcept {
        result.state = ProbeExecutionState::COMPLETED;
        result.stop_reason = ProbeStopReason::NORMAL_COMPLETION;
        result.complete = true;
        close_mouse();
    }
};

MouseEffectProbeExecutor::MouseEffectProbeExecutor() noexcept
    : impl_(new (std::nothrow) Impl) {}

MouseEffectProbeExecutor::~MouseEffectProbeExecutor() {
    if (impl_) impl_->close_mouse();
}

bool MouseEffectProbeExecutor::start(
        const ProbeExecutionOptions& options,
        const MouseEffectProbeSequence& sequence,
        std::shared_ptr<IMouseController> mouse,
        std::string& error) noexcept {
    if (!impl_) {
        set_error(error, "Mouse Effect Probe executor 内存初始化失败");
        return false;
    }
    try {
        if (impl_->result.state == ProbeExecutionState::RUNNING) {
            set_error(error, "Mouse Effect Probe executor 已在运行");
            return false;
        }
        impl_->close_mouse();
        impl_->options = options;
        impl_->sequence = sequence;
        impl_->mouse = std::move(mouse);
        impl_->result = {};
        impl_->result.dispatch_mode = options.dispatch_mode;
        impl_->last_source_frame_sequence.reset();
        impl_->last_source_timestamp.reset();
        impl_->source_clock_session_id.clear();

        if (!valid_run_uuid(options.run_uuid) ||
            options.activation_epoch == 0) {
            impl_->stop(ProbeStopReason::AUTHORIZATION_MISSING);
            set_error(error, "probe run UUID 或 activation epoch 非法");
            return false;
        }
        if (!validate_mouse_effect_probe_sequence(sequence, error)) {
            impl_->stop(ProbeStopReason::AUTHORIZATION_MISSING);
            return false;
        }
        if (options.dispatch_mode ==
                ProbeDispatchMode::OUTPUT_OFF_REHEARSAL) {
            if (options.allow_physical_output ||
                options.physical_output_confirmed || impl_->mouse) {
                impl_->stop(ProbeStopReason::AUTHORIZATION_MISSING);
                set_error(error,
                    "output-off rehearsal 禁止物理授权和 Mouse adapter");
                return false;
            }
            impl_->result.state = ProbeExecutionState::RUNNING;
            impl_->result.events.reserve(sequence.samples.size());
            error.clear();
            return true;
        }
        if (!options.allow_physical_output ||
            !options.physical_output_confirmed ||
            !options.require_protocol_ack) {
            impl_->stop(ProbeStopReason::AUTHORIZATION_MISSING);
            set_error(error,
                "实际效果 probe 缺少双重物理授权或 protocol ACK 门禁");
            return false;
        }
        if (!impl_->mouse) {
            impl_->stop(ProbeStopReason::MOUSE_FAILURE);
            set_error(error, "实际效果 probe 未提供 Mouse adapter");
            return false;
        }
        if (!impl_->mouse->open()) {
            const bool owner_conflict =
                impl_->mouse->status() == MouseStatus::OWNER_CONFLICT;
            impl_->stop(owner_conflict
                ? ProbeStopReason::EXCLUSIVE_OWNER_MISSING
                : ProbeStopReason::MOUSE_FAILURE);
            set_error(error, owner_conflict
                ? "实际效果 probe 无法获取独占 Mouse owner"
                : "实际效果 probe Mouse 打开失败");
            return false;
        }
        if (!impl_->mouse->output_owner_exclusive()) {
            impl_->stop(ProbeStopReason::EXCLUSIVE_OWNER_MISSING);
            set_error(error,
                "实际效果 probe Mouse adapter 未持有 factory owner lease");
            return false;
        }
        if (impl_->mouse->status() != MouseStatus::READY) {
            impl_->stop(ProbeStopReason::MOUSE_FAILURE);
            set_error(error, "实际效果 probe Mouse 未进入 READY");
            return false;
        }
        impl_->result.state = ProbeExecutionState::RUNNING;
        impl_->result.events.reserve(sequence.samples.size());
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        impl_->stop(ProbeStopReason::MOUSE_FAILURE);
        set_error(error, std::string("启动实际效果 probe 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        impl_->stop(ProbeStopReason::MOUSE_FAILURE);
        set_error(error, "启动实际效果 probe 时发生未知异常");
        return false;
    }
}

bool MouseEffectProbeExecutor::consume_source_frame(
        const ProbeSourceFrameEvent& frame,
        std::string& error) noexcept {
    if (!impl_ || impl_->result.state != ProbeExecutionState::RUNNING) {
        set_error(error, "实际效果 probe 未处于 RUNNING");
        return false;
    }
    try {
        const auto stop_before_sample = [&](ProbeStopReason reason,
                                            std::string_view message) {
            impl_->stop(reason);
            set_error(error, message);
            return false;
        };
        if (!frame.source_timing_valid ||
            !frame.source_timestamp_valid ||
            frame.source_timestamp <= 0 ||
            frame.source_time_at_steady_ns <= 0 ||
            frame.source_time_basis != "NDI_SDK_SUBMISSION" ||
            frame.source_clock_status != "VALID" ||
            frame.source_clock_session_id.empty() ||
            !std::isfinite(frame.source_clock_uncertainty_ms) ||
            frame.source_clock_uncertainty_ms < 0.0 ||
            !std::isfinite(frame.source_clock_rtt_ms) ||
            frame.source_clock_rtt_ms < 0.0 ||
            !std::isfinite(frame.source_clock_mapping_age_ms) ||
            frame.source_clock_mapping_age_ms < 0.0 ||
            frame.source_clock_sample_count == 0) {
            return stop_before_sample(
                ProbeStopReason::SOURCE_TIMING_INVALID,
                "source timing 无效，probe 立即停发");
        }
        if (frame.source_dropped_frames != 0 ||
            frame.transport_dropped_frames != 0 ||
            frame.transport_invalid_packets != 0) {
            return stop_before_sample(
                ProbeStopReason::SOURCE_FRAME_GAP,
                "capture/transport 出现 drop 或 invalid，probe 立即停发");
        }
        if (!frame.sidecar_recording) {
            return stop_before_sample(
                ProbeStopReason::SIDECAR_UNAVAILABLE,
                "sidecar 未证明仍在记录，probe 立即停发");
        }
        if (impl_->options.dispatch_mode == ProbeDispatchMode::PHYSICAL_A &&
            !frame.safety_allowed) {
            return stop_before_sample(
                ProbeStopReason::SAFETY_RELEASED,
                "deadman 或安全门释放，probe 立即停发");
        }
        if (!impl_->source_clock_session_id.empty() &&
            frame.source_clock_session_id !=
                impl_->source_clock_session_id) {
            return stop_before_sample(
                ProbeStopReason::SOURCE_SESSION_CHANGED,
                "source clock session 改变，probe 立即停发");
        }
        if (impl_->last_source_frame_sequence.has_value()) {
            const auto previous = *impl_->last_source_frame_sequence;
            if (previous == std::numeric_limits<std::uint64_t>::max() ||
                frame.source_frame_sequence != previous + 1U) {
                return stop_before_sample(
                    ProbeStopReason::SOURCE_FRAME_GAP,
                    "source frame 不连续，probe 不追发并立即停止");
            }
        }
        if (impl_->last_source_timestamp.has_value() &&
            frame.source_timestamp <= *impl_->last_source_timestamp) {
            return stop_before_sample(
                ProbeStopReason::SOURCE_FRAME_GAP,
                "source timestamp 未严格递增，probe 立即停发");
        }
        if (impl_->result.consumed_sample_count >=
            impl_->sequence.samples.size()) {
            impl_->complete();
            set_error(error, "probe 序列已正常完成");
            return false;
        }

        if (impl_->source_clock_session_id.empty()) {
            impl_->source_clock_session_id =
                frame.source_clock_session_id;
        }
        const auto& sample = impl_->sequence.samples[
            static_cast<std::size_t>(impl_->result.consumed_sample_count)];
        ProbeCommandEvent event;
        event.run_uuid = impl_->options.run_uuid;
        event.activation_epoch = impl_->options.activation_epoch;
        event.block_id = sample.block_id;
        event.sequence_sha256 = impl_->sequence.sequence_sha256;
        event.sample_index = sample.sample_index;
        event.source_frame_sequence = frame.source_frame_sequence;
        event.source_timestamp = frame.source_timestamp;
        event.source_timestamp_valid = frame.source_timestamp_valid;
        event.source_time_at_steady_ns = frame.source_time_at_steady_ns;
        event.source_time_basis = frame.source_time_basis;
        event.source_clock_status = frame.source_clock_status;
        event.source_clock_session_id = frame.source_clock_session_id;
        event.source_clock_uncertainty_ms =
            frame.source_clock_uncertainty_ms;
        event.source_clock_rtt_ms = frame.source_clock_rtt_ms;
        event.source_clock_mapping_age_ms =
            frame.source_clock_mapping_age_ms;
        event.source_clock_sample_count =
            frame.source_clock_sample_count;
        event.source_dropped_frames = frame.source_dropped_frames;
        event.transport_dropped_frames =
            frame.transport_dropped_frames;
        event.transport_invalid_packets =
            frame.transport_invalid_packets;
        event.scheduled_at_steady_ns = steady_nanoseconds(
            std::chrono::steady_clock::now());
        event.nominal_dx_counts = sample.dx_counts;
        event.nominal_dy_counts = sample.dy_counts;
        event.safety_allowed = frame.safety_allowed;
        event.mouse_status = impl_->mouse
            ? impl_->mouse->status() : MouseStatus::CLOSED;

        impl_->last_source_frame_sequence = frame.source_frame_sequence;
        impl_->last_source_timestamp = frame.source_timestamp;
        ++impl_->result.consumed_sample_count;
        if (impl_->options.dispatch_mode ==
                ProbeDispatchMode::OUTPUT_OFF_REHEARSAL ||
            (sample.dx_counts == 0 && sample.dy_counts == 0)) {
            event.cumulative_requested_x_counts =
                impl_->result.cumulative_requested_x_counts;
            event.cumulative_backend_completed_x_counts =
                impl_->result.cumulative_backend_completed_x_counts;
            impl_->result.events.push_back(std::move(event));
            if (impl_->result.consumed_sample_count ==
                impl_->sequence.samples.size()) {
                impl_->complete();
            }
            error.clear();
            return true;
        }

        event.dispatch_attempted = true;
        event.requested_dx_counts = sample.dx_counts;
        event.requested_dy_counts = sample.dy_counts;
        impl_->result.cumulative_requested_x_counts += sample.dx_counts;
        event.issued_at_steady_ns = steady_nanoseconds(
            std::chrono::steady_clock::now());
        const MouseMoveReceipt receipt = impl_->mouse->move(
            {sample.dx_counts, sample.dy_counts});
        event.returned_at_steady_ns = steady_nanoseconds(
            std::chrono::steady_clock::now());
        event.mouse_status = impl_->mouse->status();

        const bool valid_backend_completion = receipt.succeeded &&
            receipt.backend_completed_at !=
                std::chrono::steady_clock::time_point{} &&
            steady_nanoseconds(receipt.backend_completed_at) >=
                event.issued_at_steady_ns &&
            steady_nanoseconds(receipt.backend_completed_at) <=
                event.returned_at_steady_ns;
        if (valid_backend_completion) {
            event.backend_succeeded = true;
            event.backend_completed_at_steady_ns =
                steady_nanoseconds(receipt.backend_completed_at);
            impl_->result.cumulative_backend_completed_x_counts +=
                sample.dx_counts;
        }
        event.protocol_ack_received = receipt.protocol_ack_received;
        if (receipt.protocol_ack_received_at !=
                std::chrono::steady_clock::time_point{}) {
            event.protocol_ack_received_at_steady_ns =
                steady_nanoseconds(receipt.protocol_ack_received_at);
        }

        ProbeStopReason failure = ProbeStopReason::NONE;
        if (!valid_backend_completion) {
            failure = ProbeStopReason::MOUSE_FAILURE;
        } else if (impl_->options.require_protocol_ack &&
                   (!receipt.protocol_ack_received ||
                    event.protocol_ack_received_at_steady_ns <
                        event.issued_at_steady_ns ||
                    event.protocol_ack_received_at_steady_ns >
                        event.backend_completed_at_steady_ns)) {
            failure = ProbeStopReason::PROTOCOL_ACK_MISSING;
        }
        event.cumulative_requested_x_counts =
            impl_->result.cumulative_requested_x_counts;
        event.cumulative_backend_completed_x_counts =
            impl_->result.cumulative_backend_completed_x_counts;
        event.stop_reason = failure;
        impl_->result.events.push_back(std::move(event));
        if (failure != ProbeStopReason::NONE) {
            impl_->stop(failure);
            set_error(error, failure == ProbeStopReason::MOUSE_FAILURE
                ? "Mouse move 或 completion 失败，probe 立即停发且不补偿"
                : "Mouse protocol ACK 无效，probe 立即停发且不补偿");
            return false;
        }
        if (impl_->result.consumed_sample_count ==
            impl_->sequence.samples.size()) {
            impl_->complete();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        impl_->stop(ProbeStopReason::MOUSE_FAILURE);
        set_error(error, std::string("消费 source frame 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        impl_->stop(ProbeStopReason::MOUSE_FAILURE);
        set_error(error, "消费 source frame 时发生未知异常");
        return false;
    }
}

void MouseEffectProbeExecutor::request_stop() noexcept {
    std::string ignored;
    request_stop(ProbeStopReason::USER_STOP, ignored);
}

bool MouseEffectProbeExecutor::request_stop(
        ProbeStopReason reason,
        std::string& error) noexcept {
    if (!impl_ || impl_->result.state != ProbeExecutionState::RUNNING) {
        set_error(error, "实际效果 probe 未处于 RUNNING，无法停止");
        return false;
    }
    switch (reason) {
        case ProbeStopReason::SOURCE_TIMING_INVALID:
        case ProbeStopReason::SOURCE_SESSION_CHANGED:
        case ProbeStopReason::SOURCE_FRAME_GAP:
        case ProbeStopReason::SIDECAR_UNAVAILABLE:
        case ProbeStopReason::SAFETY_RELEASED:
        case ProbeStopReason::MOUSE_FAILURE:
        case ProbeStopReason::PROTOCOL_ACK_MISSING:
        case ProbeStopReason::RUN_TIMEOUT:
        case ProbeStopReason::USER_STOP:
            impl_->stop(reason);
            error.clear();
            return true;
        case ProbeStopReason::NONE:
        case ProbeStopReason::NORMAL_COMPLETION:
        case ProbeStopReason::AUTHORIZATION_MISSING:
        case ProbeStopReason::EXCLUSIVE_OWNER_MISSING:
            set_error(error, "外部停止原因不允许伪造启动或正常完成状态");
            return false;
    }
    set_error(error, "未知外部停止原因");
    return false;
}

const ProbeExecutionResult& MouseEffectProbeExecutor::result() const noexcept {
    static const ProbeExecutionResult empty;
    return impl_ ? impl_->result : empty;
}

namespace {

bool valid_sha256_text(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool validate_report_inputs(
        const ProbeExecutionOptions& options,
        const MouseEffectProbeSequence& sequence,
        const ProbeEvidenceBinding& binding,
        const ProbeExecutionResult& result,
        std::string& error) {
    if (!valid_run_uuid(options.run_uuid) ||
        options.activation_epoch == 0 ||
        !validate_mouse_effect_probe_sequence(sequence, error)) {
        if (error.empty()) set_error(error, "probe report 运行或序列身份非法");
        return false;
    }
    if (!valid_sha256_text(binding.probe_binding_sha256) ||
        binding.sidecar_run_uuid != options.run_uuid ||
        binding.capture_source_name.empty() ||
        binding.capture_source_name.size() > 4096U) {
        set_error(error, "probe report 与 sidecar/source binding 身份非法");
        return false;
    }
    if (result.dispatch_mode != options.dispatch_mode ||
        (result.state != ProbeExecutionState::COMPLETED &&
         result.state != ProbeExecutionState::STOPPED) ||
        result.stop_reason == ProbeStopReason::NONE ||
        result.events.size() != result.consumed_sample_count ||
        result.consumed_sample_count > sequence.samples.size()) {
        set_error(error, "probe report 只能记录一致的终态执行结果");
        return false;
    }
    if ((result.state == ProbeExecutionState::COMPLETED) != result.complete ||
        (result.complete &&
         (result.stop_reason != ProbeStopReason::NORMAL_COMPLETION ||
          result.consumed_sample_count != sequence.samples.size())) ||
        (!result.complete &&
         result.stop_reason == ProbeStopReason::NORMAL_COMPLETION)) {
        set_error(error, "probe report complete/state/stop reason 不一致");
        return false;
    }

    std::int64_t requested_x = 0;
    std::int64_t backend_completed_x = 0;
    std::optional<std::uint64_t> previous_source_frame;
    std::optional<std::int64_t> previous_source_timestamp;
    std::string source_clock_session_id;
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        const auto& event = result.events[index];
        const auto& sample = sequence.samples[index];
        if (event.run_uuid != options.run_uuid ||
            event.activation_epoch != options.activation_epoch ||
            event.sequence_sha256 != sequence.sequence_sha256 ||
            event.sample_index != sample.sample_index ||
            event.block_id != sample.block_id ||
            event.nominal_dx_counts != sample.dx_counts ||
            event.nominal_dy_counts != sample.dy_counts ||
            !event.source_timestamp_valid ||
            event.source_timestamp <= 0 ||
            event.source_time_basis != "NDI_SDK_SUBMISSION" ||
            event.source_clock_status != "VALID" ||
            event.source_time_at_steady_ns <= 0 ||
            event.source_clock_session_id.empty() ||
            !std::isfinite(event.source_clock_uncertainty_ms) ||
            event.source_clock_uncertainty_ms < 0.0 ||
            !std::isfinite(event.source_clock_rtt_ms) ||
            event.source_clock_rtt_ms < 0.0 ||
            !std::isfinite(event.source_clock_mapping_age_ms) ||
            event.source_clock_mapping_age_ms < 0.0 ||
            event.source_clock_sample_count == 0 ||
            event.source_dropped_frames != 0 ||
            event.transport_dropped_frames != 0 ||
            event.transport_invalid_packets != 0 ||
            event.scheduled_at_steady_ns <= 0) {
            set_error(error, "probe report event 与序列/source timing 不一致");
            return false;
        }
        if (source_clock_session_id.empty()) {
            source_clock_session_id = event.source_clock_session_id;
        } else if (event.source_clock_session_id != source_clock_session_id) {
            set_error(error, "probe report event source clock session 改变");
            return false;
        }
        if (previous_source_frame.has_value() &&
            (*previous_source_frame ==
                 std::numeric_limits<std::uint64_t>::max() ||
             event.source_frame_sequence != *previous_source_frame + 1U)) {
            set_error(error, "probe report event source frame 不连续");
            return false;
        }
        previous_source_frame = event.source_frame_sequence;
        if (previous_source_timestamp.has_value() &&
            event.source_timestamp <= *previous_source_timestamp) {
            set_error(error,
                "probe report event source timestamp 未严格递增");
            return false;
        }
        previous_source_timestamp = event.source_timestamp;

        const bool nominal_zero = sample.dx_counts == 0 &&
                                  sample.dy_counts == 0;
        if (options.dispatch_mode ==
                ProbeDispatchMode::OUTPUT_OFF_REHEARSAL) {
            if (event.dispatch_attempted ||
                event.requested_dx_counts != 0 ||
                event.requested_dy_counts != 0 ||
                event.backend_succeeded ||
                event.protocol_ack_received ||
                event.issued_at_steady_ns != 0 ||
                event.backend_completed_at_steady_ns != 0 ||
                event.protocol_ack_received_at_steady_ns != 0 ||
                event.returned_at_steady_ns != 0) {
                set_error(error,
                    "output-off report event 不得包含实际 Mouse 请求或回执");
                return false;
            }
        } else if (nominal_zero) {
            if (event.dispatch_attempted ||
                event.requested_dx_counts != 0 ||
                event.requested_dy_counts != 0 ||
                event.backend_succeeded ||
                event.protocol_ack_received ||
                event.issued_at_steady_ns != 0 ||
                event.backend_completed_at_steady_ns != 0 ||
                event.protocol_ack_received_at_steady_ns != 0 ||
                event.returned_at_steady_ns != 0) {
                set_error(error, "零 sample 不得伪造成已 dispatch 命令");
                return false;
            }
        } else {
            if (!event.safety_allowed || !event.dispatch_attempted ||
                event.requested_dx_counts != sample.dx_counts ||
                event.requested_dy_counts != sample.dy_counts ||
                event.issued_at_steady_ns < event.scheduled_at_steady_ns ||
                event.returned_at_steady_ns < event.issued_at_steady_ns) {
                set_error(error, "physical report pulse 的请求或时间线非法");
                return false;
            }
            requested_x += event.requested_dx_counts;
            if (event.backend_succeeded) {
                if (event.backend_completed_at_steady_ns <
                        event.issued_at_steady_ns ||
                    event.backend_completed_at_steady_ns >
                        event.returned_at_steady_ns) {
                    set_error(error,
                        "physical report backend completion 时间非法");
                    return false;
                }
                backend_completed_x += event.requested_dx_counts;
            } else if (event.backend_completed_at_steady_ns != 0) {
                set_error(error,
                    "失败 backend 不得携带有效 completion 时间");
                return false;
            }
            if (event.protocol_ack_received) {
                if (event.protocol_ack_received_at_steady_ns <
                        event.issued_at_steady_ns ||
                    !event.backend_succeeded ||
                    event.protocol_ack_received_at_steady_ns >
                        event.backend_completed_at_steady_ns) {
                    set_error(error, "physical report ACK 时间非法");
                    return false;
                }
            } else if (event.protocol_ack_received_at_steady_ns != 0) {
                set_error(error, "缺失 ACK 不得携带 ACK 时间");
                return false;
            }
        }
        if (event.cumulative_requested_x_counts != requested_x ||
            event.cumulative_backend_completed_x_counts !=
                backend_completed_x) {
            set_error(error, "probe report event 累计输入不守恒");
            return false;
        }
        if (event.stop_reason != ProbeStopReason::NONE &&
            (index + 1U != result.events.size() ||
             event.stop_reason != result.stop_reason)) {
            set_error(error, "probe report event stop reason 位置非法");
            return false;
        }
    }
    if (requested_x != result.cumulative_requested_x_counts ||
        backend_completed_x !=
            result.cumulative_backend_completed_x_counts) {
        set_error(error, "probe report 结果累计输入不守恒");
        return false;
    }
    if (result.complete &&
        (requested_x != sequence.net_x_counts ||
         backend_completed_x != sequence.net_x_counts)) {
        set_error(error, "正常完成的 probe report 必须保持请求/完成净零");
        return false;
    }
    error.clear();
    return true;
}

nlohmann::ordered_json report_event_json(const ProbeCommandEvent& event) {
    return {
        {"run_uuid", event.run_uuid},
        {"activation_epoch", event.activation_epoch},
        {"block_id", event.block_id},
        {"sequence_sha256", event.sequence_sha256},
        {"sample_index", event.sample_index},
        {"source_frame_sequence", event.source_frame_sequence},
        {"source_timestamp", event.source_timestamp},
        {"source_timestamp_valid", event.source_timestamp_valid},
        {"source_time_at_steady_ns", event.source_time_at_steady_ns},
        {"source_time_basis", event.source_time_basis},
        {"source_clock_status", event.source_clock_status},
        {"source_clock_session_id", event.source_clock_session_id},
        {"source_clock_uncertainty_ms",
         event.source_clock_uncertainty_ms},
        {"source_clock_rtt_ms", event.source_clock_rtt_ms},
        {"source_clock_mapping_age_ms",
         event.source_clock_mapping_age_ms},
        {"source_clock_sample_count", event.source_clock_sample_count},
        {"source_dropped_frames", event.source_dropped_frames},
        {"transport_dropped_frames", event.transport_dropped_frames},
        {"transport_invalid_packets", event.transport_invalid_packets},
        {"scheduled_at_steady_ns", event.scheduled_at_steady_ns},
        {"issued_at_steady_ns", event.issued_at_steady_ns},
        {"nominal_dx_counts", event.nominal_dx_counts},
        {"nominal_dy_counts", event.nominal_dy_counts},
        {"requested_dx_counts", event.requested_dx_counts},
        {"requested_dy_counts", event.requested_dy_counts},
        {"safety_allowed", event.safety_allowed},
        {"dispatch_attempted", event.dispatch_attempted},
        {"backend_succeeded", event.backend_succeeded},
        {"backend_completed_at_steady_ns",
         event.backend_completed_at_steady_ns},
        {"protocol_ack_received", event.protocol_ack_received},
        {"protocol_ack_received_at_steady_ns",
         event.protocol_ack_received_at_steady_ns},
        {"returned_at_steady_ns", event.returned_at_steady_ns},
        {"mouse_status", MouseStatusName(event.mouse_status)},
        {"cumulative_requested_x_counts",
         event.cumulative_requested_x_counts},
        {"cumulative_backend_completed_x_counts",
         event.cumulative_backend_completed_x_counts},
        {"stop_reason", probe_stop_reason_name(event.stop_reason)},
    };
}

nlohmann::ordered_json report_payload(
        const ProbeExecutionOptions& options,
        const MouseEffectProbeSequence& sequence,
        const ProbeEvidenceBinding& binding,
        const ProbeExecutionResult& result) {
    nlohmann::ordered_json events = nlohmann::ordered_json::array();
    for (const auto& event : result.events) {
        events.push_back(report_event_json(event));
    }
    return {
        {"schema", kReportSchema},
        {"evidence_type", kReportEvidenceType},
        {"profile", sequence.profile},
        {"run_uuid", options.run_uuid},
        {"activation_epoch", options.activation_epoch},
        {"dispatch_mode", probe_dispatch_mode_name(options.dispatch_mode)},
        {"sequence_sha256", sequence.sequence_sha256},
        {"binding", {
            {"probe_binding_sha256", binding.probe_binding_sha256},
            {"sidecar_run_uuid", binding.sidecar_run_uuid},
            {"capture_source_name", binding.capture_source_name},
        }},
        {"executor_timebase", {
            {"name", "steady_clock_nanoseconds_since_epoch"},
            {"ticks_per_second", 1'000'000'000ULL},
        }},
        {"result", {
            {"state", probe_execution_state_name(result.state)},
            {"stop_reason", probe_stop_reason_name(result.stop_reason)},
            {"complete", result.complete},
            {"consumed_sample_count", result.consumed_sample_count},
            {"cumulative_requested_x_counts",
             result.cumulative_requested_x_counts},
            {"cumulative_backend_completed_x_counts",
             result.cumulative_backend_completed_x_counts},
            {"events", std::move(events)},
        }},
    };
}

bool valid_report_document(const nlohmann::ordered_json& document,
                           std::string& error) {
    if (!has_exact_keys(document,
            {"schema", "evidence_type", "profile", "run_uuid",
             "activation_epoch", "dispatch_mode", "sequence_sha256",
             "binding", "executor_timebase", "result",
             "report_sha256"}) ||
        !document.at("schema").is_number_unsigned() ||
        document.at("schema").get<std::uint64_t>() != kReportSchema ||
        !document.at("evidence_type").is_string() ||
        document.at("evidence_type").get<std::string>() !=
            kReportEvidenceType ||
        !document.at("profile").is_string() ||
        !document.at("run_uuid").is_string() ||
        !document.at("activation_epoch").is_number_unsigned() ||
        !document.at("dispatch_mode").is_string() ||
        !document.at("sequence_sha256").is_string() ||
        !document.at("report_sha256").is_string()) {
        set_error(error, "probe report 根对象字段集合或类型非法");
        return false;
    }
    const auto& binding = document.at("binding");
    const auto& timebase = document.at("executor_timebase");
    const auto& result = document.at("result");
    if (!has_exact_keys(binding,
            {"probe_binding_sha256", "sidecar_run_uuid",
             "capture_source_name"}) ||
        !binding.at("probe_binding_sha256").is_string() ||
        !binding.at("sidecar_run_uuid").is_string() ||
        !binding.at("capture_source_name").is_string() ||
        !has_exact_keys(timebase, {"name", "ticks_per_second"}) ||
        !timebase.at("name").is_string() ||
        timebase.at("name").get<std::string>() !=
            "steady_clock_nanoseconds_since_epoch" ||
        !timebase.at("ticks_per_second").is_number_unsigned() ||
        timebase.at("ticks_per_second").get<std::uint64_t>() !=
            1'000'000'000ULL ||
        !has_exact_keys(result,
            {"state", "stop_reason", "complete",
             "consumed_sample_count", "cumulative_requested_x_counts",
             "cumulative_backend_completed_x_counts", "events"}) ||
        !result.at("state").is_string() ||
        !result.at("stop_reason").is_string() ||
        !result.at("complete").is_boolean() ||
        !result.at("consumed_sample_count").is_number_unsigned() ||
        !result.at("events").is_array()) {
        set_error(error, "probe report binding/timebase/result schema 非法");
        return false;
    }
    const auto stored_hash = document.at("report_sha256").get<std::string>();
    if (!valid_sha256_text(stored_hash)) {
        set_error(error, "probe report SHA-256 格式非法");
        return false;
    }
    auto payload = document;
    payload.erase("report_sha256");
    std::string computed_hash;
    if (!sha256(payload.dump(), computed_hash, error)) return false;
    if (computed_hash != stored_hash) {
        set_error(error, "probe report 规范化语义 SHA-256 不匹配");
        return false;
    }
    error.clear();
    return true;
}

} // namespace

bool write_mouse_effect_probe_report(
        const std::filesystem::path& path,
        const ProbeExecutionOptions& options,
        const MouseEffectProbeSequence& sequence,
        const ProbeEvidenceBinding& binding,
        const ProbeExecutionResult& result,
        std::string& report_sha256,
        std::string& error) noexcept {
    report_sha256.clear();
    std::filesystem::path temporary_path;
    try {
        if (!validate_report_inputs(
                options, sequence, binding, result, error)) {
            return false;
        }
        if (path.empty()) {
            set_error(error, "probe report 发布路径不能为空");
            return false;
        }
        const auto final_path = std::filesystem::absolute(path);
        if (std::filesystem::exists(final_path)) {
            set_error(error, "probe report 发布目标已存在，拒绝覆盖");
            return false;
        }
        const auto parent = final_path.parent_path();
        std::error_code directory_error;
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error ||
            (!parent.empty() && !std::filesystem::is_directory(parent))) {
            set_error(error, "probe report 发布目录创建失败");
            return false;
        }
        auto document = report_payload(options, sequence, binding, result);
        if (!sha256(document.dump(), report_sha256, error)) return false;
        document["report_sha256"] = report_sha256;
        const std::string content = document.dump(2) + '\n';
        if (content.size() > kMaximumReportFileBytes) {
            report_sha256.clear();
            set_error(error, "probe report 超过固定文件容量边界");
            return false;
        }
        temporary_path = final_path;
        temporary_path += L".pending-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        if (std::filesystem::exists(temporary_path)) {
            report_sha256.clear();
            set_error(error, "probe report 临时发布目标已存在，拒绝覆盖");
            temporary_path.clear();
            return false;
        }
        std::ofstream output(
            temporary_path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(
            content.size()));
        output.flush();
        const bool written = output.good();
        output.close();
        if (!written) {
            report_sha256.clear();
            set_error(error, "probe report 临时文件写入失败");
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            const auto win32_error = GetLastError();
            report_sha256.clear();
            set_error(error, "probe report 原子发布失败，Win32Error=" +
                             std::to_string(win32_error));
            std::error_code ignored;
            std::filesystem::remove(temporary_path, ignored);
            temporary_path.clear();
            return false;
        }
        temporary_path.clear();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        report_sha256.clear();
        set_error(error, std::string("发布 probe report 异常: ") +
                         exception.what());
    } catch (...) {
        report_sha256.clear();
        set_error(error, "发布 probe report 时发生未知异常");
    }
    if (!temporary_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    }
    return false;
}

bool verify_mouse_effect_probe_report(
        const std::filesystem::path& path,
        std::string& error) noexcept {
    try {
        std::error_code filesystem_error;
        if (path.empty() || !std::filesystem::is_regular_file(
                path, filesystem_error) || filesystem_error) {
            set_error(error, "probe report 不是可读普通文件");
            return false;
        }
        const auto bytes = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error || bytes == 0 ||
            bytes > kMaximumReportFileBytes) {
            set_error(error, "probe report 为空或超过固定文件容量边界");
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开 probe report");
            return false;
        }
        const std::string content((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            set_error(error, "读取 probe report 失败");
            return false;
        }
        const auto document = nlohmann::ordered_json::parse(content);
        return valid_report_document(document, error);
    } catch (const std::exception& exception) {
        set_error(error, std::string("校验 probe report 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        set_error(error, "校验 probe report 时发生未知异常");
        return false;
    }
}

bool calculate_mouse_effect_probe_file_sha256(
        const std::filesystem::path& path,
        std::string& output,
        std::string& error) noexcept {
    output.clear();
    try {
        std::error_code filesystem_error;
        if (path.empty() || !std::filesystem::is_regular_file(
                path, filesystem_error) || filesystem_error) {
            set_error(error, "probe binding 不是可读普通文件");
            return false;
        }
        const auto bytes = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error || bytes == 0 ||
            bytes > kMaximumBindingFileBytes) {
            set_error(error, "probe binding 为空或超过固定文件容量边界");
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            set_error(error, "无法打开 probe binding 文件");
            return false;
        }
        const std::string content((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            set_error(error, "读取 probe binding 文件失败");
            return false;
        }
        return sha256(content, output, error);
    } catch (const std::exception& exception) {
        output.clear();
        set_error(error, std::string("计算 probe binding SHA 异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        output.clear();
        set_error(error, "计算 probe binding SHA 时发生未知异常");
        return false;
    }
}

const char* probe_execution_state_name(ProbeExecutionState state) noexcept {
    switch (state) {
        case ProbeExecutionState::IDLE: return "idle";
        case ProbeExecutionState::RUNNING: return "running";
        case ProbeExecutionState::COMPLETED: return "completed";
        case ProbeExecutionState::STOPPED: return "stopped";
    }
    return "unknown";
}

const char* probe_dispatch_mode_name(ProbeDispatchMode mode) noexcept {
    switch (mode) {
        case ProbeDispatchMode::OUTPUT_OFF_REHEARSAL:
            return "output_off_rehearsal";
        case ProbeDispatchMode::PHYSICAL_A: return "physical_a";
    }
    return "unknown";
}

const char* probe_stop_reason_name(ProbeStopReason reason) noexcept {
    switch (reason) {
        case ProbeStopReason::NONE: return "none";
        case ProbeStopReason::NORMAL_COMPLETION: return "normal_completion";
        case ProbeStopReason::AUTHORIZATION_MISSING:
            return "authorization_missing";
        case ProbeStopReason::EXCLUSIVE_OWNER_MISSING:
            return "exclusive_owner_missing";
        case ProbeStopReason::SOURCE_TIMING_INVALID:
            return "source_timing_invalid";
        case ProbeStopReason::SOURCE_SESSION_CHANGED:
            return "source_session_changed";
        case ProbeStopReason::SOURCE_FRAME_GAP: return "source_frame_gap";
        case ProbeStopReason::SIDECAR_UNAVAILABLE:
            return "sidecar_unavailable";
        case ProbeStopReason::SAFETY_RELEASED: return "safety_released";
        case ProbeStopReason::MOUSE_FAILURE: return "mouse_failure";
        case ProbeStopReason::PROTOCOL_ACK_MISSING:
            return "protocol_ack_missing";
        case ProbeStopReason::RUN_TIMEOUT: return "run_timeout";
        case ProbeStopReason::USER_STOP: return "user_stop";
    }
    return "unknown";
}

} // namespace mouse_effect_probe
