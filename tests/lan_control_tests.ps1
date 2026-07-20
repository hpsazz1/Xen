param([string]$RepoRoot = (Split-Path -Parent $PSScriptRoot))

$server = Get-Content -LiteralPath (Join-Path $RepoRoot 'Xen\runtime\lan_control_server.cpp') -Raw -Encoding UTF8
$config = Get-Content -LiteralPath (Join-Path $RepoRoot 'Xen\config\config.cpp') -Raw -Encoding UTF8

foreach ($required in @(
    'Post\("/api/auth"',
    'Get\("/api/status"',
    'Post\("/api/control"',
    'Post\("/api/config"',
    'X-Xen-Session',
    'pairingCode',
    'remoteReloadRequested.store\(true\)',
    'config.saveConfig\(\)',
    'class_player',
    'class_head',
    'id=\"target_faction\"',
    'option value=\"police\"',
    'option value=\"terrorist\"',
    'option value=\"custom\"',
    'applyClassState',
    'id=\"class_player\"',
    'id=\"class_head\"',
    'std::isfinite')) {
    if ($server -notmatch $required) { throw "LAN control contract missing: $required" }
}

foreach ($forbidden in @('system\(', 'CreateProcess', 'WinExec', 'ShellExecute')) {
    if ($server -match $forbidden) { throw "Unsafe command execution token found: $forbidden" }
}

foreach ($required in @('lan_console_enabled = false', 'lan_console_bind_address = "0.0.0.0"', 'lan_console_port = 17888')) {
    if ($config -notmatch [regex]::Escape($required)) { throw "LAN console default missing: $required" }
}

Write-Output 'lan control tests passed'
