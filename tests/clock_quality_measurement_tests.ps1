param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        $Actual,
        [Parameter(Mandatory = $true)]
        $Expected,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Actual -ne $Expected) {
        throw "$Message (actual=$Actual expected=$Expected)"
    }
}

function Assert-FiniteDouble {
    param(
        [Parameter(Mandatory = $true)]
        [double]$Value,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value)) {
        throw "$Message (actual=$Value)"
    }
}

function Assert-Approx {
    param(
        [Parameter(Mandatory = $true)]
        [double]$Actual,
        [Parameter(Mandatory = $true)]
        [double]$Expected,
        [Parameter(Mandatory = $true)]
        [double]$Tolerance,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Assert-FiniteDouble $Actual "$Message actual 必须为有限数"
    Assert-FiniteDouble $Expected "$Message expected 必须为有限数"
    Assert-FiniteDouble $Tolerance "$Message tolerance 必须为有限数"
    if ([Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

function New-ClockSampleCsv {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [long]$BiasNanoseconds,
        [switch]$Alternating,
        [int]$SampleCount = 482,
        [int]$SessionChangeIndex = -1
    )

    # 这里的已知四时间戳只验证测量器的单位、符号与分布消费；数值不是生产门槛。
    [long]$sourceOriginNs = 1700000000000
    [long]$localOriginNs = 1000000000000
    [long]$sampleSpacingNs = 250000000
    [long]$oneWayBaseNs = 200000
    [long]$processingNs = 50000
    [long]$probeOffsetNs = 100000000
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('source_session_id,requester_send_steady_ns,source_receive_utc_ns,source_send_utc_ns,requester_receive_steady_ns,probe_source_utc_ns,probe_reference_local_ns')

    for ($index = 0; $index -lt $SampleCount; ++$index) {
        [long]$sampleBiasNs = $BiasNanoseconds
        if ($Alternating -and (($index % 2) -ne 0)) {
            $sampleBiasNs = -$sampleBiasNs
        }
        [long]$forwardNs = $oneWayBaseNs - $sampleBiasNs
        [long]$reverseNs = $oneWayBaseNs + $sampleBiasNs
        Assert-True ($forwardNs -ge 0 -and $reverseNs -ge 0) `
            '测试 fixture 的单向时延必须非负。'

        [long]$sourceAtRequestNs = $sourceOriginNs + $index * $sampleSpacingNs
        [long]$requesterSendNs = $localOriginNs + $index * $sampleSpacingNs
        [long]$sourceReceiveNs = $sourceAtRequestNs + $forwardNs
        [long]$sourceSendNs = $sourceReceiveNs + $processingNs
        [long]$requesterReceiveNs =
            $requesterSendNs + $forwardNs + $processingNs + $reverseNs
        [long]$probeSourceNs = $sourceAtRequestNs + $probeOffsetNs
        [long]$probeReferenceLocalNs = $requesterSendNs + $probeOffsetNs
        [long]$sessionId = if ($SessionChangeIndex -ge 0 -and
            $index -ge $SessionChangeIndex) { 8 } else { 7 }
        $lines.Add("$sessionId,$requesterSendNs,$sourceReceiveNs,$sourceSendNs,$requesterReceiveNs,$probeSourceNs,$probeReferenceLocalNs")
    }

    [System.IO.File]::WriteAllLines($Path, $lines,
        [System.Text.Encoding]::ASCII)
}

function Invoke-Measurement {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SamplesPath
    )

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 会把原生进程 stderr 包装成非终止错误；测试需同时
        # 保留 stderr 与退出码，才能验证预期的不可判定退出。
        $ErrorActionPreference = 'Continue'
        $raw = @(& $Executable --samples $SamplesPath 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines = @($raw | ForEach-Object { [string]$_ })
        Text = [string]::Join([Environment]::NewLine,
            @($raw | ForEach-Object { [string]$_ }))
    }
}

function Convert-KeyValueLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line
    )

    $fields = @{}
    foreach ($part in ($Line -split ',')) {
        $pair = $part -split '=', 2
        if ($pair.Count -eq 2) {
            $fields[$pair[0]] = $pair[1]
        }
    }
    return $fields
}

function Get-OutputFields {
    param(
        [Parameter(Mandatory = $true)]
        $Result,
        [Parameter(Mandatory = $true)]
        [string]$Prefix,
        [string]$Component = ''
    )

    foreach ($line in $Result.Lines) {
        if (-not $line.StartsWith($Prefix + ',')) {
            continue
        }
        $fields = Convert-KeyValueLine $line
        if ($Component -eq '' -or
            ($fields.ContainsKey('error_component') -and
             $fields['error_component'] -eq $Component)) {
            return $fields
        }
    }
    throw "未找到输出行 prefix=$Prefix component=$Component。`n$($Result.Text)"
}

function Get-DoubleField {
    param(
        [Parameter(Mandatory = $true)]
        $Fields,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    Assert-True $Fields.ContainsKey($Name) "输出缺少字段 $Name。"
    $value = [double]::Parse($Fields[$Name],
        [Globalization.CultureInfo]::InvariantCulture)
    Assert-FiniteDouble $value "输出字段 $Name 必须为有限数"
    return $value
}

function Assert-ComparableAim {
    param(
        [Parameter(Mandatory = $true)]
        $Result,
        [Parameter(Mandatory = $true)]
        [bool]$ExpectDifference
    )

    $aim = Get-OutputFields $Result 'aim' 'synthetic_sequence'
    Assert-Equal $aim['status'] 'comparable' 'Aim 非时序 sequence stress 应保持可比。'
    Assert-Equal $aim['time_aligned'] 'false' `
        '非时序 stress 不得伪装为 clock 与 Aim 时间对齐。'
    Assert-Equal $aim['mapping_age_evaluated'] 'false' `
        '本工具不得声称已量化 mapping age。'
    Assert-Approx (Get-DoubleField $aim 'clock_sample_interval_p50_ms') `
        250.0 0.000001 'clock 样本间隔 P50 应保留原始 250 ms cadence。'
    Assert-Approx (Get-DoubleField $aim 'aim_frame_interval_p50_ms') `
        4.2205 0.000001 'Aim 回放帧间隔 P50 应保留原 fixture cadence。'
    Assert-True ((Get-DoubleField $aim 'clock_to_aim_cadence_ratio') -gt 50.0) `
        'clock 与 Aim cadence 差异必须显式报告，不得隐含按帧对齐。'
    Assert-Equal $aim['completion_history_owner'] 'per_replay_actual_command' `
        '两路 Aim 必须各自消费实际完成命令。'
    Assert-Equal $aim['trajectory_comparable'] 'true' 'Aim 轨迹应保持可比。'
    $changedAge = [int]$aim['changed_observation_age_frames']
    $changedPoint = [int]$aim['changed_aim_point_frames']
    if ($ExpectDifference) {
        Assert-True ($changedAge -gt 0) '非零 mapper 误差必须改变 Aim observation age。'
    } else {
        Assert-Equal $changedAge 0 '零 mapper 误差不得改变 Aim observation age。'
        Assert-Equal $changedPoint 0 '零 mapper 误差不得改变 Aim point。'
        Assert-Equal ([int]$aim['changed_command_frames']) 0 `
            '零 mapper 误差不得改变 Aim command。'
    }
}

$nonFiniteCases = @(
    @{ Name = 'NaN'; Value = [double]::NaN },
    @{ Name = 'positive infinity'; Value = [double]::PositiveInfinity },
    @{ Name = 'negative infinity'; Value = [double]::NegativeInfinity }
)
foreach ($case in $nonFiniteCases) {
    $approxRejected = $false
    try {
        Assert-Approx $case.Value 0.0 0.1 `
            "Assert-Approx 必须拒绝 $($case.Name)。"
    } catch {
        $approxRejected = $true
    }
    Assert-True $approxRejected `
        "Assert-Approx 不得让 $($case.Name) 绕过数值 oracle。"

    $parserRejected = $false
    try {
        [void](Get-DoubleField @{ value = $case.Value.ToString(
            [Globalization.CultureInfo]::InvariantCulture) } 'value')
    } catch {
        $parserRejected = $true
    }
    Assert-True $parserRejected `
        "Get-DoubleField 必须在解析处拒绝 $($case.Name)。"
}

New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null

$zeroPath = Join-Path $TestRoot 'zero.csv'
New-ClockSampleCsv -Path $zeroPath -BiasNanoseconds 0
$zero = Invoke-Measurement $zeroPath
Assert-Equal $zero.ExitCode 0 "零误差 fixture 应成功。`n$($zero.Text)"
$zeroMapper = Get-OutputFields $zero 'mapper'
Assert-Equal $zeroMapper['data_source'] 'explicit_four_timestamp_csv' `
    '测量器必须消费逐样本四时间戳输入。'
Assert-Equal ([int]$zeroMapper['sample_rows']) 482 '必须消费全部显式样本。'
Assert-Equal ([int]$zeroMapper['mapped_error_samples']) 480 `
    '两帧预热后应形成完整 Aim 非时序 stress 误差序列。'
Assert-Equal $zeroMapper['metric_sample_domain'] 'mapped_valid_samples' `
    'RTT/residual/uncertainty/error 必须共用 mapped VALID 样本域。'
Assert-Equal ([int]$zeroMapper['input_rtt_samples']) 480 `
    'RTT 分布不得夹带两帧 WARMING 样本。'
Assert-Equal ([int]$zeroMapper['clock_sample_intervals']) 479 `
    '480 个 mapped VALID 样本应保留 479 个 clock cadence 间隔。'
Assert-Approx (Get-DoubleField $zeroMapper 'input_rtt_p95_ms') `
    0.4 0.000001 '400000 ns RTT 必须按 0.4 ms 报告。'
Assert-Approx (Get-DoubleField $zeroMapper 'fit_residual_abs_max_ms') `
    0.0 0.000001 '完全仿射 fixture 的残差应为 0 ms。'
Assert-Approx (Get-DoubleField $zeroMapper 'reported_uncertainty_p95_ms') `
    0.2 0.000001 '对称 0.4 ms RTT 的 Mapper uncertainty 应为 0.2 ms。'
Assert-Approx (Get-DoubleField $zeroMapper 'mapping_error_signed_mean_ms') `
    0.0 0.000001 '零误差 fixture 的映射误差应为 0 ms。'
Assert-Approx (Get-DoubleField $zeroMapper 'constant_bias_ms') `
    0.0 0.000001 '零误差 fixture 的 constant bias 应为 0 ms。'
Assert-Approx (Get-DoubleField $zeroMapper 'per_sample_stress_max_abs_ms') `
    0.0 0.000001 '零误差 fixture 的逐样本 stress 应为 0 ms。'
Assert-ComparableAim $zero $false

$shortPath = Join-Path $TestRoot 'short-481.csv'
New-ClockSampleCsv -Path $shortPath -BiasNanoseconds 0 -SampleCount 481
$short = Invoke-Measurement $shortPath
Assert-Equal $short.ExitCode 1 '少于 482 行的输入必须拒绝。'
Assert-True ($short.Text -match `
    'unexpected_sample_count_expected_482_actual_481') `
    '过短输入必须通过精确行数合同 fail-closed。'

$longPath = Join-Path $TestRoot 'long-483.csv'
New-ClockSampleCsv -Path $longPath -BiasNanoseconds 0 -SampleCount 483
$long = Invoke-Measurement $longPath
Assert-Equal $long.ExitCode 1 '多于 482 行的输入必须拒绝。'
Assert-True ($long.Text -match `
    'unexpected_sample_count_expected_482_actual_483') `
    '过长输入不得被截断成看似合法的 480 样本 stress。'

$changedSessionPath = Join-Path $TestRoot 'changed-session.csv'
New-ClockSampleCsv -Path $changedSessionPath -BiasNanoseconds 0 `
    -SessionChangeIndex 241
$changedSession = Invoke-Measurement $changedSessionPath
Assert-Equal $changedSession.ExitCode 1 `
    '单次 synthetic sequence 不得跨 source session。'
Assert-True ($changedSession.Text -match `
    'source_session_changed_expected_7_actual_8_at_row_242') `
    'source_session_id 变化必须在统计或 Aim stress 前 fail-closed。'

$positivePath = Join-Path $TestRoot 'positive-one-us.csv'
New-ClockSampleCsv -Path $positivePath -BiasNanoseconds 1000
$positive = Invoke-Measurement $positivePath
Assert-Equal $positive.ExitCode 0 `
    "正偏差 fixture 应成功且保持轨迹可比。`n$($positive.Text)"
$positiveMapper = Get-OutputFields $positive 'mapper'
Assert-Approx (Get-DoubleField $positiveMapper 'mapping_error_signed_mean_ms') `
    0.001 0.0003 '正 1000 ns 路径偏差必须以正 0.001 ms 报告。'
Assert-Approx (Get-DoubleField $positiveMapper 'constant_bias_ms') `
    0.001 0.0003 'constant bias 必须保留正号和毫秒单位。'
Assert-Approx (Get-DoubleField $positiveMapper 'per_sample_stress_max_abs_ms') `
    0.0 0.0003 '常量偏差不得伪装成逐样本 stress。'
Assert-ComparableAim $positive $true

$negativePath = Join-Path $TestRoot 'negative-one-us.csv'
New-ClockSampleCsv -Path $negativePath -BiasNanoseconds -1000
$negative = Invoke-Measurement $negativePath
Assert-Equal $negative.ExitCode 0 `
    "负偏差 fixture 应成功且保持轨迹可比。`n$($negative.Text)"
$negativeMapper = Get-OutputFields $negative 'mapper'
Assert-Approx (Get-DoubleField $negativeMapper 'mapping_error_signed_mean_ms') `
    -0.001 0.0003 '负 1000 ns 路径偏差必须以负 0.001 ms 报告。'
Assert-Approx (Get-DoubleField $negativeMapper 'constant_bias_ms') `
    -0.001 0.0003 'constant bias 必须保留负号和毫秒单位。'
Assert-ComparableAim $negative $true

$alternatingPath = Join-Path $TestRoot 'alternating-one-us.csv'
New-ClockSampleCsv -Path $alternatingPath -BiasNanoseconds 1000 -Alternating
$alternating = Invoke-Measurement $alternatingPath
Assert-Equal $alternating.ExitCode 0 `
    "交替偏差 fixture 应成功且保持轨迹可比。`n$($alternating.Text)"
$alternatingMapper = Get-OutputFields $alternating 'mapper'
Assert-True ((Get-DoubleField $alternatingMapper `
    'per_sample_stress_max_abs_ms') -gt 0.0003) `
    '交替偏差必须与 mapper 自身的 constant bias 分开，作为逐样本 stress 报告。'
Assert-ComparableAim $alternating $true
$alternatingAim = Get-OutputFields $alternating 'aim' 'synthetic_sequence'
Assert-True ([int]$alternatingAim['changed_aim_point_frames'] -gt 0) `
    '逐样本 stress 必须触发非零 Aim point 敏感度，不能被测量器清零。'

$divergencePath = Join-Path $TestRoot 'alternating-command-divergence.csv'
New-ClockSampleCsv -Path $divergencePath -BiasNanoseconds 199000 -Alternating
$divergence = Invoke-Measurement $divergencePath
Assert-Equal $divergence.ExitCode 3 `
    "首次命令分歧后无法保持同轨迹归因，必须稳定退出 3。`n$($divergence.Text)"
$divergenceAim = Get-OutputFields $divergence 'aim' 'synthetic_sequence'
Assert-Equal $divergenceAim['status'] 'indeterminate' `
    '命令分歧后的测量结论必须是不可判定。'
Assert-Equal $divergenceAim['stop_reason'] 'command_value_diverged' `
    '必须在首次命令值分歧处停止。'
Assert-Equal $divergenceAim['completion_history_owner'] `
    'per_replay_actual_command' '两路历史必须各自记录实际命令。'
Assert-Equal $divergenceAim['trajectory_comparable'] 'false' `
    '命令分歧后的轨迹不得继续标记为可比。'
Assert-Equal ([int]$divergenceAim['shared_zero_completion_frames']) 0 `
    '命令分歧后不得注入伪造零完成。'

Write-Host 'clock_quality_measurement_tests: PASS'
