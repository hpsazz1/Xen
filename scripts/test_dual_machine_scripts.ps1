param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot "..")
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) { throw $Message }
}

$scriptPaths = @(
    "scripts/benchmark_runtime.ps1",
    "scripts/benchmark_network_receiver.ps1",
    "scripts/invoke_dual_machine_receiver.ps1",
    "scripts/publish_dual_machine_package.ps1",
    "scripts/runtime_environment.ps1"
) | ForEach-Object { Join-Path $RepositoryRoot $_ }

foreach ($path in $scriptPaths) {
    $tokens = $null
    $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $path, [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) `
        "PowerShell 语法解析失败：$path"
}

. (Join-Path $RepositoryRoot "scripts/runtime_environment.ps1")
$successQuery = {
    param([string]$ClassName)
    if ($ClassName -eq "Win32_VideoController") {
        return [pscustomobject]@{
            Name = "Test GPU"
            DriverVersion = "1.2.3"
            AdapterRAM = 4096
        }
    }
    return [pscustomobject]@{ Caption = "Test Windows" }
}
$complete = Get-XenRuntimeHardwareInventory -CimQuery $successQuery
Assert-True ($complete.inventory_status -eq "complete") `
    "正常 WMI 查询必须生成 complete 硬件清单。"
Assert-True ($complete.os -eq "Test Windows" -and
    @($complete.gpu).Count -eq 1 -and
    $complete.gpu[0].name -eq "Test GPU") `
    "正常 WMI 查询的 OS/GPU 字段不正确。"

$failureQuery = {
    param([string]$ClassName)
    throw "模拟非管理员拒绝：$ClassName"
}
$partial = Get-XenRuntimeHardwareInventory -CimQuery $failureQuery
Assert-True ($partial.inventory_status -eq "partial") `
    "WMI 拒绝必须生成 partial 硬件清单。"
Assert-True (-not [string]::IsNullOrWhiteSpace($partial.os) -and
    @($partial.gpu).Count -eq 0 -and
    @($partial.inventory_errors).Count -ge 2) `
    "WMI 拒绝必须保留 OS 回退和明确错误。"

$invokeScript = Join-Path $RepositoryRoot `
    "scripts/invoke_dual_machine_receiver.ps1"
$previousLocation = Get-Location
$previousErrorActionPreference = $ErrorActionPreference
try {
    Set-Location (Join-Path $env:SystemRoot "System32")
    # Windows PowerShell 5 会把子 PowerShell 的预期 stderr 包装为非终止错误；
    # 本回归按退出码和完整文本判断，不能让外层 Stop 提前截断证据。
    $ErrorActionPreference = "Continue"
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File $invokeScript -Scenario GeometryStatic -Mode Prepare 2>&1)
    $exitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
    Set-Location $previousLocation
}
$outputText = $output -join "`n"
Assert-True ($exitCode -ne 0 -and
    $outputText -match "便携包清单不存在" -and
    $outputText -notmatch "Join-Path") `
    "任意工作目录启动必须在脚本体内解析默认 PackageRoot。"

Write-Host "双机 PowerShell 脚本回归通过。"
