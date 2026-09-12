#ifndef RECOIL_EXECUTION_EVENTS_H
#define RECOIL_EXECUTION_EVENTS_H
#include "recoil/recoil_worker.h"

enum class RecoilExecutionEventKind { BEGIN, COMMAND, END };
enum class RecoilBatchEndReason { EXHAUSTED, RELEASED, CANCELED, CONTEXT, NOT_SENT, UNKNOWN, STOPPED, EXCEPTION, LATE, LIMIT };
inline const char* RecoilBatchEndReasonName(RecoilBatchEndReason value) noexcept {
    switch (value) {
    case RecoilBatchEndReason::EXHAUSTED: return "EXHAUSTED";
    case RecoilBatchEndReason::RELEASED: return "RELEASED";
    case RecoilBatchEndReason::CANCELED: return "CANCELED";
    case RecoilBatchEndReason::CONTEXT: return "CONTEXT";
    case RecoilBatchEndReason::NOT_SENT: return "NOT_SENT";
    case RecoilBatchEndReason::UNKNOWN: return "UNKNOWN";
    case RecoilBatchEndReason::STOPPED: return "STOPPED";
    case RecoilBatchEndReason::EXCEPTION: return "EXCEPTION";
    case RecoilBatchEndReason::LATE: return "LATE";
    case RecoilBatchEndReason::LIMIT: return "LIMIT";
    }
    return "UNKNOWN";
}
struct RecoilExecutionEvent {
    std::uint64_t sequence = 0, firing_id = 0;
    RecoilExecutionEventKind kind = RecoilExecutionEventKind::BEGIN;
    RecoilTime event_at{}, firing_started_at{};
    RecoilFiringSource firing_source = RecoilFiringSource::UNKNOWN;
    std::uint64_t weapon_generation = 0, device_epoch = 0;
    std::shared_ptr<const RecoilProfile> profile;
    RecoilExecutionRecord command;
    RecoilSnapshot final;
    RecoilBatchEndReason end_reason = RecoilBatchEndReason::CANCELED;
};
struct RecoilEventSlice {
    std::vector<RecoilExecutionEvent> events;
    std::uint64_t oldest_available_sequence = 0, latest_sequence = 0, dropped_total = 0;
    bool cursor_gap = false, sequence_exhausted = false;
};
#endif
