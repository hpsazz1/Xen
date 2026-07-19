[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateExe,
    [Parameter(Mandatory = $true)][ValidateSet('DML', 'CUDA')][string]$Backend
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('xen_machine_profile_candidate_' + [guid]::NewGuid().ToString('N'))
$dataRoot = Join-Path $root 'protocol'
$configPath = Join-Path $root 'config.ini'
$candidatePath = Join-Path $root 'candidate.profile'
$rejectedPath = Join-Path $root 'rejected.profile'

try {
    New-Item -ItemType Directory -Path $dataRoot | Out-Null
    $configBackend = if ($Backend -eq 'DML') { 'DML' } else { 'TRT' }
    @"
capture_method = ndi
ndi_source_name = Unit Test Source
ndi_source_width = 2560
ndi_source_height = 1440
detection_resolution = 320
fovX = 106
fovY = 74
input_method = KMBOX_NET
kmbox_net_uuid = UNIT-DEVICE
backend = $configBackend
active_game = CS
[Games]
CS = 1.4,0.022,0.022
"@ | Set-Content -LiteralPath $configPath -Encoding utf8

    $decisionPath = Join-Path $dataRoot 'active_profile_protocol_decision.txt'
    @"
ProtocolPassed=1
Identity=$Backend|0123456789ab|r64
Files=9
Runs=3
Trials=360
RequiredCounts=16,32,64
AxisXPixelsPerCount=0.515625
AxisYPixelsPerCount=0.5
AxisXT50Ms=14.1322
AxisYT50Ms=14.0608
AxisXT90Ms=14.50823
AxisYT90Ms=14.49659
Recommendation=MANUAL_REVIEW_ONLY
ProfileAutoWrite=0
Issues=
"@ | Set-Content -LiteralPath $decisionPath -Encoding ascii

    $rows = [System.Collections.Generic.List[string]]::new()
    $rows.Add('"Run","Counts","Axis","PositiveTrials","NegativeTrials","Passed"')
    foreach ($run in 1..3) {
        foreach ($counts in @(16, 32, 64)) {
            foreach ($axis in @('x', 'y')) {
                $rows.Add(('"{0}","{1}","{2}","10","10","True"' -f $run, $counts, $axis))
            }
        }
    }
    $rows | Set-Content -LiteralPath `
        (Join-Path $dataRoot 'active_profile_protocol_summary.csv') -Encoding utf8

    $configHashBefore = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    $arguments = @(
        '--config', $configPath,
        '--data-root', $dataRoot,
        '--output', $candidatePath,
        '--aim-mode', 'hipfire',
        '--probe-roi-x', '120',
        '--probe-roi-y', '100',
        '--probe-roi-width', '80',
        '--probe-roi-height', '80',
        '--confirm-manual-review', 'YES'
    )
    $result = (& $CandidateExe @arguments 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "Candidate generation failed: $result" }
    if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf) -or
        $result -notmatch 'CandidateCreated=1' -or
        $result -notmatch 'CandidateEnabled=0' -or
        $result -notmatch 'ReverseLoadLevel=3' -or
        $result -notmatch 'CalibratedResponseEnabled=0' -or
        $result -notmatch 'InvalidationAuditFields=21' -or
        $result -notmatch 'InvalidationAuditPassed=1' -or
        $result -notmatch 'RuntimeKeyRoi=1120,560,320,320' -or
        $result -notmatch 'ProbeEvidenceRoi=120,100,80,80') {
        throw "Candidate output contract failed: $result"
    }
    $candidate = @{}
    foreach ($line in Get-Content -LiteralPath $candidatePath) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $parts = $line.Split('=', 2)
        $candidate[$parts[0]] = $parts[1]
    }
    if ([int]$candidate.ProbeRoiX -ne 120 -or
        [int]$candidate.RoiX -ne 1120 -or
        [math]::Abs([double]$candidate.DegreesPerCountX - 0.0308) -gt 1e-12 -or
        [int]$candidate.TrialCount -ne 360) {
        throw 'Candidate schema did not preserve runtime key and probe evidence.'
    }
    $configHashAfter = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
    if ($configHashAfter -ne $configHashBefore) { throw 'Generator modified config.ini.' }

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $CandidateExe @arguments *> $null
        $overwriteExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($overwriteExitCode -eq 0) { throw 'Generator overwrote an existing candidate.' }

    (Get-Content -LiteralPath $decisionPath -Raw).Replace(
        'ProtocolPassed=1', 'ProtocolPassed=0') |
        Set-Content -LiteralPath $decisionPath -Encoding ascii
    $rejectedArguments = @($arguments)
    $outputIndex = [array]::IndexOf($rejectedArguments, '--output')
    $rejectedArguments[$outputIndex + 1] = $rejectedPath
    try {
        $ErrorActionPreference = 'Continue'
        & $CandidateExe @rejectedArguments *> $null
        $rejectedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($rejectedExitCode -eq 0 -or (Test-Path -LiteralPath $rejectedPath)) {
        throw 'Failed protocol gate created a candidate.'
    }
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
