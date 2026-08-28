param(
    [Parameter(Mandatory = $true)]
    [string]$SceneCollectionPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsUserConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedNdiOutputName,
    [Parameter(Mandatory = $true)]
    [string]$SceneName,
    [Parameter(Mandatory = $true)]
    [string[]]$SourceNames,
    [Parameter(Mandatory = $true)]
    [string]$SelectedSourceName,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop

function Get-BytesSha256([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($Bytes)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-FileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-StableFileIdentity([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不是普通文件：$Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $before = Get-Item -LiteralPath $resolved
    $hash = Get-FileSha256 $resolved
    $after = Get-Item -LiteralPath $resolved
    if ($before.Length -ne $after.Length -or
        $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) {
        throw "$Description 在哈希期间发生变化：$resolved"
    }
    return [ordered]@{
        path = $resolved
        size = [UInt64]$after.Length
        sha256 = $hash
        last_write_utc = $after.LastWriteTimeUtc.ToString("o")
    }
}

function Get-StableUtf8Identity([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description 不是普通文件：$Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $before = Get-Item -LiteralPath $resolved
    $bytes = [IO.File]::ReadAllBytes($resolved)
    $after = Get-Item -LiteralPath $resolved
    if ($before.Length -ne $after.Length -or
        $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) {
        throw "$Description 在读取期间发生变化：$resolved"
    }
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    try {
        $text = $utf8.GetString($bytes)
    } catch {
        throw "$Description 不是有效 UTF-8：$($_.Exception.Message)"
    }
    if ($text.Length -gt 0 -and [int]$text[0] -eq 0xFEFF) {
        $text = $text.Substring(1)
    }
    return [ordered]@{
        path = $resolved
        size = [UInt64]$after.Length
        sha256 = Get-BytesSha256 $bytes
        last_write_utc = $after.LastWriteTimeUtc.ToString("o")
        text = $text
    }
}

function Get-IniSection(
        [string]$Text,
        [string]$SectionName,
        [string]$Description) {
    $values = [ordered]@{}
    $currentSection = ""
    $found = $false
    foreach ($rawLine in ($Text -split "`r?`n")) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(";") -or
            $line.StartsWith("#")) {
            continue
        }
        if ($line -match '^\[(.+)\]$') {
            $currentSection = $Matches[1].Trim()
            if ($currentSection -eq $SectionName) { $found = $true }
            continue
        }
        if ($currentSection -ne $SectionName) { continue }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            throw "$Description 的 [$SectionName] 存在非法行：$line"
        }
        $key = $line.Substring(0, $separator).Trim()
        $value = $line.Substring($separator + 1).Trim()
        if ($values.Contains($key)) {
            throw "$Description 的 [$SectionName] 存在重复键：$key"
        }
        $values[$key] = $value
    }
    if (-not $found) {
        throw "$Description 缺少 [$SectionName]"
    }
    return $values
}

function Get-UniqueMatch(
        [object[]]$Values,
        [scriptblock]$Predicate,
        [string]$Description) {
    $matches = @($Values | Where-Object $Predicate)
    if ($matches.Count -ne 1) {
        throw "$Description 必须且只能匹配一项，实际为 $($matches.Count) 项"
    }
    return $matches[0]
}

function Get-OptionalProperty([object]$Value, [string]$Name) {
    $property = $Value.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

if ($SourceNames.Count -eq 0 -or [string]::IsNullOrWhiteSpace($SceneName) -or
    [string]::IsNullOrWhiteSpace($SelectedSourceName) -or
    [string]::IsNullOrWhiteSpace($ExpectedNdiOutputName)) {
    throw "场景名、候选源、所选源和预期 NDI 输出名不能为空"
}
$sourceSet = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal)
foreach ($sourceName in $SourceNames) {
    if ([string]::IsNullOrWhiteSpace($sourceName) -or
        -not $sourceSet.Add($sourceName)) {
        throw "候选源名称不能为空或重复：$sourceName"
    }
}
if (-not $sourceSet.Contains($SelectedSourceName)) {
    throw "所选源不在候选源集合中：$SelectedSourceName"
}
$obsUserConfig = Get-StableUtf8Identity `
    $ObsUserConfigPath "OBS user.ini"
$ndiPlugin = Get-IniSection `
    $obsUserConfig.text "NDIPlugin" "OBS user.ini"
if (-not $ndiPlugin.Contains("MainOutputEnabled") -or
    $ndiPlugin.MainOutputEnabled -ne "true") {
    throw "OBS NDI MainOutputEnabled 必须为 true"
}
if (-not $ndiPlugin.Contains("MainOutputName") -or
    $ndiPlugin.MainOutputName -ne $ExpectedNdiOutputName) {
    throw "OBS NDI 主输出名不匹配：expected=$ExpectedNdiOutputName；actual=$($ndiPlugin.MainOutputName)"
}
if (-not (Test-Path -LiteralPath $SceneCollectionPath -PathType Leaf)) {
    throw "OBS 场景集合不是普通文件：$SceneCollectionPath"
}
$collectionResolved = (Resolve-Path -LiteralPath $SceneCollectionPath).Path
$collectionBytes = [IO.File]::ReadAllBytes($collectionResolved)
$collectionHash = Get-BytesSha256 $collectionBytes
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$collectionText = $utf8.GetString($collectionBytes)
if ($collectionText.Length -gt 0 -and
    [int]$collectionText[0] -eq 0xFEFF) {
    $collectionText = $collectionText.Substring(1)
}
try {
    $collection = $collectionText | ConvertFrom-Json
} catch {
    throw "OBS 场景集合不是有效 UTF-8 JSON：$($_.Exception.Message)"
}
if ([string]$collection.current_program_scene -ne $SceneName) {
    throw "指定场景不是 OBS 保存的 Program Scene：expected=$SceneName；actual=$($collection.current_program_scene)"
}
$scene = Get-UniqueMatch @($collection.sources) {
    $_.id -eq "scene" -and $_.name -eq $SceneName
} "OBS Program Scene"
$items = @($scene.settings.items)

$candidateVisibility = @()
$selectedSource = $null
$selectedItem = $null
foreach ($sourceName in $SourceNames) {
    $source = Get-UniqueMatch @($collection.sources) {
        $_.name -eq $sourceName
    } "OBS source '$sourceName'"
    if ($source.id -ne "ffmpeg_source" -or
        [string]::IsNullOrWhiteSpace([string]$source.settings.local_file)) {
        throw "候选源必须是带 local_file 的 ffmpeg_source：$sourceName"
    }
    $item = Get-UniqueMatch $items {
        $_.name -eq $sourceName -and $_.source_uuid -eq $source.uuid
    } "OBS scene item '$sourceName'"
    $candidateVisibility += [ordered]@{
        name = $sourceName
        source_uuid = [string]$source.uuid
        scene_item_id = [int]$item.id
        visible = [bool]$item.visible
    }
    if ($sourceName -eq $SelectedSourceName) {
        $selectedSource = $source
        $selectedItem = $item
    }
}
$visibleCandidates = @($candidateVisibility | Where-Object { $_.visible })
if ($visibleCandidates.Count -ne 1 -or
    $visibleCandidates[0].name -ne $SelectedSourceName -or
    -not [bool]$selectedItem.visible) {
    $visibleNames = @($visibleCandidates | ForEach-Object { $_.name }) -join ","
    throw "候选源必须只有所选源可见：selected=$SelectedSourceName；visible=$visibleNames"
}

$mediaIdentity = Get-StableFileIdentity `
    ([string]$selectedSource.settings.local_file) "所选 OBS 媒体文件"
$looping = Get-OptionalProperty $selectedSource.settings "looping"
$restartOnActivate = Get-OptionalProperty `
    $selectedSource.settings "restart_on_activate"
if ($looping -ne $true -or $restartOnActivate -ne $true) {
    throw "固定回放源必须启用 looping 和 restart_on_activate：$SelectedSourceName"
}
$selectedItemSnapshot = $selectedItem | Select-Object `
    name, source_uuid, visible, locked, rot, scale_ref, align, bounds_type, `
    bounds_align, bounds_crop, crop_left, crop_top, crop_right, crop_bottom, `
    id, pos, pos_rel, scale, scale_rel, bounds, bounds_rel, scale_filter, `
    blend_method, blend_type
$selectedSourceSnapshot = [ordered]@{
    name = [string]$selectedSource.name
    uuid = [string]$selectedSource.uuid
    id = [string]$selectedSource.id
    media_file = $mediaIdentity.path
    media_file_size = $mediaIdentity.size
    media_file_sha256 = $mediaIdentity.sha256
    media_file_last_write_utc = $mediaIdentity.last_write_utc
    looping = $looping
    restart_on_activate = $restartOnActivate
    close_when_inactive = Get-OptionalProperty `
        $selectedSource.settings "close_when_inactive"
    hw_decode = Get-OptionalProperty $selectedSource.settings "hw_decode"
}
$binding = [ordered]@{
    schema_version = 2
    evidence_type = "obs_source_binding"
    physical_output_capability = $false
    state_basis = "obs_saved_scene_collection"
    scene_collection = $collectionResolved
    scene_collection_sha256 = $collectionHash
    scene_collection_size = [UInt64]$collectionBytes.Length
    obs_user_config = [ordered]@{
        path = $obsUserConfig.path
        size = $obsUserConfig.size
        sha256 = $obsUserConfig.sha256
        last_write_utc = $obsUserConfig.last_write_utc
    }
    ndi_main_output = [ordered]@{
        enabled = $true
        name = [string]$ndiPlugin.MainOutputName
    }
    collection_name = [string]$collection.name
    current_scene = [string]$collection.current_scene
    current_program_scene = [string]$collection.current_program_scene
    scene_name = $SceneName
    selected_source = $selectedSourceSnapshot
    selected_scene_item = $selectedItemSnapshot
    candidate_visibility = $candidateVisibility
}

$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFullPath) {
    throw "OBS binding 输出已存在，拒绝覆盖：$outputFullPath"
}
$outputParent = Split-Path -Parent $outputFullPath
$outputName = Split-Path -Leaf $outputFullPath
if (-not $outputParent -or -not $outputName) {
    throw "OBS binding 输出路径非法：$OutputPath"
}
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
$pending = Join-Path $outputParent (
    ".{0}.pending-{1}" -f $outputName, [guid]::NewGuid().ToString("N"))
try {
    $json = $binding | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText(
        $pending, $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $pending -Destination $outputFullPath
    Write-Host "OBS source binding 已原子发布：$outputFullPath"
} catch {
    if (Test-Path -LiteralPath $pending) {
        Remove-Item -LiteralPath $pending -Force
    }
    throw
}
