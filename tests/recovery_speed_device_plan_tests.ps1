param([Parameter(Mandatory = $true)][string]$RepoRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath($RepoRoot)
$generator = Join-Path $root 'tools\new_recovery_speed_device_plan.ps1'
$auditor = Join-Path $root 'tools\test_recovery_speed_device_plan.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('xen_recovery_speed_plan_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
try {
    $config = Join-Path $temporaryRoot 'config.ini'
    @(
        '[capture]'
        'ndi_source_name = test (main)'
        '[mouse]'
        'input_method = KMBOX_NET'
        'kmbox_net_uuid = test-device'
    ) | Set-Content -LiteralPath $config -Encoding UTF8
    $plan = Join-Path $temporaryRoot 'plan'
    & $generator -Config $config -DeviceId 'KMBOX_NET:test-device' -NdiSource 'test (main)' `
        -OutputDirectory $plan -BuildBackend DML `
        -BuildRevision '0123456789abcdef0123456789abcdef01234567' -ControllerRevision 65 `
        -RoiX 120 -RoiY 100 -RoiWidth 80 -RoiHeight 80 | Out-Null

    $rows = @(Import-Csv -LiteralPath (Join-Path $plan 'recovery_speed_device_plan.csv'))
    if ($rows.Count -ne 64) { throw "Expected 64 plan rows, got $($rows.Count)." }
    $trials = @($rows.Trial | Sort-Object -Unique)
    if ($trials.Count -ne 4) { throw "Expected four trials, got $($trials.Count)." }
    foreach ($trial in $trials) {
        $trialRows = @($rows | Where-Object Trial -eq $trial)
        if (($trialRows.DeltaX | Measure-Object -Sum).Sum -ne 0) { throw "Trial $trial is not zero-net." }
        if (($trialRows | Measure-Object -Property TrialAbsoluteCounts -Maximum).Maximum -gt 120) {
            throw "Trial $trial exceeds the absolute-count envelope."
        }
        if (($trialRows | Measure-Object -Property MaximumExcursionCounts -Maximum).Maximum -gt 60) {
            throw "Trial $trial exceeds the excursion envelope."
        }
    }
    $decision = Get-Content -LiteralPath (Join-Path $plan 'recovery_speed_device_plan_decision.txt') -Raw
    if ($decision -notmatch 'PlanValidationPassed=1' -or $decision -notmatch 'Recommendation=MANUAL_REVIEW_ONLY' -or
        $decision -notmatch 'ExecutionEnabled=0' -or $decision -notmatch 'PhysicalExecutionAuthorized=0') {
        throw 'Valid plan decision did not preserve manual-review-only execution isolation.'
    }

    $tamperedPlan = Join-Path $temporaryRoot 'tampered'
    Copy-Item -LiteralPath $plan -Destination $tamperedPlan -Recurse
    $tamperedCsv = Join-Path $tamperedPlan 'recovery_speed_device_plan.csv'
    $tamperedRows = @(Import-Csv -LiteralPath $tamperedCsv)
    $tamperedRows[0].DeltaX = '9'
    $tamperedRows | Export-Csv -LiteralPath $tamperedCsv -NoTypeInformation -Encoding UTF8
    $tamperedDecision = Join-Path $tamperedPlan 'tampered_decision.txt'
    $result = & $auditor -PlanDirectory $tamperedPlan -DecisionPath $tamperedDecision
    if ($result.PlanValidationPassed -or $result.Recommendation -ne 'HOLD_DIAGNOSTIC' -or
        $result.Issues -notmatch 'plan-hash') {
        throw 'Plan tampering was not rejected by the hash gate.'
    }

    $unsafePlan = Join-Path $temporaryRoot 'unsafe'
    Copy-Item -LiteralPath $plan -Destination $unsafePlan -Recurse
    $unsafeManifest = Join-Path $unsafePlan 'recovery_speed_device_manifest.txt'
    (Get-Content -LiteralPath $unsafeManifest -Encoding UTF8) -replace '^ExecutionEnabled=0$', 'ExecutionEnabled=1' |
        Set-Content -LiteralPath $unsafeManifest -Encoding UTF8
    $unsafeResult = & $auditor -PlanDirectory $unsafePlan -DecisionPath (Join-Path $unsafePlan 'decision.txt')
    if ($unsafeResult.PlanValidationPassed -or $unsafeResult.Issues -notmatch 'unsafe-flag:ExecutionEnabled') {
        throw 'Unsafe execution flag was not rejected.'
    }

    $duplicateRejected = $false
    try {
        & $generator -Config $config -DeviceId 'KMBOX_NET:test-device' -NdiSource 'test (main)' `
            -OutputDirectory $plan -BuildBackend DML `
            -BuildRevision '0123456789abcdef0123456789abcdef01234567' -ControllerRevision 65 `
            -RoiX 120 -RoiY 100 -RoiWidth 80 -RoiHeight 80 | Out-Null
    } catch { $duplicateRejected = $true }
    if (-not $duplicateRejected) { throw 'Generator overwrote an existing plan directory.' }

    $executorSource = Get-Content -LiteralPath (Join-Path $root 'Xen\runtime\recovery_speed_device_executor_main.cpp') -Raw
    $captureStop = $executorSource.IndexOf('capture.reset();', [StringComparison]::Ordinal)
    $confirmationRead = $executorSource.IndexOf('std::getline(std::cin, confirmation)', [StringComparison]::Ordinal)
    $captureRestart = $executorSource.IndexOf(
        'capture = startStableCapture(config, plan, current, error);',
        [Math]::Max(0, $confirmationRead), [StringComparison]::Ordinal)
    if ($captureStop -lt 0 -or $confirmationRead -le $captureStop -or $captureRestart -le $confirmationRead -or
        $executorSource -notmatch 'diagnostics\.droppedFrames != 0') {
        throw 'Second-stage confirmation no longer isolates and revalidates the NDI capture session.'
    }

    Write-Host 'Recovery speed device plan tests passed.'
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
}
