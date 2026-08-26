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
        -PackageRoot $published -RunDirectory $taskScopedRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "任务范围校验不应扫描无关文件或重复哈希 DirectML Worker。"
    }
    $taskScopedTask = Get-Content -LiteralPath `
        (Join-Path $taskScopedRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([string]$taskScopedTask.package_validation -ne "task_scoped" -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.worker.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.acceptance_script.sha256) -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.aim_report.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.aim_control_diagnostics.sha256)) {
        throw "人工任务没有绑定任务范围校验模式、Worker 或报告工具哈希。"
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

    $publishedManifest = Join-Path $published "manifest.json"
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
