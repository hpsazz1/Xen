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
