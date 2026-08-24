$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-XenAimControlPrimaryZeroReason {
    param([Parameter(Mandatory = $true)][object]$Sample)

    if ([bool]$Sample.aim_reverse_gate_blocked_x) {
        return "reverse_gate"
    }
    if ([bool]$Sample.aim_pending_inventory_hold_blocked_x) {
        return "pending_inventory_hold"
    }
    if ([bool]$Sample.aim_deadzone_quiet) {
        return "deadzone_quiet"
    }
    if ([bool]$Sample.aim_shaper_direction_reset_x) {
        return "shaper_direction_reset"
    }
    if ([bool]$Sample.aim_post_alignment_sign_change_blocked_x) {
        return "post_alignment_sign_change"
    }
    if ([bool]$Sample.aim_integer_direction_blocked_x) {
        return "integer_direction"
    }
    if ([bool]$Sample.aim_command_sign_change_blocked_x) {
        return "command_sign_change"
    }
    if ([bool]$Sample.aim_quantization_zero_x) {
        return "quantization"
    }
    if ([Math]::Abs([double]$Sample.aim_desired_x_counts) -le 0.001) {
        return "desired_zero"
    }
    return "unclassified"
}

function Get-XenAimControlDiagnosticsSummary {
    param(
        [Parameter(Mandatory = $true)][object[]]$Samples,
        [ValidateRange(0.0, 100.0)][double]$HoldBandPixels = 2.25
    )

    $items = @($Samples)
    if ($items.Count -eq 0) {
        throw "控制诊断没有 Runtime 样本。"
    }
    $requiredFields = @(
        "sequence", "aim_status", "aim_has_target", "aim_track_id",
        "aim_track_state", "aim_range_locked", "aim_range_allows_control",
        "aim_final_point", "aim_control_center_x",
        "source_pixels_per_pixel_x", "aim_command",
        "aim_control_evaluated", "aim_controller_dt_ms",
        "aim_desired_x_counts", "aim_pending_absolute_x_counts",
        "aim_reverse_candidate_x",
        "aim_reverse_previous_direction_pending_x",
        "aim_reverse_deformation_active_x",
        "aim_reverse_evidence_ratio_seconds_x",
        "aim_reverse_position_ratio_seconds_x",
        "aim_reverse_required_evidence_ratio_seconds_x",
        "aim_reverse_required_position_ratio_seconds_x",
        "aim_reverse_evidence_ready_x", "aim_reverse_position_ready_x",
        "aim_reverse_gate_blocked_x",
        "aim_pending_inventory_hold_blocked_x", "aim_deadzone_quiet",
        "aim_shaper_direction_reset_x",
        "aim_post_alignment_sign_change_blocked_x",
        "aim_post_alignment_growth_limited_x",
        "aim_closing_response_tapered_x",
        "aim_integer_direction_blocked_x",
        "aim_command_sign_change_blocked_x", "aim_quantization_zero_x")
    $availableFields = if ($items[0] -is
            [System.Collections.IDictionary]) {
        @($items[0].Keys)
    } else {
        @($items[0].PSObject.Properties.Name)
    }
    $missingFields = @($requiredFields | Where-Object {
        $availableFields -notcontains $_
    })
    if ($missingFields.Count -ne 0) {
        throw "Runtime 样本缺少控制诊断字段：$($missingFields -join ', ')"
    }
    $probeFields = @(
        "aim_reverse_probe_direction_x", "aim_reverse_probe_age_ms_x",
        "aim_reverse_probe_active_x", "aim_reverse_probe_limited_x")
    $probeDiagnosticsAvailable = @($probeFields | Where-Object {
        $availableFields -notcontains $_
    }).Count -eq 0
    $translationFields = @(
        "aim_reverse_translation_seconds_x",
        "aim_reverse_translation_ready_x")
    $translationDiagnosticsAvailable = @($translationFields | Where-Object {
        $availableFields -notcontains $_
    }).Count -eq 0
    $translationDetailFields = @(
        "aim_reverse_output_direction_x",
        "aim_reverse_translation_raw_left_x_roi_pixels",
        "aim_reverse_translation_raw_right_x_roi_pixels",
        "aim_reverse_translation_raw_common_x_roi_pixels",
        "aim_reverse_translation_control_evidence_x",
        "aim_reverse_translation_gap_seconds_x",
        "aim_reverse_translation_fresh_evidence_x",
        "aim_reverse_translation_reset_reason_x")
    $translationDetailDiagnosticsAvailable = @(
        $translationDetailFields | Where-Object {
            $availableFields -notcontains $_
        }).Count -eq 0
    $positionImprovementFields = @(
        "aim_reverse_position_peak_error_x",
        "aim_reverse_position_improvement_reset_x")
    $positionImprovementDiagnosticsAvailable = @(
        $positionImprovementFields | Where-Object {
            $availableFields -notcontains $_
        }).Count -eq 0

    $reasonNames = @(
        "reverse_gate", "pending_inventory_hold", "deadzone_quiet",
        "shaper_direction_reset", "post_alignment_sign_change",
        "integer_direction", "command_sign_change", "quantization",
        "desired_zero", "unclassified")
    $zeroReasons = [ordered]@{}
    $reversalWindowReasons = [ordered]@{}
    foreach ($name in $reasonNames) {
        $zeroReasons[$name] = [uint64]0
        $reversalWindowReasons[$name] = [uint64]0
    }
    $reversalWindowReasons["no_zero"] = [uint64]0
    $translationResetReasonNames = @(
        "NONE", "CANDIDATE_INACTIVE", "BASE_NOT_ALIGNED",
        "PARTIAL_SEMANTICS", "PREVIOUS_DIRECTION_PENDING",
        "ZERO_TRANSLATION", "OPPOSING_TRANSLATION",
        "WEAK_BUDGET_EXHAUSTED", "WEAK_WITHOUT_STRONG_HISTORY")
    $translationResetReasons = [ordered]@{}
    foreach ($name in $translationResetReasonNames) {
        $translationResetReasons[$name] = [uint64]0
    }

    $flags = [ordered]@{
        reverse_candidate = [uint64]0
        reverse_previous_direction_pending = [uint64]0
        reverse_deformation_active = [uint64]0
        reverse_evidence_ready = [uint64]0
        reverse_translation_ready = [uint64]0
        reverse_translation_fresh_evidence = [uint64]0
        reverse_position_ready = [uint64]0
        reverse_position_improvement_reset = [uint64]0
        reverse_gate_blocked = [uint64]0
        reverse_probe_active = [uint64]0
        reverse_probe_limited = [uint64]0
        pending_inventory_hold_blocked = [uint64]0
        post_alignment_growth_limited = [uint64]0
        closing_response_tapered = [uint64]0
    }
    $controllerDt = [System.Collections.Generic.List[double]]::new()
    $zeroError = [System.Collections.Generic.List[double]]::new()
    $zeroPending = [System.Collections.Generic.List[double]]::new()
    $evidenceProgress = [System.Collections.Generic.List[double]]::new()
    $positionProgress = [System.Collections.Generic.List[double]]::new()
    $positionPeakError = [System.Collections.Generic.List[double]]::new()
    $translationDwell = [System.Collections.Generic.List[double]]::new()
    $translationGap = [System.Collections.Generic.List[double]]::new()
    $translationAlignedRawLeft =
        [System.Collections.Generic.List[double]]::new()
    $translationAlignedRawRight =
        [System.Collections.Generic.List[double]]::new()
    $translationAlignedRawCommon =
        [System.Collections.Generic.List[double]]::new()
    $translationAlignedControlEvidence =
        [System.Collections.Generic.List[double]]::new()
    $probeAge = [System.Collections.Generic.List[double]]::new()
    $reversalZeroFrames = [System.Collections.Generic.List[double]]::new()

    [uint64]$controlFrames = 0
    [uint64]$diagnosticsMissingFrames = 0
    [uint64]$zeroFrames = 0
    [uint64]$zeroOverHoldBand = 0
    [uint64]$zeroOverTenPixels = 0
    [uint64]$zeroOverTwentyPixels = 0
    [uint64]$directionReversals = 0
    [int]$previousNonzeroSign = 0
    [uint64]$previousTrackId = 0
    [uint64]$previousSequence = 0
    $zeroWindowReasons = [System.Collections.Generic.List[string]]::new()

    foreach ($sample in $items) {
        $sequence = [uint64]$sample.sequence
        if ($previousSequence -ne 0 -and $sequence -le $previousSequence) {
            // 多段报告可能来自独立 Runtime 生命周期；序号未继续增长时，
            // 不得把上一段末尾与下一段开头拼成一次虚假的物理反转。
            $previousNonzeroSign = 0
            $previousTrackId = 0
            $zeroWindowReasons.Clear()
        }
        $previousSequence = $sequence
        $controllable = [string]$sample.aim_status -eq "SUCCESS" -and
            [bool]$sample.aim_has_target -and
            [string]$sample.aim_track_state -eq "CONFIRMED" -and
            [bool]$sample.aim_range_locked -and
            [bool]$sample.aim_range_allows_control
        if (-not $controllable) {
            $previousNonzeroSign = 0
            $previousTrackId = 0
            $zeroWindowReasons.Clear()
            continue
        }
        ++$controlFrames
        if (-not [bool]$sample.aim_control_evaluated) {
            ++$diagnosticsMissingFrames
            $previousNonzeroSign = 0
            $previousTrackId = 0
            $zeroWindowReasons.Clear()
            continue
        }

        $trackId = [uint64]$sample.aim_track_id
        if ($previousTrackId -ne 0 -and $previousTrackId -ne $trackId) {
            $previousNonzeroSign = 0
            $zeroWindowReasons.Clear()
        }
        $previousTrackId = $trackId
        $controllerDt.Add([double]$sample.aim_controller_dt_ms)

        $flagMappings = @(
                @("reverse_candidate", "aim_reverse_candidate_x"),
                @("reverse_previous_direction_pending",
                  "aim_reverse_previous_direction_pending_x"),
                @("reverse_deformation_active",
                  "aim_reverse_deformation_active_x"),
                @("reverse_evidence_ready",
                  "aim_reverse_evidence_ready_x"),
                @("reverse_position_ready",
                  "aim_reverse_position_ready_x"),
                @("reverse_gate_blocked",
                  "aim_reverse_gate_blocked_x"),
                @("pending_inventory_hold_blocked",
                  "aim_pending_inventory_hold_blocked_x"),
                @("post_alignment_growth_limited",
                  "aim_post_alignment_growth_limited_x"),
                @("closing_response_tapered",
                  "aim_closing_response_tapered_x"))
        if ($probeDiagnosticsAvailable) {
            $flagMappings += ,@(
                "reverse_probe_active", "aim_reverse_probe_active_x")
            $flagMappings += ,@(
                "reverse_probe_limited", "aim_reverse_probe_limited_x")
        }
        if ($translationDiagnosticsAvailable) {
            $flagMappings += ,@(
                "reverse_translation_ready",
                "aim_reverse_translation_ready_x")
        }
        if ($translationDetailDiagnosticsAvailable) {
            $flagMappings += ,@(
                "reverse_translation_fresh_evidence",
                "aim_reverse_translation_fresh_evidence_x")
        }
        if ($positionImprovementDiagnosticsAvailable) {
            $flagMappings += ,@(
                "reverse_position_improvement_reset",
                "aim_reverse_position_improvement_reset_x")
        }
        foreach ($entry in $flagMappings) {
            $propertyName = [string]$entry[1]
            if ([bool]$sample.$propertyName) {
                ++$flags[[string]$entry[0]]
            }
        }
        $requiredEvidence =
            [double]$sample.aim_reverse_required_evidence_ratio_seconds_x
        if ([bool]$sample.aim_reverse_candidate_x -and
            $requiredEvidence -gt 0.0) {
            $evidenceProgress.Add(
                [double]$sample.aim_reverse_evidence_ratio_seconds_x /
                    $requiredEvidence)
        }
        $requiredPosition =
            [double]$sample.aim_reverse_required_position_ratio_seconds_x
        if ([bool]$sample.aim_reverse_candidate_x -and
            $requiredPosition -gt 0.0) {
            $positionProgress.Add(
                [double]$sample.aim_reverse_position_ratio_seconds_x /
                    $requiredPosition)
        }
        if ($positionImprovementDiagnosticsAvailable -and
            [bool]$sample.aim_reverse_candidate_x) {
            $positionPeakError.Add(
                [double]$sample.aim_reverse_position_peak_error_x)
        }
        if ($probeDiagnosticsAvailable -and
            [bool]$sample.aim_reverse_probe_active_x) {
            $probeAge.Add([double]$sample.aim_reverse_probe_age_ms_x)
        }
        if ($translationDiagnosticsAvailable -and
            [bool]$sample.aim_reverse_candidate_x) {
            $translationDwell.Add(
                [double]$sample.aim_reverse_translation_seconds_x * 1000.0)
        }
        if ($translationDetailDiagnosticsAvailable -and
            [bool]$sample.aim_reverse_candidate_x) {
            $resetReason =
                [string]$sample.aim_reverse_translation_reset_reason_x
            if ($translationResetReasonNames -notcontains $resetReason) {
                throw "序号 $sequence 的反向平移清零原因无效：$resetReason"
            }
            ++$translationResetReasons[$resetReason]
            $candidateDirection = -[Math]::Sign(
                [double]$sample.aim_reverse_output_direction_x)
            $translationGap.Add(
                [double]$sample.aim_reverse_translation_gap_seconds_x *
                    1000.0)
            $translationAlignedRawLeft.Add(
                [double]$sample.aim_reverse_translation_raw_left_x_roi_pixels *
                    $candidateDirection)
            $translationAlignedRawRight.Add(
                [double]$sample.aim_reverse_translation_raw_right_x_roi_pixels *
                    $candidateDirection)
            $translationAlignedRawCommon.Add(
                [double]$sample.aim_reverse_translation_raw_common_x_roi_pixels *
                    $candidateDirection)
            $translationAlignedControlEvidence.Add(
                [double]$sample.aim_reverse_translation_control_evidence_x *
                    $candidateDirection)
        }

        $command = @($sample.aim_command)
        $finalPoint = @($sample.aim_final_point)
        if ($command.Count -ne 2 -or $finalPoint.Count -ne 2) {
            throw "序号 $($sample.sequence) 的 Aim 向量维度无效。"
        }
        $commandX = [int]$command[0]
        if ($commandX -eq 0) {
            ++$zeroFrames
            $reason = Get-XenAimControlPrimaryZeroReason $sample
            ++$zeroReasons[$reason]
            if ($previousNonzeroSign -ne 0) {
                $zeroWindowReasons.Add($reason)
            }
            $absoluteError = [Math]::Abs(
                ([double]$finalPoint[0] -
                 [double]$sample.aim_control_center_x) *
                [double]$sample.source_pixels_per_pixel_x)
            $zeroError.Add($absoluteError)
            $zeroPending.Add(
                [double]$sample.aim_pending_absolute_x_counts)
            if ($absoluteError -gt $HoldBandPixels) { ++$zeroOverHoldBand }
            if ($absoluteError -gt 10.0) { ++$zeroOverTenPixels }
            if ($absoluteError -gt 20.0) { ++$zeroOverTwentyPixels }
            continue
        }

        $commandSign = if ($commandX -lt 0) { -1 } else { 1 }
        if ($previousNonzeroSign -ne 0 -and
            $commandSign -ne $previousNonzeroSign) {
            ++$directionReversals
            $reversalZeroFrames.Add([double]$zeroWindowReasons.Count)
            if ($zeroWindowReasons.Count -eq 0) {
                ++$reversalWindowReasons["no_zero"]
            } else {
                $dominantReason = $reasonNames[0]
                [int]$dominantCount = -1
                foreach ($name in $reasonNames) {
                    $count = @($zeroWindowReasons | Where-Object {
                        $_ -eq $name
                    }).Count
                    if ($count -gt $dominantCount) {
                        $dominantReason = $name
                        $dominantCount = $count
                    }
                }
                ++$reversalWindowReasons[$dominantReason]
            }
        }
        $previousNonzeroSign = $commandSign
        $zeroWindowReasons.Clear()
    }

    [uint64]$diagnosedFrames = $controlFrames - $diagnosticsMissingFrames
    $zeroRate = if ($diagnosedFrames -eq 0) { 0.0 } else {
        [double]$zeroFrames / [double]$diagnosedFrames
    }
    return [ordered]@{
        schema = 5
        sample_count = $items.Count
        controllable_frames = $controlFrames
        diagnosed_frames = $diagnosedFrames
        diagnostics_missing_frames = $diagnosticsMissingFrames
        reverse_probe_diagnostics_available = $probeDiagnosticsAvailable
        reverse_translation_diagnostics_available =
            $translationDiagnosticsAvailable
        reverse_translation_detail_diagnostics_available =
            $translationDetailDiagnosticsAvailable
        reverse_position_improvement_diagnostics_available =
            $positionImprovementDiagnosticsAvailable
        x = [ordered]@{
            command_zero_frames = $zeroFrames
            command_nonzero_frames = $diagnosedFrames - $zeroFrames
            command_zero_rate = $zeroRate
            stopped_final_error_over_hold_band_pixels = $zeroOverHoldBand
            hold_band_pixels = $HoldBandPixels
            stopped_final_error_over_10_pixels = $zeroOverTenPixels
            stopped_final_error_over_20_pixels = $zeroOverTwentyPixels
            zero_primary_causes = $zeroReasons
            diagnostic_flags = $flags
            reverse_translation_reset_reasons = $translationResetReasons
            nonzero_direction_reversals = $directionReversals
            reversal_window_dominant_causes = $reversalWindowReasons
            reversal_zero_frames =
                Get-XenAimDistributionSummary $reversalZeroFrames
            stopped_absolute_final_error_pixels =
                Get-XenAimDistributionSummary $zeroError
            stopped_pending_absolute_counts =
                Get-XenAimDistributionSummary $zeroPending
            controller_dt_ms = Get-XenAimDistributionSummary $controllerDt
            reverse_evidence_progress_ratio =
                Get-XenAimDistributionSummary $evidenceProgress
            reverse_position_progress_ratio =
                Get-XenAimDistributionSummary $positionProgress
            reverse_position_peak_error_pixels =
                Get-XenAimDistributionSummary $positionPeakError
            reverse_probe_age_ms =
                Get-XenAimDistributionSummary $probeAge
            reverse_translation_dwell_ms =
                Get-XenAimDistributionSummary $translationDwell
            reverse_translation_gap_ms =
                Get-XenAimDistributionSummary $translationGap
            reverse_translation_aligned_raw_left_roi_pixels =
                Get-XenAimDistributionSummary $translationAlignedRawLeft
            reverse_translation_aligned_raw_right_roi_pixels =
                Get-XenAimDistributionSummary $translationAlignedRawRight
            reverse_translation_aligned_raw_common_roi_pixels =
                Get-XenAimDistributionSummary $translationAlignedRawCommon
            reverse_translation_aligned_control_evidence =
                Get-XenAimDistributionSummary $translationAlignedControlEvidence
        }
    }
}
