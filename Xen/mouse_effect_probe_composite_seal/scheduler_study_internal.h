#ifndef XEN_MOUSE_EFFECT_PROBE_SCHEDULER_STUDY_INTERNAL_H
#define XEN_MOUSE_EFFECT_PROBE_SCHEDULER_STUDY_INTERNAL_H

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xen::scheduler_study::detail {

inline nlohmann::json protocol() {
    return {{"schema_version", 1}, {"guard_grid_ns", {300000, 325000, 350000}},
            {"round_count", 10}, {"validation_batch_count", 10},
            {"event_count", 42}, {"interval_ns", 5000000},
            {"max_wake_lateness_ns", 150000},
            {"max_event_interval_width_ns", 100000},
            {"max_active_wait_ns_per_event", 350000},
            {"max_active_wait_ns_total", 14700000},
            {"campaign_timeout_ns", 30000000000ULL},
            {"wait_mode", "WAIT_FOR_MULTIPLE_OBJECTS_STOP_FIRST"},
            {"wait_timeout_ms", 1000}, {"active_stop_poll_per_iteration", true},
            {"campaign_max_batches", 40}, {"campaign_max_active_wait_ns", 588000000},
            {"round_order", "guard_index=(round_index+slot)%3"},
            {"warmup_excluded", false}, {"characterization_quality_tail_retained", true},
            {"validation_stop_on_first_failure", true},
            {"validation_not_used_for_selection", true},
            {"statistical_independence_claimed", false}};
}

enum class EventDisposition { ACCEPT, QUALITY_FAILURE, ABORT };

inline EventDisposition classify_event(std::uint64_t lateness,
        std::uint64_t width, std::uint64_t active,
        std::uint64_t total_before) noexcept {
    // 硬上限优先；先检查单事件，避免总额减法下溢。
    if (active > 350000U || total_before > 14700000U - active)
        return EventDisposition::ABORT;
    if (lateness > 150000U || width > 100000U)
        return EventDisposition::QUALITY_FAILURE;
    return EventDisposition::ACCEPT;
}

inline void require(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}

inline std::uint64_t unsigned_integer(const nlohmann::json& value) {
    require(value.is_number_integer(), "study 字段必须是整数，拒绝 bool/float");
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    const auto number = value.get<std::int64_t>();
    require(number >= 0, "study 无符号字段为负数");
    return static_cast<std::uint64_t>(number);
}

inline std::int64_t signed_integer(const nlohmann::json& value) {
    require(value.is_number_integer(), "study 字段必须是整数，拒绝 bool/float");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        require(number <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()), "study 有符号整数溢出");
        return static_cast<std::int64_t>(number);
    }
    return value.get<std::int64_t>();
}

inline bool strictly_equal(const nlohmann::json& expected,
                           const nlohmann::json& actual) {
    if (expected.is_number_integer())
        return actual.is_number_integer() && expected == actual;
    if (expected.type() != actual.type()) return false;
    if (expected.is_object()) {
        if (expected.size() != actual.size()) return false;
        for (auto it = expected.begin(); it != expected.end(); ++it)
            if (!actual.contains(it.key()) ||
                !strictly_equal(it.value(), actual.at(it.key()))) return false;
        return true;
    }
    if (expected.is_array()) {
        if (expected.size() != actual.size()) return false;
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (!strictly_equal(expected[index], actual[index])) return false;
        return true;
    }
    return expected == actual;
}

inline std::uint64_t checked_add(std::uint64_t first, std::uint64_t second) {
    require(first <= std::numeric_limits<std::uint64_t>::max() - second,
            "study 计数加法溢出");
    return first + second;
}

inline std::uint64_t checked_multiply(std::uint64_t first,
                                    std::uint64_t second) {
    require(second == 0 || first <= std::numeric_limits<std::uint64_t>::max() / second,
            "study 计数乘法溢出");
    return first * second;
}

inline std::uint64_t scale_ceil(std::uint64_t value, std::uint64_t scale,
                               std::uint64_t divisor) {
    require(divisor > 0, "study 换算分母无效");
    const auto whole = checked_multiply(value / divisor, scale);
    const auto remainder = checked_multiply(value % divisor, scale);
    return checked_add(whole, checked_add(remainder / divisor,
                                         remainder % divisor != 0 ? 1U : 0U));
}

inline nlohmann::json batch_timing_policy(std::uint64_t guard) {
    return {{"event_capacity", 42}, {"preflight_interval_ns", 5000000},
            {"active_guard_ns", guard}, {"max_wake_lateness_ns", 150000},
            {"max_event_interval_width_ns", 100000},
            {"max_active_wait_ns_per_event", 350000},
            {"max_active_wait_ns_total", 14700000},
            {"timer_mode", "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL"}};
}

inline void validate_context(const nlohmann::json& context) {
    require(context.is_object(), "study context 必须是对象");
    const auto process = unsigned_integer(context.at("process_id"));
    const auto thread = unsigned_integer(context.at("thread_id"));
    require(process > 0 && process <= 0xffffffffULL &&
            thread > 0 && thread <= 0xffffffffULL &&
            unsigned_integer(context.at("process_priority_class")) == 32 &&
            signed_integer(context.at("thread_priority")) == 0,
            "study 进程/线程/优先级身份无效");
    require(unsigned_integer(context.at("session_id")) <= 0xffffffffULL,
            "study session 超出实际类型范围");
    for (const auto* key : {"session_id_query_error", "process_priority_class_query_error",
                            "thread_priority_query_error"})
        require(context.at(key).is_null(), "study 身份查询未成功");
    require(context.at("sampled_before_preflight") == true,
            "study 身份必须在采样前记录");
    require(context.at("computer_name").is_string() &&
            !context.at("computer_name").get_ref<const std::string&>().empty(),
            "study 缺少主机身份");
    const auto& hash = context.at("executable_sha256");
    require(hash.is_string(), "study 可执行身份不是字符串");
    const auto& text = hash.get_ref<const std::string&>();
    require(text.size() == 64 && std::all_of(text.begin(), text.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    }), "study 可执行 SHA-256 格式无效");
    require(signed_integer(context.at("process_creation_filetime")) > 0 &&
            signed_integer(context.at("campaign_start_qpc")) > 0 &&
            signed_integer(context.at("uptime_ms_before_campaign")) >= 0 &&
            context.at("same_process_same_boot_required") == true,
            "study 进程创建/单进程生命周期身份无效");
    const auto frequency = unsigned_integer(context.at("qpc_frequency_hz"));
    require(frequency > 0 && frequency <= 1000000000,
            "study context QPC 频率无效");
}

inline void validate_output_boundary(const nlohmann::json& document) {
    require(document.at("diagnostic_only") == true &&
            document.at("physical_output_capability") == false &&
            unsigned_integer(document.at("physical_dispatch_count")) == 0 &&
            document.at("formal_preflight_published") == false &&
            document.at("final_plan_published") == false,
            "study 必须保持独立无物理输出诊断");
}

inline nlohmann::json select_candidate(const nlohmann::json& characterization) {
    try {
        require(unsigned_integer(characterization.at("schema_version")) == 1 &&
                characterization.at("evidence_type") ==
                    "mouse_effect_probe_scheduler_study_characterization" &&
                characterization.at("status") == "COMPLETE",
                "study characterization 类型/完成状态无效");
        require(strictly_equal(protocol(), characterization.at("protocol")),
                "study 冻结协议漂移");
        validate_output_boundary(characterization);
        const auto& context = characterization.at("context");
        validate_context(context);
        const auto& blocks = characterization.at("blocks");
        require(blocks.is_array() && blocks.size() == 30,
                "study 必须包含固定 30 块，拒绝缺样本或增补尝试");
        constexpr std::array<std::uint64_t, 3> guards{300000, 325000, 350000};
        std::array<std::uint64_t, 3> quality_failures{}, failed_batches{}, maxima{}, widths{};
        const auto frequency = unsigned_integer(context.at("qpc_frequency_hz"));
        const auto campaign_start = unsigned_integer(context.at("campaign_start_qpc"));
        std::uint64_t previous_end = campaign_start;
        for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
            const auto round = block_index / 3U;
            const auto guard_index = (round + block_index % 3U) % 3U;
            const auto& block = blocks[block_index];
            require(unsigned_integer(block.at("round_index")) == round &&
                    unsigned_integer(block.at("guard_index")) == guard_index,
                    "study 固定轮转次序漂移");
            const auto& batch = block.at("diagnostic");
            require(unsigned_integer(batch.at("schema_version")) == 1 &&
                    batch.at("evidence_type") == "mouse_effect_probe_scheduler_study_batch" &&
                    batch.at("status") == "COMPLETE" &&
                    batch.at("study_phase") == "CHARACTERIZATION",
                    "study 批次类型/阶段/完成状态无效");
            require(strictly_equal(context, batch.at("context")) &&
                    strictly_equal(batch_timing_policy(guards[guard_index]), batch.at("timing_policy")),
                    "study 批次身份或计时参数漂移");
            validate_output_boundary(batch);
            require(batch.at("instrumented") == true &&
                    batch.at("timing_perturbed") == true &&
                    batch.at("timing_loop_json_bookkeeping") == false &&
                    batch.at("normal_preflight_timing_loop_json_bookkeeping") == true &&
                    unsigned_integer(batch.at("additional_qpc_reads_per_completed_event")) == 2 &&
                    batch.at("clock_kind") == "WINDOWS_QPC" &&
                    batch.at("completed_means") == "RAW_SAMPLE_COMPLETE_NOT_QUALITY_PASS" &&
                    batch.at("study_stop_polling") == true &&
                    unsigned_integer(batch.at("study_wait_timeout_ms")) == 1000,
                    "study 采样/停止/完成语义漂移");
            require(batch.at("failure_event").is_null() && batch.at("failure_stage").is_null() &&
                    batch.at("failure_reason") == "" && batch.at("win32_failure").is_null(),
                    "study 完整批次不得隐藏终止错误");
            const auto batch_frequency = unsigned_integer(batch.at("qpc_frequency_hz"));
            const auto anchor = unsigned_integer(batch.at("anchor_qpc"));
            require(batch_frequency > 0 && batch_frequency <= 1000000000 &&
                    anchor > 0 && anchor > previous_end,
                    "study QPC 频率/anchor/跨批次时序无效");
            require(batch_frequency == frequency, "study QPC 频率漂移");
            const auto interval_ticks = scale_ceil(5000000, frequency, 1000000000);
            const auto guard_ticks = scale_ceil(guards[guard_index], frequency, 1000000000);
            const auto& events = batch.at("events");
            require(events.is_array() && events.size() == 42 &&
                    unsigned_integer(batch.at("reached_event_count")) == 42 &&
                    unsigned_integer(batch.at("completed_event_count")) == 42,
                    "study 每批必须完整记录 42 个事件");
            std::uint64_t total_active = 0, last_marker = anchor;
            bool batch_failed = false;
            for (std::size_t index = 0; index < events.size(); ++index) {
                const auto& event = events[index];
                require(unsigned_integer(event.at("event_ordinal")) == index &&
                        event.at("completed") == true && event.at("last_stage") == "COMPLETE",
                        "study ordinal 或采样完整性无效");
                const auto raw = [&](const char* key) {
                    require(event.at("valid").at(key) == true, "study 原始字段未取得");
                    const auto value = signed_integer(event.at(key));
                    require(value >= 0, "study 原始计数为负");
                    return static_cast<std::uint64_t>(value);
                };
                const auto deadline = raw("deadline_qpc");
                const auto coarse = raw("coarse_target_qpc");
                const auto due_base = raw("due_base_qpc");
                const auto before_set = raw("set_timer_before_qpc");
                const auto after_set = raw("set_timer_after_qpc");
                const auto entered = raw("wait_return_qpc");
                const auto active_enter = raw("active_enter_qpc");
                const auto active_last = raw("active_last_qpc");
                const auto marker_before = raw("marker_before_qpc");
                const auto marker_after = raw("marker_after_qpc");
                require(deadline == checked_add(anchor, checked_multiply(index + 1U, interval_ticks)) &&
                        deadline >= guard_ticks && coarse == deadline - guard_ticks,
                        "study absolute deadline/coarse 原始公式漂移");
                require(last_marker <= due_base && due_base < coarse && due_base <= before_set &&
                        before_set <= after_set && after_set <= entered && entered == active_enter &&
                        entered <= active_last && active_last <= marker_before && marker_before <= marker_after,
                        "study 同线程原始 QPC 顺序无效");
                require(event.at("valid").at("relative_due_100ns") == true,
                        "study relative due 字段未取得");
                const auto due_ns = scale_ceil(coarse - due_base, 1000000000, frequency);
                const auto units = due_ns / 100U + (due_ns % 100U ? 1U : 0U);
                require(units > 0 && units <= static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()) &&
                        signed_integer(event.at("relative_due_100ns")) == -static_cast<std::int64_t>(units),
                        "study relative due 双层向上取整漂移");
                require(raw("set_timer_result") == 1 && raw("wait_result") == 1,
                        "study Set/Wait API 未成功");
                const auto reads = raw("active_read_count");
                require((entered < deadline && reads > 0 && active_last >= deadline) ||
                        (entered >= deadline && reads == 0 && active_last == entered),
                        "study active loop 原始读数不一致");
                const auto lateness = marker_before > deadline
                    ? scale_ceil(marker_before - deadline, 1000000000, frequency) : 0U;
                const auto width = scale_ceil(marker_after - marker_before, 1000000000, frequency);
                const auto active = scale_ceil(marker_before - entered, 1000000000, frequency);
                require(raw("deadline_lateness_ns") == lateness && raw("marker_width_ns") == width &&
                        raw("active_wait_ns") == active &&
                        unsigned_integer(event.at("active_total_before_ns")) == total_active,
                        "study 派生指标与原始 QPC/累计 active 不一致");
                const auto disposition = classify_event(lateness, width, active, total_active);
                require(disposition != EventDisposition::ABORT, "study 证据触犯 active 硬上限");
                if (disposition == EventDisposition::QUALITY_FAILURE) {
                    ++quality_failures[guard_index];
                    batch_failed = true;
                }
                total_active = checked_add(total_active, active);
                maxima[guard_index] = std::max(maxima[guard_index], lateness);
                widths[guard_index] = std::max(widths[guard_index], width);
                last_marker = marker_after;
            }
            if (batch_failed) ++failed_batches[guard_index];
            previous_end = last_marker;
            require(scale_ceil(previous_end - campaign_start, 1000000000, frequency) < 30000000000ULL,
                    "study characterization 已超过 campaign 总时限");
        }
        auto results = nlohmann::json::array();
        nlohmann::json selected = nullptr;
        for (std::size_t index = 0; index < guards.size(); ++index) {
            const bool eligible = quality_failures[index] == 0;
            if (eligible && selected.is_null()) selected = guards[index];
            results.push_back({{"guard_ns", guards[index]}, {"batch_count", 10}, {"sample_count", 420},
                {"quality_failure_count", quality_failures[index]},
                {"quality_failed_batch_count", failed_batches[index]}, {"eligible", eligible},
                {"observed_max_lateness_ns", maxima[index]}, {"observed_max_marker_width_ns", widths[index]}});
        }
        return {{"status", selected.is_null() ? "NO_CANDIDATE" : "CANDIDATE_SELECTED"},
                {"selected_guard_ns", selected}, {"guard_results", std::move(results)}};
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("study JSON 契约无效: ") + error.what());
    }
}

} // namespace xen::scheduler_study::detail

#endif
