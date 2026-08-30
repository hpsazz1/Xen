$ErrorActionPreference = "Stop"

function Get-XenRuntimeSequenceValues {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$JsonSamples,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$CsvRows
    )

    if ($CsvRows.Count -ne $JsonSamples.Count) {
        throw "CSV 正式样本行数与 JSON sample_count 不一致。"
    }

    # 固定长度数组让 10 万级报告只进行一次顺序写入；禁止在循环内重新
    # 物化 JsonSamples，否则总复制量会随样本数平方增长。
    $sequences = [uint64[]]::new($CsvRows.Count)
    for ($index = 0; $index -lt $CsvRows.Count; ++$index) {
        [uint64]$csvSequence = 0
        if (-not [uint64]::TryParse(
                [string]$CsvRows[$index].sequence,
                [ref]$csvSequence)) {
            throw "CSV 第 $index 行缺少合法 sequence。"
        }
        try {
            $jsonSequence = [uint64]$JsonSamples[$index].sequence
        } catch {
            throw "JSON 第 $index 个正式样本缺少合法 sequence。"
        }
        if ($csvSequence -ne $jsonSequence) {
            throw "CSV 与 JSON 第 $index 个正式样本 sequence 不一致。"
        }
        $sequences[$index] = $csvSequence
    }
    return ,$sequences
}

function Get-XenRuntimeReportRetention {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Report,
        [ValidateRange(1, 1000000)]
        [uint64]$RetentionCapacity = 100000
    )

    # Windows PowerShell 5 会把 [uint64]$null 转成 0；必须先检查属性存在，
    # 不能让缺失的 dropped 字段伪装成完整短报告。
    $reportFields = @($Report.PSObject.Properties.Name)
    $requiredFields = @(
        "sample_count", "successful_samples", "failed_samples",
        "report_samples_dropped", "coverage")
    foreach ($field in $requiredFields) {
        if ($reportFields -notcontains $field -or
            $null -eq $Report.$field) {
            throw "Runtime 报告缺少非空计数字段：$field"
        }
    }
    if ($null -eq $Report.coverage.formal -or
        $null -eq $Report.coverage.formal.sample_count) {
        throw "Runtime 报告缺少非空 coverage.formal.sample_count。"
    }

    # schema 17 的 sample_count/timing/samples 都只描述当前留样；formal
    # 全程总数由 coverage 给出，report_samples_dropped 只表示报告省略。
    try {
        [uint64]$formalSampleCount = $Report.coverage.formal.sample_count
        [uint64]$retainedSampleCount = $Report.sample_count
        [uint64]$successfulSamples = $Report.successful_samples
        [uint64]$failedSamples = $Report.failed_samples
        [uint64]$omittedSampleCount = $Report.report_samples_dropped
    } catch {
        throw "Runtime 报告缺少合法的 formal/留样计数字段。"
    }
    if ($formalSampleCount -eq 0) {
        throw "Runtime 报告 formal 成功样本总数必须大于零。"
    }
    if ($retainedSampleCount -gt $RetentionCapacity) {
        throw "Runtime 报告留样数超过固定容量。"
    }
    if ($successfulSamples -gt [uint64]::MaxValue - $failedSamples -or
        $successfulSamples + $failedSamples -ne $retainedSampleCount) {
        throw "Runtime 报告留样成功/失败计数不守恒。"
    }
    if ($retainedSampleCount -gt
            [uint64]::MaxValue - $omittedSampleCount -or
        $retainedSampleCount + $omittedSampleCount -ne
            $formalSampleCount) {
        throw "Runtime 报告 formal 总数、留样数和省略数不守恒。"
    }
    if ($omittedSampleCount -gt 0 -and
        $retainedSampleCount -ne $RetentionCapacity) {
        throw "Runtime 长报告发生省略时必须填满固定留样尾窗。"
    }

    return [pscustomobject][ordered]@{
        formal_sample_count = $formalSampleCount
        retained_sample_count = $retainedSampleCount
        omitted_sample_count = $omittedSampleCount
        retention_capacity = $RetentionCapacity
        retention_policy = if ($omittedSampleCount -eq 0) {
            "complete"
        } else {
            "tail"
        }
        summary_scope = if ($omittedSampleCount -eq 0) {
            "all_formal_samples"
        } else {
            "retained_tail_samples"
        }
    }
}

function Get-XenRuntimeCsvRetentionMetadata {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Lines
    )

    $prefix = "# report_samples_dropped,"
    [int]$matches = 0
    [uint64]$omittedSampleCount = 0
    foreach ($line in $Lines) {
        if (-not $line.StartsWith(
                $prefix, [System.StringComparison]::Ordinal)) {
            continue
        }
        ++$matches
        [uint64]$parsed = 0
        if (-not [uint64]::TryParse(
                $line.Substring($prefix.Length), [ref]$parsed)) {
            throw "CSV report_samples_dropped 不是合法 uint64。"
        }
        $omittedSampleCount = $parsed
    }
    if ($matches -ne 1) {
        throw "CSV 必须且只能包含一条 report_samples_dropped 元数据。"
    }

    return [pscustomobject][ordered]@{
        omitted_sample_count = $omittedSampleCount
    }
}

function Get-XenRuntimeRetainedSequenceEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [uint64[]]$Sequences,
        [Parameter(Mandatory = $true)]
        [uint64]$PreviousFormalSequence,
        [Parameter(Mandatory = $true)]
        [uint64]$OmittedSampleCount
    )

    if ($Sequences.Count -eq 0) {
        throw "Runtime 报告 formal 留样 sequence 集合为空。"
    }
    # 尾窗首项之前的 sequence 跳变同时包含实际丢帧和已省略的成功样本；
    # 扣除后者，剩余值才能与全程 coverage.sequence_gaps 交叉核对。
    [uint64]$sequenceHoles = 0
    [uint64]$previous = $PreviousFormalSequence
    foreach ($sequence in $Sequences) {
        if ($sequence -le $previous) {
            throw "Runtime 报告 formal 留样 sequence 不是严格递增。"
        }
        $sequenceHoles += $sequence - $previous - 1
        $previous = $sequence
    }
    if ($sequenceHoles -lt $OmittedSampleCount) {
        throw "Runtime 报告 sequence 缺口不足以解释省略的 formal 样本。"
    }

    return [pscustomobject][ordered]@{
        retained_sample_count = [uint64]$Sequences.Count
        first_sequence = [uint64]$Sequences[0]
        last_sequence = [uint64]$Sequences[$Sequences.Count - 1]
        formal_sequence_gaps = $sequenceHoles - $OmittedSampleCount
    }
}
