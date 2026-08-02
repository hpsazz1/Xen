param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "DetectionStatic", "DetectionMoveLeft", "DetectionMoveRight",
        "DetectionShuttle", "DetectionSuperJump", "TrackingOcclusion",
        "MultiTargetSwitch", "ControlAcquire",
        "EmergencyStop", "Stability")]
    [string]$Stage,
    [ValidateSet("Prepare", "Launch", "Collect")]
    [string]$Mode = "Prepare",
    [string]$RunDirectory = "",
    [string]$OutputRoot = "",
    [switch]$AllowPhysicalOutput,
    [string]$PhysicalOutputConfirmation = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$fixedBuildDirectory = Join-Path $repositoryRoot "build-matrix-final-cpu"
$fixedOutputDirectory = Join-Path $fixedBuildDirectory "Release"
$fixedExecutable = Join-Path $fixedOutputDirectory "Xen.exe"
$fixedModelPath = Join-Path $fixedOutputDirectory "models\cs2_320_v8s.onnx"
$fixedDeploymentReport = Join-Path `
    $fixedOutputDirectory "xen-runtime-deployment.json"
$fixedModelSha256 =
    "6429726D9FE3C9F5E78BB7811A38D572EBA18D63176B117E72EA66A53A935C47"
$physicalConfirmation =
    "XEN_LIVE_GAME_ACCEPTANCE_SENDS_REAL_INPUT"
$expectedProvider = "CPUExecutionProvider"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot "cache\live-game-acceptance"
}

function Get-StageDefinition {
    param([Parameter(Mandatory = $true)][string]$Name)

    switch ($Name) {
        "DetectionStatic" {
            return [ordered]@{
                title = "静止目标检测"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "在私有或离线训练环境保持 2560x1440 和中心准星不变。",
                    "选择单个清晰目标，依次观察正面、侧面、蹲姿和不同距离。",
                    "启动 Runtime 和独立预览，连续观察约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "目标可见时是否持续有框",
                    "是否出现背景误框或错误类别",
                    "框位置和大小是否明显抖动",
                    "异常发生时的预览帧序号或大致时间"
                )
            }
        }
        "DetectionMoveLeft" {
            return [ordered]@{
                title = "目标向左移动检测"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "测试人物视角和准星全程固定，操作员不得用鼠标跟随目标。",
                    "单个目标从 ROI 右侧向左匀速横穿，至少重复 5 次。",
                    "每次保持相近起点、速度和路线，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "目标从右侧进入后检测框是否及时出现",
                    "向左移动过程中是否连续漏框或框明显滞后",
                    "从左侧离开后是否残留旧框",
                    "5 次重复结果是否一致"
                )
            }
        }
        "DetectionMoveRight" {
            return [ordered]@{
                title = "目标向右移动检测"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "测试人物视角和准星全程固定，操作员不得用鼠标跟随目标。",
                    "单个目标从 ROI 左侧向右匀速横穿，至少重复 5 次。",
                    "每次保持相近起点、速度和路线，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "目标从左侧进入后检测框是否及时出现",
                    "向右移动过程中是否连续漏框或框明显滞后",
                    "从右侧离开后是否残留旧框",
                    "5 次重复结果是否一致"
                )
            }
        }
        "DetectionShuttle" {
            return [ordered]@{
                title = "目标持续左右往复检测"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "测试人物视角和准星全程固定，操作员不得用鼠标跟随目标。",
                    "单个目标持续左右往复横穿 ROI，至少完成 5 个完整往返。",
                    "保持移动范围和节奏稳定，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "左右两个方向的检出和恢复是否存在明显差异",
                    "折返点是否出现漏框、错框或明显抖动",
                    "每次离开 ROI 后是否及时清除旧框",
                    "连续往返时异常是否逐渐累积"
                )
            }
        }
        "DetectionSuperJump" {
            return [ordered]@{
                title = "目标超级跳检测"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "测试人物视角和准星全程固定，操作员不得用鼠标跟随目标。",
                    "单个目标在 ROI 附近连续执行超级跳，至少完成 10 次可见跳跃。",
                    "保持起始区域和移动方向可复现，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "起跳、腾空和落地阶段是否持续有合理检测框",
                    "高速位移或姿态变化时最长连续漏框现象",
                    "检测框是否错误扩大、缩小或跳到背景",
                    "落地后是否及时恢复稳定"
                )
            }
        }
        "TrackingOcclusion" {
            return [ordered]@{
                title = "遮挡与重新出现跟踪"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "让单个目标在 ROI 内被掩体部分遮挡、完全遮挡后重新出现。",
                    "至少执行 5 次遮挡与重现，保持其他目标不进入 ROI。",
                    "观察 Aim 目标标记和检测框，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "部分遮挡是否保持合理目标",
                    "完全遮挡后是否及时清除目标",
                    "重新出现后是否平稳恢复",
                    "是否出现明显跳点或方向反转"
                )
            }
        }
        "MultiTargetSwitch" {
            return [ordered]@{
                title = "多目标交叉与选择"
                view_mode = "fixed"
                physical_output = $false
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "让两个或更多目标同时进入 ROI，并执行交叉、远近变化和相互遮挡。",
                    "至少执行 5 次交叉，测试期间不修改 Aim 参数。",
                    "观察当前 Aim 目标标记，约 30 秒后停止 Runtime。"
                )
                observations = @(
                    "目标选择是否稳定",
                    "交叉时是否发生无必要切换",
                    "原目标仍可见时是否突然跳到其他目标",
                    "切换后控制点是否越界或剧烈跳动"
                )
            }
        }
        "ControlAcquire" {
            return [ordered]@{
                title = "真实输入锁定与持续控制"
                view_mode = "controlled_output"
                physical_output = $true
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "仅在私有或离线训练环境执行，确认急停键 End 可用。",
                    "启动 Runtime 后人工武装，按住鼠标右键完成 20 次锁定与释放。",
                    "每次从相同初始偏差开始，出现异常立即松开右键并按 End。"
                )
                observations = @(
                    "20 次首次锁定成功次数",
                    "持续跟随是否平稳且无明显振荡",
                    "释放右键后是否立即停止发送",
                    "是否触及限幅、反向跳动或产生非预期位移"
                )
            }
        }
        "EmergencyStop" {
            return [ordered]@{
                title = "真实输入急停与人工复位"
                view_mode = "controlled_output"
                physical_output = $true
                recommended_seconds = 30
                minimum_samples = 1000
                instructions = @(
                    "仅在私有或离线训练环境执行，安全观察员必须在场。",
                    "武装并按住鼠标右键产生控制后按 End，确认输出立即停止。",
                    "保持 End 按住不应重复触发；释放后人工复位并重新武装一次。"
                )
                observations = @(
                    "End 按下后是否立即停止",
                    "持续按住 End 是否保持稳定急停状态",
                    "人工复位前是否拒绝重新输出",
                    "复位和重新武装后是否恢复正常"
                )
            }
        }
        "Stability" {
            return [ordered]@{
                title = "真实游戏画面五分钟稳定性"
                view_mode = "manual_free_play"
                physical_output = $false
                recommended_seconds = 300
                minimum_samples = 10000
                instructions = @(
                    "先打开私有或离线游戏场景并保持为前景，测试期间正常移动视角。",
                    "执行 Launch 后有 10 秒切回游戏；随后 XenBenchmark 自动运行 300 秒。",
                    "测试期间不得切换分辨率、模型、配置或显示输出。"
                )
                observations = @(
                    "游戏是否出现明显卡顿或画面异常",
                    "测试期间是否发生切屏、分辨率变化或其他干扰",
                    "是否观察到持续漏检、恢复失败或明显资源异常",
                    "任何异常的大致时间"
                )
            }
        }
    }
    throw "未知实机测试环节：$Name"
}

function Get-FileEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)

    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "固定测试文件不得是重解析点：$Path"
    }
    return [ordered]@{
        path = $file.FullName
        bytes = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName `
            -Algorithm SHA256).Hash
        last_write_utc = $file.LastWriteTimeUtc.ToString("o")
        file_version = [string]$file.VersionInfo.FileVersion
        product_version = [string]$file.VersionInfo.ProductVersion
    }
}

function Write-TextAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $temporary = "$Path.pending-$([guid]::NewGuid().ToString('N'))"
    try {
        [System.IO.File]::WriteAllText(
            $temporary, $Content,
            (New-Object System.Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $backup = "$Path.backup-$([guid]::NewGuid().ToString('N'))"
            try {
                [System.IO.File]::Replace($temporary, $Path, $backup)
            } finally {
                if (Test-Path -LiteralPath $backup) {
                    Remove-Item -LiteralPath $backup -Force
                }
            }
        } else {
            Move-Item -LiteralPath $temporary -Destination $Path
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Write-JsonAtomically {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    Write-TextAtomically -Path $Path `
        -Content (($Value | ConvertTo-Json -Depth 10) + "`n")
}

function Assert-FixedInputs {
    foreach ($entry in @(
            @{ Path = $fixedExecutable; Description = "固定 Xen.exe" },
            @{ Path = $fixedModelPath; Description = "固定测试模型" },
            @{ Path = $fixedDeploymentReport; Description = "运行库部署报告" },
            @{ Path = (Join-Path $fixedBuildDirectory "CMakeCache.txt");
               Description = "CPU 构建 CMakeCache" })) {
        if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf)) {
            throw "$($entry.Description)不存在：$($entry.Path)"
        }
    }
    $modelHash = (Get-FileHash -LiteralPath $fixedModelPath `
        -Algorithm SHA256).Hash
    if ($modelHash -ne $fixedModelSha256) {
        throw "固定测试模型 SHA-256 不符合基线：expected=$fixedModelSha256, actual=$modelHash"
    }
}

function Assert-PhysicalAuthorization {
    param([Parameter(Mandatory = $true)][object]$Definition)

    if ($Definition.physical_output) {
        if (-not $AllowPhysicalOutput.IsPresent -or
            $PhysicalOutputConfirmation -ne $physicalConfirmation) {
            throw "该环节会发送真实输入，必须同时提供 -AllowPhysicalOutput 和固定确认令牌。"
        }
    } elseif ($AllowPhysicalOutput.IsPresent -or
              -not [string]::IsNullOrWhiteSpace(
                  $PhysicalOutputConfirmation)) {
        throw "无物理输出环节不得携带物理输出授权参数。"
    }
}

function New-FixedConfigText {
    param([Parameter(Mandatory = $true)][bool]$PhysicalOutput)

    $allow = if ($PhysicalOutput) { "true" } else { "false" }
    return @"
[detector]
model_path=cs2_320_v8s.onnx
backend=cpu
device_id=0
openvino_device=cpu
input_width=0
input_height=0
conf_threshold=0.250000
nms_threshold=0.450000
top_k=300
output_format=auto
enable_fp16=false
enable_trt_cuda_graph=false
enable_gpu_preprocess=false
trt_cache_path=cache/tensorrt

[capture]
backend=desktop_duplication
adapter_index=0
output_index=0
enable_d3d11_cuda_interop=false
enable_d3d11_directml_interop=false
udp_url=udp://0.0.0.0:5000
udp_read_timeout_ms=250
udp_disconnect_timeout_ms=2000
udp_frame_layout=full_frame_1_to_1
udp_source_width=0
udp_source_height=0
ndi_source_name=Auto
ndi_discovery_timeout_ms=5000
ndi_receive_timeout_ms=50
ndi_disconnect_timeout_ms=2000
ndi_frame_layout=full_frame_1_to_1
ndi_source_width=0
ndi_source_height=0
ndi_require_frame_metadata=false
roi_width=320
roi_height=320
center_roi=true
roi_x=0
roi_y=0
acquire_timeout_ms=16

[aim]
person_class_ids=0
head_class_ids=1
high_confidence=0.250000
low_confidence=0.100000
min_confirmed_hits=2
max_lost_frames=8
min_iou=0.100000
max_center_distance=0.250000
switch_margin=0.200000
switch_confirm_frames=3
switch_cooldown_frames=5
body_aim_height_ratio=0.350000
deadzone_pixels=1.500000
smoothing=0.350000
counts_per_pixel_x=0.500000
counts_per_pixel_y=0.500000
max_counts_per_frame=50.000000
predicted_gain=0.500000

[mouse]
backend=win32_send_input
allow_send_input=$allow
kmbox_ip=
kmbox_port=0
kmbox_uuid=
kmbox_connect_timeout_ms=1000
kmbox_command_timeout_ms=300
makcu_port=
makcu_baud_rate=115200
makcu_connect_timeout_ms=1000
makcu_command_timeout_ms=300

[keyboard]
aim_hold_virtual_key=2
emergency_virtual_key=35

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
theme=light
"@
}

function Get-GitIdentity {
    $head = (& git -C $repositoryRoot rev-parse HEAD 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
        throw "无法读取 Git HEAD。"
    }
    $trackedStatus = (& git -C $repositoryRoot status --porcelain `
        --untracked-files=no 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "无法读取 Git 工作区状态。"
    }
    return [ordered]@{
        head = $head
        tracked_dirty = -not [string]::IsNullOrWhiteSpace($trackedStatus)
        tracked_status = $trackedStatus
    }
}

function Resolve-RunDirectoryForPrepare {
    if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        $name = "{0}-{1}-{2}" -f (
            Get-Date -Format "yyyyMMdd-HHmmss"),
            $Stage.ToLowerInvariant(),
            [guid]::NewGuid().ToString("N").Substring(0, 8)
        return [System.IO.Path]::GetFullPath((Join-Path $OutputRoot $name))
    }
    return [System.IO.Path]::GetFullPath($RunDirectory)
}

function Resolve-ExistingRunDirectory {
    if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        throw "$Mode 模式必须通过 -RunDirectory 指定已生成任务目录。"
    }
    $resolved = [System.IO.Path]::GetFullPath($RunDirectory)
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "任务目录不存在：$resolved"
    }
    return $resolved
}

function New-TaskMarkdown {
    param(
        [Parameter(Mandatory = $true)][object]$Definition,
        [Parameter(Mandatory = $true)][string]$ResolvedRunDirectory,
        [Parameter(Mandatory = $true)][string]$RunId
    )

    $steps = for ($index = 0; $index -lt $Definition.instructions.Count;
                  ++$index) {
        "{0}. {1}" -f ($index + 1), $Definition.instructions[$index]
    }
    $observations = $Definition.observations |
        ForEach-Object { "- [ ] $_" }
    $safety = if ($Definition.physical_output) {
        "本环节允许真实 Win32 输入；启动时必须再次提供双重授权，人工武装前确认 End 急停。"
    } else {
        "本环节强制 allow_send_input=false；禁止武装，Mouse 必须显示 DISABLED。"
    }
    return @"
# Xen 实机测试任务

- 任务 ID：LIVE-GAME-ACCEPT-001
- 运行 ID：$RunId
- 环节：$Stage / $($Definition.title)
- 视角模式：$($Definition.view_mode)
- 固定模型：$fixedModelPath
- 固定 Provider：$expectedProvider
- 建议时长：$($Definition.recommended_seconds) 秒
- 任务目录：$ResolvedRunDirectory

## 安全门

$safety

## 操作步骤

$($steps -join "`n")

除 Stability 外，人工点击“启动”开始 Runtime，完成场景后点击“停止”，确认报告发布后关闭 Xen。
测试中不得保存配置、重载模型或修改任何参数。

## 必须反馈的观测

$($observations -join "`n")

同时反馈：是否按计划完成、是否出现干扰、异常的大致时间或预览帧序号、其他备注。

## 自动记录

Xen 退出后脚本会生成 automatic-summary.json；人工反馈保存在 observation-template.json 的副本
或直接提交给分析人员。只有自动报告和人工观测同时具备，才能形成结论与下一步计划。
"@
}

function Prepare-Task {
    $definition = Get-StageDefinition $Stage
    Assert-FixedInputs
    Assert-PhysicalAuthorization $definition
    $git = Get-GitIdentity

    $resolvedRunDirectory = Resolve-RunDirectoryForPrepare
    if (Test-Path -LiteralPath $resolvedRunDirectory) {
        throw "任务目录已存在，拒绝覆盖：$resolvedRunDirectory"
    }
    New-Item -ItemType Directory -Path $resolvedRunDirectory | Out-Null
    New-Item -ItemType Directory -Path (
        Join-Path $resolvedRunDirectory "automatic") | Out-Null

    $configPath = Join-Path $resolvedRunDirectory "config.ini"
    Write-TextAtomically -Path $configPath `
        -Content (New-FixedConfigText $definition.physical_output)

    $runId = Split-Path -Leaf $resolvedRunDirectory
    $identity = [ordered]@{
        git = $git
        executable = Get-FileEvidence $fixedExecutable
        model = Get-FileEvidence $fixedModelPath
        deployment_report = Get-FileEvidence $fixedDeploymentReport
        config = Get-FileEvidence $configPath
        expected_provider = $expectedProvider
        expected_capture_backend = "DESKTOP_DUPLICATION"
        expected_geometry = [ordered]@{
            source_width = 2560
            source_height = 1440
            encoded_width = 2560
            encoded_height = 1440
            roi_x = 1120
            roi_y = 560
            roi_width = 320
            roi_height = 320
            scale_x = 1.0
            scale_y = 1.0
        }
    }
    $task = [ordered]@{
        schema = 1
        task_id = "LIVE-GAME-ACCEPT-001"
        run_id = $runId
        stage = $Stage
        title = $definition.title
        status = "PREPARED"
        generated_utc = [DateTime]::UtcNow.ToString("o")
        physical_output = [bool]$definition.physical_output
        view_mode = $definition.view_mode
        recommended_seconds = [int]$definition.recommended_seconds
        minimum_samples = [int]$definition.minimum_samples
        fixed_identity = $identity
        instructions = $definition.instructions
        required_observations = $definition.observations
        safety = [ordered]@{
            private_or_offline_environment_required = $true
            physical_confirmation_recorded =
                [bool]$definition.physical_output
            confirmation_token_persisted = $false
            aim_hold_virtual_key = 2
            emergency_virtual_key = 35
        }
    }
    Write-JsonAtomically -Path (
        Join-Path $resolvedRunDirectory "task.json") -Value $task
    Write-JsonAtomically -Path (
        Join-Path $resolvedRunDirectory "identity-before.json") `
        -Value $identity
    Write-JsonAtomically -Path (
        Join-Path $resolvedRunDirectory "observation-template.json") `
        -Value ([ordered]@{
            schema = 1
            task_id = "LIVE-GAME-ACCEPT-001"
            run_id = $runId
            stage = $Stage
            completed = $null
            interference_observed = $null
            observation_items = @($definition.observations |
                ForEach-Object {
                    [ordered]@{ item = $_; result = $null; note = "" }
                })
            anomaly_time_or_sequence = ""
            notes = ""
            operator = ""
            reviewed_utc = ""
        })
    Write-TextAtomically -Path (
        Join-Path $resolvedRunDirectory "TASK.md") `
        -Content (New-TaskMarkdown $definition $resolvedRunDirectory $runId)

    Write-Host "实机测试任务已生成：$resolvedRunDirectory"
    Write-Host "任务单：$(Join-Path $resolvedRunDirectory 'TASK.md')"
    Write-Output $resolvedRunDirectory
}

function Load-And-ValidateTask {
    param([Parameter(Mandatory = $true)][string]$ResolvedRunDirectory)

    Assert-FixedInputs
    $taskPath = Join-Path $ResolvedRunDirectory "task.json"
    $configPath = Join-Path $ResolvedRunDirectory "config.ini"
    if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "任务目录缺少 task.json 或 config.ini：$ResolvedRunDirectory"
    }
    $task = Get-Content -LiteralPath $taskPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($task.schema -ne 1 -or
        $task.task_id -ne "LIVE-GAME-ACCEPT-001" -or
        $task.stage -ne $Stage) {
        throw "任务清单与请求环节不一致。"
    }
    $checks = @(
        @{ Name = "配置"; Current = Get-FileEvidence $configPath;
           Expected = $task.fixed_identity.config },
        @{ Name = "模型"; Current = Get-FileEvidence $fixedModelPath;
           Expected = $task.fixed_identity.model },
        @{ Name = "Xen.exe"; Current = Get-FileEvidence $fixedExecutable;
           Expected = $task.fixed_identity.executable },
        @{ Name = "部署报告"; Current = Get-FileEvidence $fixedDeploymentReport;
           Expected = $task.fixed_identity.deployment_report }
    )
    foreach ($check in $checks) {
        if ($check.Current.sha256 -ne $check.Expected.sha256 -or
            $check.Current.bytes -ne $check.Expected.bytes) {
            throw "$($check.Name)在任务生成后发生变化，拒绝继续。"
        }
    }
    return $task
}

function Get-AutomaticReportPaths {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedRunDirectory,
        [Parameter(Mandatory = $true)][string]$StageName
    )

    if ($StageName -eq "Stability") {
        $path = Join-Path $ResolvedRunDirectory "automatic\runtime.json"
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            return @($path)
        }
        return @()
    }
    $runtimeDirectory = Join-Path $ResolvedRunDirectory "cache\runtime"
    if (-not (Test-Path -LiteralPath $runtimeDirectory -PathType Container)) {
        return @()
    }
    return @(Get-ChildItem -LiteralPath $runtimeDirectory -File `
        -Filter "*.json" | Sort-Object Name | Select-Object -ExpandProperty FullName)
}

function Collect-Reports {
    param([Parameter(Mandatory = $true)][string]$ResolvedRunDirectory)

    $task = Load-And-ValidateTask $ResolvedRunDirectory
    $paths = @(Get-AutomaticReportPaths $ResolvedRunDirectory $Stage)
    if ($paths.Count -eq 0) {
        throw "没有找到 Runtime JSON 报告；请先完成测试并停止 Runtime。"
    }
    $failures = @()
    if ($paths.Count -ne 1) {
        $failures += "单个测试环节必须且只能生成一份 Runtime 报告：actual=$($paths.Count)"
    }
    $segments = @()
    $sampleCount = [uint64]0
    $successfulSamples = [uint64]0
    $failedSamples = [uint64]0
    $reportDropped = [uint64]0
    $runtimeDropped = [uint64]0
    $mouseCommands = [uint64]0
    foreach ($path in $paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $failures += "报告不存在：$path"
            continue
        }
        $csvPath = [System.IO.Path]::ChangeExtension($path, ".csv")
        if (-not (Test-Path -LiteralPath $csvPath -PathType Leaf)) {
            $failures += "JSON 缺少同名 CSV：$path"
        }
        $report = Get-Content -LiteralPath $path -Raw -Encoding UTF8 |
            ConvertFrom-Json
        if ($report.schema -ne 6) {
            $failures += "报告 schema 不是 6：$path"
        }
        if ($report.provider -ne $expectedProvider -or
            $report.final_snapshot.provider -ne $expectedProvider) {
            $failures += "实际 Provider 不是 $expectedProvider：$path"
        }
        if ($report.capture_backend -ne "DESKTOP_DUPLICATION") {
            $failures += "实际 Capture 不是 DESKTOP_DUPLICATION：$path"
        }
        if ([System.IO.Path]::GetFullPath([string]$report.model_path) -ne
            [System.IO.Path]::GetFullPath($fixedModelPath)) {
            $failures += "报告模型不是固定测试模型：$path"
        }
        $snapshot = $report.final_snapshot
        if ($snapshot.source_width -ne 2560 -or
            $snapshot.source_height -ne 1440 -or
            $snapshot.encoded_width -ne 2560 -or
            $snapshot.encoded_height -ne 1440 -or
            $snapshot.roi_x -ne 1120 -or $snapshot.roi_y -ne 560 -or
            $snapshot.roi_width -ne 320 -or
            $snapshot.roi_height -ne 320 -or
            [double]$snapshot.source_pixels_per_pixel_x -ne 1.0 -or
            [double]$snapshot.source_pixels_per_pixel_y -ne 1.0) {
            $failures += "报告几何不符合固定 2560x1440 中心 ROI：$path"
        }
        if ([bool]$snapshot.output_allowed_by_config -ne
            [bool]$task.physical_output) {
            $failures += "物理输出配置与任务不一致：$path"
        }
        if ([int64]$report.failed_samples -ne 0 -or
            [int64]$snapshot.failed_frames -ne 0 -or
            -not [string]::IsNullOrEmpty([string]$snapshot.last_error)) {
            $failures += "报告包含失败样本、失败帧或 Runtime 错误：$path"
        }
        if ([uint64]$report.successful_samples +
            [uint64]$report.failed_samples -ne
            [uint64]$report.sample_count) {
            $failures += "报告样本数不守恒：$path"
        }
        if ([int64]$report.report_samples_dropped -ne 0 -or
            [int64]$report.runtime_samples_dropped -ne 0) {
            $failures += "报告包含诊断样本丢弃：$path"
        }
        if (-not [bool]$task.physical_output -and
            [int64]$snapshot.mouse_commands -ne 0) {
            $failures += "无物理输出任务产生了 Mouse 命令：$path"
        }
        $sampleCount += [uint64]$report.sample_count
        $successfulSamples += [uint64]$report.successful_samples
        $failedSamples += [uint64]$report.failed_samples
        $reportDropped += [uint64]$report.report_samples_dropped
        $runtimeDropped += [uint64]$report.runtime_samples_dropped
        $mouseCommands += [uint64]$snapshot.mouse_commands
        $segments += [ordered]@{
            json = Get-FileEvidence $path
            csv = if (Test-Path -LiteralPath $csvPath) {
                Get-FileEvidence $csvPath
            } else { $null }
            session_id = $report.session_id
            sample_count = [uint64]$report.sample_count
            successful_samples = [uint64]$report.successful_samples
            failed_samples = [uint64]$report.failed_samples
            final_snapshot = $snapshot
            timing = $report.timing
        }
    }
    if ($sampleCount -lt [uint64]$task.minimum_samples) {
        $failures +=
            "有效样本不足：required=$($task.minimum_samples), actual=$sampleCount"
    }
    if ([bool]$task.physical_output -and $mouseCommands -eq 0) {
        $failures += "物理输出环节没有产生任何 Mouse 命令。"
    }
    $identityAfter = [ordered]@{
        executable = Get-FileEvidence $fixedExecutable
        model = Get-FileEvidence $fixedModelPath
        deployment_report = Get-FileEvidence $fixedDeploymentReport
        config = Get-FileEvidence (
            Join-Path $ResolvedRunDirectory "config.ini")
    }
    Write-JsonAtomically -Path (
        Join-Path $ResolvedRunDirectory "identity-after.json") `
        -Value $identityAfter
    $summary = [ordered]@{
        schema = 1
        task_id = "LIVE-GAME-ACCEPT-001"
        run_id = $task.run_id
        stage = $Stage
        collected_utc = [DateTime]::UtcNow.ToString("o")
        automatic_complete = $failures.Count -eq 0
        awaiting_human_observation = $true
        segment_count = $segments.Count
        sample_count = $sampleCount
        successful_samples = $successfulSamples
        failed_samples = $failedSamples
        report_samples_dropped = $reportDropped
        runtime_samples_dropped = $runtimeDropped
        mouse_commands = $mouseCommands
        failures = @($failures)
        segments = @($segments)
    }
    $summaryPath = Join-Path `
        $ResolvedRunDirectory "automatic-summary.json"
    Write-JsonAtomically -Path $summaryPath -Value $summary
    Write-Host "自动报告已汇总：$summaryPath"
    Write-Host "automatic_complete=$($summary.automatic_complete), samples=$sampleCount, failures=$($failures.Count)"
    Write-Output $summaryPath
}

function Launch-Task {
    param([Parameter(Mandatory = $true)][string]$ResolvedRunDirectory)

    $definition = Get-StageDefinition $Stage
    Assert-PhysicalAuthorization $definition
    $task = Load-And-ValidateTask $ResolvedRunDirectory
    $existingReports = @(
        Get-AutomaticReportPaths $ResolvedRunDirectory $Stage |
            Where-Object { Test-Path -LiteralPath $_ })
    if ($existingReports.Count -ne 0 -or
        (Test-Path -LiteralPath (
            Join-Path $ResolvedRunDirectory "automatic-summary.json"))) {
        throw "任务目录已经包含报告，拒绝重复启动：$ResolvedRunDirectory"
    }

    if ($Stage -eq "Stability") {
        Write-Host "10 秒后启动五分钟无界面基准，请立即切回固定游戏场景。"
        for ($remaining = 10; $remaining -ge 1; --$remaining) {
            Write-Host "启动倒计时：$remaining"
            Start-Sleep -Seconds 1
        }
        $benchmarkScript = Join-Path $PSScriptRoot "benchmark_runtime.ps1"
        $reportPrefix = Join-Path $ResolvedRunDirectory "automatic\runtime"
        & $benchmarkScript `
            -ModelPath $fixedModelPath `
            -ReportPrefix $reportPrefix `
            -ConfigPath (Join-Path $ResolvedRunDirectory "config.ini") `
            -Backend cpu `
            -ExpectedCaptureBackend desktop_duplication `
            -BuildDirectory $fixedBuildDirectory `
            -Configuration Release `
            -WarmupSamples 100 `
            -MinimumSamples 10000 `
            -MinimumSeconds 300 `
            -MaximumSeconds 360 `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedEncodedWidth 2560 `
            -ExpectedEncodedHeight 1440 `
            -ExpectedRoiX 1120 -ExpectedRoiY 560 `
            -ExpectedRoiWidth 320 -ExpectedRoiHeight 320 `
            -ExpectedScaleX 1 -ExpectedScaleY 1 `
            -EnableFp16 off -EnableCudaGraph off `
            -EnableGpuPreprocess off `
            -EnableD3D11CudaInterop off `
            -EnableD3D11DirectMlInterop off
        if ($LASTEXITCODE -ne 0) {
            throw "稳定性基准失败，退出码：$LASTEXITCODE"
        }
    } else {
        $running = @(Get-Process -Name Xen -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $fixedExecutable })
        if ($running.Count -ne 0) {
            throw "固定 Xen.exe 已在运行，拒绝并发实机测试。"
        }
        $originalTaskPath = $env:PATH
        try {
            $env:PATH = @(
                (Join-Path $env:SystemRoot "System32"),
                $env:SystemRoot
            ) -join ";"
            $process = Start-Process -FilePath $fixedExecutable `
                -WorkingDirectory $ResolvedRunDirectory `
                -PassThru -Wait
            if ($process.ExitCode -ne 0) {
                throw "Xen 实机测试进程失败，退出码：$($process.ExitCode)"
            }
        } finally {
            $env:PATH = $originalTaskPath
        }
    }
    Collect-Reports $ResolvedRunDirectory
}

switch ($Mode) {
    "Prepare" { Prepare-Task }
    "Launch" {
        $resolved = Resolve-ExistingRunDirectory
        Launch-Task $resolved
    }
    "Collect" {
        $resolved = Resolve-ExistingRunDirectory
        Collect-Reports $resolved
    }
}
