param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$help = (& $Executable --help 2>&1) -join [Environment]::NewLine
if ($LASTEXITCODE -ne 0 -or
    $help -notmatch "output-off" -or
    $help -notmatch "physical_output_capability=false") {
    throw "帮助输出必须明确声明 output-off 且命令成功"
}

$savedErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$unknown = (& $Executable --unknown-option 2>&1) -join [Environment]::NewLine
$unknownExitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorActionPreference
if ($unknownExitCode -ne 2 -or $unknown -notmatch "未知") {
    throw "未知参数必须以用法错误退出"
}

$root = [IO.Path]::GetFullPath($TestRoot)
if ($root -match '^[A-Za-z]:\\?$' -or $root -eq '\') {
    throw "拒绝在文件系统根目录执行 Capture evidence CLI 测试"
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $output = Join-Path $root "must-not-exist"
    $ErrorActionPreference = "Continue"
    $failure = (& $Executable `
        --ndi-source "fixture" `
        --binding (Join-Path $root "missing-binding.json") `
        --output $output `
        --frames 1 2>&1) -join [Environment]::NewLine
    $failureExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    if ($failureExitCode -eq 0 -or
        (Test-Path -LiteralPath $output) -or
        $failure -notmatch "binding") {
        throw "缺失 binding 时必须在连接 NDI 前失败且不得创建最终目录"
    }
    Write-Host "Capture evidence CLI 安全边界和失败封闭测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
