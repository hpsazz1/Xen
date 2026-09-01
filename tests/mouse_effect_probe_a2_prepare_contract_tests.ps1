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
        throw "A2 script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "A2 script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_a2_task',
        'dependency_calibration_a2_p_cal',
        'dependency_calibration_a2_p_holdout',
        'VALID_OFFLINE_PLAN',
        'mouse_effect_probe_a2_s0_synthetic_calibration',
        'mouse_effect_probe_a2_s1_zero_input_calibration',
        'VALID_BRACKETED_CENSORED_ZERO',
        '.ProviderPath',
        'PublishedRunDirectory',
        'XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT',
        'P-HOLDOUT 不得用于回调')) {
    if (-not $prepare.Contains($required)) {
        throw "A2 Prepare is missing contract text: $required"
    }
}
foreach ($required in @(
        'mouse_effect_probe_a2_task',
        'expected_nonzero_transition_count',
        'dependency_calibration_a2_')) {
    if (-not $launch.Contains($required)) {
        throw "A2 Launch recording contract is missing text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw "A2 Prepare must not execute its Physical Launch script"
}

Write-Host "Mouse Effect Probe A2 Prepare/Launch contract passed."
