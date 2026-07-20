[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$mouseUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\draw_mouse.cpp') -Raw -Encoding UTF8
$overlayUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\overlay.cpp') -Raw -Encoding UTF8
$targetUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\draw_target.cpp') -Raw -Encoding UTF8
$debugUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\draw_debug.cpp') -Raw -Encoding UTF8
$exportUi = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\overlay\export_progress_panel.h') -Raw -Encoding UTF8
$startupHelpers = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\runtime\startup_helpers.cpp') -Raw -Encoding UTF8
$mainSource = Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\Xen.cpp') -Raw -Encoding UTF8
$startupSources = @(
    (Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\runtime\input_device_manager.cpp') -Raw -Encoding UTF8),
    (Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\detector\dml_detector.cpp') -Raw -Encoding UTF8),
    (Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\Xen.cpp') -Raw -Encoding UTF8),
    (Get-Content -LiteralPath (Join-Path $repoRoot 'Xen\scr\other_tools.cpp') -Raw -Encoding UTF8)
)

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
foreach ($control in @('machine_profile_cache_enabled', 'machine_profile_cache_path', 'machine_profile_aim_mode')) {
    if ($mouseUi -notmatch $control) {
        throw "Machine profile cache UI is missing: $control"
    }
}
foreach ($control in @(
    '##aim_pipeline_mode',
    '##shadow_camera_delay_ms',
    '##shadow_camera_response_ms',
    '##shadow_response_ms',
    '##shadow_max_cps',
    '##shadow_ff_gain',
    '##shadow_settle_error',
    '##shadow_settle_rate',
    '##shadow_reverse_confirm',
    '##shadow_vertical_catch_up',
    '##shadow_integral_ms',
    '##shadow_integral_zone',
    '##shadow_lead_horizon',
    '##shadow_lead_strength')) {
    if ($mouseUi -notmatch $control) {
        throw "P0 shadow pipeline UI is missing: $control"
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

# The reduced navigation keeps compatibility code but must not register optional pages.
foreach ($removedDrawFunction in @('draw_overlay', 'draw_game_overlay_general', 'draw_game_overlay_visuals', 'draw_game_overlay_icon', 'draw_stats')) {
    if ($overlayUi -match ('\{[^\r\n]*' + [regex]::Escape($removedDrawFunction) + '[^\r\n]*SidebarIconKind::')) {
        throw "Removed sidebar page is registered again: $removedDrawFunction"
    }
}
foreach ($requiredDrawFunction in @('draw_capture_settings', 'draw_ai', 'draw_target', 'draw_mouse_movement', 'draw_mouse_prediction', 'draw_mouse_input', 'draw_buttons')) {
    if ($overlayUi -notmatch ('\{[^\r\n]*' + [regex]::Escape($requiredDrawFunction) + '[^\r\n]*SidebarIconKind::')) {
        throw "Core sidebar page is missing: $requiredDrawFunction"
    }
}
if ($overlayUi -notmatch 'hovered\s*&&\s*tab\.description' -or
    $overlayUi -notmatch 'ImGui::BeginTooltip\(\)' -or
    $overlayUi -notmatch 'ImGui::TextUnformatted\(tab\.description\)') {
    throw 'Sidebar descriptions are not wired to unified hover tooltips.'
}
if ($overlayUi -notmatch 'RGBA\(51, 156, 255, 245\)' -or
    $overlayUi -match 'RGBA\(16, 163, 127, 245\)' -or
    $overlayUi -match 'IM_COL32\(0, 229, 255') {
    throw 'Codex light palette accent is missing or the previous accent is still active.'
}
foreach ($fontSource in @($mouseUi, $targetUi, $debugUi, $exportUi)) {
    if ($fontSource -match '1\.0f,\s*1\.0f,\s*0\.0f|255,\s*255,\s*0|252,\s*225,\s*115') {
        throw 'Low-contrast yellow text remains in a visible UI source file.'
    }
}
$assistTabButtons = [regex]::Matches($mouseUi, 'ImGui::Button\("[^"\r\n]+",\s*tabSize\)')
if ($assistTabButtons.Count -ne 2 -or $mouseUi -match 'ImGui::SmallButton') {
    throw 'Assist sub-tabs are not using the shared Codex light segmented control.'
}
foreach ($startupSource in $startupSources) {
    foreach ($englishLog in @('[Mouse] Using', 'Model initialized with', 'DML detector created', 'Aimbot is started!', 'Pause Aiming', 'Reload Config', 'Overlay (OPTIONS)')) {
        if ($startupSource -match [regex]::Escape($englishLog)) {
            throw "English startup log remains: $englishLog"
        }
    }
}
foreach ($consoleThemeToken in @(
    'RGB(255, 255, 255)',
    'RGB(26, 28, 31)',
    'RGB(51, 156, 255)',
    'RGB(0, 162, 64)',
    'RGB(186, 38, 35)')) {
    if ($startupHelpers -notmatch [regex]::Escape($consoleThemeToken)) {
        throw "Console theme token is missing: $consoleThemeToken"
    }
}
if ($startupHelpers -notmatch 'ENABLE_VIRTUAL_TERMINAL_PROCESSING' -or
    $startupHelpers -notmatch '\\x1b\[38;2;26;28;31m') {
    throw 'Truecolor ANSI console theme sequence is missing.'
}
if ($startupHelpers -notmatch 'ConsoleToneAttributes' -or
    $startupHelpers -notmatch 'SetConsoleTextAttribute\(output, ConsoleToneAttributes\(tone\)\)') {
    throw 'Win32 console tone fallback is missing.'
}
if ($mainSource -notmatch 'SetConsoleOutputCP\(CP_UTF8\);\s*//[^\r\n]*\s*ApplyConsoleTheme\(\);') {
    throw 'Console theme is not applied after UTF-8 console initialization.'
}

Write-Output 'prediction ui visibility tests passed'
