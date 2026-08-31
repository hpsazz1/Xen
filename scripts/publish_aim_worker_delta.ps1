param(
    [string]$PackageRoot = (Join-Path $PSScriptRoot "..\cache\releases\Xen-888b04e-aim-dual"),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-release-096e1a7-nvidia"),
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$\releases\Xen-888b04e-aim-dual",
    [string]$RemotePackageRoot = "C:\XenLab\releases\Xen-888b04e-aim-dual",
    [string]$SshIdentityFile = (Join-Path $env:USERPROFILE ".ssh\xen_foxos_ed25519"),
    [string]$SshUser = "XenDeploy",
    [string]$SshHost = "192.168.3.20",
    [switch]$ConfigOnly,
    [switch]$Prepare,
    [ValidateSet("AIM-DUAL-ACCEPT-001", "AIM-LATENCY-COMP-001",
        "AIM-SUPERJUMP-ACCEPT-001")]
    [string]$TaskId = "AIM-LATENCY-COMP-001",
    [string]$Scenario = "MoveLeft",
    [ValidateSet("None", "Static", "SustainedMove", "RandomMove", "Stop", "Reverse")]
    [string]$SuperJumpCase = "None",
    [string]$Profile = "tracking",
    [ValidateRange(0.0, 1.0)]
    [double]$Smoothing = 0.50,
    [ValidateRange(0.01, 10.0)]
    [double]$CountsPerPixel = 0.40,
    [Nullable[double]]$CountsPerPixelX = $null,
    [Nullable[double]]$CountsPerPixelY = $null,
    [ValidateRange(1.0, 200.0)]
    [double]$MaxCountsPerFrame = 12.0,
    [ValidateRange(0.0, 100.0)]
    [double]$ControlDelayMs = 15.0,
    [ValidateRange(0.0, 100.0)]
    [double]$MaxDelayCompensationMs = 16.0,
    [ValidateRange(1.0, 50.0)]
    [double]$MaxDelayCompensationPercent = 15.0,
    [switch]$RequireSourceTiming,
    [switch]$CapturePixelEvidence,
    [string]$PixelEvidenceToolRoot = "",
    [string]$PixelEvidenceBindingPath = "",
    [ValidateRange(1, 2400)]
    [int]$PixelEvidenceFrames = 2400,
    [ValidateRange(1, 60)]
    [int]$PixelEvidenceMaxSeconds = 30
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$resolvedCountsPerPixelX = if ($null -eq $CountsPerPixelX) {
    $CountsPerPixel
} else { [double]$CountsPerPixelX }
$resolvedCountsPerPixelY = if ($null -eq $CountsPerPixelY) {
    $CountsPerPixel
} else { [double]$CountsPerPixelY }
if ($resolvedCountsPerPixelX -lt 0.01 -or
    $resolvedCountsPerPixelX -gt 10.0 -or
    $resolvedCountsPerPixelY -lt 0.01 -or
    $resolvedCountsPerPixelY -gt 10.0) {
    throw "分轴 counts-per-pixel 必须位于 [0.01, 10.0]。"
}
if ($Scenario -eq "SuperJump" -and $SuperJumpCase -eq "None") {
    throw "SuperJump 发布必须指定一个独立 SuperJumpCase。"
}
if ($Scenario -ne "SuperJump" -and $SuperJumpCase -ne "None") {
    throw "SuperJumpCase 只适用于 SuperJump 场景。"
}
if ($CapturePixelEvidence.IsPresent) {
    if (-not $Prepare.IsPresent -or -not $RequireSourceTiming.IsPresent) {
        throw "同步像素 sidecar 只能随 RequireSourceTiming 的 Prepare 启用。"
    }
    if ([string]::IsNullOrWhiteSpace($PixelEvidenceToolRoot) -or
        [string]::IsNullOrWhiteSpace($PixelEvidenceBindingPath) -or
        -not [System.IO.Path]::IsPathRooted($PixelEvidenceToolRoot) -or
        -not [System.IO.Path]::IsPathRooted($PixelEvidenceBindingPath)) {
        throw "同步像素 sidecar 必须提供辅机本地绝对工具根和 binding 路径。"
    }
    if ($PixelEvidenceToolRoot.Contains('"') -or
        $PixelEvidenceBindingPath.Contains('"')) {
        throw "同步像素 sidecar 路径不得包含双引号。"
    }
} elseif (-not [string]::IsNullOrWhiteSpace($PixelEvidenceToolRoot) -or
    -not [string]::IsNullOrWhiteSpace($PixelEvidenceBindingPath)) {
    throw "未启用 CapturePixelEvidence 时不得传入 sidecar 路径。"
}

function Read-Json([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不存在：$Path"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        throw "$Description 不是有效 JSON：$Path；$($_.Exception.Message)"
    }
}

function Replace-FileAtomically([string]$Pending, [string]$Target) {
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $backup = "$Target.backup-$([guid]::NewGuid().ToString('N'))"
        try {
            [System.IO.File]::Replace($Pending, $Target, $backup)
        } finally {
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
    } else {
        Move-Item -LiteralPath $Pending -Destination $Target
    }
}

function Copy-Atomic([string]$Source, [string]$Target) {
    $parent = Split-Path -Parent $Target
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $pending = "$Target.pending-$([guid]::NewGuid().ToString('N'))"
    try {
        Copy-Item -LiteralPath $Source -Destination $pending -Force
        Replace-FileAtomically $pending $Target
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

function ConvertTo-PowerShellEncodedCommand([string]$Command) {
    return [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($Command))
}

function Get-NormalizedTextSha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "文本身份文件不存在：$Path"
    }
    $text = [System.IO.File]::ReadAllText($Path).
        Replace("`r`n", "`n").Replace("`r", "`n")
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($text)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString(
            $sha256.ComputeHash($bytes)).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$packageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
$destinationRoot = $DestinationRoot.TrimEnd('\')
$manifestPath = Join-Path $packageRoot "manifest.json"
$packageWorker = Join-Path $packageRoot "runtimes\nvidia\Xen.exe"
$toolSpecs = @(
    [pscustomobject][ordered]@{
        relative = "tools/invoke_aim_manual_acceptance.ps1"
        repository = Join-Path $repositoryRoot `
            "scripts\invoke_aim_manual_acceptance.ps1"
    },
    [pscustomobject][ordered]@{
        relative = "tools/aim_report.ps1"
        repository = Join-Path $repositoryRoot "scripts\aim_report.ps1"
    },
    [pscustomobject][ordered]@{
        relative = "tools/aim_control_diagnostics.ps1"
        repository = Join-Path $repositoryRoot `
            "scripts\aim_control_diagnostics.ps1"
    },
    [pscustomobject][ordered]@{
        relative = "tools/aim_fixed_scene_analysis.ps1"
        repository = Join-Path $repositoryRoot `
            "scripts\aim_fixed_scene_analysis.ps1"
    }
)
if (-not (Test-Path -LiteralPath $destinationRoot -PathType Container)) {
    throw "辅机固定发布根不可读：$destinationRoot"
}
if ($ConfigOnly -and -not $Prepare) {
    throw "纯配置差量必须同时指定 -Prepare，由正式任务生成器更新配置和 manifest。"
}
if ($ControlDelayMs -gt $MaxDelayCompensationMs) {
    throw "固定控制延迟不得大于延迟补偿时域上限。"
}

$commit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "无法读取当前 Git 提交。"
}
if (@(& git -C $repositoryRoot status --porcelain).Count -ne 0) {
    throw "差量发布要求工作树干净。"
}
$manifest = Read-Json $manifestPath "固定包 manifest"
$record = @($manifest.files) | Where-Object {
    [string]$_.path -eq "runtimes/nvidia/Xen.exe"
}
if (@($record).Count -ne 1) {
    throw "manifest 中 NVIDIA Worker 记录不是唯一项。"
}
$remoteWorker = Join-Path $destinationRoot "runtimes\nvidia\Xen.exe"
$remoteConfig = Join-Path $destinationRoot "config.ini"
$remoteManifest = Join-Path $destinationRoot "manifest.json"
$expectedWorkerHash = ([string]$record[0].sha256).ToUpperInvariant()
$packageWorkerHashBefore = (Get-FileHash -LiteralPath $packageWorker `
    -Algorithm SHA256).Hash
$remoteWorkerHashBefore = (Get-FileHash -LiteralPath $remoteWorker `
    -Algorithm SHA256).Hash
if ($packageWorkerHashBefore -ne $expectedWorkerHash -or
    $remoteWorkerHashBefore -ne $expectedWorkerHash) {
    throw "主辅机 NVIDIA Worker 与发布 manifest 不一致。"
}

if (-not $ConfigOnly) {
    $buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $worker = Join-Path $buildRoot "Release\Xen.exe"
    if (-not (Test-Path -LiteralPath $worker -PathType Leaf)) {
        throw "NVIDIA Worker 不存在：$worker"
    }
    $identity = Read-Json (Join-Path $buildRoot "xen-build-identity.json") "构建身份"
    if ([string]$identity.runtime -ne "nvidia" -or
        [string]$identity.git_commit -ne $commit -or
        [bool]$identity.git_dirty) {
        throw "NVIDIA 构建身份与当前提交不一致。"
    }
    $hash = (Get-FileHash -LiteralPath $worker `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $length = (Get-Item -LiteralPath $worker).Length
    $manifest.git_commit = $commit.ToLowerInvariant()
    $record[0].size = [long]$length
    $record[0].sha256 = $hash
    $record[0].source = "$worker@$($commit.Substring(0, 7))"

    $manifestPending = Join-Path $packageRoot `
        ".manifest.incoming-$([guid]::NewGuid().ToString('N'))"
    $remoteStageName = ".aim-worker.incoming-$([guid]::NewGuid().ToString('N'))"
    $destinationPrefix =
        [System.IO.Path]::GetFullPath($destinationRoot).TrimEnd('\') + '\'
    $remoteStage = [System.IO.Path]::GetFullPath(
        (Join-Path $destinationRoot $remoteStageName))
    if (-not $remoteStage.StartsWith(
            $destinationPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "辅机 Worker 暂存目录越出固定发布根：$remoteStage"
    }
    try {
        $manifest | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath $manifestPending -Encoding UTF8
        Copy-Atomic $worker $packageWorker
        Replace-FileAtomically $manifestPending $manifestPath

        New-Item -ItemType Directory -Path `
            (Join-Path $remoteStage "runtimes\nvidia") -Force | Out-Null
        Copy-Item -LiteralPath $worker -Destination `
            (Join-Path $remoteStage "runtimes\nvidia\Xen.exe")
        Copy-Item -LiteralPath $manifestPath -Destination `
            (Join-Path $remoteStage "manifest.json")
        $remoteHash = (Get-FileHash -LiteralPath `
            (Join-Path $remoteStage "runtimes\nvidia\Xen.exe") `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($remoteHash -ne $hash) {
            throw "辅机暂存 Worker SHA-256 校验失败。"
        }
        if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
            throw "SSH 身份文件不存在：$SshIdentityFile"
        }
        $remotePackagePrefix =
            [System.IO.Path]::GetFullPath($RemotePackageRoot).TrimEnd('\') + '\'
        $remoteWorkerStageLocal = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot `
                "$remoteStageName\runtimes\nvidia\Xen.exe"))
        $remoteWorkerTarget = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot "runtimes\nvidia\Xen.exe"))
        $remoteManifestStageLocal = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot "$remoteStageName\manifest.json"))
        $remoteManifestTarget = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot "manifest.json"))
        foreach ($path in @(
                $remoteWorkerStageLocal, $remoteWorkerTarget,
                $remoteManifestStageLocal, $remoteManifestTarget)) {
            if (-not $path.StartsWith(
                    $remotePackagePrefix,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "辅机原子替换路径越出固定发布根：$path"
            }
        }
        $escapedWorkerStage = $remoteWorkerStageLocal.Replace("'", "''")
        $escapedWorkerTarget = $remoteWorkerTarget.Replace("'", "''")
        $escapedManifestStage = $remoteManifestStageLocal.Replace("'", "''")
        $escapedManifestTarget = $remoteManifestTarget.Replace("'", "''")
        $applyScript = '& { $ErrorActionPreference = ''Stop''; ' +
            "Move-Item -LiteralPath '$escapedWorkerStage' " +
            "-Destination '$escapedWorkerTarget' -Force; " +
            "Move-Item -LiteralPath '$escapedManifestStage' " +
            "-Destination '$escapedManifestTarget' -Force }"
        $encodedApplyCommand = ConvertTo-PowerShellEncodedCommand $applyScript
        & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes `
            "$SshUser@$SshHost" powershell.exe -NoProfile -NonInteractive `
            -EncodedCommand $encodedApplyCommand
        if ($LASTEXITCODE -ne 0) {
            throw "辅机本地原子替换失败，退出码：$LASTEXITCODE"
        }
    } finally {
        if (Test-Path -LiteralPath $manifestPending -PathType Leaf) {
            Remove-Item -LiteralPath $manifestPending -Force
        }
        if (Test-Path -LiteralPath $remoteStage) {
            Remove-Item -LiteralPath $remoteStage -Recurse -Force
        }
    }
}

# 人工入口、Aim 契约和控制诊断是同一个正式报告闭包。任一脚本变化时只传输变化文件，
# 但 manifest 最后发布；中途失败会让旧清单与新文件哈希不一致并 fail-closed，不能误启任务。
$changedTools = [System.Collections.Generic.List[object]]::new()
$toolManifestChanged = $false
foreach ($spec in $toolSpecs) {
    if (-not (Test-Path -LiteralPath $spec.repository -PathType Leaf)) {
        throw "仓库 Aim 报告工具不存在：$($spec.repository)"
    }
    $relativeWindows = ([string]$spec.relative).Replace('/', '\')
    $packagePath = Join-Path $packageRoot $relativeWindows
    $remotePath = Join-Path $destinationRoot $relativeWindows
    $records = @(@($manifest.files) | Where-Object {
        [string]$_.path -eq [string]$spec.relative
    })
    if ($records.Count -gt 1) {
        throw "manifest 中 Aim 报告工具记录重复：$($spec.relative)"
    }
    $repositoryItem = Get-Item -LiteralPath $spec.repository
    $repositoryHash = (Get-FileHash -LiteralPath $spec.repository `
        -Algorithm SHA256).Hash
    $source = "$($spec.repository)@$($commit.Substring(0, 7))"
    $packageItem = if (Test-Path -LiteralPath $packagePath -PathType Leaf) {
        Get-Item -LiteralPath $packagePath
    } else { $null }
    $remoteItem = if (Test-Path -LiteralPath $remotePath -PathType Leaf) {
        Get-Item -LiteralPath $remotePath
    } else { $null }
    $packageHash = if ($null -ne $packageItem) {
        (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
    } else { "" }
    $remoteHash = if ($null -ne $remoteItem) {
        (Get-FileHash -LiteralPath $remotePath -Algorithm SHA256).Hash
    } else { "" }
    $record = if ($records.Count -eq 1) { $records[0] } else { $null }
    $publishedIdentityValid = $null -ne $record -and
        $null -ne $packageItem -and $null -ne $remoteItem -and
        $packageHash -eq $remoteHash -and
        $packageHash -eq ([string]$record.sha256).ToUpperInvariant() -and
        [long]$record.size -eq [long]$packageItem.Length -and
        [long]$record.size -eq [long]$remoteItem.Length
    $repositoryEquivalent = $publishedIdentityValid -and
        (Get-NormalizedTextSha256 $spec.repository) -eq
            (Get-NormalizedTextSha256 $packagePath)
    if ($publishedIdentityValid -and $repositoryEquivalent) {
        continue
    }
    if ($null -eq $record) {
        $record = [pscustomobject][ordered]@{
            path = [string]$spec.relative
            runtime = ""
            size = [long]0
            sha256 = ""
            source = ""
        }
        $manifest.files = @($manifest.files) + $record
    }
    if ([long]$record.size -ne [long]$repositoryItem.Length -or
        ([string]$record.sha256).ToUpperInvariant() -ne $repositoryHash -or
        [string]$record.source -ne $source) {
        $toolManifestChanged = $true
    }
    $record.size = [long]$repositoryItem.Length
    $record.sha256 = $repositoryHash.ToLowerInvariant()
    $record.source = $source
    if ($packageHash -ne $repositoryHash -or
        $remoteHash -ne $repositoryHash) {
        $changedTools.Add([pscustomobject][ordered]@{
            relative = [string]$spec.relative
            repository = [string]$spec.repository
            package = $packagePath
            remote = $remotePath
        })
    }
}
if ($changedTools.Count -gt 0 -or $toolManifestChanged) {
    $manifest.git_commit = $commit.ToLowerInvariant()
    $manifestPending = Join-Path $packageRoot `
        ".manifest.incoming-$([guid]::NewGuid().ToString('N'))"
    $remoteToolStageName =
        ".aim-tools.incoming-$([guid]::NewGuid().ToString('N'))"
    $toolDestinationPrefix =
        [System.IO.Path]::GetFullPath($destinationRoot).TrimEnd('\') + '\'
    $remoteToolStage = [System.IO.Path]::GetFullPath(
        (Join-Path $destinationRoot $remoteToolStageName))
    if (-not $remoteToolStage.StartsWith(
            $toolDestinationPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "辅机报告工具暂存目录越出固定发布根：$remoteToolStage"
    }
    try {
        $manifest | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath $manifestPending -Encoding UTF8
        foreach ($tool in $changedTools) {
            Copy-Atomic $tool.repository $tool.package
        }
        Replace-FileAtomically $manifestPending $manifestPath

        New-Item -ItemType Directory -Path $remoteToolStage -Force |
            Out-Null
        foreach ($tool in $changedTools) {
            $stagePath = Join-Path $remoteToolStage `
                ([string]$tool.relative).Replace('/', '\')
            New-Item -ItemType Directory -Path (Split-Path -Parent $stagePath) `
                -Force | Out-Null
            Copy-Item -LiteralPath $tool.repository -Destination $stagePath
            $stageHash = (Get-FileHash -LiteralPath $stagePath `
                -Algorithm SHA256).Hash
            $repositoryHash = (Get-FileHash -LiteralPath $tool.repository `
                -Algorithm SHA256).Hash
            if ($stageHash -ne $repositoryHash) {
                throw "辅机报告工具暂存 SHA-256 校验失败：$($tool.relative)"
            }
        }
        Copy-Item -LiteralPath $manifestPath -Destination `
            (Join-Path $remoteToolStage "manifest.json")
        if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
            throw "SSH 身份文件不存在：$SshIdentityFile"
        }
        $remotePackagePrefix =
            [System.IO.Path]::GetFullPath($RemotePackageRoot).TrimEnd('\') + '\'
        $remoteToolStageLocal = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot $remoteToolStageName))
        $moveStatements = [System.Collections.Generic.List[string]]::new()
        foreach ($tool in $changedTools) {
            $relativeWindows = ([string]$tool.relative).Replace('/', '\')
            $stageLocal = [System.IO.Path]::GetFullPath(
                (Join-Path $remoteToolStageLocal $relativeWindows))
            $targetLocal = [System.IO.Path]::GetFullPath(
                (Join-Path $RemotePackageRoot $relativeWindows))
            foreach ($path in @($stageLocal, $targetLocal)) {
                if (-not $path.StartsWith(
                        $remotePackagePrefix,
                        [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "辅机报告工具替换路径越出固定发布根：$path"
                }
            }
            $escapedStage = $stageLocal.Replace("'", "''")
            $escapedTarget = $targetLocal.Replace("'", "''")
            $moveStatements.Add(
                "Move-Item -LiteralPath '$escapedStage' " +
                "-Destination '$escapedTarget' -Force")
        }
        $manifestStageLocal = [System.IO.Path]::GetFullPath(
            (Join-Path $remoteToolStageLocal "manifest.json"))
        $manifestTargetLocal = [System.IO.Path]::GetFullPath(
            (Join-Path $RemotePackageRoot "manifest.json"))
        foreach ($path in @($manifestStageLocal, $manifestTargetLocal)) {
            if (-not $path.StartsWith(
                    $remotePackagePrefix,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "辅机报告工具清单替换路径越出固定发布根：$path"
            }
        }
        $escapedManifestStage = $manifestStageLocal.Replace("'", "''")
        $escapedManifestTarget = $manifestTargetLocal.Replace("'", "''")
        $moveStatements.Add(
            "Move-Item -LiteralPath '$escapedManifestStage' " +
            "-Destination '$escapedManifestTarget' -Force")
        $applyScript = '& { $ErrorActionPreference = ''Stop''; ' +
            ($moveStatements -join '; ') + ' }'
        $encodedApplyCommand = ConvertTo-PowerShellEncodedCommand $applyScript
        & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes `
            "$SshUser@$SshHost" powershell.exe -NoProfile -NonInteractive `
            -EncodedCommand $encodedApplyCommand
        if ($LASTEXITCODE -ne 0) {
            throw "辅机报告工具闭包原子替换失败，退出码：$LASTEXITCODE"
        }
    } finally {
        if (Test-Path -LiteralPath $manifestPending -PathType Leaf) {
            Remove-Item -LiteralPath $manifestPending -Force
        }
        if (Test-Path -LiteralPath $remoteToolStage -PathType Container) {
            Remove-Item -LiteralPath $remoteToolStage -Recurse -Force
        }
    }
}

$prepareOutput = @()
if ($Prepare) {
    if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
        throw "SSH 身份文件不存在：$SshIdentityFile"
    }
    $remoteRoot = [System.IO.Path]::GetFullPath($RemotePackageRoot).TrimEnd('\')
    if ($remoteRoot.Contains('"')) {
        throw "辅机本地包根不得包含双引号：$remoteRoot"
    }
    $remoteScript = Join-Path $remoteRoot "tools\invoke_aim_manual_acceptance.ps1"
    $sourceTimingSwitch = if ($RequireSourceTiming.IsPresent) {
        ' -RequireSourceTiming'
    } else { '' }
    $pixelEvidenceSwitch = if ($CapturePixelEvidence.IsPresent) {
        ' -CapturePixelEvidence -PixelEvidenceToolRoot "' +
            $PixelEvidenceToolRoot + '" -PixelEvidenceBindingPath "' +
            $PixelEvidenceBindingPath + '" -PixelEvidenceFrames ' +
            $PixelEvidenceFrames + ' -PixelEvidenceMaxSeconds ' +
            $PixelEvidenceMaxSeconds
    } else { '' }
    $remoteCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' +
        $remoteScript + '" -TaskId ' + $TaskId + ' -Scenario ' +
        $Scenario + ' -SuperJumpCase ' + $SuperJumpCase +
        ' -Profile ' + $Profile +
        ' -Mode Prepare -PackageRoot "' + $remoteRoot +
        '" -Smoothing ' + $Smoothing.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -CountsPerPixelX ' + $resolvedCountsPerPixelX.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -CountsPerPixelY ' + $resolvedCountsPerPixelY.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -MaxCountsPerFrame ' + $MaxCountsPerFrame.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -LightweightPackageValidation -EnableDelayCompensation' +
        ' -ControlDelayMs ' + $ControlDelayMs.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -MaxDelayCompensationMs ' + $MaxDelayCompensationMs.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -MaxDelayCompensationPercent ' +
            $MaxDelayCompensationPercent.ToString(
                'F6', [Globalization.CultureInfo]::InvariantCulture) +
        $sourceTimingSwitch + $pixelEvidenceSwitch
    $prepareOutput = @(& ssh -i $SshIdentityFile -o IdentitiesOnly=yes `
        -o BatchMode=yes "$SshUser@$SshHost" $remoteCommand 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "辅机 Prepare 失败，退出码：$LASTEXITCODE" }
    $prepareOutput | ForEach-Object { Write-Host $_ }

    # Prepare 会把本轮 Run ID 写入 manifest 的 config 来源。辅机最终 manifest 才是任务绑定事实，
    # 必须把 config 和 manifest 一起原子回写主机固定包，避免主辅机形成两个发布基线。
    Copy-Atomic $remoteConfig (Join-Path $packageRoot "config.ini")
    Copy-Atomic $remoteManifest $manifestPath
}

$finalManifest = Read-Json $manifestPath "最终固定包 manifest"
$finalToolEvidence = [ordered]@{}
foreach ($spec in $toolSpecs) {
    $records = @(@($finalManifest.files) | Where-Object {
        [string]$_.path -eq [string]$spec.relative
    })
    if ($records.Count -ne 1) {
        throw "最终 manifest 中 Aim 报告工具记录不是唯一项：$($spec.relative)"
    }
    $relativeWindows = ([string]$spec.relative).Replace('/', '\')
    $packagePath = Join-Path $packageRoot $relativeWindows
    $remotePath = Join-Path $destinationRoot $relativeWindows
    $packageItem = Get-Item -LiteralPath $packagePath
    $remoteItem = Get-Item -LiteralPath $remotePath
    $packageHash = (Get-FileHash -LiteralPath $packagePath `
        -Algorithm SHA256).Hash
    $remoteHash = (Get-FileHash -LiteralPath $remotePath `
        -Algorithm SHA256).Hash
    $declaredHash = ([string]$records[0].sha256).ToUpperInvariant()
    $repositoryEquivalent =
        (Get-NormalizedTextSha256 $spec.repository) -eq
            (Get-NormalizedTextSha256 $packagePath)
    if ($packageHash -ne $remoteHash -or
        $declaredHash -ne $packageHash -or
        [long]$records[0].size -ne [long]$packageItem.Length -or
        [long]$packageItem.Length -ne [long]$remoteItem.Length -or
        -not $repositoryEquivalent) {
        throw "Aim 报告工具闭包回读不一致：$($spec.relative)"
    }
    $finalToolEvidence[[string]$spec.relative] = $packageHash
}
$finalConfigRecord = @($finalManifest.files) | Where-Object {
    [string]$_.path -eq "config.ini"
}
if (@($finalConfigRecord).Count -ne 1) {
    throw "最终 manifest 中 config.ini 记录不是唯一项。"
}
$localConfig = Join-Path $packageRoot "config.ini"
$localConfigHash = (Get-FileHash -LiteralPath $localConfig -Algorithm SHA256).Hash
$remoteConfigHash = (Get-FileHash -LiteralPath $remoteConfig -Algorithm SHA256).Hash
if ($localConfigHash -ne $remoteConfigHash -or
    $localConfigHash -ne ([string]$finalConfigRecord[0].sha256).ToUpperInvariant()) {
    throw "主辅机 config.ini 或最终 manifest SHA-256 不一致。"
}
$configText = Get-Content -LiteralPath $remoteConfig -Raw -Encoding UTF8
$expectedSmoothing = $Smoothing.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedCountsX = $resolvedCountsPerPixelX.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedCountsY = $resolvedCountsPerPixelY.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedMaximumCounts = $MaxCountsPerFrame.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedControlDelay = $ControlDelayMs.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedMaximumDelay = $MaxDelayCompensationMs.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedMaximumDelayPercent = $MaxDelayCompensationPercent.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedPrediction = if ($Profile -eq "prediction") { "true" } else { "false" }
if ($configText -notmatch "(?m)^smoothing=$([regex]::Escape($expectedSmoothing))\r?$" -or
    $configText -notmatch "(?m)^counts_per_pixel_x=$([regex]::Escape($expectedCountsX))\r?$" -or
    $configText -notmatch "(?m)^counts_per_pixel_y=$([regex]::Escape($expectedCountsY))\r?$" -or
    $configText -notmatch "(?m)^max_counts_per_frame=$([regex]::Escape($expectedMaximumCounts))\r?$" -or
    $configText -notmatch "(?m)^enable_prediction=$expectedPrediction\r?$" -or
    $configText -notmatch '(?m)^enable_delay_compensation=true\r?$' -or
    $configText -notmatch
        "(?m)^control_delay_ms=$([regex]::Escape($expectedControlDelay))\r?$" -or
    $configText -notmatch
        "(?m)^max_delay_compensation_ms=$([regex]::Escape($expectedMaximumDelay))\r?$" -or
    $configText -notmatch
        "(?m)^max_delay_compensation_percent=$([regex]::Escape($expectedMaximumDelayPercent))\r?$") {
    throw "最终 config.ini 没有固化本轮唯一变量和不变参数。"
}

$localManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
$remoteManifestHash = (Get-FileHash -LiteralPath $remoteManifest -Algorithm SHA256).Hash
if ($localManifestHash -ne $remoteManifestHash) {
    throw "主辅机最终 manifest SHA-256 不一致。"
}

$packageWorkerHashAfter = (Get-FileHash -LiteralPath $packageWorker `
    -Algorithm SHA256).Hash
$remoteWorkerHashAfter = (Get-FileHash -LiteralPath $remoteWorker `
    -Algorithm SHA256).Hash
if ($ConfigOnly -and ($packageWorkerHashAfter -ne $packageWorkerHashBefore -or
        $remoteWorkerHashAfter -ne $remoteWorkerHashBefore)) {
    throw "纯配置差量不得修改主机或辅机 NVIDIA Worker。"
}

$runId = ""
$launchCommand = ""
if ($Prepare) {
    $outputText = $prepareOutput -join "`n"
    $runMatch = [regex]::Match(
        $outputText, '(?m)^\s*run_id=([^\r\n]+)\s*$')
    $launchMatch = [regex]::Match(
        $outputText, '(?m)^powershell\.exe .*AllowPhysicalOutput.*$')
    if (-not $runMatch.Success -or -not $launchMatch.Success) {
        throw "Prepare 输出缺少 Run ID 或完整 Launch 命令。"
    }
    $runId = $runMatch.Groups[1].Value.Trim()
    $launchCommand = $launchMatch.Value.Trim()
    $runRoot = Join-Path "\\192.168.3.20\XenLab$\reports\aim-dual-manual" $runId
    foreach ($name in @("config.ini", "task.json", "TASK.md", "OBSERVATION.md")) {
        if (-not (Test-Path -LiteralPath (Join-Path $runRoot $name) -PathType Leaf)) {
            throw "Prepare Run 缺少文件：$name"
        }
    }
    $runConfigHash = (Get-FileHash -LiteralPath `
        (Join-Path $runRoot "config.ini") -Algorithm SHA256).Hash
    $task = Read-Json (Join-Path $runRoot "task.json") "Prepare task.json"
    if ($runConfigHash -ne $remoteConfigHash -or
        [string]$task.run_id -ne $runId -or
        [string]$task.task_id -ne $TaskId -or
        [string]$task.scenario -ne $Scenario -or
        [string]$task.superjump_case -ne $SuperJumpCase -or
        [string]$task.profile -ne $Profile -or
        [bool]$task.require_source_timing -ne
            $RequireSourceTiming.IsPresent -or
        [bool]$task.pixel_evidence.enabled -ne
            $CapturePixelEvidence.IsPresent -or
        ($CapturePixelEvidence.IsPresent -and
            ([string]$task.pixel_evidence.executable.path -ne
                (Join-Path $PixelEvidenceToolRoot "XenCaptureEvidence.exe") -or
            [string]$task.pixel_evidence.source_binding.path -ne
                $PixelEvidenceBindingPath -or
            [int]$task.pixel_evidence.frames -ne $PixelEvidenceFrames -or
            [int]$task.pixel_evidence.max_seconds -ne
                $PixelEvidenceMaxSeconds -or
            [bool]$task.pixel_evidence.physical_output_capability -or
            $launchCommand -notmatch "-CapturePixelEvidence")) -or
        [double]$task.aim.smoothing -ne $Smoothing -or
        [double]$task.aim.counts_per_pixel_x -ne $resolvedCountsPerPixelX -or
        [double]$task.aim.counts_per_pixel_y -ne $resolvedCountsPerPixelY -or
        [double]$task.aim.max_counts_per_frame -ne $MaxCountsPerFrame -or
        -not [bool]$task.aim.delay_compensation_enabled -or
        [bool]$task.aim.prediction_enabled -ne ($Profile -eq "prediction") -or
        [double]$task.aim.control_delay_ms -ne $ControlDelayMs -or
        [double]$task.aim.max_delay_compensation_ms -ne
            $MaxDelayCompensationMs -or
        [double]$task.aim.max_delay_compensation_percent -ne
            $MaxDelayCompensationPercent -or
        [string]$task.acceptance_script.sha256 -ne
            $finalToolEvidence["tools/invoke_aim_manual_acceptance.ps1"] -or
        [string]$task.aim_report.sha256 -ne
            $finalToolEvidence["tools/aim_report.ps1"] -or
        [string]$task.aim_control_diagnostics.sha256 -ne
            $finalToolEvidence["tools/aim_control_diagnostics.ps1"] -or
        [string]$task.aim_fixed_scene_analysis.sha256 -ne
            $finalToolEvidence["tools/aim_fixed_scene_analysis.ps1"] -or
        [string]$task.config.sha256 -ne $remoteConfigHash -or
        [string]$task.package_manifest.sha256 -ne $remoteManifestHash) {
        throw "Prepare Run 参数或发布身份回读不一致。"
    }
}

$residue = @(Get-ChildItem -LiteralPath $destinationRoot -Force | Where-Object {
    $_.Name -like '.pending-*' -or $_.Name -like '.incoming-*' -or
    $_.Name -like '*.pending-*' -or $_.Name -like '*.incoming-*'
})
if ($residue.Count -ne 0) {
    throw "辅机发布根存在临时残留：$($residue.FullName -join ', ')"
}

Write-Host $(if ($ConfigOnly) {
    "Aim 配置差量发布完成。"
} else {
    "Aim NVIDIA Worker 差量发布完成。"
})
Write-Host "  commit=$commit"
Write-Host "  worker_sha256=$remoteWorkerHashAfter"
Write-Host "  config_sha256=$remoteConfigHash"
Write-Host "  manifest_sha256=$remoteManifestHash"
foreach ($entry in $finalToolEvidence.GetEnumerator()) {
    Write-Host "  $($entry.Key)_sha256=$($entry.Value)"
}
Write-Host "  package_root=$destinationRoot"
if ($Prepare) {
    Write-Host "  run_id=$runId"
    Write-Host "  superjump_case=$SuperJumpCase"
    Write-Host "  report_root=C:\XenLab\reports\aim-dual-manual\$runId"
    Write-Host "以下命令会发送真实 KMBOX 输入，确认现场安全后可直接复制执行："
    Write-Output $launchCommand
}
