param(
    [ValidateSet("AIM-DUAL-ACCEPT-001", "AIM-LATENCY-COMP-001",
        "AIM-SUPERJUMP-ACCEPT-001")]
    [string]$TaskId = "AIM-DUAL-ACCEPT-001",
    [Parameter(Mandatory = $true)]
    [ValidateSet("Static", "MoveLeft", "MoveRight", "Shuttle", "SuperJump")]
    [string]$Scenario,
    [ValidateSet("None", "Static", "SustainedMove", "Stop", "Reverse")]
    [string]$SuperJumpCase = "None",
    [Parameter(Mandatory = $true)]
    [ValidateSet("tracking", "prediction")]
    [string]$Profile,
    [ValidateSet("Prepare", "Launch", "Recover")]
    [string]$Mode = "Prepare",
    [ValidateSet("Physical", "ObserveOnly")]
    [string]$OutputMode = "Physical",
    [string]$PackageRoot = "",
    [string]$RunDirectory = "",
    [string]$OutputRoot = "C:\XenLab\reports\aim-dual-manual",
    [ValidateRange(0.0, 1.0)]
    [double]$Smoothing = 0.35,
    [ValidateRange(0.01, 10.0)]
    [double]$CountsPerPixel = 0.40,
    [Nullable[double]]$CountsPerPixelX = $null,
    [Nullable[double]]$CountsPerPixelY = $null,
    [ValidateRange(1.0, 200.0)]
    [double]$MaxCountsPerFrame = 12.0,
    [switch]$EnableDelayCompensation,
    [ValidateRange(0.0, 100.0)]
    [double]$ControlDelayMs = 0.0,
    [ValidateRange(0.0, 100.0)]
    [double]$MaxDelayCompensationMs = 16.0,
    [ValidateRange(1.0, 50.0)]
    [double]$MaxDelayCompensationPercent = 15.0,
    [switch]$RequireSourceTiming,
    # 兼容旧发布入口的调用参数；当前校验固定为 task_scoped，不再分 full/lightweight。
    [switch]$LightweightPackageValidation,
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$taskId = $TaskId
$observeOnly = $OutputMode -eq "ObserveOnly"
$outputModeRecord = if ($observeOnly) { "observe_only" } else { "physical" }
$outputModeDisplay = if ($observeOnly) {
    "Observe-only / 无物理输出对照"
} else {
    "Physical / 真实 KMBOX 输出"
}
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
    throw "SuperJump 必须指定一个独立 SuperJumpCase。"
}
if ($Scenario -ne "SuperJump" -and $SuperJumpCase -ne "None") {
    throw "SuperJumpCase 只适用于 SuperJump 场景。"
}
$physicalConfirmation = "XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT"
$ndiSourceName = "HPSAZZ (Xen-ROI-320)"
$kmboxIp = "192.168.2.188"
$kmboxPort = 13384
$kmboxUuid = "7679E04E"
$maxPredictionLeadPercent = 35.0
$taskManifestPaths = @(
    "XenLauncher.exe",
    "config.ini",
    "runtimes\nvidia\Xen.exe",
    "tools\invoke_aim_manual_acceptance.ps1",
    "tools\aim_report.ps1",
    "tools\aim_control_diagnostics.ps1"
)
$packageValidationMode = "task_scoped"

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

function Assert-TaskManifestFiles(
        [object]$Manifest,
        [string]$ModelName,
        [switch]$AllowConfigMismatch) {
    $declared = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $required = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($path in @($taskManifestPaths) + "models\$ModelName") {
        [void]$required.Add($path.Replace('/', '\'))
    }
    $verified = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($record in @($Manifest.files)) {
        $relative = ([string]$record.path).Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or
            [System.IO.Path]::IsPathRooted($relative) -or
            $relative -match '(^|\\)\.\.(\\|$)' -or
            -not $declared.Add($relative)) {
            throw "发布清单包含非法或重复路径：$relative"
        }
        if (-not $required.Contains($relative)) {
            continue
        }
        $path = Join-Path $PackageRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "人工任务依赖文件缺失：$relative"
        }
        if ($AllowConfigMismatch -and $relative -ieq "config.ini") {
            continue
        }
        $file = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ([long]$file.Length -ne [long]$record.size -or
            $hash -ne ([string]$record.sha256).ToUpperInvariant()) {
            throw "人工任务依赖文件长度或 SHA-256 不一致：$relative"
        }
        [void]$verified.Add($relative)
    }
    foreach ($relative in $required) {
        if (-not $verified.Contains($relative) -and
            -not ($AllowConfigMismatch -and $relative -ieq "config.ini")) {
            throw "发布清单缺少人工任务依赖文件：$relative"
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
    $allowSendInput = if ($observeOnly) { "false" } else { "true" }
    $allowObserveOnlyControl = if ($observeOnly) { "true" } else { "false" }
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
ndi_clock_sync_url=udp://192.168.3.10:5011
ndi_clock_sync_interval_ms=250
ndi_clock_sync_timeout_ms=200
ndi_clock_mapping_max_age_ms=1000
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
body_aim_range_percent=50.000000
deadzone_pixels=1.500000
smoothing=$('{0:F6}' -f $Smoothing)
# tracking 基线针对移动跟随的轻微滞后做单变量增益修正；单帧上限和平滑保持不变，
# prediction 只改变提前项，不改变基础控制曲线。
counts_per_pixel_x=$('{0:F6}' -f $resolvedCountsPerPixelX)
counts_per_pixel_y=$('{0:F6}' -f $resolvedCountsPerPixelY)
max_counts_per_frame=$('{0:F6}' -f $MaxCountsPerFrame)
enable_delay_compensation=$($EnableDelayCompensation.IsPresent.ToString().ToLowerInvariant())
control_delay_ms=$('{0:F6}' -f $ControlDelayMs)
max_delay_compensation_ms=$('{0:F6}' -f $MaxDelayCompensationMs)
max_delay_compensation_percent=$('{0:F6}' -f $MaxDelayCompensationPercent)
enable_prediction=$prediction
max_prediction_lead_percent=35.000000
predicted_gain=0.500000

[mouse]
backend=kmbox_net
allow_send_input=$allowSendInput
allow_observe_only_control=$allowObserveOnlyControl
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
        [string]$SourceLabel,
        [string]$ModelName) {
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
    Assert-TaskManifestFiles $verified.Value $ModelName
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
            switch ($SuperJumpCase) {
                "Static" {
                    return [ordered]@{
                        title = "超级跳期间 X 静止稳定性"
                        actions = @(
                            "本 Run 唯一动作：X 静止",
                            "选择单个目标，只做超级跳，X 轴和视角保持静止。",
                            "目标进入后按住右键约 5 秒再松开，不追加其他动作。"
                        )
                        observations = @(
                            "本 Run 唯一动作：X 静止",
                            "基础点或准星是否自行晃动",
                            "Y 起跳、腾空和落地跟随是否及时稳定"
                        )
                    }
                }
                "SustainedMove" {
                    return [ordered]@{
                        title = "超级跳期间 X 持续横移跟随"
                        actions = @(
                            "本 Run 唯一动作：X 持续横移",
                            "选择单个目标，只做一个方向的连续横移并维持相近的超级跳节奏。",
                            "目标进入后按住右键覆盖完整横移，期间不停止、不换向。"
                        )
                        observations = @(
                            "本 Run 唯一动作：X 持续横移",
                            "跟随是否落后、追不上或出框",
                            "Y 起跳、腾空和落地跟随是否及时稳定"
                        )
                    }
                }
                "Stop" {
                    return [ordered]@{
                        title = "超级跳期间 X 横移后停止"
                        actions = @(
                            "本 Run 唯一动作：X 横移后停止",
                            "选择单个目标，沿同一方向横移约 2 秒后完全停止并保持约 3 秒。",
                            "按住右键覆盖横移与停止过程，中途不换向、不追加其他动作。"
                        )
                        observations = @(
                            "本 Run 唯一动作：X 横移后停止",
                            "停止后是否延迟停发、过冲或细碎往返",
                            "Y 起跳、腾空和落地跟随是否及时稳定"
                        )
                    }
                }
                "Reverse" {
                    return [ordered]@{
                        title = "超级跳期间 X 横移后换向"
                        actions = @(
                            "本 Run 唯一动作：X 横移后换向",
                            "选择单个目标，沿一个方向横移约 2 秒后明确反向，并沿新方向继续约 3 秒。",
                            "按住右键覆盖换向过程，中途不停顿、不追加其他动作。"
                        )
                        observations = @(
                            "本 Run 唯一动作：X 横移后换向",
                            "换向时是否硬停、过冲或细碎往返",
                            "Y 起跳、腾空和落地跟随是否及时稳定"
                        )
                    }
                }
            }
        }
    }
}

function New-LaunchCommand([string]$ResolvedRunDirectory) {
    $delaySwitch = if ($EnableDelayCompensation.IsPresent) {
        " -EnableDelayCompensation"
    } else {
        ""
    }
    $sourceTimingSwitch = if ($RequireSourceTiming.IsPresent) {
        " -RequireSourceTiming"
    } else {
        ""
    }
    $outputModeSwitch = if ($observeOnly) {
        " -OutputMode ObserveOnly"
    } else {
        ""
    }
    $physicalAuthorization = if ($observeOnly) {
        ""
    } else {
        " -AllowPhysicalOutput -PhysicalOutputConfirmation $physicalConfirmation"
    }
    return ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" ' +
        '-TaskId {1} -Mode Launch{2} -Scenario {3} -SuperJumpCase {4} ' +
        '-Profile {5} -PackageRoot "{6}" -RunDirectory "{7}" ' +
        '-Smoothing {8:F6} -CountsPerPixelX {9:F6} ' +
        '-CountsPerPixelY {10:F6} -MaxCountsPerFrame {11:F6}{12} ' +
        '-ControlDelayMs {13:F6} -MaxDelayCompensationMs {14:F6} ' +
        '-MaxDelayCompensationPercent {15:F6}{16}{17}') -f
        (Join-Path $PackageRoot "tools\invoke_aim_manual_acceptance.ps1"),
        $taskId, $outputModeSwitch, $Scenario, $SuperJumpCase, $Profile,
        $PackageRoot,
        $ResolvedRunDirectory,
        $Smoothing, $resolvedCountsPerPixelX, $resolvedCountsPerPixelY,
        $MaxCountsPerFrame, $delaySwitch, $ControlDelayMs,
        $MaxDelayCompensationMs, $MaxDelayCompensationPercent,
        $sourceTimingSwitch,
        $physicalAuthorization
}

function New-TaskMarkdown(
        [object]$Definition,
        [string]$ResolvedRunDirectory,
        [string]$RunId) {
    $steps = for ($index = 0; $index -lt $Definition.actions.Count; ++$index) {
        "{0}. {1}" -f ($index + 1), $Definition.actions[$index]
    }
    $checks = $Definition.observations | ForEach-Object { "- [ ] $_" }
    $launch = New-LaunchCommand $ResolvedRunDirectory
    $mouseDescription = if ($observeOnly) {
        "KMBOX NET $kmboxIp`:$kmboxPort（只读按键，Mouse 必须保持 DISABLED）"
    } else {
        "KMBOX NET $kmboxIp`:$kmboxPort"
    }
    $safetyText = if ($observeOnly) {
@"
本 Run 禁止发送真实 KMBOX 输入，allow_send_input=false。按住右键只驱动 Aim 计算和诊断采集，
启动 Runtime 后点击“观测武装”，同时确认 Mouse/物理输出仍为 DISABLED；Launch 命令不接受
-AllowPhysicalOutput 或确认令牌。若出现任何非预期物理
移动，立即结束程序并按 End，记录为安全门失败。
"@
    } else {
@"
仅在私有或离线训练环境执行。启动前确认 End 可用、现场无非预期窗口，程序启动后仍需人工武装。
任何方向错误、持续发送、松键不停止或失控移动，立即松开右键并按 End。
"@
    }
    return @"
# Xen 双机 Aim 人工测试任务

- 任务 ID：$taskId
- 运行 ID：$RunId
- 场景：$Scenario / $($Definition.title)
- 配置：$Profile
- 输出模式：$outputModeDisplay
- smoothing：$('{0:F6}' -f $Smoothing)
- counts-per-pixel X：$('{0:F6}' -f $resolvedCountsPerPixelX)
- counts-per-pixel Y：$('{0:F6}' -f $resolvedCountsPerPixelY)
- 单帧二维上限：$('{0:F6}' -f $MaxCountsPerFrame) counts
- 延迟补偿：$($EnableDelayCompensation.IsPresent)
- 固定控制延迟：$('{0:F6}' -f $ControlDelayMs) ms
- 必须取得有效 source timing：$($RequireSourceTiming.IsPresent)
- Capture：NDI / $ndiSourceName
- Provider：TensorRT，FP16 + CUDA Graph + GPU 前处理
- Mouse：$mouseDescription
- 自瞄按键：鼠标右键
- 急停键：End

## 安全门

$safetyText
若本任务要求 source timing，Launch 前须在源机 `HPSAZZ` 前台运行
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\Xen\scripts\run_ndi_clock_source.ps1"
并保持到 Run 结束；缺少时钟样本会使 automatic gate 失败。

## 操作步骤

$($steps -join "`n")

## 人工观察

$($checks -join "`n")

## Launch 命令

```powershell
$launch
```

应用退出后脚本会收集本轮新增 Runtime CSV/JSON 和日志。完成后请将上述人工观察直接发送到当前对话，
由代理记录到 `OBSERVATION.md`、回收自动证据并继续后续流程；不需要手工编辑观察文件。
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
$aimControlDiagnosticsScript = Join-Path $PackageRoot `
    "tools\aim_control_diagnostics.ps1"
if (-not (Test-Path -LiteralPath $aimControlDiagnosticsScript -PathType Leaf)) {
    throw "完整发布包缺少 Aim 控制诊断助手。"
}

if ($Mode -eq "Prepare") {
    if ($AllowPhysicalOutput.IsPresent -or
        -not [string]::IsNullOrWhiteSpace($PhysicalOutputConfirmation)) {
        throw "Prepare 只生成任务，不接受 Launch 物理输出授权。"
    }
    $manifestResult = Read-Manifest
    $modelName = Get-ModelName $manifestResult.Value
    Assert-TaskManifestFiles $manifestResult.Value $modelName `
        -AllowConfigMismatch
    $configText = New-ConfigText $modelName
    $definition = Get-ScenarioDefinition
    $scenarioSlug = if ($Scenario -eq "SuperJump") {
        $caseSlug = switch ($SuperJumpCase) {
            "Static" { "static" }
            "SustainedMove" { "sustained-move" }
            "Stop" { "stop" }
            "Reverse" { "reverse" }
        }
        "superjump-$caseSlug"
    } else {
        $Scenario.ToLowerInvariant()
    }
    $runId = if ($observeOnly) {
        "{0}-{1}-{2}-observe-only-{3}" -f
            (Get-Date -Format "yyyyMMdd-HHmmss"),
            $scenarioSlug, $Profile,
            [guid]::NewGuid().ToString("N").Substring(0, 8)
    } else {
        "{0}-{1}-{2}-{3}" -f
            (Get-Date -Format "yyyyMMdd-HHmmss"),
            $scenarioSlug, $Profile,
            [guid]::NewGuid().ToString("N").Substring(0, 8)
    }
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
        "generated:$taskId/$Scenario/$Profile/$outputModeRecord" $modelName
    $task = [ordered]@{
        schema = 2
        task_id = $taskId
        run_id = $runId
        scenario = $Scenario
        superjump_case = $SuperJumpCase
        profile = $Profile
        output_mode = $outputModeRecord
        require_source_timing = $RequireSourceTiming.IsPresent
        prepared_utc = [DateTime]::UtcNow.ToString("o")
        package_root = $PackageRoot
        package_commit = [string]$manifestResult.Value.git_commit
        package_validation = $packageValidationMode
        package_manifest = Get-FileEvidence $manifestResult.Path
        worker = Get-FileEvidence (
            Join-Path $PackageRoot "runtimes\nvidia\Xen.exe")
        acceptance_script = Get-FileEvidence (
            Join-Path $PackageRoot "tools\invoke_aim_manual_acceptance.ps1")
        aim_report = Get-FileEvidence $aimReportScript
        aim_control_diagnostics = Get-FileEvidence `
            $aimControlDiagnosticsScript
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
            smoothing = $Smoothing
            counts_per_pixel = if ($resolvedCountsPerPixelX -eq
                $resolvedCountsPerPixelY) { $resolvedCountsPerPixelX } else { $null }
            counts_per_pixel_x = $resolvedCountsPerPixelX
            counts_per_pixel_y = $resolvedCountsPerPixelY
            max_counts_per_frame = $MaxCountsPerFrame
            delay_compensation_enabled = $EnableDelayCompensation.IsPresent
            control_delay_ms = $ControlDelayMs
            max_delay_compensation_ms = $MaxDelayCompensationMs
            max_delay_compensation_percent = $MaxDelayCompensationPercent
            prediction_enabled = $Profile -eq "prediction"
            max_prediction_lead_percent = $maxPredictionLeadPercent
            predicted_gain = 0.5
        }
        mouse = [ordered]@{
            backend = "kmbox_net"
            ip = $kmboxIp
            port = $kmboxPort
            uuid = $kmboxUuid
            allow_send_input = -not $observeOnly
            allow_observe_only_control = $observeOnly
        }
    }
    Write-JsonAtomically (Join-Path $resolvedRun "task.json") $task
    Write-TextAtomically (Join-Path $resolvedRun "TASK.md") `
        (New-TaskMarkdown $definition $resolvedRun $runId)
    $observationRecords = $definition.observations | ForEach-Object {
        "- $_："
    }
    Write-TextAtomically (Join-Path $resolvedRun "OBSERVATION.md") @"
# 人工观察记录

- 运行 ID：$runId
- 场景：$Scenario
- 配置：$Profile
- 输出模式：$outputModeDisplay
- smoothing：$('{0:F6}' -f $Smoothing)
- counts-per-pixel X：$('{0:F6}' -f $resolvedCountsPerPixelX)
- counts-per-pixel Y：$('{0:F6}' -f $resolvedCountsPerPixelY)
- 单帧二维上限：$('{0:F6}' -f $MaxCountsPerFrame) counts
- 延迟补偿：$($EnableDelayCompensation.IsPresent)
- 固定控制延迟：$('{0:F6}' -f $ControlDelayMs) ms
- 执行人：
- 开始/结束时间：
- 是否完成全部操作：
- 是否触发 End 急停：
$($observationRecords -join "`n")
- 松开右键后的停止表现：
- 与上一配置相比的变化：
- 异常发生时间或复现步骤：
- 人工结论：通过 / 需调整 / 立即停止
"@
    Write-Host "Aim 人工任务已准备：$resolvedRun"
    Write-Host "  run_id=$runId"
    Write-Host "  scenario=$Scenario"
    Write-Host "  superjump_case=$SuperJumpCase"
    Write-Host "  profile=$Profile"
    Write-Host "  config_sha256=$($task.config.sha256)"
    if ($observeOnly) {
        Write-Host "本轮为无物理输出对照；请先人工复核 TASK.md。"
        Write-Host "以下命令禁止真实 KMBOX 输出，可在前台复制执行："
    } else {
        Write-Host "本轮尚未启动物理输出；请先人工复核 TASK.md。"
        Write-Host "以下命令会发送真实 KMBOX 输入，确认现场安全后可直接复制执行："
    }
    Write-Output (New-LaunchCommand $resolvedRun)
    exit 0
}

if ($Mode -eq "Launch") {
    if ($observeOnly) {
        if ($AllowPhysicalOutput.IsPresent -or
            -not [string]::IsNullOrWhiteSpace($PhysicalOutputConfirmation)) {
            throw "Observe-only Launch 禁止接受任何物理输出授权。"
        }
    } else {
        Assert-PhysicalAuthorization
    }
} elseif ($AllowPhysicalOutput.IsPresent -or
    -not [string]::IsNullOrWhiteSpace($PhysicalOutputConfirmation)) {
    throw "Recover 只回收已有报告，不接受真实物理输出授权。"
}
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    throw "$Mode 必须指定 Prepare 生成的 RunDirectory。"
}
$resolvedRun = [System.IO.Path]::GetFullPath($RunDirectory)
$taskPath = Join-Path $resolvedRun "task.json"
if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) {
    throw "人工任务缺少 task.json：$resolvedRun"
}
$task = Get-Content -LiteralPath $taskPath -Raw -Encoding utf8 |
    ConvertFrom-Json
$taskCountsPerPixelX = if ($task.aim.PSObject.Properties.Name -contains
    "counts_per_pixel_x") { [double]$task.aim.counts_per_pixel_x } else {
    [double]$task.aim.counts_per_pixel
}
$taskCountsPerPixelY = if ($task.aim.PSObject.Properties.Name -contains
    "counts_per_pixel_y") { [double]$task.aim.counts_per_pixel_y } else {
    [double]$task.aim.counts_per_pixel
}
$taskRequireSourceTiming = if ($task.PSObject.Properties.Name -contains
    "require_source_timing") { [bool]$task.require_source_timing } else {
    $false
}
$taskOutputMode = if ($task.PSObject.Properties.Name -contains "output_mode") {
    [string]$task.output_mode
} else {
    "physical"
}
$taskAllowSendInput = if ($task.mouse.PSObject.Properties.Name -contains
    "allow_send_input") { [bool]$task.mouse.allow_send_input } else { $true }
$taskAllowObserveOnlyControl = if (
    $task.mouse.PSObject.Properties.Name -contains
        "allow_observe_only_control") {
    [bool]$task.mouse.allow_observe_only_control
} else { $false }
if ([int]$task.schema -notin @(1, 2) -or
    [string]$task.task_id -ne $taskId -or
    [string]$task.scenario -ne $Scenario -or
    [string]$task.superjump_case -ne $SuperJumpCase -or
    [string]$task.profile -ne $Profile -or
    $taskOutputMode -ne $outputModeRecord -or
    $taskAllowSendInput -ne (-not $observeOnly) -or
    $taskAllowObserveOnlyControl -ne $observeOnly -or
    $taskRequireSourceTiming -ne $RequireSourceTiming.IsPresent -or
    [string]$task.package_validation -ne $packageValidationMode -or
    [string]$task.package_root -ne $PackageRoot -or
    [double]$task.aim.smoothing -ne $Smoothing -or
    $taskCountsPerPixelX -ne $resolvedCountsPerPixelX -or
    $taskCountsPerPixelY -ne $resolvedCountsPerPixelY -or
    [double]$task.aim.max_counts_per_frame -ne $MaxCountsPerFrame -or
    [bool]$task.aim.delay_compensation_enabled -ne
        $EnableDelayCompensation.IsPresent -or
    [double]$task.aim.control_delay_ms -ne $ControlDelayMs -or
    [double]$task.aim.max_delay_compensation_ms -ne
        $MaxDelayCompensationMs -or
    [double]$task.aim.max_delay_compensation_percent -ne
        $MaxDelayCompensationPercent) {
    throw "$Mode 参数与 Prepare 任务不一致。"
}

$automaticRoot = Join-Path $resolvedRun "automatic"
$newReports = @()
$reportEvidence = @()
$logEvidence = @()
$startedUtc = $null
$endedUtc = $null
$process = $null
if ($Mode -eq "Launch") {
    $activeConfig = Join-Path $PackageRoot "config.ini"
    $activeHash = (Get-FileHash -LiteralPath $activeConfig -Algorithm SHA256).Hash
    if ($activeHash -ne [string]$task.config.sha256) {
        throw "当前完整包 config.ini 不是本任务 Prepare 的配置。"
    }
    $manifestResult = Read-Manifest
    $manifestEvidence = Get-FileEvidence $manifestResult.Path
    if ($manifestEvidence.length -ne [long]$task.package_manifest.length -or
        $manifestEvidence.sha256 -ne [string]$task.package_manifest.sha256) {
        throw "当前 manifest.json 不是本任务 Prepare 绑定的清单。"
    }
    $modelName = Get-ModelName $manifestResult.Value
    Assert-TaskManifestFiles $manifestResult.Value $modelName

    $runtimeRoot = Join-Path $PackageRoot "cache\runtime"
    $before = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    if (Test-Path -LiteralPath $runtimeRoot) {
        foreach ($file in @(Get-ChildItem -LiteralPath $runtimeRoot -File)) {
            [void]$before.Add($file.FullName)
        }
    }
    $startedUtc = [DateTime]::UtcNow
    if ($observeOnly) {
        Write-Host "即将启动无物理输出对照任务：$($task.run_id)"
        Write-Host "Mouse 必须保持 DISABLED；按 TASK.md 操作并观察画面。"
    } else {
        Write-Host "即将启动真实 KMBOX 输出任务：$($task.run_id)"
        Write-Host "确认 End 急停可用；程序启动后仍需人工武装并按 TASK.md 操作。"
    }
    $process = Start-Process -FilePath $launcher -WorkingDirectory $PackageRoot `
        -PassThru -Wait
    $endedUtc = [DateTime]::UtcNow

    New-Item -ItemType Directory -Path $automaticRoot -Force | Out-Null
    if (Test-Path -LiteralPath $runtimeRoot) {
        $newReports = @(Get-ChildItem -LiteralPath $runtimeRoot -File |
            Where-Object { -not $before.Contains($_.FullName) } |
            Sort-Object Name)
    }
    if ($newReports.Count -eq 0) {
        throw "应用退出后未发现本轮新增 Runtime 报告。"
    }
    foreach ($file in $newReports) {
        $destination = Join-Path $automaticRoot $file.Name
        Copy-Item -LiteralPath $file.FullName -Destination $destination
        $reportEvidence += Get-FileEvidence $destination
    }
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
} else {
    $runConfigEvidence = Get-FileEvidence (Join-Path $resolvedRun "config.ini")
    if ($runConfigEvidence.length -ne [long]$task.config.length -or
        $runConfigEvidence.sha256 -ne [string]$task.config.sha256) {
        throw "Recover Run config.ini 与 Prepare 证据不一致。"
    }
    foreach ($helper in @(
            [pscustomobject]@{
                path = $aimReportScript
                expected = $task.aim_report
            },
            [pscustomobject]@{
                path = $aimControlDiagnosticsScript
                expected = $task.aim_control_diagnostics
            })) {
        $actual = Get-FileEvidence $helper.path
        if ($actual.length -ne [long]$helper.expected.length -or
            $actual.sha256 -ne [string]$helper.expected.sha256) {
            throw "Recover 报告助手与 Prepare 证据不一致：$($helper.path)"
        }
    }
    if (-not (Test-Path -LiteralPath $automaticRoot -PathType Container)) {
        throw "Recover 缺少 Run automatic 目录：$resolvedRun"
    }
    $newReports = @(Get-ChildItem -LiteralPath $automaticRoot -File |
        Sort-Object Name)
    if ($newReports.Count -eq 0) {
        throw "Recover automatic 目录没有 Runtime 报告：$resolvedRun"
    }
    foreach ($file in $newReports) {
        $reportEvidence += Get-FileEvidence $file.FullName
    }
    $copiedLogRoot = Join-Path $resolvedRun "logs"
    if (Test-Path -LiteralPath $copiedLogRoot -PathType Container) {
        foreach ($file in @(Get-ChildItem -LiteralPath $copiedLogRoot -File)) {
            $logEvidence += Get-FileEvidence $file.FullName
        }
    }
}

. $aimReportScript
. $aimControlDiagnosticsScript
$jsonReports = @($newReports | Where-Object { $_.Extension -ieq ".json" })
$segments = @()
$allSamples = @()
$sampleCount = [uint64]0
$successful = [uint64]0
$failed = [uint64]0
$reportDropped = [uint64]0
$runtimeDropped = [uint64]0
$reportSchemas = [System.Collections.Generic.HashSet[int]]::new()
foreach ($file in $jsonReports) {
    $report = Get-Content -LiteralPath $file.FullName -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ($null -eq $report.samples) { continue }
    $reportSchema = [int]$report.schema
    if ($reportSchema -notin @(13, 14, 15, 16)) {
        throw "Aim 人工任务 Runtime 报告 schema 不受支持：$reportSchema；$($file.FullName)"
    }
    [void]$reportSchemas.Add($reportSchema)
    $samples = @($report.samples)
    $allSamples += $samples
    $sampleCount += [uint64]$report.sample_count
    $successful += [uint64]$report.successful_samples
    $failed += [uint64]$report.failed_samples
    $reportDropped += [uint64]$report.report_samples_dropped
    $runtimeDropped += [uint64]$report.runtime_samples_dropped
    $segments += [ordered]@{
        schema = $reportSchema
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
$requireMatchedObservation = @($reportSchemas | Where-Object {
    $_ -lt 15
}).Count -eq 0
$aimSummary = Get-XenAimReportSummary -Samples $allSamples `
    -PredictionEnabled $predictionState `
    -MaxPredictionLeadPercent $maxPredictionLeadPercent `
    -RequireMatchedObservation:$requireMatchedObservation
$controlDiagnostics = Get-XenAimControlDiagnosticsSummary -Samples $allSamples
$providerMismatch = @($segments | Where-Object {
    $_.provider -ne "TensorrtExecutionProvider"
}).Count
$captureMismatch = @($segments | Where-Object {
    $_.capture_backend -ne "NDI"
}).Count
$mouseCommands = @($allSamples | Where-Object { [bool]$_.mouse_sent }).Count
$aimLockActiveSamples = @($allSamples | Where-Object {
    $_.PSObject.Properties.Name -contains "aim_lock_active" -and
        [bool]$_.aim_lock_active
}).Count
$sourceTimingEvidence = Get-XenSourceTimingEvidence -Samples $allSamples
$sourceTimingValidSamples = [uint64]$sourceTimingEvidence.valid_samples
$sourceTimingGatePassed = -not $RequireSourceTiming.IsPresent -or
    [string]$sourceTimingEvidence.diagnostic -eq "VALID"
$mouseBackendCompletionSamples = @($allSamples | Where-Object {
    ($_.PSObject.Properties.Name -contains
        "mouse_backend_completion_timing_valid" -and
        [bool]$_.mouse_backend_completion_timing_valid) -or
    ($_.PSObject.Properties.Name -contains "mouse_completion_timing_valid" -and
        [bool]$_.mouse_completion_timing_valid)
}).Count
$mouseProtocolAckSamples = @($allSamples | Where-Object {
    $_.PSObject.Properties.Name -contains "mouse_protocol_ack_timing_valid" -and
        [bool]$_.mouse_protocol_ack_timing_valid
}).Count
$mousePhysicalEffectSamples = @($allSamples | Where-Object {
    $_.PSObject.Properties.Name -contains "mouse_physical_effect_timing_valid" -and
        [bool]$_.mouse_physical_effect_timing_valid
}).Count
$outputEvidenceComplete = if ($observeOnly) {
    $reportSchemas.Count -eq 1 -and $reportSchemas.Contains(16) -and
    $aimLockActiveSamples -gt 0 -and
    $mouseCommands -eq 0 -and
    $mouseBackendCompletionSamples -eq 0 -and
    $mouseProtocolAckSamples -eq 0 -and
    $mousePhysicalEffectSamples -eq 0 -and
    [uint64]$aimSummary.command_frames -gt 0 -and
    [uint64]$aimSummary.precomputed_command_frames -eq
        [uint64]$aimSummary.command_frames
} else {
    $mouseCommands -gt 0
}
$complete = $failed -eq 0 -and $reportDropped -eq 0 -and
    $runtimeDropped -eq 0 -and $providerMismatch -eq 0 -and
    $captureMismatch -eq 0 -and $outputEvidenceComplete -and
    [uint64]$aimSummary.violation_count -eq 0 -and
    [uint64]$controlDiagnostics.diagnostics_missing_frames -eq 0 -and
    [bool]$controlDiagnostics.reverse_translation_detail_diagnostics_available -and
    $sourceTimingGatePassed
$summary = [ordered]@{
    schema = 2
    task_id = $taskId
    run_id = [string]$task.run_id
    scenario = $Scenario
    superjump_case = $SuperJumpCase
    profile = $Profile
    output_mode = $outputModeRecord
    physical_output_enabled = -not $observeOnly
    smoothing = $Smoothing
    counts_per_pixel = if ($resolvedCountsPerPixelX -eq
        $resolvedCountsPerPixelY) { $resolvedCountsPerPixelX } else { $null }
    counts_per_pixel_x = $resolvedCountsPerPixelX
    counts_per_pixel_y = $resolvedCountsPerPixelY
    max_counts_per_frame = $MaxCountsPerFrame
    enable_delay_compensation = $EnableDelayCompensation.IsPresent
    control_delay_ms = $ControlDelayMs
    max_delay_compensation_ms = $MaxDelayCompensationMs
    max_delay_compensation_percent = $MaxDelayCompensationPercent
    collection_mode = $Mode
    runtime_report_schemas = @($reportSchemas | Sort-Object)
    recovered_utc = [DateTime]::UtcNow.ToString("o")
    started_utc = if ($null -eq $startedUtc) { $null } else {
        $startedUtc.ToString("o")
    }
    ended_utc = if ($null -eq $endedUtc) { $null } else {
        $endedUtc.ToString("o")
    }
    launcher_exit_code = if ($null -eq $process) { $null } else {
        [int]$process.ExitCode
    }
    automatic_complete = $complete
    awaiting_human_observation = $true
    segment_count = $segments.Count
    sample_count = $sampleCount
    successful_samples = $successful
    failed_samples = $failed
    report_samples_dropped = $reportDropped
    runtime_samples_dropped = $runtimeDropped
    mouse_commands = $mouseCommands
    aim_lock_active_samples = $aimLockActiveSamples
    source_timing_valid_samples = $sourceTimingValidSamples
    source_timing_required = $RequireSourceTiming.IsPresent
    source_timing_gate_passed = $sourceTimingGatePassed
    source_timing_diagnostic = [string]$sourceTimingEvidence.diagnostic
    source_clock_no_sample_frames =
        [uint64]$sourceTimingEvidence.no_sample_frames
    source_clock_sample_count_max = $sourceTimingEvidence.sample_count_max
    source_clock_session_ids = @($sourceTimingEvidence.session_ids)
    source_clock_status_counts = $sourceTimingEvidence.status_counts
    mouse_backend_completion_samples = $mouseBackendCompletionSamples
    mouse_protocol_ack_samples = $mouseProtocolAckSamples
    mouse_physical_effect_samples = $mousePhysicalEffectSamples
    provider_mismatch_segments = $providerMismatch
    capture_mismatch_segments = $captureMismatch
    aim = $aimSummary
    control_diagnostics = $controlDiagnostics
    segments = $segments
    collected_files = $reportEvidence
    logs = $logEvidence
}
Write-JsonAtomically (Join-Path $resolvedRun "automatic-summary.json") $summary
Write-Host "Aim 人工任务自动证据已收集：$resolvedRun"
Write-Host "  samples=$sampleCount, failed=$failed, mouse_commands=$mouseCommands"
Write-Host "  aim_violations=$($aimSummary.violation_count)"
Write-Host "  control_diagnostics_schema=$($controlDiagnostics.schema)"
Write-Host "  source_timing=$($sourceTimingEvidence.diagnostic), required=$($RequireSourceTiming.IsPresent)"
Write-Host ("  reverse_translation_details=" +
    $controlDiagnostics.reverse_translation_detail_diagnostics_available)
Write-Host "  automatic_complete=$complete"
Write-Host "请将人工观察结果直接发送到当前对话；代理会记录 OBSERVATION.md 并继续回收证据。"
