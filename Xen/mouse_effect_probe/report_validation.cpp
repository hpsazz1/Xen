#include "mouse_effect_probe/report_validation_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>

namespace mouse_effect_probe {
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
        case ProbeDispatchMode::PHYSICAL_B: return "physical_b";
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
        case ProbeStopReason::SCHEDULER_TIMING_INVALID:
            return "scheduler_timing_invalid";
        case ProbeStopReason::RUN_TIMEOUT: return "run_timeout";
        case ProbeStopReason::USER_STOP: return "user_stop";
    }
    return "unknown";
}


namespace detail {
void set_error(std::string& output, std::string_view value) noexcept {
    try {
        output.assign(value);
    } catch (...) {
    }
}
bool has_exact_keys(const nlohmann::ordered_json& value,
                    std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
        return value.contains(std::string(key));
    });
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

bool valid_sha256_text(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool validate_report_execution(
        const ProbeExecutionOptions& options,
        const MouseEffectProbeSequence& sequence,
        const ProbeExecutionResult& result,
        std::string& error) {
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
            !std::isfinite(event.source_clock_rate) ||
            event.source_clock_rate <= 0.0 ||
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
        // 实际executor在记录任一物理sample前检查安全门，零response也不能例外。
        if (options.dispatch_mode != ProbeDispatchMode::OUTPUT_OFF_REHEARSAL && !event.safety_allowed) {
            set_error(error, "physical report事件安全门未许可");
            return false;
        }
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

nlohmann::ordered_json report_event_json(const ProbeCommandEvent& event, const char* mouse_status, const char* stop_reason) {
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
        {"source_clock_rate", event.source_clock_rate},
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
        {"mouse_status", mouse_status},
        {"cumulative_requested_x_counts",
         event.cumulative_requested_x_counts},
        {"cumulative_backend_completed_x_counts",
         event.cumulative_backend_completed_x_counts},
        {"stop_reason", stop_reason},
    };
}

// JSON 正整数解析后会成为 unsigned；有符号累计值仍必须处于 int64 范围。
bool report_signed_integer(const nlohmann::ordered_json& value) {
    return value.is_number_integer() &&
        (!value.is_number_unsigned() || value.get<std::uint64_t>() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

bool valid_report_event(const nlohmann::ordered_json& event) {
    // 和生产 writer 共用唯一字段列表，类型约束由实际事件成员定义。
    static const auto schema = report_event_json(ProbeCommandEvent{}, "", "");
    if (!event.is_object() || event.size() != schema.size()) return false;
    for (const auto& field : schema.items()) {
        if (!event.contains(field.key())) return false;
        const auto& value = event.at(field.key());
        const auto& expected = field.value();
        if (expected.is_string()) {
            if (!value.is_string()) return false;
        } else if (expected.is_boolean()) {
            if (!value.is_boolean()) return false;
        } else if (expected.is_number_unsigned()) {
            if (!value.is_number_unsigned()) return false;
        } else if (expected.is_number_integer()) {
            if (!report_signed_integer(value)) return false;
        } else if (expected.is_number_float()) {
            if (!value.is_number() || !std::isfinite(value.get<double>())) return false;
        } else {
            return false;
        }
    }
    return true;
}

template<class Value> bool read_member(const nlohmann::ordered_json& value, Value& output) {
    if constexpr (std::is_integral_v<Value> && !std::is_same_v<Value, bool>) {
        if (value.is_number_unsigned()) {
            if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<Value>::max())) return false;
        } else {
            const auto number = value.get<std::int64_t>();
            if (number < std::numeric_limits<Value>::lowest() || number > std::numeric_limits<Value>::max()) return false;
        }
    }
    output = value.get<Value>();
    return true;
}

template<class Enum, class Name>
bool read_enum(const nlohmann::ordered_json& value, std::initializer_list<Enum> values, Name name, Enum& output) {
    for (const auto candidate : values) {
        if (value == name(candidate)) { output = candidate; return true; }
    }
    return false;
}
bool read_stop_reason(const nlohmann::ordered_json& value, ProbeStopReason& output) {
    return read_enum(value, {ProbeStopReason::NONE, ProbeStopReason::NORMAL_COMPLETION,
        ProbeStopReason::AUTHORIZATION_MISSING, ProbeStopReason::EXCLUSIVE_OWNER_MISSING,
        ProbeStopReason::SOURCE_TIMING_INVALID, ProbeStopReason::SOURCE_SESSION_CHANGED,
        ProbeStopReason::SOURCE_FRAME_GAP, ProbeStopReason::SIDECAR_UNAVAILABLE,
        ProbeStopReason::SAFETY_RELEASED, ProbeStopReason::MOUSE_FAILURE,
        ProbeStopReason::PROTOCOL_ACK_MISSING, ProbeStopReason::SCHEDULER_TIMING_INVALID,
        ProbeStopReason::RUN_TIMEOUT, ProbeStopReason::USER_STOP}, probe_stop_reason_name, output);
}
bool read_event(const nlohmann::ordered_json& value, ProbeCommandEvent& event) {
    // 字段集合/类型先由writer序列化结构验证；转换时再防止int计数被截断。
#define READ_MEMBER(member) if (!read_member(value.at(#member), event.member)) return false
    READ_MEMBER(run_uuid); READ_MEMBER(activation_epoch); READ_MEMBER(block_id);
    READ_MEMBER(sequence_sha256); READ_MEMBER(sample_index); READ_MEMBER(source_frame_sequence);
    READ_MEMBER(source_timestamp); READ_MEMBER(source_timestamp_valid); READ_MEMBER(source_time_at_steady_ns);
    READ_MEMBER(source_time_basis); READ_MEMBER(source_clock_status); READ_MEMBER(source_clock_session_id);
    READ_MEMBER(source_clock_uncertainty_ms); READ_MEMBER(source_clock_rtt_ms); READ_MEMBER(source_clock_rate);
    READ_MEMBER(source_clock_mapping_age_ms); READ_MEMBER(source_clock_sample_count); READ_MEMBER(source_dropped_frames);
    READ_MEMBER(transport_dropped_frames); READ_MEMBER(transport_invalid_packets); READ_MEMBER(scheduled_at_steady_ns);
    READ_MEMBER(issued_at_steady_ns); READ_MEMBER(nominal_dx_counts); READ_MEMBER(nominal_dy_counts);
    READ_MEMBER(requested_dx_counts); READ_MEMBER(requested_dy_counts); READ_MEMBER(safety_allowed);
    READ_MEMBER(dispatch_attempted); READ_MEMBER(backend_succeeded); READ_MEMBER(backend_completed_at_steady_ns);
    READ_MEMBER(protocol_ack_received); READ_MEMBER(protocol_ack_received_at_steady_ns); READ_MEMBER(returned_at_steady_ns);
    READ_MEMBER(cumulative_requested_x_counts); READ_MEMBER(cumulative_backend_completed_x_counts);
#undef READ_MEMBER
    return read_stop_reason(value.at("stop_reason"), event.stop_reason);
}
bool validate_decoded_report(const nlohmann::ordered_json& document, std::string& error) {
    ProbeExecutionOptions options;
    options.run_uuid = document.at("run_uuid").get<std::string>();
    options.activation_epoch = document.at("activation_epoch").get<std::uint64_t>();
    const auto& binding = document.at("binding");
    const auto source = binding.at("capture_source_name").get<std::string>();
    if (!valid_run_uuid(options.run_uuid) || !options.activation_epoch ||
        !valid_sha256_text(document.at("sequence_sha256").get<std::string>()) ||
        !valid_sha256_text(binding.at("probe_binding_sha256").get<std::string>()) ||
        binding.at("sidecar_run_uuid") != options.run_uuid || source.empty() || source.size() > 4096U ||
        !read_enum(document.at("dispatch_mode"), {ProbeDispatchMode::OUTPUT_OFF_REHEARSAL,
            ProbeDispatchMode::PHYSICAL_A, ProbeDispatchMode::PHYSICAL_B}, probe_dispatch_mode_name, options.dispatch_mode)) {
        set_error(error, "probe report运行、模式或来源绑定身份非法");
        return false;
    }
    const auto& raw_result = document.at("result");
    ProbeExecutionResult result;
    result.dispatch_mode = options.dispatch_mode;
    if (!read_enum(raw_result.at("state"), {ProbeExecutionState::COMPLETED, ProbeExecutionState::STOPPED},
            probe_execution_state_name, result.state) || !read_stop_reason(raw_result.at("stop_reason"), result.stop_reason)) {
        set_error(error, "probe report终态枚举非法");
        return false;
    }
    result.complete = raw_result.at("complete").get<bool>();
    result.consumed_sample_count = raw_result.at("consumed_sample_count").get<std::uint64_t>();
    result.cumulative_requested_x_counts = raw_result.at("cumulative_requested_x_counts").get<std::int64_t>();
    result.cumulative_backend_completed_x_counts = raw_result.at("cumulative_backend_completed_x_counts").get<std::int64_t>();
    MouseEffectProbeSequence nominal;
    nominal.sequence_sha256 = document.at("sequence_sha256").get<std::string>();
    for (const auto& value : raw_result.at("events")) {
        ProbeCommandEvent event;
        if (!read_event(value, event)) {
            set_error(error, "probe report事件计数超出类型范围或终态枚举非法");
            return false;
        }
        ProbeSequenceSample sample;
        sample.sample_index = nominal.samples.size();
        sample.block_id = event.block_id;
        sample.dx_counts = event.nominal_dx_counts;
        sample.dy_counts = event.nominal_dy_counts;
        nominal.samples.push_back(sample);
        result.events.push_back(std::move(event));
    }
    // 报告没有完整序列，这里只验证已记录事件的内在合同、连续性及完整终局净零。
    // 外部plan/sequence身份及每个nominal值仍需消费者绑定核验，不能由本地重建冒充。
    return validate_report_execution(options, nominal, result, error);
}
bool validate_report_structure_and_execution(const nlohmann::ordered_json& document,
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
        !report_signed_integer(result.at("cumulative_requested_x_counts")) ||
        !report_signed_integer(result.at("cumulative_backend_completed_x_counts")) ||
        !result.at("events").is_array() ||
        result.at("consumed_sample_count").get<std::uint64_t>() !=
            result.at("events").size() ||
        !std::all_of(result.at("events").begin(), result.at("events").end(),
                     valid_report_event)) {
        set_error(error, "probe report binding/timebase/result schema 非法");
        return false;
    }
    const auto stored_hash = document.at("report_sha256").get<std::string>();
    if (!valid_sha256_text(stored_hash)) {
        set_error(error, "probe report SHA-256 格式非法");
        return false;
    }
    return validate_decoded_report(document, error);
}

} // namespace detail
} // namespace mouse_effect_probe
