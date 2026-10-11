#include "mouse_effect_probe_runner/mouse_effect_probe_runner.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace plan_fixture {
std::filesystem::path plan_path;
std::string original_bytes;
std::string replacement_bytes;
bool replace_after_hash = false;
int replacements = 0;

void write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("夹具文件无法完整写入");
}

void replace() {
    if (!replace_after_hash) return;
    replace_after_hash = false;
    write(plan_path, replacement_bytes);
    ++replacements;
}

NTSTATUS hash_data(BCRYPT_HASH_HANDLE hash, PUCHAR bytes, ULONG count, ULONG flags) {
    const auto status = BCryptHashData(hash, bytes, count, flags);
    if (status >= 0 && replace_after_hash &&
        std::string_view(reinterpret_cast<const char*>(bytes), count) == original_bytes) {
        replace();
    }
    return status;
}
} // namespace plan_fixture

namespace mouse_effect_probe {
bool fixture_file_sha256(const std::filesystem::path& path,
                         std::string& hash, std::string& error) noexcept {
    const bool result = calculate_mouse_effect_probe_file_sha256(path, hash, error);
    if (result && path == plan_fixture::plan_path) {
        try { plan_fixture::replace(); }
        catch (...) { return false; }
    }
    return result;
}
} // namespace mouse_effect_probe

// 直接编译生产私有验证器，仅在哈希已消费字节后改变自有临时文件。
// 不调用 run_mouse_effect_probe、sidecar、Capture、Mouse 或任何输出入口。
#define BCryptHashData plan_fixture::hash_data
#define calculate_mouse_effect_probe_file_sha256 fixture_file_sha256
#include "../Xen/mouse_effect_probe_runner/mouse_effect_probe_runner.cpp"
#undef calculate_mouse_effect_probe_file_sha256
#undef BCryptHashData

namespace {
int plan_failures = 0;
void check(bool condition, std::string_view message) {
    if (condition) return;
    ++plan_failures;
    std::cerr << "[失败] " << message << '\n';
}

std::string hash_bytes(std::string_view bytes) {
    std::string hash;
    if (!sha256_payload(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), hash)) {
        throw std::runtime_error("夹具SHA计算失败");
    }
    return hash;
}

std::string seal(nlohmann::json document) {
    document.erase("plan_semantic_sha256");
    document["plan_semantic_sha256"] = hash_bytes(document.dump());
    return document.dump();
}

nlohmann::json make_plan(const MouseEffectProbeRunOptions& options,
                        const mouse_effect_probe::MouseEffectProbeSequence& sequence,
                        std::string_view sequence_file_sha) {
    std::vector<std::string> order;
    for (const auto& window : sequence.composite_phase_windows) order.push_back(window.window_id);
    const auto& request = sequence.composite_phase_request;
    return {
        {"schema_version", 1},
        {"evidence_type", "mouse_effect_probe_b_composite_phase_calibration_plan"},
        {"status", "FROZEN_BEFORE_CAPTURE"},
        {"run_uuid", options.run_uuid}, {"activation_epoch", options.activation_epoch},
        {"physical_output_capability", false}, {"physical_dispatch_count", 0},
        {"sequence_binding", {
            {"sequence_schema", 7}, {"sequence_profile", sequence.profile},
            {"sequence_semantic_sha256", sequence.sequence_sha256},
            {"sample_count", sequence.samples.size()},
            {"window_count", sequence.composite_phase_windows.size()},
            {"sequence_file_sha256", sequence_file_sha}, {"window_order", order}}},
        {"scheduler_policy", {
            {"clock_kind", "WINDOWS_QPC"}, {"timer_mode", request.timer_mode},
            {"deadline_basis", "PREDICTOR_NEXT_NDI_SUBMISSION_BOUNDARY"},
            {"issue_lead_ns", request.issue_lead_ns},
            {"issue_lead_applies_to", "NONZERO_PULSE_ONLY"},
            {"negative_control_marker_lead_ns", 0},
            {"target_tolerance_q32", request.target_tolerance_q32},
            {"active_guard_ns", request.active_guard_ns},
            {"max_wake_lateness_ns", request.max_wake_lateness_ns},
            {"max_event_interval_width_ns", request.max_event_interval_width_ns},
            {"max_active_wait_ns_per_event", request.max_active_wait_ns_per_event},
            {"max_active_wait_ns_total", request.max_active_wait_ns_total},
            {"preflight_required", true}, {"preflight_file_sha256", std::string(64, 'a')},
            {"per_event_tuning_allowed", false}, {"process_priority", "NORMAL"},
            {"thread_priority", "NORMAL"}, {"cpu_affinity_used", false},
            {"time_begin_period_used", false}, {"periodic_timer_used", false}}}};
}

void run_cases(const std::filesystem::path& root) {
    MouseEffectProbeRunOptions options;
    options.composite_plan_path = root / L"计划-\U0001F9EA.json";
    options.sequence_path = root / L"序列.json";
    options.run_uuid = "11111111-2222-4333-8444-555555555555";
    options.activation_epoch = 7;
    mouse_effect_probe::MouseEffectProbeSequence sequence;
    std::string error;
    if (!mouse_effect_probe::make_composite_phase_calibration_sequence(sequence, error) ||
        !mouse_effect_probe::write_mouse_effect_probe_sequence(options.sequence_path, sequence, error)) {
        throw std::runtime_error(error);
    }
    std::string sequence_sha;
    if (!mouse_effect_probe::calculate_mouse_effect_probe_file_sha256(
            options.sequence_path, sequence_sha, error)) throw std::runtime_error(error);
    const auto document = make_plan(options, sequence, sequence_sha);
    const auto valid = seal(document);
    auto invalid_document = document;
    invalid_document["physical_output_capability"] = true;
    const auto invalid = seal(invalid_document);
    plan_fixture::plan_path = options.composite_plan_path;

    const auto execute = [&](std::string bytes, bool expected, std::string_view label,
                             std::string replacement = {}, std::string expected_hash = {}) {
        plan_fixture::replace_after_hash = false;
        options.expected_composite_plan_sha256 = expected_hash.empty() ? hash_bytes(bytes) : expected_hash;
        plan_fixture::write(options.composite_plan_path, bytes);
        plan_fixture::original_bytes = bytes;
        plan_fixture::replacement_bytes = replacement;
        plan_fixture::replacements = 0;
        plan_fixture::replace_after_hash = !replacement.empty();
        std::string actual_sha = "stale";
        error.clear();
        const bool accepted = validate_composite_plan(options, sequence, actual_sha, error);
        plan_fixture::replace_after_hash = false;
        check(accepted == expected, label);
        check(!accepted || actual_sha == options.expected_composite_plan_sha256,
              "接纳计划必须返回同一文件快照的哈希");
        check(accepted || (actual_sha.empty() && !error.empty()),
              "拒绝计划必须清空旧身份并提供错误原因");
        if (!replacement.empty()) check(plan_fixture::replacements == 1, "必须实际完成一次哈希后文件替换");
        std::cout << label << ": accepted=" << accepted
                  << " replacements=" << plan_fixture::replacements << '\n';
    };
    execute(valid, true, "合法Unicode计划");
    execute(invalid, false, "物理输出能力头拒绝");
    execute(valid, true, "哈希后合法A被非法B替换仍验证A", invalid);
    execute(invalid, false, "哈希后非法A被合法B替换不得接纳B", valid);
    execute(valid, false, "读取前已换字节拒绝", {}, hash_bytes(invalid));
    execute("{", false, "JSON截断拒绝");
    auto semantic_drift = nlohmann::json::parse(valid);
    semantic_drift["plan_semantic_sha256"] = std::string(64, '0');
    execute(semantic_drift.dump(), false, "semantic漂移拒绝");
    auto binding_drift = document;
    binding_drift["sequence_binding"]["sequence_file_sha256"] = std::string(64, '0');
    execute(seal(binding_drift), false, "sequence文件绑定漂移拒绝");
    execute("", false, "空计划拒绝");
    execute(valid + std::string(1'048'576U - valid.size(), ' '), true, "恰好1MiB合法计划");
    execute(valid + std::string(1'048'577U - valid.size(), ' '), false, "超过1MiB拒绝");
    std::filesystem::remove(options.composite_plan_path);
    std::string hash;
    check(!validate_composite_plan(options, sequence, hash, error), "缺失文件拒绝");
    std::cout << "缺失文件拒绝: accepted=0\n";
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"xen-plan-snapshot-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("夹具目录创建失败");
        run_cases(root);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        ++plan_failures;
    }
    // root由本测试在系统临时目录下唯一创建，只清理自有文件。
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "计划快照测试失败数: " << plan_failures << '\n';
    return plan_failures == 0 ? 0 : 1;
}
