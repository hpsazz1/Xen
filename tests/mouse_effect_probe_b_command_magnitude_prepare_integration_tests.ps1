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
        throw "Fixture already exists: $Path"
    }
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 30) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).
        Hash.ToLowerInvariant()
}

function Assert-FileIdentity([object]$Identity, [string]$Description) {
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

$obsPath = Join-Path $inputRoot "obs-source-binding.json"
Write-NewUtf8Json $obsPath ([ordered]@{
    schema_version = 1
    evidence_type = "obs_source_binding"
    physical_output_capability = $false
    ndi_main_output = [ordered]@{ name = "Xen-ROI-320" }
    program_geometry = [ordered]@{ roi_width = 320; roi_height = 320 }
})

$a2Path = Join-Path $inputRoot "a2-magnitude-analysis.json"
$a2Fixture = [ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_magnitude_domain_analysis"
    status = "F1_OUTSIDE_A2_MAGNITUDE_DOMAIN"
    physical_output_capability = $false
    physical_dispatch_count = 0
    production_aim_changed = $false
    analysis_contract = [ordered]@{
        model = [ordered]@{
            frozen_f1_gains = [ordered]@{ left = -0.39; right = -0.41 }
        }
    }
    evaluation = [ordered]@{
        f1_deleted_for_magnitude_domain = $true
        new_production_gain_claimed = $false
        primary_model = [ordered]@{ validation_used_for_refit = $false }
    }
}
Write-NewUtf8Json $a2Path $a2Fixture

$b0Path = Join-Path $inputRoot "b0-fidelity-evaluation.json"
Write-NewUtf8Json $b0Path ([ordered]@{
    schema_version = 1
    evidence_type = "aim_production_red_evaluation"
    status = "BASELINE_REPLAY_FIDELITY_INVALID"
    physical_output_capability = $false
    physical_dispatch_count = 0
    production_aim_changed = $false
})

$runDirectory = Join-Path $caseRoot "primary"
$prepareArguments = @{
    ToolRoot = $ToolRoot
    ConfigPath = $configPath
    ObsSourceBindingPath = $obsPath
    A2MagnitudeAnalysisPath = $a2Path
    B0EvaluationPath = $b0Path
    RunDirectory = $runDirectory
    PublishedRunDirectory = $runDirectory
    SidecarFrames = 2200
    MaxSeconds = 15
}
& $PrepareScript @prepareArguments

$task = Get-Content -LiteralPath (Join-Path $runDirectory "task.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$sequence = Get-Content -LiteralPath (Join-Path $runDirectory "sequence.json") `
    -Raw -Encoding utf8 | ConvertFrom-Json
$binding = Get-Content -LiteralPath (
    Join-Path $runDirectory "probe-binding.json") -Raw -Encoding utf8 |
    ConvertFrom-Json
$summary = Get-Content -LiteralPath (
    Join-Path $runDirectory "prepare-summary.json") -Raw -Encoding utf8 |
    ConvertFrom-Json

$amplitudes = @($sequence.blocks | ForEach-Object {
    [int]$_.amplitude_counts
}) -join ","
$roles = @($sequence.blocks | ForEach-Object { [string]$_.role }) -join ","
$polarities = @($sequence.blocks | ForEach-Object {
    [string]$_.polarity
}) -join ","
if ([int]$task.schema_version -ne 8 -or
    [string]$task.evidence_type -ne
        "mouse_effect_probe_b_command_magnitude_task" -or
    [string]$task.status -ne "PREPARED" -or
    [string]$task.dispatch_mode -ne "physical_b" -or
    [string]$task.run_role -ne "primary" -or
    [string]$task.profile -ne
        "physical_b_command_magnitude_primary" -or
    [string]$task.physical_output_confirmation -ne
        "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT" -or
    -not [bool]$task.requires_user_frontend_launch -or
    [uint64]$task.sequence_sample_count -ne 1684 -or
    [uint64]$task.expected_nonzero_transition_count -ne 20 -or
    [uint64]$task.max_abs_prefix_x_counts -ne 13 -or
    [bool]$task.dynamics_policy.validation_used_for_refit -or
    [bool]$task.dynamics_policy.new_production_gain_claimed -or
    -not [bool]$task.dynamics_policy.cross_run_holdout_required_before_candidate -or
    [bool]$task.dynamics_policy.fixed_pixel_speed_used_as_gate -or
    [int]$sequence.schema -ne 6 -or
    @($sequence.samples).Count -ne 1684 -or
    @($sequence.blocks).Count -ne 10 -or
    $amplitudes -ne "1,1,4,4,13,13,2,2,8,8" -or
    $roles -ne
        "estimation,estimation,estimation,estimation,estimation,estimation,confirmation,confirmation,confirmation,confirmation" -or
    $polarities -ne
        "normal,inverted,normal,inverted,normal,inverted,normal,inverted,normal,inverted" -or
    [int64]$sequence.summary.net_x_counts -ne 0 -or
    [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 13 -or
    [string]$binding.scope_id -ne [string]$task.scope_id -or
    [string]$binding.sequence_sha256 -ne [string]$task.sequence_sha256 -or
    [string]$summary.status -ne "PREPARED_NOT_LAUNCHED" -or
    [bool]$summary.physical_launch_executed) {
    throw "Prepared command-magnitude identities do not close"
}

foreach ($property in $task.files.PSObject.Properties) {
    Assert-FileIdentity $property.Value "task.files.$($property.Name)"
}
$taskMarkdown = Get-Content -LiteralPath (
    Join-Path $runDirectory "TASK.md") -Raw -Encoding utf8
if (-not $taskMarkdown.Contains("-AllowPhysicalOutput") -or
    -not $taskMarkdown.Contains(
        "XEN_MOUSE_EFFECT_PROBE_B_MAGNITUDE_PRIMARY_SENDS_REAL_KMBOX_INPUT")) {
    throw "TASK.md does not contain the exact user-only Launch command"
}
foreach ($forbidden in @(
        "command-report.json", "safety-ledger.json", "launch-summary.json",
        "pixel-evidence")) {
    if (Test-Path -LiteralPath (Join-Path $runDirectory $forbidden)) {
        throw "Prepare produced forbidden physical output: $forbidden"
    }
}

$badA2Path = Join-Path $inputRoot "bad-a2.json"
$badA2 = $a2Fixture | ConvertTo-Json -Depth 30 | ConvertFrom-Json
$badA2.evaluation.new_production_gain_claimed = $true
Write-NewUtf8Json $badA2Path $badA2
$badRun = Join-Path $caseRoot "rejected-production-claim"
$rejected = $false
try {
    $badArguments = @{} + $prepareArguments
    $badArguments.A2MagnitudeAnalysisPath = $badA2Path
    $badArguments.RunDirectory = $badRun
    $badArguments.PublishedRunDirectory = $badRun
    & $PrepareScript @badArguments
} catch {
    $rejected = $true
}
if (-not $rejected -or (Test-Path -LiteralPath $badRun)) {
    throw "Prepare must reject upstream production-gain claims before Run creation"
}
if (@(Get-ChildItem -LiteralPath $caseRoot -Directory | Where-Object {
            $_.Name -like ".rejected-production-claim.incoming-*"
        }).Count -ne 0) {
    throw "Rejected Prepare left an incoming directory"
}

Write-Host "Physical B command-magnitude Prepare integration passed."
