param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$LaunchScript
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($PrepareScript, $LaunchScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required holdout script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "Holdout script has parse errors: $path"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_PREPARE_ONLY',
        'XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT',
        'READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE',
        'analyze_mouse_effect_probe_b_holdout.py',
        'physical-b-holdout',
        'physical_b_prbs_holdout',
        'cross_run_holdout',
        'holdout_used_for_tuning',
        'holdout-plan.json')) {
    if (-not $prepare.Contains($required)) {
        throw "Holdout Prepare is missing contract text: $required"
    }
}
foreach ($required in @(
        '$isBHoldoutTask',
        'schema_version -eq 7',
        'physical_b_prbs_holdout',
        'cross_run_holdout',
        'XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT',
        'holdout_plan',
        'feedback_mask -ne 51',
        'phase -ne 21',
        'samples.Count -ne 288',
        'same stable source clock session is allowed',
        'source_time_ranges_overlap',
        'source_timestamp_ranges_overlap',
        'event/frame source session')) {
    if (-not $launch.Contains($required)) {
        throw "Holdout Launch is missing contract text: $required"
    }
}
if ($launch.Contains('different source clock session')) {
    throw 'Holdout Launch must not require a different source-clock server epoch.'
}
$holdoutFileEntry = $launch.IndexOf(
    '$task.files.holdout_analyzer; name = "holdout analyzer"',
    [StringComparison]::Ordinal)
$fileEvidenceLoop = $launch.IndexOf(
    'foreach ($entry in $fileEntries)',
    [StringComparison]::Ordinal)
if ($holdoutFileEntry -lt 0 -or $fileEvidenceLoop -lt 0 -or
    $holdoutFileEntry -gt $fileEvidenceLoop) {
    throw 'Holdout 专属文件必须在统一 path/size/SHA 校验循环前登记。'
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript') -or
    $prepare.Contains('& ([string]$launchScript.path)')) {
    throw 'Holdout Prepare must never invoke its Launch script.'
}

Write-Host 'Physical B holdout Prepare/Launch script contract passed.'
