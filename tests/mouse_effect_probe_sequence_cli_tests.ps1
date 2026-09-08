[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\scripts\path_safety.psm1') -Force

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $repositoryRoot
$resolvedTestRoot = $ownedTest.RootPath

try {
    $output = Join-Path $resolvedTestRoot 'physical-b-holdout.json'
    & $resolvedExecutable `
        --output $output `
        --profile physical-b-holdout `
        --guard-samples 32 `
        --lfsr-order 6 `
        --feedback-mask 51 `
        --seed 1 `
        --phase 21 `
        --offline-sequence-semantic-sha256 `
            e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4
    Assert-True ($LASTEXITCODE -eq 0) `
        'The holdout CLI should generate the frozen sequence.'
    Assert-True (Test-Path -LiteralPath $output -PathType Leaf) `
        'The holdout CLI did not publish a sequence file.'
    $sequence = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    Assert-True ($sequence.schema -eq 5) 'The holdout schema must be 5.'
    Assert-True ($sequence.profile -eq 'physical_b_prbs_holdout') `
        'The holdout profile did not match.'
    Assert-True ($sequence.samples.Count -eq 288) `
        'The holdout sample count must be 288.'
    Assert-True ($sequence.blocks.Count -eq 2) `
        'The holdout block count must be 2.'
    Assert-True ($sequence.blocks[0].role -eq 'cross_run_holdout' -and
                 $sequence.blocks[1].role -eq 'cross_run_holdout') `
        'The holdout blocks must carry the cross-run role.'
    Assert-True ($sequence.summary.net_x_counts -eq 0 -and
                 $sequence.summary.max_abs_prefix_x_counts -eq 1) `
        'The holdout must be net-zero with one-count prefix displacement.'

    $invalid = Join-Path $resolvedTestRoot 'invalid-primary-recurrence.json'
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $resolvedExecutable `
        --output $invalid `
        --profile physical-b-holdout `
        --guard-samples 32 `
        --lfsr-order 6 `
        --feedback-mask 39 `
        --seed 1 `
        --phase 49 `
        --offline-sequence-semantic-sha256 `
            e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4 `
        2>&1 | Out-Null
    $invalidExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    Assert-True ($invalidExitCode -ne 0) `
        'The holdout CLI must reject the Primary recurrence.'
    Assert-True (-not (Test-Path -LiteralPath $invalid)) `
        'An invalid recurrence must not leave an output file.'

    $magnitudeOutput = Join-Path $resolvedTestRoot `
        'physical-b-command-magnitude-primary.json'
    & $resolvedExecutable `
        --output $magnitudeOutput `
        --profile physical-b-command-magnitude `
        --run-role primary `
        --baseline-samples 64 `
        --response-samples 48 `
        --guard-samples 32
    Assert-True ($LASTEXITCODE -eq 0) `
        'The command-magnitude Primary CLI should generate its fixed sequence.'
    $magnitude = Get-Content -LiteralPath $magnitudeOutput -Raw |
        ConvertFrom-Json
    Assert-True ($magnitude.schema -eq 6 -and
                 $magnitude.profile -eq
                    'physical_b_command_magnitude_primary' -and
                 $magnitude.samples.Count -eq 1684 -and
                 $magnitude.blocks.Count -eq 10 -and
                 $magnitude.summary.net_x_counts -eq 0 -and
                 $magnitude.summary.max_abs_prefix_x_counts -eq 13) `
        'The command-magnitude Primary CLI contract did not match.'
    Assert-True ((@($magnitude.blocks | ForEach-Object {
                    [int]$_.amplitude_counts }) -join ',') -eq
                    '1,1,4,4,13,13,2,2,8,8') `
        'The command-magnitude Primary amplitude order must stay frozen.'

    $invalidMagnitude = Join-Path $resolvedTestRoot `
        'invalid-command-magnitude.json'
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $resolvedExecutable `
        --output $invalidMagnitude `
        --profile physical-b-command-magnitude `
        --run-role primary `
        --baseline-samples 64 `
        --response-samples 47 `
        --guard-samples 32 2>&1 | Out-Null
    $invalidMagnitudeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    Assert-True ($invalidMagnitudeExitCode -ne 0 -and
                 -not (Test-Path -LiteralPath $invalidMagnitude)) `
        'The command-magnitude CLI must reject response-window drift.'

    $compositeOutput = Join-Path $resolvedTestRoot `
        'physical-b-composite-phase-calibration.json'
    & $resolvedExecutable `
        --output $compositeOutput `
        --profile physical-b-composite-phase-calibration
    Assert-True ($LASTEXITCODE -eq 0) `
        'The composite-phase calibration CLI should generate its fixed sequence.'
    $composite = Get-Content -LiteralPath $compositeOutput -Raw |
        ConvertFrom-Json
    Assert-True ($composite.schema -eq 7 -and
                 $composite.profile -eq
                    'physical_b_composite_phase_calibration' -and
                 $composite.samples.Count -eq 295 -and
                 $composite.windows.Count -eq 42 -and
                 $composite.blocks.Count -eq 0 -and
                 $composite.summary.net_x_counts -eq 0 -and
                 $composite.summary.max_abs_prefix_x_counts -eq 1) `
        'The composite-phase CLI sample/window/net contract did not match.'
    Assert-True (@($composite.windows | Where-Object {
                    [bool]$_.negative_control }).Count -eq 4 -and
                 @($composite.samples | Where-Object {
                    [int]$_.dx_counts -ne 0 }).Count -eq 38) `
        'The composite-phase CLI must freeze 38 pulses and four controls.'

    Assert-True ($composite.request.timer_mode -ceq 'HIGH_RESOLUTION_ONE_SHOT_OR_FAIL' -and
                 $composite.request.active_guard_ns -eq 300000 -and
                 $composite.request.max_wake_lateness_ns -eq 150000 -and
                 $composite.request.max_event_interval_width_ns -eq 100000 -and
                 $composite.request.max_active_wait_ns_per_event -eq 350000 -and
                 $composite.request.max_active_wait_ns_total -eq 14700000) `
        'The omitted scheduler policy must preserve the complete legacy tuple.'
    $resourceOutput = Join-Path $resolvedTestRoot 'composite-active-1ms.json'
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $resolvedExecutable --output $resourceOutput `
        --profile physical-b-composite-phase-calibration `
        --scheduler-policy active-1ms-v1 2>&1 | Out-Null
    $resourceExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    Assert-True ($resourceExitCode -eq 0 -and
                 (Test-Path -LiteralPath $resourceOutput -PathType Leaf)) `
        'The explicit active-1ms-v1 CLI must publish its distinct fixed sequence.'
    $resource = Get-Content -LiteralPath $resourceOutput -Raw | ConvertFrom-Json
    Assert-True ($resource.schema -eq 7 -and
                 $resource.request.timer_mode -ceq 'HIGH_RESOLUTION_ONE_SHOT_ACTIVE_1MS_V1' -and
                 $resource.request.active_guard_ns -eq 1000000 -and
                 $resource.request.max_wake_lateness_ns -eq 150000 -and
                 $resource.request.max_event_interval_width_ns -eq 100000 -and
                 $resource.request.max_active_wait_ns_per_event -eq 1000000 -and
                 $resource.request.max_active_wait_ns_total -eq 42000000 -and
                 $resource.sequence_sha256 -cne $composite.sequence_sha256 -and
                 ($resource.samples | ConvertTo-Json -Depth 10 -Compress) -ceq
                    ($composite.samples | ConvertTo-Json -Depth 10 -Compress) -and
                 ($resource.windows | ConvertTo-Json -Depth 10 -Compress) -ceq
                    ($composite.windows | ConvertTo-Json -Depth 10 -Compress)) `
        'The new resource tuple must change identity while preserving commands and phase cells.'
    $explicitLegacy = Join-Path $resolvedTestRoot 'composite-legacy.json'
    & $resolvedExecutable --output $explicitLegacy `
        --profile physical-b-composite-phase-calibration --scheduler-policy legacy
    Assert-True ($LASTEXITCODE -eq 0 -and
        (Get-Content -LiteralPath $explicitLegacy -Raw) -ceq
            (Get-Content -LiteralPath $compositeOutput -Raw)) `
        'Explicit legacy must preserve the default sequence bytes and semantic hash.'
    $invalidPolicies = @(
        @{ name = 'unknown'; profile = 'physical-b-composite-phase-calibration'; arguments = @('--scheduler-policy', 'active-2ms-v1') },
        @{ name = 'duplicate'; profile = 'physical-b-composite-phase-calibration'; arguments = @('--scheduler-policy', 'legacy', '--scheduler-policy', 'active-1ms-v1') },
        @{ name = 'other-profile'; profile = 'physical-b-command-magnitude'; arguments = @('--scheduler-policy', 'active-1ms-v1') },
        @{ name = 'other-profile-legacy'; profile = 'physical-b-command-magnitude'; arguments = @('--scheduler-policy', 'legacy') }
    )
    foreach ($case in $invalidPolicies) {
        $invalidPolicyOutput = Join-Path $resolvedTestRoot ($case.name + '.json')
        $caseArguments = @('--output', $invalidPolicyOutput, '--profile', $case.profile) + $case.arguments
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $resolvedExecutable @caseArguments 2>&1 | Out-Null
        $invalidPolicyExitCode = $LASTEXITCODE
        $ErrorActionPreference = $savedErrorActionPreference
        Assert-True ($invalidPolicyExitCode -ne 0 -and
                     -not (Test-Path -LiteralPath $invalidPolicyOutput)) `
            "Invalid scheduler policy must fail without output: $($case.name)"
    }

    Write-Host 'Mouse Effect Probe sequence CLI contracts passed.'
}
finally {
    Remove-XenOwnedTestDirectory -RootPath $resolvedTestRoot `
        -BasePath $ownedTest.BasePath -RepositoryRoot $repositoryRoot `
        -OwnerId $ownedTest.OwnerId
}
