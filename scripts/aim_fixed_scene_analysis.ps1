param(
    [string[]]$ReportPath = @(),
    [string]$RunDirectory = "",
    [string]$Scenario = "",
    [string]$OutputPath = "",
    [switch]$RequireXStability
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Test-XenFixedSceneField([object]$Sample, [string]$Name) {
    if ($Sample -is [System.Collections.IDictionary]) {
        return $Sample.Contains($Name)
    }
    return $Sample.PSObject.Properties.Name -contains $Name
}

function Get-XenFixedSceneField([object]$Sample, [string]$Name) {
    if ($Sample -is [System.Collections.IDictionary]) {
        return $Sample[$Name]
    }
    return $Sample.PSObject.Properties[$Name].Value
}

function Get-XenFixedScenePercentile(
    [double[]]$SortedValues,
    [double]$Quantile) {
    if ($SortedValues.Count -eq 0) { return $null }
    $position = $Quantile * ($SortedValues.Count - 1)
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$SortedValues[$lower] }
    $fraction = $position - $lower
    return [double]$SortedValues[$lower] * (1.0 - $fraction) +
        [double]$SortedValues[$upper] * $fraction
}

function Get-XenFixedSceneDistribution([double[]]$Values) {
    if ($Values.Count -eq 0) {
        return [ordered]@{
            sample_count = 0
            mean = $null
            p50 = $null
            p95 = $null
            p99 = $null
            maximum = $null
        }
    }
    [double[]]$sorted = @($Values | Sort-Object)
    [double]$sum = 0.0
    foreach ($value in $sorted) { $sum += $value }
    return [ordered]@{
        sample_count = $sorted.Count
        mean = $sum / $sorted.Count
        p50 = Get-XenFixedScenePercentile $sorted 0.50
        p95 = Get-XenFixedScenePercentile $sorted 0.95
        p99 = Get-XenFixedScenePercentile $sorted 0.99
        maximum = [double]$sorted[$sorted.Count - 1]
    }
}

function Get-XenFixedSceneCorrelation(
    [double[]]$Left,
    [double[]]$Right) {
    if ($Left.Count -ne $Right.Count -or $Left.Count -lt 2) { return $null }
    [double]$leftMean = ($Left | Measure-Object -Average).Average
    [double]$rightMean = ($Right | Measure-Object -Average).Average
    [double]$numerator = 0.0
    [double]$leftSquare = 0.0
    [double]$rightSquare = 0.0
    for ($index = 0; $index -lt $Left.Count; ++$index) {
        $leftDelta = $Left[$index] - $leftMean
        $rightDelta = $Right[$index] - $rightMean
        $numerator += $leftDelta * $rightDelta
        $leftSquare += $leftDelta * $leftDelta
        $rightSquare += $rightDelta * $rightDelta
    }
    $denominator = [Math]::Sqrt($leftSquare * $rightSquare)
    if ($denominator -le 0.0) { return $null }
    return $numerator / $denominator
}

function Get-XenFixedScenePointSummary(
    [double[]]$Errors,
    [double[]]$FirstDifferences,
    [double[]]$SecondDifferences,
    [int]$ZeroCrossings,
    [double]$DurationSeconds) {
    [double]$sum = 0.0
    foreach ($value in $Errors) { $sum += $value }
    return [ordered]@{
        sample_count = $Errors.Count
        mean_signed_error = if ($Errors.Count -eq 0) {
            $null
        } else {
            $sum / $Errors.Count
        }
        absolute_error = Get-XenFixedSceneDistribution @(
            $Errors | ForEach-Object { [Math]::Abs($_) })
        absolute_first_difference = Get-XenFixedSceneDistribution @(
            $FirstDifferences | ForEach-Object { [Math]::Abs($_) })
        absolute_second_difference = Get-XenFixedSceneDistribution @(
            $SecondDifferences | ForEach-Object { [Math]::Abs($_) })
        zero_crossings = $ZeroCrossings
        estimated_round_trip_hz = if ($DurationSeconds -gt 0.0) {
            $ZeroCrossings / (2.0 * $DurationSeconds)
        } else {
            $null
        }
    }
}

function Get-XenAimFixedSceneAnalysis {
    param(
        [Parameter(Mandatory = $true)][object[]]$Samples,
        [string]$Scenario = ""
    )

    $items = @($Samples)
    if ($items.Count -eq 0) {
        throw "固定场景分析至少需要一个 Runtime 样本。"
    }
    $usesLockField = @($items | Where-Object {
        Test-XenFixedSceneField $_ "aim_lock_active"
    }).Count -gt 0

    $pointStreamNames = @(
        "observation", "track", "reference", "normalized_reference",
        "base", "final")
    $streams = [ordered]@{}
    foreach ($axis in @("x", "y")) {
        $streams[$axis] = [ordered]@{}
        foreach ($name in $pointStreamNames) {
            $streams[$axis][$name] = [ordered]@{
                errors = [System.Collections.Generic.List[double]]::new()
                first = [System.Collections.Generic.List[double]]::new()
                second = [System.Collections.Generic.List[double]]::new()
                crossings = 0
            }
        }
        $streams[$axis].command = [ordered]@{
            values = [System.Collections.Generic.List[double]]::new()
            first = [System.Collections.Generic.List[double]]::new()
            reversals = 0
            nonzero = 0
        }
    }
    $pairedFirst = [ordered]@{
        x = [ordered]@{
            observation = [System.Collections.Generic.List[double]]::new()
            track = [System.Collections.Generic.List[double]]::new()
            base = [System.Collections.Generic.List[double]]::new()
        }
        y = [ordered]@{
            observation = [System.Collections.Generic.List[double]]::new()
            track = [System.Collections.Generic.List[double]]::new()
            base = [System.Collections.Generic.List[double]]::new()
        }
    }
    $previous = $null
    $previousFirst = $null
    $previousNonzeroSign = @{ x = 0; y = 0 }
    [int]$selectedFrames = 0
    [int]$contiguousPairs = 0
    [double]$durationSeconds = 0.0

    foreach ($sample in $items) {
        $required = @(
            "sequence", "aim_has_target", "aim_matched_observation_valid",
            "aim_track_id", "aim_control_center_x", "aim_control_center_y",
            "aim_matched_observation_box", "aim_box", "aim_base_point",
            "aim_final_point", "aim_command", "aim_controller_dt_ms")
        $missing = @($required | Where-Object {
            -not (Test-XenFixedSceneField $sample $_)
        })
        if ($missing.Count -ne 0) {
            throw "固定场景样本缺少字段：$($missing -join ', ')"
        }
        $selected = [bool](Get-XenFixedSceneField $sample "aim_has_target") -and
            [bool](Get-XenFixedSceneField $sample "aim_matched_observation_valid")
        if ($usesLockField) {
            $selected = $selected -and
                (Test-XenFixedSceneField $sample "aim_lock_active") -and
                [bool](Get-XenFixedSceneField $sample "aim_lock_active")
        } elseif (Test-XenFixedSceneField $sample "aim_control_evaluated") {
            $selected = $selected -and
                [bool](Get-XenFixedSceneField $sample "aim_control_evaluated")
        }
        if (-not $selected) {
            $previous = $null
            $previousFirst = $null
            $previousNonzeroSign.x = 0
            $previousNonzeroSign.y = 0
            continue
        }

        $observationBox = @(Get-XenFixedSceneField $sample `
            "aim_matched_observation_box")
        $trackBox = @(Get-XenFixedSceneField $sample "aim_box")
        $basePoint = @(Get-XenFixedSceneField $sample "aim_base_point")
        $finalPoint = @(Get-XenFixedSceneField $sample "aim_final_point")
        $command = @(Get-XenFixedSceneField $sample "aim_command")
        if ($observationBox.Count -ne 4 -or $trackBox.Count -ne 4 -or
            $basePoint.Count -ne 2 -or $finalPoint.Count -ne 2 -or
            $command.Count -ne 2) {
            throw "固定场景样本的框、点或命令维度不合法。"
        }
        $current = [ordered]@{
            sequence = [uint64](Get-XenFixedSceneField $sample "sequence")
            track_id = [uint64](Get-XenFixedSceneField $sample "aim_track_id")
            x = [ordered]@{
                observation = ([double]$observationBox[0] +
                    [double]$observationBox[2]) * 0.5 -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_x")
                track = ([double]$trackBox[0] + [double]$trackBox[2]) * 0.5 -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_x")
                reference = [double]$basePoint[0] -
                    ([double]$trackBox[0] + [double]$trackBox[2]) * 0.5
                normalized_reference = if (
                    [double]$trackBox[2] - [double]$trackBox[0] -gt 0.0) {
                    ([double]$basePoint[0] -
                        ([double]$trackBox[0] + [double]$trackBox[2]) * 0.5) /
                        ([double]$trackBox[2] - [double]$trackBox[0])
                } else {
                    0.0
                }
                base = [double]$basePoint[0] -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_x")
                final = [double]$finalPoint[0] -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_x")
                command = [double]$command[0]
            }
            y = [ordered]@{
                observation = ([double]$observationBox[1] +
                    [double]$observationBox[3]) * 0.5 -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_y")
                track = ([double]$trackBox[1] + [double]$trackBox[3]) * 0.5 -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_y")
                reference = [double]$basePoint[1] -
                    ([double]$trackBox[1] + [double]$trackBox[3]) * 0.5
                normalized_reference = if (
                    [double]$trackBox[3] - [double]$trackBox[1] -gt 0.0) {
                    ([double]$basePoint[1] -
                        ([double]$trackBox[1] + [double]$trackBox[3]) * 0.5) /
                        ([double]$trackBox[3] - [double]$trackBox[1])
                } else {
                    0.0
                }
                base = [double]$basePoint[1] -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_y")
                final = [double]$finalPoint[1] -
                    [double](Get-XenFixedSceneField $sample "aim_control_center_y")
                command = [double]$command[1]
            }
        }
        ++$selectedFrames
        $controllerDtMs = [double](Get-XenFixedSceneField `
            $sample "aim_controller_dt_ms")
        if ($controllerDtMs -gt 0.0 -and
            -not [double]::IsNaN($controllerDtMs) -and
            -not [double]::IsInfinity($controllerDtMs)) {
            $durationSeconds += $controllerDtMs / 1000.0
        }

        foreach ($axis in @("x", "y")) {
            foreach ($name in $pointStreamNames) {
                $streams[$axis][$name].errors.Add(
                    [double]$current[$axis][$name])
            }
            $streams[$axis].command.values.Add(
                [double]$current[$axis].command)
            if ([double]$current[$axis].command -ne 0.0) {
                ++$streams[$axis].command.nonzero
            }
        }

        $contiguous = $null -ne $previous -and
            $current.track_id -eq $previous.track_id -and
            $current.sequence -eq $previous.sequence + 1
        if ($contiguous) {
            ++$contiguousPairs
            $currentFirst = [ordered]@{ x = [ordered]@{}; y = [ordered]@{} }
            foreach ($axis in @("x", "y")) {
                foreach ($name in $pointStreamNames) {
                    $difference = [double]$current[$axis][$name] -
                        [double]$previous[$axis][$name]
                    $currentFirst[$axis][$name] = $difference
                    $streams[$axis][$name].first.Add($difference)
                    if ($previous[$axis][$name] -ne 0.0 -and
                        $current[$axis][$name] -ne 0.0 -and
                        [Math]::Sign($previous[$axis][$name]) -ne
                            [Math]::Sign($current[$axis][$name])) {
                        ++$streams[$axis][$name].crossings
                    }
                }
                foreach ($name in @("observation", "track", "base")) {
                    $pairedFirst[$axis][$name].Add(
                        [double]$currentFirst[$axis][$name])
                }
                $commandDifference = [double]$current[$axis].command -
                    [double]$previous[$axis].command
                $streams[$axis].command.first.Add($commandDifference)
                $commandSign = [Math]::Sign([double]$current[$axis].command)
                if ($commandSign -ne 0) {
                    if ($previousNonzeroSign[$axis] -ne 0 -and
                        $commandSign -ne $previousNonzeroSign[$axis]) {
                        ++$streams[$axis].command.reversals
                    }
                    $previousNonzeroSign[$axis] = $commandSign
                }
            }
            if ($null -ne $previousFirst) {
                foreach ($axis in @("x", "y")) {
                    foreach ($name in $pointStreamNames) {
                        $streams[$axis][$name].second.Add(
                            [double]$currentFirst[$axis][$name] -
                            [double]$previousFirst[$axis][$name])
                    }
                }
            }
            $previousFirst = $currentFirst
        } else {
            $previousFirst = $null
            foreach ($axis in @("x", "y")) {
                $commandSign = [Math]::Sign([double]$current[$axis].command)
                $previousNonzeroSign[$axis] = $commandSign
            }
        }
        $previous = $current
    }

    if ($selectedFrames -eq 0) {
        throw "固定场景报告没有已锁定且带当前 Observation 的控制帧。"
    }
    $result = [ordered]@{
        schema = 1
        scenario = if ([string]::IsNullOrWhiteSpace($Scenario)) {
            $null
        } else {
            $Scenario
        }
        sample_count = $items.Count
        selected_frames = $selectedFrames
        contiguous_pairs = $contiguousPairs
        duration_seconds = $durationSeconds
        selection = if ($usesLockField) {
            "aim_lock_active && aim_has_target && aim_matched_observation_valid"
        } else {
            "aim_control_evaluated && aim_has_target && aim_matched_observation_valid"
        }
    }
    foreach ($axis in @("x", "y")) {
        $axisResult = [ordered]@{}
        foreach ($name in $pointStreamNames) {
            $axisResult[$name] = Get-XenFixedScenePointSummary `
                $streams[$axis][$name].errors.ToArray() `
                $streams[$axis][$name].first.ToArray() `
                $streams[$axis][$name].second.ToArray() `
                $streams[$axis][$name].crossings $durationSeconds
        }
        $axisResult.command = [ordered]@{
            sample_count = $streams[$axis].command.values.Count
            nonzero_frames = $streams[$axis].command.nonzero
            direction_reversals = $streams[$axis].command.reversals
            estimated_round_trip_hz = if ($durationSeconds -gt 0.0) {
                $streams[$axis].command.reversals /
                    (2.0 * $durationSeconds)
            } else {
                $null
            }
            absolute_value = Get-XenFixedSceneDistribution @(
                $streams[$axis].command.values.ToArray() |
                    ForEach-Object { [Math]::Abs($_) })
            absolute_first_difference = Get-XenFixedSceneDistribution @(
                $streams[$axis].command.first.ToArray() |
                    ForEach-Object { [Math]::Abs($_) })
        }
        $axisResult.first_difference_correlation = [ordered]@{
            observation_to_track = Get-XenFixedSceneCorrelation `
                $pairedFirst[$axis].observation.ToArray() `
                $pairedFirst[$axis].track.ToArray()
            track_to_base = Get-XenFixedSceneCorrelation `
                $pairedFirst[$axis].track.ToArray() `
                $pairedFirst[$axis].base.ToArray()
            observation_to_base = Get-XenFixedSceneCorrelation `
                $pairedFirst[$axis].observation.ToArray() `
                $pairedFirst[$axis].base.ToArray()
        }
        $observationP95 = $axisResult.observation.absolute_error.p95
        $baseP95 = $axisResult.base.absolute_error.p95
        $observationD1P95 =
            $axisResult.observation.absolute_first_difference.p95
        $baseD1P95 = $axisResult.base.absolute_first_difference.p95
        $axisResult.base_to_observation_absolute_error_p95_ratio =
            if ($null -eq $observationP95 -or $observationP95 -le 0.0) {
                $null
            } else {
                $baseP95 / $observationP95
            }
        $axisResult.base_to_observation_first_difference_p95_ratio =
            if ($null -eq $observationD1P95 -or
                $observationD1P95 -le 0.0) {
                $null
            } else {
                $baseD1P95 / $observationD1P95
            }
        $axisResult.stability_passed = $null -ne $observationP95 -and
            $null -ne $observationD1P95 -and
            $baseP95 -le $observationP95 + 0.000001 -and
            $baseD1P95 -le $observationD1P95 + 0.000001
        $result[$axis] = $axisResult
    }
    return $result
}

if ($MyInvocation.InvocationName -ne ".") {
    if ($ReportPath.Count -ne 0 -and
        -not [string]::IsNullOrWhiteSpace($RunDirectory)) {
        throw "-ReportPath 与 -RunDirectory 只能使用一个。"
    }
    $inputReports = [System.Collections.Generic.List[string]]::new()
    foreach ($path in $ReportPath) {
        $inputReports.Add((Resolve-Path -LiteralPath $path).Path)
    }
    if (-not [string]::IsNullOrWhiteSpace($RunDirectory)) {
        $resolvedRun = (Resolve-Path -LiteralPath $RunDirectory).Path
        $automaticDirectory = Join-Path $resolvedRun "automatic"
        if (-not (Test-Path -LiteralPath $automaticDirectory `
                -PathType Container)) {
            throw "Run 缺少 automatic 目录：$resolvedRun"
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $automaticDirectory `
                -Filter "*.json" -File | Sort-Object Name)) {
            $inputReports.Add($file.FullName)
        }
    }
    if ($inputReports.Count -eq 0) {
        throw "请通过 -ReportPath 或 -RunDirectory 指定 Runtime JSON。"
    }
    $allSamples = [System.Collections.Generic.List[object]]::new()
    foreach ($resolvedReport in $inputReports) {
        $report = Get-Content -LiteralPath $resolvedReport -Raw -Encoding utf8 |
            ConvertFrom-Json
        if ($null -eq $report.samples) {
            throw "Runtime JSON 不包含 samples：$resolvedReport"
        }
        foreach ($sample in @($report.samples)) { $allSamples.Add($sample) }
    }
    $analysis = Get-XenAimFixedSceneAnalysis `
        -Samples $allSamples.ToArray() -Scenario $Scenario
    $json = $analysis | ConvertTo-Json -Depth 12
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $json
    } else {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent) -and
            -not (Test-Path -LiteralPath $parent -PathType Container)) {
            throw "输出目录不存在：$parent"
        }
        Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8
    }
    if ($RequireXStability.IsPresent -and
        -not [bool]$analysis.x.stability_passed) {
        [Console]::Error.WriteLine(
            "固定场景 X 稳定性门禁失败：base 放大了 Observation。")
        exit 2
    }
}
