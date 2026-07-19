param(
    [Parameter(Mandatory = $true)][string]$BaselineFrames,
    [string]$ComparisonFrames = '',
    [string]$OutputCsv = ''
)

$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $parsed = 0.0
    if (-not [double]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed) -or
        [double]::IsNaN($parsed) -or [double]::IsInfinity($parsed)) {
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

function Get-Magnitude {
    param([object]$Row, [string]$X, [string]$Y)
    $xValue = Get-FiniteDouble $Row.$X $X
    $yValue = Get-FiniteDouble $Row.$Y $Y
    return [math]::Sqrt($xValue * $xValue + $yValue * $yValue)
}

function Get-Phase {
    param([string]$Scenario)
    if ($Scenario -eq 'left' -or $Scenario -eq 'right') { return 'sustained' }
    if ($Scenario -eq 'jump' -or $Scenario -eq 'reverse') { return 'maneuver' }
    return 'static'
}

function Import-PhaseFrames {
    param([string]$Path)
    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "Cross-domain frame CSV does not exist: $resolved"
    }
    $rows = @(Import-Csv -LiteralPath $resolved | Where-Object Detected -eq '1')
    if ($rows.Count -eq 0) { throw "Cross-domain frame CSV has no detected rows: $resolved" }
    $required = @(
        'Scenario', 'Detected', 'CandidateErrorX', 'CandidateErrorY',
        'TruthRateX', 'TruthRateY', 'PhysicalCameraRateX', 'PhysicalCameraRateY',
        'ModelAngleDeltaDeg', 'ModelRateDeltaDps', 'ManeuverRateEvidenceDps',
        'ManeuverModelActive', 'UnlimitedToFrameLimitRatio',
        'LimitedToUnlimitedRatio'
    )
    $missing = @($required | Where-Object { $rows[0].PSObject.Properties.Name -notcontains $_ })
    if ($missing.Count -gt 0) {
        throw "Cross-domain frame CSV misses phase diagnostics [$($missing -join ', ')]: $resolved"
    }
    return $rows
}

function Get-ErrorP95 {
    param([object[]]$Rows)
    if ($Rows.Count -eq 0) { return 0.0 }
    $values = @($Rows | ForEach-Object {
        Get-Magnitude $_ 'CandidateErrorX' 'CandidateErrorY'
    })
    return Get-Percentile $values 0.95
}

$baselineRows = Import-PhaseFrames $BaselineFrames
$comparisonRows = if ($ComparisonFrames) { Import-PhaseFrames $ComparisonFrames } else { @() }
$comparisonLookup = @{}
foreach ($row in $comparisonRows) {
    $key = "$($row.Scenario)|$($row.Variant)|$($row.TimeSeconds)"
    if ($comparisonLookup.ContainsKey($key)) {
        throw "Comparison frame identity is not unique: $key"
    }
    $comparisonLookup[$key] = $row
}
$result = [Collections.Generic.List[object]]::new()
$groups = [Collections.Generic.List[object]]::new()
foreach ($scenario in @('jump', 'left', 'reverse', 'right', 'static')) {
    $groups.Add([pscustomobject]@{ Name = $scenario; Phase = Get-Phase $scenario })
}
$groups.Add([pscustomobject]@{ Name = 'sustained'; Phase = 'sustained' })
$groups.Add([pscustomobject]@{ Name = 'maneuver'; Phase = 'maneuver' })

foreach ($group in $groups) {
    $base = if ($group.Name -eq $group.Phase -and
        ($group.Name -eq 'sustained' -or $group.Name -eq 'maneuver')) {
        @($baselineRows | Where-Object { (Get-Phase $_.Scenario) -eq $group.Phase })
    } else {
        @($baselineRows | Where-Object Scenario -eq $group.Name)
    }
    if ($base.Count -eq 0) { continue }
    $compare = if ($comparisonRows.Count -eq 0) { @() } elseif (
        $group.Name -eq $group.Phase -and
        ($group.Name -eq 'sustained' -or $group.Name -eq 'maneuver')) {
        @($comparisonRows | Where-Object { (Get-Phase $_.Scenario) -eq $group.Phase })
    } else {
        @($comparisonRows | Where-Object Scenario -eq $group.Name)
    }
    $matchedBase = [Collections.Generic.List[object]]::new()
    $matchedCompare = [Collections.Generic.List[object]]::new()
    if ($comparisonRows.Count -gt 0) {
        foreach ($row in $base) {
            $key = "$($row.Scenario)|$($row.Variant)|$($row.TimeSeconds)"
            if (-not $comparisonLookup.ContainsKey($key)) { continue }
            $matchedBase.Add($row)
            $matchedCompare.Add($comparisonLookup[$key])
        }
    }
    $featureRows = if ($comparisonRows.Count -gt 0) { @($matchedBase) } else { $base }
    if ($featureRows.Count -eq 0) {
        throw "Baseline and comparison have no common detected frames for cohort '$($group.Name)'."
    }
    $truthRates = @($featureRows | ForEach-Object { Get-Magnitude $_ 'TruthRateX' 'TruthRateY' })
    $physicalRates = @($featureRows | ForEach-Object {
        Get-Magnitude $_ 'PhysicalCameraRateX' 'PhysicalCameraRateY'
    })
    $modelAngles = @($featureRows | ForEach-Object {
        Get-FiniteDouble $_.ModelAngleDeltaDeg 'ModelAngleDeltaDeg'
    })
    $modelRates = @($featureRows | ForEach-Object {
        Get-FiniteDouble $_.ModelRateDeltaDps 'ModelRateDeltaDps'
    })
    $evidence = @($featureRows | ForEach-Object {
        Get-FiniteDouble $_.ManeuverRateEvidenceDps 'ManeuverRateEvidenceDps'
    })
    $limitRatios = @($featureRows | ForEach-Object {
        Get-FiniteDouble $_.UnlimitedToFrameLimitRatio 'UnlimitedToFrameLimitRatio'
    })
    $appliedRatios = @($featureRows | ForEach-Object {
        Get-FiniteDouble $_.LimitedToUnlimitedRatio 'LimitedToUnlimitedRatio'
    })
    $baselineP95 = Get-ErrorP95 $featureRows
    $comparisonP95 = if ($matchedCompare.Count -gt 0) {
        Get-ErrorP95 @($matchedCompare)
    } else { 0.0 }
    $result.Add([pscustomobject]@{
        Cohort = $group.Name
        Phase = $group.Phase
        BaselineSamples = $base.Count
        ComparisonSamples = $compare.Count
        MatchedSamples = $featureRows.Count
        BaselineMatchPercent = if ($comparisonRows.Count -gt 0) {
            100.0 * $featureRows.Count / $base.Count
        } else { 100.0 }
        ComparisonMatchPercent = if ($comparisonRows.Count -gt 0 -and $compare.Count -gt 0) {
            100.0 * $featureRows.Count / $compare.Count
        } else { 100.0 }
        BaselineErrorP95Deg = $baselineP95
        ComparisonErrorP95Deg = $comparisonP95
        ErrorP95DeltaPercent = if ($compare.Count -gt 0 -and $baselineP95 -gt 1e-9) {
            100.0 * ($comparisonP95 - $baselineP95) / $baselineP95
        } else { 0.0 }
        TruthRateP50Dps = Get-Percentile $truthRates 0.50
        TruthRateP95Dps = Get-Percentile $truthRates 0.95
        PhysicalRateP50Dps = Get-Percentile $physicalRates 0.50
        PhysicalRateP95Dps = Get-Percentile $physicalRates 0.95
        ModelAngleDeltaP50Deg = Get-Percentile $modelAngles 0.50
        ModelAngleDeltaP95Deg = Get-Percentile $modelAngles 0.95
        ModelRateDeltaP50Dps = Get-Percentile $modelRates 0.50
        ModelRateDeltaP95Dps = Get-Percentile $modelRates 0.95
        ManeuverEvidenceP50Dps = Get-Percentile $evidence 0.50
        ManeuverEvidenceP95Dps = Get-Percentile $evidence 0.95
        UnlimitedToLimitP50 = Get-Percentile $limitRatios 0.50
        UnlimitedToLimitP95 = Get-Percentile $limitRatios 0.95
        AppliedToUnlimitedP05 = Get-Percentile $appliedRatios 0.05
        SaturatedPercent = 100.0 * @($limitRatios | Where-Object { $_ -gt 1.0 + 1e-9 }).Count /
            $featureRows.Count
        ManeuverActivePercent = 100.0 * @($featureRows | Where-Object ManeuverModelActive -eq '1').Count /
            $featureRows.Count
    })
}

$result | Format-Table Cohort,BaselineSamples,ComparisonSamples,MatchedSamples,BaselineMatchPercent,
    BaselineErrorP95Deg,ComparisonErrorP95Deg,ErrorP95DeltaPercent,
    TruthRateP50Dps,PhysicalRateP50Dps,ModelAngleDeltaP50Deg,
    ModelRateDeltaP50Dps,ManeuverEvidenceP50Dps,UnlimitedToLimitP50,
    SaturatedPercent,ManeuverActivePercent -AutoSize | Out-Host

if ($OutputCsv) {
    $resolvedOutput = [IO.Path]::GetFullPath($OutputCsv)
    $parent = Split-Path -Parent $resolvedOutput
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $result | Export-Csv -LiteralPath $resolvedOutput -NoTypeInformation -Encoding UTF8
    Write-Host "Phase separation summary written to $resolvedOutput"
}

$result
