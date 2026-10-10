#ifndef XEN_PROBE_REPORT_VALIDATION_INTERNAL_H
#define XEN_PROBE_REPORT_VALIDATION_INTERNAL_H

// 纯数据合同：只依赖事件类型与JSON，不链接Mouse/Capture/Runtime实现。
#include "mouse_effect_probe/mouse_effect_probe.h"
#include <nlohmann/json.hpp>
#include <string_view>

namespace mouse_effect_probe::detail {
inline constexpr std::uint32_t kReportSchema = 1;
inline constexpr std::string_view kReportEvidenceType =
    "backend_completed_command_to_visible_background_response";
bool valid_run_uuid(std::string_view value) noexcept;
bool valid_sha256_text(std::string_view value) noexcept;
nlohmann::ordered_json report_event_json(const ProbeCommandEvent& event,
    const char* mouse_status, const char* stop_reason);
bool validate_report_execution(const ProbeExecutionOptions& options,
    const MouseEffectProbeSequence& sequence, const ProbeExecutionResult& result,
    std::string& error);
// 不验证摘要；调用者必须针对同一个已解析document计算并比较摘要。
// 只验证报告内在合同；与外部冻结序列/plan的绑定仍由调用入口校验。
bool validate_report_structure_and_execution(const nlohmann::ordered_json& document,
    std::string& error);
}
#endif
