param(
    [Parameter(Mandatory = $true)][string]$PrepareScript,
    [Parameter(Mandatory = $true)][string]$ToolRoot,
    [Parameter(Mandatory = $true)][string]$TestRoot
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PrepareScript) "probe_ndi_runtime_support.ps1")

$caseRoot = Join-Path ([IO.Path]::GetFullPath($TestRoot)) ("ndi-contract-" + [guid]::NewGuid().ToString("N"))
[void](New-Item -ItemType Directory -Path $caseRoot)
$arguments = @{}
foreach ($parameter in (Get-Command $PrepareScript).Parameters.Values) {
    $mandatory = @($parameter.Attributes | Where-Object { $_ -is [Management.Automation.ParameterAttribute] -and $_.Mandatory })
    if ($mandatory.Count -eq 0) { continue }
    $values = @($parameter.Attributes | Where-Object { $_ -is [Management.Automation.ValidateSetAttribute] })
    $arguments[$parameter.Name] = if ($values.Count) { $values[0].ValidValues[0] } else { Join-Path $caseRoot $parameter.Name }
}
$arguments.RunDirectory = Join-Path $caseRoot "must-not-create-run"
$arguments.PublishedRunDirectory = $arguments.RunDirectory

function Assert-PrepareRejected([string]$Root, [string]$Reason) {
    $arguments.ToolRoot = $Root
    $before = @(Get-ChildItem -LiteralPath $caseRoot -Force | ForEach-Object { $_.Name }) -join '|'
    $caught = $false
    try { & $PrepareScript @arguments | Out-Null }
    catch {
        if (-not $_.Exception.Message.Contains($Reason)) { throw }
        $caught = $true
    }
    if (-not $caught) { throw "Prepare accepted an invalid NDI capability" }
    $after = @(Get-ChildItem -LiteralPath $caseRoot -Force | ForEach-Object { $_.Name }) -join '|'
    if ($after -ne $before -or (Test-Path -LiteralPath $arguments.RunDirectory)) {
        throw "Rejected Prepare created a staging/task side effect"
    }
}

function Write-Deployment([string]$Root, [object[]]$Files) {
    $value = @{ schema = 1; configuration = "Release"; output_directory = $Root; files = @($Files) }
    [IO.File]::WriteAllText((Join-Path $Root "xen-runtime-deployment.json"),
        ($value | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
}

# 仅临时夹具中的字节身份，不向真实构建目录补任何 DLL，也不运行这些假文件。
$fixture = Join-Path $caseRoot "runtime-fixture"
[void](New-Item -ItemType Directory -Path $fixture)
Write-Deployment $fixture @()
Assert-PrepareRejected $fixture "UNSUPPORTED_NDI:"
$entries = @()
foreach ($name in @("Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")) {
    $path = Join-Path $fixture $name
    [IO.File]::WriteAllText($path, "isolated identity fixture " + $name)
    $entries += @{ name = $name; source = $path; sha256 = (Get-XenProbeRuntimeSha256 $path) }
}
# 报告未授权但磁盘遗留 NDI 必须报不一致，不能误判 UNSUPPORTED。
Assert-PrepareRejected $fixture "INVALID_NDI_DEPLOYMENT:"
Write-Deployment $fixture $entries
Assert-XenProbeNdiRuntime $fixture
$moved = Join-Path $caseRoot "relocated-runtime"
[void](New-Item -ItemType Directory -Path $moved)
foreach ($name in @("xen-runtime-deployment.json", "Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")) {
    Copy-Item -LiteralPath (Join-Path $fixture $name) -Destination (Join-Path $moved $name)
}
# 保留报告中的原始 output_directory，复现正式发布器原样携带来源报告的合同。
Assert-XenProbeNdiRuntime $moved
foreach ($name in @("Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")) {
    $path = Join-Path $fixture $name
    $bytes = [IO.File]::ReadAllBytes($path)
    Remove-Item -LiteralPath $path
    Assert-PrepareRejected $fixture "INVALID_NDI_DEPLOYMENT:"
    [IO.File]::WriteAllBytes($path, $bytes)
    [IO.File]::AppendAllText($path, "wrong hash")
    Assert-PrepareRejected $fixture "INVALID_NDI_DEPLOYMENT:"
    [IO.File]::WriteAllBytes($path, $bytes)
}

$supported = $true
try { Assert-XenProbeNdiRuntime $ToolRoot }
catch {
    if (-not $_.Exception.Message.Contains("UNSUPPORTED_NDI:")) { throw }
    $supported = $false
}
if (-not $supported) {
    # 无 NDI 分支仍真实调用本 Prepare；它必须在所有输入/输出准备之前明确拒绝。
    Assert-PrepareRejected $ToolRoot "UNSUPPORTED_NDI:"
}
return $supported
