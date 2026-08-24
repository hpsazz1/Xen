$ErrorActionPreference = "Stop"

function ConvertTo-XenAimFiniteDouble {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Description
    )

    try {
        $number = [double]$Value
    } catch {
        throw "$Description 不是合法数值。"
    }
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        throw "$Description 不是有限数值。"
    }
    return $number
}

function Get-XenAimPercentile {
    param(
        [Parameter(Mandatory = $true)][double[]]$SortedValues,
        [Parameter(Mandatory = $true)][double]$Quantile
    )

    if ($SortedValues.Count -eq 0) { return $null }
    $position = $Quantile * ($SortedValues.Count - 1)
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$SortedValues[$lower] }
    $fraction = $position - $lower
    return [double]$SortedValues[$lower] * (1.0 - $fraction) +
        [double]$SortedValues[$upper] * $fraction
}

function Get-XenAimDistributionSummary {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[double]]$Values
    )

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
    [double[]]$sorted = @($Values.ToArray() | Sort-Object)
    [double]$sum = 0.0
    foreach ($value in $sorted) { $sum += $value }
    return [ordered]@{
        sample_count = $sorted.Count
        mean = $sum / $sorted.Count
        p50 = Get-XenAimPercentile $sorted 0.50
        p95 = Get-XenAimPercentile $sorted 0.95
        p99 = Get-XenAimPercentile $sorted 0.99
        maximum = [double]$sorted[$sorted.Count - 1]
    }
}

function Get-XenAimReportSummary {
    param(
        [Parameter(Mandatory = $true)][object[]]$Samples,
        [Parameter(Mandatory = $true)]
        [ValidateSet("on", "off")][string]$PredictionEnabled,
        [Parameter(Mandatory = $true)]
        [ValidateRange(1.0, 50.0)][double]$MaxPredictionLeadPercent
    )

    $items = @($Samples)
    if ($items.Count -eq 0) {
        throw "Aim schema 12 报告没有正式样本。"
    }
    $requiredFields = @(
        "aim_status", "mouse_sent", "aim_has_target", "aim_has_command",
        "aim_track_id", "aim_track_state", "aim_track_predicted",
        "aim_lead_active", "aim_base_point_inside_box",
        "aim_prediction_point_outside_box", "aim_command_toward_target",
        "aim_acquisition_range_radius", "aim_active_range_radius",
        "aim_range_locked", "aim_range_allows_control", "aim_box",
        "aim_base_point", "aim_delay_compensated_point", "aim_final_point",
        "aim_lead", "aim_delay_compensation",
        "aim_delay_compensation_active", "aim_delay_compensation_ms_x",
        "aim_delay_compensation_ms_y", "aim_delay_compensation_ms",
        "aim_observation_age_ms", "aim_command")
    $availableFields = if ($items[0] -is
            [System.Collections.IDictionary]) {
        @($items[0].Keys)
    } else {
        @($items[0].PSObject.Properties.Name)
    }
    $missingFields = @($requiredFields | Where-Object {
        $availableFields -notcontains $_
    })
    if ($missingFields.Count -ne 0) {
        throw "Aim schema 12 样本缺少字段：$($missingFields -join ', ')"
    }

    $leadDistances = [System.Collections.Generic.List[double]]::new()
    $observationAges = [System.Collections.Generic.List[double]]::new()
    $acquisitionRanges = [System.Collections.Generic.List[double]]::new()
    $activeRanges = [System.Collections.Generic.List[double]]::new()
    $violations = [ordered]@{
        aim_status_frames = [uint64]0
        output_contract_frames = [uint64]0
        target_geometry_frames = [uint64]0
        base_point_outside_box_frames = [uint64]0
        prediction_marker_mismatch_frames = [uint64]0
        lead_vector_consistency_frames = [uint64]0
        lead_limit_frames = [uint64]0
        prediction_disabled_lead_frames = [uint64]0
        command_direction_frames = [uint64]0
        range_contract_frames = [uint64]0
    }
    [uint64]$targetFrames = 0
    [uint64]$noTargetFrames = 0
    [uint64]$commandFrames = 0
    [uint64]$precomputedCommandFrames = 0
    [uint64]$mouseSentFrames = 0
    [uint64]$predictedTargetFrames = 0
    [uint64]$leadActiveFrames = 0
    [uint64]$predictionPointOutsideBoxFrames = 0
    [uint64]$rangeLockedFrames = 0
    [uint64]$rangeBlockedTargetFrames = 0
    [uint64]$targetSwitches = 0
    [uint64]$targetStateChanges = 0
    [uint64]$predictionStateChanges = 0
    $hasPreviousTarget = $false
    [uint64]$previousTrackId = 0
    [string]$previousTrackState = ""
    $previousTrackPredicted = $false
    $predictionEnabledValue = $PredictionEnabled -eq "on"
    $geometryTolerance = 0.01

    for ($index = 0; $index -lt $items.Count; ++$index) {
        $sample = $items[$index]
        if ([string]$sample.aim_status -ne "SUCCESS") {
            ++$violations.aim_status_frames
            $hasPreviousTarget = $false
            continue
        }

        $hasTarget = [bool]$sample.aim_has_target
        $hasCommand = [bool]$sample.aim_has_command
        $mouseSent = [bool]$sample.mouse_sent
        if ($mouseSent) { ++$mouseSentFrames }
        if (-not $hasTarget) {
            ++$noTargetFrames
            if ($hasCommand -or $mouseSent -or
                [bool]$sample.aim_range_locked -or
                [bool]$sample.aim_range_allows_control -or
                [bool]$sample.aim_lead_active) {
                ++$violations.output_contract_frames
            }
            $hasPreviousTarget = $false
            continue
        }

        ++$targetFrames
        [uint64]$trackId = [uint64]$sample.aim_track_id
        [string]$trackState = [string]$sample.aim_track_state
        $trackPredicted = [bool]$sample.aim_track_predicted
        if ($trackId -eq 0 -or
            $trackState -notin @("TENTATIVE", "CONFIRMED", "LOST") -or
            $trackPredicted -ne ($trackState -eq "LOST")) {
            ++$violations.output_contract_frames
        }
        if ($trackPredicted) { ++$predictedTargetFrames }
        if ($hasPreviousTarget) {
            if ($previousTrackId -ne $trackId) {
                ++$targetSwitches
            } else {
                if ($previousTrackState -ne $trackState) {
                    ++$targetStateChanges
                }
                if ($previousTrackPredicted -ne $trackPredicted) {
                    ++$predictionStateChanges
                }
            }
        }
        $hasPreviousTarget = $true
        $previousTrackId = $trackId
        $previousTrackState = $trackState
        $previousTrackPredicted = $trackPredicted

        $box = @($sample.aim_box)
        $basePoint = @($sample.aim_base_point)
        $delayCompensatedPoint = @($sample.aim_delay_compensated_point)
        $finalPoint = @($sample.aim_final_point)
        $lead = @($sample.aim_lead)
        $delayCompensation = @($sample.aim_delay_compensation)
        $command = @($sample.aim_command)
        if ($box.Count -ne 4 -or $basePoint.Count -ne 2 -or
            $delayCompensatedPoint.Count -ne 2 -or
            $finalPoint.Count -ne 2 -or $lead.Count -ne 2 -or
            $delayCompensation.Count -ne 2 -or $command.Count -ne 2) {
            throw "第 $index 个 Aim 样本的向量维度无效。"
        }
        $x1 = ConvertTo-XenAimFiniteDouble $box[0] "第 $index 个框 x1"
        $y1 = ConvertTo-XenAimFiniteDouble $box[1] "第 $index 个框 y1"
        $x2 = ConvertTo-XenAimFiniteDouble $box[2] "第 $index 个框 x2"
        $y2 = ConvertTo-XenAimFiniteDouble $box[3] "第 $index 个框 y2"
        $baseX = ConvertTo-XenAimFiniteDouble $basePoint[0] `
            "第 $index 个基础点 x"
        $baseY = ConvertTo-XenAimFiniteDouble $basePoint[1] `
            "第 $index 个基础点 y"
        $finalX = ConvertTo-XenAimFiniteDouble $finalPoint[0] `
            "第 $index 个最终点 x"
        $finalY = ConvertTo-XenAimFiniteDouble $finalPoint[1] `
            "第 $index 个最终点 y"
        $leadX = ConvertTo-XenAimFiniteDouble $lead[0] `
            "第 $index 个提前量 x"
        $leadY = ConvertTo-XenAimFiniteDouble $lead[1] `
            "第 $index 个提前量 y"
        $commandX = ConvertTo-XenAimFiniteDouble $command[0] `
            "第 $index 个命令 x"
        $commandY = ConvertTo-XenAimFiniteDouble $command[1] `
            "第 $index 个命令 y"
        if ($x2 -le $x1 -or $y2 -le $y1) {
            ++$violations.target_geometry_frames
        }

        $baseInside = $baseX -ge $x1 -and $baseX -le $x2 -and
            $baseY -ge $y1 -and $baseY -le $y2
        if (-not $baseInside -or
            -not [bool]$sample.aim_base_point_inside_box) {
            ++$violations.base_point_outside_box_frames
        }
        $leadActive = [bool]$sample.aim_lead_active
        if ($leadActive) { ++$leadActiveFrames }
        $finalOutside = $finalX -lt $x1 -or $finalX -gt $x2 -or
            $finalY -lt $y1 -or $finalY -gt $y2
        $reportedPredictionOutside =
            [bool]$sample.aim_prediction_point_outside_box
        $expectedPredictionOutside = $leadActive -and $finalOutside
        if ($reportedPredictionOutside -ne $expectedPredictionOutside) {
            ++$violations.prediction_marker_mismatch_frames
        }
        if ($expectedPredictionOutside) {
            ++$predictionPointOutsideBoxFrames
        }

        $leadDistance = [Math]::Sqrt($leadX * $leadX + $leadY * $leadY)
        $delayX = ConvertTo-XenAimFiniteDouble `
            $delayCompensation[0] "第 $index 个延迟补偿 X"
        $delayY = ConvertTo-XenAimFiniteDouble `
            $delayCompensation[1] "第 $index 个延迟补偿 Y"
        $delayCompensatedX = ConvertTo-XenAimFiniteDouble `
            $delayCompensatedPoint[0] "第 $index 个延迟补偿点 X"
        $delayCompensatedY = ConvertTo-XenAimFiniteDouble `
            $delayCompensatedPoint[1] "第 $index 个延迟补偿点 Y"
        $delayMsX = ConvertTo-XenAimFiniteDouble `
            $sample.aim_delay_compensation_ms_x "第 $index 个 X 延迟补偿时域"
        $delayMsY = ConvertTo-XenAimFiniteDouble `
            $sample.aim_delay_compensation_ms_y "第 $index 个 Y 延迟补偿时域"
        $delayMs = ConvertTo-XenAimFiniteDouble `
            $sample.aim_delay_compensation_ms "第 $index 个兼容延迟补偿时域"
        $delayActive = [bool]$sample.aim_delay_compensation_active
        $targetWidth = [Math]::Max(0.0, $x2 - $x1)
        $targetHeight = [Math]::Max(0.0, $y2 - $y1)
        $targetDiagonal = [Math]::Sqrt(
            $targetWidth * $targetWidth + $targetHeight * $targetHeight)
        $leadLimit = $targetDiagonal * $MaxPredictionLeadPercent / 100.0
        if ($leadDistance -gt $leadLimit + $geometryTolerance) {
            ++$violations.lead_limit_frames
        }
        if ([Math]::Abs($delayCompensatedX - $baseX - $delayX) -gt
                $geometryTolerance -or
            [Math]::Abs($delayCompensatedY - $baseY - $delayY) -gt
                $geometryTolerance -or
            [Math]::Abs($finalX - $delayCompensatedX - $leadX) -gt
                $geometryTolerance -or
            [Math]::Abs($finalY - $delayCompensatedY - $leadY) -gt
                $geometryTolerance -or
            $delayMsX -lt 0.0 -or $delayMsY -lt 0.0 -or
            [Math]::Abs($delayMs - [Math]::Max($delayMsX, $delayMsY)) -gt
                $geometryTolerance -or
            (-not $delayActive -and
             ([Math]::Abs($delayX) -gt $geometryTolerance -or
              [Math]::Abs($delayY) -gt $geometryTolerance -or
              [Math]::Abs($delayMsX) -gt $geometryTolerance -or
              [Math]::Abs($delayMsY) -gt $geometryTolerance -or
              [Math]::Abs($delayMs) -gt $geometryTolerance)) -or
            (-not $leadActive -and $leadDistance -gt $geometryTolerance)) {
            ++$violations.lead_vector_consistency_frames
        }
        if (-not $predictionEnabledValue -and
            ($leadActive -or $leadDistance -gt $geometryTolerance -or
             $reportedPredictionOutside)) {
            ++$violations.prediction_disabled_lead_frames
        }
        $leadDistances.Add($leadDistance)
        $observationAge = ConvertTo-XenAimFiniteDouble `
            $sample.aim_observation_age_ms "第 $index 个观测年龄"
        if ($observationAge -lt 0.0 -or $observationAge -gt 100.001) {
            ++$violations.output_contract_frames
        }
        $observationAges.Add($observationAge)

        $acquisitionRange = ConvertTo-XenAimFiniteDouble `
            $sample.aim_acquisition_range_radius "第 $index 个获取范围"
        $activeRange = ConvertTo-XenAimFiniteDouble `
            $sample.aim_active_range_radius "第 $index 个活动范围"
        if ($acquisitionRange -le 0.0 -or $activeRange -le 0.0 -or
            $activeRange -gt $acquisitionRange + $geometryTolerance) {
            ++$violations.range_contract_frames
        }
        $acquisitionRanges.Add($acquisitionRange)
        $activeRanges.Add($activeRange)
        if ([bool]$sample.aim_range_locked) { ++$rangeLockedFrames }
        if (-not [bool]$sample.aim_range_allows_control) {
            ++$rangeBlockedTargetFrames
        }

        if ($hasCommand) {
            ++$commandFrames
            if (-not $mouseSent) { ++$precomputedCommandFrames }
            if (-not [bool]$sample.aim_range_allows_control -or
                ($commandX -eq 0.0 -and $commandY -eq 0.0)) {
                ++$violations.output_contract_frames
            }
            if (-not [bool]$sample.aim_command_toward_target) {
                ++$violations.command_direction_frames
            }
        } elseif ($mouseSent -or $commandX -ne 0.0 -or $commandY -ne 0.0) {
            ++$violations.output_contract_frames
        }
    }

    [uint64]$violationCount = 0
    foreach ($value in $violations.Values) {
        $violationCount += [uint64]$value
    }
    $messages = @()
    if ($violations.base_point_outside_box_frames -ne 0) {
        $messages += "基础追踪点在目标框外：$($violations.base_point_outside_box_frames) 帧"
    }
    if ($violations.lead_limit_frames -ne 0) {
        $messages += "预测提前量超过最大距离：$($violations.lead_limit_frames) 帧"
    }
    if ($violations.prediction_disabled_lead_frames -ne 0) {
        $messages += "预测关闭时仍存在提前量：$($violations.prediction_disabled_lead_frames) 帧"
    }
    if ($violations.command_direction_frames -ne 0) {
        $messages += "控制命令未朝向当前基础/预测点：$($violations.command_direction_frames) 帧"
    }
    foreach ($name in @(
            "aim_status_frames", "output_contract_frames",
            "target_geometry_frames", "prediction_marker_mismatch_frames",
            "lead_vector_consistency_frames", "range_contract_frames")) {
        if ([uint64]$violations[$name] -ne 0) {
            $messages += "Aim 契约 $name 违规：$($violations[$name]) 帧"
        }
    }

    return [ordered]@{
        schema = 1
        contract_valid = $violationCount -eq 0
        prediction_enabled = $predictionEnabledValue
        max_prediction_lead_percent = $MaxPredictionLeadPercent
        sample_count = $items.Count
        target_frames = $targetFrames
        no_target_frames = $noTargetFrames
        command_frames = $commandFrames
        precomputed_command_frames = $precomputedCommandFrames
        mouse_sent_frames = $mouseSentFrames
        predicted_target_frames = $predictedTargetFrames
        lead_active_frames = $leadActiveFrames
        prediction_point_outside_box_frames =
            $predictionPointOutsideBoxFrames
        range_locked_frames = $rangeLockedFrames
        range_blocked_target_frames = $rangeBlockedTargetFrames
        target_switches = $targetSwitches
        target_state_changes = $targetStateChanges
        prediction_state_changes = $predictionStateChanges
        contract = [ordered]@{
            basic_points_must_remain_inside_selected_box = $true
            prediction_points_may_leave_selected_box = $true
            prediction_points_are_not_clamped_to_selected_box = $true
            prediction_lead_is_limited_by_target_diagonal_percent = $true
        }
        violations = $violations
        violation_count = $violationCount
        violation_messages = @($messages)
        lead_distance_pixels = Get-XenAimDistributionSummary $leadDistances
        observation_age_ms = Get-XenAimDistributionSummary $observationAges
        acquisition_range_pixels =
            Get-XenAimDistributionSummary $acquisitionRanges
        active_range_pixels = Get-XenAimDistributionSummary $activeRanges
    }
}
