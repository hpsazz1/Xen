$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "aim_report.ps1")

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

Write-Host "Aim report delay-compensation contract tests passed."
