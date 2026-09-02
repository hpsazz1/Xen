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
        ($Value | ConvertTo-Json -Depth 20) + [Environment]::NewLine,
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
    "ndi_clock_sync_url=http://192.168.3.10:43130",
    "ndi_frame_layout=roi",
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
    evidence_type = "test_obs_source_binding"
    source_name = "HPSAZZ (Xen-ROI-320)"
})

$scopeId = "a2-prepare-integration-scope"
$s0Path = Join-Path $inputRoot "s0.json"
$s1Path = Join-Path $inputRoot "s1.json"
$planPath = Join-Path $inputRoot "plan.json"
Write-NewUtf8Json $s0Path ([ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_s0_synthetic_calibration"
    status = "VALID"
    physical_output_capability = $false
})
Write-NewUtf8Json $s1Path ([ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_s1_zero_input_calibration"
    status = "VALID"
    physical_output_capability = $false
    probe_started = $false
    mouse_opened = $false
    actual_command_zero = $true
    scope_id = $scopeId
    geometry = [ordered]@{
        left_roi = @(16, 48, 96, 224)
        right_roi = @(208, 48, 96, 224)
    }
})
$baseline = 4
$response = 32
$guard = 40
$blocks = 4
$sampleCount = $baseline + $blocks * (2 * $guard + 2 * $response + 2)
Write-NewUtf8Json $planPath ([ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_dependency_calibration_plan"
    status = "VALID_OFFLINE_PLAN"
    physical_output_capability = $false
    physical_launch_authorized = $false
    scope_id = $scopeId
    sequence_request = [ordered]@{
        baseline_sample_count = $baseline
        response_sample_count = $response
        guard_sample_count = $guard
        block_count = $blocks
        sample_count = $sampleCount
        nonzero_transition_count = 8
        net_x_counts = 0
        max_abs_prefix_x_counts = 1
        dy_counts_required = 0
    }
    sidecar = [ordered]@{
        frame_count = 800
        max_seconds = 30
    }
    roles = [ordered]@{
        p_cal = [ordered]@{
            profile = "dependency_calibration_a2_p_cal"
        }
        p_holdout = [ordered]@{
            profile = "dependency_calibration_a2_p_holdout"
        }
    }
})

foreach ($role in @("p-cal", "p-holdout")) {
    $runDirectory = Join-Path $caseRoot $role
    & $PrepareScript `
        -ToolRoot $ToolRoot `
        -ConfigPath $configPath `
        -ObsSourceBindingPath $obsBindingPath `
        -SyntheticCalibrationPath $s0Path `
        -ZeroInputCalibrationPath $s1Path `
        -CalibrationPlanPath $planPath `
        -RunDirectory $runDirectory `
        -PublishedRunDirectory $runDirectory `
        -RunRole $role
    if ($LASTEXITCODE -ne 0) {
        throw "A2 $role Prepare 失败，ExitCode=$LASTEXITCODE"
    }
    $task = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "task.json") | ConvertFrom-Json
    $expectedProfile = if ($role -eq "p-cal") {
        "dependency_calibration_a2_p_cal"
    } else {
        "dependency_calibration_a2_p_holdout"
    }
    if ([string]$task.evidence_type -ne "mouse_effect_probe_a2_task" -or
        [string]$task.status -ne "PREPARED" -or
        [string]$task.profile -ne $expectedProfile -or
        [string]$task.run_role -ne $role -or
        [uint64]$task.expected_nonzero_transition_count -ne 8) {
        throw "A2 $role task 身份或序列合同不闭合"
    }
    foreach ($property in $task.files.PSObject.Properties) {
        Assert-FileIdentity $property.Value "A2 $role $($property.Name)"
    }
    if ((Test-Path -LiteralPath (
                Join-Path $runDirectory "command-report.json")) -or
        (Test-Path -LiteralPath (
                Join-Path $runDirectory "safety-ledger.json")) -or
        (Test-Path -LiteralPath (
                Join-Path $runDirectory "pixel-evidence"))) {
        throw "A2 $role Prepare 不得产生 Launch/Physical 产物"
    }
    $taskMarkdown = Get-Content -Raw -Encoding utf8 -LiteralPath (
        Join-Path $runDirectory "TASK.md")
    if (-not $taskMarkdown.Contains("-AllowPhysicalOutput") -or
        -not $taskMarkdown.Contains("XEN_MOUSE_EFFECT_PROBE_A_SENDS_REAL_KMBOX_INPUT")) {
        throw "A2 $role TASK.md 缺少用户前台双重授权命令"
    }
    if ($role -eq "p-holdout" -and
        -not $taskMarkdown.Contains("P-HOLDOUT 不得用于回调")) {
        throw "A2 P-HOLDOUT 缺少禁止回调合同"
    }
}

Write-Host "Mouse Effect Probe A2 Prepare integration passed: $caseRoot"
