param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$LaunchScript
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

foreach ($path in @($PrepareScript, $LaunchScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "composite-phase script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "composite-phase script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_b_composite_phase_task',
        'physical_b_composite_phase_calibration',
        '--profile physical-b-composite-phase-calibration',
        'AWAITING_AUXILIARY_PREFLIGHT',
        'final_plan_frozen_on_auxiliary_before_sidecar = $true',
        'same_auxiliary_host_preflight_required = $true',
        'response_revealed_before_final_plan = $false',
        'sequence_sample_count = 295',
        'window_count = 42',
        'negative_control_count = 4',
        'expected_nonzero_transition_count = 38',
        'max_abs_prefix_x_counts = 1',
        'minimum_coverage_frames = $minimumSidecarFrames',
        'production_aim_changed = $false',
        'fixed_pixel_speed_used_as_gate = $false',
        'PREPARED_NOT_LAUNCHED',
        'physical_launch_executed = $false',
        'XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT')) {
    if (-not $prepare.Contains($required)) {
        throw "composite-phase Prepare is missing contract text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw "composite-phase Prepare must not execute Launch"
}

foreach ($required in @(
        '$isBCompositeTask',
        'XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT',
        'XenMouseEffectProbeCompositeSeal.exe',
        '--plan-seed',
        '--preflight-output',
        '--plan-output',
        '--composite-plan',
        '--composite-plan-sha256',
        '--composite-schedule-ledger',
        'PHASE_CONFIRMED',
        '[uint64]$task.sidecar.minimum_coverage_frames -ne 1735',
        '[int]$sequence.schema -ne 7',
        '$samples.Count -ne 295',
        '$sequenceWindows.Count -ne 42')) {
    if (-not $launch.Contains($required)) {
        throw "composite-phase Launch is missing contract text: $required"
    }
}
$sealIndex = $launch.IndexOf('--preflight-output')
$sidecarIndex = $launch.IndexOf(
    '$sidecarProcess = Start-Process -FilePath')
if ($sealIndex -lt 0 -or $sidecarIndex -lt 0 -or
    $sealIndex -ge $sidecarIndex) {
    throw "scheduler preflight/final plan must happen before sidecar start"
}
if ($launch.Contains('& ([string]$task.files.ledger_producer.path)') -or
    $launch.Contains('& ([string]$task.files.binder.path)') -or
    $launch.Contains('& ([string]$task.files.evaluator.path)')) {
    throw "Launch must not derive or evaluate composite response evidence"
}

Write-Host "Physical B composite-phase Prepare/Launch contract passed."
