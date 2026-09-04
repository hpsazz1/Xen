#include "mouse_effect_probe/mouse_effect_probe.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string_view>

namespace mouse_effect_probe {

namespace {

constexpr std::uint32_t kSequenceSchema = 1;
constexpr std::uint32_t kDependencyCalibrationSequenceSchema = 2;
constexpr std::uint32_t kS1LivenessSequenceSchema = 3;
constexpr std::uint32_t kPhysicalBPrimarySequenceSchema = 5;
constexpr std::uint32_t kCommandMagnitudeSequenceSchema = 6;
constexpr std::uint32_t kCompositePhaseSequenceSchema = 7;
constexpr std::string_view kSparsePulseProfile = "sparse_pulse_a";
constexpr std::string_view kDependencyCalibrationPrimaryProfile =
    "dependency_calibration_a2_p_cal";
constexpr std::string_view kDependencyCalibrationHoldoutProfile =
    "dependency_calibration_a2_p_holdout";
constexpr std::string_view kS1LivenessPrimaryProfile =
    "dependency_calibration_a2_s1_primary";
constexpr std::string_view kS1LivenessValidationProfile =
    "dependency_calibration_a2_s1_validation";
constexpr std::string_view kPhysicalBPrimaryProfile =
    "physical_b_prbs_primary";
constexpr std::uint64_t kPhysicalBPrimaryGuardSamples = 32;
constexpr std::uint32_t kPhysicalBPrimaryLfsrOrder = 6;
constexpr std::uint32_t kPhysicalBPrimaryFeedbackMask = 0x27;
constexpr std::uint64_t kPhysicalBPrimarySeed = 1;
constexpr std::uint64_t kPhysicalBPrimaryPhase = 49;
constexpr std::string_view kPhysicalBPrimaryOfflineSequenceSha256 =
    "b69917ffdbf32061644c1531913371590e81719ee5b2440eb0609fba2c9c0b2d";
constexpr std::string_view kPhysicalBHoldoutProfile =
    "physical_b_prbs_holdout";
constexpr std::uint64_t kPhysicalBHoldoutGuardSamples = 32;
constexpr std::uint32_t kPhysicalBHoldoutLfsrOrder = 6;
constexpr std::uint32_t kPhysicalBHoldoutFeedbackMask = 0x33;
constexpr std::uint64_t kPhysicalBHoldoutSeed = 1;
constexpr std::uint64_t kPhysicalBHoldoutPhase = 21;
constexpr std::string_view kPhysicalBHoldoutOfflineSequenceSha256 =
    "e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4";
constexpr std::string_view kCommandMagnitudePrimaryProfile =
    "physical_b_command_magnitude_primary";
constexpr std::string_view kCommandMagnitudeHoldoutProfile =
    "physical_b_command_magnitude_holdout";
constexpr std::string_view kCompositePhaseProfile =
    "physical_b_composite_phase_calibration";
constexpr std::uint64_t kCommandMagnitudeBaselineSamples = 64;
constexpr std::uint64_t kCommandMagnitudeResponseSamples = 48;
constexpr std::uint64_t kCommandMagnitudeGuardSamples = 32;
constexpr std::uint64_t kCompositePhasePredictorSamples = 1;
constexpr std::uint64_t kCompositePhaseWindowSamples = 6;
constexpr int kCompositePhaseMagnitudeCounts = 1;
constexpr std::int64_t kCompositePhaseIssueLeadNs = 400'000;
constexpr std::uint64_t kCompositePhaseTargetToleranceQ32 =
    std::uint64_t{1} << 28U;
constexpr std::uint64_t kCompositePhaseActiveGuardNs = 300'000;
constexpr std::uint64_t kCompositePhaseMaxWakeLatenessNs = 150'000;
constexpr std::uint64_t kCompositePhaseMaxEventIntervalWidthNs = 100'000;
constexpr std::uint64_t kCompositePhaseMaxActiveWaitPerEventNs = 350'000;
constexpr std::uint64_t kCompositePhaseMaxActiveWaitTotalNs =
    42U * kCompositePhaseMaxActiveWaitPerEventNs;
constexpr std::string_view kCompositePhaseTimerMode =
    "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL";
constexpr std::uint64_t kMaximumSequenceSamples = 1'000'000;
constexpr std::uint64_t kMaximumDependencyCalibrationBlocks = 64;
constexpr std::uint64_t kMaximumS1LivenessSamples = 2'400;
constexpr std::uint64_t kMaximumS1LivenessChallengePulseCount = 64;
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

bool expected_dependency_calibration_sample_count(
        const DependencyCalibrationSequenceRequest& request,
        std::uint64_t& output) noexcept {
    if (request.baseline_sample_count == 0 ||
        request.response_sample_count == 0 ||
        request.guard_sample_count == 0 ||
        request.block_count < 4 ||
        request.block_count > kMaximumDependencyCalibrationBlocks ||
        request.block_count % 4U != 0U) {
        return false;
    }
    std::uint64_t response_and_guard = 0;
    std::uint64_t per_block = 0;
    std::uint64_t all_blocks = 0;
    if (!checked_add(request.response_sample_count,
                     request.guard_sample_count,
                     response_and_guard) ||
        response_and_guard >
            (std::numeric_limits<std::uint64_t>::max() - 2U) / 2U) {
        return false;
    }
    per_block = response_and_guard * 2U + 2U;
    if (request.block_count >
            std::numeric_limits<std::uint64_t>::max() / per_block) {
        return false;
    }
    all_blocks = request.block_count * per_block;
    if (!checked_add(request.baseline_sample_count, all_blocks, output) ||
        output > kMaximumSequenceSamples) {
        return false;
    }
    return true;
}

bool expected_s1_liveness_sample_count(
        const S1LivenessSequenceRequest& request,
        std::uint64_t& output) noexcept {
    if (request.challenge_pulse_count == 0 ||
        request.challenge_pulse_count >
            kMaximumS1LivenessChallengePulseCount ||
        request.challenge_stride_sample_count == 0 ||
        request.settle_sample_count == 0 ||
        request.baseline_sample_count == 0) {
        return false;
    }
    if (request.challenge_pulse_count >
            std::numeric_limits<std::uint64_t>::max() /
                request.challenge_stride_sample_count) {
        return false;
    }
    const auto one_direction = request.challenge_pulse_count *
        request.challenge_stride_sample_count;
    if (one_direction >
            std::numeric_limits<std::uint64_t>::max() / 4U) {
        return false;
    }
    std::uint64_t total = one_direction * 4U;
    if (request.peak_hold_sample_count >
            (std::numeric_limits<std::uint64_t>::max() - total) / 2U) {
        return false;
    }
    total += request.peak_hold_sample_count * 2U;
    if (!checked_add(total, request.settle_sample_count, total) ||
        !checked_add(total, request.baseline_sample_count, total) ||
        total > kMaximumS1LivenessSamples) {
        return false;
    }
    output = total;
    return true;
}

bool valid_command_magnitude_request(
        const CommandMagnitudeSequenceRequest& request) noexcept {
    return request.baseline_sample_count ==
               kCommandMagnitudeBaselineSamples &&
           request.response_sample_count ==
               kCommandMagnitudeResponseSamples &&
           request.guard_sample_count == kCommandMagnitudeGuardSamples &&
           (request.run_role == CommandMagnitudeRunRole::PRIMARY ||
            request.run_role == CommandMagnitudeRunRole::HOLDOUT);
}

bool same_physical_b_primary_request(
        const PhysicalBPrimarySequenceRequest& first,
        const PhysicalBPrimarySequenceRequest& second) noexcept {
    return first.guard_sample_count == second.guard_sample_count &&
           first.lfsr_order == second.lfsr_order &&
           first.feedback_mask == second.feedback_mask &&
           first.seed == second.seed &&
           first.phase == second.phase &&
           first.offline_sequence_semantic_sha256 ==
               second.offline_sequence_semantic_sha256;
}

bool valid_physical_b_primary_request(
        const PhysicalBPrimarySequenceRequest& request) noexcept {
    return request.guard_sample_count == kPhysicalBPrimaryGuardSamples &&
           request.lfsr_order == kPhysicalBPrimaryLfsrOrder &&
           request.feedback_mask == kPhysicalBPrimaryFeedbackMask &&
           request.seed == kPhysicalBPrimarySeed &&
           request.phase == kPhysicalBPrimaryPhase &&
           request.offline_sequence_semantic_sha256 ==
               kPhysicalBPrimaryOfflineSequenceSha256;
}

bool same_physical_b_holdout_request(
        const PhysicalBHoldoutSequenceRequest& first,
        const PhysicalBHoldoutSequenceRequest& second) noexcept {
    return first.guard_sample_count == second.guard_sample_count &&
           first.lfsr_order == second.lfsr_order &&
           first.feedback_mask == second.feedback_mask &&
           first.seed == second.seed &&
           first.phase == second.phase &&
           first.offline_sequence_semantic_sha256 ==
               second.offline_sequence_semantic_sha256;
}

bool same_command_magnitude_request(
        const CommandMagnitudeSequenceRequest& first,
        const CommandMagnitudeSequenceRequest& second) noexcept {
    return first.baseline_sample_count == second.baseline_sample_count &&
           first.response_sample_count == second.response_sample_count &&
           first.guard_sample_count == second.guard_sample_count &&
           first.run_role == second.run_role;
}

bool valid_physical_b_holdout_request(
        const PhysicalBHoldoutSequenceRequest& request) noexcept {
    return request.guard_sample_count == kPhysicalBHoldoutGuardSamples &&
           request.lfsr_order == kPhysicalBHoldoutLfsrOrder &&
           request.feedback_mask == kPhysicalBHoldoutFeedbackMask &&
           request.seed == kPhysicalBHoldoutSeed &&
           request.phase == kPhysicalBHoldoutPhase &&
           request.offline_sequence_semantic_sha256 ==
               kPhysicalBHoldoutOfflineSequenceSha256;
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

void append_dependency_calibration_block(
        MouseEffectProbeSequence& sequence,
        std::uint64_t block_id,
        int first_direction,
        const DependencyCalibrationSequenceRequest& request) {
    ProbeSequenceBlock block;
    block.block_id = block_id;
    block.first_sample_index = sequence.samples.size();
    block.first_pulse_dx_counts = first_direction;
    block.second_pulse_dx_counts = -first_direction;

    append_zeros(sequence, block_id, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  first_direction);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 request.response_sample_count);
    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  -first_direction);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 request.response_sample_count);
    append_zeros(sequence, block_id, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);

    block.sample_count = sequence.samples.size() - block.first_sample_index;
    sequence.blocks.push_back(block);
}

void append_s1_liveness_challenge(
        MouseEffectProbeSequence& sequence,
        std::uint64_t block_id,
        int first_direction,
        const S1LivenessSequenceRequest& request) {
    ProbeSequenceBlock block;
    block.block_id = block_id;
    block.first_sample_index = sequence.samples.size();
    block.first_pulse_dx_counts = first_direction;
    block.second_pulse_dx_counts = -first_direction;
    const auto append_direction = [&](int direction) {
        for (std::uint64_t pulse = 0;
             pulse < request.challenge_pulse_count; ++pulse) {
            append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                          direction);
            append_zeros(
                sequence, block_id, ProbeSamplePhase::RESPONSE,
                request.challenge_stride_sample_count - 1U);
        }
    };
    append_direction(first_direction);
    append_zeros(sequence, block_id, ProbeSamplePhase::HOLD,
                 request.peak_hold_sample_count);
    append_direction(-first_direction);
    block.sample_count = sequence.samples.size() - block.first_sample_index;
    sequence.blocks.push_back(block);
}

void summarize_sequence(MouseEffectProbeSequence& sequence) {
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

    summarize_sequence(sequence);
    error.clear();
    return true;
}

bool build_dependency_calibration_sequence(
        const DependencyCalibrationSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    std::uint64_t sample_count = 0;
    if (!expected_dependency_calibration_sample_count(
            request, sample_count)) {
        set_error(error,
            "A2 baseline/response/guard 必须为正，block_count 必须为 4 的倍数且总样本在容量内");
        return false;
    }
    sequence = {};
    sequence.schema = kDependencyCalibrationSequenceSchema;
    sequence.profile = request.run_role ==
            DependencyCalibrationRunRole::P_CAL
        ? kDependencyCalibrationPrimaryProfile
        : kDependencyCalibrationHoldoutProfile;
    sequence.dependency_calibration_request = request;
    sequence.samples.reserve(static_cast<std::size_t>(sample_count));
    sequence.blocks.reserve(static_cast<std::size_t>(request.block_count));

    append_zeros(sequence, 0, ProbeSamplePhase::BASELINE,
                 request.baseline_sample_count);
    constexpr std::array<int, 4> primary_order{1, -1, -1, 1};
    const int role_sign = request.run_role ==
            DependencyCalibrationRunRole::P_CAL ? 1 : -1;
    for (std::uint64_t index = 0; index < request.block_count; ++index) {
        append_dependency_calibration_block(
            sequence, index + 1U,
            primary_order[static_cast<std::size_t>(index % 4U)] *
                role_sign,
            request);
    }
    summarize_sequence(sequence);
    error.clear();
    return true;
}

bool build_s1_liveness_sequence(
        const S1LivenessSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    std::uint64_t sample_count = 0;
    if (!expected_s1_liveness_sample_count(request, sample_count)) {
        set_error(error,
            "A2 S1 challenge/stride/hold/settle/baseline 必须满足固定安全容量");
        return false;
    }
    sequence = {};
    sequence.schema = kS1LivenessSequenceSchema;
    sequence.profile = request.run_role == S1LivenessRunRole::PRIMARY
        ? kS1LivenessPrimaryProfile
        : kS1LivenessValidationProfile;
    sequence.s1_liveness_request = request;
    sequence.samples.reserve(static_cast<std::size_t>(sample_count));
    sequence.blocks.reserve(2U);

    const int role_sign = request.run_role == S1LivenessRunRole::PRIMARY
        ? 1 : -1;
    append_s1_liveness_challenge(sequence, 1U, role_sign, request);
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.settle_sample_count);
    append_zeros(sequence, 0U, ProbeSamplePhase::BASELINE,
                 request.baseline_sample_count);
    append_s1_liveness_challenge(sequence, 2U, -role_sign, request);
    summarize_sequence(sequence);
    error.clear();
    return true;
}

void append_command_magnitude_block(
        MouseEffectProbeSequence& sequence,
        std::uint64_t block_id,
        std::uint64_t pair_index,
        ProbeSequenceBlockRole role,
        ProbeSequenceBlockPolarity polarity,
        int amplitude_counts,
        const CommandMagnitudeSequenceRequest& request) {
    ProbeSequenceBlock block;
    block.block_id = block_id;
    block.pair_index = pair_index;
    block.role = role;
    block.polarity = polarity;
    block.first_sample_index = sequence.samples.size();
    block.amplitude_counts = amplitude_counts;
    const int direction = polarity == ProbeSequenceBlockPolarity::NORMAL
        ? 1 : -1;
    append_zeros(sequence, block_id, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  direction * amplitude_counts);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 request.response_sample_count);
    append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                  -direction * amplitude_counts);
    append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                 request.response_sample_count);
    append_zeros(sequence, block_id, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    block.sample_count = sequence.samples.size() - block.first_sample_index;
    sequence.blocks.push_back(block);
}

bool build_command_magnitude_sequence(
        const CommandMagnitudeSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    if (!valid_command_magnitude_request(request)) {
        set_error(error,
            "Physical B 多幅值 request 必须保持 baseline/response/guard=64/48/32");
        return false;
    }
    constexpr std::array<int, 5> primary_amplitudes{1, 4, 13, 2, 8};
    constexpr std::array<int, 5> holdout_amplitudes{8, 2, 13, 1, 4};
    constexpr std::uint64_t kPairCount = primary_amplitudes.size();
    constexpr std::uint64_t kBlockCount = kPairCount * 2U;
    constexpr std::uint64_t kBlockSampleCount =
        kCommandMagnitudeGuardSamples * 2U +
        kCommandMagnitudeResponseSamples * 2U + 2U;
    constexpr std::uint64_t kExpectedSampleCount =
        kCommandMagnitudeBaselineSamples +
        kBlockCount * kBlockSampleCount;
    static_assert(kExpectedSampleCount == 1684U);

    sequence = {};
    sequence.schema = kCommandMagnitudeSequenceSchema;
    sequence.profile = request.run_role == CommandMagnitudeRunRole::PRIMARY
        ? kCommandMagnitudePrimaryProfile
        : kCommandMagnitudeHoldoutProfile;
    sequence.command_magnitude_request = request;
    sequence.samples.reserve(static_cast<std::size_t>(kExpectedSampleCount));
    sequence.blocks.reserve(static_cast<std::size_t>(kBlockCount));
    append_zeros(sequence, 0U, ProbeSamplePhase::BASELINE,
                 request.baseline_sample_count);

    const auto& amplitudes = request.run_role ==
            CommandMagnitudeRunRole::PRIMARY
        ? primary_amplitudes : holdout_amplitudes;
    std::uint64_t block_id = 1U;
    for (std::size_t pair = 0; pair < amplitudes.size(); ++pair) {
        const auto role = request.run_role == CommandMagnitudeRunRole::HOLDOUT
            ? ProbeSequenceBlockRole::CROSS_RUN_HOLDOUT
            : pair < 3U
                ? ProbeSequenceBlockRole::ESTIMATION
                : ProbeSequenceBlockRole::CONFIRMATION;
        const auto first_polarity = request.run_role ==
                CommandMagnitudeRunRole::PRIMARY
            ? ProbeSequenceBlockPolarity::NORMAL
            : ProbeSequenceBlockPolarity::INVERTED;
        const auto second_polarity = first_polarity ==
                ProbeSequenceBlockPolarity::NORMAL
            ? ProbeSequenceBlockPolarity::INVERTED
            : ProbeSequenceBlockPolarity::NORMAL;
        append_command_magnitude_block(
            sequence, block_id++, pair + 1U, role, first_polarity,
            amplitudes[pair], request);
        append_command_magnitude_block(
            sequence, block_id++, pair + 1U, role, second_polarity,
            amplitudes[pair], request);
    }
    summarize_sequence(sequence);
    if (sequence.samples.size() != kExpectedSampleCount ||
        sequence.blocks.size() != kBlockCount ||
        sequence.net_x_counts != 0 ||
        sequence.max_abs_prefix_x_counts != 13U) {
        set_error(error,
            "Physical B 多幅值 exact schedule 未满足 sample/net/prefix 合同");
        return false;
    }
    error.clear();
    return true;
}

bool build_composite_phase_calibration_sequence(
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    struct PlannedWindow {
        std::string id;
        std::string phase_cell;
        bool negative_control = false;
        int dx_counts = 0;
    };
    struct PlannedPulse {
        std::string id;
        std::string phase_cell;
        int dx_counts = 0;
    };

    constexpr std::array<std::array<std::string_view, 4>, 4> rows{{
        {{"P1_8", "P3_8", "P7_8", "P5_8"}},
        {{"P3_8", "P5_8", "P1_8", "P7_8"}},
        {{"P5_8", "P7_8", "P3_8", "P1_8"}},
        {{"P7_8", "P1_8", "P5_8", "P3_8"}},
    }};
    std::vector<PlannedPulse> pulses;
    pulses.reserve(38U);
    const auto append_pair = [&](std::string block_id,
                                 std::string_view phase_cell,
                                 bool positive_first) {
        const int first = positive_first ? 1 : -1;
        pulses.push_back({block_id + "-1", std::string(phase_cell), first});
        pulses.push_back({block_id + "-2", std::string(phase_cell), -first});
    };

    append_pair("S-BEGIN", "P1_8", true);
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        if (row_index == 2U) {
            append_pair("S-MIDDLE", "P1_8", false);
        }
        for (std::size_t position = 0; position < rows[row_index].size();
             ++position) {
            const std::string block_id = "R" + std::to_string(row_index) +
                "-" + std::to_string(position) + "-" +
                std::string(rows[row_index][position]);
            append_pair(block_id, rows[row_index][position],
                        (row_index + position) % 2U == 0U);
        }
    }
    append_pair("S-END", "P1_8", true);
    if (pulses.size() != 38U) {
        set_error(error, "composite-phase pulse 设计内部数量漂移");
        return false;
    }

    constexpr std::array<std::size_t, 4> control_after{{9U, 17U, 27U, 35U}};
    constexpr std::array<std::string_view, 4> control_cells{{
        "P1_8", "P3_8", "P5_8", "P7_8"}};
    std::vector<PlannedWindow> windows;
    windows.reserve(42U);
    for (std::size_t index = 0; index < pulses.size(); ++index) {
        windows.push_back({pulses[index].id, pulses[index].phase_cell,
                           false, pulses[index].dx_counts});
        const auto match = std::find(
            control_after.begin(), control_after.end(), index);
        if (match != control_after.end()) {
            const auto control_index = static_cast<std::size_t>(
                std::distance(control_after.begin(), match));
            const std::string phase_cell(control_cells[control_index]);
            windows.push_back({"NC-" + phase_cell, phase_cell, true, 0});
        }
    }
    if (windows.size() != 42U) {
        set_error(error, "composite-phase window 设计内部数量漂移");
        return false;
    }

    sequence = {};
    sequence.schema = kCompositePhaseSequenceSchema;
    sequence.profile = kCompositePhaseProfile;
    sequence.composite_phase_request = {
        .predictor_sample_count = kCompositePhasePredictorSamples,
        .window_sample_count = kCompositePhaseWindowSamples,
        .single_magnitude_counts = kCompositePhaseMagnitudeCounts,
        .issue_lead_ns = kCompositePhaseIssueLeadNs,
        .target_tolerance_q32 = kCompositePhaseTargetToleranceQ32,
        .active_guard_ns = kCompositePhaseActiveGuardNs,
        .max_wake_lateness_ns = kCompositePhaseMaxWakeLatenessNs,
        .max_event_interval_width_ns =
            kCompositePhaseMaxEventIntervalWidthNs,
        .max_active_wait_ns_per_event =
            kCompositePhaseMaxActiveWaitPerEventNs,
        .max_active_wait_ns_total = kCompositePhaseMaxActiveWaitTotalNs,
        .timer_mode = std::string(kCompositePhaseTimerMode),
    };
    sequence.samples.reserve(1U + windows.size() * 7U);
    sequence.composite_phase_windows.reserve(windows.size());
    append_zeros(sequence, 0U, ProbeSamplePhase::BASELINE, 1U);
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto& planned = windows[index];
        const std::uint64_t block_id = index + 1U;
        CompositePhaseWindow window;
        window.window_ordinal = index;
        window.window_id = planned.id;
        window.phase_cell = planned.phase_cell;
        window.negative_control = planned.negative_control;
        window.first_sample_index = sequence.samples.size();
        window.sample_count = 1U + kCompositePhaseWindowSamples;
        append_sample(sequence, block_id, ProbeSamplePhase::PULSE,
                      planned.dx_counts);
        append_zeros(sequence, block_id, ProbeSamplePhase::RESPONSE,
                     kCompositePhaseWindowSamples);
        sequence.composite_phase_windows.push_back(std::move(window));
    }
    summarize_sequence(sequence);
    if (sequence.samples.size() != 295U ||
        sequence.composite_phase_windows.size() != 42U ||
        sequence.net_x_counts != 0 ||
        sequence.max_abs_prefix_x_counts != 1U) {
        set_error(error, "composite-phase exact schedule 未满足 sample/net/prefix 合同");
        return false;
    }
    error.clear();
    return true;
}

bool generate_physical_b_primary_period(
        const PhysicalBPrimarySequenceRequest& request,
        std::vector<int>& bits,
        std::string& error) {
    if (!valid_physical_b_primary_request(request)) {
        set_error(error,
            "Physical B Primary request 与冻结 F0 recurrence/guard/SHA 不一致");
        return false;
    }
    const std::uint64_t expected_period =
        (std::uint64_t{1} << request.lfsr_order) - 1U;
    std::uint64_t state = request.seed;
    std::vector<bool> seen(
        static_cast<std::size_t>(std::uint64_t{1} << request.lfsr_order),
        false);
    std::vector<int> unshifted;
    unshifted.reserve(static_cast<std::size_t>(expected_period));
    for (std::uint64_t index = 0; index < expected_period; ++index) {
        if (state == 0 || seen[static_cast<std::size_t>(state)]) {
            set_error(error,
                "Physical B Primary recurrence 未覆盖完整非零状态周期");
            return false;
        }
        seen[static_cast<std::size_t>(state)] = true;
        unshifted.push_back(static_cast<int>(state & 1U));
        const auto feedback = static_cast<std::uint64_t>(
            std::popcount(state & request.feedback_mask) & 1U);
        state = (state >> 1U) |
            (feedback << (request.lfsr_order - 1U));
    }
    if (state != request.seed) {
        set_error(error, "Physical B Primary recurrence 未回到冻结 seed");
        return false;
    }
    const auto offset = static_cast<std::size_t>(
        request.phase % expected_period);
    bits.clear();
    bits.reserve(unshifted.size());
    bits.insert(bits.end(), unshifted.begin() + offset, unshifted.end());
    bits.insert(bits.end(), unshifted.begin(), unshifted.begin() + offset);
    error.clear();
    return true;
}

bool generate_physical_b_holdout_period(
        const PhysicalBHoldoutSequenceRequest& request,
        std::vector<int>& bits,
        std::string& error) {
    if (!valid_physical_b_holdout_request(request)) {
        set_error(error,
            "Physical B holdout request 与冻结 F1 recurrence/guard/SHA 不一致");
        return false;
    }
    const std::uint64_t expected_period =
        (std::uint64_t{1} << request.lfsr_order) - 1U;
    std::uint64_t state = request.seed;
    std::vector<bool> seen(
        static_cast<std::size_t>(std::uint64_t{1} << request.lfsr_order),
        false);
    std::vector<int> unshifted;
    unshifted.reserve(static_cast<std::size_t>(expected_period));
    for (std::uint64_t index = 0; index < expected_period; ++index) {
        if (state == 0 || seen[static_cast<std::size_t>(state)]) {
            set_error(error,
                "Physical B holdout recurrence 未覆盖完整非零状态周期");
            return false;
        }
        seen[static_cast<std::size_t>(state)] = true;
        unshifted.push_back(static_cast<int>(state & 1U));
        const auto feedback = static_cast<std::uint64_t>(
            std::popcount(state & request.feedback_mask) & 1U);
        state = (state >> 1U) |
            (feedback << (request.lfsr_order - 1U));
    }
    if (state != request.seed) {
        set_error(error, "Physical B holdout recurrence 未回到冻结 seed");
        return false;
    }
    const auto offset = static_cast<std::size_t>(
        request.phase % expected_period);
    bits.clear();
    bits.reserve(unshifted.size());
    bits.insert(bits.end(), unshifted.begin() + offset, unshifted.end());
    bits.insert(bits.end(), unshifted.begin(), unshifted.begin() + offset);
    error.clear();
    return true;
}

void append_physical_b_primary_block(
        MouseEffectProbeSequence& sequence,
        std::uint64_t block_id,
        std::uint64_t pair_index,
        ProbeSequenceBlockRole role,
        ProbeSequenceBlockPolarity polarity,
        std::span<const int> bits,
        int& position_x) {
    ProbeSequenceBlock block;
    block.block_id = block_id;
    block.pair_index = pair_index;
    block.role = role;
    block.polarity = polarity;
    block.first_sample_index = sequence.samples.size();
    block.period_sample_count = bits.size();
    const bool inverted = polarity == ProbeSequenceBlockPolarity::INVERTED;
    for (const int bit : bits) {
        const int target_position = inverted ? -bit : bit;
        append_sample(
            sequence, block_id,
            inverted ? ProbeSamplePhase::INVERTED_PERIOD
                     : ProbeSamplePhase::PERIOD,
            target_position - position_x);
        position_x = target_position;
    }
    append_sample(sequence, block_id, ProbeSamplePhase::RETURN_TO_ZERO,
                  -position_x);
    position_x = 0;
    block.sample_count = sequence.samples.size() - block.first_sample_index;
    sequence.blocks.push_back(block);
}

bool build_physical_b_holdout_sequence(
        const PhysicalBHoldoutSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    std::vector<int> bits;
    if (!generate_physical_b_holdout_period(request, bits, error)) {
        return false;
    }
    const std::uint64_t per_block = bits.size() + 1U;
    const std::uint64_t expected_sample_count =
        2U * (per_block + request.guard_sample_count * 2U) +
        request.guard_sample_count;
    if (expected_sample_count > kMaximumSequenceSamples) {
        set_error(error, "Physical B holdout sample count 超出固定容量");
        return false;
    }

    sequence = {};
    sequence.schema = kPhysicalBPrimarySequenceSchema;
    sequence.profile = kPhysicalBHoldoutProfile;
    sequence.physical_b_holdout_request = request;
    sequence.samples.reserve(static_cast<std::size_t>(expected_sample_count));
    sequence.blocks.reserve(2U);

    int position_x = 0;
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_physical_b_primary_block(
        sequence, 1U, 1U, ProbeSequenceBlockRole::CROSS_RUN_HOLDOUT,
        ProbeSequenceBlockPolarity::NORMAL, bits, position_x);
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_physical_b_primary_block(
        sequence, 2U, 1U, ProbeSequenceBlockRole::CROSS_RUN_HOLDOUT,
        ProbeSequenceBlockPolarity::INVERTED, bits, position_x);
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    summarize_sequence(sequence);
    if (position_x != 0 ||
        sequence.samples.size() != expected_sample_count ||
        sequence.net_x_counts != 0 ||
        sequence.max_abs_prefix_x_counts != 1U) {
        set_error(error,
            "Physical B holdout exact schedule 未满足 sample/net/prefix 合同");
        return false;
    }
    error.clear();
    return true;
}

bool build_physical_b_primary_sequence(
        const PhysicalBPrimarySequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) {
    std::vector<int> bits;
    if (!generate_physical_b_primary_period(request, bits, error)) {
        return false;
    }
    constexpr std::uint64_t kPairCount = 3;
    const std::uint64_t per_block = bits.size() + 1U;
    const std::uint64_t expected_sample_count =
        kPairCount * 2U *
            (per_block + request.guard_sample_count * 2U) +
        request.guard_sample_count;
    if (expected_sample_count > kMaximumSequenceSamples) {
        set_error(error, "Physical B Primary sample count 超出固定容量");
        return false;
    }

    sequence = {};
    sequence.schema = kPhysicalBPrimarySequenceSchema;
    sequence.profile = kPhysicalBPrimaryProfile;
    sequence.physical_b_primary_request = request;
    sequence.samples.reserve(static_cast<std::size_t>(expected_sample_count));
    sequence.blocks.reserve(6U);

    int position_x = 0;
    std::uint64_t block_id = 1;
    for (std::uint64_t pair_index = 1;
         pair_index <= kPairCount; ++pair_index) {
        const auto role = pair_index == 1U
            ? ProbeSequenceBlockRole::ESTIMATION
            : pair_index == 2U
                ? ProbeSequenceBlockRole::SELECTION
                : ProbeSequenceBlockRole::CONFIRMATION;
        append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                     request.guard_sample_count);
        append_physical_b_primary_block(
            sequence, block_id++, pair_index, role,
            ProbeSequenceBlockPolarity::NORMAL, bits, position_x);
        append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                     request.guard_sample_count);
        append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                     request.guard_sample_count);
        append_physical_b_primary_block(
            sequence, block_id++, pair_index, role,
            ProbeSequenceBlockPolarity::INVERTED, bits, position_x);
        append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                     request.guard_sample_count);
    }
    append_zeros(sequence, 0U, ProbeSamplePhase::GUARD,
                 request.guard_sample_count);
    summarize_sequence(sequence);
    if (position_x != 0 ||
        sequence.samples.size() != expected_sample_count ||
        sequence.net_x_counts != 0 ||
        sequence.max_abs_prefix_x_counts != 1U) {
        set_error(error,
            "Physical B Primary exact schedule 未满足 sample/net/prefix 合同");
        return false;
    }
    error.clear();
    return true;
}

nlohmann::ordered_json canonical_payload(
        const MouseEffectProbeSequence& sequence) {
    nlohmann::ordered_json blocks = nlohmann::ordered_json::array();
    for (const auto& block : sequence.blocks) {
        if (sequence.schema == kPhysicalBPrimarySequenceSchema) {
            blocks.push_back({
                {"block_id", block.block_id},
                {"pair_index", block.pair_index},
                {"role", probe_sequence_block_role_name(block.role)},
                {"polarity", probe_sequence_block_polarity_name(
                    block.polarity)},
                {"first_sample_index", block.first_sample_index},
                {"period_sample_count", block.period_sample_count},
                {"sample_count", block.sample_count},
            });
        } else if (sequence.schema == kCommandMagnitudeSequenceSchema) {
            blocks.push_back({
                {"block_id", block.block_id},
                {"pair_index", block.pair_index},
                {"role", probe_sequence_block_role_name(block.role)},
                {"polarity", probe_sequence_block_polarity_name(
                    block.polarity)},
                {"amplitude_counts", block.amplitude_counts},
                {"first_sample_index", block.first_sample_index},
                {"sample_count", block.sample_count},
            });
        } else {
            blocks.push_back({
                {"block_id", block.block_id},
                {"first_sample_index", block.first_sample_index},
                {"sample_count", block.sample_count},
                {"first_pulse_dx_counts", block.first_pulse_dx_counts},
                {"second_pulse_dx_counts", block.second_pulse_dx_counts},
            });
        }
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
    nlohmann::ordered_json windows = nlohmann::ordered_json::array();
    for (const auto& window : sequence.composite_phase_windows) {
        windows.push_back({
            {"window_ordinal", window.window_ordinal},
            {"window_id", window.window_id},
            {"phase_cell", window.phase_cell},
            {"negative_control", window.negative_control},
            {"first_sample_index", window.first_sample_index},
            {"sample_count", window.sample_count},
        });
    }
    nlohmann::ordered_json request;
    if (sequence.schema == kSequenceSchema &&
        sequence.profile == kSparsePulseProfile) {
        request = {
            {"baseline_sample_count",
             sequence.request.baseline_sample_count},
            {"response_sample_count",
             sequence.request.response_sample_count},
            {"guard_sample_count", sequence.request.guard_sample_count},
        };
    } else if (sequence.schema == kDependencyCalibrationSequenceSchema) {
        request = {
            {"baseline_sample_count",
             sequence.dependency_calibration_request.baseline_sample_count},
            {"response_sample_count",
             sequence.dependency_calibration_request.response_sample_count},
            {"guard_sample_count",
             sequence.dependency_calibration_request.guard_sample_count},
            {"block_count",
             sequence.dependency_calibration_request.block_count},
            {"run_role", dependency_calibration_run_role_name(
             sequence.dependency_calibration_request.run_role)},
        };
    } else if (sequence.schema == kS1LivenessSequenceSchema) {
        request = {
            {"challenge_pulse_count",
             sequence.s1_liveness_request.challenge_pulse_count},
            {"challenge_stride_sample_count",
             sequence.s1_liveness_request.challenge_stride_sample_count},
        };
        if (sequence.s1_liveness_request.peak_hold_sample_count != 0) {
            request["peak_hold_sample_count"] =
                sequence.s1_liveness_request.peak_hold_sample_count;
        }
        request["settle_sample_count"] =
            sequence.s1_liveness_request.settle_sample_count;
        request["baseline_sample_count"] =
            sequence.s1_liveness_request.baseline_sample_count;
        request["run_role"] = s1_liveness_run_role_name(
            sequence.s1_liveness_request.run_role);
    } else if (sequence.schema == kCommandMagnitudeSequenceSchema) {
        request = {
            {"baseline_sample_count",
             sequence.command_magnitude_request.baseline_sample_count},
            {"response_sample_count",
             sequence.command_magnitude_request.response_sample_count},
            {"guard_sample_count",
             sequence.command_magnitude_request.guard_sample_count},
            {"run_role", command_magnitude_run_role_name(
             sequence.command_magnitude_request.run_role)},
        };
    } else if (sequence.schema == kCompositePhaseSequenceSchema) {
        request = {
            {"predictor_sample_count",
             sequence.composite_phase_request.predictor_sample_count},
            {"window_sample_count",
             sequence.composite_phase_request.window_sample_count},
            {"single_magnitude_counts",
             sequence.composite_phase_request.single_magnitude_counts},
            {"issue_lead_ns",
             sequence.composite_phase_request.issue_lead_ns},
            {"target_tolerance_q32",
             sequence.composite_phase_request.target_tolerance_q32},
            {"active_guard_ns",
             sequence.composite_phase_request.active_guard_ns},
            {"max_wake_lateness_ns",
             sequence.composite_phase_request.max_wake_lateness_ns},
            {"max_event_interval_width_ns",
             sequence.composite_phase_request.max_event_interval_width_ns},
            {"max_active_wait_ns_per_event",
             sequence.composite_phase_request.max_active_wait_ns_per_event},
            {"max_active_wait_ns_total",
             sequence.composite_phase_request.max_active_wait_ns_total},
            {"timer_mode", sequence.composite_phase_request.timer_mode},
        };
    } else if (sequence.profile == kPhysicalBPrimaryProfile) {
        request = {
            {"guard_sample_count",
             sequence.physical_b_primary_request.guard_sample_count},
            {"lfsr_order",
             sequence.physical_b_primary_request.lfsr_order},
            {"feedback_mask",
             sequence.physical_b_primary_request.feedback_mask},
            {"seed", sequence.physical_b_primary_request.seed},
            {"phase", sequence.physical_b_primary_request.phase},
            {"offline_sequence_semantic_sha256",
             sequence.physical_b_primary_request.
                 offline_sequence_semantic_sha256},
        };
    } else {
        request = {
            {"guard_sample_count",
             sequence.physical_b_holdout_request.guard_sample_count},
            {"lfsr_order",
             sequence.physical_b_holdout_request.lfsr_order},
            {"feedback_mask",
             sequence.physical_b_holdout_request.feedback_mask},
            {"seed", sequence.physical_b_holdout_request.seed},
            {"phase", sequence.physical_b_holdout_request.phase},
            {"offline_sequence_semantic_sha256",
             sequence.physical_b_holdout_request.
                 offline_sequence_semantic_sha256},
        };
    }
    nlohmann::ordered_json document = {
        {"schema", sequence.schema},
        {"profile", sequence.profile},
        {"request", std::move(request)},
        {"blocks", std::move(blocks)},
        {"samples", std::move(samples)},
        {"summary", {
            {"net_x_counts", sequence.net_x_counts},
            {"max_abs_prefix_x_counts",
             sequence.max_abs_prefix_x_counts},
        }},
    };
    if (sequence.schema == kCompositePhaseSequenceSchema) {
        document["windows"] = std::move(windows);
    }
    return document;
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

bool same_dependency_calibration_request(
        const DependencyCalibrationSequenceRequest& first,
        const DependencyCalibrationSequenceRequest& second) noexcept {
    return first.baseline_sample_count == second.baseline_sample_count &&
           first.response_sample_count == second.response_sample_count &&
           first.guard_sample_count == second.guard_sample_count &&
           first.block_count == second.block_count &&
           first.run_role == second.run_role;
}

bool same_s1_liveness_request(
        const S1LivenessSequenceRequest& first,
        const S1LivenessSequenceRequest& second) noexcept {
    return first.challenge_pulse_count == second.challenge_pulse_count &&
           first.challenge_stride_sample_count ==
               second.challenge_stride_sample_count &&
           first.peak_hold_sample_count == second.peak_hold_sample_count &&
           first.settle_sample_count == second.settle_sample_count &&
           first.baseline_sample_count == second.baseline_sample_count &&
           first.run_role == second.run_role;
}

bool same_block(const ProbeSequenceBlock& first,
                const ProbeSequenceBlock& second) noexcept {
    return first.block_id == second.block_id &&
           first.pair_index == second.pair_index &&
           first.role == second.role &&
           first.polarity == second.polarity &&
           first.first_sample_index == second.first_sample_index &&
           first.period_sample_count == second.period_sample_count &&
           first.sample_count == second.sample_count &&
           first.amplitude_counts == second.amplitude_counts &&
           first.first_pulse_dx_counts == second.first_pulse_dx_counts &&
           first.second_pulse_dx_counts == second.second_pulse_dx_counts;
}

bool same_composite_phase_request(
        const CompositePhaseSequenceRequest& first,
        const CompositePhaseSequenceRequest& second) noexcept {
    return first.predictor_sample_count == second.predictor_sample_count &&
           first.window_sample_count == second.window_sample_count &&
           first.single_magnitude_counts == second.single_magnitude_counts &&
           first.issue_lead_ns == second.issue_lead_ns &&
           first.target_tolerance_q32 == second.target_tolerance_q32 &&
           first.active_guard_ns == second.active_guard_ns &&
           first.max_wake_lateness_ns == second.max_wake_lateness_ns &&
           first.max_event_interval_width_ns ==
               second.max_event_interval_width_ns &&
           first.max_active_wait_ns_per_event ==
               second.max_active_wait_ns_per_event &&
           first.max_active_wait_ns_total ==
               second.max_active_wait_ns_total &&
           first.timer_mode == second.timer_mode;
}

bool same_composite_phase_window(const CompositePhaseWindow& first,
                                 const CompositePhaseWindow& second) noexcept {
    return first.window_ordinal == second.window_ordinal &&
           first.window_id == second.window_id &&
           first.phase_cell == second.phase_cell &&
           first.negative_control == second.negative_control &&
           first.first_sample_index == second.first_sample_index &&
           first.sample_count == second.sample_count;
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
    } else if (value == "hold") {
        output = ProbeSamplePhase::HOLD;
    } else if (value == "guard") {
        output = ProbeSamplePhase::GUARD;
    } else if (value == "period") {
        output = ProbeSamplePhase::PERIOD;
    } else if (value == "inverted_period") {
        output = ProbeSamplePhase::INVERTED_PERIOD;
    } else if (value == "return_to_zero") {
        output = ProbeSamplePhase::RETURN_TO_ZERO;
    } else {
        return false;
    }
    return true;
}

bool parse_probe_sequence_block_role(
        std::string_view value,
        ProbeSequenceBlockRole& output) noexcept {
    if (value == "estimation") {
        output = ProbeSequenceBlockRole::ESTIMATION;
        return true;
    }
    if (value == "selection") {
        output = ProbeSequenceBlockRole::SELECTION;
        return true;
    }
    if (value == "confirmation") {
        output = ProbeSequenceBlockRole::CONFIRMATION;
        return true;
    }
    if (value == "cross_run_holdout") {
        output = ProbeSequenceBlockRole::CROSS_RUN_HOLDOUT;
        return true;
    }
    return false;
}

bool parse_probe_sequence_block_polarity(
        std::string_view value,
        ProbeSequenceBlockPolarity& output) noexcept {
    if (value == "normal") {
        output = ProbeSequenceBlockPolarity::NORMAL;
        return true;
    }
    if (value == "inverted") {
        output = ProbeSequenceBlockPolarity::INVERTED;
        return true;
    }
    return false;
}

bool parse_dependency_calibration_run_role(
        std::string_view value,
        DependencyCalibrationRunRole& output) noexcept {
    if (value == "p_cal") {
        output = DependencyCalibrationRunRole::P_CAL;
        return true;
    }
    if (value == "p_holdout") {
        output = DependencyCalibrationRunRole::P_HOLDOUT;
        return true;
    }
    return false;
}

bool parse_s1_liveness_run_role(
        std::string_view value,
        S1LivenessRunRole& output) noexcept {
    if (value == "primary") {
        output = S1LivenessRunRole::PRIMARY;
        return true;
    }
    if (value == "validation") {
        output = S1LivenessRunRole::VALIDATION;
        return true;
    }
    return false;
}

bool parse_command_magnitude_run_role(
        std::string_view value,
        CommandMagnitudeRunRole& output) noexcept {
    if (value == "primary") {
        output = CommandMagnitudeRunRole::PRIMARY;
        return true;
    }
    if (value == "holdout") {
        output = CommandMagnitudeRunRole::HOLDOUT;
        return true;
    }
    return false;
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
    const bool base_root = has_exact_keys(document,
        {"schema", "profile", "request", "blocks", "samples",
         "summary", "sequence_sha256"});
    const bool windows_root = has_exact_keys(document,
        {"schema", "profile", "request", "blocks", "samples",
         "summary", "windows", "sequence_sha256"});
    if ((!base_root && !windows_root) ||
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
    const bool sparse_profile =
        candidate.schema == kSequenceSchema &&
        candidate.profile == kSparsePulseProfile;
    const bool dependency_profile =
        candidate.schema == kDependencyCalibrationSequenceSchema &&
        (candidate.profile == kDependencyCalibrationPrimaryProfile ||
         candidate.profile == kDependencyCalibrationHoldoutProfile);
    const bool s1_liveness_profile =
        candidate.schema == kS1LivenessSequenceSchema &&
        (candidate.profile == kS1LivenessPrimaryProfile ||
         candidate.profile == kS1LivenessValidationProfile);
    const bool physical_b_primary_profile =
        candidate.schema == kPhysicalBPrimarySequenceSchema &&
        candidate.profile == kPhysicalBPrimaryProfile;
    const bool physical_b_holdout_profile =
        candidate.schema == kPhysicalBPrimarySequenceSchema &&
        candidate.profile == kPhysicalBHoldoutProfile;
    const bool command_magnitude_profile =
        candidate.schema == kCommandMagnitudeSequenceSchema &&
        (candidate.profile == kCommandMagnitudePrimaryProfile ||
         candidate.profile == kCommandMagnitudeHoldoutProfile);
    const bool composite_phase_profile =
        candidate.schema == kCompositePhaseSequenceSchema &&
        candidate.profile == kCompositePhaseProfile;
    if (composite_phase_profile != windows_root) {
        set_error(error, "composite-phase sequence windows 根字段无效");
        return false;
    }
    if (sparse_profile) {
        if (!has_exact_keys(request,
                {"baseline_sample_count", "response_sample_count",
                 "guard_sample_count"}) ||
            !read_u64(request, "baseline_sample_count",
                      candidate.request.baseline_sample_count) ||
            !read_u64(request, "response_sample_count",
                      candidate.request.response_sample_count) ||
            !read_u64(request, "guard_sample_count",
                      candidate.request.guard_sample_count)) {
            set_error(error, "A 级序列 request 字段集合或类型非法");
            return false;
        }
    } else if (dependency_profile) {
        std::string run_role;
        if (!has_exact_keys(request,
                {"baseline_sample_count", "response_sample_count",
                 "guard_sample_count", "block_count", "run_role"}) ||
            !read_u64(request, "baseline_sample_count",
                      candidate.dependency_calibration_request.
                          baseline_sample_count) ||
            !read_u64(request, "response_sample_count",
                      candidate.dependency_calibration_request.
                          response_sample_count) ||
            !read_u64(request, "guard_sample_count",
                      candidate.dependency_calibration_request.
                          guard_sample_count) ||
            !read_u64(request, "block_count",
                      candidate.dependency_calibration_request.block_count) ||
            !request.at("run_role").is_string()) {
            set_error(error, "A2 序列 request 字段集合或类型非法");
            return false;
        }
        run_role = request.at("run_role").get<std::string>();
        if (!parse_dependency_calibration_run_role(
                run_role,
                candidate.dependency_calibration_request.run_role)) {
            set_error(error, "A2 序列 run_role 非法");
            return false;
        }
    } else if (s1_liveness_profile) {
        std::string run_role;
        const bool legacy_request = has_exact_keys(request,
                {"challenge_pulse_count",
                 "challenge_stride_sample_count", "settle_sample_count",
                 "baseline_sample_count", "run_role"});
        const bool peak_hold_request = has_exact_keys(request,
                {"challenge_pulse_count",
                 "challenge_stride_sample_count", "peak_hold_sample_count",
                 "settle_sample_count", "baseline_sample_count",
                 "run_role"});
        if ((!legacy_request && !peak_hold_request) ||
            !read_u64(request, "challenge_pulse_count",
                      candidate.s1_liveness_request.
                          challenge_pulse_count) ||
            !read_u64(request, "challenge_stride_sample_count",
                      candidate.s1_liveness_request.
                          challenge_stride_sample_count) ||
            (peak_hold_request &&
             !read_u64(request, "peak_hold_sample_count",
                       candidate.s1_liveness_request.
                           peak_hold_sample_count)) ||
            !read_u64(request, "settle_sample_count",
                      candidate.s1_liveness_request.settle_sample_count) ||
            !read_u64(request, "baseline_sample_count",
                      candidate.s1_liveness_request.baseline_sample_count) ||
            !request.at("run_role").is_string()) {
            set_error(error, "A2 S1 活性序列 request 字段集合或类型非法");
            return false;
        }
        run_role = request.at("run_role").get<std::string>();
        if (!parse_s1_liveness_run_role(
                run_role, candidate.s1_liveness_request.run_role)) {
            set_error(error, "A2 S1 活性序列 run_role 非法");
            return false;
        }
    } else if (command_magnitude_profile) {
        std::string run_role;
        if (!has_exact_keys(request,
                {"baseline_sample_count", "response_sample_count",
                 "guard_sample_count", "run_role"}) ||
            !read_u64(request, "baseline_sample_count",
                      candidate.command_magnitude_request.
                          baseline_sample_count) ||
            !read_u64(request, "response_sample_count",
                      candidate.command_magnitude_request.
                          response_sample_count) ||
            !read_u64(request, "guard_sample_count",
                      candidate.command_magnitude_request.
                          guard_sample_count) ||
            !request.at("run_role").is_string()) {
            set_error(error,
                "Physical B 多幅值 request 字段集合或类型非法");
            return false;
        }
        run_role = request.at("run_role").get<std::string>();
        if (!parse_command_magnitude_run_role(
                run_role, candidate.command_magnitude_request.run_role) ||
            (candidate.profile == kCommandMagnitudePrimaryProfile) !=
                (candidate.command_magnitude_request.run_role ==
                 CommandMagnitudeRunRole::PRIMARY)) {
            set_error(error,
                "Physical B 多幅值 profile/run_role 组合非法");
            return false;
        }
    } else if (composite_phase_profile) {
        if (!has_exact_keys(request,
                {"predictor_sample_count", "window_sample_count",
                 "single_magnitude_counts", "issue_lead_ns",
                 "target_tolerance_q32", "active_guard_ns",
                 "max_wake_lateness_ns", "max_event_interval_width_ns",
                 "max_active_wait_ns_per_event",
                 "max_active_wait_ns_total", "timer_mode"}) ||
            !read_u64(request, "predictor_sample_count",
                      candidate.composite_phase_request.
                          predictor_sample_count) ||
            !read_u64(request, "window_sample_count",
                      candidate.composite_phase_request.window_sample_count) ||
            !read_int(request, "single_magnitude_counts",
                      candidate.composite_phase_request.
                          single_magnitude_counts) ||
            !read_i64(request, "issue_lead_ns",
                      candidate.composite_phase_request.issue_lead_ns) ||
            !read_u64(request, "target_tolerance_q32",
                      candidate.composite_phase_request.
                          target_tolerance_q32) ||
            !read_u64(request, "active_guard_ns",
                      candidate.composite_phase_request.active_guard_ns) ||
            !read_u64(request, "max_wake_lateness_ns",
                      candidate.composite_phase_request.
                          max_wake_lateness_ns) ||
            !read_u64(request, "max_event_interval_width_ns",
                      candidate.composite_phase_request.
                          max_event_interval_width_ns) ||
            !read_u64(request, "max_active_wait_ns_per_event",
                      candidate.composite_phase_request.
                          max_active_wait_ns_per_event) ||
            !read_u64(request, "max_active_wait_ns_total",
                      candidate.composite_phase_request.
                          max_active_wait_ns_total) ||
            !request.at("timer_mode").is_string()) {
            set_error(error,
                "Physical B composite-phase request 字段集合或类型非法");
            return false;
        }
        candidate.composite_phase_request.timer_mode =
            request.at("timer_mode").get<std::string>();
    } else if (physical_b_primary_profile) {
        std::uint64_t lfsr_order = 0;
        std::uint64_t feedback_mask = 0;
        if (!has_exact_keys(request,
                {"guard_sample_count", "lfsr_order", "feedback_mask",
                 "seed", "phase", "offline_sequence_semantic_sha256"}) ||
            !read_u64(request, "guard_sample_count",
                      candidate.physical_b_primary_request.
                          guard_sample_count) ||
            !read_u64(request, "lfsr_order", lfsr_order) ||
            lfsr_order > std::numeric_limits<std::uint32_t>::max() ||
            !read_u64(request, "feedback_mask", feedback_mask) ||
            feedback_mask > std::numeric_limits<std::uint32_t>::max() ||
            !read_u64(request, "seed",
                      candidate.physical_b_primary_request.seed) ||
            !read_u64(request, "phase",
                      candidate.physical_b_primary_request.phase) ||
            !request.at("offline_sequence_semantic_sha256").is_string()) {
            set_error(error,
                "Physical B Primary request 字段集合或类型非法");
            return false;
        }
        candidate.physical_b_primary_request.lfsr_order =
            static_cast<std::uint32_t>(lfsr_order);
        candidate.physical_b_primary_request.feedback_mask =
            static_cast<std::uint32_t>(feedback_mask);
        candidate.physical_b_primary_request.
            offline_sequence_semantic_sha256 = request.at(
                "offline_sequence_semantic_sha256").get<std::string>();
    } else if (physical_b_holdout_profile) {
        std::uint64_t lfsr_order = 0;
        std::uint64_t feedback_mask = 0;
        if (!has_exact_keys(request,
                {"guard_sample_count", "lfsr_order", "feedback_mask",
                 "seed", "phase", "offline_sequence_semantic_sha256"}) ||
            !read_u64(request, "guard_sample_count",
                      candidate.physical_b_holdout_request.
                          guard_sample_count) ||
            !read_u64(request, "lfsr_order", lfsr_order) ||
            lfsr_order > std::numeric_limits<std::uint32_t>::max() ||
            !read_u64(request, "feedback_mask", feedback_mask) ||
            feedback_mask > std::numeric_limits<std::uint32_t>::max() ||
            !read_u64(request, "seed",
                      candidate.physical_b_holdout_request.seed) ||
            !read_u64(request, "phase",
                      candidate.physical_b_holdout_request.phase) ||
            !request.at("offline_sequence_semantic_sha256").is_string()) {
            set_error(error,
                "Physical B holdout request 字段集合或类型非法");
            return false;
        }
        candidate.physical_b_holdout_request.lfsr_order =
            static_cast<std::uint32_t>(lfsr_order);
        candidate.physical_b_holdout_request.feedback_mask =
            static_cast<std::uint32_t>(feedback_mask);
        candidate.physical_b_holdout_request.
            offline_sequence_semantic_sha256 = request.at(
                "offline_sequence_semantic_sha256").get<std::string>();
    } else {
        set_error(error, "序列 schema/profile 组合不受支持");
        return false;
    }

    const auto& blocks = document.at("blocks");
    const std::size_t maximum_blocks =
        sparse_profile || s1_liveness_profile ? 2U :
        physical_b_primary_profile ? 6U :
        physical_b_holdout_profile ? 2U :
        command_magnitude_profile ? 10U :
        composite_phase_profile ? 0U :
        static_cast<std::size_t>(kMaximumDependencyCalibrationBlocks);
    if (!blocks.is_array() || blocks.size() > maximum_blocks) {
        set_error(error, "序列 blocks 必须是固定容量数组");
        return false;
    }
    candidate.blocks.reserve(blocks.size());
    for (const auto& value : blocks) {
        ProbeSequenceBlock block;
        if (physical_b_primary_profile || physical_b_holdout_profile) {
            if (!has_exact_keys(value,
                    {"block_id", "pair_index", "role", "polarity",
                     "first_sample_index", "period_sample_count",
                     "sample_count"}) ||
                !value.at("role").is_string() ||
                !value.at("polarity").is_string() ||
                !read_u64(value, "block_id", block.block_id) ||
                !read_u64(value, "pair_index", block.pair_index) ||
                !parse_probe_sequence_block_role(
                    value.at("role").get<std::string>(), block.role) ||
                !parse_probe_sequence_block_polarity(
                    value.at("polarity").get<std::string>(),
                    block.polarity) ||
                !read_u64(value, "first_sample_index",
                          block.first_sample_index) ||
                !read_u64(value, "period_sample_count",
                          block.period_sample_count) ||
                !read_u64(value, "sample_count", block.sample_count)) {
                set_error(error,
                    "Physical B block 字段集合或类型非法");
                return false;
            }
        } else if (command_magnitude_profile) {
            if (!has_exact_keys(value,
                    {"block_id", "pair_index", "role", "polarity",
                     "amplitude_counts", "first_sample_index",
                     "sample_count"}) ||
                !value.at("role").is_string() ||
                !value.at("polarity").is_string() ||
                !read_u64(value, "block_id", block.block_id) ||
                !read_u64(value, "pair_index", block.pair_index) ||
                !parse_probe_sequence_block_role(
                    value.at("role").get<std::string>(), block.role) ||
                !parse_probe_sequence_block_polarity(
                    value.at("polarity").get<std::string>(),
                    block.polarity) ||
                !read_int(value, "amplitude_counts",
                          block.amplitude_counts) ||
                !read_u64(value, "first_sample_index",
                          block.first_sample_index) ||
                !read_u64(value, "sample_count", block.sample_count)) {
                set_error(error,
                    "Physical B 多幅值 block 字段集合或类型非法");
                return false;
            }
        } else {
            if (!has_exact_keys(value,
                    {"block_id", "first_sample_index", "sample_count",
                     "first_pulse_dx_counts", "second_pulse_dx_counts"}) ||
                !read_u64(value, "block_id", block.block_id) ||
                !read_u64(value, "first_sample_index",
                          block.first_sample_index) ||
                !read_u64(value, "sample_count", block.sample_count) ||
                !read_int(value, "first_pulse_dx_counts",
                          block.first_pulse_dx_counts) ||
                !read_int(value, "second_pulse_dx_counts",
                          block.second_pulse_dx_counts)) {
                set_error(error, "序列 block 字段集合、类型或数值非法");
                return false;
            }
        }
        candidate.blocks.push_back(block);
    }

    if (composite_phase_profile) {
        const auto& windows = document.at("windows");
        if (!windows.is_array() || windows.size() != 42U) {
            set_error(error,
                "Physical B composite-phase windows 必须恰为 42 个");
            return false;
        }
        candidate.composite_phase_windows.reserve(windows.size());
        for (const auto& value : windows) {
            CompositePhaseWindow window;
            if (!has_exact_keys(value,
                    {"window_ordinal", "window_id", "phase_cell",
                     "negative_control", "first_sample_index",
                     "sample_count"}) ||
                !read_u64(value, "window_ordinal",
                          window.window_ordinal) ||
                !value.at("window_id").is_string() ||
                !value.at("phase_cell").is_string() ||
                !value.at("negative_control").is_boolean() ||
                !read_u64(value, "first_sample_index",
                          window.first_sample_index) ||
                !read_u64(value, "sample_count", window.sample_count)) {
                set_error(error,
                    "Physical B composite-phase window 字段集合或类型非法");
                return false;
            }
            window.window_id = value.at("window_id").get<std::string>();
            window.phase_cell = value.at("phase_cell").get<std::string>();
            window.negative_control =
                value.at("negative_control").get<bool>();
            candidate.composite_phase_windows.push_back(std::move(window));
        }
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
        case ProbeSamplePhase::HOLD: return "hold";
        case ProbeSamplePhase::GUARD: return "guard";
        case ProbeSamplePhase::PERIOD: return "period";
        case ProbeSamplePhase::INVERTED_PERIOD: return "inverted_period";
        case ProbeSamplePhase::RETURN_TO_ZERO: return "return_to_zero";
    }
    return "unknown";
}

const char* dependency_calibration_run_role_name(
        DependencyCalibrationRunRole role) noexcept {
    switch (role) {
        case DependencyCalibrationRunRole::P_CAL: return "p_cal";
        case DependencyCalibrationRunRole::P_HOLDOUT: return "p_holdout";
    }
    return "unknown";
}

const char* s1_liveness_run_role_name(S1LivenessRunRole role) noexcept {
    switch (role) {
        case S1LivenessRunRole::PRIMARY: return "primary";
        case S1LivenessRunRole::VALIDATION: return "validation";
    }
    return "unknown";
}

const char* command_magnitude_run_role_name(
        CommandMagnitudeRunRole role) noexcept {
    switch (role) {
        case CommandMagnitudeRunRole::PRIMARY: return "primary";
        case CommandMagnitudeRunRole::HOLDOUT: return "holdout";
    }
    return "unknown";
}

const char* probe_sequence_block_role_name(
        ProbeSequenceBlockRole role) noexcept {
    switch (role) {
        case ProbeSequenceBlockRole::UNSPECIFIED: return "unspecified";
        case ProbeSequenceBlockRole::ESTIMATION: return "estimation";
        case ProbeSequenceBlockRole::SELECTION: return "selection";
        case ProbeSequenceBlockRole::CONFIRMATION: return "confirmation";
        case ProbeSequenceBlockRole::CROSS_RUN_HOLDOUT:
            return "cross_run_holdout";
    }
    return "unknown";
}

const char* probe_sequence_block_polarity_name(
        ProbeSequenceBlockPolarity polarity) noexcept {
    switch (polarity) {
        case ProbeSequenceBlockPolarity::UNSPECIFIED: return "unspecified";
        case ProbeSequenceBlockPolarity::NORMAL: return "normal";
        case ProbeSequenceBlockPolarity::INVERTED: return "inverted";
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

bool make_dependency_calibration_sequence(
        const DependencyCalibrationSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_dependency_calibration_sequence(
                request, candidate, error)) {
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) return false;
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error, std::string("生成 A2 依赖校准序列异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成 A2 依赖校准序列时发生未知异常");
        return false;
    }
}

bool make_s1_liveness_sequence(
        const S1LivenessSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_s1_liveness_sequence(request, candidate, error)) {
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) return false;
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error, std::string("生成 A2 S1 活性序列异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成 A2 S1 活性序列时发生未知异常");
        return false;
    }
}

bool make_command_magnitude_sequence(
        const CommandMagnitudeSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_command_magnitude_sequence(request, candidate, error)) {
            sequence = {};
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) {
            sequence = {};
            return false;
        }
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error,
            std::string("生成 Physical B 多幅值序列异常: ") +
            exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成 Physical B 多幅值序列时发生未知异常");
        return false;
    }
}

bool make_composite_phase_calibration_sequence(
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_composite_phase_calibration_sequence(candidate, error)) {
            sequence = {};
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) {
            sequence = {};
            return false;
        }
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error,
            std::string("生成 Physical B composite-phase 序列异常: ") +
            exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error,
            "生成 Physical B composite-phase 序列时发生未知异常");
        return false;
    }
}

bool make_physical_b_primary_sequence(
        const PhysicalBPrimarySequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_physical_b_primary_sequence(request, candidate, error)) {
            sequence = {};
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) {
            sequence = {};
            return false;
        }
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error, std::string("生成 Physical B Primary 序列异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成 Physical B Primary 序列时发生未知异常");
        return false;
    }
}

bool make_physical_b_holdout_sequence(
        const PhysicalBHoldoutSequenceRequest& request,
        MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence candidate;
        if (!build_physical_b_holdout_sequence(request, candidate, error)) {
            sequence = {};
            return false;
        }
        const std::string payload = canonical_payload(candidate).dump();
        if (!sha256(payload, candidate.sequence_sha256, error)) {
            sequence = {};
            return false;
        }
        sequence = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        sequence = {};
        set_error(error, std::string("生成 Physical B holdout 序列异常: ") +
                         exception.what());
        return false;
    } catch (...) {
        sequence = {};
        set_error(error, "生成 Physical B holdout 序列时发生未知异常");
        return false;
    }
}

bool validate_mouse_effect_probe_sequence(
        const MouseEffectProbeSequence& sequence,
        std::string& error) noexcept {
    try {
        MouseEffectProbeSequence expected;
        const bool sparse = sequence.schema == kSequenceSchema &&
            sequence.profile == kSparsePulseProfile;
        const bool dependency =
            sequence.schema == kDependencyCalibrationSequenceSchema &&
            (sequence.profile == kDependencyCalibrationPrimaryProfile ||
             sequence.profile == kDependencyCalibrationHoldoutProfile);
        const bool s1_liveness =
            sequence.schema == kS1LivenessSequenceSchema &&
            (sequence.profile == kS1LivenessPrimaryProfile ||
             sequence.profile == kS1LivenessValidationProfile);
        const bool physical_b_primary =
            sequence.schema == kPhysicalBPrimarySequenceSchema &&
            sequence.profile == kPhysicalBPrimaryProfile;
        const bool physical_b_holdout =
            sequence.schema == kPhysicalBPrimarySequenceSchema &&
            sequence.profile == kPhysicalBHoldoutProfile;
        const bool command_magnitude =
            sequence.schema == kCommandMagnitudeSequenceSchema &&
            (sequence.profile == kCommandMagnitudePrimaryProfile ||
             sequence.profile == kCommandMagnitudeHoldoutProfile);
        const bool composite_phase =
            sequence.schema == kCompositePhaseSequenceSchema &&
            sequence.profile == kCompositePhaseProfile;
        if (sparse) {
            if (!make_sparse_pulse_sequence(
                    sequence.request, expected, error)) {
                return false;
            }
        } else if (dependency) {
            if (!make_dependency_calibration_sequence(
                    sequence.dependency_calibration_request,
                    expected, error)) {
                return false;
            }
        } else if (s1_liveness) {
            if (!make_s1_liveness_sequence(
                    sequence.s1_liveness_request, expected, error)) {
                return false;
            }
        } else if (command_magnitude) {
            if (!make_command_magnitude_sequence(
                    sequence.command_magnitude_request, expected, error)) {
                return false;
            }
        } else if (composite_phase) {
            if (!make_composite_phase_calibration_sequence(
                    expected, error)) {
                return false;
            }
        } else if (physical_b_primary) {
            if (!make_physical_b_primary_sequence(
                    sequence.physical_b_primary_request, expected, error)) {
                return false;
            }
        } else if (physical_b_holdout) {
            if (!make_physical_b_holdout_sequence(
                    sequence.physical_b_holdout_request, expected, error)) {
                return false;
            }
        } else {
            set_error(error, "序列 schema/profile 组合不受支持");
            return false;
        }
        if (sequence.schema != expected.schema ||
            sequence.profile != expected.profile ||
            (sparse && !same_request(
                sequence.request, expected.request)) ||
            (dependency && !same_dependency_calibration_request(
                sequence.dependency_calibration_request,
                expected.dependency_calibration_request)) ||
            (s1_liveness && !same_s1_liveness_request(
                sequence.s1_liveness_request,
                expected.s1_liveness_request)) ||
            (command_magnitude && !same_command_magnitude_request(
                sequence.command_magnitude_request,
                expected.command_magnitude_request)) ||
            (composite_phase && !same_composite_phase_request(
                sequence.composite_phase_request,
                expected.composite_phase_request)) ||
            (physical_b_primary && !same_physical_b_primary_request(
                sequence.physical_b_primary_request,
                expected.physical_b_primary_request)) ||
            (physical_b_holdout && !same_physical_b_holdout_request(
                sequence.physical_b_holdout_request,
                expected.physical_b_holdout_request)) ||
            sequence.net_x_counts != expected.net_x_counts ||
            sequence.max_abs_prefix_x_counts !=
                expected.max_abs_prefix_x_counts ||
            sequence.blocks.size() != expected.blocks.size() ||
            sequence.composite_phase_windows.size() !=
                expected.composite_phase_windows.size() ||
            sequence.samples.size() != expected.samples.size()) {
            set_error(error, "Mouse Effect Probe 序列结构或汇总不符合固定合同");
            return false;
        }
        for (std::size_t index = 0; index < sequence.blocks.size(); ++index) {
            if (!same_block(sequence.blocks[index], expected.blocks[index])) {
                set_error(error, "Mouse Effect Probe block 边界或方向非法");
                return false;
            }
        }
        for (std::size_t index = 0;
             index < sequence.composite_phase_windows.size(); ++index) {
            if (!same_composite_phase_window(
                    sequence.composite_phase_windows[index],
                    expected.composite_phase_windows[index])) {
                set_error(error,
                    "Mouse Effect Probe composite-phase window 合同非法");
                return false;
            }
        }
        for (std::size_t index = 0; index < sequence.samples.size(); ++index) {
            if (!same_sample(sequence.samples[index], expected.samples[index])) {
                set_error(error,
                    "Mouse Effect Probe sample 顺序、相位或 X/Y 位移非法");
                return false;
            }
        }
        if (sequence.sequence_sha256 != expected.sequence_sha256) {
            set_error(error, "Mouse Effect Probe 规范语义 SHA-256 不匹配");
            return false;
        }
        error.clear();
        return true;
    } catch (...) {
        set_error(error, "校验 Mouse Effect Probe 序列时发生未知异常");
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
            !std::isfinite(frame.source_clock_rate) ||
            frame.source_clock_rate <= 0.0 ||
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
        if (impl_->options.dispatch_mode !=
                ProbeDispatchMode::OUTPUT_OFF_REHEARSAL &&
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
        event.source_clock_rate = frame.source_clock_rate;
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
        case ProbeStopReason::SCHEDULER_TIMING_INVALID:
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

} // namespace mouse_effect_probe
