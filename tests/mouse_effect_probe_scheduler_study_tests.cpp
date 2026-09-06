#include "mouse_effect_probe_composite_seal/scheduler_study_internal.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {

using nlohmann::json;
namespace study = xen::scheduler_study::detail;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

template<class Function>
void expect_rejected(Function function, const std::string& message) {
    try {
        function();
        expect(false, message);
    } catch (const std::runtime_error&) {
    } catch (...) {
        expect(false, message + "：必须收敛为 runtime_error");
    }
}

json fixture() {
    const json context{{"process_id", 1234}, {"thread_id", 5678}, {"session_id", 1},
        {"process_priority_class", 32}, {"thread_priority", 0},
        {"session_id_query_error", nullptr}, {"process_priority_class_query_error", nullptr},
        {"thread_priority_query_error", nullptr}, {"sampled_before_preflight", true},
        {"computer_name", "fixture-host"}, {"executable_sha256", std::string(64, 'a')},
        {"process_creation_filetime", 134331674740349914ULL},
        {"qpc_frequency_hz", 10000000}, {"campaign_start_qpc", 900000},
        {"uptime_ms_before_campaign", 1000}, {"same_process_same_boot_required", true}};
    json document{{"schema_version", 1},
        {"evidence_type", "mouse_effect_probe_scheduler_study_characterization"},
        {"status", "COMPLETE"}, {"protocol", study::protocol()}, {"context", context},
        {"diagnostic_only", true}, {"physical_output_capability", false},
        {"physical_dispatch_count", 0}, {"formal_preflight_published", false},
        {"final_plan_published", false},
        {"blocks", json::array()}};
    const std::array<std::uint64_t, 3> guards{300000, 325000, 350000};
    for (std::size_t block = 0; block < 30; ++block) {
        const auto round = block / 3;
        const auto guard_index = (round + block % 3) % 3;
        const std::uint64_t anchor = 1000000U + block * 3000000U;
        json batch{{"schema_version", 1}, {"evidence_type", "mouse_effect_probe_scheduler_study_batch"},
            {"status", "COMPLETE"}, {"study_phase", "CHARACTERIZATION"},
            {"completed_means", "RAW_SAMPLE_COMPLETE_NOT_QUALITY_PASS"},
            {"study_stop_polling", true}, {"study_wait_timeout_ms", 1000},
            {"diagnostic_only", true}, {"instrumented", true}, {"timing_perturbed", true},
            {"timing_loop_json_bookkeeping", false}, {"normal_preflight_timing_loop_json_bookkeeping", true},
            {"additional_qpc_reads_per_completed_event", 2},
            {"physical_output_capability", false}, {"physical_dispatch_count", 0},
            {"formal_preflight_published", false}, {"final_plan_published", false},
            {"context", context}, {"clock_kind", "WINDOWS_QPC"},
            {"qpc_frequency_hz", 10000000}, {"anchor_qpc", anchor},
            {"timing_policy", study::batch_timing_policy(guards[guard_index])},
            {"reached_event_count", 42}, {"completed_event_count", 42},
            {"failure_event", nullptr}, {"failure_stage", nullptr}, {"failure_reason", ""},
            {"win32_failure", nullptr}, {"events", json::array()}};
        for (std::size_t index = 0; index < 42; ++index) {
            const auto deadline = anchor + (index + 1) * 50000;
            const auto coarse = deadline - guards[guard_index] / 100;
            const auto due_base = anchor + index * 50000 + 10;
            json event{{"event_ordinal", index}, {"completed", true}, {"last_stage", "COMPLETE"},
                {"active_total_before_ns", index * 10100U}, {"valid", json::object()},
                {"deadline_qpc", deadline}, {"coarse_target_qpc", coarse}, {"due_base_qpc", due_base},
                {"relative_due_100ns", -static_cast<std::int64_t>(coarse - due_base)},
                {"set_timer_before_qpc", due_base + 1}, {"set_timer_after_qpc", due_base + 2},
                {"set_timer_result", 1}, {"wait_result", 1},
                {"wait_return_qpc", deadline - 100}, {"active_enter_qpc", deadline - 100},
                {"active_last_qpc", deadline}, {"marker_before_qpc", deadline + 1},
                {"marker_after_qpc", deadline + 2}, {"active_read_count", 4},
                {"deadline_lateness_ns", 100}, {"marker_width_ns", 100}, {"active_wait_ns", 10100}};
            for (const auto* key : {"deadline_qpc", "coarse_target_qpc", "due_base_qpc", "relative_due_100ns",
                "set_timer_before_qpc", "set_timer_after_qpc", "set_timer_result", "wait_result",
                "wait_return_qpc", "active_enter_qpc", "active_last_qpc", "marker_before_qpc",
                "marker_after_qpc", "active_read_count", "deadline_lateness_ns", "marker_width_ns", "active_wait_ns"})
                event["valid"][key] = true;
            batch["events"].push_back(std::move(event));
        }
        document["blocks"].push_back({{"round_index", round}, {"guard_index", guard_index},
                                      {"diagnostic", std::move(batch)}});
    }
    return document;
}

void refresh_batch(json& batch) {
    std::uint64_t total = 0;
    auto previous = batch["anchor_qpc"].get<std::uint64_t>();
    for (auto& event : batch["events"]) {
        const auto due_base = std::max(event["due_base_qpc"].get<std::uint64_t>(), previous + 10);
        const auto coarse = event["coarse_target_qpc"].get<std::uint64_t>();
        event["due_base_qpc"] = due_base;
        event["set_timer_before_qpc"] = due_base + 1;
        event["set_timer_after_qpc"] = due_base + 2;
        event["relative_due_100ns"] = -static_cast<std::int64_t>(coarse - due_base);
        event["active_total_before_ns"] = total;
        total += event["active_wait_ns"].get<std::uint64_t>();
        previous = event["marker_after_qpc"].get<std::uint64_t>();
    }
}

void set_lateness(json& batch, std::size_t index, std::uint64_t lateness_ns) {
    auto& event = batch["events"][index];
    const auto observed = event["deadline_qpc"].get<std::uint64_t>() + lateness_ns / 100;
    event["wait_return_qpc"] = observed;
    event["active_enter_qpc"] = observed;
    event["active_last_qpc"] = observed;
    event["marker_before_qpc"] = observed;
    event["marker_after_qpc"] = observed + 1;
    event["active_read_count"] = 0;
    event["active_wait_ns"] = 0;
    event["deadline_lateness_ns"] = lateness_ns;
    refresh_batch(batch);
}

void test_classification() {
    using D = study::EventDisposition;
    expect(study::classify_event(176100, 100, 0, 82900) == D::QUALITY_FAILURE,
           "现场 176100ns 只触发质量失败，允许继续采样");
    expect(study::classify_event(150000, 100000, 350000, 14350000) == D::ACCEPT,
           "质量与硬上限恰好相等必须接纳");
    expect(study::classify_event(150001, 0, 0, 0) == D::QUALITY_FAILURE &&
           study::classify_event(0, 100001, 0, 0) == D::QUALITY_FAILURE,
           "质量边界采用严格大于");
    expect(study::classify_event(176100, 100001, 350001, 0) == D::ABORT &&
           study::classify_event(176100, 0, 1, 14700000) == D::ABORT &&
           study::classify_event(0, 0, std::numeric_limits<std::uint64_t>::max(), 0) == D::ABORT,
           "active 硬上限优先且不发生 unsigned 下溢");
}

void test_fixed_selection() {
    auto data = fixture();
    expect(study::select_candidate(data)["selected_guard_ns"] == 300000,
           "全部可行时选预注册最小 guard");
    set_lateness(data["blocks"][0]["diagnostic"], 4, 176100);
    const auto after_first_failure = study::select_candidate(data);
    expect(after_first_failure["selected_guard_ns"] == 325000 &&
           after_first_failure["guard_results"][0]["quality_failure_count"] == 1 &&
           after_first_failure["guard_results"][0]["sample_count"] == 420,
           "单个 176100ns 样本淘汰原 guard，保留完整尾部，不重复挑 PASS");
    set_lateness(data["blocks"][1]["diagnostic"], 41, 150100);
    expect(study::select_candidate(data)["selected_guard_ns"] == 350000,
           "后续 guard 任意尾部样本超限也参与决定");
    set_lateness(data["blocks"][2]["diagnostic"], 0, 176100);
    const auto none = study::select_candidate(data);
    expect(none["status"] == "NO_CANDIDATE" && none["selected_guard_ns"].is_null(),
           "全部 grid 值不可行时无候选，不夹取350000或改门槛");
    expect(study::select_candidate(json::parse(data.dump())) == none,
           "保存后重新解析得到同一纯决定，接受JSON整数符号存储差异");
}

void test_rejection() {
    const auto original = fixture();
    const auto check = [&](const char* name, auto mutate) {
        auto data = original;
        mutate(data);
        expect_rejected([&] { (void)study::select_candidate(data); }, name);
    };
    check("拒绝旧5事件/错误evidence type", [](auto& d) {
        d["evidence_type"] = "mouse_effect_probe_scheduler_phase_diagnostic";
    });
    check("拒绝不足固定批次", [](auto& d) { d["blocks"].erase(29); });
    check("拒绝不足固定事件", [](auto& d) { d["blocks"][0]["diagnostic"]["events"].erase(41); });
    check("拒绝原始QPC改1tick但派生值没变", [](auto& d) {
        auto& value = d["blocks"][0]["diagnostic"]["events"][4]["marker_before_qpc"];
        value = value.template get<std::uint64_t>() + 1;
    });
    check("拒绝改变relative due", [](auto& d) {
        d["blocks"][0]["diagnostic"]["events"][4]["relative_due_100ns"] = -1;
    });
    check("拒绝硬active超限，即便派生数值一致", [](auto& d) {
        auto& batch = d["blocks"][0]["diagnostic"];
        auto& e = batch["events"][4];
        const auto end = e["marker_before_qpc"].template get<std::uint64_t>();
        e["wait_return_qpc"] = end - 3501;
        e["active_enter_qpc"] = end - 3501;
        e["active_wait_ns"] = 350100;
        refresh_batch(batch);
    });
    check("拒绝累计active漂移", [](auto& d) {
        d["blocks"][0]["diagnostic"]["events"][4]["active_total_before_ns"] = 0;
    });
    check("拒绝身份漂移", [](auto& d) { d["blocks"][0]["diagnostic"]["context"]["process_id"] = 999; });
    check("拒绝顶层物理能力伪装", [](auto& d) { d["physical_output_capability"] = true; });
    check("拒绝顶层优先级偏离NORMAL", [](auto& d) { d["context"]["process_priority_class"] = 128; });
    check("拒绝空主机身份", [](auto& d) { d["context"]["computer_name"] = ""; });
    check("拒绝无效可执行hash", [](auto& d) { d["context"]["executable_sha256"] = "not-a-hash"; });
    check("拒绝进程创建时间无效", [](auto& d) { d["context"]["process_creation_filetime"] = 0; });
    check("拒绝把study真实Wait返回归一成旧0", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["wait_result"] = 0; });
    check("拒绝批次stop策略漂移", [](auto& d) { d["blocks"][0]["diagnostic"]["study_wait_timeout_ms"] = 2000; });
    check("拒绝协议漂移", [](auto& d) { d["protocol"]["max_wake_lateness_ns"] = 200000; });
    check("拒绝采样次序漂移", [](auto& d) { d["blocks"][4]["guard_index"] = 0; });
    check("拒绝guard计时参数漂移", [](auto& d) {
        d["blocks"][0]["diagnostic"]["timing_policy"]["active_guard_ns"] = 325000;
    });
    check("拒绝频率漂移", [](auto& d) { d["blocks"][1]["diagnostic"]["qpc_frequency_hz"] = 10000001; });
    check("拒绝原始字段bool", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["set_timer_result"] = true; });
    check("拒绝原始字段float", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["marker_width_ns"] = 100.0; });
    check("拒绝原始字段负数", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["due_base_qpc"] = -1; });
    check("拒绝原始字段超int64", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["deadline_qpc"] = std::numeric_limits<std::uint64_t>::max(); });
    check("拒绝超大frequency避免换算溢出", [](auto& d) { d["blocks"][0]["diagnostic"]["qpc_frequency_hz"] = std::numeric_limits<std::uint64_t>::max(); });
    check("拒绝protocol整数伪装float", [](auto& d) { d["protocol"]["round_count"] = 10.0; });
    check("拒绝已取得字段valid为false", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["valid"]["marker_after_qpc"] = false; });
    check("拒绝完整事件隐藏终止阶段", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0]["last_stage"] = "STUDY_STOP"; });
    check("拒绝完整事件缺少阶段", [](auto& d) { d["blocks"][0]["diagnostic"]["events"][0].erase("last_stage"); });
    check("拒绝anchor倒退", [](auto& d) { d["blocks"][1]["diagnostic"]["anchor_qpc"] = 1; });
    check("恰好总时限也须拒绝，包含首anchor前的经过时间", [](auto& d) {
        const auto last = d["blocks"].back()["diagnostic"]["events"].back()["marker_after_qpc"].template get<std::uint64_t>();
        const auto start = d["context"]["campaign_start_qpc"].template get<std::uint64_t>();
        const std::uint64_t offset = 300000000 - (last - start);
        for (auto& block : d["blocks"]) {
            auto& batch = block["diagnostic"];
            batch["anchor_qpc"] = batch["anchor_qpc"].template get<std::uint64_t>() + offset;
            for (auto& event : batch["events"])
                for (const auto* key : {"deadline_qpc", "coarse_target_qpc", "due_base_qpc",
                    "set_timer_before_qpc", "set_timer_after_qpc", "wait_return_qpc", "active_enter_qpc",
                    "active_last_qpc", "marker_before_qpc", "marker_after_qpc"})
                    event[key] = event[key].template get<std::uint64_t>() + offset;
        }
    });
    check("拒绝拿validation反向选择", [](auto& d) { d["blocks"][0]["diagnostic"]["study_phase"] = "VALIDATION"; });
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--characterization") {
            const std::filesystem::path path(argv[2]);
            if (!path.is_absolute() || !std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) > 16U * 1024U * 1024U)
                throw std::runtime_error("离线 characterization 必须是不超过16MiB的绝对文件路径");
            std::ifstream input(path, std::ios::binary);
            const auto document = json::parse(input);
            std::cout << study::select_candidate(document).dump(2) << '\n';
            return 0;
        }
        if (argc != 1) throw std::runtime_error("仅接受 --characterization <absolute-json>");
        test_classification();
        test_fixed_selection();
        test_rejection();
        if (failures != 0) {
            std::cerr << "scheduler study 离线测试失败数: " << failures << '\n';
            return 1;
        }
        std::cout << "scheduler study 决定逻辑与原始证据离线测试全部通过。\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scheduler study 拒绝: " << error.what() << '\n';
        return 2;
    }
}
