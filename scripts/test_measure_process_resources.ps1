#requires -Version 5.1
[CmdletBinding()]
param([string]$EvidenceParent = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $EvidenceParent) { $EvidenceParent = Join-Path $PSScriptRoot '../docs/evidence/SOAK-RC-001/20260928/resource-sampler' }
$null = New-Item -ItemType Directory -Force -Path $EvidenceParent
$root = Join-Path ([IO.Path]::GetFullPath($EvidenceParent)) ('fixture-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$sampler = Join-Path $PSScriptRoot 'measure_process_resources.ps1'
$engine = Join-Path $PSHOME 'powershell.exe'
$children = New-Object 'Collections.Generic.List[Diagnostics.Process]'
$checks = New-Object 'Collections.Generic.List[string]'
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $checks.Add($Message)
}
$helperScript = Join-Path $root 'helper.ps1'
@'
param([int]$LifetimeSeconds)
$bytes = New-Object byte[] (8MB)
for ($i=0; $i -lt $bytes.Length; $i+=4096) { $bytes[$i]=1 }
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt $LifetimeSeconds) {
    for ($i=0; $i -lt 1000; $i++) { $value = [math]::Sqrt($i) }
    Start-Sleep -Milliseconds 10
}
'@ | Set-Content -LiteralPath $helperScript -Encoding utf8
function Start-Helper([int]$LifetimeSeconds) {
    $child = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$helperScript+'"'),'-LifetimeSeconds',$LifetimeSeconds) -WindowStyle Hidden -PassThru
    $children.Add($child)
    Start-Sleep -Milliseconds 300
    $child.Refresh()
    return $child
}
function Capture([Diagnostics.Process]$Child, [string]$Name, [int]$Seconds, [string]$Path, [datetimeoffset]$Started) {
    $folder = Join-Path $root $Name
    & $engine -NoProfile -ExecutionPolicy Bypass -File $sampler -TargetProcessId $Child.Id -ExpectedExecutablePath $Path -ExpectedStartTimeUtc $Started.ToString('o') -OutputDirectory $folder -DurationSeconds $Seconds -IntervalMilliseconds 100 > (Join-Path $root ($Name+'.log')) 2>&1
    return [pscustomobject]@{ ExitCode=$LASTEXITCODE; Folder=$folder; Summary=(Get-Content -LiteralPath (Join-Path $folder 'summary.json') -Raw | ConvertFrom-Json) }
}
try {
    $helper = Start-Helper 30
    $exe = $helper.MainModule.FileName
    $started = [datetimeoffset]$helper.StartTime.ToUniversalTime()
    $ok = Capture $helper 'duration' 1 $exe $started
    Assert ($ok.ExitCode -eq 0 -and $ok.Summary.status -eq 'duration_completed' -and $ok.Summary.resource_capture_completed) '达到时长才标完成'
    Assert (-not $helper.HasExited) '采样不终止被观察helper'
    Assert ($ok.Summary.cpu_observation_span_seconds -ge 1) '完成要求首末有效CPU样本覆盖指定时长'
    $rows = @(Import-Csv -LiteralPath (Join-Path $ok.Folder 'samples.csv'))
    Assert ($rows.Count -ge 2 -and $rows.Count -eq $ok.Summary.sample_count) 'CSV和summary样本数一致'
    Assert ($rows[0].cpu_percent_one_core -eq '') '首样本CPU差分为空，不伪造零'
    $lastElapsed = -1.0
    $culture = [Globalization.CultureInfo]::InvariantCulture
    foreach ($row in $rows) {
        $elapsed = [double]::Parse($row.elapsed_seconds,$culture)
        Assert ($elapsed -gt $lastElapsed) '单调elapsed严格递增'
        Assert ([long]$row.private_bytes -gt 0 -and [long]$row.working_set_bytes -gt 0 -and [int]$row.threads -gt 0) '资源计数有效'
        if ($row.cpu_percent_one_core -ne '') {
            $computed = 100.0 * [double]::Parse($row.cpu_delta_seconds,$culture) / [double]::Parse($row.interval_seconds,$culture)
            Assert ([math]::Abs($computed-[double]::Parse($row.cpu_percent_one_core,$culture)) -lt 0.000001) 'CPU使用实际单调间隔'
        }
        $lastElapsed = $elapsed
    }
    # 只向证据目录临时副本注入延迟，正式采集器不增加测试参数。
    $delayedSampler = Join-Path $root 'delayed-sampler.ps1'
    $source = [IO.File]::ReadAllText($sampler, [Text.Encoding]::UTF8)
    $source = $source.Replace('try { $targetProcess = [Diagnostics.Process]::GetProcessById($TargetProcessId) }',
        'Start-Sleep -Milliseconds 1500; $slowReadIndex = 0; try { $targetProcess = [Diagnostics.Process]::GetProcessById($TargetProcessId) }')
    $source = $source.Replace('$threads = $targetProcess.Threads.Count',
        '$slowReadIndex++; if ($slowReadIndex % 2 -eq 0) { Start-Sleep -Milliseconds 350 }; $threads = $targetProcess.Threads.Count')
    [IO.File]::WriteAllText($delayedSampler, $source, (New-Object Text.UTF8Encoding($true)))
    $originalSampler = $sampler
    try {
        $sampler = $delayedSampler
        $delayed = Capture $helper 'delayed' 1 $exe $started
    } finally { $sampler = $originalSampler }
    Assert ($delayed.ExitCode -eq 0 -and $delayed.Summary.cpu_observation_span_seconds -ge 1 -and $delayed.Summary.elapsed_seconds -ge 2.5) '初始化延迟不得充当有效采样时长'
    $delayedRows = @(Import-Csv -LiteralPath (Join-Path $delayed.Folder 'samples.csv'))
    $separated = $false
    for ($index = 1; $index -lt $delayedRows.Count; $index++) {
        $current = $delayedRows[$index]; $prior = $delayedRows[$index-1]
        $cpuInterval = [double]::Parse($current.elapsed_seconds,$culture) - [double]::Parse($prior.elapsed_seconds,$culture)
        $resourceInterval = [double]::Parse($current.resources_completed_elapsed_seconds,$culture) - [double]::Parse($prior.resources_completed_elapsed_seconds,$culture)
        Assert ([math]::Abs([double]::Parse($current.interval_seconds,$culture)-$cpuInterval) -lt 0.000001) 'CPU分母绑定CPU邻近时刻'
        if ([math]::Abs($resourceInterval-$cpuInterval) -gt 0.2) { $separated = $true }
    }
    Assert $separated '慢资源属性反例确已分离CPU与资源读取时刻'
    $wrongStart = Capture $helper 'wrong-start' 1 $exe ($started.AddSeconds(1))
    Assert ($wrongStart.ExitCode -eq 4 -and $wrongStart.Summary.sample_count -eq 0) '同PID启动时间失配拒绝，保护PID复用'
    $wrongPath = Capture $helper 'wrong-path' 1 ($exe+'.different') $started
    Assert ($wrongPath.ExitCode -eq 4 -and $wrongPath.Summary.sample_count -eq 0) '完整路径失配拒绝'
    $oldHash = (Get-FileHash -LiteralPath (Join-Path $ok.Folder 'summary.json')).Hash
    $repeat = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$sampler+'"'),'-TargetProcessId',$helper.Id,'-ExpectedExecutablePath',('"'+$exe+'"'),'-ExpectedStartTimeUtc',$started.ToString('o'),'-OutputDirectory',('"'+$ok.Folder+'"'),'-DurationSeconds',1) -WindowStyle Hidden -Wait -PassThru -RedirectStandardError (Join-Path $root 'overwrite-error.log')
    Assert ($repeat.ExitCode -ne 0 -and (Get-FileHash -LiteralPath (Join-Path $ok.Folder 'summary.json')).Hash -eq $oldHash) '重复输出目录拒绝且旧证据不变'
    $repeat.Dispose()
    $short = Start-Helper 3
    $shortStart = [datetimeoffset]$short.StartTime.ToUniversalTime()
    $early = Capture $short 'exited' 10 $short.MainModule.FileName $shortStart
    Assert ($early.ExitCode -eq 2 -and $early.Summary.status -eq 'process_exited' -and -not $early.Summary.resource_capture_completed) '目标提前退出不标完成'
    $missing = Capture $short 'missing' 1 $exe $shortStart
    Assert ($missing.ExitCode -eq 3 -and $missing.Summary.status -eq 'process_not_found') '不存在进程明确未运行'
    $failureFolder = Join-Path $root 'publish-failure'
    $failedCollector = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$sampler+'"'),'-TargetProcessId',$helper.Id,'-ExpectedExecutablePath',('"'+$exe+'"'),'-ExpectedStartTimeUtc',$started.ToString('o'),'-OutputDirectory',('"'+$failureFolder+'"'),'-DurationSeconds',2,'-IntervalMilliseconds',100) -WindowStyle Hidden -PassThru
    $children.Add($failedCollector)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath (Join-Path $failureFolder 'samples.partial.csv')) -and $wait.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 50 }
    Assert (Test-Path -LiteralPath (Join-Path $failureFolder 'samples.partial.csv')) '发布失败前实际采样已开始'
    # 仅在本fixture目录注入发布冲突，验证输出失败不能标成完成。
    $null = New-Item -ItemType Directory -Path (Join-Path $failureFolder 'samples.csv')
    Assert ($failedCollector.WaitForExit(6000)) '发布失败测试有界结束'
    $failedSummary = Get-Content -LiteralPath (Join-Path $failureFolder 'summary.json') -Raw | ConvertFrom-Json
    Assert ($failedCollector.ExitCode -eq 5 -and $failedSummary.status -eq 'collection_failed' -and -not $failedSummary.resource_capture_completed) '写出失败明确区别于时长完成'
    $cutFolder = Join-Path $root 'interrupted'
    $cut = Start-Process -FilePath $engine -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$sampler+'"'),'-TargetProcessId',$helper.Id,'-ExpectedExecutablePath',('"'+$exe+'"'),'-ExpectedStartTimeUtc',$started.ToString('o'),'-OutputDirectory',('"'+$cutFolder+'"'),'-DurationSeconds',20,'-IntervalMilliseconds',100) -WindowStyle Hidden -PassThru
    $children.Add($cut)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath (Join-Path $cutFolder 'samples.partial.csv')) -and $wait.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 50 }
    Assert (Test-Path -LiteralPath (Join-Path $cutFolder 'samples.partial.csv')) '中断前采集器确已运行'
    # 仅终止本fixture创建的采集器，验证硬中断不会标成完成。
    $cut.Kill(); $cut.WaitForExit()
    $partial = Get-Content -LiteralPath (Join-Path $cutFolder 'summary.partial.json') -Raw | ConvertFrom-Json
    Assert (-not $partial.resource_capture_completed -and $partial.status -eq 'interrupted' -and -not (Test-Path -LiteralPath (Join-Path $cutFolder 'summary.json'))) '硬中断partial不能伪装最终完成'
    Assert (-not $helper.HasExited) '中断采集器不影响目标helper'
    [pscustomobject]@{status='passed'; checks=$checks.Count; powershell=$PSVersionTable.PSVersion.ToString(); evidence=$root} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'test-summary.json') -Encoding utf8
    Write-Output ('资源采集fixture通过：{0}项，证据 {1}' -f $checks.Count,$root)
}
finally {
    foreach ($child in $children) {
        try { if (-not $child.HasExited) { $child.Kill(); $child.WaitForExit() } } finally { $child.Dispose() }
    }
}
