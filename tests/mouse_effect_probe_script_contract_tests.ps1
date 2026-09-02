param(
    [Parameter(Mandatory = $true)]
    [string]$PrepareScript,
    [Parameter(Mandatory = $true)]
    [string]$LaunchScript
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-Contains(
        [string]$Content,
        [string]$Expected,
        [string]$Description) {
    if (-not $Content.Contains($Expected)) {
        throw "$Description is missing its required text."
    }
}

function ConvertFrom-Utf8Base64([string]$Value) {
    return [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String($Value))
}

foreach ($path in @($PrepareScript, $LaunchScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Script does not exist: $path"
    }
}

$prepare = Get-Content -LiteralPath $PrepareScript -Raw -Encoding utf8
$launch = Get-Content -LiteralPath $LaunchScript -Raw -Encoding utf8

Assert-Contains $prepare (ConvertFrom-Utf8Base64 `
    "5omn6KGM5ZG95Luk5pe25L+d5oyB5Y+z6ZSu5p2+5byA") `
    "Physical A TASK safety protocol"
Assert-Contains $prepare (ConvertFrom-Utf8Base64 `
    "55yL5YiwIHByb2JlIOaPkOekuiBtb25pdG9yIOW3suWwsee7quWQjg==") `
    "Physical A TASK safety protocol"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    '44CQ5YeG5aSH44CR5L+d5oyB5Y+z6ZSu5p2+5byA77yb562J5b6F4oCc5oyJ5L2P5Y+z6ZSu4oCd44CC') `
    "Physical A concise prepare cue"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    '44CQ5oyJ5L2P5Y+z6ZSu44CRNSDnp5LlhoXmjInkvY/lubbmjIHnu63kv53mjIHvvJvnm7TliLDnnIvliLDigJznjrDlnKjmnb7lvIDlj7PplK7igJ3jgII=') `
    "Physical A concise hold cue"
Assert-Contains $prepare (ConvertFrom-Utf8Base64 `
    "5pe26Ze057q/5a6M5oiQ5oiW5pyq5q2j5bi45a6M5oiQ") `
    "Physical A TASK release terminal"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    '44CQ546w5Zyo5p2+5byA5Y+z6ZSu44CR5ZG95Luk6Zi25q615a6M5oiQ77yb5q2j5Zyo5pW055CG6K+B5o2u44CC') `
    "Physical A concise release cue"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    '44CQ6K6w5b2V5a6M5oiQ44CR5pys5qyh5bCa5pyq5YiG5p6Q5Y+v6KeB5pWI5p6c44CC') `
    "Physical A concise completion cue"
Assert-Contains $launch 'ConvertTo-PhysicalProbeOperatorCue' `
    "Physical A native-output cue mapper"
Assert-Contains $launch '2>&1 | ForEach-Object' `
    "Physical A native-output filtering pipeline"
Assert-Contains $launch '"safety-ledger.json"' `
    "Physical A safety ledger output"
Assert-Contains $launch '"--safety-ledger"' `
    "Physical A safety ledger argument"
Assert-Contains $launch "safety_ledger_sha256" `
    "Physical A launch summary safety ledger binding"
Assert-Contains $launch '$safetyLedger.schema_version -ne 2' `
    "Physical A safety ledger schema 2 gate"
Assert-Contains $launch 'monitor_packets' `
    "Physical A raw monitor packet identity input"
Assert-Contains $launch 'payload_sha256' `
    "Physical A raw monitor payload digest validation"
Assert-Contains $launch 'source_endpoint_valid' `
    "Physical A raw monitor source endpoint validation"
Assert-Contains $launch 'monitor_sequence_before' `
    "Physical A raw monitor sequence-before validation"
Assert-Contains $launch 'monitor_sequence_after' `
    "Physical A raw monitor sequence-after validation"
Assert-Contains $launch 'safety_monitor_packet_identity_complete' `
    "Physical A launch summary packet identity verdict"

foreach ($verbose in @(
        (ConvertFrom-Utf8Base64 `
            '5Y2z5bCG5omn6KGMICRwcm9iZUxhYmVs'),
        (ConvertFrom-Utf8Base64 `
            'c2lkZWNhciDlt7LlvZXmu6Hlubbov5vlhaUgUE5HL+WTiOW4jCBwdWJsaXNoaW5n'),
        (ConvertFrom-Utf8Base64 `
            'UGh5c2ljYWwgQSDlt7LlrozmlbTorrDlvZXvvJpwdWxzZS9iYWNrZW5kL0FDSz0='),
        (ConvertFrom-Utf8Base64 `
            '6K+35p2+5byA5Y+z6ZSu77yM5bm25oqK5pa55ZCRL+WPr+ingeaApy/lvILluLjmiJbmgKXlgZzmg4XlhrXnm7TmjqXlj5Hlm57lvZPliY3lr7nor53jgII='))) {
    if ($launch.Contains($verbose)) {
        throw "Physical A Launch must not expose verbose operator text: $verbose"
    }
}

if ($prepare.Contains((ConvertFrom-Utf8Base64 `
        "6L+b5YWl6Z2Z5q2i44CB5peg5Lq654mpL092ZXJsYXkg55qE6auY5a+55q+U6IOM5pmv5ZCO5oyB57ut5oyJ5L2P5Y+z6ZSu")) -or
    $launch.Contains((ConvertFrom-Utf8Base64 `
        "6K+35L+d5oyB6Z2Z5q2i6IOM5pmv44CB5oyB57ut5oyJ5L2P5Y+z6ZSu"))) {
    throw "Physical A must not request an early right-button hold."
}

Write-Host "Mouse Effect Probe script safety contract passed."
