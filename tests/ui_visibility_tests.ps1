[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$mouseUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\draw_mouse.cpp') -Raw -Encoding UTF8
$overlayUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\overlay.cpp') -Raw -Encoding UTF8

if ($mouseUi -match 'false\s*&&\s*shouldDrawMousePage\(page,\s*MouseSettingsPage::Prediction\)') {
    throw 'Prediction settings page is compile-time hidden.'
}
if ($mouseUi -notmatch 'shouldDrawMousePage\(page,\s*MouseSettingsPage::Prediction\)\s*&&') {
    throw 'Prediction settings page has no active draw condition.'
}
foreach ($control in @('##pred_enabled', '##pred_lead_ms', '##pred_velocity_tau_ms', '##pred_strength')) {
    if ($mouseUi -notmatch $control) {
        throw "Prediction settings control is missing: $control"
    }
}
foreach ($control in @('profile_calibration_enabled', 'profile_calibration_results', 'reset_profile_calibration')) {
    if ($mouseUi -notmatch $control) {
        throw "Passive profile calibration UI is missing: $control"
    }
}
foreach ($control in @(
    '##shadow_response_ms',
    '##shadow_max_cps',
    '##shadow_ff_gain',
    '##shadow_settle_error',
    '##shadow_settle_rate',
    '##shadow_reverse_confirm',
    '##shadow_integral_ms',
    '##shadow_integral_zone',
    '##shadow_lead_horizon',
    '##shadow_lead_strength')) {
    if ($mouseUi -notmatch $control) {
        throw "P0-4A shadow controller UI is missing: $control"
    }
}
foreach ($control in @(
    '##trajectory_shaper_mode',
    '##trajectory_output_hz',
    '##trajectory_max_velocity',
    '##trajectory_max_acceleration',
    '##trajectory_max_jerk')) {
    if ($mouseUi -notmatch $control) {
        throw "P0-4B trajectory shaper UI is missing: $control"
    }
}
if ($overlayUi -notmatch '\{[^\r\n]*draw_mouse_prediction[^\r\n]*SidebarIconKind::Curve\s*\}') {
    throw 'Prediction settings page is not registered in the sidebar.'
}

Write-Output 'prediction ui visibility tests passed'
