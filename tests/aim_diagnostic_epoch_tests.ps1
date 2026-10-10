param([Parameter(Mandatory = $true)][string]$FixtureExecutable)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../scripts/aim_report.ps1')
. (Join-Path $PSScriptRoot '../scripts/aim_control_diagnostics.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-diagnostic-epochs-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
& $FixtureExecutable $root
if ($LASTEXITCODE -ne 0) { throw 'Actual ring/report fixture failed.' }
$report = Get-Content -LiteralPath (Join-Path $root 'ring.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$script:failures = 0
function Check([bool]$Value, [string]$Description) {
    if ($Value) { Write-Output "PASS $Description" } else { Write-Output "FAIL $Description"; ++$script:failures }
}
function Copy-Sample([object]$Sample) { return ($Sample | ConvertTo-Json -Depth 8 | ConvertFrom-Json) }
Check ($report.runtime_samples_dropped -eq 1 -and $report.samples.Count -eq 4097) 'Actual ring loss remains visible in report; never drop=0 evidence'
$control = Get-XenAimControlDiagnosticsSummary -Samples @($report.samples)
Check ($control.x.nonzero_direction_reversals -eq 0) 'Dropped reset must not join directions across persistent epoch'
foreach ($mode in @('epoch', 'sequence_gap', 'sequence_restart', 'epoch_missing', 'healthy', 'large_epoch', 'legacy', 'zero_epoch', 'large_sequence')) {
    $left = Copy-Sample $report.samples[0]
    $right = Copy-Sample $report.samples[1]
    $left.sequence = 1; $right.sequence = 2
    $right.aim_observation_epoch = $left.aim_observation_epoch
    switch ($mode) {
        'epoch' { $right.aim_observation_epoch = '9007199254740994' }
        'sequence_gap' { $right.sequence = 3 }
        'sequence_restart' { $right.sequence = 1 }
        'epoch_missing' { $right.PSObject.Properties.Remove('aim_observation_epoch') }
        'large_epoch' { $left.aim_observation_epoch = '18446744073709551614'; $right.aim_observation_epoch = '18446744073709551615' }
        'legacy' { $left.PSObject.Properties.Remove('aim_observation_epoch'); $right.PSObject.Properties.Remove('aim_observation_epoch') }
        'zero_epoch' { $left.aim_observation_epoch = '0'; $right.aim_observation_epoch = '0' }
        'large_sequence' { $left.sequence = '18446744073709551614'; $right.sequence = '18446744073709551615' }
    }
    $control = Get-XenAimControlDiagnosticsSummary -Samples @($left, $right)
    $expected = if ($mode -in @('healthy', 'legacy', 'zero_epoch', 'large_sequence')) { 1 } else { 0 }
    Check ($control.x.nonzero_direction_reversals -eq $expected) "Direction continuity $mode"
    $right.aim_track_id = 8
    $summary = Get-XenAimReportSummary -Samples @($left, $right) -PredictionEnabled off -MaxPredictionLeadPercent 35
    Check ($summary.target_switches -eq $expected) "Target continuity $mode"
    Check ([bool]$summary.contract_valid) "Per-frame checks remain valid $mode"
}
foreach ($field in @('sequence', 'aim_observation_epoch')) {
    foreach ($bad in @([double]2, '-1', '18446744073709551616', $null, $true, 'invalid')) {
        $item = Copy-Sample $report.samples[0]
        $item.$field = $bad
        $controlRejected = $false; $reportRejected = $false
        try { $null = Get-XenAimControlDiagnosticsSummary -Samples @($item) } catch { $controlRejected = $true }
        try { $null = Get-XenAimReportSummary -Samples @($item) -PredictionEnabled off -MaxPredictionLeadPercent 35 } catch { $reportRejected = $true }
        Check ($controlRejected -and $reportRejected) "Malformed provided $field rejected by both consumers: $bad"
    }
}
$left = Copy-Sample $report.samples[0]
$right = Copy-Sample $report.samples[1]
$right.sequence = 2; $right.aim_track_state = 'LOST'; $right.aim_track_predicted = $true
$right.aim_has_command = $false; $right.aim_matched_observation_valid = $false
$right.aim_matched_observation_box = @(0, 0, 0, 0)
$stateSummary = Get-XenAimReportSummary -Samples @($left, $right) -PredictionEnabled off -MaxPredictionLeadPercent 35
Check ($stateSummary.target_state_changes -eq 0 -and $stateSummary.prediction_state_changes -eq 0) 'Same track number across epochs cannot create state/prediction transitions'
Write-Output "Retained fixture: $root"
if ($script:failures) { throw "$script:failures diagnostic continuity assertions failed" }
