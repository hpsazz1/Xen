param(
    [Parameter(Mandatory = $true)]
    [string]$ToolRoot,
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable,
    [Parameter(Mandatory = $true)]
    [string]$ConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsSourceBindingPath,
    [Parameter(Mandatory = $true)]
    [string]$PrimaryAnalysisPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedPrimaryAnalysisSha256,
    [Parameter(Mandatory = $true)]
    [string]$OfflineDesignPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedOfflineDesignSha256,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PrepareAuthorization
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$prepareConfirmation = "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_PREPARE_ONLY"
$physicalConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT"
if ($PrepareAuthorization -ne $prepareConfirmation) {
    throw "Physical B holdout Prepare 授权令牌不匹配；本入口不具 Launch 权限。"
}
foreach ($value in @(
        $ExpectedPrimaryAnalysisSha256, $ExpectedOfflineDesignSha256)) {
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "Physical B holdout expected SHA-256 必须是 64 位小写十六进制"
    }
}

function Get-FileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-FileIdentity([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不是普通文件：$Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).ProviderPath
    $before = Get-Item -LiteralPath $resolved
    $hash = Get-FileSha256 $resolved
    $after = Get-Item -LiteralPath $resolved
    if ($before.Length -ne $after.Length -or
        $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) {
        throw "$Description 在哈希期间发生变化：$resolved"
    }
    return [ordered]@{
        path = $resolved
        size = [uint64]$after.Length
        sha256 = $hash
    }
}

function Get-PublishedIdentity(
        [string]$LocalPath,
        [string]$PublishedPath,
        [string]$Description) {
    $identity = Get-FileIdentity $LocalPath $Description
    return [ordered]@{
        path = [IO.Path]::GetFullPath($PublishedPath)
        size = $identity.size
        sha256 = $identity.sha256
    }
}

function Copy-NewPublishedFile(
        [string]$Source,
        [string]$LocalDestination,
        [string]$PublishedDestination,
        [string]$Description) {
    if (Test-Path -LiteralPath $LocalDestination) {
        throw "$Description 目标已存在，拒绝覆盖：$LocalDestination"
    }
    Copy-Item -LiteralPath $Source -Destination $LocalDestination
    return Get-PublishedIdentity `
        $LocalDestination $PublishedDestination $Description
}

function Write-NewUtf8Text([string]$Path, [string]$Content) {
    if (Test-Path -LiteralPath $Path) {
        throw "文本发布目标已存在，拒绝覆盖：$Path"
    }
    [IO.File]::WriteAllText(
        $Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Write-NewUtf8Json([string]$Path, [object]$Value) {
    Write-NewUtf8Text $Path (
        ($Value | ConvertTo-Json -Depth 40) + [Environment]::NewLine)
}

function Read-IniSection([string]$Path, [string]$SectionName) {
    $values = [ordered]@{}
    $current = ""
    foreach ($rawLine in [IO.File]::ReadAllLines($Path)) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(";") -or
            $line.StartsWith("#")) {
            continue
        }
        if ($line -match '^\[(.+)\]$') {
            $current = $Matches[1].Trim()
            continue
        }
        if (-not [string]::Equals(
                $current, $SectionName,
                [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            throw "config [$SectionName] 存在非法行：$line"
        }
        $key = $line.Substring(0, $separator).Trim().ToLowerInvariant()
        if ($values.Contains($key)) {
            throw "config [$SectionName] 存在重复键：$key"
        }
        $values[$key] = $line.Substring($separator + 1).Trim()
    }
    if ($values.Count -eq 0) {
        throw "config 缺少 [$SectionName]"
    }
    return $values
}

function Get-RequiredValue(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    if (-not $Values.Contains($Name) -or
        [string]::IsNullOrWhiteSpace([string]$Values[$Name])) {
        throw "config 缺少 $Name"
    }
    return [string]$Values[$Name]
}

function Get-RequiredInteger(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $value = Get-RequiredValue $Values $Name
    if ($value -notmatch '^[0-9]+$') {
        throw "config 的 $Name 不是非负整数"
    }
    return [int]$value
}

function Get-RequiredBoolean(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name) {
    $value = (Get-RequiredValue $Values $Name).ToLowerInvariant()
    if ($value -eq "true") { return $true }
    if ($value -eq "false") { return $false }
    throw "config 的 $Name 不是布尔值"
}

$sourcePrimary = Get-FileIdentity `
    $PrimaryAnalysisPath "Physical B Primary analysis"
$sourceDesign = Get-FileIdentity `
    $OfflineDesignPath "Physical B offline design"
if ([string]$sourcePrimary.sha256 -ne $ExpectedPrimaryAnalysisSha256) {
    throw "Physical B Primary analysis expected SHA-256 不匹配"
}
if ([string]$sourceDesign.sha256 -ne $ExpectedOfflineDesignSha256) {
    throw "Physical B offline design expected SHA-256 不匹配"
}
$primaryReportPath = Join-Path `
    (Split-Path -Parent $sourcePrimary.path) "command-report.json"
$sourcePrimaryReport = Get-FileIdentity `
    $primaryReportPath "Physical B Primary command report"
$sourceConfig = Get-FileIdentity $ConfigPath "Physical B holdout config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "Physical B holdout OBS source binding"
$resolvedPython = (Resolve-Path -LiteralPath $PythonExecutable).ProviderPath
$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).ProviderPath
$primaryAnalyzerSource = Join-Path `
    $PSScriptRoot "analyze_mouse_effect_probe_b.py"
$holdoutAnalyzerSource = Join-Path `
    $PSScriptRoot "analyze_mouse_effect_probe_b_holdout.py"
[void](Get-FileIdentity $primaryAnalyzerSource "Physical B Primary analyzer")
[void](Get-FileIdentity $holdoutAnalyzerSource "Physical B holdout analyzer")

$capture = Read-IniSection $sourceConfig.path "capture"
$mouse = Read-IniSection $sourceConfig.path "mouse"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouse "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouse "allow_send_input")) {
    throw "Physical B holdout Prepare 要求精确 NDI/source clock、KMBOX NET 且默认输出关闭"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
if ($roiWidth -ne 320 -or $roiHeight -ne 320) {
    throw "Physical B holdout 只接受冻结 F1 的 320x320 witness scope"
}

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "Physical B holdout RunDirectory 已存在，拒绝覆盖：$resolvedRun"
}
foreach ($path in @($resolvedRun, $resolvedPublishedRun)) {
    if ([string]::Equals(
            $path, [IO.Path]::GetPathRoot($path),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Physical B holdout RunDirectory 不能是根目录"
    }
}
$runParent = Split-Path -Parent $resolvedRun
if (-not (Test-Path -LiteralPath $runParent -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $runParent)
}
$runName = Split-Path -Leaf $resolvedRun
$stagingDirectory = Join-Path $runParent (
    ".{0}.incoming-{1}" -f $runName, [guid]::NewGuid().ToString("N"))
if (Test-Path -LiteralPath $stagingDirectory) {
    throw "Physical B holdout staging 已存在：$stagingDirectory"
}
[void](New-Item -ItemType Directory -Path $stagingDirectory)
$toolDirectory = Join-Path $stagingDirectory "tool"
$evidenceDirectory = Join-Path $stagingDirectory "evidence"
[void](New-Item -ItemType Directory -Path $toolDirectory)
[void](New-Item -ItemType Directory -Path $evidenceDirectory)

try {
    $publishedTool = Join-Path $resolvedPublishedRun "tool"
    $publishedEvidence = Join-Path $resolvedPublishedRun "evidence"
    function Copy-Tool([string]$Name, [string]$Description) {
        return Copy-NewPublishedFile `
            (Join-Path $resolvedToolRoot $Name) `
            (Join-Path $toolDirectory $Name) `
            (Join-Path $publishedTool $Name) $Description
    }

    $probeExecutable = Copy-Tool `
        "XenMouseEffectProbe.exe" "Physical B holdout probe executable"
    $sidecarExecutable = Copy-Tool `
        "XenCaptureEvidence.exe" "Physical B holdout sidecar executable"
    $sequenceExecutable = Copy-Tool `
        "XenMouseEffectProbeSequence.exe" "Physical B holdout sequence executable"
    $opencvRuntime = Copy-Tool `
        "opencv_world4140.dll" "Physical B holdout OpenCV runtime"
    $ndiRuntime = Copy-Tool `
        "Processing.NDI.Lib.x64.dll" "Physical B holdout NDI runtime"
    $ndiLicense = Copy-Tool `
        "Processing.NDI.Lib.Licenses.txt" "Physical B holdout NDI license"
    $launchScript = Copy-NewPublishedFile `
        (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
        (Join-Path $toolDirectory "launch_mouse_effect_probe_b.ps1") `
        (Join-Path $publishedTool "launch_mouse_effect_probe_b.ps1") `
        "Physical B holdout Launch script"
    $primaryAnalyzer = Copy-NewPublishedFile `
        $primaryAnalyzerSource `
        (Join-Path $toolDirectory "analyze_mouse_effect_probe_b.py") `
        (Join-Path $publishedTool "analyze_mouse_effect_probe_b.py") `
        "Physical B Primary analyzer"
    $holdoutAnalyzer = Copy-NewPublishedFile `
        $holdoutAnalyzerSource `
        (Join-Path $toolDirectory "analyze_mouse_effect_probe_b_holdout.py") `
        (Join-Path $publishedTool "analyze_mouse_effect_probe_b_holdout.py") `
        "Physical B holdout analyzer"
    $configCopy = Copy-NewPublishedFile `
        $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
        (Join-Path $resolvedPublishedRun "config.ini") `
        "Physical B holdout config copy"
    $obsCopy = Copy-NewPublishedFile `
        $sourceObsBinding.path `
        (Join-Path $stagingDirectory "obs-source-binding.json") `
        (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
        "Physical B holdout OBS binding copy"
    $primaryCopy = Copy-NewPublishedFile `
        $sourcePrimary.path `
        (Join-Path $evidenceDirectory "primary-analysis.json") `
        (Join-Path $publishedEvidence "primary-analysis.json") `
        "Physical B Primary analysis copy"
    $primaryReportCopy = Copy-NewPublishedFile `
        $sourcePrimaryReport.path `
        (Join-Path $evidenceDirectory "primary-command-report.json") `
        (Join-Path $publishedEvidence "primary-command-report.json") `
        "Physical B Primary command report copy"
    $designCopy = Copy-NewPublishedFile `
        $sourceDesign.path `
        (Join-Path $evidenceDirectory "offline-design.json") `
        (Join-Path $publishedEvidence "offline-design.json") `
        "Physical B offline design copy"

    $planPath = Join-Path $stagingDirectory "holdout-plan.json"
    $holdoutAnalyzerLocal = Join-Path `
        $toolDirectory "analyze_mouse_effect_probe_b_holdout.py"
    & $resolvedPython $holdoutAnalyzerLocal bind `
        --primary-analysis $sourcePrimary.path `
        --primary-analysis-sha256 $ExpectedPrimaryAnalysisSha256 `
        --offline-design $sourceDesign.path `
        --offline-design-sha256 $ExpectedOfflineDesignSha256 `
        --output $planPath
    if ($LASTEXITCODE -ne 0) {
        throw "Physical B holdout plan binder 失败，ExitCode=$LASTEXITCODE"
    }
    $plan = Get-Content -LiteralPath $planPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([int]$plan.schema_version -ne 1 -or
        [string]$plan.evidence_type -ne
            "mouse_effect_probe_physical_b_holdout_plan" -or
        [string]$plan.status -ne
            "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE" -or
        [bool]$plan.physical_output_capability -or
        [bool]$plan.physical_b_launch_authorized -or
        [bool]$plan.production_aim_changed -or
        [bool]$plan.holdout_used_for_tuning -or
        [string]$plan.bindings.primary_analysis_file_sha256 -ne
            [string]$primaryCopy.sha256 -or
        [string]$plan.bindings.primary_command_report_file_sha256 -ne
            [string]$primaryReportCopy.sha256 -or
        [string]$plan.bindings.primary_analyzer_file_sha256 -ne
            [string]$primaryAnalyzer.sha256 -or
        [string]$plan.bindings.offline_design_file_sha256 -ne
            [string]$designCopy.sha256 -or
        [string]$plan.bindings.holdout_analyzer_file_sha256 -ne
            [string]$holdoutAnalyzer.sha256 -or
        [string]$plan.sequence.profile -ne "physical_b_prbs_holdout" -or
        [uint64]$plan.sequence.sample_count -ne 288 -or
        [uint64]$plan.sequence.block_count -ne 2 -or
        [uint64]$plan.sequence.expected_nonzero_transition_count -ne 68) {
        throw "Physical B holdout plan 身份、哈希或 no-tuning 合同无效"
    }
    $planIdentity = Get-PublishedIdentity `
        $planPath (Join-Path $resolvedPublishedRun "holdout-plan.json") `
        "Physical B holdout plan"

    $request = $plan.sequence
    $frozenOfflineSequence = $plan.offline_sequence
    $sequencePath = Join-Path $stagingDirectory "sequence.json"
    & (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
        --output $sequencePath `
        --profile physical-b-holdout `
        --guard-samples 32 `
        --lfsr-order ([uint64]$request.lfsr.order) `
        --feedback-mask ([uint64]$request.lfsr.feedback_mask) `
        --seed ([uint64]$request.lfsr.seed) `
        --phase ([uint64]$request.lfsr.phase) `
        --offline-sequence-semantic-sha256 `
            ([string]$request.sequence_semantic_sha256)
    if ($LASTEXITCODE -ne 0) {
        throw "Physical B holdout sequence tool 失败，ExitCode=$LASTEXITCODE"
    }
    $sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $samples = @($sequence.samples)
    $blocks = @($sequence.blocks)
    $frozenSamples = @($frozenOfflineSequence.samples)
    $frozenBlocks = @($frozenOfflineSequence.blocks)
    $transitions = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
    if ([int]$sequence.schema -ne 5 -or
        [string]$sequence.profile -ne "physical_b_prbs_holdout" -or
        [string]$sequence.request.offline_sequence_semantic_sha256 -ne
            [string]$request.sequence_semantic_sha256 -or
        $samples.Count -ne 288 -or $blocks.Count -ne 2 -or
        $transitions.Count -ne 68 -or
        [int64]$sequence.summary.net_x_counts -ne 0 -or
        [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
        (@($blocks | ForEach-Object { [string]$_.role }) -join ",") -ne
            "cross_run_holdout,cross_run_holdout" -or
        (@($blocks | ForEach-Object { [string]$_.polarity }) -join ",") -ne
            "normal,inverted" -or
        @($samples | Where-Object {
            [int]$_.dx_counts -lt -1 -or [int]$_.dx_counts -gt 1 -or
            [int]$_.dy_counts -ne 0
        }).Count -ne 0) {
        throw "Physical B holdout sequence 与冻结 recurrence/X-only/net/prefix 不一致"
    }
    if ($frozenSamples.Count -ne $samples.Count -or
        $frozenBlocks.Count -ne $blocks.Count) {
        throw "Physical B holdout C++/offline exact sequence 容量不一致"
    }
    for ($index = 0; $index -lt $samples.Count; ++$index) {
        $actual = $samples[$index]
        $expected = $frozenSamples[$index]
        if ([uint64]$actual.sample_index -ne [uint64]$expected.sample_index -or
            [uint64]$actual.block_id -ne [uint64]$expected.block_id -or
            [string]$actual.phase -ne [string]$expected.phase -or
            [int]$actual.dx_counts -ne [int]$expected.command_dx_counts -or
            [int]$actual.dy_counts -ne [int]$expected.command_dy_counts) {
            throw "Physical B holdout sample $index 偏离 offline exact sequence"
        }
    }
    for ($index = 0; $index -lt $blocks.Count; ++$index) {
        $actual = $blocks[$index]
        $expected = $frozenBlocks[$index]
        if ([uint64]$actual.block_id -ne [uint64]$expected.block_id -or
            [uint64]$actual.pair_index -ne [uint64]$expected.pair_index -or
            [string]$actual.role -ne [string]$expected.role -or
            [string]$actual.polarity -ne [string]$expected.polarity -or
            [uint64]$actual.first_sample_index -ne
                [uint64]$expected.first_sample_index -or
            [uint64]$actual.period_sample_count -ne
                [uint64]$expected.period_sample_count -or
            [uint64]$actual.sample_count -ne [uint64]$expected.sample_count) {
            throw "Physical B holdout block $index 偏离 offline exact sequence"
        }
    }
    $sequenceIdentity = Get-PublishedIdentity `
        $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
        "Physical B holdout sequence"

    $runUuid = [guid]::NewGuid().ToString()
    $activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    if ($runUuid -eq [string]$plan.primary.run_uuid -or
        $activationEpoch -eq [uint64]$plan.primary.activation_epoch) {
        throw "Physical B holdout 未获得独立 Run UUID/activation"
    }
    $bindingPath = Join-Path $stagingDirectory "probe-binding.json"
    $binding = [ordered]@{
        schema_version = 4
        evidence_type = "mouse_effect_probe_binding"
        experiment = "physical_b_cross_run_holdout"
        run_uuid = $runUuid
        activation_epoch = $activationEpoch
        dispatch_mode = "physical_b"
        profile = "physical_b_prbs_holdout"
        run_role = "cross_run_holdout"
        scope_id = [string]$plan.primary.scope_id
        sequence_sha256 = [string]$sequence.sequence_sha256
        offline_sequence_semantic_sha256 =
            [string]$request.sequence_semantic_sha256
        sequence_file = $sequenceIdentity
        holdout_plan = $planIdentity
        primary_run_uuid = [string]$plan.primary.run_uuid
        primary_activation_epoch = [uint64]$plan.primary.activation_epoch
        primary_source_clock_session_id =
            [string]$plan.primary.source_clock_session_id
        primary_analysis = $primaryCopy
        capture_source_name = $ndiSource
        config = $configCopy
        obs_source_binding = $obsCopy
        source_offline_design = $designCopy
        sidecar_physical_output_capability = $false
        normal_aim_output_required = $false
        identification_input_definition = "cumulative_position_counts"
        actuator_audit_input = "completed_command_dx_counts"
        dy_counts_required = 0
        max_abs_sample_counts = 1
        max_abs_prefix_x_counts = 1
        holdout_used_for_tuning = $false
    }
    Write-NewUtf8Json $bindingPath $binding
    $bindingIdentity = Get-PublishedIdentity `
        $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
        "Physical B holdout probe binding"

    $taskPath = Join-Path $stagingDirectory "task.json"
    $task = [ordered]@{
        schema_version = 6
        evidence_type = "mouse_effect_probe_b_task"
        status = "PREPARED"
        experiment = "physical_b_cross_run_holdout"
        run_role = "cross_run_holdout"
        run_directory = $resolvedPublishedRun
        run_uuid = $runUuid
        activation_epoch = $activationEpoch
        dispatch_mode = "physical_b"
        profile = "physical_b_prbs_holdout"
        scope_id = [string]$plan.primary.scope_id
        sequence_sha256 = [string]$sequence.sequence_sha256
        offline_sequence_semantic_sha256 =
            [string]$request.sequence_semantic_sha256
        sequence_sample_count = [uint64]$samples.Count
        expected_nonzero_transition_count = [uint64]$transitions.Count
        max_abs_prefix_x_counts = [uint64]1
        physical_output_capability = $true
        physical_output_confirmation = $physicalConfirmation
        requires_user_frontend_launch = $true
        cross_run_holdout_prepare_authorized = $true
        holdout_used_for_tuning = $false
        files = [ordered]@{
            launch_script = $launchScript
            probe_executable = $probeExecutable
            sidecar_executable = $sidecarExecutable
            sequence_executable = $sequenceExecutable
            opencv_runtime = $opencvRuntime
            ndi_runtime = $ndiRuntime
            ndi_license = $ndiLicense
            physical_b_analyzer = $primaryAnalyzer
            holdout_analyzer = $holdoutAnalyzer
            config = $configCopy
            sequence = $sequenceIdentity
            probe_binding = $bindingIdentity
            obs_source_binding = $obsCopy
            holdout_plan = $planIdentity
            primary_analysis = $primaryCopy
            primary_command_report = $primaryReportCopy
            offline_design = $designCopy
        }
        primary = [ordered]@{
            run_uuid = [string]$plan.primary.run_uuid
            activation_epoch = [uint64]$plan.primary.activation_epoch
            source_clock_session_id =
                [string]$plan.primary.source_clock_session_id
            analysis_semantic_sha256 =
                [string]$plan.primary.analysis_semantic_sha256
            f1_semantic_sha256 = [string]$plan.primary.f1_semantic_sha256
        }
        holdout = [ordered]@{
            plan_semantic_sha256 =
                [string]$plan.holdout_plan_semantic_sha256
            recurrence_feedback_mask = [uint64]$request.lfsr.feedback_mask
            recurrence_phase = [uint64]$request.lfsr.phase
            input_forced_required = $true
            output_free_run_required = $true
            delay_and_tail_are_frozen = $true
            holdout_used_for_tuning = $false
        }
        capture = [ordered]@{
            source_name = $ndiSource
            frame_layout = (Get-RequiredValue `
                $capture "ndi_frame_layout").ToLowerInvariant()
            source_width = Get-RequiredInteger $capture "ndi_source_width"
            source_height = Get-RequiredInteger $capture "ndi_source_height"
            roi_width = $roiWidth
            roi_height = $roiHeight
            center_roi = Get-RequiredBoolean $capture "center_roi"
            roi_x = Get-RequiredInteger $capture "roi_x"
            roi_y = Get-RequiredInteger $capture "roi_y"
            discovery_timeout_ms = Get-RequiredInteger `
                $capture "ndi_discovery_timeout_ms"
            receive_timeout_ms = Get-RequiredInteger `
                $capture "ndi_receive_timeout_ms"
            disconnect_timeout_ms = Get-RequiredInteger `
                $capture "ndi_disconnect_timeout_ms"
            clock_sync_url = Get-RequiredValue $capture "ndi_clock_sync_url"
            clock_sync_interval_ms = Get-RequiredInteger `
                $capture "ndi_clock_sync_interval_ms"
            clock_sync_timeout_ms = Get-RequiredInteger `
                $capture "ndi_clock_sync_timeout_ms"
            clock_mapping_max_age_ms = Get-RequiredInteger `
                $capture "ndi_clock_mapping_max_age_ms"
            require_frame_metadata = Get-RequiredBoolean `
                $capture "ndi_require_frame_metadata"
        }
        sidecar = [ordered]@{
            frames = [uint64]2400
            max_seconds = [uint64]25
            publishing_max_seconds = [uint64]180
            physical_output_capability = $false
            capture_source_name = $ndiSource
            frame_layout = Get-RequiredValue $capture "ndi_frame_layout"
            source_width = Get-RequiredInteger $capture "ndi_source_width"
            source_height = Get-RequiredInteger $capture "ndi_source_height"
            roi_width = $roiWidth
            roi_height = $roiHeight
            left_witness_roi = "16,48,96,224"
            right_witness_roi = "208,48,96,224"
            require_source_timing = $true
        }
        safety = [ordered]@{
            normal_aim_must_be_closed = $true
            emergency_virtual_keys = @(35, 119)
            any_failure_stops_without_compensation = $true
            zero_y_required = $true
            max_abs_sample_counts = 1
            max_abs_prefix_x_counts = 1
            no_runtime_amplitude_or_repetition_change = $true
            cross_run_holdout_prepare_authorized = $true
        }
    }
    Write-NewUtf8Json $taskPath $task

    $launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
        '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
        '-PhysicalOutputConfirmation {2}') -f
        $launchScript.path, $resolvedPublishedRun, $physicalConfirmation
    $taskMarkdown = @(
        "# Mouse Effect Probe Physical B cross-Run holdout",
        "",
        "- Run UUID：``$runUuid``；Primary：``$($plan.primary.run_uuid)``",
        "- 序列：288 samples；一个独立 normal/inverted pair；0x33/phase 21；Y=0、净 X=0、最大前缀=1 count",
        "- 裁决：只使用 Primary 已冻结 F1；input-forced 与 output-free-run 均需通过；本 Run 不得用于调参",
        '- 安全：启动时右键松开；看到“按住右键”后在 5 秒内按住并保持，直到看到“现在松开右键”；松键、End、F8 或任何失败立即停发且不补偿',
        "",
        "下面命令会发送真实 KMBOX 输入，只能由用户在本机前台执行：",
        "",
        "``````powershell",
        $launchCommand,
        "``````",
        "",
        "执行后请回传：是否看到轻微视角变化；是否移动物理鼠标/WASD；是否有方向异常、遮挡/scene cut 或急停。",
        "") -join [Environment]::NewLine
    Write-NewUtf8Text (Join-Path $stagingDirectory "TASK.md") $taskMarkdown
    $observation = @(
        "# Physical B cross-Run holdout 人工观察",
        "",
        "- Run UUID：``$runUuid``",
        "- 用户原话：",
        "- 可见视角变化：",
        "- 物理鼠标/WASD：",
        "- 正/负方向与左右 witness：",
        "- 遮挡/scene cut：",
        "- 异常/急停：",
        "- 人工结论：",
        "") -join [Environment]::NewLine
    Write-NewUtf8Text `
        (Join-Path $stagingDirectory "OBSERVATION.md") $observation
    $summary = [ordered]@{
        schema_version = 1
        evidence_type = "mouse_effect_probe_b_prepare"
        status = "PREPARED_NOT_LAUNCHED"
        run_uuid = $runUuid
        run_role = "cross_run_holdout"
        profile = "physical_b_prbs_holdout"
        scope_id = [string]$task.scope_id
        local_bundle_directory = $resolvedRun
        published_run_directory = $resolvedPublishedRun
        primary_run_uuid = [string]$plan.primary.run_uuid
        primary_activation_epoch = [uint64]$plan.primary.activation_epoch
        f1_semantic_sha256 = [string]$plan.primary.f1_semantic_sha256
        holdout_plan_semantic_sha256 =
            [string]$plan.holdout_plan_semantic_sha256
        sequence_sha256 = [string]$sequence.sequence_sha256
        sequence_sample_count = [uint64]$samples.Count
        expected_nonzero_transition_count = [uint64]$transitions.Count
        cross_run_holdout_prepare_authorized = $true
        holdout_used_for_tuning = $false
        physical_launch_executed = $false
    }
    Write-NewUtf8Json `
        (Join-Path $stagingDirectory "prepare-summary.json") $summary

    Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
    Write-Host "Physical B holdout Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
    Write-Host "BundleDirectory=$resolvedRun"
    Write-Host "PublishedRunDirectory=$resolvedPublishedRun"
    Write-Host $launchCommand
} catch {
    $failure = $_
    if (Test-Path -LiteralPath $stagingDirectory -PathType Container) {
        $stagingFull = [IO.Path]::GetFullPath($stagingDirectory)
        $runParentFull = [IO.Path]::GetFullPath($runParent)
        $runParentPrefix = $runParentFull.TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar) +
            [IO.Path]::DirectorySeparatorChar
        $expectedStagingPrefix = ".{0}.incoming-" -f $runName
        if (-not $stagingFull.StartsWith(
                $runParentPrefix,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not (Split-Path -Leaf $stagingFull).StartsWith(
                $expectedStagingPrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Physical B holdout staging 清理目标越界：$stagingFull"
        }
        Remove-Item -LiteralPath $stagingFull -Recurse -Force
    }
    throw $failure
}
