param(
    [Parameter(Mandatory = $true)][string]$LeaseExecutable,
    [Parameter(Mandatory = $true)][string]$TestRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\scripts\path_safety.psm1') -Force
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$owned = New-XenOwnedTestDirectory -BasePath $TestRoot -RepositoryRoot $repository
$children = [Collections.Generic.List[Diagnostics.Process]]::new()

function Read-ChildMessage([Diagnostics.Process]$Child) {
    $pending = $Child.StandardOutput.ReadLineAsync()
    if (-not $pending.Wait(10000)) {
        throw "lease 子进程 IPC 超时，PID=$($Child.Id)"
    }
    $message = $pending.GetAwaiter().GetResult()
    if ($null -eq $message) {
        $details = $Child.StandardError.ReadToEnd()
        throw "lease 子进程未返回 IPC 消息：$details"
    }
    return $message
}

function Start-LeaseChild([string]$Scope, [string]$EnvironmentRoot,
                         [switch]$WaitAfterRefusal) {
    $tmpRoot = Join-Path $EnvironmentRoot 'tmp'
    $tempRoot = Join-Path $EnvironmentRoot 'temp'
    [IO.Directory]::CreateDirectory($tmpRoot) | Out-Null
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $LeaseExecutable
    $start.ArgumentList.Add($Scope)
    if ($WaitAfterRefusal) { $start.ArgumentList.Add('--wait-after-refusal') }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    # 同时替换两个进程环境变量；仅更换 TEMP 会被继承的 TMP 掩盖。
    $start.Environment['TMP'] = $tmpRoot
    $start.Environment['TEMP'] = $tempRoot
    $child = [Diagnostics.Process]::new()
    $child.StartInfo = $start
    if (-not $child.Start()) { throw '无法启动 lease-only 子进程' }
    $children.Add($child)
    return [pscustomobject]@{
        Process = $child
        Message = Read-ChildMessage $child
    }
}

function Assert-Held($Child, [string]$Context) {
    if ($Child.Message -cne 'HELD') {
        $Child.Process.WaitForExit(10000) | Out-Null
        $details = $Child.Process.StandardError.ReadToEnd()
        throw "环境未满足：$Context 无法先建立持锁状态；保留外部 owner，不关闭它。$details"
    }
}

function Assert-Refused($Child, [string]$Context, [switch]$KeepAlive) {
    if ($Child.Message -cne 'REFUSED') {
        throw "$Context：第一方已通过 IPC 证明持锁，但第二方仍取得 PRODUCTION lease"
    }
    if ($KeepAlive) {
        if ($Child.Process.HasExited) { throw "$Context：拒绝方必须保持存活以验证回滚" }
        return
    }
    if (-not $Child.Process.WaitForExit(10000) -or
        $Child.Process.ExitCode -ne 3) {
        throw "$Context：竞争拒绝没有通过公开 acquire/held 合同"
    }
}

function Release-Child($Child, [switch]$OtherThread) {
    $command = if ($OtherThread) { 'release-other-thread' } else { 'release' }
    $Child.Process.StandardInput.WriteLine($command)
    $Child.Process.StandardInput.Flush()
    if ((Read-ChildMessage $Child.Process) -cne 'RELEASED' -or
        -not $Child.Process.WaitForExit(10000) -or
        $Child.Process.ExitCode -ne 0) {
        throw 'lease 子进程释放或退出失败'
    }
}

try {
    $environmentA = Join-Path $owned.RootPath 'environment-a'
    $environmentB = Join-Path $owned.RootPath 'environment-b'

    $sameOwner = Start-LeaseChild 'production' $environmentA
    Assert-Held $sameOwner '同目录对照 owner'
    $legacyPath = Join-Path $environmentA 'tmp\Xen-mouse-output-owner-v1.lock'
    $legacyProbe = $null
    $legacyRefused = $false
    try {
        $legacyProbe = [IO.File]::Open($legacyPath, [IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
        # ERROR_SHARING_VIOLATION；不能把其他 I/O 故障当作新版保留 legacy 锁。
        $failure = $_.Exception
        while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
        if ($failure -is [IO.IOException] -and
            ($failure.HResult -band 0xFFFF) -eq 32) {
            $legacyRefused = $true
        } else {
            throw
        }
    } finally {
        if ($null -ne $legacyProbe) { $legacyProbe.Dispose() }
    }
    if (-not $legacyRefused) { throw '新版持有期间必须持续保留同目录旧版 v1 锁' }
    $sameContender = Start-LeaseChild 'production' $environmentA
    Assert-Refused $sameContender '同目录对照'
    Release-Child $sameOwner -OtherThread
    $afterRelease = Start-LeaseChild 'production' $environmentA
    Assert-Held $afterRelease '跨线程释放后的 owner'
    Release-Child $afterRelease
    Write-Output 'PASS 同目录互斥与跨线程释放后可重新获取'

    # 旧版已持有同一 TMP 下的 v1 文件时，新版仍必须尊重它；不运行旧设备代码。
    $legacyOwner = [IO.File]::Open($legacyPath, [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $legacyContender = Start-LeaseChild 'production' $environmentA -WaitAfterRefusal
        Assert-Refused $legacyContender '同目录旧版 v1 owner 兼容' -KeepAlive
        $afterLegacyFailure = Start-LeaseChild 'production' $environmentB
        Assert-Held $afterLegacyFailure '拒绝方仍存活时稳定锁必须已经回滚'
        if ($legacyContender.Process.HasExited) {
            throw '拒绝方提前退出，不能用 OS 清理代替 acquire 失败回滚证据'
        }
        Release-Child $afterLegacyFailure
        $legacyContender.Process.StandardInput.WriteLine('finish-refused')
        $legacyContender.Process.StandardInput.Flush()
        if ((Read-ChildMessage $legacyContender.Process) -cne 'REFUSAL_FINISHED' -or
            -not $legacyContender.Process.WaitForExit(10000) -or
            $legacyContender.Process.ExitCode -ne 3) {
            throw '兼容拒绝方未完成持活同步'
        }
    } finally {
        $legacyOwner.Dispose()
    }
    Write-Output 'PASS 同目录旧版 owner 冲突与拒绝方仍存活时的稳定锁回滚'

    $processOwner = Start-LeaseChild 'current-process-test' $environmentA
    Assert-Held $processOwner 'CURRENT_PROCESS_TEST owner'
    $processOwner.Process.StandardInput.WriteLine('check-same-process-conflict')
    $processOwner.Process.StandardInput.Flush()
    if ((Read-ChildMessage $processOwner.Process) -cne 'SAME_PROCESS_CONFLICT') {
        throw 'CURRENT_PROCESS_TEST 同进程独占失效'
    }
    $otherProcess = Start-LeaseChild 'current-process-test' $environmentA
    Assert-Held $otherProcess 'CURRENT_PROCESS_TEST 独立进程'
    Release-Child $otherProcess
    Release-Child $processOwner
    Write-Output 'PASS CURRENT_PROCESS_TEST 同进程冲突与不同进程隔离'

    $distinctOwner = Start-LeaseChild 'production' $environmentA
    Assert-Held $distinctOwner '不同 TMP/TEMP owner'
    $distinctContender = Start-LeaseChild 'production' $environmentB
    Assert-Refused $distinctContender '不同 TMP/TEMP'
    Release-Child $distinctOwner
    $distinctAfterRelease = Start-LeaseChild 'production' $environmentB
    Assert-Held $distinctAfterRelease '不同 TMP/TEMP 释放后的 owner'
    Release-Child $distinctAfterRelease
    Write-Output 'PASS 不同 TMP/TEMP 互斥与释放后可重新获取'

    $terminatedOwner = Start-LeaseChild 'production' $environmentA
    Assert-Held $terminatedOwner '异常退出对照 owner'
    $terminatedOwner.Process.StandardInput.WriteLine('exit-without-release')
    $terminatedOwner.Process.StandardInput.Flush()
    if (-not $terminatedOwner.Process.WaitForExit(10000) -or
        $terminatedOwner.Process.ExitCode -ne 0) {
        throw 'lease 子进程未按无析构协议退出'
    }
    $afterTermination = Start-LeaseChild 'production' $environmentB
    Assert-Held $afterTermination '异常退出后的稳定 owner'
    Release-Child $afterTermination
    $legacyAfterTermination = [IO.File]::Open($legacyPath, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $legacyAfterTermination.Dispose()
    Write-Output 'PASS 无析构退出后稳定与旧版文件句柄均已释放'
} finally {
    # 仅通过本轮持有的进程对象回收测试子进程，不查找或关闭其他 owner。
    foreach ($child in $children) {
        try {
            if (-not $child.HasExited) {
                try {
                    $child.StandardInput.WriteLine('release')
                    $child.StandardInput.Flush()
                } catch { }
                if (-not $child.WaitForExit(5000)) {
                    $child.Kill()
                    $child.WaitForExit()
                }
            }
        } finally {
            $child.Dispose()
        }
    }
    Remove-XenOwnedTestDirectory -RootPath $owned.RootPath -BasePath $owned.BasePath `
        -RepositoryRoot $repository -OwnerId $owned.OwnerId
}
