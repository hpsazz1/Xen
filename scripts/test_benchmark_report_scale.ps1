param(
    [ValidateRange(1000, 100000)]
    [int]$SyntheticSampleCount = 72002,
    [ValidateRange(1000, 10000)]
    [int]$LegacyProbeCount = 5000,
    [ValidateRange(2.0, 1000000.0)]
    [double]$MinimumSpeedup = 20.0,
    [ValidateRange(100.0, 60000.0)]
    [double]$MaximumFullScanMilliseconds = 5000.0,
    [string]$RecordedCsvPath = "",
    [string]$ExpectedRecordedCsvSha256 = "",
    [ValidateRange(1, 100000)]
    [int]$ExpectedRecordedSampleCount = 72002,
    [uint64]$RecordedPreviousSequence = 115,
    [uint64]$ExpectedRecordedSequenceGaps = 2,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "runtime_report_sequence.ps1")

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-SequenceEvidence {
    param(
        [Parameter(Mandatory = $true)][uint64[]]$Sequences,
        [Parameter(Mandatory = $true)][uint64]$PreviousSequence
    )

    if ($Sequences.Count -eq 0) {
        throw "正式 sequence 集合为空。"
    }
    [uint64]$gaps = 0
    [uint64]$previous = $PreviousSequence
    foreach ($sequence in $Sequences) {
        if ($sequence -le $previous) {
            throw "正式 CSV sequence 不是严格递增。"
        }
        $gaps += $sequence - $previous - 1
        $previous = $sequence
    }
    return [ordered]@{
        sample_count = $Sequences.Count
        first_sequence = [uint64]$Sequences[0]
        last_sequence = [uint64]$Sequences[$Sequences.Count - 1]
        sequence_gaps = $gaps
    }
}

function Get-LegacySequenceValues {
    param(
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][object[]]$CsvRows
    )

    if ($CsvRows.Count -ne @($Report.samples).Count) {
        throw "CSV 正式样本行数与 JSON sample_count 不一致。"
    }
    $sequences = [uint64[]]::new($CsvRows.Count)
    for ($index = 0; $index -lt $CsvRows.Count; ++$index) {
        [uint64]$csvSequence = 0
        if (-not [uint64]::TryParse(
                [string]$CsvRows[$index].sequence,
                [ref]$csvSequence)) {
            throw "CSV 第 $index 行缺少合法 sequence。"
        }
        $jsonSequence = [uint64](@($Report.samples)[$index].sequence)
        if ($csvSequence -ne $jsonSequence) {
            throw "CSV 与 JSON 第 $index 个正式样本 sequence 不一致。"
        }
        $sequences[$index] = $csvSequence
    }
    return ,$sequences
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString(
            $sha256.ComputeHash($bytes))).Replace("-", "")
    } finally {
        $sha256.Dispose()
    }
}

function Get-RejectionMessage {
    param([Parameter(Mandatory = $true)][scriptblock]$Action)
    try {
        & $Action | Out-Null
        return ""
    } catch {
        return $_.Exception.Message
    }
}

$jsonSamples = [object[]]::new($SyntheticSampleCount)
$csvRows = [object[]]::new($SyntheticSampleCount)
$firstGapIndex = [Math]::Max(1, [int]($SyntheticSampleCount / 3))
$secondGapIndex = [Math]::Max(
    $firstGapIndex + 1, [int](2 * $SyntheticSampleCount / 3))
[uint64]$sequence = 115
for ($index = 0; $index -lt $SyntheticSampleCount; ++$index) {
    ++$sequence
    if ($index -eq $firstGapIndex -or $index -eq $secondGapIndex) {
        ++$sequence
    }
    $jsonSamples[$index] = [pscustomobject]@{ sequence = $sequence }
    $csvRows[$index] = [pscustomobject]@{
        sequence = $sequence.ToString(
            [System.Globalization.CultureInfo]::InvariantCulture)
    }
}
$report = [pscustomobject]@{ samples = $jsonSamples }

$fullWatch = [System.Diagnostics.Stopwatch]::StartNew()
[uint64[]]$fullValues = Get-XenRuntimeSequenceValues `
    -JsonSamples $jsonSamples -CsvRows $csvRows
$fullEvidence = Get-SequenceEvidence $fullValues 115
$fullWatch.Stop()
Assert-True ($fullEvidence.sample_count -eq $SyntheticSampleCount -and
        $fullEvidence.sequence_gaps -eq 2) `
    "72k 合成报告的样本数或 sequence gap 不符合预期。"
Assert-True ($fullWatch.Elapsed.TotalMilliseconds -le
        $MaximumFullScanMilliseconds) `
    ("缓存扫描耗时超限：actual={0:F3} ms, limit={1:F3} ms" -f
        $fullWatch.Elapsed.TotalMilliseconds,
        $MaximumFullScanMilliseconds)

$probeCount = [Math]::Min($LegacyProbeCount, $SyntheticSampleCount)
$probeJson = [object[]]::new($probeCount)
$probeCsv = [object[]]::new($probeCount)
[Array]::Copy($jsonSamples, $probeJson, $probeCount)
[Array]::Copy($csvRows, $probeCsv, $probeCount)
$probeReport = [pscustomobject]@{ samples = $probeJson }

$legacyWatch = [System.Diagnostics.Stopwatch]::StartNew()
[uint64[]]$legacyValues = Get-LegacySequenceValues $probeReport $probeCsv
$legacyEvidence = Get-SequenceEvidence $legacyValues 115
$legacyWatch.Stop()

$cachedWatch = [System.Diagnostics.Stopwatch]::StartNew()
[uint64[]]$cachedValues = Get-XenRuntimeSequenceValues $probeJson $probeCsv
$cachedEvidence = Get-SequenceEvidence $cachedValues 115
$cachedWatch.Stop()

$legacyEvidenceText = $legacyEvidence | ConvertTo-Json -Compress
$cachedEvidenceText = $cachedEvidence | ConvertTo-Json -Compress
$legacyEvidenceHash = Get-TextSha256 $legacyEvidenceText
$cachedEvidenceHash = Get-TextSha256 $cachedEvidenceText
$speedup = $legacyWatch.Elapsed.TotalMilliseconds /
    [Math]::Max($cachedWatch.Elapsed.TotalMilliseconds, 0.001)
Assert-True ($legacyEvidenceText -eq $cachedEvidenceText -and
        $legacyEvidenceHash -eq $cachedEvidenceHash) `
    "旧/新扫描对同一有效输入的 coverage 证据或哈希不一致。"
Assert-True ($speedup -ge $MinimumSpeedup) `
    ("缓存扫描加速不足：actual={0:F2}x, required={1:F2}x" -f
        $speedup, $MinimumSpeedup)

$invalidCsv = [object[]]$probeCsv.Clone()
$invalidIndex = [int]($probeCount / 2)
$invalidCsv[$invalidIndex] = [pscustomobject]@{
    sequence = ([uint64]$probeJson[$invalidIndex].sequence + 1).ToString(
        [System.Globalization.CultureInfo]::InvariantCulture)
}
$legacyRejection = Get-RejectionMessage {
    Get-LegacySequenceValues $probeReport $invalidCsv
}
$cachedRejection = Get-RejectionMessage {
    Get-XenRuntimeSequenceValues $probeJson $invalidCsv
}
Assert-True (-not [string]::IsNullOrWhiteSpace($legacyRejection) -and
        $legacyRejection -eq $cachedRejection) `
    "旧/新扫描对同一无效输入的拒绝结果不一致。"

$recordedEvidence = $null
if (-not [string]::IsNullOrWhiteSpace($RecordedCsvPath)) {
    if (-not (Test-Path -LiteralPath $RecordedCsvPath -PathType Leaf)) {
        throw "固定记录 CSV 不存在：$RecordedCsvPath"
    }
    $recordedPath = (Resolve-Path -LiteralPath $RecordedCsvPath).ProviderPath
    $recordedHashBefore = (Get-FileHash -LiteralPath $recordedPath `
        -Algorithm SHA256).Hash
    if (-not [string]::IsNullOrWhiteSpace($ExpectedRecordedCsvSha256) -and
        $recordedHashBefore -ne $ExpectedRecordedCsvSha256) {
        throw "固定记录 CSV SHA-256 不符合预期。"
    }
    $recordedWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $recordedDataLines = @(Get-Content -LiteralPath $recordedPath `
        -Encoding UTF8 | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            -not $_.StartsWith(
                "#", [System.StringComparison]::Ordinal)
        })
    $recordedRows = @($recordedDataLines | ConvertFrom-Csv)
    $recordedJson = [object[]]::new($recordedRows.Count)
    for ($index = 0; $index -lt $recordedRows.Count; ++$index) {
        $recordedJson[$index] = [pscustomobject]@{
            sequence = [uint64]$recordedRows[$index].sequence
        }
    }
    [uint64[]]$recordedValues = Get-XenRuntimeSequenceValues `
        $recordedJson $recordedRows
    $recordedEvidence = Get-SequenceEvidence `
        $recordedValues $RecordedPreviousSequence
    $recordedWatch.Stop()
    $recordedHashAfter = (Get-FileHash -LiteralPath $recordedPath `
        -Algorithm SHA256).Hash
    Assert-True ($recordedEvidence.sample_count -eq
            $ExpectedRecordedSampleCount -and
            $recordedEvidence.sequence_gaps -eq
                $ExpectedRecordedSequenceGaps -and
            $recordedHashBefore -eq $recordedHashAfter) `
        "固定记录 CSV 的样本数、sequence gap 或前后哈希不一致。"
    $recordedEvidence.input_sha256 = $recordedHashAfter
    $recordedEvidence.input_bytes =
        (Get-Item -LiteralPath $recordedPath).Length
    $recordedEvidence.parse_and_scan_ms =
        $recordedWatch.Elapsed.TotalMilliseconds
}

$summary = [ordered]@{
    schema = 1
    synthetic = [ordered]@{
        sample_count = $SyntheticSampleCount
        full_scan_ms = $fullWatch.Elapsed.TotalMilliseconds
        sequence_gaps = $fullEvidence.sequence_gaps
        legacy_probe_count = $probeCount
        legacy_ms = $legacyWatch.Elapsed.TotalMilliseconds
        cached_ms = $cachedWatch.Elapsed.TotalMilliseconds
        speedup = $speedup
        accepted_evidence_sha256 = $cachedEvidenceHash
        rejection_message = $cachedRejection
    }
    recorded = $recordedEvidence
}
if (-not $Quiet) {
    Write-Host ($summary | ConvertTo-Json -Depth 5)
}
