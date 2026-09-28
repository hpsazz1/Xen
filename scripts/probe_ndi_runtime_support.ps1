# Prepare 消费本次构建部署报告；不能把 DLL 恰好缺席误称为无 NDI 能力。
function Get-XenProbeRuntimeSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Assert-XenProbeNdiRuntime([string]$ToolRoot) {
    $root = (Resolve-Path -LiteralPath $ToolRoot -ErrorAction Stop).ProviderPath
    $reportPath = Join-Path $root "xen-runtime-deployment.json"
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "INVALID_NDI_DEPLOYMENT: 缺少正式运行库部署报告"
    }
    $report = Get-Content -LiteralPath $reportPath -Raw -Encoding utf8 | ConvertFrom-Json
    # 正式发布器保留原 build 的 output_directory；搬迁副本以声明和落地 SHA 核对。
    if ([int]$report.schema -ne 1 -or [string]$report.configuration -ne "Release" -or
        -not [IO.Path]::IsPathRooted([string]$report.output_directory) -or $null -eq $report.files) {
        throw "INVALID_NDI_DEPLOYMENT: 部署报告 schema、配置或来源字段无效"
    }
    $names = @("Processing.NDI.Lib.x64.dll", "Processing.NDI.Lib.Licenses.txt")
    $entries = @($report.files | Where-Object { [string]$_.name -in $names })
    if ($entries.Count -eq 0) {
        foreach ($name in $names) {
            if (Test-Path -LiteralPath (Join-Path $root $name)) {
                throw "INVALID_NDI_DEPLOYMENT: 存在未授权的 NDI 残留文件"
            }
        }
        throw "UNSUPPORTED_NDI: 此构建未授权 NDI 采集运行库，不能准备 NDI 实测任务"
    }
    foreach ($name in $names) {
        $entry = @($entries | Where-Object { [string]$_.name -eq $name })
        $path = Join-Path $root $name
        if ($entry.Count -ne 1 -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "INVALID_NDI_DEPLOYMENT: 已声明的 NDI 运行库或许可证缺失或重复"
        }
        if (((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            [string]$entry[0].sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            (Get-XenProbeRuntimeSha256 $path) -ne [string]$entry[0].sha256) {
            throw "INVALID_NDI_DEPLOYMENT: NDI 运行库或许可证身份不符"
        }
    }
}
