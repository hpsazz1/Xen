$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "aim_video_replay_matrix.ps1")

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-ReplaySample(
    [int]$Sequence,
    [double]$ObservationX,
    [double]$BaseX) {
    return [pscustomobject][ordered]@{
        sequence = $Sequence
        aim_lock_active = $true
        aim_control_evaluated = $true
        mouse_sent = $false
        aim_has_target = $true
        aim_matched_observation_valid = $true
        aim_track_id = 5
        aim_control_center_x = 0.0
        aim_control_center_y = 0.0
        aim_matched_observation_box = @(
            ($ObservationX - 2.0), 0.0, ($ObservationX + 2.0), 4.0)
        aim_box = @(($ObservationX - 2.0), 0.0,
            ($ObservationX + 2.0), 4.0)
        aim_base_point = @($BaseX, 2.0)
        aim_final_point = @($BaseX, 2.0)
        aim_command = @([Math]::Sign($BaseX), 0)
        aim_controller_dt_ms = 10.0
    }
}

function Write-ReplayReport(
    [string]$Path,
    [object[]]$Samples,
    [bool]$OutputArmed = $false) {
    $report = [ordered]@{
        schema = 16
        capture_backend = "VIDEO_REPLAY"
        mouse_backend = "SIMULATED_BACKEND_COMPLETION"
        sample_count = $Samples.Count
        successful_samples = $Samples.Count
        failed_samples = 0
        report_samples_dropped = 0
        final_snapshot = [ordered]@{
            output_allowed_by_config = $false
            output_armed = $OutputArmed
            mouse_commands = 0
        }
        timing = [ordered]@{
            control_to_mouse_physical_effect = [ordered]@{
                sample_count = 0
            }
            capture_to_mouse_physical_effect = [ordered]@{
                sample_count = 0
            }
            source_to_mouse_physical_effect = [ordered]@{
                sample_count = 0
            }
        }
        samples = $Samples
    }
    Set-Content -LiteralPath $Path `
        -Value ($report | ConvertTo-Json -Depth 8) -Encoding utf8
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("xen-aim-video-replay-matrix-{0}" -f [guid]::NewGuid())
$baseline = Join-Path $root "baseline"
$current = Join-Path $root "current"
New-Item -ItemType Directory -Path $baseline, $current -Force | Out-Null
try {
    $before = @(
        New-ReplaySample 1 -2.0 -3.0
        New-ReplaySample 2 -1.0 -1.5
        New-ReplaySample 3  1.0  1.5
        New-ReplaySample 4  2.0  3.0
    )
    $after = @(
        New-ReplaySample 1 -2.0 -2.0
        New-ReplaySample 2 -1.0 -1.0
        New-ReplaySample 3  1.0  1.0
        New-ReplaySample 4  2.0  2.0
    )
    foreach ($name in @("静止.mp4", "左右横移.mp4")) {
        Write-ReplayReport (Join-Path $baseline `
            "$name.aim-runtime.json") $before
        Write-ReplayReport (Join-Path $current `
            "$name.aim-runtime.json") $after
    }

    $matrix = Get-XenAimVideoReplayMatrix `
        -CurrentDirectory $current -BeforeDirectory $baseline
    Assert-Condition ($matrix.schema -eq 1 -and
            -not $matrix.physical_output -and $matrix.scene_count -eq 2) `
        "回放矩阵必须保留离线证据层和完整场景数。"
    Assert-Condition ($matrix.scenes[0].scenario.EndsWith(".mp4") -and
            $matrix.scenes[0].comparison.x.base_first_difference_p95.current `
                -lt $matrix.scenes[0].comparison.x.base_first_difference_p95.baseline) `
        "回放矩阵必须按视频名配对并输出 base X 一阶 A/B。"
    Assert-Condition (
            $matrix.scenes[0].comparison.y.base_first_difference_p95.delta `
                -eq 0.0) `
        "X-only 夹具必须证明矩阵中的 Y A/B 不变。"

    Write-ReplayReport (Join-Path $current `
        "拒绝武装.mp4.aim-runtime.json") $after $true
    $rejected = $false
    try {
        Get-XenAimVideoReplayMatrix -CurrentDirectory $current | Out-Null
    } catch {
        $rejected = $_.Exception.Message -like `
            "*不满足完整无物理输出契约*"
    }
    Assert-Condition $rejected `
        "回放矩阵必须拒绝被标记为已武装的报告。"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}

Write-Host "Aim video replay matrix tests passed."
