param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$network = Join-Path $RepositoryRoot 'scripts/benchmark_network_receiver.ps1'
$runtime = Join-Path $RepositoryRoot 'scripts/benchmark_runtime.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-output-owner-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

function Find-Line([string]$Path, [string]$Text) {
    $lines = [IO.File]::ReadAllLines($Path)
    for ($index = 0; $index -lt $lines.Length; ++$index) {
        if ($lines[$index].Trim() -eq $Text) { return $index + 1 }
    }
    throw "未找到实际脚本的确定性交错边界：$Text"
}

$failed = 0
try {
    foreach ($case in @('outer-collision', 'outer-pending-failure', 'runtime-pending-failure', 'own-pending-control')) {
        $directory = Join-Path $root $case
        $build = Join-Path $directory 'build'
        [IO.Directory]::CreateDirectory((Join-Path $build 'Release')) | Out-Null
        $model = Join-Path $directory 'model.onnx'
        [IO.File]::WriteAllText($model, '不执行模型，只验证正式脚本拒绝与文件所有权')
        [IO.File]::WriteAllText((Join-Path $build 'CMakeCache.txt'), 'synthetic ownership fixture')
        # 不是可执行PE；正式入口必须在运行它之前因既有目标或缺少DLL拒绝。
        [IO.File]::WriteAllText((Join-Path $build 'Release/XenBenchmark.exe'), 'not-an-executable')
        $prefix = Join-Path $directory 'same-target'
        $ready = "$prefix.ready.json"
        $global:XenOwnershipFixture = @{
            Hits = 0; Prefix = $prefix; Ready = $ready; Expected = @{}; OwnPending = @()
            OtherWriter = $case -ne 'own-pending-control'
            IncludeReports = $case -like 'outer-*'
            PendingBoundary = $case -ne 'outer-collision'
        }
        $entry = if ($case -eq 'runtime-pending-failure') { $runtime } else { $network }
        $breakScript = if ($case -eq 'outer-collision') { $network } else { $runtime }
        $breakText = if ($case -eq 'outer-collision') {
            '$benchmarkScript = Join-Path $PSScriptRoot "benchmark_runtime.ps1"'
        } else { '$modelBefore = Get-FileSnapshot $ModelPath' }
        $breakpoint = Set-PSBreakpoint -Script $breakScript -Line (Find-Line $breakScript $breakText) -Action {
            $fixture = $global:XenOwnershipFixture
            ++$fixture.Hits
            if ($fixture.PendingBoundary) {
                $fixture.OwnPending = @($pendingArtifacts | Where-Object { $_ -ne $fixture.Ready })
                foreach ($path in $fixture.OwnPending) { [IO.File]::WriteAllText($path, 'B自己的未发布片段') }
            }
            if ($fixture.OtherWriter) {
                $paths = @($fixture.Ready)
                if ($fixture.IncludeReports) {
                    $paths += @('.csv', '.json', '.environment.json', '.provider-profile.json', '.network.json') |
                        ForEach-Object { $fixture.Prefix + $_ }
                }
                foreach ($path in $paths) {
                    $value = '另一写入者A的原始字节：' + [IO.Path]::GetFileName($path)
                    $pending = $path + '.writer-a-pending'
                    [IO.File]::WriteAllText($pending, $value, [Text.UTF8Encoding]::new($false))
                    [IO.File]::Move($pending, $path)
                    $fixture.Expected[$path] = [Convert]::ToBase64String([IO.File]::ReadAllBytes($path))
                }
            }
        }
        $errorText = ''
        try {
            & $entry -ModelPath $model -ReportPrefix $prefix -BuildDirectory $build -Backend cpu -ReadyFilePath $ready
        } catch { $errorText = $_.Exception.Message }
        finally { Remove-PSBreakpoint -Breakpoint $breakpoint }
        $fixture = $global:XenOwnershipFixture
        $correctFailure = if ($case -eq 'outer-collision') { $errorText -match '目标已存在' } else { $errorText -match '没有部署任何第三方 DLL' }
        $preserved = $true
        foreach ($path in $fixture.Expected.Keys) {
            if (-not [IO.File]::Exists($path) -or
                [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -ne $fixture.Expected[$path]) { $preserved = $false }
        }
        $pendingClean = @($fixture.OwnPending | Where-Object { [IO.File]::Exists($_) }).Count -eq 0
        $ownedCreated = -not $fixture.PendingBoundary -or $fixture.OwnPending.Count -eq 3
        Write-Host "${case}: boundary=$($fixture.Hits) rejected=$correctFailure preserved=$preserved pending_clean=$pendingClean own_created=$ownedCreated"
        if ($fixture.Hits -ne 1 -or -not $correctFailure -or -not $preserved -or -not $pendingClean -or -not $ownedCreated) {
            ++$failed
            Write-Host "实际失败信息：$errorText"
        }
    }
    if ($failed) { throw "$failed 个实际脚本所有权案例失败；夹具证据：$root" }
    Write-Host '两层正式脚本所有权案例通过；未调用有效EXE、采集、网络发送或真实设备。'
} finally {
    Remove-Variable -Name XenOwnershipFixture -Scope Global -ErrorAction SilentlyContinue
    # 保留本轮少量合成夹具，便于直接核验丢失或保留的文件，不执行递归删除。
    Write-Host "夹具目录：$root"
}
