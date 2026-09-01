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
        throw "A2 S1 script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "A2 S1 script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_a2_s1_task',
        'dependency_calibration_a2_s1_$RunRole',
        '[ValidateSet("primary", "validation")]',
        '--profile s1-liveness-a2',
        'challenge_pulse_count',
        'challenge_stride_sample_count',
        'PREPARED_NOT_LAUNCHED',
        'physical_launch_executed = $false',
        'XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT',
        'Move-Item -LiteralPath $stagingDirectory')) {
    if (-not $prepare.Contains($required)) {
        throw "A2 S1 Prepare is missing contract text: $required"
    }
}
foreach ($required in @(
        'mouse_effect_probe_a2_s1_task',
        'bracketed_kmbox',
        's1-liveness-bracket.json',
        'baseline_actual_command_zero',
        'challenge_frames_excluded_from_estimands')) {
    if (-not $launch.Contains($required)) {
        throw "A2 S1 Launch recording contract is missing text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw "A2 S1 Prepare must not execute its Physical Launch script"
}

Write-Host "Mouse Effect Probe A2 S1 Prepare/Launch contract passed."
