[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedParent = (Resolve-Path -LiteralPath (Split-Path -Parent $TestRoot)).Path
$leaf = Split-Path -Leaf $TestRoot
Assert-True ($leaf -eq 'mouse-effect-probe-sequence-cli-tests') `
    'Unexpected isolated test directory name.'
$resolvedTestRoot = Join-Path $resolvedParent $leaf
if (Test-Path -LiteralPath $resolvedTestRoot) {
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $resolvedTestRoot | Out-Null

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

    Write-Host 'Mouse Effect Probe sequence CLI holdout contract passed.'
}
finally {
    if (Test-Path -LiteralPath $resolvedTestRoot) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
