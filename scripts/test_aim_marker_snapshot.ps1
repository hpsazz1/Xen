param(
    [string]$SourcePath = (Join-Path $PSScriptRoot 'invoke_aim_manual_acceptance.ps1'),
    [string]$TestRoot = (Join-Path $PSScriptRoot '..\cache\aim-marker-snapshot-test')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 提取实际生产首启块与函数。绝不执行 Launcher 或 sidecar；唯一进程 seam
# 是计数替身，故障仅作用于本次 GUID 目录中的 marker 文件。
$sourceTokens = $null
$sourceErrors = $null
$sourceAst = [System.Management.Automation.Language.Parser]::ParseFile(
    [System.IO.Path]::GetFullPath($SourcePath),
    [ref]$sourceTokens, [ref]$sourceErrors)
if ($sourceErrors.Count -ne 0) { throw '生产脚本存在语法错误。' }
$functionNames = @(
    'Get-FileEvidence', 'Read-RuntimeAlignmentMarker',
    'Read-RuntimeAlignmentMarkerSnapshot',
    'Get-RuntimeAlignmentMarkerProbe',
    'Get-RuntimeAlignmentMarkerProbeWithRetry',
    'Test-RuntimeAlignmentMarkerActive')
$snapshotAvailable = $false
foreach ($functionName in $functionNames) {
    $functionNode = $sourceAst.Find({
        param($candidate)
        $candidate -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $candidate.Name -eq $functionName
    }, $true)
    if ($null -eq $functionNode) { continue }
    $definition = $functionNode.Extent.Text
    if ($functionName -eq 'Read-RuntimeAlignmentMarkerSnapshot') {
        $snapshotAvailable = $true
        $definition = $definition.Replace(
            'function Read-RuntimeAlignmentMarkerSnapshot(',
            'function Read-RuntimeAlignmentMarkerSnapshotActual(')
    } elseif ($functionName -eq 'Get-FileEvidence') {
        $definition = $definition.Replace(
            'function Get-FileEvidence(', 'function Get-FileEvidenceActual(')
    }
    . ([scriptblock]::Create($definition))
}
$startupNodes = @($sourceAst.FindAll({
    param($candidate)
    $candidate -is [System.Management.Automation.Language.IfStatementAst] -and
        $candidate.Clauses[0].Item1.Extent.Text.Contains('$pixelEvidenceRuntimeMarkerPath') -and
        $candidate.Clauses[0].Item1.Extent.Text.Contains('$taskCapturePixelEvidence') -and
        $candidate.Extent.Text.Contains('Read-RuntimeAlignmentMarker')
}, $true))
if ($startupNodes.Count -ne 1) { throw '没有唯一的生产 sidecar 首启块。' }
$startupBlock = [scriptblock]::Create($startupNodes[0].Extent.Text)
# 即使未来生产块改变，也不能由测试误启真实进程。
function Start-Process { throw '测试禁止调用真实 Start-Process。' }

$resolvedTestBase = [System.IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
$ownedRoot = Join-Path $resolvedTestBase ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $ownedRoot -Force | Out-Null
$script:caseNumber = 0
$script:startedCount = 0
$script:releasePromptCount = 0
$script:markerPath = ''
$script:afterSnapshotMutation = ''
$script:evidenceDeletePending = $false
$script:fileEvidenceCalls = 0
$script:passedCount = 0

function New-MarkerBytes(
        [string]$SessionId = 'fixture-session',
        [int]$Epoch = 1,
        [int]$Sequence = 101) {
    # 保留非规范空白，防止实现解析/重新序列化后才计算 SHA256。
    $content = "{  `n  `"schema`": 2, `"session_id`": `"$SessionId`", " +
        "`"gate`": `"AIM_LOCK_ACTIVE`", `"activation_epoch`": $Epoch, " +
        "`"sequence`": $Sequence`n}`n"
    return ,([System.Text.Encoding]::UTF8.GetBytes($content))
}

function Get-BytesHash([byte[]]$Bytes) {
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($hasher.ComputeHash($Bytes))).Replace('-', '')
    } finally { $hasher.Dispose() }
}

function Get-FileEvidence([string]$Path) {
    ++$script:fileEvidenceCalls
    if ($script:evidenceDeletePending) {
        $script:evidenceDeletePending = $false
        Remove-Item -LiteralPath $Path -Force
    }
    return Get-FileEvidenceActual $Path
}

function Read-RuntimeAlignmentMarkerSnapshot([string]$Path) {
    $snapshot = Read-RuntimeAlignmentMarkerSnapshotActual $Path
    if ($null -ne $snapshot) {
        switch ($script:afterSnapshotMutation) {
            'DELETE' { Remove-Item -LiteralPath $Path -Force }
            'EPOCH' {
                [System.IO.File]::WriteAllBytes($Path, (New-MarkerBytes -Epoch 2 -Sequence 202))
            }
            'REGRESS' {
                [System.IO.File]::WriteAllBytes($Path, (New-MarkerBytes -Sequence 100))
            }
        }
        $script:afterSnapshotMutation = ''
    }
    return $snapshot
}

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-NoBinding([object]$State, [string]$Context) {
    Assert-Condition (-not $State.blocked -and $State.started -eq 0 -and
        [string]::IsNullOrWhiteSpace($State.path) -and
        [string]::IsNullOrWhiteSpace($State.alignment.session_id) -and
        $null -eq $State.alignment.marker -and
        [string]::IsNullOrWhiteSpace($State.error) -and $State.release_prompts -eq 0) "$Context 必须保持尚未绑定且可继续发现。"
}

function Invoke-StartupCase(
        [string]$Name,
        [byte[]]$InitialBytes = (New-MarkerBytes),
        [string]$Mutation = '',
        [switch]$DeleteDuringEvidence,
        [switch]$InitiallyMissing,
        [switch]$InitiallyLocked,
        [scriptblock]$BetweenTicks = {},
        [switch]$TwoTicks) {
    ++$script:caseNumber
    $runtimeRoot = Join-Path $ownedRoot ('{0:D2}-{1}' -f $script:caseNumber, $Name)
    New-Item -ItemType Directory -Path $runtimeRoot | Out-Null
    $script:markerPath = Join-Path $runtimeRoot 'fixture-session.json.aim-lock-active'
    if (-not $InitiallyMissing) {
        [System.IO.File]::WriteAllBytes($script:markerPath, $InitialBytes)
    }
    $script:startedCount = 0
    $script:releasePromptCount = 0
    $script:afterSnapshotMutation = $Mutation
    $script:evidenceDeletePending = $DeleteDuringEvidence.IsPresent
    $script:fileEvidenceCalls = 0
    $taskCapturePixelEvidence = $true
    $taskHasPixelEvidenceRuntimeAlignmentContract = $true
    $pixelEvidenceRuntimeAlignmentBlocked = $false
    $pixelEvidenceRuntimeMarkerPath = ''
    $pixelEvidenceRuntimeGate = 'AIM_LOCK_ACTIVE'
    $pixelEvidenceRuntimeMarkerSuffix = '.aim-lock-active'
    $pixelEvidenceRuntimeMarkerSchema = 2
    $pixelEvidenceRuntimeMarkerMaxAgeMs = 1000
    $pixelEvidenceRuntimeAlignment = [ordered]@{
        required = $true; gate = $pixelEvidenceRuntimeGate; gate_passed = $false
        session_id = ''; activation_epoch = 0; marker = $null
    }
    $pixelEvidenceExecutionError = ''
    $pixelEvidenceAttemptState = $null
    $pixelEvidenceProcess = $null
    $before = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $process = [pscustomobject]@{ HasExited = $false }
    $process | Add-Member ScriptMethod WaitForExit {
        param([int]$Milliseconds)
        return $false
    }
    $startPixelEvidenceAttempt = {
        param([int]$Attempt)
        ++$script:startedCount
        [pscustomobject]@{ process = $process; attempt = $Attempt }
    }
    $writePixelEvidenceReleasePrompt = {
        param([bool]$Success)
        ++$script:releasePromptCount
    }
    $firstState = $null
    $lockStream = $null
    try {
        if ($InitiallyLocked) {
            $lockStream = [System.IO.FileStream]::new(
                $script:markerPath, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        }
        $tickCount = if ($TwoTicks) { 2 } else { 1 }
        for ($tick = 1; $tick -le $tickCount; ++$tick) {
            if ($tick -eq 2) {
                if ($null -ne $lockStream) { $lockStream.Dispose(); $lockStream = $null }
                & $BetweenTicks $firstState
            }
            # 生产 continue 必须只结束当前轮询，不跳过测试断言。
            foreach ($singleTick in @(1)) { . $startupBlock }
            $state = [pscustomobject]@{
                blocked = $pixelEvidenceRuntimeAlignmentBlocked
                started = $script:startedCount
                path = $pixelEvidenceRuntimeMarkerPath
                alignment = [pscustomobject]@{
                    session_id = [string]$pixelEvidenceRuntimeAlignment.session_id
                    activation_epoch = [uint64]$pixelEvidenceRuntimeAlignment.activation_epoch
                    marker = $pixelEvidenceRuntimeAlignment.marker
                }
                error = $pixelEvidenceExecutionError
                evidence_calls = $script:fileEvidenceCalls
                release_prompts = $script:releasePromptCount
            }
            if ($tick -eq 1) { $firstState = $state }
        }
        return [pscustomobject]@{ first = $firstState; final = $state }
    } finally {
        if ($null -ne $lockStream) { $lockStream.Dispose() }
    }
}

function Assert-StartedSnapshot(
        [object]$State, [byte[]]$ExpectedBytes, [int]$ExpectedEpoch = 1) {
    Assert-Condition (-not $State.blocked -and $State.started -eq 1 -and
        $State.release_prompts -eq 0) (
        '完整初始 snapshot 必须启动一次。实际：' + ($State | ConvertTo-Json -Depth 6 -Compress))
    Assert-Condition ($State.alignment.session_id -eq 'fixture-session' -and
        $State.alignment.activation_epoch -eq $ExpectedEpoch) 'binding 身份必须来自当前完整 snapshot。'
    Assert-Condition ($null -ne $State.alignment.marker -and
        [long]$State.alignment.marker.length -eq $ExpectedBytes.Length -and
        [string]$State.alignment.marker.sha256 -eq (Get-BytesHash $ExpectedBytes)) 'marker length/SHA256 必须来自同一次读取的原始 bytes。'
    Assert-Condition ($State.evidence_calls -eq 0) '动态 marker 不得另行调用 Get-FileEvidence。'
    ++$script:passedCount
}

try {
    # 在旧生产实现上先实际经过 Read→Active→Get-FileEvidence 并复现
    # binding 半提交，不以缺少新 helper 作为唯一 red。
    $baselineBytes = New-MarkerBytes
    $race = Invoke-StartupCase 'evidence-race' -InitialBytes $baselineBytes -DeleteDuringEvidence
    Assert-StartedSnapshot $race.final $baselineBytes
    Assert-Condition $snapshotAvailable '需要实际 snapshot API。'

    $missing = Invoke-StartupCase 'missing' -InitiallyMissing -TwoTicks -BetweenTicks {
        param($state)
        Assert-NoBinding $state '初始文件缺席'
        [System.IO.File]::WriteAllBytes($script:markerPath, (New-MarkerBytes))
    }
    Assert-StartedSnapshot $missing.final $baselineBytes

    $locked = Invoke-StartupCase 'locked' -InitiallyLocked -TwoTicks -BetweenTicks {
        param($state)
        Assert-NoBinding $state '初始文件读冲突'
    }
    Assert-StartedSnapshot $locked.final $baselineBytes

    $deleted = Invoke-StartupCase 'deleted-after-snapshot' -Mutation DELETE -TwoTicks -BetweenTicks {
        param($state)
        Assert-NoBinding $state 'snapshot 后删除'
        [System.IO.File]::WriteAllBytes($script:markerPath, (New-MarkerBytes))
    }
    Assert-StartedSnapshot $deleted.final $baselineBytes

    $epoch = Invoke-StartupCase 'epoch-after-snapshot' -Mutation EPOCH -TwoTicks -BetweenTicks {
        param($state)
        Assert-NoBinding $state 'snapshot/probe epoch 不一致'
    }
    Assert-StartedSnapshot $epoch.final (New-MarkerBytes -Epoch 2 -Sequence 202) 2

    $regressed = Invoke-StartupCase 'sequence-regressed' -Mutation REGRESS
    Assert-Condition ($regressed.final.blocked -and $regressed.final.started -eq 0 -and
        $null -eq $regressed.final.alignment.marker) 'sequence 回退必须拒绝，且不能提交 marker binding。'
    ++$script:passedCount

    foreach ($invalid in @(
        [pscustomobject]@{ name = 'bad-json'; content = '{' },
        [pscustomobject]@{ name = 'bad-schema'; content = '{"schema":1,"session_id":"fixture-session","gate":"AIM_LOCK_ACTIVE","activation_epoch":1,"sequence":101}' },
        [pscustomobject]@{ name = 'missing-sequence'; content = '{"schema":2,"session_id":"fixture-session","gate":"AIM_LOCK_ACTIVE","activation_epoch":1}' })) {
        $invalidResult = Invoke-StartupCase $invalid.name -InitialBytes (
            [System.Text.Encoding]::UTF8.GetBytes($invalid.content))
        Assert-Condition ($invalidResult.final.blocked -and $invalidResult.final.started -eq 0 -and
            $null -eq $invalidResult.final.alignment.marker) "$($invalid.name) 必须拒绝且不能启动。"
        ++$script:passedCount
    }

    # 再直接检验 snapshot 证据不会因路径后续变更而变更。
    $snapshotPath = Join-Path $ownedRoot 'snapshot-bytes.json'
    [System.IO.File]::WriteAllBytes($snapshotPath, $baselineBytes)
    $pixelEvidenceRuntimeMarkerSchema = 2
    $pixelEvidenceRuntimeGate = 'AIM_LOCK_ACTIVE'
    $snapshot = Read-RuntimeAlignmentMarkerSnapshotActual $snapshotPath
    [System.IO.File]::WriteAllBytes($snapshotPath, (New-MarkerBytes -Epoch 2 -Sequence 202))
    Assert-Condition ([long]$snapshot.evidence.length -eq $baselineBytes.Length -and
        [string]$snapshot.evidence.sha256 -eq (Get-BytesHash $baselineBytes) -and
        [uint64]$snapshot.marker.activation_epoch -eq 1 -and
        [uint64]$snapshot.marker.sequence -eq 101) 'snapshot 解析和文件证据必须绑定同一 bytes，不能随后重读路径。'
    ++$script:passedCount
    Write-Host "Aim marker snapshot 专项通过：$script:passedCount 个用例；physical_output_capability=false。"
} finally {
    # 只清理本脚本新建的 GUID 目录；解析绝对范围并拒绝 reparse point。
    $cleanupPath = [System.IO.Path]::GetFullPath($ownedRoot)
    $expectedPrefix = $resolvedTestBase + [System.IO.Path]::DirectorySeparatorChar
    if (-not $cleanupPath.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $cleanupPath) -notmatch '^[0-9a-f]{32}$') {
        throw "测试清理路径越界：$cleanupPath"
    }
    if (Test-Path -LiteralPath $cleanupPath) {
        $cleanupItems = @(Get-Item -LiteralPath $cleanupPath) +
            @(Get-ChildItem -LiteralPath $cleanupPath -Recurse -Force)
        if (@($cleanupItems | Where-Object {
                ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
            }).Count -ne 0) { throw '测试目录出现 reparse point，拒绝递归清理。' }
        Remove-Item -LiteralPath $cleanupPath -Recurse -Force
    }
}
