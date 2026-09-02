param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable,
    [Parameter(Mandatory = $true)]
    [string]$DesignerScript,
    [Parameter(Mandatory = $true)]
    [string]$AnalyzerScript,
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
        throw "$Description 不存在"
    }
    $item = Get-Item -LiteralPath ([string]$Identity.path)
    if ([uint64]$item.Length -ne [uint64]$Identity.size -or
        (Get-LowerSha256 $item.FullName) -ne [string]$Identity.sha256) {
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
    "ndi_clock_sync_url=udp://192.168.3.10:5011",
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
$obsBindingPath = Join-Path $inputRoot "obs-source-binding.json"
Write-NewUtf8Json $obsBindingPath ([ordered]@{
    evidence_type = "test_obs_source_binding"
    source_name = "HPSAZZ (Xen-ROI-320)"
})

$physicalAPath = Join-Path $inputRoot "physical-a.json"
Write-NewUtf8Json $physicalAPath ([ordered]@{
    status = "VALID"
    machine_visible_effect_observed = $true
    human_physical_acceptance = "NOT_INFERRED_BY_ANALYZER"
    method = [ordered]@{
        timestamp_semantic = "NDI_SDK_SUBMISSION_NOT_EXPOSURE"
    }
    run_binding = [ordered]@{
        run_uuid = "fixture-run"
        sequence_sha256 = ("a" * 64)
    }
    geometry = [ordered]@{
        left_roi = [ordered]@{
            left_margin_px = 16
            right_margin_px = 208
        }
        right_roi = [ordered]@{
            left_margin_px = 208
            right_margin_px = 16
        }
    }
    zero_input_baseline = [ordered]@{
        left_exact_state_count = 1
        right_exact_state_count = 1
    }
    pulse_responses = @(
        [ordered]@{ onset = @{ first_changed_frame_lag = 4 }; x_px_per_count = 0.55 },
        [ordered]@{ onset = @{ first_changed_frame_lag = 4 }; x_px_per_count = 0.55 },
        [ordered]@{ onset = @{ first_changed_frame_lag = 4 }; x_px_per_count = 0.52 },
        [ordered]@{ onset = @{ first_changed_frame_lag = 4 }; x_px_per_count = 0.52 }
    )
    witness_state_summary = [ordered]@{
        statistical_independence_claimed = $false
    }
})
$offlineDesignPath = Join-Path $inputRoot "offline-design.json"
& $PythonExecutable $DesignerScript `
    --physical-a-analysis $physicalAPath `
    --output $offlineDesignPath `
    --orders 5,6,7 `
    --horizons 4,8,16,32 `
    --guard-samples 32
if ($LASTEXITCODE -ne 0) {
    throw "测试 offline design 生成失败，ExitCode=$LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $AnalyzerScript -PathType Leaf)) {
    throw "Physical B analyzer fixture 不存在"
}
$offlineDesignSha = Get-LowerSha256 $offlineDesignPath

$a2DecisionPath = Join-Path $inputRoot "a2-decision.json"
Write-NewUtf8Json $a2DecisionPath ([ordered]@{
    schema_version = 1
    evidence_type = "mouse_effect_probe_a2_dependency_holdout_decision"
    status = "A2_DEPENDENCY_GREEN"
    invalid_reasons = @()
    physical_output_capability = $false
    production_aim_changed = $false
    run_role = "p-holdout"
    profile = "dependency_calibration_a2_p_holdout"
    scope_id = ("b" * 64)
    run_uuid = "6056cd77-5e96-4276-9278-7e3b6c6ea0a2"
    sequence_sha256 = ("c" * 64)
    candidate_sha256 = ("d" * 64)
    candidate_run_uuid = "f00d86dc-9d2f-4d5b-9dfb-eec2bc3c56d0"
    candidate_values_changed = $false
    holdout_used_for_tuning = $false
    a2_dependency_gate_claimed = $true
    physical_b_authorized = $false
    independence = [ordered]@{
        different_run_uuid = $true
        different_activation_epoch = $true
        different_sidecar_manifest = $true
        same_analyzer = $true
        same_capture_source = $true
    }
    human_observation = [ordered]@{
        observation_sha256 = ("e" * 64)
        visible_effect_reported = $true
        manual_mouse_or_wasd_used = $false
        left_right_witness_consistent = $true
        occlusion_or_scene_cut_reported = $false
        anomaly_or_emergency_stop_reported = $false
    }
    comparisons = [ordered]@{
        tail_support = [ordered]@{
            candidate_upper_lag = 7
            holdout_observed_upper_lag = 5
            passed = $true
        }
        mapping_uncertainty = [ordered]@{
            candidate_upper_px = 1.342895110591
            holdout_upper_px = 1.332698341823
            passed = $true
        }
        single_count_gain_upper_scope = [ordered]@{
            candidate_upper_px = 1.342895105713
            holdout_upper_px = 1.332698363329
            passed = $true
        }
        witness_occlusion_margin = [ordered]@{
            candidate_usable_margin_lower_px = 14.657104894287
            holdout_usable_margin_lower_px = 14.667301636671
            passed = $true
        }
        physical_b_prefix_candidate = [ordered]@{
            candidate_allowed_prefix_counts = 9
            holdout_allowed_prefix_counts = 10
            passed = $true
            physical_b_authorized = $false
        }
    }
})
$a2DecisionSha = Get-LowerSha256 $a2DecisionPath

$runDirectory = Join-Path $caseRoot "primary"
$prepareArguments = @{
    ToolRoot = $ToolRoot
    PythonExecutable = $PythonExecutable
    ConfigPath = $configPath
    ObsSourceBindingPath = $obsBindingPath
    OfflineDesignPath = $offlineDesignPath
    ExpectedOfflineDesignSha256 = $offlineDesignSha
    A2DecisionPath = $a2DecisionPath
    ExpectedA2DecisionSha256 = $a2DecisionSha
    RunDirectory = $runDirectory
    PublishedRunDirectory = $runDirectory
    PrepareAuthorization =
        "XEN_MOUSE_EFFECT_PROBE_B_PRIMARY_PREPARE_ONLY"
}
& $PrepareScript @prepareArguments
if ($LASTEXITCODE -ne 0) {
    throw "Physical B Primary Prepare 失败，ExitCode=$LASTEXITCODE"
}

$task = Get-Content -Raw -Encoding utf8 -LiteralPath (
    Join-Path $runDirectory "task.json") | ConvertFrom-Json
$sequence = Get-Content -Raw -Encoding utf8 -LiteralPath (
    Join-Path $runDirectory "sequence.json") | ConvertFrom-Json
$f0 = Get-Content -Raw -Encoding utf8 -LiteralPath (
    Join-Path $runDirectory "f0-primary.json") | ConvertFrom-Json
if ([int]$task.schema_version -ne 4 -or
    [string]$task.evidence_type -ne "mouse_effect_probe_b_task" -or
    [string]$task.status -ne "PREPARED" -or
    [string]$task.dispatch_mode -ne "physical_b" -or
    [string]$task.profile -ne "physical_b_prbs_primary" -or
    [string]$task.run_role -ne "primary" -or
    [string]$task.physical_output_confirmation -ne
        "XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT" -or
    [bool]$task.cross_run_holdout_prepare_authorized -or
    [uint64]$task.sequence_sample_count -ne 416 -or
    [uint64]$task.max_abs_prefix_x_counts -ne 1 -or
    [int]$sequence.schema -ne 4 -or
    @($sequence.blocks).Count -ne 4 -or
    @($sequence.samples).Count -ne 416 -or
    [string]$f0.status -ne "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE" -or
    [bool]$f0.physical_b_launch_authorized) {
    throw "Physical B Primary task/F0/sequence 身份或安全合同不闭合"
}
if ([string]$f0.analyzer.file_sha256 -ne
        [string]$task.files.physical_b_analyzer.sha256 -or
    [string]$f0.analysis_contract.contract_semantic_sha256 -ne
        [string]$task.f0.analysis_contract_semantic_sha256) {
    throw "Physical B analyzer/F0 analysis contract 未精确绑定"
}
foreach ($property in $task.files.PSObject.Properties) {
    Assert-FileIdentity $property.Value "Physical B $($property.Name)"
}
$taskMarkdown = Get-Content -Raw -Encoding utf8 -LiteralPath (
    Join-Path $runDirectory "TASK.md")
if (-not $taskMarkdown.Contains("-AllowPhysicalOutput") -or
    -not $taskMarkdown.Contains(
        "XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT") -or
    -not $taskMarkdown.Contains("只能由用户在本机前台执行")) {
    throw "Physical B TASK.md 缺少唯一用户前台 Launch 命令"
}
foreach ($forbidden in @(
        "command-report.json", "safety-ledger.json", "launch-summary.json",
        "pixel-evidence")) {
    if (Test-Path -LiteralPath (Join-Path $runDirectory $forbidden)) {
        throw "Physical B Prepare 不得产生 Launch/Physical 产物：$forbidden"
    }
}
if (Test-Path -LiteralPath (
        Join-Path $runDirectory "cross-run-holdout-sequence.json")) {
    throw "Physical B Primary Prepare 不得物化 cross-Run holdout sequence"
}

$unauthorizedRun = Join-Path $caseRoot "rejected-authorization"
$unauthorized = $false
try {
    $badArguments = @{} + $prepareArguments
    $badArguments.RunDirectory = $unauthorizedRun
    $badArguments.PublishedRunDirectory = $unauthorizedRun
    $badArguments.PrepareAuthorization = "WRONG"
    & $PrepareScript @badArguments
} catch {
    $unauthorized = $_.Exception.Message.Contains("Prepare 授权")
}
if (-not $unauthorized -or (Test-Path -LiteralPath $unauthorizedRun)) {
    throw "缺少 Primary Prepare token 时必须在创建 Run 前拒绝"
}

$wrongHashRun = Join-Path $caseRoot "rejected-decision-hash"
$wrongHash = $false
try {
    $badArguments = @{} + $prepareArguments
    $badArguments.RunDirectory = $wrongHashRun
    $badArguments.PublishedRunDirectory = $wrongHashRun
    $badArguments.ExpectedA2DecisionSha256 = ("0" * 64)
    & $PrepareScript @badArguments
} catch {
    $wrongHash = $_.Exception.Message.Contains("SHA-256")
}
$wrongHashStaging = @(Get-ChildItem -LiteralPath $caseRoot -Directory |
    Where-Object { $_.Name.StartsWith(".rejected-decision-hash.incoming-") })
if (-not $wrongHash -or (Test-Path -LiteralPath $wrongHashRun) -or
    $wrongHashStaging.Count -ne 0) {
    throw "A2 decision expected SHA 错误时必须拒绝且不遗留 staging"
}

Write-Host "Mouse Effect Probe Physical B Prepare integration passed: $caseRoot"
