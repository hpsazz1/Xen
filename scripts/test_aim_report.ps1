$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "aim_report.ps1")
. (Join-Path $PSScriptRoot "aim_control_diagnostics.ps1")

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-AimSample {
    return [ordered]@{
        aim_status = "SUCCESS"
        mouse_sent = $true
        aim_has_target = $true
        aim_has_command = $true
        aim_track_id = 7
        aim_track_state = "CONFIRMED"
        aim_track_predicted = $false
        aim_lead_active = $false
        aim_delay_compensation_active = $true
        aim_base_point_inside_box = $true
        aim_prediction_point_outside_box = $false
        aim_command_toward_target = $true
        aim_acquisition_range_radius = 144.0
        aim_active_range_radius = 50.4
        aim_range_locked = $true
        aim_range_allows_control = $true
        aim_box = @(150.0, 100.0, 230.0, 220.0)
        aim_base_point = @(200.0, 140.0)
        aim_delay_compensated_point = @(206.0, 140.0)
        aim_final_point = @(206.0, 140.0)
        aim_lead = @(0.0, 0.0)
        aim_delay_compensation = @(6.0, 0.0)
        aim_delay_compensation_ms_x = 6.0
        aim_delay_compensation_ms_y = 6.0
        aim_delay_compensation_ms = 6.0
        aim_observation_age_ms = 2.0
        aim_command = @(12, 0)
    }
}

$sample = New-AimSample
$summary = Get-XenAimReportSummary -Samples @($sample) `
    -PredictionEnabled off -MaxPredictionLeadPercent 35.0
Assert-Condition ([bool]$summary.contract_valid) `
    "Valid delay compensation must not be reported as prediction lead."
Assert-Condition ($summary.violation_count -eq 0) `
    "Valid delay compensation must not produce contract violations."

$sample.aim_delay_compensated_point = @(205.0, 140.0)
$invalid = Get-XenAimReportSummary -Samples @($sample) `
    -PredictionEnabled off -MaxPredictionLeadPercent 35.0
Assert-Condition (-not [bool]$invalid.contract_valid) `
    "Inconsistent delay-compensated point must be rejected."
Assert-Condition `
    ($invalid.violations.lead_vector_consistency_frames -eq 1) `
    "Inconsistent delay geometry must produce one vector violation."

$sample = New-AimSample
$sample.aim_delay_compensation_ms = 5.0
$invalid = Get-XenAimReportSummary -Samples @($sample) `
    -PredictionEnabled off -MaxPredictionLeadPercent 35.0
Assert-Condition (-not [bool]$invalid.contract_valid) `
    "Legacy delay horizon must equal the maximum per-axis horizon."

function New-ControlDiagnosticSample(
        [uint64]$Sequence,
        [int]$CommandX,
        [string]$ZeroReason) {
    $sample = New-AimSample
    $sample.sequence = $Sequence
    $sample.aim_control_center_x = 160.0
    $sample.source_pixels_per_pixel_x = 1.0
    $sample.aim_final_point = @(172.0, 140.0)
    $sample.aim_command = @($CommandX, 0)
    $sample.aim_control_evaluated = $true
    $sample.aim_controller_dt_ms = 4.167
    $sample.aim_desired_x_counts = if ($CommandX -eq 0) { -2.0 } else {
        [double]$CommandX
    }
    $sample.aim_pending_absolute_x_counts = 3.0
    $sample.aim_reverse_candidate_x = $Sequence -eq 4
    $sample.aim_reverse_previous_direction_pending_x = $false
    $sample.aim_reverse_deformation_active_x = $false
    $sample.aim_reverse_evidence_ratio_seconds_x = 0.0
    $sample.aim_reverse_position_ratio_seconds_x = 0.0
    $sample.aim_reverse_position_peak_error_x = 18.0
    $sample.aim_reverse_translation_seconds_x = if ($Sequence -eq 4) {
        0.015
    } else { 0.0 }
    $sample.aim_reverse_required_evidence_ratio_seconds_x = 0.00042
    $sample.aim_reverse_required_position_ratio_seconds_x = 0.0015
    $sample.aim_reverse_probe_direction_x = if ($Sequence -eq 4) {
        -1.0
    } else { 0.0 }
    $sample.aim_reverse_probe_age_ms_x = if ($Sequence -eq 4) {
        8.0
    } else { 0.0 }
    $sample.aim_reverse_evidence_ready_x = $false
    $sample.aim_reverse_translation_ready_x = $Sequence -eq 4
    $sample.aim_reverse_position_ready_x = $false
    $sample.aim_reverse_position_improvement_reset_x = $Sequence -eq 4
    $sample.aim_reverse_gate_blocked_x =
        $ZeroReason -eq "reverse_gate"
    $sample.aim_reverse_probe_active_x = $Sequence -eq 4
    $sample.aim_reverse_probe_limited_x = $Sequence -eq 4
    $sample.aim_pending_inventory_hold_blocked_x =
        $ZeroReason -eq "pending_inventory_hold"
    $sample.aim_deadzone_quiet = $ZeroReason -eq "deadzone_quiet"
    $sample.aim_shaper_direction_reset_x =
        $ZeroReason -eq "shaper_direction_reset"
    $sample.aim_post_alignment_sign_change_blocked_x =
        $ZeroReason -eq "post_alignment_sign_change"
    $sample.aim_post_alignment_growth_limited_x = $false
    $sample.aim_integer_direction_blocked_x =
        $ZeroReason -eq "integer_direction"
    $sample.aim_command_sign_change_blocked_x =
        $ZeroReason -eq "command_sign_change"
    $sample.aim_quantization_zero_x = $ZeroReason -eq "quantization"
    return $sample
}

$controlSamples = @(
    (New-ControlDiagnosticSample 1 2 ""),
    (New-ControlDiagnosticSample 2 0 "reverse_gate"),
    (New-ControlDiagnosticSample 3 0 "pending_inventory_hold"),
    (New-ControlDiagnosticSample 4 -1 ""),
    (New-ControlDiagnosticSample 5 0 "shaper_direction_reset"),
    (New-ControlDiagnosticSample 6 0 "quantization"),
    (New-ControlDiagnosticSample 7 1 ""))
$controlSummary = Get-XenAimControlDiagnosticsSummary $controlSamples
Assert-Condition ($controlSummary.controllable_frames -eq 7 -and
        $controlSummary.x.command_zero_frames -eq 4 -and
        $controlSummary.x.stopped_final_error_over_hold_band_pixels -eq 4 -and
        $controlSummary.x.stopped_final_error_over_10_pixels -eq 4) `
    "Control diagnostics must summarize eligible zero-output frames."
Assert-Condition ($controlSummary.x.zero_primary_causes.reverse_gate -eq 1 -and
        $controlSummary.x.zero_primary_causes.pending_inventory_hold -eq 1 -and
        $controlSummary.x.zero_primary_causes.shaper_direction_reset -eq 1 -and
        $controlSummary.x.zero_primary_causes.quantization -eq 1) `
    "Control diagnostics must classify every zero-output frame once."
Assert-Condition ($controlSummary.x.nonzero_direction_reversals -eq 2 -and
        $controlSummary.x.reversal_zero_frames.p50 -eq 2 -and
        $controlSummary.x.reversal_window_dominant_causes.reverse_gate -eq 1 -and
        $controlSummary.x.reversal_window_dominant_causes.shaper_direction_reset `
            -eq 1) `
    "Control diagnostics must summarize reversal zero-window causes."
Assert-Condition ($controlSummary.schema -eq 4 -and
        [bool]$controlSummary.reverse_probe_diagnostics_available -and
        [bool]$controlSummary.reverse_translation_diagnostics_available -and
        [bool]$controlSummary.reverse_position_improvement_diagnostics_available -and
        $controlSummary.x.diagnostic_flags.reverse_probe_active -eq 1 -and
        $controlSummary.x.diagnostic_flags.reverse_probe_limited -eq 1 -and
        $controlSummary.x.diagnostic_flags.reverse_translation_ready -eq 1 -and
        $controlSummary.x.diagnostic_flags.reverse_position_improvement_reset `
            -eq 1 -and
        $controlSummary.x.reverse_probe_age_ms.p50 -eq 8.0 -and
        $controlSummary.x.reverse_translation_dwell_ms.p50 -eq 15.0 -and
        $controlSummary.x.reverse_position_peak_error_pixels.p50 -eq 18.0) `
    "Control diagnostics must summarize translation probes and position resets."

$legacyControlSamples = foreach ($controlSample in $controlSamples) {
    $legacy = [ordered]@{}
    foreach ($key in $controlSample.Keys) {
        if ($key -notlike "aim_reverse_probe_*" -and
            $key -notlike "aim_reverse_translation_*" -and
            $key -notlike "aim_reverse_position_peak_*" -and
            $key -notlike "aim_reverse_position_improvement_*") {
            $legacy[$key] = $controlSample[$key]
        }
    }
    $legacy
}
$legacyControlSummary = Get-XenAimControlDiagnosticsSummary `
    $legacyControlSamples
Assert-Condition (-not [bool]$legacyControlSummary.reverse_probe_diagnostics_available -and
        -not [bool]$legacyControlSummary.reverse_translation_diagnostics_available -and
        -not [bool]$legacyControlSummary.reverse_position_improvement_diagnostics_available -and
        $legacyControlSummary.x.nonzero_direction_reversals -eq 2) `
    "Schema 8/9/10 control diagnostics must remain backward-compatible."

Write-Host "Aim report and control-diagnostics tests passed."
