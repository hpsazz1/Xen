[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DataRoot,
    [string]$OutputCsv = '',
    [string]$DecisionPath = '',
    [int[]]$RequiredCounts = @(16, 32, 64),
    [ValidateRange(2, 20)][int]$MinimumRuns = 3,
    [ValidateRange(2, 100)][int]$MinimumTrialsPerDirection = 5,
    [double]$MinimumPixelsPerCount = 0.25,
    [double]$MaximumPixelsPerCount = 0.75,
    [double]$MaximumDirectionAsymmetryPercent = 15.0,
    [double]$MaximumCrossRunSpreadPercent = 15.0,
    [double]$MaximumLinearitySpreadPercent = 15.0,
    [double]$MaximumTimingSpreadMs = 5.0,
    [double]$MaximumT90Ms = 100.0,
    [double]$MaximumCrossAxisLeakagePercent = 10.0,
    [switch]$RequirePass
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $parsed = 0.0
    if (-not [double]::TryParse([string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed) -or [double]::IsNaN($parsed) -or
        [double]::IsInfinity($parsed)) {
        throw "Column '$Name' contains a non-finite value: $Value"
    }
    return $parsed
}

function Get-Percentile {
    param([double[]]$Values, [double]$Ratio)
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $index = [math]::Floor([math]::Max(0.0,
        [math]::Min(1.0, $Ratio)) * ($ordered.Count - 1))
    return [double]$ordered[$index]
}

function Get-RelativeSpreadPercent {
    param([double[]]$Values)
    if ($Values.Count -lt 2) { return 0.0 }
    $center = Get-Percentile $Values 0.5
    if ([math]::Abs($center) -le 1e-9) { return [double]::PositiveInfinity }
    return 100.0 * (($Values | Measure-Object -Maximum).Maximum -
        ($Values | Measure-Object -Minimum).Minimum) / [math]::Abs($center)
}

$root = [IO.Path]::GetFullPath($DataRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Active profile data root does not exist: $root"
}
$files = @(Get-ChildItem -LiteralPath $root -Filter 'probe_summary.csv' -File -Recurse)
if ($files.Count -eq 0) { throw "No probe_summary.csv files found under: $root" }
if ($RequiredCounts.Count -eq 0 -or
    @($RequiredCounts | Where-Object { $_ -le 0 } | Select-Object -Unique).Count -gt 0 -or
    @($RequiredCounts | Select-Object -Unique).Count -ne $RequiredCounts.Count) {
    throw 'RequiredCounts must contain unique positive integers.'
}

$requiredColumns = @(
    'BuildBackend', 'BuildRevision', 'ControllerRevision', 'Axis', 'SignedCounts',
    'FinalDisplacementPx', 'OrthogonalDisplacementPx', 'PixelsPerCount',
    'CrossAxisLeakagePercent', 'T50Ms', 'T90Ms', 'Valid'
)
$rows = [Collections.Generic.List[object]]::new()
$issues = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    if ($file.Directory.Name -notmatch '^counts(?<counts>[0-9]+)_run(?<run>[0-9]+)$') {
        $issues.Add("目录身份无效:$($file.Directory.Name)")
        continue
    }
    $directoryCounts = [int]$Matches.counts
    $run = [int]$Matches.run
    $imported = @(Import-Csv -LiteralPath $file.FullName)
    if ($imported.Count -eq 0) {
        $issues.Add("空文件:$($file.FullName)")
        continue
    }
    $missing = @($requiredColumns | Where-Object {
        $imported[0].PSObject.Properties.Name -notcontains $_
    })
    if ($missing.Count -gt 0) {
        throw "Probe summary misses columns [$($missing -join ', ')]: $($file.FullName)"
    }
    foreach ($row in $imported) {
        $signedCounts = [int](Get-FiniteDouble $row.SignedCounts 'SignedCounts')
        $axis = ([string]$row.Axis).ToLowerInvariant()
        $pixelsPerCount = Get-FiniteDouble $row.PixelsPerCount 'PixelsPerCount'
        $final = Get-FiniteDouble $row.FinalDisplacementPx 'FinalDisplacementPx'
        $recomputedScale = if ($signedCounts -ne 0) {
            [math]::Abs($final / $signedCounts)
        } else { 0.0 }
        if ([math]::Abs($pixelsPerCount - $recomputedScale) -gt 0.0001) {
            $issues.Add("比例列不一致:run$run/$axis/$signedCounts")
        }
        if ([math]::Abs($signedCounts) -ne $directoryCounts) {
            $issues.Add("目录counts与行不一致:run$run/$signedCounts")
        }
        if ($RequiredCounts -notcontains [math]::Abs($signedCounts)) {
            $issues.Add("协议外幅值:run$run/$signedCounts")
        }
        if ($signedCounts -eq 0 -or $final * $signedCounts -ge 0.0) {
            $issues.Add("主轴极性错误:run$run/$axis/$signedCounts")
        }
        if ($axis -ne 'x' -and $axis -ne 'y') {
            $issues.Add("未知轴:run$run/$axis")
        }
        $rows.Add([pscustomobject]@{
            Run = $run
            Counts = [math]::Abs($signedCounts)
            Direction = if ($signedCounts -gt 0) { 1 } else { -1 }
            Axis = $axis
            BuildBackend = [string]$row.BuildBackend
            BuildRevision = [string]$row.BuildRevision
            ControllerRevision = [string]$row.ControllerRevision
            Valid = [string]$row.Valid -eq '1'
            PixelsPerCount = $pixelsPerCount
            T50Ms = Get-FiniteDouble $row.T50Ms 'T50Ms'
            T90Ms = Get-FiniteDouble $row.T90Ms 'T90Ms'
            LeakagePercent = Get-FiniteDouble $row.CrossAxisLeakagePercent 'CrossAxisLeakagePercent'
        })
    }
}
if ($rows.Count -eq 0) { throw 'No parseable active profile trials were found.' }

$identities = @($rows | ForEach-Object {
    "$($_.BuildBackend)|$($_.BuildRevision)|r$($_.ControllerRevision)"
} | Sort-Object -Unique)
if ($identities.Count -ne 1) { $issues.Add("构建身份不唯一:$($identities -join ';')") }
$runIds = @($rows.Run | Sort-Object -Unique)
if ($runIds.Count -lt $MinimumRuns) {
    $issues.Add("轮数不足:$($runIds.Count)/$MinimumRuns")
}
if (@($rows | Where-Object { -not $_.Valid }).Count -gt 0) {
    $issues.Add('存在无效trial')
}
if (@($rows | Where-Object {
            $_.T50Ms -lt 0.0 -or $_.T90Ms -lt $_.T50Ms -or
            $_.T90Ms -gt $MaximumT90Ms
        }).Count -gt 0) {
    $issues.Add("存在无效响应时序或T90超过$MaximumT90Ms ms")
}

$groupResults = [Collections.Generic.List[object]]::new()
foreach ($run in $runIds) {
    foreach ($count in $RequiredCounts) {
        foreach ($axis in @('x', 'y')) {
            $cellIssues = [Collections.Generic.List[string]]::new()
            $cell = @($rows | Where-Object {
                $_.Run -eq $run -and $_.Counts -eq $count -and $_.Axis -eq $axis
            })
            $positive = @($cell | Where-Object Direction -eq 1)
            $negative = @($cell | Where-Object Direction -eq -1)
            if ($positive.Count -lt $MinimumTrialsPerDirection -or
                $negative.Count -lt $MinimumTrialsPerDirection) {
                $cellIssues.Add("正负样本不足:$($positive.Count)/$($negative.Count)")
            }
            $positiveScale = Get-Percentile @($positive.PixelsPerCount) 0.5
            $negativeScale = Get-Percentile @($negative.PixelsPerCount) 0.5
            $scale = Get-Percentile @($cell.PixelsPerCount) 0.5
            $denominator = ($positiveScale + $negativeScale) * 0.5
            $asymmetry = if ($denominator -gt 1e-9) {
                100.0 * [math]::Abs($positiveScale - $negativeScale) / $denominator
            } else { [double]::PositiveInfinity }
            $leakageP95 = Get-Percentile @($cell.LeakagePercent) 0.95
            if ($scale -lt $MinimumPixelsPerCount -or $scale -gt $MaximumPixelsPerCount) {
                $cellIssues.Add("比例越界:$scale")
            }
            if ($asymmetry -gt $MaximumDirectionAsymmetryPercent) {
                $cellIssues.Add("方向不对称:$asymmetry%")
            }
            if ($leakageP95 -gt $MaximumCrossAxisLeakagePercent) {
                $cellIssues.Add("正交泄漏:$leakageP95%")
            }
            $groupResults.Add([pscustomobject]@{
                Run = $run
                Counts = $count
                Axis = $axis
                PositiveTrials = $positive.Count
                NegativeTrials = $negative.Count
                PositivePixelsPerCount = $positiveScale
                NegativePixelsPerCount = $negativeScale
                PixelsPerCount = $scale
                DirectionAsymmetryPercent = $asymmetry
                T50Ms = Get-Percentile @($cell.T50Ms) 0.5
                T90Ms = Get-Percentile @($cell.T90Ms) 0.5
                CrossAxisLeakageP95Percent = $leakageP95
                Passed = $cellIssues.Count -eq 0
                Reason = if ($cellIssues.Count -eq 0) { 'passed' } else { $cellIssues -join ';' }
            })
            foreach ($issue in $cellIssues) {
                $issues.Add("run$run/$axis/$count`:$issue")
            }
        }
    }
}

foreach ($run in $runIds) {
    foreach ($axis in @('x', 'y')) {
        $scales = @($groupResults | Where-Object {
            $_.Run -eq $run -and $_.Axis -eq $axis
        } | ForEach-Object PixelsPerCount)
        $spread = Get-RelativeSpreadPercent $scales
        if ($spread -gt $MaximumLinearitySpreadPercent) {
            $issues.Add("run$run/$axis线性跨度:$spread%")
        }
    }
}
foreach ($count in $RequiredCounts) {
    foreach ($axis in @('x', 'y')) {
        $acrossRuns = @($groupResults | Where-Object {
            $_.Counts -eq $count -and $_.Axis -eq $axis
        })
        $scaleSpread = Get-RelativeSpreadPercent @($acrossRuns.PixelsPerCount)
        $t50Spread = (($acrossRuns.T50Ms | Measure-Object -Maximum).Maximum -
            ($acrossRuns.T50Ms | Measure-Object -Minimum).Minimum)
        $t90Spread = (($acrossRuns.T90Ms | Measure-Object -Maximum).Maximum -
            ($acrossRuns.T90Ms | Measure-Object -Minimum).Minimum)
        if ($scaleSpread -gt $MaximumCrossRunSpreadPercent) {
            $issues.Add("$axis/${count}跨轮比例跨度:$scaleSpread%")
        }
        if ($t50Spread -gt $MaximumTimingSpreadMs -or
            $t90Spread -gt $MaximumTimingSpreadMs) {
            $issues.Add("$axis/${count}跨轮时序跨度:$t50Spread/$t90Spread ms")
        }
    }
}

$protocolPassed = $issues.Count -eq 0
$axisX = @($groupResults | Where-Object Axis -eq 'x')
$axisY = @($groupResults | Where-Object Axis -eq 'y')
$summary = [pscustomobject]@{
    ProtocolPassed = $protocolPassed
    Identity = $identities -join ';'
    Files = $files.Count
    Runs = $runIds.Count
    Trials = $rows.Count
    AxisXPixelsPerCount = Get-Percentile @($axisX.PixelsPerCount) 0.5
    AxisYPixelsPerCount = Get-Percentile @($axisY.PixelsPerCount) 0.5
    AxisXT50Ms = Get-Percentile @($axisX.T50Ms) 0.5
    AxisYT50Ms = Get-Percentile @($axisY.T50Ms) 0.5
    AxisXT90Ms = Get-Percentile @($axisX.T90Ms) 0.5
    AxisYT90Ms = Get-Percentile @($axisY.T90Ms) 0.5
    Recommendation = if ($protocolPassed) { 'MANUAL_REVIEW_ONLY' } else { 'HOLD_DIAGNOSTIC' }
    Issues = $issues -join '|'
}

if (-not $OutputCsv) { $OutputCsv = Join-Path $root 'active_profile_protocol_summary.csv' }
if (-not $DecisionPath) { $DecisionPath = Join-Path $root 'active_profile_protocol_decision.txt' }
$resolvedOutput = [IO.Path]::GetFullPath($OutputCsv)
$resolvedDecision = [IO.Path]::GetFullPath($DecisionPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutput) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedDecision) | Out-Null
$groupResults | Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
@(
    "ProtocolPassed=$([int]$protocolPassed)"
    "Identity=$($summary.Identity)"
    "Files=$($summary.Files)"
    "Runs=$($summary.Runs)"
    "Trials=$($summary.Trials)"
    "RequiredCounts=$($RequiredCounts -join ',')"
    "MinimumTrialsPerDirection=$MinimumTrialsPerDirection"
    "MaximumDirectionAsymmetryPercent=$MaximumDirectionAsymmetryPercent"
    "MaximumCrossRunSpreadPercent=$MaximumCrossRunSpreadPercent"
    "MaximumLinearitySpreadPercent=$MaximumLinearitySpreadPercent"
    "MaximumTimingSpreadMs=$MaximumTimingSpreadMs"
    "MaximumT90Ms=$MaximumT90Ms"
    "MaximumCrossAxisLeakagePercent=$MaximumCrossAxisLeakagePercent"
    "AxisXPixelsPerCount=$($summary.AxisXPixelsPerCount)"
    "AxisYPixelsPerCount=$($summary.AxisYPixelsPerCount)"
    "AxisXT50Ms=$($summary.AxisXT50Ms)"
    "AxisYT50Ms=$($summary.AxisYT50Ms)"
    "AxisXT90Ms=$($summary.AxisXT90Ms)"
    "AxisYT90Ms=$($summary.AxisYT90Ms)"
    "Recommendation=$($summary.Recommendation)"
    "ProfileAutoWrite=0"
    "Issues=$($summary.Issues)"
) | Set-Content -LiteralPath $resolvedDecision -Encoding UTF8

$summary | Format-List | Out-Host
Write-Host "Active profile protocol summary written to $resolvedOutput"
Write-Host "Active profile protocol decision written to $resolvedDecision"
if ($RequirePass -and -not $protocolPassed) {
    throw "Active profile protocol failed: $($summary.Issues)"
}
$summary
