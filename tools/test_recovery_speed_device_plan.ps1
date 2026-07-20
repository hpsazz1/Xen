[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PlanDirectory,
    [string]$DecisionPath = '',
    [switch]$RequirePass
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-KeyValueFile {
    param([string]$Path)
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line -split '=', 2
        if ($parts.Count -ne 2 -or $values.ContainsKey($parts[0])) {
            throw "Invalid or duplicate manifest entry: $line"
        }
        $values[$parts[0]] = $parts[1]
    }
    return $values
}

function Add-Issue {
    param([string]$Message)
    $script:issues.Add($Message)
}

function Read-IniValues {
    param([string]$Path)
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path -Encoding UTF8) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith('#') -or $trimmed.StartsWith(';') -or
            $trimmed.StartsWith('[')) { continue }
        $parts = $trimmed -split '=', 2
        if ($parts.Count -eq 2) { $values[$parts[0].Trim().ToLowerInvariant()] = $parts[1].Trim() }
    }
    return $values
}

$root = [IO.Path]::GetFullPath($PlanDirectory)
$planPath = Join-Path $root 'recovery_speed_device_plan.csv'
$manifestPath = Join-Path $root 'recovery_speed_device_manifest.txt'
if (-not (Test-Path -LiteralPath $planPath -PathType Leaf)) { throw "Plan CSV not found: $planPath" }
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Manifest not found: $manifestPath" }
if (-not $DecisionPath) { $DecisionPath = Join-Path $root 'recovery_speed_device_plan_decision.txt' }
$resolvedDecision = [IO.Path]::GetFullPath($DecisionPath)

$issues = [Collections.Generic.List[string]]::new()
$manifest = Read-KeyValueFile $manifestPath
$requiredManifest = @(
    'ProtocolVersion', 'PlanId', 'BuildBackend', 'BuildRevision', 'ControllerRevision',
    'DeviceId', 'ConfigPath', 'ConfigSha256', 'NdiSource', 'Roi', 'FrameRateHz',
    'RatesCountsPerSecond', 'PulseFrames', 'MaximumExcursionCounts',
    'MaximumAbsoluteCountsPerTrial', 'BaselineMs', 'StopObservationMs',
    'TailObservationMs', 'InterTrialMs', 'PlanSha256', 'ExecutionEnabled',
    'PhysicalExecutionAuthorized', 'ZeroTargetRiskConfirmed',
    'AutomaticConfigurationWrite', 'AutomaticActiveEnable'
)
foreach ($name in $requiredManifest) {
    if (-not $manifest.ContainsKey($name)) { Add-Issue "missing-manifest:$name" }
}

if ($issues.Count -eq 0) {
    if ($manifest.ProtocolVersion -ne 'xen-recovery-speed-device-v1') { Add-Issue 'protocol-version' }
    if ($manifest.BuildBackend -notin @('DML', 'CUDA')) { Add-Issue 'build-backend' }
    if ($manifest.BuildRevision -notmatch '^[0-9a-f]{40}$') { Add-Issue 'build-revision' }
    if ($manifest.DeviceId.Trim().Length -lt 4) { Add-Issue 'device-id' }
    if ($manifest.NdiSource -in @('', 'Auto', 'None')) { Add-Issue 'ndi-source' }
    if ($manifest.FrameRateHz -ne '240' -or $manifest.RatesCountsPerSecond -ne '1440,1800' -or
        $manifest.PulseFrames -ne '8' -or $manifest.MaximumExcursionCounts -ne '60' -or
        $manifest.MaximumAbsoluteCountsPerTrial -ne '120') { Add-Issue 'fixed-envelope' }
    if ($manifest.BaselineMs -ne '500' -or $manifest.StopObservationMs -ne '1000' -or
        $manifest.TailObservationMs -ne '1000' -or $manifest.InterTrialMs -ne '1000') {
        Add-Issue 'observation-envelope'
    }
    foreach ($name in @('ExecutionEnabled', 'PhysicalExecutionAuthorized', 'ZeroTargetRiskConfirmed',
            'AutomaticConfigurationWrite', 'AutomaticActiveEnable')) {
        if ($manifest[$name] -ne '0') { Add-Issue "unsafe-flag:$name" }
    }
    if (-not (Test-Path -LiteralPath $manifest.ConfigPath -PathType Leaf)) {
        Add-Issue 'config-missing'
    } elseif ((Get-FileHash -LiteralPath $manifest.ConfigPath -Algorithm SHA256).Hash -ne $manifest.ConfigSha256) {
        Add-Issue 'config-hash'
    } else {
        $configValues = Read-IniValues $manifest.ConfigPath
        if (-not $configValues.ContainsKey('input_method') -or
            -not $configValues.ContainsKey('kmbox_net_uuid') -or
            $configValues.input_method -ne 'KMBOX_NET' -or
            $manifest.DeviceId -cne "KMBOX_NET:$($configValues.kmbox_net_uuid)") {
            Add-Issue 'config-device-identity'
        }
        if (-not $configValues.ContainsKey('ndi_source_name') -or
            $manifest.NdiSource -cne $configValues.ndi_source_name) {
            Add-Issue 'config-ndi-source'
        }
    }
    if ((Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash -ne $manifest.PlanSha256) {
        Add-Issue 'plan-hash'
    }
}

$rows = @(Import-Csv -LiteralPath $planPath)
$requiredColumns = @('ProtocolVersion', 'PlanId', 'Trial', 'RateCountsPerSecond',
    'LeadingDirection', 'Segment', 'Command', 'FrameIndex', 'TrialStartOffsetUs',
    'RelativeOffsetUs', 'ScheduledOffsetUs', 'DeltaX', 'DeltaY', 'CumulativeX',
    'TrialAbsoluteCounts', 'MaximumExcursionCounts')
if ($rows.Count -ne 64) { Add-Issue "row-count:$($rows.Count)" }
if ($rows.Count -gt 0) {
    foreach ($column in $requiredColumns) {
        if ($rows[0].PSObject.Properties.Name -notcontains $column) { Add-Issue "missing-column:$column" }
    }
}

if ($issues.Count -eq 0) {
    # 独立重构固定协议，禁止只信任CSV中的自报累计值或最大位移。
    $expectedTrial = 0
    $trialStartUs = 0L
    $trialDurationUs = 2566667L
    foreach ($rate in @(1440, 1800)) {
        foreach ($leadingDirection in @(1, -1)) {
            $expectedTrial++
            $pulse = if ($rate -eq 1440) { @(6, 6, 6, 6, 6, 6, 6, 6) } else { @(7, 8, 7, 8, 7, 8, 7, 8) }
            $maximumExcursion = [int](($pulse | Measure-Object -Sum).Sum)
            $cumulative = 0
            $absoluteCounts = 0
            $expected = [Collections.Generic.List[object]]::new()
            for ($frame = 0; $frame -lt 8; $frame++) {
                $delta = $leadingDirection * $pulse[$frame]
                $cumulative += $delta
                $absoluteCounts += [math]::Abs($delta)
                $expected.Add([pscustomobject]@{
                    Segment = 'forward'
                    FrameIndex = $frame
                    RelativeOffsetUs = 500000L + [long][math]::Round($frame * 1000000.0 / 240)
                    DeltaX = $delta
                    CumulativeX = $cumulative
                    TrialAbsoluteCounts = $absoluteCounts
                })
            }
            [array]::Reverse($pulse)
            for ($frame = 0; $frame -lt 8; $frame++) {
                $delta = -$leadingDirection * $pulse[$frame]
                $cumulative += $delta
                $absoluteCounts += [math]::Abs($delta)
                $expected.Add([pscustomobject]@{
                    Segment = 'return'
                    FrameIndex = $frame
                    RelativeOffsetUs = 1533333L + [long][math]::Round($frame * 1000000.0 / 240)
                    DeltaX = $delta
                    CumulativeX = $cumulative
                    TrialAbsoluteCounts = $absoluteCounts
                })
            }
            $actual = @($rows | Where-Object { [int]$_.Trial -eq $expectedTrial })
            if ($actual.Count -ne 16) { Add-Issue "trial-row-count:$expectedTrial"; continue }
            for ($index = 0; $index -lt 16; $index++) {
                $row = $actual[$index]
                $e = $expected[$index]
                $valid = $row.ProtocolVersion -eq $manifest.ProtocolVersion -and
                    $row.PlanId -eq $manifest.PlanId -and [int]$row.RateCountsPerSecond -eq $rate -and
                    [int]$row.LeadingDirection -eq $leadingDirection -and $row.Segment -eq $e.Segment -and
                    [int]$row.Command -eq ($index + 1) -and [int]$row.FrameIndex -eq $e.FrameIndex -and
                    [long]$row.TrialStartOffsetUs -eq $trialStartUs -and
                    [long]$row.RelativeOffsetUs -eq $e.RelativeOffsetUs -and
                    [long]$row.ScheduledOffsetUs -eq ($trialStartUs + $e.RelativeOffsetUs) -and
                    [int]$row.DeltaX -eq $e.DeltaX -and [int]$row.DeltaY -eq 0 -and
                    [int]$row.CumulativeX -eq $e.CumulativeX -and
                    [int]$row.TrialAbsoluteCounts -eq $e.TrialAbsoluteCounts -and
                    [int]$row.MaximumExcursionCounts -eq $maximumExcursion
                if (-not $valid) { Add-Issue "schedule:$expectedTrial/$($index + 1)" }
            }
            if ($cumulative -ne 0 -or $absoluteCounts -gt 120 -or $maximumExcursion -gt 60) {
                Add-Issue "envelope:$expectedTrial"
            }
            $trialStartUs += $trialDurationUs + 1000000L
        }
    }
}

$passed = $issues.Count -eq 0
# 合格计划仍锁定执行；任何异常只允许降级，不存在自动修复或自动启用分支。
$recommendation = if ($passed) { 'MANUAL_REVIEW_ONLY' } else { 'HOLD_DIAGNOSTIC' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedDecision) | Out-Null
@(
    "PlanValidationPassed=$([int]$passed)"
    "Recommendation=$recommendation"
    'ExecutionEnabled=0'
    'PhysicalExecutionAuthorized=0'
    'AutomaticConfigurationWrite=0'
    'AutomaticActiveEnable=0'
    "Issues=$($issues -join '|')"
) | Set-Content -LiteralPath $resolvedDecision -Encoding UTF8

$result = [pscustomobject]@{
    PlanValidationPassed = $passed
    Recommendation = $recommendation
    Rows = $rows.Count
    Issues = $issues -join '|'
}
$result | Format-List | Out-Host
if ($RequirePass -and -not $passed) { throw "Recovery speed device plan failed: $($result.Issues)" }
$result
