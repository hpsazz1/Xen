$ErrorActionPreference = "Stop"

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot ".."))
$workflow = Join-Path $PSScriptRoot "invoke_live_game_acceptance.ps1"
$testRoot = Join-Path $repositoryRoot (
    "cache\live-game-workflow-test-{0}" -f [guid]::NewGuid().ToString("N"))
$fixedModel = Join-Path $repositoryRoot `
    "build-matrix-final-cpu\Release\models\14wv8.onnx"
$failures = 0

function Expect {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { return }
    ++$script:failures
    Write-Error "[失败] $Message" -ErrorAction Continue
}

function Invoke-ExpectedFailure {
    param([scriptblock]$Action, [string]$Message)
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Expect $failed $Message
}

try {
    $observationRun = Join-Path $testRoot "observation"
    & $workflow -Mode Prepare -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    Expect (Test-Path -LiteralPath (
        Join-Path $observationRun "TASK.md")) "应生成 TASK.md"
    $task = Get-Content -LiteralPath (
        Join-Path $observationRun "task.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $config = Get-Content -LiteralPath (
        Join-Path $observationRun "config.ini") -Raw -Encoding UTF8
    Expect ($task.stage -eq "DetectionStatic" -and
            -not [bool]$task.physical_output) `
        "静止检测任务必须禁用物理输出"
    Expect ($config -match '(?m)^model_path=14wv8\.onnx$' -and
            $config -match '(?m)^backend=cpu$' -and
            $config -match '(?m)^allow_send_input=false$' -and
            $config -match
                '(?m)^open_detached_preview_on_start=true$') `
        "观察任务必须固定模型、CPU、置顶预览和禁用物理输出"
    Expect ($config -match '(?m)^person_class_ids=0,2$' -and
            $config -match '(?m)^head_class_ids=1,3$' -and
            $config -match '(?m)^roi_width=640$' -and
            $config -match '(?m)^roi_height=640$') `
        "固定模型必须使用 640x640 ROI 并纳入 CT/T 身体和头部类别"
    $taskMarkdown = Get-Content -LiteralPath (
        Join-Path $observationRun "TASK.md") -Raw -Encoding UTF8
    $expectedLaunchPrefix =
        'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Mode Launch' -f
        $workflow
    Expect ($taskMarkdown -match [regex]::Escape($expectedLaunchPrefix) -and
            $taskMarkdown -match '~~~powershell' -and
            $taskMarkdown -match
                'Xen 进程退出后才会汇总并输出“自动报告已汇总”') `
        "任务单必须提供绝对启动命令并明确停止、退出、汇总顺序"
    Expect ($task.view_mode -eq "fixed") `
        "静止检测必须固定人物视角"

    foreach ($stage in @(
            "DetectionMoveLeft", "DetectionMoveRight", "DetectionShuttle",
            "DetectionSuperJump")) {
        $movementRun = Join-Path $testRoot $stage
        & $workflow -Mode Prepare -Stage $stage `
            -RunDirectory $movementRun | Out-Null
        $movementTask = Get-Content -LiteralPath (
            Join-Path $movementRun "task.json") -Raw -Encoding UTF8 |
            ConvertFrom-Json
        Expect ($movementTask.view_mode -eq "fixed" -and
                -not [bool]$movementTask.physical_output) `
            "$stage 必须固定人物视角并禁用物理输出"
    }
    $unauthorizedRun = Join-Path $testRoot "unauthorized-control"
    Invoke-ExpectedFailure {
        & $workflow -Mode Prepare -Stage ControlAcquire `
            -RunDirectory $unauthorizedRun | Out-Null
    } "控制环节缺少双重授权时必须失败"
    Expect (-not (Test-Path -LiteralPath $unauthorizedRun)) `
        "授权失败不得留下任务目录"

    $controlRun = Join-Path $testRoot "authorized-control"
    & $workflow -Mode Prepare -Stage ControlAcquire `
        -RunDirectory $controlRun -AllowPhysicalOutput `
        -PhysicalOutputConfirmation `
            XEN_LIVE_GAME_ACCEPTANCE_SENDS_REAL_INPUT | Out-Null
    $controlConfig = Get-Content -LiteralPath (
        Join-Path $controlRun "config.ini") -Raw -Encoding UTF8
    Expect ($controlConfig -match '(?m)^allow_send_input=true$') `
        "完成双重授权的控制任务才允许生成物理输出配置"

    $runtimeDirectory = Join-Path $observationRun "cache\runtime"
    New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null
    $reportPath = Join-Path $runtimeDirectory "synthetic.json"
    $csvPath = Join-Path $runtimeDirectory "synthetic.csv"
    $synthetic = [ordered]@{
        schema = 6
        session_id = "synthetic"
        model_path = $fixedModel
        provider = "CPUExecutionProvider"
        capture_backend = "DESKTOP_DUPLICATION"
        mouse_backend = "win32_send_input"
        sample_count = 1200
        successful_samples = 1200
        failed_samples = 0
        report_samples_dropped = 0
        runtime_samples_dropped = 0
        final_snapshot = [ordered]@{
            provider = "CPUExecutionProvider"
            source_width = 2560
            source_height = 1440
            encoded_width = 2560
            encoded_height = 1440
            roi_x = 960
            roi_y = 400
            roi_width = 640
            roi_height = 640
            source_pixels_per_pixel_x = 1.0
            source_pixels_per_pixel_y = 1.0
            output_allowed_by_config = $false
            preview_enabled = $true
            preview_sampled_frames = 120
            preview_dropped_frames = 0
            failed_frames = 0
            last_error = ""
            mouse_commands = 0
        }
        timing = [ordered]@{
            total = [ordered]@{
                sample_count = 1200
                mean_ms = 4.0
                p50_ms = 3.8
                p95_ms = 5.0
                p99_ms = 6.0
                max_ms = 7.0
            }
        }
        samples = @(
            [ordered]@{
                person_detection_count = 1
                head_detection_count = 1
                max_person_confidence = 0.864
                max_head_confidence = 0.871
                detection_count_by_class = @(1, 1, 0, 0)
                max_confidence_by_class = @(0.864, 0.871, 0.0, 0.0)
            }
        )
    }
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText(
        $csvPath, "# synthetic`n",
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $summary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect ([bool]$summary.automatic_complete -and
            $summary.sample_count -eq 1200 -and
            $summary.failed_samples -eq 0 -and
            $summary.detection_observability.classes[0].name -eq "ct_body" -and
            $summary.detection_observability.classes[0].confidence.p50 -eq
                0.864 -and
            $summary.detection_observability.classes[2].confidence.detected_frames `
                -eq 0) `
        "合法 schema 6 报告应完成自动汇总"

    $synthetic.final_snapshot.preview_sampled_frames = 0
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $noPreviewSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$noPreviewSummary.automatic_complete -and
            ($noPreviewSummary.failures -join "`n") -match
                '未形成独立置顶检测预览采样') `
        "GUI 报告预览采样为零时必须拒绝自动通过"
    $synthetic.final_snapshot.preview_sampled_frames = 120
    [System.IO.File]::WriteAllText(
        $reportPath, (($synthetic | ConvertTo-Json -Depth 8) + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))

    Copy-Item -LiteralPath $reportPath -Destination (
        Join-Path $runtimeDirectory "duplicate.json")
    Copy-Item -LiteralPath $csvPath -Destination (
        Join-Path $runtimeDirectory "duplicate.csv")
    & $workflow -Mode Collect -Stage DetectionStatic `
        -RunDirectory $observationRun | Out-Null
    $duplicateSummary = Get-Content -LiteralPath (
        Join-Path $observationRun "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    Expect (-not [bool]$duplicateSummary.automatic_complete -and
            ($duplicateSummary.failures -join "`n") -match
                '必须且只能生成一份') `
        "单个环节存在多份 Runtime 报告时必须拒绝自动通过"
    Remove-Item -LiteralPath (
        Join-Path $runtimeDirectory "duplicate.json") -Force
    Remove-Item -LiteralPath (
        Join-Path $runtimeDirectory "duplicate.csv") -Force

    Add-Content -LiteralPath (Join-Path $observationRun "config.ini") `
        -Value "# tampered" -Encoding UTF8
    Invoke-ExpectedFailure {
        & $workflow -Mode Collect -Stage DetectionStatic `
            -RunDirectory $observationRun | Out-Null
    } "配置发生变化后必须拒绝再次汇总"
} finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $resolvedCacheRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $repositoryRoot "cache"))
    if ($resolvedTestRoot.StartsWith(
            $resolvedCacheRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTestRoot) -like
            "live-game-workflow-test-*") {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}

if ($failures -ne 0) {
    throw "实机测试工作流回归失败数：$failures"
}
Write-Host "实机测试工作流回归全部通过。"
