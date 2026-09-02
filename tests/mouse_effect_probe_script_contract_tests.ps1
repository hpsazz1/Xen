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
    "5ZCv5Yqo5ZG95Luk5pe26K+35YWI5L+d5oyB5Y+z6ZSu5p2+5byA") `
    "Physical A Launch safety protocol"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    "cHJvYmUg5o+Q56S6IG1vbml0b3Ig5bey5bCx57uq5ZCO") `
    "Physical A Launch safety protocol"
Assert-Contains $prepare (ConvertFrom-Utf8Base64 `
    "5pe26Ze057q/5a6M5oiQ5oiW5pyq5q2j5bi45a6M5oiQ") `
    "Physical A TASK release terminal"
Assert-Contains $launch (ConvertFrom-Utf8Base64 `
    "5pe26Ze057q/5a6M5oiQ5oiW5pyq5q2j5bi45a6M5oiQ") `
    "Physical A Launch release terminal"
Assert-Contains $launch '"safety-ledger.json"' `
    "Physical A safety ledger output"
Assert-Contains $launch '"--safety-ledger"' `
    "Physical A safety ledger argument"
Assert-Contains $launch "safety_ledger_sha256" `
    "Physical A launch summary safety ledger binding"

if ($prepare.Contains((ConvertFrom-Utf8Base64 `
        "6L+b5YWl6Z2Z5q2i44CB5peg5Lq654mpL092ZXJsYXkg55qE6auY5a+55q+U6IOM5pmv5ZCO5oyB57ut5oyJ5L2P5Y+z6ZSu")) -or
    $launch.Contains((ConvertFrom-Utf8Base64 `
        "6K+35L+d5oyB6Z2Z5q2i6IOM5pmv44CB5oyB57ut5oyJ5L2P5Y+z6ZSu"))) {
    throw "Physical A must not request an early right-button hold."
}

Write-Host "Mouse Effect Probe script safety contract passed."
