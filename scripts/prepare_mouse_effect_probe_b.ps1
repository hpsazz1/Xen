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
    [string]$OfflineDesignPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedOfflineDesignSha256,
    [Parameter(Mandatory = $true)]
    [string]$A2DecisionPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedA2DecisionSha256,
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PublishedRunDirectory,
    [Parameter(Mandatory = $true)]
    [string]$PrepareAuthorization
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$prepareConfirmation = "XEN_MOUSE_EFFECT_PROBE_B_PRIMARY_PREPARE_ONLY"
$physicalConfirmation =
    "XEN_MOUSE_EFFECT_PROBE_B_SENDS_REAL_KMBOX_INPUT"
if ($PrepareAuthorization -ne $prepareConfirmation) {
    throw "Physical B Primary Prepare 授权令牌不匹配；本入口不具 Launch 权限。"
}
foreach ($value in @(
        $ExpectedOfflineDesignSha256, $ExpectedA2DecisionSha256)) {
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "Physical B 输入 expected SHA-256 必须是 64 位小写十六进制"
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
        ($Value | ConvertTo-Json -Depth 30) + [Environment]::NewLine)
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

$sourceDesign = Get-FileIdentity `
    $OfflineDesignPath "Physical B offline design"
$sourceDecision = Get-FileIdentity `
    $A2DecisionPath "A2 dependency decision"
if ([string]$sourceDesign.sha256 -ne $ExpectedOfflineDesignSha256) {
    throw "Physical B offline design expected SHA-256 不匹配"
}
if ([string]$sourceDecision.sha256 -ne $ExpectedA2DecisionSha256) {
    throw "A2 dependency decision expected SHA-256 不匹配"
}
$sourceConfig = Get-FileIdentity $ConfigPath "Physical B config.ini"
$sourceObsBinding = Get-FileIdentity `
    $ObsSourceBindingPath "Physical B OBS source binding"
$resolvedPython = (Resolve-Path -LiteralPath $PythonExecutable).ProviderPath
$resolvedToolRoot = (Resolve-Path -LiteralPath $ToolRoot).ProviderPath
$designerSource = Join-Path $PSScriptRoot "design_mouse_effect_probe_prbs.py"
[void](Get-FileIdentity $designerSource "Physical B PRBS designer")
$analyzerSource = Join-Path $PSScriptRoot "analyze_mouse_effect_probe_b.py"
[void](Get-FileIdentity $analyzerSource "Physical B Primary analyzer")

$capture = Read-IniSection $sourceConfig.path "capture"
$mouse = Read-IniSection $sourceConfig.path "mouse"
if ((Get-RequiredValue $capture "backend").ToLowerInvariant() -ne "ndi" -or
    (Get-RequiredValue $capture "ndi_source_name") -eq "Auto" -or
    [string]::IsNullOrWhiteSpace(
        (Get-RequiredValue $capture "ndi_clock_sync_url")) -or
    (Get-RequiredValue $mouse "backend").ToLowerInvariant() -ne
        "kmbox_net" -or
    (Get-RequiredBoolean $mouse "allow_send_input")) {
    throw "Physical B Prepare 要求精确 NDI/source clock、KMBOX NET 且默认输出关闭"
}
$ndiSource = Get-RequiredValue $capture "ndi_source_name"
$roiWidth = Get-RequiredInteger $capture "roi_width"
$roiHeight = Get-RequiredInteger $capture "roi_height"
if ($roiWidth -ne 320 -or $roiHeight -ne 320) {
    throw "Physical B 当前 F0 只接受已校准的 320x320 witness scope"
}

$resolvedRun = [IO.Path]::GetFullPath($RunDirectory)
$resolvedPublishedRun = [IO.Path]::GetFullPath($PublishedRunDirectory)
if (Test-Path -LiteralPath $resolvedRun) {
    throw "Physical B RunDirectory 已存在，拒绝覆盖：$resolvedRun"
}
foreach ($path in @($resolvedRun, $resolvedPublishedRun)) {
    if ([string]::Equals(
            $path, [IO.Path]::GetPathRoot($path),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Physical B RunDirectory/PublishedRunDirectory 不能是根目录"
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
    throw "Physical B staging 已存在：$stagingDirectory"
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
        "XenMouseEffectProbe.exe" "Physical B probe executable"
    $sidecarExecutable = Copy-Tool `
        "XenCaptureEvidence.exe" "Physical B sidecar executable"
    $sequenceExecutable = Copy-Tool `
        "XenMouseEffectProbeSequence.exe" "Physical B sequence executable"
    $opencvRuntime = Copy-Tool `
        "opencv_world4140.dll" "Physical B OpenCV runtime"
    $ndiRuntime = Copy-Tool `
        "Processing.NDI.Lib.x64.dll" "Physical B NDI runtime"
    $ndiLicense = Copy-Tool `
        "Processing.NDI.Lib.Licenses.txt" "Physical B NDI license"
    $launchScript = Copy-NewPublishedFile `
        (Join-Path $PSScriptRoot "launch_mouse_effect_probe_a.ps1") `
        (Join-Path $toolDirectory "launch_mouse_effect_probe_b.ps1") `
        (Join-Path $publishedTool "launch_mouse_effect_probe_b.ps1") `
        "Physical B Launch script"
    $designer = Copy-NewPublishedFile `
        $designerSource `
        (Join-Path $toolDirectory "design_mouse_effect_probe_prbs.py") `
        (Join-Path $publishedTool "design_mouse_effect_probe_prbs.py") `
        "Physical B PRBS designer"
    $designerLocalPath = Join-Path `
        $toolDirectory "design_mouse_effect_probe_prbs.py"
    $analyzer = Copy-NewPublishedFile `
        $analyzerSource `
        (Join-Path $toolDirectory "analyze_mouse_effect_probe_b.py") `
        (Join-Path $publishedTool "analyze_mouse_effect_probe_b.py") `
        "Physical B Primary analyzer"

    $configCopy = Copy-NewPublishedFile `
        $sourceConfig.path (Join-Path $stagingDirectory "config.ini") `
        (Join-Path $resolvedPublishedRun "config.ini") `
        "Physical B config copy"
    $obsCopy = Copy-NewPublishedFile `
        $sourceObsBinding.path `
        (Join-Path $stagingDirectory "obs-source-binding.json") `
        (Join-Path $resolvedPublishedRun "obs-source-binding.json") `
        "Physical B OBS binding copy"
    $designCopy = Copy-NewPublishedFile `
        $sourceDesign.path `
        (Join-Path $evidenceDirectory "offline-design.json") `
        (Join-Path $publishedEvidence "offline-design.json") `
        "Physical B offline design copy"
    $decisionCopy = Copy-NewPublishedFile `
        $sourceDecision.path `
        (Join-Path $evidenceDirectory "a2-decision.json") `
        (Join-Path $publishedEvidence "a2-decision.json") `
        "A2 dependency decision copy"

    $f0Path = Join-Path $stagingDirectory "f0-primary.json"
    & $resolvedPython $designerLocalPath bind-primary `
        --offline-design $sourceDesign.path `
        --expected-offline-design-sha256 $ExpectedOfflineDesignSha256 `
        --a2-decision $sourceDecision.path `
        --expected-a2-decision-sha256 $ExpectedA2DecisionSha256 `
        --output $f0Path
    if ($LASTEXITCODE -ne 0) {
        throw "Physical B Primary F0 binder 失败，ExitCode=$LASTEXITCODE"
    }
    $f0 = Get-Content -LiteralPath $f0Path -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([int]$f0.schema_version -ne 2 -or
        [string]$f0.evidence_type -ne
            "mouse_effect_probe_physical_b_primary_f0" -or
        [string]$f0.status -ne "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE" -or
        [bool]$f0.physical_output_capability -or
        [bool]$f0.physical_b_launch_authorized -or
        [bool]$f0.production_aim_changed -or
        -not [bool]$f0.physical_b_primary_prepare_gate.ready -or
        [bool]$f0.cross_run_holdout.prepare_allowed -or
        [int]$f0.model_contract.core_delay_samples -ne 4 -or
        (@($f0.model_contract.tail_lengths) -join ",") -ne "0,1,2,4,8" -or
        [string]$f0.model_contract.nuisance_fit_rows -ne
            "exact_dedicated_pre_guard_only" -or
        -not [bool]$f0.model_contract.selection_used_for_single_refit -or
        [bool]$f0.model_contract.confirmation_used_for_refit -or
        [string]$f0.analyzer.file_sha256 -ne [string]$analyzer.sha256 -or
        [string]$f0.analyzer.contract_semantic_sha256 -ne
            [string]$f0.analysis_contract.contract_semantic_sha256 -or
        [string]$f0.source_offline_design.file_sha256 -ne
            $ExpectedOfflineDesignSha256 -or
        [string]$f0.source_a2_dependency_decision.file_sha256 -ne
            $ExpectedA2DecisionSha256) {
        throw "Physical B Primary F0 身份、授权或源哈希合同无效"
    }
    $f0Identity = Get-PublishedIdentity `
        $f0Path (Join-Path $resolvedPublishedRun "f0-primary.json") `
        "Physical B Primary F0"

    $request = $f0.primary_sequence
    $offlineDesign = Get-Content -LiteralPath $sourceDesign.path `
        -Raw -Encoding utf8 | ConvertFrom-Json
    $frozenOfflineSequence = $offlineDesign.selected_candidate.sequence
    if ([string]$frozenOfflineSequence.sequence_semantic_sha256 -ne
            [string]$request.offline_sequence_semantic_sha256) {
        throw "Physical B F0 与 offline design 的 exact sequence SHA 不一致"
    }
    $sequencePath = Join-Path $stagingDirectory "sequence.json"
    & (Join-Path $toolDirectory "XenMouseEffectProbeSequence.exe") `
        --output $sequencePath `
        --profile physical-b-primary `
        --guard-samples ([uint64]$request.guard_sample_count) `
        --lfsr-order ([uint64]$request.lfsr.order) `
        --feedback-mask ([uint64]$request.lfsr.feedback_mask) `
        --seed ([uint64]$request.lfsr.seed) `
        --phase ([uint64]$request.lfsr.phase) `
        --offline-sequence-semantic-sha256 `
            ([string]$request.offline_sequence_semantic_sha256)
    if ($LASTEXITCODE -ne 0) {
        throw "Physical B sequence tool 失败，ExitCode=$LASTEXITCODE"
    }
    $sequence = Get-Content -LiteralPath $sequencePath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $samples = @($sequence.samples)
    $blocks = @($sequence.blocks)
    $frozenSamples = @($frozenOfflineSequence.samples)
    $frozenBlocks = @($frozenOfflineSequence.blocks)
    $transitions = @($samples | Where-Object { [int]$_.dx_counts -ne 0 })
    $blockRoles = @($blocks | ForEach-Object { [string]$_.role })
    $blockPolarities = @($blocks | ForEach-Object { [string]$_.polarity })
    if ([int]$sequence.schema -ne 5 -or
        [string]$sequence.profile -ne "physical_b_prbs_primary" -or
        [string]$sequence.request.offline_sequence_semantic_sha256 -ne
            [string]$request.offline_sequence_semantic_sha256 -or
        $samples.Count -ne [uint64]$request.sample_count -or
        $blocks.Count -ne 6 -or
        [int64]$sequence.summary.net_x_counts -ne 0 -or
        [uint64]$sequence.summary.max_abs_prefix_x_counts -ne 1 -or
        ($blockRoles -join ",") -ne
            "estimation,estimation,selection,selection,confirmation,confirmation" -or
        ($blockPolarities -join ",") -ne
            "normal,inverted,normal,inverted,normal,inverted" -or
        @($samples | Where-Object {
            [int]$_.dx_counts -lt -1 -or [int]$_.dx_counts -gt 1 -or
            [int]$_.dy_counts -ne 0
        }).Count -ne 0) {
        throw "Physical B sequence 与 F0/exact block/X-only/net/prefix 合同不一致"
    }
    if ($frozenSamples.Count -ne $samples.Count -or
        $frozenBlocks.Count -ne $blocks.Count) {
        throw "Physical B C++ sequence 与 offline exact sequence 长度不一致"
    }
    for ($index = 0; $index -lt $samples.Count; ++$index) {
        $actual = $samples[$index]
        $expected = $frozenSamples[$index]
        if ([uint64]$actual.sample_index -ne [uint64]$expected.sample_index -or
            [uint64]$actual.block_id -ne [uint64]$expected.block_id -or
            [string]$actual.phase -ne [string]$expected.phase -or
            [int]$actual.dx_counts -ne [int]$expected.command_dx_counts -or
            [int]$actual.dy_counts -ne [int]$expected.command_dy_counts) {
            throw "Physical B C++ sequence 在 sample $index 偏离 offline exact sequence"
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
            throw "Physical B C++ sequence 在 block $index 偏离 offline exact sequence"
        }
    }
    $sequenceIdentity = Get-PublishedIdentity `
        $sequencePath (Join-Path $resolvedPublishedRun "sequence.json") `
        "Physical B Primary sequence"

    $runUuid = [guid]::NewGuid().ToString()
    $activationEpoch = [uint64][DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $bindingPath = Join-Path $stagingDirectory "probe-binding.json"
    $binding = [ordered]@{
        schema_version = 3
        evidence_type = "mouse_effect_probe_binding"
        experiment = "physical_b_primary_system_identification"
        run_uuid = $runUuid
        activation_epoch = $activationEpoch
        dispatch_mode = "physical_b"
        profile = "physical_b_prbs_primary"
        run_role = "primary"
        scope_id = [string]$f0.source_a2_dependency_decision.scope_id
        sequence_sha256 = [string]$sequence.sequence_sha256
        offline_sequence_semantic_sha256 =
            [string]$request.offline_sequence_semantic_sha256
        sequence_file = $sequenceIdentity
        primary_f0 = $f0Identity
        capture_source_name = $ndiSource
        config = $configCopy
        obs_source_binding = $obsCopy
        source_offline_design = $designCopy
        source_a2_dependency_decision = $decisionCopy
        sidecar_physical_output_capability = $false
        normal_aim_output_required = $false
        identification_input_definition = "cumulative_position_counts"
        actuator_audit_input = "completed_command_dx_counts"
        dy_counts_required = 0
        max_abs_sample_counts = 1
        max_abs_prefix_x_counts = 1
    }
    Write-NewUtf8Json $bindingPath $binding
    $bindingIdentity = Get-PublishedIdentity `
        $bindingPath (Join-Path $resolvedPublishedRun "probe-binding.json") `
        "Physical B probe binding"

    $taskPath = Join-Path $stagingDirectory "task.json"
    $task = [ordered]@{
        schema_version = 5
        evidence_type = "mouse_effect_probe_b_task"
        status = "PREPARED"
        experiment = "physical_b_primary_system_identification"
        run_role = "primary"
        run_directory = $resolvedPublishedRun
        run_uuid = $runUuid
        activation_epoch = $activationEpoch
        dispatch_mode = "physical_b"
        profile = "physical_b_prbs_primary"
        scope_id = [string]$f0.source_a2_dependency_decision.scope_id
        sequence_sha256 = [string]$sequence.sequence_sha256
        offline_sequence_semantic_sha256 =
            [string]$request.offline_sequence_semantic_sha256
        sequence_sample_count = [uint64]$samples.Count
        expected_nonzero_transition_count = [uint64]$transitions.Count
        max_abs_prefix_x_counts = [uint64]1
        physical_output_capability = $true
        physical_output_confirmation = $physicalConfirmation
        requires_user_frontend_launch = $true
        cross_run_holdout_prepare_authorized = $false
        files = [ordered]@{
            launch_script = $launchScript
            probe_executable = $probeExecutable
            sidecar_executable = $sidecarExecutable
            sequence_executable = $sequenceExecutable
            opencv_runtime = $opencvRuntime
            ndi_runtime = $ndiRuntime
            ndi_license = $ndiLicense
            prbs_designer = $designer
            physical_b_analyzer = $analyzer
            config = $configCopy
            sequence = $sequenceIdentity
            probe_binding = $bindingIdentity
            obs_source_binding = $obsCopy
            primary_f0 = $f0Identity
            offline_design = $designCopy
            a2_dependency_decision = $decisionCopy
        }
        f0 = [ordered]@{
            semantic_sha256 = [string]$f0.f0_semantic_sha256
            file_sha256 = [string]$f0Identity.sha256
            identification_input_definition =
                [string]$f0.model_contract.identification_input_definition
            actuator_audit_input =
                [string]$f0.model_contract.actuator_audit_input
            core_delay_samples = [uint64]$f0.model_contract.core_delay_samples
            tail_lengths = @($f0.model_contract.tail_lengths)
            nuisance_fit_rows = [string]$f0.model_contract.nuisance_fit_rows
            selection_used_for_single_refit =
                [bool]$f0.model_contract.selection_used_for_single_refit
            confirmation_used_for_refit =
                [bool]$f0.model_contract.confirmation_used_for_refit
            input_forced_validation_required = $true
            holdout_used_for_tuning = $false
            analysis_contract_semantic_sha256 =
                [string]$f0.analysis_contract.contract_semantic_sha256
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
            clock_sync_url = Get-RequiredValue `
                $capture "ndi_clock_sync_url"
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
            cross_run_holdout_prepare_authorized = $false
        }
    }
    Write-NewUtf8Json $taskPath $task

    $launchCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass ' +
        '-File "{0}" -RunDirectory "{1}" -AllowPhysicalOutput ' +
        '-PhysicalOutputConfirmation {2}') -f
        $launchScript.path, $resolvedPublishedRun, $physicalConfirmation
    $taskMarkdown = @(
        "# Mouse Effect Probe Physical B Primary",
        "",
        "- Run UUID：``$runUuid``；scope：``$($task.scope_id)``",
        "- 输入：累计位置用于辨识；完成的相对 X 命令用于 ACK/净零/前缀审计",
        "- 序列：800 samples；完整 estimation/selection/confirmation pairs；各 block pre/post guard 不共享；Y=0、净 X=0、最大前缀=1 count",
        "- F0 v2：delay=4 static-gain core；T={0,1,2,4,8} relative-command tail",
        "- 数据边界：nuisance 仅拟合独立 pre-guard；confirmation 不参与重拟合",
        "- 本 Run 不包含 cross-Run holdout；不得改 recurrence、phase、delay、T、幅度、重复数或 guard",
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
        "# Physical B Primary 人工观察",
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
        run_role = "primary"
        profile = "physical_b_prbs_primary"
        scope_id = [string]$task.scope_id
        local_bundle_directory = $resolvedRun
        published_run_directory = $resolvedPublishedRun
        f0_semantic_sha256 = [string]$f0.f0_semantic_sha256
        sequence_sha256 = [string]$sequence.sequence_sha256
        sequence_sample_count = [uint64]$samples.Count
        expected_nonzero_transition_count = [uint64]$transitions.Count
        cross_run_holdout_prepare_authorized = $false
        physical_launch_executed = $false
    }
    Write-NewUtf8Json `
        (Join-Path $stagingDirectory "prepare-summary.json") $summary

    Move-Item -LiteralPath $stagingDirectory -Destination $resolvedRun
    Write-Host "Physical B Primary Prepare 完成；未启动 Mouse、sidecar 或 Launch。"
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
            throw "Physical B staging 清理目标越界：$stagingFull"
        }
        Remove-Item -LiteralPath $stagingFull -Recurse -Force
    }
    throw $failure
}
