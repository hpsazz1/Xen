param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$entry = Join-Path $RepositoryRoot 'scripts/invoke_dual_machine_receiver.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-rid-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
[IO.Directory]::CreateDirectory((Join-Path $package 'scripts')) | Out-Null
Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'scripts/benchmark_network_receiver.ps1') -Destination (Join-Path $package 'scripts')
[IO.File]::WriteAllText((Join-Path $package 'model.onnx'), '仅Prepare入口的占位模型，禁止加载')
@{
    schema = 1; complete = $true; package_type = 'xen-dual-machine-receiver'
    package_id = 'synthetic-run-id'; allowed_capture_backends = @('xudp_jpeg')
    allowed_backends = @('cpu'); model = @{ relative_path = 'model.onnx' }
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Encoding UTF8

# 只冻结生产入口用于显示的秒级时间；运行身份仍由正式脚本生成。
function Get-Date {
    param([string]$Format)
    if ($Format -ne 'yyyyMMdd-HHmmss') { throw "意外的时间调用：$Format" }
    return '20261011-120000'
}

$failures = 0
try {
    foreach ($case in @('same-parameters', 'changed-parameters')) {
        $reports = Join-Path $root "$case/reports"
        $runs = Join-Path $root "$case/runs"
        $ids = @(); $ready = @(); $prefixes = @()
        try {
            for ($index = 0; $index -lt 2; ++$index) {
                $prediction = if ($case -eq 'changed-parameters' -and $index -eq 1) { 'on' } else { 'off' }
                $output = @(& $entry -Scenario GeometryStatic -Backend cpu -Mode Prepare -PackageRoot $package -ReportRoot $reports -RunRoot $runs -AimPrediction $prediction 6>&1)
                foreach ($line in $output) {
                    $text = [string]$line
                    if ($text -match '^\s*run_id=(.+)$') { $ids += $Matches[1] }
                    if ($text -match '^\s*ready_file=(.+)$') { $ready += $Matches[1] }
                    if ($text -match '^\s*report_prefix=(.+)$') { $prefixes += $Matches[1] }
                }
                $config = [IO.File]::ReadAllText($prefixes[$index] + '.receiver.ini')
                $expectedPrediction = if ($prediction -eq 'on') { 'true' } else { 'false' }
                if ($config -notmatch '(?m)^allow_send_input=false\r?$' -or
                    $config -notmatch "(?m)^enable_prediction=$expectedPrediction\r?`$" -or
                    [IO.File]::Exists($ready[$index])) { throw 'Prepare配置或禁输出边界不正确' }
            }
            $configs = @(Get-ChildItem -LiteralPath $reports -Filter '*.receiver.ini' -File)
            if (@($ids | Select-Object -Unique).Count -ne 2 -or
                @($ready | Select-Object -Unique).Count -ne 2 -or
                @($prefixes | Select-Object -Unique).Count -ne 2 -or $configs.Count -ne 2) {
                throw "同秒运行共享目标：ids=$($ids -join ',') configs=$($configs.Count)"
            }
            Write-Host "${case}: 两次Prepare、两个ID、两份禁输出配置及两个ready目标通过"
        } catch { ++$failures; Write-Host "${case}: FAIL $($_.Exception.Message)" }
    }
    $providerRejected = $false
    try {
        & $entry -Scenario GeometryStatic -Backend cuda -Mode Prepare -PackageRoot $package -ReportRoot (Join-Path $root 'rejected/reports') -RunRoot (Join-Path $root 'rejected/runs')
    } catch { $providerRejected = $_.Exception.Message -match '不允许 Provider' }
    if (-not $providerRejected -or (Test-Path -LiteralPath (Join-Path $root 'rejected'))) {
        ++$failures; Write-Host '严格Provider拒绝失败'
    } else { Write-Host '严格Provider拒绝通过，未生成运行目录' }
    if ($failures) { throw "$failures 个实际接收Prepare案例失败" }
} finally {
    # 保留少量本机合成输入与输出；未执行任何二进制或设备操作。
    Write-Host "夹具目录：$root"
}
