param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 30) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).
        Hash.ToLowerInvariant()
}

function Assert-Identity([object]$Identity, [string]$Description) {
    if ($null -eq $Identity -or
        -not (Test-Path -LiteralPath ([string]$Identity.path) -PathType Leaf)) {
        throw "$Description is missing"
    }
    $item = Get-Item -LiteralPath ([string]$Identity.path)
    if ([uint64]$item.Length -ne [uint64]$Identity.size -or
        (Get-LowerSha256 $item.FullName) -ne [string]$Identity.sha256) {
        throw "$Description identity does not close"
    }
}

$resolvedRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $resolvedRoot)
}
$caseRoot = Join-Path $resolvedRoot (
    "case-{0}" -f [guid]::NewGuid().ToString("N"))
$inputRoot = Join-Path $caseRoot "inputs"
[void](New-Item -ItemType Directory -Path $inputRoot -Force)

$configPath = Join-Path $inputRoot "config.ini"
$config = @(
    "[capture]",
    "backend=ndi",
    "ndi_source_name=HPSAZZ (Xen-ROI-320)",
    "ndi_clock_sync_url=udp://192.0.2.10:5011",
    "ndi_frame_layout=center_crop_1_to_1",
    "ndi_source_width=2560",
    "ndi_source_height=1440",
    "roi_width=320",
    "roi_height=320",
    "center_roi=true",
    "roi_x=0",
    "roi_y=0",
    "ndi_discovery_timeout_ms=10000",
    "ndi_receive_timeout_ms=50",
    "ndi_disconnect_timeout_ms=2000",
    "ndi_clock_sync_interval_ms=250",
    "ndi_clock_sync_timeout_ms=200",
    "ndi_clock_mapping_max_age_ms=1000",
    "ndi_require_frame_metadata=false",
    "",
    "[mouse]",
    "backend=kmbox_net",
    "allow_send_input=false",
    "") -join [Environment]::NewLine
[IO.File]::WriteAllText(
    $configPath, $config, [Text.UTF8Encoding]::new($false))

$scenePath = Join-Path $inputRoot "scene.json"
$profilePath = Join-Path $inputRoot "basic.ini"
$logPath = Join-Path $inputRoot "obs.log"
[IO.File]::WriteAllText(
    $scenePath, '{"scene":"fixture"}', [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText(
    $profilePath,
    "[Video]`nFPSNum=240`nFPSDen=1`n",
    [Text.UTF8Encoding]::new($false))
$log = @(
    "04:06:24.611: OBS 32.1.0-rc4 (64-bit, windows)",
    "04:06:25.351: [duplicator-monitor-capture: '主画面'] update settings:",
    "04:06:25.351: `tmethod: DXGI",
    "") -join [Environment]::NewLine
[IO.File]::WriteAllText(
    $logPath, $log, [Text.UTF8Encoding]::new($false))
$obsPath = Join-Path $inputRoot "obs-source-binding.json"
Write-NewUtf8Json $obsPath ([ordered]@{
    schema_version = 2
    evidence_type = "obs_source_binding"
    binding_mode = "real_game"
    physical_output_capability = $false
    state_basis = "obs_saved_scene_collection"
    scene_collection = $scenePath
    scene_collection_sha256 = Get-LowerSha256 $scenePath
    obs_profile_config = [ordered]@{
        path = $profilePath
        sha256 = Get-LowerSha256 $profilePath
    }
    ndi_main_output = [ordered]@{ enabled = $true; name = "Xen-ROI-320" }
    program_geometry = [ordered]@{
        mapping = "monitor_crop_filter_1_to_1"
        source_width = 2560
        source_height = 1440
        roi_width = 320
        roi_height = 320
        roi_x = 1120
        roi_y = 560
    }
    selected_source = [ordered]@{
        name = "主画面"
        id = "monitor_capture"
        capture_cursor = $false
        crop_filter = [ordered]@{
            enabled = $true
            settings = [ordered]@{ relative = $false }
        }
    }
})

$runDirectory = Join-Path $caseRoot "prepared"
$arguments = @{
    ToolRoot = $ToolRoot
    ConfigPath = $configPath
    ObsSourceBindingPath = $obsPath
    ObsLogPath = $logPath
    PythonExecutable = $PythonExecutable
    RunDirectory = $runDirectory
    PublishedRunDirectory = $runDirectory
    MaxSeconds = 15
}
& $PrepareScript @arguments

$task = Get-Content -LiteralPath (Join-Path $runDirectory "task.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$sequence = Get-Content -LiteralPath (Join-Path $runDirectory "sequence.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$seed = Get-Content -LiteralPath (
    Join-Path $runDirectory "composite-phase-plan-seed.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$policy = Get-Content -LiteralPath (
    Join-Path $runDirectory "capture-policy.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$summary = Get-Content -LiteralPath (
    Join-Path $runDirectory "prepare-summary.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
if ([int]$task.schema_version -ne 10 -or
    [string]$task.evidence_type -ne
        "mouse_effect_probe_b_composite_phase_task" -or
    [string]$task.status -ne "PREPARED" -or
    [string]$task.profile -ne "physical_b_composite_phase_calibration" -or
    [string]$task.run_role -ne "calibration_deletion" -or
    [string]$task.physical_output_confirmation -ne
        "XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT" -or
    -not [bool]$task.requires_user_frontend_launch -or
    [uint64]$task.sequence_sample_count -ne 295 -or
    [uint64]$task.window_count -ne 42 -or
    [uint64]$task.negative_control_count -ne 4 -or
    [uint64]$task.expected_nonzero_transition_count -ne 38 -or
    [uint64]$task.max_abs_prefix_x_counts -ne 1 -or
    [uint64]$task.sidecar.frames -ne 2400 -or
    [uint64]$task.sidecar.minimum_coverage_frames -ne 1735 -or
    [string]$task.sidecar.coverage_basis -ne
        "ARMING_5S_PLUS_295_SOURCE_EVENTS_PLUS_1S_MARGIN" -or
    -not [bool]$task.composite_policy.final_plan_frozen_on_auxiliary_before_sidecar -or
    -not [bool]$task.composite_policy.same_auxiliary_host_preflight_required -or
    [bool]$task.composite_policy.response_revealed_before_final_plan -or
    [bool]$task.composite_policy.production_aim_changed -or
    [int]$sequence.schema -ne 7 -or
    @($sequence.samples).Count -ne 295 -or
    @($sequence.windows).Count -ne 42 -or
    [string]$seed.status -ne "AWAITING_AUXILIARY_PREFLIGHT" -or
    $null -ne $seed.frozen_at_utc_unix_ns -or
    $null -ne $seed.scheduler_policy.preflight_file_sha256 -or
    [string]$policy.capture_stack.capture_method_resolved -ne "DXGI" -or
    [string]$policy.capture_stack.producer_version -ne "32.1.0-rc4" -or
    [string]$summary.status -ne "PREPARED_NOT_LAUNCHED" -or
    [bool]$summary.scheduler_preflight_executed -or
    [bool]$summary.final_plan_frozen -or
    [bool]$summary.physical_launch_executed) {
    throw "Prepared composite-phase identities do not close"
}
foreach ($property in $task.files.PSObject.Properties) {
    Assert-Identity $property.Value "task.files.$($property.Name)"
}
foreach ($forbidden in @(
        "scheduler-preflight.json", "composite-phase-plan.json",
        "composite-schedule-ledger.json", "command-report.json",
        "safety-ledger.json", "launch-summary.json", "pixel-evidence")) {
    if (Test-Path -LiteralPath (Join-Path $runDirectory $forbidden)) {
        throw "Prepare produced forbidden acquisition output: $forbidden"
    }
}
$taskMarkdown = Get-Content -LiteralPath (
    Join-Path $runDirectory "TASK.md") -Raw -Encoding utf8
if (-not $taskMarkdown.Contains("-AllowPhysicalOutput") -or
    -not $taskMarkdown.Contains(
        "XEN_MOUSE_EFFECT_PROBE_B_COMPOSITE_PHASE_CALIBRATION_SENDS_REAL_KMBOX_INPUT")) {
    throw "TASK.md lacks the exact user-only Launch command"
}

[IO.File]::AppendAllText(
    $scenePath, "drift", [Text.UTF8Encoding]::new($false))
$badRun = Join-Path $caseRoot "stale-scene"
$rejected = $false
try {
    $badArguments = @{} + $arguments
    $badArguments.RunDirectory = $badRun
    $badArguments.PublishedRunDirectory = $badRun
    & $PrepareScript @badArguments
} catch {
    $rejected = $true
}
if (-not $rejected -or (Test-Path -LiteralPath $badRun)) {
    throw "Prepare must reject a stale live OBS scene binding"
}

Write-Host "Physical B composite-phase Prepare integration passed."
