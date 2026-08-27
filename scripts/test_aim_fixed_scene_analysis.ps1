$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "aim_fixed_scene_analysis.ps1")

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-FixedSceneSample(
    [int]$Sequence,
    [double]$ObservationX,
    [double]$TrackX,
    [double]$BaseX,
    [int]$CommandX) {
    return [pscustomobject][ordered]@{
        sequence = $Sequence
        aim_lock_active = $true
        aim_control_evaluated = $true
        aim_has_target = $true
        aim_matched_observation_valid = $true
        aim_track_id = 9
        aim_control_center_x = 0.0
        aim_control_center_y = 0.0
        aim_matched_observation_box = @(
            ($ObservationX - 2.0), 0.0, ($ObservationX + 2.0), 4.0)
        aim_box = @(($TrackX - 2.0), 0.0, ($TrackX + 2.0), 4.0)
        aim_base_point = @($BaseX, 2.0)
        aim_final_point = @($BaseX, 2.0)
        aim_command = @($CommandX, 0)
        aim_controller_dt_ms = 10.0
    }
}

$samples = @(
    New-FixedSceneSample 1 -2.0 -1.5 -3.0 -1
    New-FixedSceneSample 2 -1.0 -0.5 -1.0 -1
    New-FixedSceneSample 3  1.0  0.5  1.0  1
    New-FixedSceneSample 4  2.0  1.5  3.0  1
    New-FixedSceneSample 5  1.0  0.5  1.0  1
    New-FixedSceneSample 6 -1.0 -0.5 -1.0 -1
)

$summary = Get-XenAimFixedSceneAnalysis -Samples $samples
Assert-Condition ($summary.schema -eq 1 -and
        $summary.selected_frames -eq 6 -and
        [Math]::Abs($summary.duration_seconds - 0.06) -lt 0.000001) `
    "固定场景分析必须保留完整锁定帧数和真实控制时长。"
Assert-Condition ($summary.x.observation.zero_crossings -eq 2 -and
        $summary.x.command.direction_reversals -eq 2) `
    "固定夹具必须逐值统计 Observation 过零与命令反向。"
Assert-Condition ([Math]::Abs(
        $summary.x.observation.absolute_error.p95 - 2.0) -lt 0.000001 -and
        [Math]::Abs(
        $summary.x.base.absolute_error.p95 - 3.0) -lt 0.000001) `
    "固定夹具的 Observation/base X P95 必须使用线性插值逐值复现。"
Assert-Condition (-not [bool]$summary.x.stability_passed -and
        $summary.x.base_to_observation_absolute_error_p95_ratio -eq 1.5) `
    "base X 放大同帧 Observation X 时必须形成几何相对红灯。"
Assert-Condition ($summary.x.reference.absolute_error.p95 -eq 1.5 -and
        $summary.x.normalized_reference.absolute_error.p95 -eq 0.375) `
    "通用分析必须把 Track 公共平移与框内归一化 reference 分开。"
Assert-Condition ($summary.y.base.absolute_first_difference.p95 -eq 0.0 -and
        $summary.y.command.nonzero_frames -eq 0) `
    "只改变 X 的夹具必须证明 Y 统计保持安静。"

$filtered = @($samples)
$filtered[2].aim_lock_active = $false
$filtered[4].aim_matched_observation_valid = $false
$filteredSummary = Get-XenAimFixedSceneAnalysis -Samples $filtered
Assert-Condition ($filteredSummary.selected_frames -eq 4 -and
        $filteredSummary.contiguous_pairs -eq 1) `
    "分析必须排除未锁定/无当前 Observation 帧，并且不跨序号缺口求差分。"

Write-Host "Aim fixed-scene analysis tests passed."
