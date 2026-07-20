[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DataRoot,
    [string]$ExpectedBuildRevision = '',
    [double]$ExpectedBaselineMaxCountsPerSecond = 1440.0,
    [double]$ExpectedAdvisoryMaxCountsPerSecond = 1800.0,
    [string]$OutputEventsCsv = '',
    [string]$OutputSummaryCsv = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FiniteDouble {
    param([object]$Value, [string]$Name)
    $number = 0.0
    if (-not [double]::TryParse(
            [string]$Value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$number) -or [double]::IsNaN($number) -or
        [double]::IsInfinity($number)) {
        throw "Invalid finite number in $Name`: $Value"
    }
    return $number
}

function Get-Percentile {
    param([double[]]$Values, [double]$Ratio)
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $position = [math]::Max(0.0, [math]::Min(1.0, $Ratio)) * ($ordered.Count - 1)
    $lower = [math]::Floor($position)
    $upper = [math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$ordered[$lower] }
    return [double]$ordered[$lower] +
        ($position - $lower) * ([double]$ordered[$upper] - [double]$ordered[$lower])
}

$resolvedRoot = Resolve-Path -LiteralPath $DataRoot
$files = @(Get-ChildItem -LiteralPath $resolvedRoot -File | Where-Object {
    $_.Name -match '^horizontal_(left|right).*\.csv$'
})
if ($files.Count -eq 0) {
    throw "No horizontal_left/right CSV files found in $resolvedRoot"
}

$requiredColumns = @(
    'BuildBackend', 'BuildRevision', 'ControllerRevision',
    'AimPipelineEffectiveMode', 'AimPipelineShadowProcessed',
    'AimPipelineControlValid', 'AimPipelineControlSpeedLimited',
    'AimPipelineUnlimitedCountsX', 'AimPipelineUnlimitedCountsY',
    'AimPipelineRequestedCountsX', 'AimPipelineRequestedCountsY',
    'AimPipelineFrameCountLimit', 'RecoverySpeedAdviceEligible',
    'RecoverySpeedAdviceActive', 'RecoverySpeedAdviceExited',
    'RecoverySpeedAdviceLimited', 'RecoverySpeedBaselineMaxCps',
    'RecoverySpeedAdvisoryMaxCps', 'RecoverySpeedAdvisoryFrameCountLimit',
    'RecoverySpeedAdvisoryRequestedCountsX',
    'RecoverySpeedAdvisoryRequestedCountsY',
    'RecoverySpeedBaselineStaticBudgetFrames',
    'RecoverySpeedAdvisoryStaticBudgetFrames',
    'RecoverySpeedStaticBudgetFramesSaved'
)

$events = [Collections.Generic.List[object]]::new()
$identityKeys = [Collections.Generic.HashSet[string]]::new()
$violations = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $rows = @(Import-Csv -LiteralPath $file.FullName)
    if ($rows.Count -eq 0) { continue }
    foreach ($column in $requiredColumns) {
        if (-not $rows[0].PSObject.Properties.Name.Contains($column)) {
            throw "Missing required column $column in $($file.FullName)"
        }
    }
    foreach ($row in $rows) {
        if ($ExpectedBuildRevision -and $row.BuildRevision -ne $ExpectedBuildRevision) { continue }
        [void]$identityKeys.Add("$($row.BuildBackend)|$($row.BuildRevision)|$($row.ControllerRevision)")
        $eligible = $row.RecoverySpeedAdviceEligible -eq '1'
        $active = $row.RecoverySpeedAdviceActive -eq '1'
        $exited = $row.RecoverySpeedAdviceExited -eq '1'
        if (-not ($eligible -or $active -or $exited)) { continue }

        $baselineMax = Get-FiniteDouble $row.RecoverySpeedBaselineMaxCps 'RecoverySpeedBaselineMaxCps'
        $advisoryMax = Get-FiniteDouble $row.RecoverySpeedAdvisoryMaxCps 'RecoverySpeedAdvisoryMaxCps'
        $frameLimit = Get-FiniteDouble $row.AimPipelineFrameCountLimit 'AimPipelineFrameCountLimit'
        $formalX = Get-FiniteDouble $row.AimPipelineRequestedCountsX 'AimPipelineRequestedCountsX'
        $formalY = Get-FiniteDouble $row.AimPipelineRequestedCountsY 'AimPipelineRequestedCountsY'
        $advisoryLimit = Get-FiniteDouble $row.RecoverySpeedAdvisoryFrameCountLimit 'RecoverySpeedAdvisoryFrameCountLimit'
        $advisoryX = Get-FiniteDouble $row.RecoverySpeedAdvisoryRequestedCountsX 'RecoverySpeedAdvisoryRequestedCountsX'
        $advisoryY = Get-FiniteDouble $row.RecoverySpeedAdvisoryRequestedCountsY 'RecoverySpeedAdvisoryRequestedCountsY'
        $formalMagnitude = [math]::Sqrt($formalX * $formalX + $formalY * $formalY)
        $advisoryMagnitude = [math]::Sqrt($advisoryX * $advisoryX + $advisoryY * $advisoryY)
        if ($active -and (-not $eligible -or $row.AimPipelineControlSpeedLimited -ne '1' -or
                $row.AimPipelineControlValid -ne '1' -or
                $row.AimPipelineEffectiveMode -ne 'shadow' -or
                $row.AimPipelineShadowProcessed -ne '1')) {
            $violations.Add("$($file.Name): active advice outside an eligible baseline-limited frame")
        }
        if ([math]::Abs($baselineMax - $ExpectedBaselineMaxCountsPerSecond) -gt 0.01) {
            $violations.Add("$($file.Name): baseline identity is $baselineMax counts/s")
        }
        if ([math]::Abs($advisoryMax - $ExpectedAdvisoryMaxCountsPerSecond) -gt 0.01) {
            $violations.Add("$($file.Name): advisory identity is $advisoryMax counts/s")
        }
        if ($formalMagnitude -gt $frameLimit + 1e-6) {
            $violations.Add("$($file.Name): formal request exceeds the 1440 frame limit")
        }
        if ($active -and $advisoryMagnitude -gt $advisoryLimit + 1e-6) {
            $violations.Add("$($file.Name): advisory request exceeds its diagnostic frame limit")
        }
        $events.Add([pscustomobject]@{
            File = $file.Name
            Eligible = [int]$eligible
            Active = [int]$active
            Exited = [int]$exited
            AdvisoryLimited = [int]($row.RecoverySpeedAdviceLimited -eq '1')
            BaselineStaticBudgetFrames = Get-FiniteDouble $row.RecoverySpeedBaselineStaticBudgetFrames 'RecoverySpeedBaselineStaticBudgetFrames'
            AdvisoryStaticBudgetFrames = Get-FiniteDouble $row.RecoverySpeedAdvisoryStaticBudgetFrames 'RecoverySpeedAdvisoryStaticBudgetFrames'
            StaticBudgetFramesSaved = Get-FiniteDouble $row.RecoverySpeedStaticBudgetFramesSaved 'RecoverySpeedStaticBudgetFramesSaved'
        })
    }
}

if ($identityKeys.Count -eq 0) { throw 'No rows matched the requested build revision.' }
if (-not $ExpectedBuildRevision -and $identityKeys.Count -ne 1) {
    throw "Mixed build identities detected: $($identityKeys -join ', ')"
}
if ($events.Count -eq 0) { throw 'No recovery speed advice windows were found.' }

$activeEvents = @($events | Where-Object Active -eq 1)
$summary = [pscustomobject]@{
    BuildIdentity = @($identityKeys)[0]
    EligibleFrames = @($events | Where-Object Eligible -eq 1).Count
    ActiveFrames = $activeEvents.Count
    ExitFrames = @($events | Where-Object Exited -eq 1).Count
    BaselineStaticBudgetP50 = Get-Percentile @($activeEvents.BaselineStaticBudgetFrames) 0.50
    BaselineStaticBudgetP95 = Get-Percentile @($activeEvents.BaselineStaticBudgetFrames) 0.95
    AdvisoryStaticBudgetP50 = Get-Percentile @($activeEvents.AdvisoryStaticBudgetFrames) 0.50
    AdvisoryStaticBudgetP95 = Get-Percentile @($activeEvents.AdvisoryStaticBudgetFrames) 0.95
    StaticBudgetFramesSavedP50 = Get-Percentile @($activeEvents.StaticBudgetFramesSaved) 0.50
    StaticBudgetFramesSavedP95 = Get-Percentile @($activeEvents.StaticBudgetFramesSaved) 0.95
    ViolationCount = $violations.Count
    Conclusion = if ($violations.Count -eq 0) { 'DIAGNOSTIC_ONLY_HOLD_SHADOW' } else { 'REJECT' }
}

if ($OutputEventsCsv) { $events | Export-Csv -LiteralPath $OutputEventsCsv -NoTypeInformation -Encoding UTF8 }
if ($OutputSummaryCsv) { @($summary) | Export-Csv -LiteralPath $OutputSummaryCsv -NoTypeInformation -Encoding UTF8 }
if ($violations.Count -gt 0) { throw ($violations -join [Environment]::NewLine) }
$summary
