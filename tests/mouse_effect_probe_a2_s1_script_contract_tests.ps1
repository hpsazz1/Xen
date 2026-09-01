param(
    [Parameter(Mandatory = $true)]
    [string]$CaptureScript
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $CaptureScript -PathType Leaf)) {
    throw "S1 capture script does not exist: $CaptureScript"
}
$tokens = $null
$errors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $CaptureScript, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) {
    throw "S1 capture script has parse errors: $($errors[0].Message)"
}
$content = Get-Content -LiteralPath $CaptureScript -Raw -Encoding utf8
foreach ($required in @(
        'XenCaptureEvidence.exe',
        'mouse_effect_probe_a2_s1_session',
        'physical_output_capability = $false',
        'probe_started = $false',
        'mouse_opened = $false',
        'actual_command_zero = $true',
        'capture_process_session_id',
        '$ndiSource.EndsWith(" ($bindingOutputName)")',
        '.incoming-',
        'Move-Item -LiteralPath $stagingDirectory')) {
    if (-not $content.Contains($required)) {
        throw "S1 capture script is missing contract text: $required"
    }
}
foreach ($forbidden in @(
        'XenMouseEffectProbe.exe',
        'AllowPhysicalOutput',
        'confirm-physical-output',
        'allow_send_input = true')) {
    if ($content.Contains($forbidden)) {
        throw "S1 capture script contains physical/probe capability: $forbidden"
    }
}

Write-Host "Mouse Effect Probe A2 S1 capture script contract passed."
