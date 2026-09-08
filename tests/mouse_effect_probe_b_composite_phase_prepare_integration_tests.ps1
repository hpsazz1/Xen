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

# 这是测试自建的模拟包；借用 Release 二进制只执行无输出的 Sequence，
# 不把 fixture manifest 当作正式发布或真实采集身份。
$fixtureToolRoot = Join-Path $caseRoot "tool-package"
[void](New-Item -ItemType Directory -Path $fixtureToolRoot)
$sourceScriptRoot = Split-Path -Parent $PrepareScript
$nativeNames = @(
    "XenMouseEffectProbe.exe", "XenCaptureEvidence.exe",
    "XenMouseEffectProbeSequence.exe", "XenMouseEffectProbeCompositeSeal.exe",
    "opencv_world4140.dll", "Processing.NDI.Lib.x64.dll",
    "Processing.NDI.Lib.Licenses.txt")
$scriptNames = @(
    "prepare_mouse_effect_probe_b.ps1", "prepare_mouse_effect_probe_b_holdout.ps1",
    "prepare_mouse_effect_probe_b_command_magnitude.ps1",
    "prepare_mouse_effect_probe_b_composite_phase.ps1",
    "launch_mouse_effect_probe_a.ps1", "design_mouse_effect_probe_prbs.py",
    "analyze_mouse_effect_probe_b.py", "analyze_mouse_effect_probe_b_holdout.py",
    "analyze_mouse_effect_probe_b_command_magnitude.py",
    "freeze_mouse_effect_probe_b_composite_phase_plan.py",
    "produce_mouse_effect_probe_b_composite_phase_ledgers.py",
    "bind_mouse_effect_probe_b_composite_phase_calibration.py",
    "evaluate_mouse_effect_probe_b_composite_phase.py")
foreach ($name in $nativeNames) {
    Copy-Item -LiteralPath (Join-Path $ToolRoot $name) -Destination $fixtureToolRoot
}
foreach ($name in $scriptNames) {
    Copy-Item -LiteralPath (Join-Path $sourceScriptRoot $name) -Destination $fixtureToolRoot
}
$fixtureCommit = "1" * 40
Write-NewUtf8Json (Join-Path $fixtureToolRoot "xen-build-identity.json") ([ordered]@{
    schema = 1; git_commit = $fixtureCommit; git_dirty = $false
    runtime = "nvidia"; components = @("fixture_only")
})
$fixtureLauncher = Join-Path $fixtureToolRoot "launch_mouse_effect_probe_a.ps1"
[IO.File]::AppendAllText(
    $fixtureLauncher, "`n# TEST_FIXTURE_SELECTED_PACKAGE`n", [Text.UTF8Encoding]::new($false))
$fixtureFiles = @()
foreach ($name in @($nativeNames) + @("xen-build-identity.json") + @($scriptNames)) {
    $path = Join-Path $fixtureToolRoot $name
    $fixtureFiles += [ordered]@{
        name = $name; size = (Get-Item -LiteralPath $path).Length
        sha256 = Get-LowerSha256 $path
        provenance = [ordered]@{ kind = "test_fixture"; git_commit = $fixtureCommit }
    }
}
$fixtureManifestPath = Join-Path $fixtureToolRoot "manifest.json"
$fixtureManifest = [ordered]@{
    schema_version = 1; evidence_type = "mouse_effect_probe_b_tool_package"
    package_name = "MouseEffectProbe-B-1111111"; git_commit = $fixtureCommit
    source_tracked_clean = $true; source_untracked_files_excluded = $true
    build_identity_git_dirty = $false; runtime = "nvidia"; file_count = 21
    physical_run_included = $false; physical_launch_executed = $false
    launch_requires_user_frontend_action = $true
    composite_phase_tooling_included = $true; composite_phase_run_included = $false
    files = $fixtureFiles
}
Write-NewUtf8Json $fixtureManifestPath $fixtureManifest

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
    ToolRoot = $fixtureToolRoot
    ExpectedToolCommit = $fixtureCommit
    ExpectedToolManifestSha256 = Get-LowerSha256 $fixtureManifestPath
    ConfigPath = $configPath
    ObsSourceBindingPath = $obsPath
    ObsLogPath = $logPath
    PythonExecutable = $PythonExecutable
    RunDirectory = $runDirectory
    PublishedRunDirectory = $runDirectory
    MaxSeconds = 15
}

function Assert-PackageRejected(
        [string]$Name,
        [hashtable]$Overrides,
        [string]$ExpectedDetail) {
    $badRun = Join-Path $caseRoot $Name
    $badArguments = @{} + $arguments
    $badArguments.RunDirectory = $badRun
    $badArguments.PublishedRunDirectory = $badRun
    foreach ($key in $Overrides.Keys) { $badArguments[$key] = $Overrides[$key] }
    $failure = ""
    try {
        & $PrepareScript @badArguments
    } catch {
        $failure = $_.Exception.Message
    }
    if (-not $failure.Contains($ExpectedDetail) -or
        (Test-Path -LiteralPath $badRun) -or
        @(Get-ChildItem -LiteralPath $caseRoot -Directory -Filter ".$Name.incoming-*").Count -ne 0) {
        throw "Prepare did not reject $Name before creating Run: $failure"
    }
    Write-Host "Package rejection passed: $Name"
}

Assert-PackageRejected "wrong-commit" @{
    ExpectedToolCommit = "2" * 40
} "commit"
Assert-PackageRejected "wrong-manifest" @{
    ExpectedToolManifestSha256 = "0" * 64
} "manifest SHA-256"
Assert-PackageRejected "unknown-policy" @{ SchedulerPolicy = "active-2ms-v1" } "SchedulerPolicy"
Assert-PackageRejected "auto-overlong" @{ BoundedCompositeAutoArm = $true; MaxSeconds = 16 } "15"
Assert-PackageRejected "event-without-auto" @{ BoundedCompositeEventMonitor = $true } "BoundedCompositeAutoArm"
Assert-PackageRejected "event-overlong" @{
    BoundedCompositeAutoArm = $true; BoundedCompositeEventMonitor = $true; MaxSeconds = 16
} "15"

$launcherBytes = [IO.File]::ReadAllBytes($fixtureLauncher)
$oldLauncherPath = Join-Path $ToolRoot "launch_mouse_effect_probe_a.ps1"
$oldLauncherBytes = if (Test-Path -LiteralPath $oldLauncherPath -PathType Leaf) {
    [IO.File]::ReadAllBytes($oldLauncherPath)
} else {
    # CTest 的输入通常仅含构建产物；此处用旧调用形式作不执行的替换字节。
    [Text.Encoding]::UTF8.GetBytes('$sealOutput = @(& $sealExecutablePath 2>&1)')
}
try {
    [IO.File]::WriteAllBytes($fixtureLauncher, $oldLauncherBytes)
    Assert-PackageRejected "mixed-old-launcher" @{} "launch_mouse_effect_probe_a.ps1"
} finally {
    [IO.File]::WriteAllBytes($fixtureLauncher, $launcherBytes)
}

$packagePreparePath = Join-Path $fixtureToolRoot "prepare_mouse_effect_probe_b_composite_phase.ps1"
$prepareBytes = [IO.File]::ReadAllBytes($packagePreparePath)
$prepareRecord = $fixtureManifest.files | Where-Object {
    $_.name -eq "prepare_mouse_effect_probe_b_composite_phase.ps1"
}
$prepareSize = $prepareRecord.size
$prepareSha256 = $prepareRecord.sha256
try {
    [IO.File]::AppendAllText(
        $packagePreparePath, "`n# DIFFERENT_PACKAGE_CALLER`n", [Text.UTF8Encoding]::new($false))
    $prepareRecord.size = (Get-Item -LiteralPath $packagePreparePath).Length
    $prepareRecord.sha256 = Get-LowerSha256 $packagePreparePath
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
    Assert-PackageRejected "caller-drift" @{
        ExpectedToolManifestSha256 = Get-LowerSha256 $fixtureManifestPath
    } "Prepare"
} finally {
    [IO.File]::WriteAllBytes($packagePreparePath, $prepareBytes)
    $prepareRecord.size = $prepareSize
    $prepareRecord.sha256 = $prepareSha256
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
}

$sealRecord = $fixtureManifest.files | Where-Object {
    $_.name -eq "XenMouseEffectProbeCompositeSeal.exe"
}
try {
    $sealRecord.name = "unselected-fixture.exe"
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
    Assert-PackageRejected "missing-required-entry" @{
        ExpectedToolManifestSha256 = Get-LowerSha256 $fixtureManifestPath
    } "XenMouseEffectProbeCompositeSeal.exe"
} finally {
    $sealRecord.name = "XenMouseEffectProbeCompositeSeal.exe"
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
}

try {
    $fixtureManifest.composite_phase_tooling_included = $false
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
    Assert-PackageRejected "invalid-package-header" @{
        ExpectedToolManifestSha256 = Get-LowerSha256 $fixtureManifestPath
    } "header"
} finally {
    $fixtureManifest.composite_phase_tooling_included = $true
    Write-NewUtf8Json $fixtureManifestPath $fixtureManifest
}

& $PrepareScript @arguments

if ((Get-LowerSha256 (Join-Path $runDirectory "tool\launch_mouse_effect_probe_a.ps1")) -ne
        (Get-LowerSha256 $fixtureLauncher)) {
    throw "Prepare copied Launcher from outside the explicitly selected ToolRoot"
}

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
foreach ($document in @($task, $summary)) {
    if ([string]$document.tool_package.git_commit -cne $fixtureCommit -or
        [string]$document.tool_package.package_name -cne $fixtureManifest.package_name -or
        [string]$document.tool_package.manifest.sha256 -cne $arguments.ExpectedToolManifestSha256 -or
        [string]$document.tool_package.prepare_script.sha256 -cne (Get-LowerSha256 $PrepareScript)) {
        throw "Prepared tool package provenance does not match the selected fixture"
    }
    Assert-Identity $document.tool_package.manifest "selected manifest"
    Assert-Identity $document.tool_package.prepare_script "selected caller"
}
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
    -not [bool]$task.safety.right_button_deadman_required -or
    [bool]$task.safety.bounded_composite_auto_arm -or
    [bool]$task.safety.bounded_composite_event_monitor -or
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
foreach ($scheduler in @($sequence.request, $seed.scheduler_policy)) {
    if ([string]$scheduler.timer_mode -cne "HIGH_RESOLUTION_ONE_SHOT_OR_FAIL" -or
        [uint64]$scheduler.active_guard_ns -ne 300000 -or
        [uint64]$scheduler.max_wake_lateness_ns -ne 150000 -or
        [uint64]$scheduler.max_event_interval_width_ns -ne 100000 -or
        [uint64]$scheduler.max_active_wait_ns_per_event -ne 350000 -or
        [uint64]$scheduler.max_active_wait_ns_total -ne 14700000) {
        throw "Prepare 默认 legacy 必须保留完整原合同"
    }
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

foreach ($autoArm in @($false, $true)) {
    $newRun = Join-Path $caseRoot $(if ($autoArm) { 'active-auto' } else { 'active-manual' })
    $newArguments = @{} + $arguments
    $newArguments.RunDirectory = $newRun
    $newArguments.PublishedRunDirectory = $newRun
    $newArguments.SchedulerPolicy = 'active-1ms-v1'
    $newArguments.BoundedCompositeAutoArm = $autoArm
    & $PrepareScript @newArguments
    $newTask = Get-Content -LiteralPath (Join-Path $newRun 'task.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $newSequence = Get-Content -LiteralPath (Join-Path $newRun 'sequence.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $newSeed = Get-Content -LiteralPath (Join-Path $newRun 'composite-phase-plan-seed.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $newSummary = Get-Content -LiteralPath (Join-Path $newRun 'prepare-summary.json') -Raw -Encoding utf8 | ConvertFrom-Json
    foreach ($scheduler in @($newSequence.request, $newSeed.scheduler_policy)) {
        if ([string]$scheduler.timer_mode -cne 'HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1' -or
            [uint64]$scheduler.active_guard_ns -ne 1000000 -or
            [uint64]$scheduler.max_wake_lateness_ns -ne 150000 -or
            [uint64]$scheduler.max_event_interval_width_ns -ne 100000 -or
            [uint64]$scheduler.max_active_wait_ns_per_event -ne 1000000 -or
            [uint64]$scheduler.max_active_wait_ns_total -ne 42000000) {
            throw '显式新 Prepare 没有贯通 Sequence 和 seed 的完整固定策略'
        }
    }
    if ([string]$newTask.scope_id -ceq [string]$task.scope_id -or
        [string]$newSequence.sequence_sha256 -ceq [string]$sequence.sequence_sha256 -or
        [string]$newSeed.sequence_binding.sequence_semantic_sha256 -cne [string]$newSequence.sequence_sha256 -or
        [string]$newSummary.status -cne 'PREPARED_NOT_LAUNCHED' -or
        [bool]$newSummary.scheduler_preflight_executed -or [bool]$newSummary.physical_launch_executed -or
        [bool]$newTask.safety.bounded_composite_auto_arm -ne $autoArm -or
        [bool]$newTask.safety.bounded_composite_event_monitor -or
        [bool]$newTask.safety.right_button_deadman_required -eq $autoArm -or
        [bool]$newTask.requires_user_frontend_launch -eq $autoArm -or
        [uint64]$newTask.safety.max_abs_pulse_counts -ne 1 -or
        [uint64]$newTask.safety.max_abs_prefix_x_counts -ne 1 -or
        (@($newTask.safety.emergency_virtual_keys) -join ',') -ne '35,119' -or
        [string]$newTask.tool_package.manifest.sha256 -cne $arguments.ExpectedToolManifestSha256) {
        throw '新 Prepare 必须取得新 scope/sequence 身份并严格限定授权模式'
    }
    foreach ($property in $newTask.files.PSObject.Properties) {
        Assert-Identity $property.Value "new task.files.$($property.Name)"
    }
    foreach ($forbidden in @('scheduler-preflight.json', 'composite-phase-plan.json',
            'composite-schedule-ledger.json', 'command-report.json', 'safety-ledger.json', 'launch-summary.json', 'pixel-evidence')) {
        if (Test-Path -LiteralPath (Join-Path $newRun $forbidden)) {
            throw "新 Prepare 不得执行采集：$forbidden"
        }
    }
    $newMarkdown = Get-Content -LiteralPath (Join-Path $newRun 'TASK.md') -Raw -Encoding utf8
    if ($newMarkdown.Contains('System.Object[]') -or
        -not $newMarkdown.Contains('active-1ms-v1') -or
        -not $newMarkdown.Contains('-AllowPhysicalOutput') -or
        ($autoArm -and (-not $newMarkdown.Contains('自动有限取证') -or
            $newMarkdown.Contains('只能由用户在辅机前台执行'))) -or
        (-not $autoArm -and -not $newMarkdown.Contains('只能由用户在辅机前台执行'))) {
        throw '新 TASK 必须如实说明显式策略和自动/人工有限取证授权'
    }
}

$eventRun = Join-Path $caseRoot 'active-events'
$eventArguments = @{} + $arguments
$eventArguments.RunDirectory = $eventRun
$eventArguments.PublishedRunDirectory = $eventRun
$eventArguments.SchedulerPolicy = 'active-1ms-v1'
$eventArguments.BoundedCompositeAutoArm = $true
$eventArguments.BoundedCompositeEventMonitor = $true
& $PrepareScript @eventArguments
$eventTask = Get-Content -LiteralPath (Join-Path $eventRun 'task.json') -Raw -Encoding utf8 | ConvertFrom-Json
$eventSequence = Get-Content -LiteralPath (Join-Path $eventRun 'sequence.json') -Raw -Encoding utf8 | ConvertFrom-Json
$eventMarkdown = Get-Content -LiteralPath (Join-Path $eventRun 'TASK.md') -Raw -Encoding utf8
if ($eventTask.safety.bounded_composite_event_monitor -isnot [bool] -or
    -not $eventTask.safety.bounded_composite_event_monitor -or
    -not $eventTask.safety.bounded_composite_auto_arm -or
    $eventTask.safety.right_button_deadman_required -or $eventTask.requires_user_frontend_launch -or
    $eventTask.sidecar.max_seconds -gt 15 -or
    $eventTask.sequence_sample_count -ne 295 -or $eventTask.window_count -ne 42 -or
    $eventTask.expected_nonzero_transition_count -ne 38 -or
    $eventTask.safety.max_abs_pulse_counts -ne 1 -or $eventTask.safety.max_abs_prefix_x_counts -ne 1 -or
    (@($eventTask.safety.emergency_virtual_keys) -join ',') -ne '35,119' -or
    $eventSequence.request.timer_mode -cne 'HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1' -or
    -not $eventMarkdown.Contains('monitor 配置 ACK') -or
    -not $eventMarkdown.Contains('UNKNOWN') -or
    -not $eventMarkdown.Contains('已收到的 End/F8') -or
    -not $eventMarkdown.Contains('不证明全部监控事件已投递') -or
    $eventMarkdown.Contains('System.Object[]')) {
    throw '显式事件监控必须保留有限取证范围并说明首态未知和已收到的急停事件'
}
foreach ($property in $eventTask.files.PSObject.Properties) {
    Assert-Identity $property.Value "event task.files.$($property.Name)"
}
foreach ($forbidden in @('scheduler-preflight.json', 'composite-phase-plan.json',
        'composite-schedule-ledger.json', 'command-report.json', 'safety-ledger.json', 'launch-summary.json', 'pixel-evidence')) {
    if (Test-Path -LiteralPath (Join-Path $eventRun $forbidden)) {
        throw "事件模式 Prepare 不得执行采集：$forbidden"
    }
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
