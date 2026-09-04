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
        throw "Physical B command-magnitude script does not exist: $path"
    }
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        (Resolve-Path -LiteralPath $path).Path,
        [ref]$tokens,
        [ref]$errors)
    if ($errors.Count -ne 0) {
        throw "Physical B command-magnitude script has parse errors: $($errors[0].Message)"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8
foreach ($required in @(
        'mouse_effect_probe_b_command_magnitude_task',
        'physical_b_command_magnitude_primary',
        '--profile physical-b-command-magnitude',
        '--run-role primary',
        '--baseline-samples 64',
        '--response-samples 48',
        '--guard-samples 32',
        '1,1,4,4,13,13,2,2,8,8',
        'primary_estimation_amplitudes = @(1, 4, 13)',
        'within_run_confirmation_amplitudes = @(2, 8)',
        'validation_used_for_refit = $false',
        'new_production_gain_claimed = $false',
        'max_abs_prefix_x_counts = 13',
        'expected_nonzero_transition_count = 20',
        'manual_mouse_motion_or_wasd_forbidden = $true',
        'XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT',
        'PREPARED_NOT_LAUNCHED',
        'physical_launch_executed = $false',
        'Move-Item -LiteralPath $stagingDirectory')) {
    if (-not $prepare.Contains($required)) {
        throw "Physical B command-magnitude Prepare is missing contract text: $required"
    }
}
if ($prepare.Contains('& $launchScript.path') -or
    $prepare.Contains('Start-Process -FilePath $launchScript')) {
    throw 'Physical B command-magnitude Prepare must not execute Launch'
}

foreach ($required in @(
        'XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT',
        '$isBMagnitudePrimaryTask',
        'physical_b_command_magnitude_primary',
        '[int]$sequence.schema -ne 6',
        '$samples.Count -ne 1684',
        '$sequenceBlocks.Count -ne 10',
        '[uint64]$sequence.summary.max_abs_prefix_x_counts -ne 13',
        '1,1,4,4,13,13,2,2,8,8',
        'estimation,estimation,estimation,estimation,estimation,estimation,confirmation,confirmation,confirmation,confirmation',
        'normal,inverted,normal,inverted,normal,inverted,normal,inverted,normal,inverted')) {
    if (-not $launch.Contains($required)) {
        throw "Physical B command-magnitude Launch is missing contract text: $required"
    }
}

Write-Host "Physical B command-magnitude Prepare/Launch contract passed."
