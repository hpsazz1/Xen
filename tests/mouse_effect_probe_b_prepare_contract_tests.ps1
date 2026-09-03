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
        throw "Physical B script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "Physical B script has parse errors: $path line=$($errors[0].Extent.StartLineNumber) $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'XEN_MOUSE_EFFECT_PROBE_B_PRIMARY_PREPARE_ONLY',
        'bind-primary',
        'mouse_effect_probe_physical_b_primary_f0',
        'READY_FOR_PHYSICAL_B_PRIMARY_PREPARE',
        'analyze_mouse_effect_probe_b.py',
        'analysis_contract_semantic_sha256',
        'core_delay_samples',
        'confirmation_used_for_refit',
        'estimation,estimation,selection,selection,confirmation,confirmation',
        'physical_b_prbs_primary',
        'launch_mouse_effect_probe_b.ps1',
        'XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT',
        'cross_run_holdout_prepare_authorized',
        'PublishedRunDirectory')) {
    if (-not $prepare.Contains($required)) {
        throw "Physical B Prepare is missing contract text: $required"
    }
}
foreach ($required in @(
        'mouse_effect_probe_b_task',
        '$isBTask',
        'physical_b_prbs_primary',
        'physical_b',
        'XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT',
        'cross_run_holdout_prepare_authorized')) {
    if (-not $launch.Contains($required)) {
        throw "Physical B Launch is missing contract text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript') -or
    $prepare.Contains('& ([string]$launchScript.path)')) {
    throw "Physical B Prepare must not execute its Physical Launch script"
}

Write-Host "Mouse Effect Probe Physical B Prepare/Launch contract passed."
