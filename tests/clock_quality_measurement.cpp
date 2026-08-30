#include "aim/aim.h"
#include "clock_sync/clock_sync_internal.h"

#include "aim_superjump_random_move_replay_fixture.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::int64_t kReplayBaseNs = 10'000'000'000LL;
constexpr std::size_t kMapperWarmupRows = 2U;
constexpr std::size_t kRequiredSampleRows =
    aim_superjump_random_move_replay_fixture::kObservations.size() +
    kMapperWarmupRows;
constexpr std::string_view kSampleHeader =
    "source_session_id,requester_send_steady_ns,source_receive_utc_ns,"
    "source_send_utc_ns,requester_receive_steady_ns,probe_source_utc_ns,"
    "probe_reference_local_ns";

struct Options {
    std::string samples_path;
};

struct ClockSampleRow {
    clock_sync::detail::Sample exchange;
    std::uint64_t probe_source_utc_ns = 0;
    std::uint64_t probe_reference_local_ns = 0;
};

struct ClockMeasurement {
    std::size_t sample_rows = 0;
    std::vector<double> input_round_trip_ms;
    std::vector<double> clock_sample_interval_ms;
    std::vector<double> fit_residual_ms;
    std::vector<double> reported_uncertainty_ms;
    std::vector<double> mapping_error_ms;
    double constant_bias_ms = 0.0;
    std::vector<double> per_sample_stress_ms;
};

struct AimSample {
    AimStatus status = AimStatus::NOT_RUN;
    bool has_target = false;
    bool has_command = false;
    float base_x = 0.0f;
    float base_y = 0.0f;
    float delay_x = 0.0f;
    float delay_y = 0.0f;
    float prediction_x = 0.0f;
    float prediction_y = 0.0f;
    float final_x = 0.0f;
    float final_y = 0.0f;
    float velocity_x = 0.0f;
    float velocity_y = 0.0f;
    float observation_age_ms = 0.0f;
    int command_x = 0;
    int command_y = 0;
    float shaped_x_counts = 0.0f;
    float modelled_response_x_counts = 0.0f;
};

struct Sensitivity {
    std::size_t compared_frames = 0;
    std::size_t changed_target_state_frames = 0;
    std::size_t changed_aim_point_frames = 0;
    std::size_t changed_velocity_frames = 0;
    std::size_t changed_observation_age_frames = 0;
    std::size_t changed_control_diagnostic_frames = 0;
    std::size_t changed_command_frames = 0;
    std::size_t first_changed_aim_point_sequence = 0;
    std::size_t first_changed_command_sequence = 0;
    double max_base_point_delta_px = 0.0;
    double max_delay_point_delta_px = 0.0;
    double max_prediction_point_delta_px = 0.0;
    double max_final_point_delta_px = 0.0;
    double max_velocity_delta_px_per_second = 0.0;
    double max_observation_age_delta_ms = 0.0;
    double max_shaped_x_delta_counts = 0.0;
    double max_modelled_response_x_delta_counts = 0.0;
    int max_command_delta_counts = 0;
};

enum class ReplayStopReason {
    NONE,
    COMMAND_PRESENCE_DIVERGED,
    COMMAND_VALUE_DIVERGED,
    COMPLETION_RECORD_FAILED,
    TIMESTAMP_OUT_OF_RANGE,
};

enum class MeasurementOutcome {
    COMPARABLE,
    INDETERMINATE,
    FAILED,
};

struct ReplayPair {
    std::vector<AimSample> baseline;
    std::vector<AimSample> candidate;
    bool completion_history_isolated = true;
    std::size_t shared_zero_completion_frames = 0;
    std::size_t baseline_completed_commands = 0;
    std::size_t candidate_completed_commands = 0;
    std::size_t stopped_at_sequence = 0;
    ReplayStopReason stop_reason = ReplayStopReason::NONE;
};

const char* replay_stop_reason_name(ReplayStopReason reason) noexcept {
    switch (reason) {
    case ReplayStopReason::NONE:
        return "none";
    case ReplayStopReason::COMMAND_PRESENCE_DIVERGED:
        return "command_presence_diverged";
    case ReplayStopReason::COMMAND_VALUE_DIVERGED:
        return "command_value_diverged";
    case ReplayStopReason::COMPLETION_RECORD_FAILED:
        return "completion_record_failed";
    case ReplayStopReason::TIMESTAMP_OUT_OF_RANGE:
        return "timestamp_out_of_range";
    }
    return "unknown";
}

bool parse_options(int argc, char** argv, Options& options) noexcept {
    if (argc != 3 || std::string_view(argv[1]) != "--samples")
        return false;
    options.samples_path = argv[2];
    return !options.samples_path.empty();
}

bool parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty())
        return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::vector<std::string_view> split_csv_row(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t comma = line.find(',', begin);
        if (comma == std::string_view::npos) {
            fields.push_back(line.substr(begin));
            break;
        }
        fields.push_back(line.substr(begin, comma - begin));
        begin = comma + 1U;
    }
    return fields;
}

bool load_sample_rows(const std::string& path,
                      std::vector<ClockSampleRow>& rows,
                      std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "cannot_open_sample_file";
        return false;
    }

    std::string line;
    if (!std::getline(input, line)) {
        error = "missing_header";
        return false;
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != kSampleHeader) {
        error = "unexpected_header";
        return false;
    }

    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const auto fields = split_csv_row(line);
        if (fields.size() != 7U) {
            error = "field_count_at_line_" + std::to_string(line_number);
            return false;
        }

        ClockSampleRow row;
        if (!parse_u64(fields[0], row.exchange.source_session_id) ||
            !parse_u64(fields[1],
                       row.exchange.requester_send_steady_ns) ||
            !parse_u64(fields[2], row.exchange.source_receive_utc_ns) ||
            !parse_u64(fields[3], row.exchange.source_send_utc_ns) ||
            !parse_u64(fields[4],
                       row.exchange.requester_receive_steady_ns) ||
            !parse_u64(fields[5], row.probe_source_utc_ns) ||
            !parse_u64(fields[6], row.probe_reference_local_ns)) {
            error = "invalid_integer_at_line_" + std::to_string(line_number);
            return false;
        }
        if (row.probe_source_utc_ns == 0 ||
            row.probe_reference_local_ns == 0 ||
            row.exchange.requester_receive_steady_ns >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
            row.probe_reference_local_ns >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            error = "out_of_range_at_line_" + std::to_string(line_number);
            return false;
        }
        if (!rows.empty() && row.exchange.source_session_id !=
                rows.front().exchange.source_session_id) {
            error = "source_session_changed_expected_" +
                std::to_string(rows.front().exchange.source_session_id) +
                "_actual_" +
                std::to_string(row.exchange.source_session_id) + "_at_row_" +
                std::to_string(rows.size() + 1U);
            return false;
        }
        rows.push_back(row);
    }
    if (rows.size() != kRequiredSampleRows) {
        error = "unexpected_sample_count_expected_" +
            std::to_string(kRequiredSampleRows) + "_actual_" +
            std::to_string(rows.size());
        return false;
    }
    return true;
}

bool signed_nanoseconds_from_ms(double milliseconds,
                                std::int64_t& nanoseconds) noexcept {
    if (!std::isfinite(milliseconds)) {
        return false;
    }
    const double maximum_safe = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0);
    const double minimum_safe = std::nextafter(
        static_cast<double>(std::numeric_limits<std::int64_t>::min()), 0.0);
    const double rounded = std::round(milliseconds * 1.0e6);
    if (!std::isfinite(rounded) || rounded > maximum_safe ||
        rounded < minimum_safe) {
        return false;
    }
    nanoseconds = static_cast<std::int64_t>(rounded);
    return true;
}

bool shift_timepoint_by_ms(
    std::chrono::steady_clock::time_point base, double delta_ms,
    std::chrono::steady_clock::time_point& shifted) noexcept {
    std::int64_t delta_ns = 0;
    if (!signed_nanoseconds_from_ms(delta_ms, delta_ns)) {
        return false;
    }
    const std::int64_t base_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            base.time_since_epoch())
            .count();
    if ((delta_ns > 0 &&
         base_ns > std::numeric_limits<std::int64_t>::max() - delta_ns) ||
        (delta_ns < 0 &&
         base_ns < std::numeric_limits<std::int64_t>::min() - delta_ns)) {
        return false;
    }
    shifted = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(base_ns + delta_ns));
    return true;
}

double milliseconds(std::chrono::steady_clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double percentile(std::vector<double> values, double probability) {
    if (values.empty())
        return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const double position = probability *
        static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

std::vector<double> absolute_values(std::span<const double> values) {
    std::vector<double> result;
    result.reserve(values.size());
    std::transform(values.begin(), values.end(), std::back_inserter(result),
                   [](double value) { return std::abs(value); });
    return result;
}

bool measure_mapper(const Options& options,
                    ClockMeasurement& measurement) noexcept {
    try {
        std::vector<ClockSampleRow> rows;
        std::string input_error;
        if (!load_sample_rows(options.samples_path, rows, input_error)) {
            std::cout << "mapper,status=input_invalid,error=" << input_error
                      << '\n';
            return false;
        }
        measurement.sample_rows = rows.size();
        measurement.input_round_trip_ms.reserve(rows.size());
        measurement.clock_sample_interval_ms.reserve(rows.size() - 1U);
        measurement.fit_residual_ms.reserve(rows.size());
        measurement.reported_uncertainty_ms.reserve(rows.size());
        measurement.mapping_error_ms.reserve(rows.size());

        clock_sync::detail::AffineMapper mapper;
        std::uint64_t previous_valid_receive_ns = 0;
        for (const ClockSampleRow& row : rows) {
            const auto& sample = row.exchange;
            if (sample.requester_receive_steady_ns <
                    sample.requester_send_steady_ns ||
                sample.source_send_utc_ns < sample.source_receive_utc_ns) {
                std::cout << "mapper,status=input_invalid,error="
                             "timestamp_order\n";
                return false;
            }
            const std::uint64_t local_round_trip_ns =
                sample.requester_receive_steady_ns -
                sample.requester_send_steady_ns;
            const std::uint64_t source_processing_ns =
                sample.source_send_utc_ns - sample.source_receive_utc_ns;
            if (source_processing_ns > local_round_trip_ns ||
                !mapper.add_sample(sample)) {
                std::cout << "mapper,status=input_invalid,error="
                             "invalid_exchange\n";
                return false;
            }
            const auto local_now = std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(static_cast<std::int64_t>(
                    sample.requester_receive_steady_ns)));
            // 每行在该交换刚完成时测量，1 秒仅是允许 age=0 的测量器有效域，
            // 不参与任何生产门限或策略判定。
            constexpr auto kMeasurementMaximumAge = std::chrono::seconds(1);
            const auto mapped_probe = mapper.map_utc_ns(
                row.probe_source_utc_ns, local_now, kMeasurementMaximumAge);
            if (mapped_probe.status == clock_sync::MappingStatus::WARMING)
                continue;
            if (!mapped_probe.valid ||
                mapped_probe.status != clock_sync::MappingStatus::VALID) {
                std::cout << "mapper,status=map_failed,error="
                          << clock_sync::MappingStatusName(mapped_probe.status)
                          << '\n';
                return false;
            }

            const std::uint64_t source_midpoint_ns =
                sample.source_receive_utc_ns + source_processing_ns / 2U;
            const std::uint64_t local_midpoint_ns =
                sample.requester_send_steady_ns + local_round_trip_ns / 2U;
            const auto mapped_midpoint = mapper.map_utc_ns(
                source_midpoint_ns, local_now, kMeasurementMaximumAge);
            if (!mapped_midpoint.valid) {
                std::cout << "mapper,status=map_failed,error=midpoint\n";
                return false;
            }

            if (previous_valid_receive_ns != 0) {
                if (sample.requester_receive_steady_ns <=
                    previous_valid_receive_ns) {
                    std::cout << "mapper,status=input_invalid,error="
                                 "non_monotonic_receive_time\n";
                    return false;
                }
                measurement.clock_sample_interval_ms.push_back(
                    static_cast<double>(sample.requester_receive_steady_ns -
                                        previous_valid_receive_ns) /
                    1.0e6);
            }
            previous_valid_receive_ns = sample.requester_receive_steady_ns;
            // 所有分布只消费同一批 mapped VALID 样本，不将
            // WARMING 的 RTT 与后续误差/残差/不确定度混在一起。
            measurement.input_round_trip_ms.push_back(
                static_cast<double>(local_round_trip_ns -
                                    source_processing_ns) /
                1.0e6);

            const auto probe_reference =
                std::chrono::steady_clock::time_point(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        row.probe_reference_local_ns)));
            const auto local_midpoint =
                std::chrono::steady_clock::time_point(
                    std::chrono::nanoseconds(static_cast<std::int64_t>(
                        local_midpoint_ns)));
            measurement.mapping_error_ms.push_back(
                milliseconds(mapped_probe.local_time - probe_reference));
            measurement.fit_residual_ms.push_back(
                milliseconds(mapped_midpoint.local_time - local_midpoint));
            measurement.reported_uncertainty_ms.push_back(
                mapped_probe.uncertainty_ms);
        }

        using aim_superjump_random_move_replay_fixture::kObservations;
        if (measurement.mapping_error_ms.size() < kObservations.size()) {
            std::cout << "mapper,status=insufficient_mapped_samples"
                      << ",sample_rows=" << rows.size()
                      << ",mapped_error_samples="
                      << measurement.mapping_error_ms.size()
                      << ",required=" << kObservations.size() << '\n';
            return false;
        }
        measurement.mapping_error_ms.resize(kObservations.size());
        measurement.input_round_trip_ms.resize(kObservations.size());
        measurement.clock_sample_interval_ms.resize(kObservations.size() - 1U);
        measurement.fit_residual_ms.resize(kObservations.size());
        measurement.reported_uncertainty_ms.resize(kObservations.size());
        measurement.constant_bias_ms = std::accumulate(
            measurement.mapping_error_ms.begin(),
            measurement.mapping_error_ms.end(), 0.0) /
            static_cast<double>(measurement.mapping_error_ms.size());
        measurement.per_sample_stress_ms.reserve(
            measurement.mapping_error_ms.size());
        for (double error_ms : measurement.mapping_error_ms) {
            measurement.per_sample_stress_ms.push_back(
                error_ms - measurement.constant_bias_ms);
        }

        const auto absolute_residuals =
            absolute_values(measurement.fit_residual_ms);
        const auto absolute_stress =
            absolute_values(measurement.per_sample_stress_ms);
        const auto [minimum_error, maximum_error] = std::minmax_element(
            measurement.mapping_error_ms.begin(),
            measurement.mapping_error_ms.end());
        std::cout
            << "mapper,status=valid"
            << ",data_source=explicit_four_timestamp_csv"
            << ",reference_source=per_sample_probe_reference"
            << ",sample_rows=" << measurement.sample_rows
            << ",mapped_error_samples="
            << measurement.mapping_error_ms.size()
            << ",metric_sample_domain=mapped_valid_samples"
            << ",input_rtt_samples="
            << measurement.input_round_trip_ms.size()
            << ",clock_sample_intervals="
            << measurement.clock_sample_interval_ms.size()
            << ",input_rtt_p50_ms="
            << percentile(measurement.input_round_trip_ms, 0.50)
            << ",input_rtt_p95_ms="
            << percentile(measurement.input_round_trip_ms, 0.95)
            << ",input_rtt_p99_ms="
            << percentile(measurement.input_round_trip_ms, 0.99)
            << ",input_rtt_max_ms="
            << *std::max_element(measurement.input_round_trip_ms.begin(),
                                 measurement.input_round_trip_ms.end())
            << ",fit_residual_abs_p95_ms="
            << percentile(absolute_residuals, 0.95)
            << ",fit_residual_abs_max_ms="
            << *std::max_element(absolute_residuals.begin(),
                                 absolute_residuals.end())
            << ",reported_uncertainty_p95_ms="
            << percentile(measurement.reported_uncertainty_ms, 0.95)
            << ",reported_uncertainty_max_ms="
            << *std::max_element(
                   measurement.reported_uncertainty_ms.begin(),
                   measurement.reported_uncertainty_ms.end())
            << ",mapping_error_signed_mean_ms="
            << measurement.constant_bias_ms
            << ",mapping_error_min_ms=" << *minimum_error
            << ",mapping_error_max_ms=" << *maximum_error
            << ",constant_bias_ms=" << measurement.constant_bias_ms
            << ",per_sample_stress_p95_ms="
            << percentile(absolute_stress, 0.95)
            << ",per_sample_stress_max_abs_ms="
            << *std::max_element(absolute_stress.begin(),
                                 absolute_stress.end())
            << '\n';
        return true;
    } catch (...) {
        std::cout << "mapper,status=measurement_failed,error=exception\n";
        return false;
    }
}

Detection head_box(float center_x, float center_y, float width,
                   float height) noexcept {
    return {center_x - width * 0.5f,
            center_y - height * 0.5f,
            center_x + width * 0.5f,
            center_y + height * 0.5f,
            0.95f,
            1};
}

AimConfig replay_config() noexcept {
    AimConfig config;
    config.high_confidence = 0.25f;
    config.low_confidence = 0.10f;
    config.min_confirmed_hits = 2;
    config.acquisition_range_percent = 90.0f;
    config.body_aim_height_ratio = 0.35f;
    config.body_aim_range_percent = 50.0f;
    config.deadzone_pixels = 1.5f;
    config.smoothing = 0.475f;
    config.counts_per_pixel_x = 0.425f;
    config.counts_per_pixel_y = 0.400f;
    config.max_counts_per_frame = 14.0f;
    config.enable_delay_compensation = true;
    config.control_delay_ms = 15.0f;
    config.max_delay_compensation_ms = 44.0f;
    config.max_delay_compensation_percent = 15.0f;
    config.enable_prediction = false;
    return config;
}

AimSample sample_from_result(const AimResult& result) noexcept {
    AimSample sample;
    sample.status = result.status;
    sample.has_target = result.has_target;
    sample.has_command = result.has_command;
    if (result.has_target) {
        sample.base_x = result.target.base_aim_x;
        sample.base_y = result.target.base_aim_y;
        sample.delay_x = result.target.delay_compensated_aim_x;
        sample.delay_y = result.target.delay_compensated_aim_y;
        sample.prediction_x = result.target.prediction_aim_x;
        sample.prediction_y = result.target.prediction_aim_y;
        sample.final_x = result.target.aim_x;
        sample.final_y = result.target.aim_y;
        sample.velocity_x = result.target.velocity_x;
        sample.velocity_y = result.target.velocity_y;
        sample.observation_age_ms = result.target.observation_age_ms;
    }
    if (result.has_command) {
        sample.command_x = result.command.dx_counts;
        sample.command_y = result.command.dy_counts;
    }
    sample.shaped_x_counts = result.control.shaped_x_counts;
    sample.modelled_response_x_counts =
        result.control.modelled_response_x_counts;
    return sample;
}

ReplayPair replay_aim_pair(std::span<const double> timestamp_errors_ms) {
    using aim_superjump_random_move_replay_fixture::kObservations;

    const AimConfig config = replay_config();
    Aim baseline_aim(config);
    Aim candidate_aim(config);

    auto control_at = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(kReplayBaseNs));
    ReplayPair replay;
    replay.baseline.reserve(kObservations.size());
    replay.candidate.reserve(kObservations.size());
    if (timestamp_errors_ms.size() < kObservations.size()) {
        replay.completion_history_isolated = false;
        replay.stop_reason = ReplayStopReason::TIMESTAMP_OUT_OF_RANGE;
        return replay;
    }
    for (std::size_t index = 0; index < kObservations.size(); ++index) {
        const auto& observation = kObservations[index];
        if (index != 0) {
            control_at += std::chrono::nanoseconds(static_cast<std::int64_t>(
                std::llround(observation.controller_dt_ms * 1.0e6f)));
        }
        const auto true_captured_at =
            control_at -
            std::chrono::nanoseconds(static_cast<std::int64_t>(
                std::llround(observation.observation_age_ms * 1.0e6f)));
        std::chrono::steady_clock::time_point candidate_captured_at;
        if (!shift_timepoint_by_ms(
                true_captured_at, timestamp_errors_ms[index],
                candidate_captured_at)) {
            replay.completion_history_isolated = false;
            replay.stopped_at_sequence = index + 1U;
            replay.stop_reason = ReplayStopReason::TIMESTAMP_OUT_OF_RANGE;
            break;
        }

        AimFrame baseline_frame;
        baseline_frame.sequence = index + 1U;
        baseline_frame.captured_at = true_captured_at;
        baseline_frame.control_at = control_at;
        baseline_frame.roi_width = 320;
        baseline_frame.roi_height = 320;
        baseline_frame.control_center_x = 160.0f;
        baseline_frame.control_center_y = 160.0f;
        baseline_frame.lock_active = true;
        baseline_frame.detections = {{observation.x1, observation.y1,
                                      observation.x2, observation.y2, 0.9f, 0}};
        if (observation.aim_from_head) {
            const float center_x = (observation.x1 + observation.x2) * 0.5f;
            const float head_width = (observation.x2 - observation.x1) * 0.5f;
            baseline_frame.detections.push_back(
                head_box(center_x, observation.y1 + 6.0f, head_width, 10.0f));
        }

        AimFrame candidate_frame = baseline_frame;
        candidate_frame.captured_at = candidate_captured_at;
        const AimResult baseline_result = baseline_aim.process(baseline_frame);
        const AimResult candidate_result =
            candidate_aim.process(candidate_frame);
        AimSample baseline_sample = sample_from_result(baseline_result);
        AimSample candidate_sample = sample_from_result(candidate_result);

        bool stop_after_frame = false;
        const bool command_presence_diverged =
            baseline_result.has_command != candidate_result.has_command;
        const bool command_value_diverged =
            baseline_result.has_command && candidate_result.has_command &&
            (baseline_result.command.dx_counts !=
                 candidate_result.command.dx_counts ||
             baseline_result.command.dy_counts !=
                 candidate_result.command.dy_counts);
        if (baseline_result.has_command || candidate_result.has_command) {
            const auto completed_at =
                control_at +
                std::chrono::microseconds(static_cast<std::int64_t>(
                    std::llround(observation.backend_completion_ms * 1000.0f)));
            const bool baseline_recorded = !baseline_result.has_command ||
                baseline_aim.record_backend_completed_command(
                    baseline_frame.sequence, completed_at,
                    baseline_result.command.dx_counts,
                    baseline_result.command.dy_counts);
            const bool candidate_recorded = !candidate_result.has_command ||
                candidate_aim.record_backend_completed_command(
                    candidate_frame.sequence, completed_at,
                    candidate_result.command.dx_counts,
                    candidate_result.command.dy_counts);
            if (baseline_result.has_command && baseline_recorded)
                ++replay.baseline_completed_commands;
            if (candidate_result.has_command && candidate_recorded)
                ++replay.candidate_completed_commands;
            if (!baseline_recorded || !candidate_recorded) {
                baseline_sample.status = AimStatus::CONTROL_FAILED;
                candidate_sample.status = AimStatus::CONTROL_FAILED;
                replay.completion_history_isolated = false;
                replay.stopped_at_sequence = baseline_frame.sequence;
                replay.stop_reason = ReplayStopReason::COMPLETION_RECORD_FAILED;
                stop_after_frame = true;
            }
        }
        if (!stop_after_frame && command_presence_diverged) {
            replay.stopped_at_sequence = baseline_frame.sequence;
            replay.stop_reason = ReplayStopReason::COMMAND_PRESENCE_DIVERGED;
            stop_after_frame = true;
        } else if (!stop_after_frame && command_value_diverged) {
            replay.stopped_at_sequence = baseline_frame.sequence;
            replay.stop_reason = ReplayStopReason::COMMAND_VALUE_DIVERGED;
            stop_after_frame = true;
        }
        replay.baseline.push_back(baseline_sample);
        replay.candidate.push_back(candidate_sample);
        if (stop_after_frame)
            break;
    }
    return replay;
}

Sensitivity compare_replays(const std::vector<AimSample>& baseline,
                            const std::vector<AimSample>& candidate) noexcept {
    using aim_superjump_random_move_replay_fixture::kMeasurementStart;

    Sensitivity result;
    if (baseline.size() != candidate.size() ||
        baseline.size() < kMeasurementStart) {
        return result;
    }
    result.compared_frames = baseline.size() - kMeasurementStart;
    for (std::size_t index = kMeasurementStart; index < baseline.size();
         ++index) {
        const AimSample& left = baseline[index];
        const AimSample& right = candidate[index];
        const auto point_delta = [](float left_x, float left_y, float right_x,
                                    float right_y) {
            return std::hypot(static_cast<double>(left_x - right_x),
                              static_cast<double>(left_y - right_y));
        };
        const double base_delta =
            point_delta(left.base_x, left.base_y, right.base_x, right.base_y);
        const double delay_delta = point_delta(left.delay_x, left.delay_y,
                                               right.delay_x, right.delay_y);
        const double prediction_delta =
            point_delta(left.prediction_x, left.prediction_y,
                        right.prediction_x, right.prediction_y);
        const double final_delta = point_delta(left.final_x, left.final_y,
                                               right.final_x, right.final_y);
        const double velocity_delta =
            point_delta(left.velocity_x, left.velocity_y, right.velocity_x,
                        right.velocity_y);
        const double age_delta = std::abs(static_cast<double>(
            left.observation_age_ms - right.observation_age_ms));
        const double shaped_delta = std::abs(
            static_cast<double>(left.shaped_x_counts - right.shaped_x_counts));
        const double modelled_response_delta =
            std::abs(static_cast<double>(left.modelled_response_x_counts -
                                         right.modelled_response_x_counts));
        if (left.status != right.status ||
            left.has_target != right.has_target) {
            ++result.changed_target_state_frames;
        }
        if (base_delta > 0.0 || delay_delta > 0.0 || prediction_delta > 0.0 ||
            final_delta > 0.0) {
            ++result.changed_aim_point_frames;
            if (result.first_changed_aim_point_sequence == 0) {
                result.first_changed_aim_point_sequence = index + 1U;
            }
        }
        if (velocity_delta > 0.0)
            ++result.changed_velocity_frames;
        if (age_delta > 0.0)
            ++result.changed_observation_age_frames;
        if (shaped_delta > 0.0 || modelled_response_delta > 0.0) {
            ++result.changed_control_diagnostic_frames;
        }
        if (left.has_command != right.has_command ||
            left.command_x != right.command_x ||
            left.command_y != right.command_y) {
            ++result.changed_command_frames;
            if (result.first_changed_command_sequence == 0) {
                result.first_changed_command_sequence = index + 1U;
            }
        }
        result.max_base_point_delta_px =
            std::max(result.max_base_point_delta_px, base_delta);
        result.max_delay_point_delta_px =
            std::max(result.max_delay_point_delta_px, delay_delta);
        result.max_prediction_point_delta_px =
            std::max(result.max_prediction_point_delta_px, prediction_delta);
        result.max_final_point_delta_px =
            std::max(result.max_final_point_delta_px, final_delta);
        result.max_velocity_delta_px_per_second =
            std::max(result.max_velocity_delta_px_per_second, velocity_delta);
        result.max_observation_age_delta_ms =
            std::max(result.max_observation_age_delta_ms, age_delta);
        result.max_shaped_x_delta_counts =
            std::max(result.max_shaped_x_delta_counts, shaped_delta);
        result.max_modelled_response_x_delta_counts =
            std::max(result.max_modelled_response_x_delta_counts,
                     modelled_response_delta);
        result.max_command_delta_counts =
            std::max(result.max_command_delta_counts,
                     std::max(std::abs(left.command_x - right.command_x),
                              std::abs(left.command_y - right.command_y)));
    }
    return result;
}

double aim_frame_interval_p50_ms() {
    using aim_superjump_random_move_replay_fixture::kObservations;

    std::vector<double> intervals;
    intervals.reserve(kObservations.size() - 1U);
    for (std::size_t index = 1; index < kObservations.size(); ++index)
        intervals.push_back(kObservations[index].controller_dt_ms);
    return percentile(std::move(intervals), 0.50);
}

MeasurementOutcome measure_aim_synthetic_sequence(
    std::span<const double> timestamp_errors_ms,
    std::string_view error_component, double clock_sample_interval_p50_ms,
    double aim_frame_interval_p50_ms) {
    using aim_superjump_random_move_replay_fixture::kObservations;

    const ReplayPair replay = replay_aim_pair(timestamp_errors_ms);
    const Sensitivity sensitivity =
        compare_replays(replay.baseline, replay.candidate);
    const bool replay_complete = replay.baseline.size() == kObservations.size();
    const bool command_diverged =
        replay.stop_reason == ReplayStopReason::COMMAND_PRESENCE_DIVERGED ||
        replay.stop_reason == ReplayStopReason::COMMAND_VALUE_DIVERGED;
    const bool samples_succeeded =
        std::all_of(replay.baseline.begin(), replay.baseline.end(),
                    [](const AimSample& sample) {
                        return sample.status == AimStatus::SUCCESS;
                    }) &&
        std::all_of(replay.candidate.begin(), replay.candidate.end(),
                    [](const AimSample& sample) {
                        return sample.status == AimStatus::SUCCESS;
                    });
    MeasurementOutcome outcome = MeasurementOutcome::FAILED;
    if (replay.completion_history_isolated && samples_succeeded &&
        command_diverged) {
        outcome = MeasurementOutcome::INDETERMINATE;
    } else if (replay.completion_history_isolated && samples_succeeded &&
               replay_complete && sensitivity.compared_frames > 0) {
        outcome = MeasurementOutcome::COMPARABLE;
    }
    const char* status_name = "failed";
    if (outcome == MeasurementOutcome::COMPARABLE) {
        status_name = "comparable";
    } else if (outcome == MeasurementOutcome::INDETERMINATE) {
        status_name = "indeterminate";
    }
    const double mean_error_ms = timestamp_errors_ms.empty()
        ? 0.0
        : std::accumulate(timestamp_errors_ms.begin(),
                          timestamp_errors_ms.end(), 0.0) /
              static_cast<double>(timestamp_errors_ms.size());
    const auto [minimum_error, maximum_error] = timestamp_errors_ms.empty()
        ? std::pair{timestamp_errors_ms.end(), timestamp_errors_ms.end()}
        : std::minmax_element(timestamp_errors_ms.begin(),
                              timestamp_errors_ms.end());

    std::cout
        << "aim"
        << ",status=" << status_name
        << ",fixture=random_move_superjump"
        << ",fixture_source=actual_run_20260830_112517"
        << ",delay_compensation=on"
        << ",prediction=off"
        << ",time_aligned=false"
        << ",mapping_age_evaluated=false"
        << ",sequence_alignment=index_only"
        << ",clock_sample_interval_p50_ms="
        << clock_sample_interval_p50_ms
        << ",aim_frame_interval_p50_ms=" << aim_frame_interval_p50_ms
        << ",clock_to_aim_cadence_ratio="
        << clock_sample_interval_p50_ms / aim_frame_interval_p50_ms
        << ",aim_timing_input=captured_at_error_only"
        << ",error_source=mapper_per_sample_signed_error"
        << ",error_component=" << error_component
        << ",error_sample_order=input_row_order"
        << ",input_error_mean_ms=" << mean_error_ms
        << ",input_error_min_ms="
        << (timestamp_errors_ms.empty() ? 0.0 : *minimum_error)
        << ",input_error_max_ms="
        << (timestamp_errors_ms.empty() ? 0.0 : *maximum_error)
        << ",completion_history_owner=per_replay_actual_command"
        << ",completion_history_isolated="
        << (replay.completion_history_isolated ? "true" : "false")
        << ",trajectory_comparable="
        << (outcome == MeasurementOutcome::COMPARABLE ? "true" : "false")
        << ",shared_zero_completion_frames="
        << replay.shared_zero_completion_frames
        << ",baseline_completed_commands="
        << replay.baseline_completed_commands
        << ",candidate_completed_commands="
        << replay.candidate_completed_commands
        << ",replay_complete=" << (replay_complete ? "true" : "false")
        << ",stop_reason=" << replay_stop_reason_name(replay.stop_reason)
        << ",stopped_at_sequence=" << replay.stopped_at_sequence
        << ",frames=" << sensitivity.compared_frames
        << ",changed_target_state_frames="
        << sensitivity.changed_target_state_frames
        << ",changed_aim_point_frames=" << sensitivity.changed_aim_point_frames
        << ",changed_velocity_frames=" << sensitivity.changed_velocity_frames
        << ",changed_observation_age_frames="
        << sensitivity.changed_observation_age_frames
        << ",changed_control_diagnostic_frames="
        << sensitivity.changed_control_diagnostic_frames
        << ",changed_command_frames=" << sensitivity.changed_command_frames
        << ",first_changed_aim_point_sequence="
        << sensitivity.first_changed_aim_point_sequence
        << ",first_changed_command_sequence="
        << sensitivity.first_changed_command_sequence
        << ",max_base_point_delta_px=" << sensitivity.max_base_point_delta_px
        << ",max_delay_point_delta_px=" << sensitivity.max_delay_point_delta_px
        << ",max_prediction_point_delta_px="
        << sensitivity.max_prediction_point_delta_px
        << ",max_final_point_delta_px=" << sensitivity.max_final_point_delta_px
        << ",max_velocity_delta_px_per_second="
        << sensitivity.max_velocity_delta_px_per_second
        << ",max_observation_age_delta_ms="
        << sensitivity.max_observation_age_delta_ms
        << ",max_shaped_x_delta_counts="
        << sensitivity.max_shaped_x_delta_counts
        << ",max_modelled_response_x_delta_counts="
        << sensitivity.max_modelled_response_x_delta_counts
        << ",max_command_delta_counts=" << sensitivity.max_command_delta_counts
        << '\n';

    return outcome;
}

void print_usage() {
    std::cerr
        << "用法: clock_quality_measurement --samples <逐样本四时间戳.csv>\n"
        << "CSV: " << kSampleHeader << '\n'
        << "输出仅为非时序 synthetic sequence stress；不评估 mapping "
           "age，不形成生产阈值或真实因果结论。\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    std::cout << std::fixed << std::setprecision(6);
    ClockMeasurement measurement;
    if (!measure_mapper(options, measurement)) {
        std::cerr << "clock quality 离线测量输入或 Mapper 回放失败。\n";
        return 1;
    }

    std::vector<double> constant_bias_errors(
        measurement.mapping_error_ms.size(), measurement.constant_bias_ms);
    const double clock_sample_interval_p50_ms =
        percentile(measurement.clock_sample_interval_ms, 0.50);
    const double aim_interval_p50_ms = aim_frame_interval_p50_ms();
    const MeasurementOutcome synthetic_sequence_outcome =
        measure_aim_synthetic_sequence(
            measurement.mapping_error_ms, "synthetic_sequence",
            clock_sample_interval_p50_ms, aim_interval_p50_ms);
    const MeasurementOutcome constant_outcome =
        measure_aim_synthetic_sequence(
            constant_bias_errors, "constant_bias",
            clock_sample_interval_p50_ms, aim_interval_p50_ms);
    const MeasurementOutcome stress_outcome =
        measure_aim_synthetic_sequence(
            measurement.per_sample_stress_ms, "per_sample_stress",
            clock_sample_interval_p50_ms, aim_interval_p50_ms);
    if (synthetic_sequence_outcome == MeasurementOutcome::FAILED ||
        constant_outcome == MeasurementOutcome::FAILED ||
        stress_outcome == MeasurementOutcome::FAILED) {
        std::cerr << "clock quality Mapper→Aim 离线测量失败。\n";
        return 1;
    }
    if (synthetic_sequence_outcome == MeasurementOutcome::INDETERMINATE ||
        constant_outcome == MeasurementOutcome::INDETERMINATE ||
        stress_outcome == MeasurementOutcome::INDETERMINATE) {
        std::cerr << "clock quality Mapper→Aim 非时序 stress 轨迹分歧，"
                     "测量不可判定。\n";
        return 3;
    }
    return 0;
}
