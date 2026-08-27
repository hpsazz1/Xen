param(
    [string]$TestRoot = (Join-Path $PSScriptRoot `
        "..\cache\release-transfer-aim-manual-test")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Utf8([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText(
        $Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

function Add-ManifestFile(
        [System.Collections.Generic.List[object]]$Files,
        [string]$Root,
        [string]$RelativePath,
        [string]$Runtime = "") {
    $path = Join-Path $Root $RelativePath
    $file = Get-Item -LiteralPath $path
    $Files.Add([ordered]@{
        path = $RelativePath.Replace('\', '/')
        runtime = $Runtime
        size = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $path `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        source = "fixture/$RelativePath"
    })
}

function New-Schema13AimSample() {
    return [ordered]@{
        sequence = 541
        aim_status = "SUCCESS"
        mouse_sent = $true
        aim_has_target = $true
        aim_has_command = $true
        aim_track_id = 1
        aim_track_state = "CONFIRMED"
        aim_track_predicted = $false
        aim_lead_active = $false
        aim_base_point_inside_box = $true
        aim_prediction_point_outside_box = $false
        aim_command_toward_target = $true
        aim_acquisition_range_radius = 144.0
        aim_active_range_radius = 85.9412918
        aim_range_locked = $true
        aim_range_allows_control = $true
        aim_box = @(218.163315, 154.180466, 239.103516, 215.603760)
        aim_base_point = @(227.931946, 175.677979)
        aim_delay_compensated_point = @(227.931946, 175.677979)
        aim_final_point = @(227.931946, 175.677979)
        aim_lead = @(0.0, 0.0)
        aim_delay_compensation = @(0.0, 0.0)
        aim_delay_compensation_active = $false
        aim_delay_compensation_ms_x = 0.0
        aim_delay_compensation_ms_y = 0.0
        aim_delay_compensation_ms = 0.0
        aim_observation_age_ms = 1.65289998
        aim_command = @(13, 3)
        aim_control_center_x = 160.0
        source_pixels_per_pixel_x = 1.0
        aim_control_evaluated = $true
        aim_controller_dt_ms = 5.97350025
        aim_desired_x_counts = 13.680974
        aim_pending_absolute_x_counts = 0.0
        aim_reverse_candidate_x = $false
        aim_reverse_previous_direction_pending_x = $false
        aim_reverse_deformation_active_x = $false
        aim_reverse_evidence_ratio_seconds_x = 0.0
        aim_reverse_position_ratio_seconds_x = 0.0
        aim_reverse_required_evidence_ratio_seconds_x = 0.0
        aim_reverse_required_position_ratio_seconds_x = 0.0
        aim_reverse_evidence_ready_x = $false
        aim_reverse_position_ready_x = $false
        aim_reverse_gate_blocked_x = $false
        aim_pending_inventory_hold_blocked_x = $false
        aim_deadzone_quiet = $false
        aim_shaper_direction_reset_x = $false
        aim_post_alignment_sign_change_blocked_x = $false
        aim_post_alignment_growth_limited_x = $false
        aim_closing_response_tapered_x = $false
        aim_integer_direction_blocked_x = $false
        aim_command_sign_change_blocked_x = $false
        aim_quantization_zero_x = $false
        aim_reverse_probe_direction_x = 0
        aim_reverse_probe_age_ms_x = 0.0
        aim_reverse_probe_active_x = $false
        aim_reverse_probe_limited_x = $false
        aim_reverse_translation_seconds_x = 0.0
        aim_reverse_translation_ready_x = $false
        aim_reverse_output_direction_x = 0
        aim_reverse_translation_raw_left_x_roi_pixels = 0.0799713135
        aim_reverse_translation_raw_right_x_roi_pixels = -0.0196380615
        aim_reverse_translation_raw_common_x_roi_pixels = 0.0
        aim_reverse_translation_control_evidence_x = 0.0
        aim_reverse_translation_gap_seconds_x = 0.0
        aim_reverse_translation_fresh_evidence_x = $false
        aim_reverse_translation_reset_reason_x = "NONE"
        aim_reverse_position_peak_error_x = 0.0
        aim_reverse_position_improvement_reset_x = $false
        aim_modelled_response_x_counts = 0.0
        aim_observer_phase_command_x_counts = 0.0
        aim_observer_consistency_weight_x = 0.0
        mouse_completion_timing_valid = $true
    }
}

$root = [System.IO.Path]::GetFullPath($TestRoot)
if ($root -match '^[A-Za-z]:\?$' -or $root -eq '\') {
    throw "拒绝在文件系统根目录运行测试。"
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

try {
    $package = Join-Path $root "Xen-fixture"
    foreach ($directory in @(
            "models", "runtimes\nvidia", "runtimes\directml",
            "runtimes\openvino", "tools", "cache", "logs", "licenses")) {
        New-Item -ItemType Directory -Path (Join-Path $package $directory) `
            -Force | Out-Null
    }
    Write-Utf8 (Join-Path $package "XenLauncher.exe") "launcher"
    Write-Utf8 (Join-Path $package "config.ini") "fixture-config"
    Write-Utf8 (Join-Path $package "models\14wv11.onnx") "model"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\Xen.exe") "nvidia"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\nvinfer_10.dll") "trt"
    Write-Utf8 (Join-Path $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll") "ndi"
    Write-Utf8 (Join-Path $package "runtimes\directml\Xen.exe") "dml"
    Write-Utf8 (Join-Path $package "runtimes\openvino\Xen.exe") "ov"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "aim_report.ps1") `
        -Destination (Join-Path $package "tools\aim_report.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "aim_control_diagnostics.ps1") `
        -Destination (Join-Path $package `
            "tools\aim_control_diagnostics.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "aim_fixed_scene_analysis.ps1") `
        -Destination (Join-Path $package `
            "tools\aim_fixed_scene_analysis.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "invoke_aim_manual_acceptance.ps1") `
        -Destination (Join-Path $package `
            "tools\invoke_aim_manual_acceptance.ps1")

    $files = [System.Collections.Generic.List[object]]::new()
    Add-ManifestFile $files $package "XenLauncher.exe"
    Add-ManifestFile $files $package "config.ini"
    Add-ManifestFile $files $package "models\14wv11.onnx"
    Add-ManifestFile $files $package "runtimes\nvidia\Xen.exe" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\nvinfer_10.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\directml\Xen.exe" "directml"
    Add-ManifestFile $files $package `
        "runtimes\openvino\Xen.exe" "openvino"
    Add-ManifestFile $files $package "tools\aim_report.ps1"
    Add-ManifestFile $files $package "tools\aim_control_diagnostics.ps1"
    Add-ManifestFile $files $package "tools\aim_fixed_scene_analysis.ps1"
    Add-ManifestFile $files $package `
        "tools\invoke_aim_manual_acceptance.ps1"
    [ordered]@{
        schema = 1
        product = "Xen"
        git_commit = "1" * 40
        runtimes = @(
            [ordered]@{
                id = "nvidia"
                executable = "runtimes/nvidia/Xen.exe"
                backends = @("cpu", "cuda", "tensorrt")
            },
            [ordered]@{
                id = "directml"
                executable = "runtimes/directml/Xen.exe"
                backends = @("directml")
            },
            [ordered]@{
                id = "openvino"
                executable = "runtimes/openvino/Xen.exe"
                backends = @("openvino")
            }
        )
        files = @($files)
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $package "manifest.json") `
            -Encoding utf8

    $destination = Join-Path $root "remote"
    New-Item -ItemType Directory -Path $destination | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot "transfer_release_bundle.ps1") `
        -PackagePath $package -DestinationRoot $destination
    if ($LASTEXITCODE -ne 0) {
        throw "完整发布包传输夹具失败。"
    }
    $published = Join-Path $destination "releases\Xen-fixture"
    if (-not (Test-Path -LiteralPath $published -PathType Container) -or
        @(Get-ChildItem -LiteralPath (Join-Path $destination "releases") `
            -Force | Where-Object { $_.Name -like ".incoming-*" }).Count -ne 0) {
        throw "完整发布包传输未原子收口。"
    }

    Write-Utf8 (Join-Path $published "cache\tensorrt\fixture.engine") `
        "runtime provider cache"
    Write-Utf8 (Join-Path $published "logs\xen.log") "runtime log"

    Write-Utf8 (Join-Path $published "notes\local.txt") `
        "与本轮人工任务无关的本地文件"
    $directMlWorker = Join-Path $published "runtimes\directml\Xen.exe"
    Write-Utf8 $directMlWorker "dml-not-transferred-this-round"
    $taskScopedRoot = Join-Path $root "task-scoped-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -RequireSourceTiming `
        -PackageRoot $published -RunDirectory $taskScopedRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "任务范围校验不应扫描无关文件或重复哈希 DirectML Worker。"
    }
    $taskScopedTask = Get-Content -LiteralPath `
        (Join-Path $taskScopedRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    $taskScopedMarkdown = Get-Content -LiteralPath `
        (Join-Path $taskScopedRoot "TASK.md") -Raw -Encoding utf8
    $sourceClockCommand =
        'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\Xen\scripts\run_ndi_clock_source.ps1"'
    if ([string]$taskScopedTask.package_validation -ne "task_scoped" -or
        -not [bool]$taskScopedTask.require_source_timing -or
        $taskScopedMarkdown -notmatch [regex]::Escape($sourceClockCommand) -or
        $taskScopedMarkdown -match '填写 OBSERVATION\.md' -or
        $taskScopedMarkdown -notmatch '直接发送到当前对话' -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.worker.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.acceptance_script.sha256) -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.aim_report.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.aim_control_diagnostics.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.aim_fixed_scene_analysis.sha256)) {
        throw "人工任务没有绑定可执行 source timing、对话回收、任务范围校验模式、Worker 或通用固定场景报告工具哈希。"
    }
    Write-Utf8 $directMlWorker "dml"

    $controlDiagnosticsTool = Join-Path $published `
        "tools\aim_control_diagnostics.ps1"
    $controlDiagnosticsBytes = [System.IO.File]::ReadAllBytes(
        $controlDiagnosticsTool)
    try {
        [System.IO.File]::AppendAllText(
            $controlDiagnosticsTool, "`n# corrupt", `
            [System.Text.UTF8Encoding]::new($false))
        $invalidControlDiagnosticsRoot = Join-Path $root `
            "invalid-task-control-diagnostics"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published `
            -RunDirectory $invalidControlDiagnosticsRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝控制诊断工具哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes(
            $controlDiagnosticsTool, $controlDiagnosticsBytes)
    }

    $nvidiaWorker = Join-Path $published "runtimes\nvidia\Xen.exe"
    Write-Utf8 $nvidiaWorker "nvidia-corrupt"
    $invalidWorkerRoot = Join-Path $root "invalid-task-worker"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $invalidWorkerRoot
    if ($LASTEXITCODE -eq 0) {
        throw "任务范围校验必须拒绝本轮使用的 NVIDIA Worker 哈希变化。"
    }
    Write-Utf8 $nvidiaWorker "nvidia"

    $modelPath = Join-Path $published "models\14wv11.onnx"
    $modelBytes = [System.IO.File]::ReadAllBytes($modelPath)
    try {
        [System.IO.File]::AppendAllText(
            $modelPath, "`ncorrupt", [System.Text.UTF8Encoding]::new($false))
        $invalidModelRoot = Join-Path $root "invalid-task-model"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published -RunDirectory $invalidModelRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝本轮使用的模型哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($modelPath, $modelBytes)
    }

    $launcherPath = Join-Path $published "XenLauncher.exe"
    $launcherBytes = [System.IO.File]::ReadAllBytes($launcherPath)
    try {
        [System.IO.File]::AppendAllText(
            $launcherPath, "`ncorrupt",
            [System.Text.UTF8Encoding]::new($false))
        $invalidLauncherRoot = Join-Path $root "invalid-task-launcher"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published -RunDirectory $invalidLauncherRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝本轮使用的 Launcher 哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($launcherPath, $launcherBytes)
    }

    $prepareAuthorizationRoot = Join-Path $root "invalid-prepare-authorization"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $prepareAuthorizationRoot `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
    if ($LASTEXITCODE -eq 0) {
        throw "Prepare 不得接受真实物理输出授权。"
    }

    $trackingRoot = Join-Path $root "tracking-task"
    $trackingOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -Smoothing 0.50 `
        -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
        -MaxCountsPerFrame 14.0 `
        -EnableDelayCompensation -ControlDelayMs 7.5 `
        -MaxDelayCompensationMs 18.0 `
        -MaxDelayCompensationPercent 12.0 `
        -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "tracking 任务准备失败。" }
    $trackingOutput | ForEach-Object { Write-Host $_ }
    $trackingOutputText = $trackingOutput -join "`n"
    if ($trackingOutputText -notmatch
            ('(?m)^powershell\.exe .* -TaskId AIM-SUPERJUMP-ACCEPT-001 ' +
             '-Mode Launch -Scenario SuperJump -SuperJumpCase Static ' +
             '-Profile tracking .* ' +
             '-Smoothing 0\.500000 -CountsPerPixelX 0\.450000 ' +
             '-CountsPerPixelY 0\.400000 ' +
             '-MaxCountsPerFrame 14\.000000 ' +
             '-EnableDelayCompensation ' +
             '-ControlDelayMs 7\.500000 ' +
             '-MaxDelayCompensationMs 18\.000000 ' +
             '-MaxDelayCompensationPercent 12\.000000 ' +
             '-AllowPhysicalOutput ' +
             '-PhysicalOutputConfirmation ' +
             'XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT\r?$')) {
        throw "Prepare 前台没有输出可直接复制的完整 Launch 命令。"
    }
    $trackingConfig = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "config.ini") -Raw -Encoding utf8
    if ($trackingConfig -notmatch '(?m)^backend=tensorrt\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=ndi\r?$' -or
        $trackingConfig -notmatch '(?m)^enable_prediction=false\r?$' -or
        $trackingConfig -notmatch '(?m)^smoothing=0\.500000\r?$' -or
        $trackingConfig -notmatch '(?m)^enable_delay_compensation=true\r?$' -or
        $trackingConfig -notmatch '(?m)^control_delay_ms=7\.500000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_delay_compensation_ms=18\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_delay_compensation_percent=12\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^counts_per_pixel_x=0\.450000\r?$' -or
        $trackingConfig -notmatch '(?m)^counts_per_pixel_y=0\.400000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_counts_per_frame=14\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=kmbox_net\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_ip=192\.168\.2\.188\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_port=13384\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_uuid=7679E04E\r?$' -or
        $trackingConfig -notmatch '(?m)^allow_send_input=true\r?$') {
        throw "tracking 配置没有固定 NDI、TensorRT 或 KMBOX 契约。"
    }
    $trackingTask = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([string]$trackingTask.task_id -ne "AIM-SUPERJUMP-ACCEPT-001" -or
        [string]$trackingTask.scenario -ne "SuperJump" -or
        [string]$trackingTask.superjump_case -ne "Static" -or
        [double]$trackingTask.aim.smoothing -ne 0.50 -or
        [double]$trackingTask.aim.counts_per_pixel_x -ne 0.45 -or
        [double]$trackingTask.aim.counts_per_pixel_y -ne 0.40 -or
        [double]$trackingTask.aim.max_counts_per_frame -ne 14.0 -or
        [bool]$trackingTask.aim.delay_compensation_enabled -ne $true -or
        [string]$trackingTask.package_validation -ne "task_scoped" -or
        [double]$trackingTask.aim.control_delay_ms -ne 7.5 -or
        [double]$trackingTask.aim.max_delay_compensation_ms -ne 18.0 -or
        [double]$trackingTask.aim.max_delay_compensation_percent -ne 12.0) {
        throw "task.json 没有固化任务 ID、平滑或延迟补偿参数。"
    }

    $superJumpCases = [ordered]@{
        Static = [ordered]@{
            root = $trackingRoot
            marker = "本 Run 唯一动作：X 静止"
        }
        SustainedMove = [ordered]@{
            root = Join-Path $root "superjump-sustained-move-task"
            marker = "本 Run 唯一动作：X 持续横移"
        }
        Stop = [ordered]@{
            root = Join-Path $root "superjump-stop-task"
            marker = "本 Run 唯一动作：X 横移后停止"
        }
        Reverse = [ordered]@{
            root = Join-Path $root "superjump-reverse-task"
            marker = "本 Run 唯一动作：X 横移后换向"
        }
    }
    $preparedManifestHashes = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($caseName in $superJumpCases.Keys) {
        $caseSpec = $superJumpCases[$caseName]
        if ($caseName -ne "Static") {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
                (Join-Path $published `
                    "tools\invoke_aim_manual_acceptance.ps1") `
                -TaskId AIM-SUPERJUMP-ACCEPT-001 `
                -Mode Prepare -Scenario SuperJump `
                -SuperJumpCase $caseName -Profile tracking -Smoothing 0.50 `
                -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
                -MaxCountsPerFrame 14.0 -EnableDelayCompensation `
                -ControlDelayMs 7.5 -MaxDelayCompensationMs 18.0 `
                -MaxDelayCompensationPercent 12.0 `
                -PackageRoot $published -RunDirectory $caseSpec.root | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "SuperJump $caseName 独立任务准备失败。"
            }
        }
        $caseFiles = @(Get-ChildItem -LiteralPath $caseSpec.root -File)
        $caseDirectories = @(Get-ChildItem -LiteralPath $caseSpec.root -Directory)
        if ($caseFiles.Count -ne 4 -or $caseDirectories.Count -ne 0 -or
            (@($caseFiles.Name | Sort-Object) -join ',') -ne
                'config.ini,OBSERVATION.md,task.json,TASK.md') {
            throw "SuperJump $caseName 必须生成严格独立的 Run 四文件。"
        }
        $caseTask = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "task.json") -Raw -Encoding utf8 |
            ConvertFrom-Json
        if ([string]$caseTask.scenario -ne "SuperJump" -or
            [string]$caseTask.superjump_case -ne $caseName) {
            throw "SuperJump $caseName task.json 没有绑定唯一动作。"
        }
        [void]$preparedManifestHashes.Add(
            [string]$caseTask.package_manifest.sha256)
        $caseTaskMarkdown = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "TASK.md") -Raw -Encoding utf8
        $caseObservation = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "OBSERVATION.md") -Raw -Encoding utf8
        foreach ($candidate in $superJumpCases.Values.marker) {
            $expectedCount = if ($candidate -eq $caseSpec.marker) { 2 } else { 0 }
            $actualCount = @(
                $caseTaskMarkdown, $caseObservation |
                    Where-Object { $_ -match [regex]::Escape($candidate) }
            ).Count
            if ($actualCount -ne $expectedCount) {
                throw "SuperJump $caseName 混入其他动作或缺少唯一动作标记：$candidate"
            }
        }
    }
    $activeManifestHash = (Get-FileHash -LiteralPath `
        (Join-Path $published "manifest.json") -Algorithm SHA256).Hash
    if ($preparedManifestHashes.Count -ne 1 -or
        -not $preparedManifestHashes.Contains($activeManifestHash)) {
        throw "四个 SuperJump Run 必须共享同一活动 manifest 身份。"
    }

    $automaticRoot = Join-Path $trackingRoot "automatic"
    New-Item -ItemType Directory -Path $automaticRoot -Force | Out-Null
    Write-Utf8 (Join-Path $automaticRoot "schema13.csv") `
        "sequence,success`n541,true`n"
    $schema13Report = [ordered]@{
        schema = 13
        session_id = "schema13"
        provider = "TensorrtExecutionProvider"
        capture_backend = "NDI"
        mouse_backend = "kmbox_net"
        sample_count = 1
        successful_samples = 1
        failed_samples = 0
        report_samples_dropped = 0
        runtime_samples_dropped = 0
        samples = @(New-Schema13AimSample)
    }
    Write-Utf8 (Join-Path $automaticRoot "schema13.json") `
        (($schema13Report | ConvertTo-Json -Depth 10) + "`n")
    $publishedManifest = Join-Path $published "manifest.json"
    $manifestBytes = [System.IO.File]::ReadAllBytes($publishedManifest)
    try {
        [System.IO.File]::AppendAllText(
            $publishedManifest, "`n", [System.Text.UTF8Encoding]::new($false))
        $recoverOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -Smoothing 0.50 `
            -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
            -MaxCountsPerFrame 14.0 `
            -EnableDelayCompensation -ControlDelayMs 7.5 `
            -MaxDelayCompensationMs 18.0 `
            -MaxDelayCompensationPercent 12.0 `
            -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $recoverOutput | ForEach-Object { Write-Host $_ }
            throw "schema 13 离线回收失败。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($publishedManifest, $manifestBytes)
    }
    $recoveredSummary = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$recoveredSummary.collection_mode -ne "Recover" -or
        @($recoveredSummary.runtime_report_schemas).Count -ne 1 -or
        [int]@($recoveredSummary.runtime_report_schemas)[0] -ne 13 -or
        [uint64]$recoveredSummary.sample_count -ne 1 -or
        [uint64]$recoveredSummary.successful_samples -ne 1 -or
        [uint64]$recoveredSummary.failed_samples -ne 0 -or
        [uint64]$recoveredSummary.source_timing_valid_samples -ne 0 -or
        [string]$recoveredSummary.source_timing_diagnostic -ne
            "REPORT_FIELDS_UNAVAILABLE" -or
        $null -ne $recoveredSummary.source_clock_sample_count_max -or
        [uint64]$recoveredSummary.mouse_backend_completion_samples -ne 1 -or
        [uint64]$recoveredSummary.mouse_protocol_ack_samples -ne 0 -or
        [uint64]$recoveredSummary.mouse_physical_effect_samples -ne 0 -or
        -not [bool]$recoveredSummary.automatic_complete) {
        throw "schema 13 回收没有保留已知证据并把缺失时序层降级为 unknown。"
    }

    $manifestBytes = [System.IO.File]::ReadAllBytes($publishedManifest)
    try {
        [System.IO.File]::AppendAllText(
            $publishedManifest, "`n", [System.Text.UTF8Encoding]::new($false))
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -Smoothing 0.50 `
            -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
            -MaxCountsPerFrame 14.0 `
            -EnableDelayCompensation -ControlDelayMs 7.5 `
            -MaxDelayCompensationMs 18.0 `
            -MaxDelayCompensationPercent 12.0 `
            -PackageRoot $published -RunDirectory $trackingRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围 Launch 必须拒绝 Prepare 后变化的 manifest。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($publishedManifest, $manifestBytes)
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Launch -Scenario Static -Profile tracking -Smoothing 0.35 `
        -CountsPerPixel 0.55 `
        -EnableDelayCompensation -ControlDelayMs 7.5 `
        -MaxDelayCompensationMs 18.0 `
        -MaxDelayCompensationPercent 12.0 `
        -PackageRoot $published -RunDirectory $trackingRoot `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
    if ($LASTEXITCODE -eq 0) {
        throw "Launch 不应接受与 Prepare 快照不一致的 smoothing 参数。"
    }

    $predictionRoot = Join-Path $root "prediction-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Prepare -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -ne 0) { throw "prediction 任务准备失败。" }
    $predictionConfig = Get-Content -LiteralPath `
        (Join-Path $predictionRoot "config.ini") -Raw -Encoding utf8
    if ($predictionConfig -notmatch '(?m)^enable_prediction=true\r?$' -or
        $predictionConfig -notmatch `
            '(?m)^max_prediction_lead_percent=35\.000000\r?$') {
        throw "prediction 配置没有启用已验证的预测边界。"
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Launch -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -eq 0) {
        throw "缺少物理输出双重授权时 Launch 不应成功。"
    }
    Write-Host "完整包传输、任务范围校验、Aim 配置生成和授权拒绝回归通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
