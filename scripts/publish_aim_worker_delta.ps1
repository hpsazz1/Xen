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
    [string]$Profile = "tracking",
    [ValidateRange(0.0, 1.0)]
    [double]$Smoothing = 0.50,
    [ValidateRange(0.01, 10.0)]
    [double]$CountsPerPixel = 0.40,
    [ValidateRange(0.0, 100.0)]
    [double]$ControlDelayMs = 15.0,
    [ValidateRange(0.0, 100.0)]
    [double]$MaxDelayCompensationMs = 16.0,
    [ValidateRange(1.0, 50.0)]
    [double]$MaxDelayCompensationPercent = 15.0
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$packageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
$destinationRoot = $DestinationRoot.TrimEnd('\')
$manifestPath = Join-Path $packageRoot "manifest.json"
$packageWorker = Join-Path $packageRoot "runtimes\nvidia\Xen.exe"
$repositoryAcceptanceScript = Join-Path $repositoryRoot `
    "scripts\invoke_aim_manual_acceptance.ps1"
$packageAcceptanceScript = Join-Path $packageRoot `
    "tools\invoke_aim_manual_acceptance.ps1"
$remoteAcceptanceScript = Join-Path $destinationRoot `
    "tools\invoke_aim_manual_acceptance.ps1"
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
$acceptanceRecord = @($manifest.files) | Where-Object {
    [string]$_.path -eq "tools/invoke_aim_manual_acceptance.ps1"
}
if (@($acceptanceRecord).Count -ne 1) {
    throw "manifest 中 Aim 正式任务脚本记录不是唯一项。"
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
    $remoteStage = Join-Path $destinationRoot $remoteStageName
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
        $remoteStageLocal = Join-Path $RemotePackageRoot $remoteStageName
        $applyCommand = 'cmd.exe /d /c move /Y "' +
            (Join-Path $remoteStageLocal "runtimes\nvidia\Xen.exe") + '" "' +
            (Join-Path $RemotePackageRoot "runtimes\nvidia\Xen.exe") +
            '" && move /Y "' + (Join-Path $remoteStageLocal "manifest.json") +
            '" "' + (Join-Path $RemotePackageRoot "manifest.json") + '"'
        & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes `
            "$SshUser@$SshHost" $applyCommand
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

# 正式任务 ID 或生成契约变化时，只差量同步任务脚本及 manifest。Worker、模型和运行库保持原位，
# 但发布提交必须更新为当前干净 HEAD，使 Prepare 的任务身份能够精确绑定本轮脚本版本。
$acceptanceHash = (Get-FileHash -LiteralPath $repositoryAcceptanceScript `
    -Algorithm SHA256).Hash
$packageAcceptanceHash = (Get-FileHash -LiteralPath $packageAcceptanceScript `
    -Algorithm SHA256).Hash
$remoteAcceptanceHash = (Get-FileHash -LiteralPath $remoteAcceptanceScript `
    -Algorithm SHA256).Hash
$declaredAcceptanceHash = ([string]$acceptanceRecord[0].sha256).ToUpperInvariant()
if ($acceptanceHash -ne $packageAcceptanceHash -or
    $acceptanceHash -ne $remoteAcceptanceHash -or
    $acceptanceHash -ne $declaredAcceptanceHash) {
    $acceptanceRecord[0].size = [long](Get-Item -LiteralPath `
        $repositoryAcceptanceScript).Length
    $acceptanceRecord[0].sha256 = $acceptanceHash.ToLowerInvariant()
    $acceptanceRecord[0].source =
        "$repositoryAcceptanceScript@$($commit.Substring(0, 7))"
    $manifest.git_commit = $commit.ToLowerInvariant()

    $manifestPending = Join-Path $packageRoot `
        ".manifest.incoming-$([guid]::NewGuid().ToString('N'))"
    $remoteStageName = ".aim-tool.incoming-$([guid]::NewGuid().ToString('N'))"
    $remoteStage = Join-Path $destinationRoot $remoteStageName
    try {
        $manifest | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath $manifestPending -Encoding UTF8
        Copy-Atomic $repositoryAcceptanceScript $packageAcceptanceScript
        Replace-FileAtomically $manifestPending $manifestPath

        New-Item -ItemType Directory -Path (Join-Path $remoteStage "tools") `
            -Force | Out-Null
        Copy-Item -LiteralPath $repositoryAcceptanceScript -Destination `
            (Join-Path $remoteStage "tools\invoke_aim_manual_acceptance.ps1")
        Copy-Item -LiteralPath $manifestPath -Destination `
            (Join-Path $remoteStage "manifest.json")
        if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
            throw "SSH 身份文件不存在：$SshIdentityFile"
        }
        $remoteStageLocal = Join-Path $RemotePackageRoot $remoteStageName
        $applyCommand = 'cmd.exe /d /c move /Y "' +
            (Join-Path $remoteStageLocal `
                "tools\invoke_aim_manual_acceptance.ps1") + '" "' +
            (Join-Path $RemotePackageRoot `
                "tools\invoke_aim_manual_acceptance.ps1") +
            '" && move /Y "' + (Join-Path $remoteStageLocal "manifest.json") +
            '" "' + (Join-Path $RemotePackageRoot "manifest.json") + '"'
        & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes `
            "$SshUser@$SshHost" $applyCommand
        if ($LASTEXITCODE -ne 0) {
            throw "辅机 Aim 正式任务脚本原子替换失败，退出码：$LASTEXITCODE"
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

$prepareOutput = @()
if ($Prepare) {
    if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
        throw "SSH 身份文件不存在：$SshIdentityFile"
    }
    $remoteScript = 'C:\XenLab\releases\Xen-888b04e-aim-dual\tools\invoke_aim_manual_acceptance.ps1'
    $remoteRoot = 'C:\XenLab\releases\Xen-888b04e-aim-dual'
    $remoteCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' +
        $remoteScript + '" -TaskId ' + $TaskId + ' -Scenario ' +
        $Scenario + ' -Profile ' + $Profile +
        ' -Mode Prepare -PackageRoot "' + $remoteRoot +
        '" -Smoothing ' + $Smoothing.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -CountsPerPixel ' + $CountsPerPixel.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -LightweightPackageValidation -EnableDelayCompensation' +
        ' -ControlDelayMs ' + $ControlDelayMs.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -MaxDelayCompensationMs ' + $MaxDelayCompensationMs.ToString(
            'F6', [Globalization.CultureInfo]::InvariantCulture) +
        ' -MaxDelayCompensationPercent ' +
            $MaxDelayCompensationPercent.ToString(
                'F6', [Globalization.CultureInfo]::InvariantCulture)
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
$expectedCounts = $CountsPerPixel.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedControlDelay = $ControlDelayMs.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedMaximumDelay = $MaxDelayCompensationMs.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedMaximumDelayPercent = $MaxDelayCompensationPercent.ToString(
    'F6', [Globalization.CultureInfo]::InvariantCulture)
$expectedPrediction = if ($Profile -eq "prediction") { "true" } else { "false" }
if ($configText -notmatch "(?m)^smoothing=$([regex]::Escape($expectedSmoothing))\r?$" -or
    $configText -notmatch "(?m)^counts_per_pixel_x=$([regex]::Escape($expectedCounts))\r?$" -or
    $configText -notmatch "(?m)^counts_per_pixel_y=$([regex]::Escape($expectedCounts))\r?$" -or
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
        [string]$task.profile -ne $Profile -or
        [double]$task.aim.smoothing -ne $Smoothing -or
        [double]$task.aim.counts_per_pixel -ne $CountsPerPixel -or
        -not [bool]$task.aim.delay_compensation_enabled -or
        [bool]$task.aim.prediction_enabled -ne ($Profile -eq "prediction") -or
        [double]$task.aim.control_delay_ms -ne $ControlDelayMs -or
        [double]$task.aim.max_delay_compensation_ms -ne
            $MaxDelayCompensationMs -or
        [double]$task.aim.max_delay_compensation_percent -ne
            $MaxDelayCompensationPercent -or
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
Write-Host "  package_root=$destinationRoot"
if ($Prepare) {
    Write-Host "  run_id=$runId"
    Write-Host "  report_root=C:\XenLab\reports\aim-dual-manual\$runId"
    Write-Host "以下命令会发送真实 KMBOX 输入，确认现场安全后可直接复制执行："
    Write-Output $launchCommand
}
