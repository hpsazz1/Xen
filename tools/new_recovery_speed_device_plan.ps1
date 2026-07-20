[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Config,
    [Parameter(Mandatory = $true)][string]$DeviceId,
    [Parameter(Mandatory = $true)][string]$NdiSource,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('DML', 'CUDA')][string]$BuildBackend,
    [Parameter(Mandatory = $true)][string]$BuildRevision,
    [Parameter(Mandatory = $true)][ValidateRange(1, 10000)][int]$ControllerRevision,
    [Parameter(Mandatory = $true)][ValidateRange(0, 10000)][int]$RoiX,
    [Parameter(Mandatory = $true)][ValidateRange(0, 10000)][int]$RoiY,
    [Parameter(Mandatory = $true)][ValidateRange(8, 10000)][int]$RoiWidth,
    [Parameter(Mandatory = $true)][ValidateRange(8, 10000)][int]$RoiHeight
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

if ($DeviceId.Trim().Length -lt 4) { throw 'DeviceId must be an explicit stable device identity.' }
if ($NdiSource -eq 'Auto' -or $NdiSource -eq 'None' -or [string]::IsNullOrWhiteSpace($NdiSource)) {
    throw 'NdiSource must be an exact source name.'
}
if ($BuildRevision -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'BuildRevision must be a clean 40-character Git commit without -dirty.'
}

$configPath = [IO.Path]::GetFullPath($Config)
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    throw "Config file not found: $configPath"
}
if (Test-Path -LiteralPath $outputPath) {
    throw "OutputDirectory must not already exist: $outputPath"
}
$configValues = Read-IniValues $configPath
foreach ($key in @('input_method', 'kmbox_net_uuid', 'ndi_source_name')) {
    if (-not $configValues.ContainsKey($key)) { throw "Config misses required protocol key: $key" }
}
$expectedDeviceId = "KMBOX_NET:$($configValues.kmbox_net_uuid)"
if ($configValues.input_method -ne 'KMBOX_NET' -or $DeviceId -cne $expectedDeviceId) {
    throw "Protocol v1 only accepts the exact KMBOX_NET identity from config: $expectedDeviceId"
}
if ($NdiSource -cne $configValues.ndi_source_name) {
    throw "NdiSource does not match config ndi_source_name: $($configValues.ndi_source_name)"
}

$protocolVersion = 'xen-recovery-speed-device-v1'
# 240 Hz下8帧恰好使1440/1800分别形成48/60 counts；1800用7/8交替避免伪装成孤立大脉冲。
$frameRateHz = 240
$pulseFrames = 8
$baselineUs = 500000L
$stopObservationUs = 1000000L
$tailObservationUs = 1000000L
$interTrialUs = 1000000L
$trialDurationUs = $baselineUs + [long][math]::Round(2.0 * $pulseFrames * 1000000.0 / $frameRateHz) +
    $stopObservationUs + $tailObservationUs
$planId = [guid]::NewGuid().ToString('N')

function Get-PulseCounts {
    param([int]$Rate)
    if ($Rate -eq 1440) { return @(6, 6, 6, 6, 6, 6, 6, 6) }
    if ($Rate -eq 1800) { return @(7, 8, 7, 8, 7, 8, 7, 8) }
    throw "Unsupported protocol rate: $Rate"
}

$rows = [Collections.Generic.List[object]]::new()
$trial = 0
$trialStartUs = 0L
foreach ($rate in @(1440, 1800)) {
    foreach ($leadingDirection in @(1, -1)) {
        $trial++
        $pulse = @(Get-PulseCounts $rate)
        $maximumExcursion = [int](($pulse | Measure-Object -Sum).Sum)
        $cumulative = 0
        $absoluteCounts = 0
        $command = 0
        for ($frame = 0; $frame -lt $pulseFrames; $frame++) {
            $command++
            $delta = $leadingDirection * $pulse[$frame]
            $cumulative += $delta
            $absoluteCounts += [math]::Abs($delta)
            $relativeUs = $baselineUs + [long][math]::Round($frame * 1000000.0 / $frameRateHz)
            $rows.Add([pscustomobject][ordered]@{
                ProtocolVersion = $protocolVersion; PlanId = $planId; Trial = $trial
                RateCountsPerSecond = $rate; LeadingDirection = $leadingDirection
                Segment = 'forward'; Command = $command; FrameIndex = $frame
                TrialStartOffsetUs = $trialStartUs; RelativeOffsetUs = $relativeUs
                ScheduledOffsetUs = $trialStartUs + $relativeUs
                DeltaX = $delta; DeltaY = 0; CumulativeX = $cumulative
                TrialAbsoluteCounts = $absoluteCounts; MaximumExcursionCounts = $maximumExcursion
            })
        }
        # 首段后保留完整静默观测，再按逆序取反回零，确保离原点峰值和净位移都有硬上界。
        $returnStartUs = $baselineUs + [long][math]::Round($pulseFrames * 1000000.0 / $frameRateHz) +
            $stopObservationUs
        [array]::Reverse($pulse)
        for ($frame = 0; $frame -lt $pulseFrames; $frame++) {
            $command++
            $delta = -$leadingDirection * $pulse[$frame]
            $cumulative += $delta
            $absoluteCounts += [math]::Abs($delta)
            $relativeUs = $returnStartUs + [long][math]::Round($frame * 1000000.0 / $frameRateHz)
            $rows.Add([pscustomobject][ordered]@{
                ProtocolVersion = $protocolVersion; PlanId = $planId; Trial = $trial
                RateCountsPerSecond = $rate; LeadingDirection = $leadingDirection
                Segment = 'return'; Command = $command; FrameIndex = $frame
                TrialStartOffsetUs = $trialStartUs; RelativeOffsetUs = $relativeUs
                ScheduledOffsetUs = $trialStartUs + $relativeUs
                DeltaX = $delta; DeltaY = 0; CumulativeX = $cumulative
                TrialAbsoluteCounts = $absoluteCounts; MaximumExcursionCounts = $maximumExcursion
            })
        }
        if ($cumulative -ne 0) { throw "Internal protocol error: trial $trial does not return to zero." }
        $trialStartUs += $trialDurationUs + $interTrialUs
    }
}

New-Item -ItemType Directory -Path $outputPath | Out-Null
$planPath = Join-Path $outputPath 'recovery_speed_device_plan.csv'
$manifestPath = Join-Path $outputPath 'recovery_speed_device_manifest.txt'
$decisionPath = Join-Path $outputPath 'recovery_speed_device_plan_decision.txt'
$rows | Export-Csv -LiteralPath $planPath -NoTypeInformation -Encoding UTF8
$planHash = (Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash
$configHash = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash

@(
    "ProtocolVersion=$protocolVersion"
    "PlanId=$planId"
    "BuildBackend=$BuildBackend"
    "BuildRevision=$($BuildRevision.ToLowerInvariant())"
    "ControllerRevision=$ControllerRevision"
    "DeviceId=$DeviceId"
    "ConfigPath=$configPath"
    "ConfigSha256=$configHash"
    "NdiSource=$NdiSource"
    "Roi=$RoiX,$RoiY,$RoiWidth,$RoiHeight"
    "FrameRateHz=$frameRateHz"
    "RatesCountsPerSecond=1440,1800"
    "PulseFrames=$pulseFrames"
    "MaximumExcursionCounts=60"
    "MaximumAbsoluteCountsPerTrial=120"
    "BaselineMs=$($baselineUs / 1000)"
    "StopObservationMs=$($stopObservationUs / 1000)"
    "TailObservationMs=$($tailObservationUs / 1000)"
    "InterTrialMs=$($interTrialUs / 1000)"
    "PlanSha256=$planHash"
    'ExecutionEnabled=0'
    'PhysicalExecutionAuthorized=0'
    'ZeroTargetRiskConfirmed=0'
    'AutomaticConfigurationWrite=0'
    'AutomaticActiveEnable=0'
) | Set-Content -LiteralPath $manifestPath -Encoding UTF8

# 生成即审计，但审计通过仍只表示计划可供人工复核，不代表设备执行授权。
& (Join-Path $PSScriptRoot 'test_recovery_speed_device_plan.ps1') -PlanDirectory $outputPath -DecisionPath $decisionPath -RequirePass
Write-Host "Recovery speed device plan written to $outputPath"
