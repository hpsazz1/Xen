param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Static", "MoveLeft", "MoveRight", "Shuttle", "SuperJump")]
    [string]$Scenario,
    [Parameter(Mandatory = $true)]
    [ValidateSet("tracking", "prediction")]
    [string]$Profile,
    [ValidateSet("Prepare", "Launch")]
    [string]$Mode = "Prepare",
    [string]$PackageRoot = "",
    [string]$RunDirectory = "",
    [string]$OutputRoot = "C:\XenLab\reports\aim-dual-manual",
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$taskId = "AIM-DUAL-ACCEPT-001"
$physicalConfirmation = "XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT"
$ndiSourceName = "HPSAZZ (Xen-ROI-320)"
$kmboxIp = "192.168.2.188"
$kmboxPort = 13384
$kmboxUuid = "7679E04E"
$maxPredictionLeadPercent = 35.0

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot ".."))
} else {
    $PackageRoot = [System.IO.Path]::GetFullPath($PackageRoot)
}

function Write-TextAtomically([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $pending = "$Path.pending-$([guid]::NewGuid().ToString('N'))"
    [System.IO.File]::WriteAllText(
        $pending, $Content, [System.Text.UTF8Encoding]::new($false))
    try {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $backup = "$Path.backup-$([guid]::NewGuid().ToString('N'))"
            try {
                [System.IO.File]::Replace($pending, $Path, $backup)
            } finally {
                if (Test-Path -LiteralPath $backup) {
                    Remove-Item -LiteralPath $backup -Force
                }
            }
        } else {
            Move-Item -LiteralPath $pending -Destination $Path
        }
    } finally {
        if (Test-Path -LiteralPath $pending) {
            Remove-Item -LiteralPath $pending -Force
        }
    }
}

function Write-JsonAtomically([string]$Path, [object]$Value) {
    Write-TextAtomically $Path (($Value | ConvertTo-Json -Depth 12) + "`n")
}

function Get-FileEvidence([string]$Path) {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path = $file.FullName
        length = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
    }
}

function Read-Manifest() {
    $path = Join-Path $PackageRoot "manifest.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "完整发布包缺少 manifest.json：$PackageRoot"
    }
    $manifest = Get-Content -LiteralPath $path -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([int]$manifest.schema -ne 1 -or
        [string]$manifest.product -ne "Xen" -or
        [string]$manifest.git_commit -notmatch '^[0-9a-fA-F]{40}$' -or
        @($manifest.runtimes).Count -ne 3) {
        throw "完整发布包清单无效。"
    }
    return [pscustomobject]@{ Path = $path; Value = $manifest }
}

function Test-MutablePackageFile([string]$RelativePath) {
    return $RelativePath -match '^(cache|logs)\\.+'
}

function Assert-ManifestFiles(
        [object]$Manifest,
        [switch]$AllowConfigMismatch) {
    $declared = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $relative = ([string]$record.path).Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or
            [System.IO.Path]::IsPathRooted($relative) -or
            $relative -match '(^|\\)\.\.(\\|$)' -or
            -not $declared.Add($relative)) {
            throw "发布清单包含非法或重复路径：$relative"
        }
        $path = Join-Path $PackageRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "发布包缺少清单文件：$relative"
        }
        if ($AllowConfigMismatch -and $relative -ieq "config.ini") {
            continue
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ([long]$file.Length -ne [long]$record.size -or
            $hash -ne ([string]$record.sha256).ToUpperInvariant()) {
            throw "发布包文件长度或 SHA-256 不一致：$relative"
        }
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File)) {
        $relative = $file.FullName.Substring(
            $PackageRoot.TrimEnd('\').Length + 1)
        if ($relative -ieq "manifest.json") { continue }
        if (-not $declared.Contains($relative) -and
            -not (Test-MutablePackageFile $relative)) {
            throw "发布包包含清单外文件：$relative"
        }
    }
}

function Get-ModelName([object]$Manifest) {
    $models = @($Manifest.files | Where-Object {
        [string]$_.path -match '^models/[^/]+\.onnx$'
    })
    if ($models.Count -ne 1) {
        throw "人工验收完整包必须且只能包含一个根 models ONNX 文件。"
    }
    return Split-Path -Leaf ([string]$models[0].path)
}

function New-ConfigText([string]$ModelName) {
    $prediction = if ($Profile -eq "prediction") { "true" } else { "false" }
    return @"
[detector]
model_path=$ModelName
backend=tensorrt
device_id=0
openvino_device=cpu
input_width=0
input_height=0
conf_threshold=0.250000
nms_threshold=0.450000
top_k=300
output_format=auto
enable_fp16=true
enable_trt_cuda_graph=true
enable_gpu_preprocess=true
trt_cache_path=cache/tensorrt

[capture]
backend=ndi
adapter_index=0
output_index=0
enable_d3d11_cuda_interop=false
enable_d3d11_directml_interop=false
udp_url=udp://0.0.0.0:5000
udp_read_timeout_ms=250
udp_disconnect_timeout_ms=2000
udp_frame_layout=center_crop_1_to_1
udp_source_width=2560
udp_source_height=1440
ndi_source_name=$ndiSourceName
ndi_discovery_timeout_ms=10000
ndi_receive_timeout_ms=50
ndi_disconnect_timeout_ms=2000
ndi_frame_layout=center_crop_1_to_1
ndi_source_width=2560
ndi_source_height=1440
ndi_require_frame_metadata=false
roi_width=320
roi_height=320
center_roi=true
roi_x=0
roi_y=0
acquire_timeout_ms=16

[aim]
person_class_ids=0,2
head_class_ids=1,3
high_confidence=0.250000
low_confidence=0.100000
min_confirmed_hits=2
max_lost_frames=8
min_iou=0.100000
max_center_distance=0.250000
switch_margin=0.200000
switch_confirm_frames=3
switch_cooldown_frames=5
acquisition_range_percent=90.000000
body_aim_height_ratio=0.350000
deadzone_pixels=1.500000
smoothing=0.350000
counts_per_pixel_x=0.500000
counts_per_pixel_y=0.500000
max_counts_per_frame=50.000000
enable_prediction=$prediction
max_prediction_lead_percent=35.000000
predicted_gain=0.500000

[mouse]
backend=kmbox_net
allow_send_input=true
kmbox_ip=$kmboxIp
kmbox_port=$kmboxPort
kmbox_uuid=$kmboxUuid
kmbox_connect_timeout_ms=1000
kmbox_command_timeout_ms=300
makcu_port=
makcu_baud_rate=4000000
makcu_connect_timeout_ms=1000
makcu_command_timeout_ms=300

[keyboard]
aim_hold_virtual_keys=2
emergency_virtual_keys=35
runtime_toggle_virtual_keys=119

[log]
global_level=info
enable_console=true
enable_file=true
enable_debug_file=false
enable_ringbuf=true
ringbuf_capacity=1024
log_dir=logs
file_max_size_mb=10
file_max_count=3

[runtime]
profile_window=256

[ui]
width=900
height=640
enable_vsync=true
open_detached_preview_on_start=true
theme=light
"@
}

function Activate-Config(
        [object]$ManifestResult,
        [string]$ConfigText,
        [string]$SourceLabel) {
    $configPath = Join-Path $PackageRoot "config.ini"
    Write-TextAtomically $configPath $ConfigText
    $record = @($ManifestResult.Value.files | Where-Object {
        [string]$_.path -ieq "config.ini"
    })
    if ($record.Count -ne 1) {
        throw "完整包清单必须且只能登记一个 config.ini。"
    }
    $file = Get-Item -LiteralPath $configPath
    $record[0].size = [long]$file.Length
    $record[0].sha256 = (Get-FileHash -LiteralPath $configPath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $record[0].source = $SourceLabel
    Write-JsonAtomically $ManifestResult.Path $ManifestResult.Value
    $verified = Read-Manifest
    Assert-ManifestFiles $verified.Value
    return $verified
}

function Get-ScenarioDefinition() {
    switch ($Scenario) {
        "Static" {
            return [ordered]@{
                title = "静止目标自瞄稳定性"
                actions = @(
                    "选择单个静止目标，保持目标位置和人物姿态基本不变。",
                    "启动 Runtime、确认 KMBOX READY 后人工武装。",
                    "从相同初始偏差按住右键 3～5 秒再松开，重复 10 次。",
                    "观察锁定抖动、漂移、过冲；每次松键必须立即停止。"
                )
                observations = @(
                    "首次锁定是否方向正确",
                    "持续按住时是否抖动或来回修正",
                    "准星稳定后的残余偏差",
                    "松开右键是否立即停止",
                    "End 急停是否有效"
                )
            }
        }
        "MoveLeft" {
            return [ordered]@{
                title = "目标向左移动自瞄跟随"
                actions = @(
                    "单个目标从 ROI 右侧向左匀速移动，至少重复 10 次。",
                    "目标进入后按住右键跟随，离开或异常时立即松开。",
                    "每次尽量保持相同起点、速度和距离。"
                )
                observations = @(
                    "获取是否及时",
                    "跟随是否落后或超前",
                    "移动方向是否正确",
                    "离场后是否残留输出",
                    "重复结果是否一致"
                )
            }
        }
        "MoveRight" {
            return [ordered]@{
                title = "目标向右移动自瞄跟随"
                actions = @(
                    "单个目标从 ROI 左侧向右匀速移动，至少重复 10 次。",
                    "目标进入后按住右键跟随，离开或异常时立即松开。",
                    "保持与左移场景相近的速度、距离和路线。"
                )
                observations = @(
                    "获取是否及时",
                    "跟随是否落后或超前",
                    "与左移结果是否对称",
                    "离场后是否残留输出",
                    "重复结果是否一致"
                )
            }
        }
        "Shuttle" {
            return [ordered]@{
                title = "目标左右往复与反向收敛"
                actions = @(
                    "单个目标持续左右往复，至少完成 10 个完整往返。",
                    "按住右键覆盖突然反向、越过准星、归位和重新预测。",
                    "出现持续反向修正或失控时立即松键并按 End。"
                )
                observations = @(
                    "折返点是否过冲",
                    "越过准星后是否及时归位",
                    "反向后是否形成持续震荡",
                    "重新预测是否自然",
                    "左右方向是否对称"
                )
            }
        }
        "SuperJump" {
            return [ordered]@{
                title = "超级跳垂直跟随"
                actions = @(
                    "单个目标在 ROI 附近连续超级跳，至少完成 15 次。",
                    "按住右键覆盖起跳、腾空、落地和再次起跳。",
                    "保持起始区域、距离和移动方向尽量一致。"
                )
                observations = @(
                    "向上和向下跟随是否及时",
                    "腾空阶段是否过冲",
                    "落地后是否快速恢复",
                    "头身观测变化是否导致跳点",
                    "是否出现垂直持续震荡"
                )
            }
        }
    }
}

function New-TaskMarkdown(
        [object]$Definition,
        [string]$ResolvedRunDirectory,
        [string]$RunId) {
    $steps = for ($index = 0; $index -lt $Definition.actions.Count; ++$index) {
        "{0}. {1}" -f ($index + 1), $Definition.actions[$index]
    }
    $checks = $Definition.observations | ForEach-Object { "- [ ] $_" }
    $launch = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Mode Launch -Scenario {1} -Profile {2} -PackageRoot "{3}" -RunDirectory "{4}" -AllowPhysicalOutput -PhysicalOutputConfirmation {5}' -f
        (Join-Path $PackageRoot "tools\invoke_aim_manual_acceptance.ps1"),
        $Scenario, $Profile, $PackageRoot, $ResolvedRunDirectory,
        $physicalConfirmation
    return @"
# Xen 双机 Aim 人工测试任务

- 任务 ID：$taskId
- 运行 ID：$RunId
- 场景：$Scenario / $($Definition.title)
- 配置：$Profile
- Capture：NDI / $ndiSourceName
- Provider：TensorRT，FP16 + CUDA Graph + GPU 前处理
- Mouse：KMBOX NET $kmboxIp`:$kmboxPort
- 自瞄按键：鼠标右键
- 急停键：End

## 安全门

仅在私有或离线训练环境执行。启动前确认 End 可用、现场无非预期窗口，程序启动后仍需人工武装。
任何方向错误、持续发送、松键不停止或失控移动，立即松开右键并按 End。

## 操作步骤

$($steps -join "`n")

## 人工观察

$($checks -join "`n")

## Launch 命令

```powershell
$launch
```

应用退出后脚本会收集本轮新增 Runtime CSV/JSON 和日志。完成后填写 `OBSERVATION.md`，再等待下一任务。
"@
}

function Assert-PhysicalAuthorization() {
    if (-not $AllowPhysicalOutput.IsPresent -or
        $PhysicalOutputConfirmation -ne $physicalConfirmation) {
        throw "Launch 会发送真实 KMBOX 输入，必须同时提供物理输出开关和固定确认令牌。"
    }
}

if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
    throw "完整发布包目录不存在：$PackageRoot"
}
$launcher = Join-Path $PackageRoot "XenLauncher.exe"
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "完整发布包缺少 XenLauncher.exe。"
}
$aimReportScript = Join-Path $PackageRoot "tools\aim_report.ps1"
if (-not (Test-Path -LiteralPath $aimReportScript -PathType Leaf)) {
    throw "完整发布包缺少 Aim 报告助手。"
}

if ($Mode -eq "Prepare") {
    if ($AllowPhysicalOutput.IsPresent -or
        -not [string]::IsNullOrWhiteSpace($PhysicalOutputConfirmation)) {
        throw "Prepare 只生成任务，不接受 Launch 物理输出授权。"
    }
    $manifestResult = Read-Manifest
    Assert-ManifestFiles $manifestResult.Value -AllowConfigMismatch
    $modelName = Get-ModelName $manifestResult.Value
    $configText = New-ConfigText $modelName
    $definition = Get-ScenarioDefinition
    $runId = "{0}-{1}-{2}-{3}" -f
        (Get-Date -Format "yyyyMMdd-HHmmss"),
        $Scenario.ToLowerInvariant(), $Profile,
        [guid]::NewGuid().ToString("N").Substring(0, 8)
    $resolvedRun = if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        [System.IO.Path]::GetFullPath((Join-Path $OutputRoot $runId))
    } else {
        [System.IO.Path]::GetFullPath($RunDirectory)
    }
    if (Test-Path -LiteralPath $resolvedRun) {
        throw "人工任务目录已存在，拒绝覆盖：$resolvedRun"
    }
    New-Item -ItemType Directory -Path $resolvedRun | Out-Null
    $configPath = Join-Path $resolvedRun "config.ini"
    Write-TextAtomically $configPath $configText
    $manifestResult = Activate-Config $manifestResult $configText `
        "generated:$taskId/$runId"
    $task = [ordered]@{
        schema = 1
        task_id = $taskId
        run_id = $runId
        scenario = $Scenario
        profile = $Profile
        prepared_utc = [DateTime]::UtcNow.ToString("o")
        package_root = $PackageRoot
        package_commit = [string]$manifestResult.Value.git_commit
        package_manifest = Get-FileEvidence $manifestResult.Path
        launcher = Get-FileEvidence $launcher
        model = Get-FileEvidence (Join-Path $PackageRoot "models\$modelName")
        config = Get-FileEvidence $configPath
        capture = [ordered]@{
            backend = "ndi"
            source = $ndiSourceName
            source_size = @(2560, 1440)
            roi_size = @(320, 320)
        }
        detector = [ordered]@{
            backend = "tensorrt"
            fp16 = $true
            cuda_graph = $true
            gpu_preprocess = $true
        }
        aim = [ordered]@{
            prediction_enabled = $Profile -eq "prediction"
            max_prediction_lead_percent = $maxPredictionLeadPercent
            predicted_gain = 0.5
        }
        mouse = [ordered]@{
            backend = "kmbox_net"
            ip = $kmboxIp
            port = $kmboxPort
            uuid = $kmboxUuid
            allow_send_input = $true
        }
    }
    Write-JsonAtomically (Join-Path $resolvedRun "task.json") $task
    Write-TextAtomically (Join-Path $resolvedRun "TASK.md") `
        (New-TaskMarkdown $definition $resolvedRun $runId)
    Write-TextAtomically (Join-Path $resolvedRun "OBSERVATION.md") @"
# 人工观察记录

- 运行 ID：$runId
- 场景：$Scenario
- 配置：$Profile
- 执行人：
- 开始/结束时间：
- 是否完成全部操作：
- 是否触发 End 急停：
- 锁定、滞后、过冲、抖动和方向表现：
- 松开右键后的停止表现：
- 与上一配置相比的变化：
- 异常发生时间或复现步骤：
- 人工结论：通过 / 需调整 / 立即停止
"@
    Write-Host "Aim 人工任务已准备：$resolvedRun"
    Write-Host "  run_id=$runId"
    Write-Host "  scenario=$Scenario"
    Write-Host "  profile=$Profile"
    Write-Host "  config_sha256=$($task.config.sha256)"
    Write-Host "本轮尚未启动物理输出；请先人工复核 TASK.md。"
    exit 0
}

Assert-PhysicalAuthorization
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    throw "Launch 必须指定 Prepare 生成的 RunDirectory。"
}
$resolvedRun = [System.IO.Path]::GetFullPath($RunDirectory)
$taskPath = Join-Path $resolvedRun "task.json"
if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) {
    throw "人工任务缺少 task.json：$resolvedRun"
}
$task = Get-Content -LiteralPath $taskPath -Raw -Encoding utf8 |
    ConvertFrom-Json
if ([int]$task.schema -ne 1 -or
    [string]$task.task_id -ne $taskId -or
    [string]$task.scenario -ne $Scenario -or
    [string]$task.profile -ne $Profile -or
    [string]$task.package_root -ne $PackageRoot) {
    throw "Launch 参数与 Prepare 任务不一致。"
}
$activeConfig = Join-Path $PackageRoot "config.ini"
$activeHash = (Get-FileHash -LiteralPath $activeConfig -Algorithm SHA256).Hash
if ($activeHash -ne [string]$task.config.sha256) {
    throw "当前完整包 config.ini 不是本任务 Prepare 的配置。"
}
$manifestResult = Read-Manifest
Assert-ManifestFiles $manifestResult.Value

$runtimeRoot = Join-Path $PackageRoot "cache\runtime"
$before = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
if (Test-Path -LiteralPath $runtimeRoot) {
    foreach ($file in @(Get-ChildItem -LiteralPath $runtimeRoot -File)) {
        [void]$before.Add($file.FullName)
    }
}
$startedUtc = [DateTime]::UtcNow
Write-Host "即将启动真实 KMBOX 输出任务：$($task.run_id)"
Write-Host "确认 End 急停可用；程序启动后仍需人工武装并按 TASK.md 操作。"
$process = Start-Process -FilePath $launcher -WorkingDirectory $PackageRoot `
    -PassThru -Wait
$endedUtc = [DateTime]::UtcNow

$automaticRoot = Join-Path $resolvedRun "automatic"
New-Item -ItemType Directory -Path $automaticRoot -Force | Out-Null
$newReports = @()
if (Test-Path -LiteralPath $runtimeRoot) {
    $newReports = @(Get-ChildItem -LiteralPath $runtimeRoot -File |
        Where-Object { -not $before.Contains($_.FullName) } |
        Sort-Object Name)
}
if ($newReports.Count -eq 0) {
    throw "应用退出后未发现本轮新增 Runtime 报告。"
}
$reportEvidence = @()
foreach ($file in $newReports) {
    $destination = Join-Path $automaticRoot $file.Name
    Copy-Item -LiteralPath $file.FullName -Destination $destination
    $reportEvidence += Get-FileEvidence $destination
}
$logEvidence = @()
$logRoot = Join-Path $PackageRoot "logs"
if (Test-Path -LiteralPath $logRoot) {
    $copiedLogRoot = Join-Path $resolvedRun "logs"
    New-Item -ItemType Directory -Path $copiedLogRoot -Force | Out-Null
    foreach ($file in @(Get-ChildItem -LiteralPath $logRoot -File)) {
        $destination = Join-Path $copiedLogRoot $file.Name
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
        $logEvidence += Get-FileEvidence $destination
    }
}

. $aimReportScript
$jsonReports = @($newReports | Where-Object { $_.Extension -ieq ".json" })
$segments = @()
$allSamples = @()
$sampleCount = [uint64]0
$successful = [uint64]0
$failed = [uint64]0
$reportDropped = [uint64]0
$runtimeDropped = [uint64]0
foreach ($file in $jsonReports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ($null -eq $report.samples) { continue }
    $samples = @($report.samples)
    $allSamples += $samples
    $sampleCount += [uint64]$report.sample_count
    $successful += [uint64]$report.successful_samples
    $failed += [uint64]$report.failed_samples
    $reportDropped += [uint64]$report.report_samples_dropped
    $runtimeDropped += [uint64]$report.runtime_samples_dropped
    $segments += [ordered]@{
        session_id = [string]$report.session_id
        provider = [string]$report.provider
        capture_backend = [string]$report.capture_backend
        mouse_backend = [string]$report.mouse_backend
        sample_count = [uint64]$report.sample_count
        successful_samples = [uint64]$report.successful_samples
        failed_samples = [uint64]$report.failed_samples
        file = Get-FileEvidence $file.FullName
    }
}
if ($allSamples.Count -eq 0) {
    throw "本轮 Runtime JSON 没有可汇总样本。"
}
$predictionState = if ($Profile -eq "prediction") { "on" } else { "off" }
$aimSummary = Get-XenAimReportSummary -Samples $allSamples `
    -PredictionEnabled $predictionState `
    -MaxPredictionLeadPercent $maxPredictionLeadPercent
$providerMismatch = @($segments | Where-Object {
    $_.provider -ne "TensorrtExecutionProvider"
}).Count
$captureMismatch = @($segments | Where-Object {
    $_.capture_backend -ne "NDI"
}).Count
$mouseCommands = @($allSamples | Where-Object { [bool]$_.mouse_sent }).Count
$complete = $failed -eq 0 -and $reportDropped -eq 0 -and
    $runtimeDropped -eq 0 -and $providerMismatch -eq 0 -and
    $captureMismatch -eq 0 -and $mouseCommands -gt 0 -and
    [uint64]$aimSummary.violation_count -eq 0
$summary = [ordered]@{
    schema = 1
    task_id = $taskId
    run_id = [string]$task.run_id
    scenario = $Scenario
    profile = $Profile
    started_utc = $startedUtc.ToString("o")
    ended_utc = $endedUtc.ToString("o")
    launcher_exit_code = [int]$process.ExitCode
    automatic_complete = $complete
    awaiting_human_observation = $true
    segment_count = $segments.Count
    sample_count = $sampleCount
    successful_samples = $successful
    failed_samples = $failed
    report_samples_dropped = $reportDropped
    runtime_samples_dropped = $runtimeDropped
    mouse_commands = $mouseCommands
    provider_mismatch_segments = $providerMismatch
    capture_mismatch_segments = $captureMismatch
    aim = $aimSummary
    segments = $segments
    collected_files = $reportEvidence
    logs = $logEvidence
}
Write-JsonAtomically (Join-Path $resolvedRun "automatic-summary.json") $summary
Write-Host "Aim 人工任务自动证据已收集：$resolvedRun"
Write-Host "  samples=$sampleCount, failed=$failed, mouse_commands=$mouseCommands"
Write-Host "  aim_violations=$($aimSummary.violation_count)"
Write-Host "  automatic_complete=$complete"
Write-Host "请填写 OBSERVATION.md，并等待下一场景任务。"
