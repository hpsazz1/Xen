param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$root = Join-Path ([IO.Path]::GetTempPath()) ('xen-transfer-owner-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

function Find-Boundary([string]$Script, [bool]$Creation) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw '正式传输脚本必须可解析' }
    if ($Creation) {
        $nodes = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -in @('New-Item', 'New-XenExclusiveDirectory') -and
            $node.Extent.Text -match '-Path \$(TargetRoot|incomingPath|incoming)\b'
        }, $true))
    } else {
        $nodes = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -eq '$source' -and $node.Right.Extent.Text -match 'FileStream'
        }, $true))
    }
    if ($nodes.Count -ne 1) { throw '必须命中唯一实际目录创建或复制前边界' }
    return $nodes[0].Extent.StartLineNumber
}

$failed = 0
try {
    foreach ($kind in @('dual', 'release')) {
        $scriptPath = Join-Path $RepositoryRoot $(if ($kind -eq 'dual') {
            'scripts/transfer_dual_machine_package.ps1'
        } else { 'scripts/transfer_release_bundle.ps1' })
        foreach ($case in @('healthy', 'foreign-directory', 'own-copy-failure')) {
            $directory = Join-Path $root "$kind-$case"
            $package = Join-Path $directory 'package-fixture'
            $destination = Join-Path $directory 'local-destination'
            $targetRoot = Join-Path $destination $(if ($kind -eq 'dual') { 'packages' } else { 'releases' })
            [IO.Directory]::CreateDirectory($package) | Out-Null
            [IO.Directory]::CreateDirectory($targetRoot) | Out-Null
            $payload = Join-Path $package 'payload.txt'
            [IO.File]::WriteAllText($payload, '只用于本机复制与回读的合成内容', [Text.UTF8Encoding]::new($false))
            $hash = (Get-FileHash -LiteralPath $payload).Hash
            $length = (Get-Item -LiteralPath $payload).Length
            $manifest = if ($kind -eq 'dual') {
                @{ schema = 1; complete = $true; package_type = 'xen-dual-machine-receiver'; package_id = 'package-fixture'
                    files = @(@{ relative_path = 'payload.txt'; length = $length; sha256 = $hash }) }
            } else {
                @{ schema = 1; product = 'Xen'; git_commit = ('a' * 40); runtimes = @('nvidia', 'directml', 'openvino')
                    files = @(@{ path = 'payload.txt'; size = $length; sha256 = $hash }) }
            }
            $manifestName = if ($kind -eq 'dual') { 'package-manifest.json' } else { 'manifest.json' }
            [IO.File]::WriteAllText((Join-Path $package $manifestName), ($manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
            $incoming = [IO.Path]::GetFullPath((Join-Path $targetRoot '.incoming-package-fixture'))
            $published = Join-Path $targetRoot 'package-fixture'
            # 两个生产catch可能递归删除incoming；调用前核对精确目标只在本夹具目录内。
            $expectedParent = [IO.Path]::GetFullPath($targetRoot)
            if ([IO.Path]::GetDirectoryName($incoming) -ne $expectedParent -or
                -not $incoming.StartsWith([IO.Path]::GetFullPath($root) + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw '传输测试目标超出本轮显式临时目录'
            }
            $global:XenTransferFixture = @{ Hits = 0; Incoming = $incoming; Payload = $payload; Lock = $null; Case = $case }
            $breakpoint = $null
            if ($case -ne 'healthy') {
                $breakpoint = Set-PSBreakpoint -Script $scriptPath -Line (Find-Boundary $scriptPath ($case -eq 'foreign-directory')) -Action {
                    $fixture = $global:XenTransferFixture
                    ++$fixture.Hits
                    if ($fixture.Case -eq 'foreign-directory') {
                        [IO.Directory]::CreateDirectory($fixture.Incoming) | Out-Null
                        [IO.File]::WriteAllText((Join-Path $fixture.Incoming 'writer-a.txt'), 'A必须保持原样')
                    } else {
                        $fixture.Lock = [IO.File]::Open($fixture.Payload, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
                    }
                }
            }
            $errorText = ''
            try { & $scriptPath -PackagePath $package -DestinationRoot $destination }
            catch { $errorText = $_.Exception.Message }
            finally {
                if ($breakpoint) { Remove-PSBreakpoint -Breakpoint $breakpoint }
                if ($global:XenTransferFixture.Lock) { $global:XenTransferFixture.Lock.Dispose() }
            }
            $hits = $global:XenTransferFixture.Hits
            $okay = switch ($case) {
                'healthy' {
                    -not $errorText -and [IO.File]::Exists((Join-Path $published 'payload.txt')) -and
                    (Get-FileHash -LiteralPath (Join-Path $published 'payload.txt')).Hash -eq $hash -and -not [IO.Directory]::Exists($incoming)
                }
                'foreign-directory' {
                    $sentinel = Join-Path $incoming 'writer-a.txt'
                    $hits -eq 1 -and $errorText -and -not [IO.Directory]::Exists($published) -and
                    [IO.File]::Exists($sentinel) -and [IO.File]::ReadAllText($sentinel) -ceq 'A必须保持原样'
                }
                'own-copy-failure' { $hits -eq 1 -and $errorText -and -not [IO.Directory]::Exists($incoming) -and -not [IO.Directory]::Exists($published) }
            }
            Write-Host "$kind/${case}: boundary=$hits pass=$okay"
            if (-not $okay) { ++$failed; Write-Host "实际失败信息：$errorText" }
        }
    }
    $module = Join-Path $RepositoryRoot 'scripts/path_safety.psm1'
    $gate = Join-Path $root 'create-now'
    $claim = Join-Path $root 'one-owner'
    $jobs = @()
    try {
        for ($i = 0; $i -lt 2; ++$i) {
            $jobs += Start-Job -ScriptBlock {
                param($Module, $Claim, $Gate, $Ready)
                $ErrorActionPreference = 'Stop'
                Import-Module $Module -Force
                [IO.File]::WriteAllText($Ready, 'ready')
                $deadline = [DateTime]::UtcNow.AddSeconds(15)
                while (-not [IO.File]::Exists($Gate)) {
                    if ([DateTime]::UtcNow -gt $deadline) { throw '目录竞争屏障超时' }
                    Start-Sleep -Milliseconds 5
                }
                try { New-XenExclusiveDirectory -Path $Claim; [pscustomobject]@{ Created = $true } }
                catch { [pscustomobject]@{ Created = $false } }
            } -ArgumentList $module, $claim, $gate, (Join-Path $root "claim-$i.ready")
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not ([IO.File]::Exists((Join-Path $root 'claim-0.ready')) -and [IO.File]::Exists((Join-Path $root 'claim-1.ready')))) {
            if ([DateTime]::UtcNow -gt $deadline) { throw '两个本机创建进程未就绪' }
            Start-Sleep -Milliseconds 5
        }
        [IO.File]::WriteAllText($gate, 'go')
        $done = @($jobs | Wait-Job -Timeout 20)
        if ($done.Count -ne 2) { throw '两个本机目录创建进程未结束' }
        $results = @($jobs | Receive-Job -ErrorAction Stop)
        if ($results.Count -ne 2 -or @($results | Where-Object { $_.Created }).Count -ne 1) { throw '同一目录只能产生一个创建者' }
        Write-Host 'concurrent/exclusive-create: 两个真实本机进程恰好一个创建者'
    } finally {
        if ($jobs.Count) { $jobs | Stop-Job; $jobs | Remove-Job }
    }
    if ($failed) { throw "$failed 个真实传输入口所有权案例失败" }
} finally {
    Remove-Variable -Name XenTransferFixture -Scope Global -ErrorAction SilentlyContinue
    Write-Host "保留本机合成包证据：$root"
}
