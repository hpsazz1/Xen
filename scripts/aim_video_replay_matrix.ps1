param(
    [string]$ReportDirectory = "",
    [string]$BaselineDirectory = "",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$requestedOutputPath = $OutputPath
. (Join-Path $PSScriptRoot "aim_fixed_scene_analysis.ps1")

function Get-XenAimVideoReplayReportSet([string]$Directory) {
    $resolved = (Resolve-Path -LiteralPath $Directory).Path
    $files = @(Get-ChildItem -LiteralPath $resolved `
        -Filter "*.aim-runtime.json" -File | Sort-Object Name)
    if ($files.Count -eq 0) {
        throw "目录没有 Aim 视频回放 JSON：$resolved"
    }

    $reports = [ordered]@{}
    foreach ($file in $files) {
        $report = Get-Content -LiteralPath $file.FullName -Raw `
            -Encoding utf8 | ConvertFrom-Json
        if ([int]$report.schema -ne 16 -or
            $report.capture_backend -ne "VIDEO_REPLAY" -or
            $report.mouse_backend -ne "SIMULATED_BACKEND_COMPLETION" -or
            [long]$report.sample_count -ne @($report.samples).Count -or
            [long]$report.successful_samples -ne [long]$report.sample_count -or
            [long]$report.failed_samples -ne 0 -or
            [long]$report.report_samples_dropped -ne 0 -or
            [bool]$report.final_snapshot.output_allowed_by_config -or
            [bool]$report.final_snapshot.output_armed -or
            [long]$report.final_snapshot.mouse_commands -ne 0 -or
            @($report.samples | Where-Object {
                [bool]$_.mouse_sent
            }).Count -ne 0 -or
            [long]$report.timing.control_to_mouse_physical_effect.sample_count `
                -ne 0 -or
            [long]$report.timing.capture_to_mouse_physical_effect.sample_count `
                -ne 0 -or
            [long]$report.timing.source_to_mouse_physical_effect.sample_count `
                -ne 0) {
            throw "Aim 视频回放报告不满足完整无物理输出契约：$($file.FullName)"
        }
        $suffix = ".aim-runtime.json"
        $scenario = $file.Name.Substring(0, $file.Name.Length - $suffix.Length)
        $reports[$file.Name] = [ordered]@{
            scenario = $scenario
            path = $file.FullName
            sha256 = (Get-FileHash -LiteralPath $file.FullName `
                -Algorithm SHA256).Hash
            report = $report
            analysis = Get-XenAimFixedSceneAnalysis `
                -Samples @($report.samples) -Scenario $scenario
        }
    }
    return [ordered]@{
        directory = $resolved
        reports = $reports
    }
}

function New-XenAimVideoReplayComparison(
    [object]$Baseline,
    [object]$Current) {
    return [ordered]@{
        x = [ordered]@{
            observation_first_difference_p95 = [ordered]@{
                baseline = [double]$Baseline.x.observation.absolute_first_difference.p95
                current = [double]$Current.x.observation.absolute_first_difference.p95
            }
            track_first_difference_p95 = [ordered]@{
                baseline = [double]$Baseline.x.track.absolute_first_difference.p95
                current = [double]$Current.x.track.absolute_first_difference.p95
            }
            base_first_difference_p95 = [ordered]@{
                baseline = [double]$Baseline.x.base.absolute_first_difference.p95
                current = [double]$Current.x.base.absolute_first_difference.p95
                delta = [double]$Current.x.base.absolute_first_difference.p95 -
                    [double]$Baseline.x.base.absolute_first_difference.p95
            }
            base_to_observation_first_difference_p95_ratio = [ordered]@{
                baseline = $Baseline.x.base_to_observation_first_difference_p95_ratio
                current = $Current.x.base_to_observation_first_difference_p95_ratio
            }
            command_direction_reversals = [ordered]@{
                baseline = [int]$Baseline.x.command.direction_reversals
                current = [int]$Current.x.command.direction_reversals
            }
        }
        y = [ordered]@{
            observation_first_difference_p95 = [ordered]@{
                baseline = [double]$Baseline.y.observation.absolute_first_difference.p95
                current = [double]$Current.y.observation.absolute_first_difference.p95
            }
            base_first_difference_p95 = [ordered]@{
                baseline = [double]$Baseline.y.base.absolute_first_difference.p95
                current = [double]$Current.y.base.absolute_first_difference.p95
                delta = [double]$Current.y.base.absolute_first_difference.p95 -
                    [double]$Baseline.y.base.absolute_first_difference.p95
            }
        }
    }
}

function Get-XenAimVideoReplayMatrix {
    param(
        [Parameter(Mandatory = $true)][string]$CurrentDirectory,
        [string]$BeforeDirectory = ""
    )

    $current = Get-XenAimVideoReplayReportSet $CurrentDirectory
    $baseline = $null
    if (-not [string]::IsNullOrWhiteSpace($BeforeDirectory)) {
        $baseline = Get-XenAimVideoReplayReportSet $BeforeDirectory
        $difference = @(Compare-Object `
            -ReferenceObject @($baseline.reports.Keys) `
            -DifferenceObject @($current.reports.Keys))
        if ($difference.Count -ne 0) {
            throw "Aim 视频回放 A/B 场景集合不一致：$($difference -join '; ')"
        }
    }

    $scenes = [System.Collections.Generic.List[object]]::new()
    foreach ($name in $current.reports.Keys) {
        $item = $current.reports[$name]
        $scene = [ordered]@{
            scenario = $item.scenario
            report_path = $item.path
            report_sha256 = $item.sha256
            analysis = $item.analysis
        }
        if ($null -ne $baseline) {
            $before = $baseline.reports[$name]
            $scene.baseline_report_path = $before.path
            $scene.baseline_report_sha256 = $before.sha256
            $scene.comparison = New-XenAimVideoReplayComparison `
                $before.analysis $item.analysis
        }
        $scenes.Add([pscustomobject]$scene)
    }

    return [ordered]@{
        schema = 1
        evidence = "offline_video_replay"
        physical_output = $false
        report_directory = $current.directory
        baseline_directory = if ($null -eq $baseline) {
            $null
        } else {
            $baseline.directory
        }
        scene_count = $scenes.Count
        scenes = $scenes.ToArray()
    }
}

if ($MyInvocation.InvocationName -ne ".") {
    if ([string]::IsNullOrWhiteSpace($ReportDirectory)) {
        throw "请通过 -ReportDirectory 指定 Aim 视频回放报告目录。"
    }
    $matrix = Get-XenAimVideoReplayMatrix `
        -CurrentDirectory $ReportDirectory `
        -BeforeDirectory $BaselineDirectory
    $json = $matrix | ConvertTo-Json -Depth 14
    if ([string]::IsNullOrWhiteSpace($requestedOutputPath)) {
        $json
    } else {
        $parent = Split-Path -Parent $requestedOutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent) -and
            -not (Test-Path -LiteralPath $parent -PathType Container)) {
            throw "输出目录不存在：$parent"
        }
        Set-Content -LiteralPath $requestedOutputPath `
            -Value $json -Encoding utf8
    }
}
