param(
    [string]$PackageRoot = (Join-Path $PSScriptRoot "..\cache\releases\Xen-888b04e-aim-dual"),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build-release-096e1a7-nvidia"),
    [string]$DestinationRoot = "\\192.168.3.20\XenLab$\releases\Xen-888b04e-aim-dual",
    [string]$SshIdentityFile = (Join-Path $env:USERPROFILE ".ssh\xen_foxos_ed25519"),
    [string]$SshUser = "XenDeploy",
    [string]$SshHost = "192.168.3.20",
    [switch]$Prepare,
    [string]$Scenario = "MoveLeft",
    [string]$Profile = "tracking"
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
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$worker = Join-Path $buildRoot "Release\Xen.exe"
$manifestPath = Join-Path $packageRoot "manifest.json"
$packageWorker = Join-Path $packageRoot "runtimes\nvidia\Xen.exe"
if (-not (Test-Path -LiteralPath $worker -PathType Leaf)) {
    throw "NVIDIA Worker 不存在：$worker"
}
if (-not (Test-Path -LiteralPath $destinationRoot -PathType Container)) {
    throw "辅机固定发布根不可读：$destinationRoot"
}

$commit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "无法读取当前 Git 提交。"
}
if (@(& git -C $repositoryRoot status --porcelain).Count -ne 0) {
    throw "差量发布要求工作树干净。"
}
$identity = Read-Json (Join-Path $buildRoot "xen-build-identity.json") "构建身份"
if ([string]$identity.runtime -ne "nvidia" -or
    [string]$identity.git_commit -ne $commit -or
    [bool]$identity.git_dirty) {
    throw "NVIDIA 构建身份与当前提交不一致。"
}
$manifest = Read-Json $manifestPath "固定包 manifest"
$record = @($manifest.files) | Where-Object {
    [string]$_.path -eq "runtimes/nvidia/Xen.exe"
}
if (@($record).Count -ne 1) {
    throw "manifest 中 NVIDIA Worker 记录不是唯一项。"
}
$hash = (Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash.ToLowerInvariant()
$length = (Get-Item -LiteralPath $worker).Length
$manifest.git_commit = $commit.ToLowerInvariant()
$record[0].size = [long]$length
$record[0].sha256 = $hash
$record[0].source = "$worker@$($commit.Substring(0, 7))"

$manifestPending = Join-Path $packageRoot ".manifest.incoming-$([guid]::NewGuid().ToString('N'))"
$remoteStage = Join-Path $destinationRoot ".aim-worker.incoming-$([guid]::NewGuid().ToString('N'))"
$remoteWorker = Join-Path $destinationRoot "runtimes\nvidia\Xen.exe"
$remoteManifest = Join-Path $destinationRoot "manifest.json"
try {
    $manifest | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $manifestPending -Encoding UTF8
    Copy-Atomic $worker $packageWorker
    Replace-FileAtomically $manifestPending $manifestPath

    New-Item -ItemType Directory -Path (Join-Path $remoteStage "runtimes\nvidia") -Force |
        Out-Null
    Copy-Item -LiteralPath $worker -Destination (Join-Path $remoteStage "runtimes\nvidia\Xen.exe")
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $remoteStage "manifest.json")
    $remoteHash = (Get-FileHash -LiteralPath (Join-Path $remoteStage "runtimes\nvidia\Xen.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($remoteHash -ne $hash) { throw "辅机暂存 Worker SHA-256 校验失败。" }
    Replace-FileAtomically (Join-Path $remoteStage "runtimes\nvidia\Xen.exe") $remoteWorker
    Replace-FileAtomically (Join-Path $remoteStage "manifest.json") $remoteManifest
} finally {
    if (Test-Path -LiteralPath $manifestPending -PathType Leaf) {
        Remove-Item -LiteralPath $manifestPending -Force
    }
    if (Test-Path -LiteralPath $remoteStage) {
        Remove-Item -LiteralPath $remoteStage -Recurse -Force
    }
}

$localManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
$remoteWorkerHash = (Get-FileHash -LiteralPath $remoteWorker -Algorithm SHA256).Hash
$remoteManifestHash = (Get-FileHash -LiteralPath $remoteManifest -Algorithm SHA256).Hash
if ($remoteWorkerHash -ne $hash.ToUpperInvariant()) {
    throw "辅机 Worker 回读 SHA-256 不一致。"
}

Write-Host "Aim NVIDIA Worker 差量发布完成。"
Write-Host "  commit=$commit"
Write-Host "  worker_sha256=$($hash.ToUpperInvariant())"
Write-Host "  manifest_sha256=$remoteManifestHash"
Write-Host "  package_root=$destinationRoot"

if ($Prepare) {
    if (-not (Test-Path -LiteralPath $SshIdentityFile -PathType Leaf)) {
        throw "SSH 身份文件不存在：$SshIdentityFile"
    }
    $remoteScript = 'C:\XenLab\releases\Xen-888b04e-aim-dual\tools\invoke_aim_manual_acceptance.ps1'
    $remoteRoot = 'C:\XenLab\releases\Xen-888b04e-aim-dual'
    $remoteCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' +
        $remoteScript + '" -TaskId AIM-LATENCY-COMP-001 -Scenario ' +
        $Scenario + ' -Profile ' + $Profile +
        ' -Mode Prepare -PackageRoot "' + $remoteRoot +
        '" -LightweightPackageValidation -EnableDelayCompensation -ControlDelayMs 15'
    & ssh -i $SshIdentityFile -o IdentitiesOnly=yes -o BatchMode=yes `
        "$SshUser@$SshHost" $remoteCommand
    if ($LASTEXITCODE -ne 0) { throw "辅机 Prepare 失败，退出码：$LASTEXITCODE" }
}
