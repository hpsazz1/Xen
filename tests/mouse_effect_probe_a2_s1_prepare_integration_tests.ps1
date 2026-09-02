param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    if (Test-Path -LiteralPath $Path) {
        throw "测试输入已存在：$Path"
    }
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 16) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Assert-FileIdentity([object]$Identity, [string]$Description) {
    if ($null -eq $Identity -or
        -not (Test-Path -LiteralPath ([string]$Identity.path) -PathType Leaf)) {
        throw "$Description 不存在"
    }
    $item = Get-Item -LiteralPath ([string]$Identity.path)
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).
        Hash.ToLowerInvariant()
    if ([uint64]$item.Length -ne [uint64]$Identity.size -or
        $hash -ne [string]$Identity.sha256) {
        throw "$Description 的 size/SHA 不闭合"
    }
}

$resolvedRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $resolvedRoot)
}
$caseRoot = Join-Path $resolvedRoot (
    "case-{0}" -f [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $caseRoot)
$inputRoot = Join-Path $caseRoot "inputs"
[void](New-Item -ItemType Directory -Path $inputRoot)

$configPath = Join-Path $inputRoot "config.ini"
$config = @(
    "[capture]",
    "backend=ndi",
    "ndi_source_name=HPSAZZ (Xen-ROI-320)",
    "ndi_clock_sync_url=http://192.0.2.1:43130",
    "ndi_frame_layout=center_crop_1_to_1",
    "ndi_source_width=2560",
    "ndi_source_height=1440",
    "roi_width=320",
    "roi_height=320",
    "center_roi=true",
    "roi_x=0",
    "roi_y=0",
    "ndi_discovery_timeout_ms=10000",
    "ndi_receive_timeout_ms=100",
    "ndi_disconnect_timeout_ms=2000",
    "ndi_clock_sync_interval_ms=1000",
    "ndi_clock_sync_timeout_ms=500",
    "ndi_clock_mapping_max_age_ms=5000",
    "ndi_require_frame_metadata=true",
    "",
    "[mouse]",
    "backend=kmbox_net",
    "allow_send_input=false",
    "") -join [Environment]::NewLine
[IO.File]::WriteAllText(
    $configPath, $config, [Text.UTF8Encoding]::new($false))

$obsBindingPath = Join-Path $inputRoot "obs-source-binding.json"
Write-NewUtf8Json $obsBindingPath ([ordered]@{
    schema_version = 1
    evidence_type = "obs_source_binding"
    physical_output_capability = $false
    ndi_main_output = [ordered]@{
        name = "Xen-ROI-320"
    }
    program_geometry = [ordered]@{
        roi_width = 320
        roi_height = 320
    }
})

$tasks = @{}
foreach ($role in @("primary", "validation")) {
    $runDirectory = Join-Path $caseRoot $role
    & $PrepareScript `
        -ToolRoot $ToolRoot `
        -ConfigPath $configPath `
        -ObsSourceBindingPath $obsBindingPath `
        -RunDirectory $runDirectory `
        -PublishedRunDirectory $runDirectory `
        -RunRole $role `
        -ChallengePulseCount 2 `
        -ChallengeStrideSampleCount 3 `
        -SettleSampleCount 32 `
        -BaselineSampleCount 64 `
        -SidecarFrames 256 `
        -MaxSeconds 15
    if ($LASTEXITCODE -ne 0) {
        throw "A2 S1 $role Prepare 失败，ExitCode=$LASTEXITCODE"
    }
    $task = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "task.json") | ConvertFrom-Json
    $summary = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "prepare-summary.json") | ConvertFrom-Json
    $sequence = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "sequence.json") | ConvertFrom-Json
    $expectedProfile = "dependency_calibration_a2_s1_$role"
    if ([int]$task.schema_version -ne 3 -or
        [string]$task.evidence_type -ne "mouse_effect_probe_a2_s1_task" -or
        [string]$task.status -ne "PREPARED" -or
        [string]$task.profile -ne $expectedProfile -or
        [string]$task.run_role -ne $role -or
        [uint64]$task.sequence_sample_count -ne 120 -or
        [uint64]$task.expected_nonzero_transition_count -ne 8 -or
        [uint64]$task.sidecar.publishing_max_seconds -ne 60 -or
        [bool]$task.liveness_policy.challenge_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.settle_frames_eligible_for_estimands -or
        [bool]$task.liveness_policy.fixed_pixel_speed_used_as_gate -or
        [int]$sequence.schema -ne 3 -or
        [string]$sequence.profile -ne $expectedProfile -or
        [int64]$sequence.summary.net_x_counts -ne 0 -or
        [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 2 -or
        [string]$summary.status -ne "PREPARED_NOT_LAUNCHED" -or
        [bool]$summary.physical_launch_executed) {
        throw "A2 S1 $role task/sequence/Prepare 身份不闭合"
    }
    foreach ($property in $task.files.PSObject.Properties) {
        Assert-FileIdentity $property.Value "A2 S1 $role $($property.Name)"
    }
    foreach ($forbidden in @(
            "command-report.json", "launch-summary.json",
            "s1-liveness-bracket.json", "s1-session.json",
            "sidecar-lifecycle.json",
            "pixel-evidence")) {
        if (Test-Path -LiteralPath (Join-Path $runDirectory $forbidden)) {
            throw "A2 S1 $role Prepare 不得产生 Launch/Physical 产物：$forbidden"
        }
    }
    $taskMarkdown = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "TASK.md")
    if (-not $taskMarkdown.Contains("-AllowPhysicalOutput") -or
        -not $taskMarkdown.Contains(
            "XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT") -or
        -not $taskMarkdown.Contains("不要按 WASD")) {
        throw "A2 S1 $role TASK.md 缺少用户授权/自动挑战边界"
    }
    $tasks[$role] = $task
}

if ([string]$tasks.primary.scope_id -ne
        [string]$tasks.validation.scope_id -or
    [string]$tasks.primary.run_uuid -eq
        [string]$tasks.validation.run_uuid -or
    [string]$tasks.primary.sequence_sha256 -eq
        [string]$tasks.validation.sequence_sha256) {
    throw "A2 S1 primary/validation 必须同 scope、独立 Run、镜像序列"
}

Write-Host "Mouse Effect Probe A2 S1 Prepare integration passed: $caseRoot"
