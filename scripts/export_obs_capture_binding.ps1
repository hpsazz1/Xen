param(
    [ValidateSet("FixedMedia", "RealGame")]
    [string]$BindingMode = "FixedMedia",
    [Parameter(Mandatory = $true)]
    [string]$SceneCollectionPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsUserConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ObsProfileConfigPath,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedNdiOutputName,
    [Parameter(Mandatory = $true)]
    [string]$SceneName,
    [Parameter(Mandatory = $true)]
    [string[]]$SourceNames,
    [Parameter(Mandatory = $true)]
    [string]$SelectedSourceName,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 16384)]
    [int]$ExpectedSourceWidth,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 16384)]
    [int]$ExpectedSourceHeight,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 16384)]
    [int]$ExpectedRoiWidth,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 16384)]
    [int]$ExpectedRoiHeight,
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

function Get-RequiredPositiveIniInteger(
        [Collections.Specialized.OrderedDictionary]$Values,
        [string]$Name,
        [string]$Description) {
    if (-not $Values.Contains($Name) -or
        [string]$Values[$Name] -notmatch '^[1-9][0-9]*$') {
        throw "$Description 的 $Name 必须是正整数"
    }
    try {
        return [int]$Values[$Name]
    } catch {
        throw "$Description 的 $Name 超出整数范围：$($Values[$Name])"
    }
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
if ($BindingMode -eq "RealGame" -and
    ($SourceNames.Count -ne 1 -or $SourceNames[0] -ne $SelectedSourceName)) {
    throw "RealGame binding 必须且只能指定一个所选源：$SelectedSourceName"
}
if ($ExpectedSourceWidth -lt $ExpectedRoiWidth -or
    $ExpectedSourceHeight -lt $ExpectedRoiHeight -or
    ($ExpectedSourceWidth - $ExpectedRoiWidth) % 2 -ne 0 -or
    ($ExpectedSourceHeight - $ExpectedRoiHeight) % 2 -ne 0) {
    throw "预期源尺寸必须覆盖 ROI，且中心 ROI 原点必须落在整数像素"
}
$expectedRoiX = [int](
    ($ExpectedSourceWidth - $ExpectedRoiWidth) / 2)
$expectedRoiY = [int](
    ($ExpectedSourceHeight - $ExpectedRoiHeight) / 2)
$obsUserConfig = Get-StableUtf8Identity `
    $ObsUserConfigPath "OBS user.ini"
$obsBasic = Get-IniSection `
    $obsUserConfig.text "Basic" "OBS user.ini"
foreach ($name in @("ProfileDir", "SceneCollectionFile")) {
    if (-not $obsBasic.Contains($name) -or
        [string]::IsNullOrWhiteSpace([string]$obsBasic[$name])) {
        throw "OBS user.ini [Basic] 缺少活动配置指针：$name"
    }
}
$activeProfileDirectory = [string]$obsBasic.ProfileDir
$activeSceneCollectionFile = [string]$obsBasic.SceneCollectionFile
if ([IO.Path]::GetFileName($activeProfileDirectory) -ne
        $activeProfileDirectory -or
    [IO.Path]::GetFileName($activeSceneCollectionFile) -ne
        $activeSceneCollectionFile) {
    throw "OBS user.ini [Basic] 活动配置指针必须是单一文件名"
}
$obsConfigRoot = Split-Path -Parent $obsUserConfig.path
$activeProfilePath = [IO.Path]::GetFullPath((Join-Path $obsConfigRoot `
    "basic\profiles\$activeProfileDirectory\basic.ini"))
$activeSceneCollectionPath = [IO.Path]::GetFullPath((Join-Path `
    $obsConfigRoot "basic\scenes\$activeSceneCollectionFile"))
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
$obsProfileConfig = Get-StableUtf8Identity `
    $ObsProfileConfigPath "OBS profile basic.ini"
if (-not [string]::Equals(
        $obsProfileConfig.path, $activeProfilePath,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw ("传入 OBS profile 不是 user.ini 指向的当前活动 profile：" +
        "expected=$activeProfilePath；actual=$($obsProfileConfig.path)")
}
$videoProfile = Get-IniSection `
    $obsProfileConfig.text "Video" "OBS profile basic.ini"
$profileBaseWidth = Get-RequiredPositiveIniInteger `
    $videoProfile "BaseCX" "OBS profile basic.ini [Video]"
$profileBaseHeight = Get-RequiredPositiveIniInteger `
    $videoProfile "BaseCY" "OBS profile basic.ini [Video]"
$profileOutputWidth = Get-RequiredPositiveIniInteger `
    $videoProfile "OutputCX" "OBS profile basic.ini [Video]"
$profileOutputHeight = Get-RequiredPositiveIniInteger `
    $videoProfile "OutputCY" "OBS profile basic.ini [Video]"
if ($profileBaseWidth -ne $ExpectedRoiWidth -or
    $profileBaseHeight -ne $ExpectedRoiHeight -or
    $profileOutputWidth -ne $ExpectedRoiWidth -or
    $profileOutputHeight -ne $ExpectedRoiHeight) {
    throw ("OBS Program profile 必须与预期 ROI 完全一致：" +
        "expected=$($ExpectedRoiWidth)x$($ExpectedRoiHeight)；" +
        "base=$($profileBaseWidth)x$($profileBaseHeight)；" +
        "output=$($profileOutputWidth)x$($profileOutputHeight)")
}
if (-not (Test-Path -LiteralPath $SceneCollectionPath -PathType Leaf)) {
    throw "OBS 场景集合不是普通文件：$SceneCollectionPath"
}
$collectionResolved = (Resolve-Path -LiteralPath $SceneCollectionPath).Path
if (-not [string]::Equals(
        $collectionResolved, $activeSceneCollectionPath,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw ("传入 OBS scene collection 不是 user.ini 指向的当前活动集合：" +
        "expected=$activeSceneCollectionPath；actual=$collectionResolved")
}
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
$collectionResolution = Get-OptionalProperty $collection "resolution"
if ($null -eq $collectionResolution -or
    [int]$collectionResolution.x -ne $ExpectedRoiWidth -or
    [int]$collectionResolution.y -ne $ExpectedRoiHeight) {
    $actualCollectionResolution = if ($null -eq $collectionResolution) {
        "missing"
    } else {
        "$($collectionResolution.x)x$($collectionResolution.y)"
    }
    throw ("OBS 场景集合 resolution 必须与预期 ROI 完全一致：" +
        "expected=$($ExpectedRoiWidth)x$($ExpectedRoiHeight)；" +
        "actual=$actualCollectionResolution")
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
    if ($BindingMode -eq "FixedMedia") {
        if ($source.id -ne "ffmpeg_source" -or
            [string]::IsNullOrWhiteSpace(
                [string]$source.settings.local_file)) {
            throw "候选源必须是带 local_file 的 ffmpeg_source：$sourceName"
        }
    } elseif ($source.id -ne "monitor_capture") {
        throw "RealGame 所选源必须是 monitor_capture：$sourceName"
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
$visibleSceneItems = @($items | Where-Object { [bool]$_.visible })
$selectedIsOnlyVisibleSceneItem = $visibleSceneItems.Count -eq 1 -and
    [int]$visibleSceneItems[0].id -eq [int]$selectedItem.id -and
    [string]$visibleSceneItems[0].source_uuid -eq
        [string]$selectedSource.uuid
if (-not $selectedIsOnlyVisibleSceneItem) {
    $visibleItemNames = @($visibleSceneItems | ForEach-Object {
        "{0}(id={1})" -f ([string]$_.name), ([int]$_.id)
    }) -join ","
    throw ("OBS 保存的 Program Scene 必须只有所选源可见：" +
        "selected=$SelectedSourceName；visible=$visibleItemNames")
}
$selectedItemSnapshot = $selectedItem | Select-Object `
    name, source_uuid, visible, locked, rot, scale_ref, align, bounds_type, `
    bounds_align, bounds_crop, crop_left, crop_top, crop_right, crop_bottom, `
    id, pos, pos_rel, scale, scale_rel, bounds, bounds_rel, scale_filter, `
    blend_method, blend_type
$commonItemGeometryInvalid = [double]$selectedItem.rot -ne 0.0 -or
    [int]$selectedItem.align -ne 5 -or
    [int]$selectedItem.bounds_type -ne 0 -or
    [bool]$selectedItem.bounds_crop -or
    [int]$selectedItem.crop_left -ne 0 -or
    [int]$selectedItem.crop_top -ne 0 -or
    [int]$selectedItem.crop_right -ne 0 -or
    [int]$selectedItem.crop_bottom -ne 0 -or
    [double]$selectedItem.scale.x -ne 1.0 -or
    [double]$selectedItem.scale.y -ne 1.0
$selectedSourceSnapshot = $null
if ($BindingMode -eq "FixedMedia") {
    if ($commonItemGeometryInvalid -or
        [double]$selectedItem.pos.x -ne -$expectedRoiX -or
        [double]$selectedItem.pos.y -ne -$expectedRoiY) {
        throw ("所选视频必须以 1:1、无旋转/二次裁剪方式对齐 Program 中心 ROI：" +
            "expected_pos=(-$expectedRoiX,-$expectedRoiY)；" +
            "actual_pos=($($selectedItem.pos.x),$($selectedItem.pos.y))")
    }
    $mediaIdentity = Get-StableFileIdentity `
        ([string]$selectedSource.settings.local_file) "所选 OBS 媒体文件"
    $looping = Get-OptionalProperty $selectedSource.settings "looping"
    $restartOnActivate = Get-OptionalProperty `
        $selectedSource.settings "restart_on_activate"
    if ($looping -ne $true -or $restartOnActivate -ne $true) {
        throw "固定回放源必须启用 looping 和 restart_on_activate：$SelectedSourceName"
    }
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
} else {
    if ($commonItemGeometryInvalid -or
        [double]$selectedItem.pos.x -ne 0.0 -or
        [double]$selectedItem.pos.y -ne 0.0 -or
        [double]$selectedItem.scale_ref.x -ne $ExpectedSourceWidth -or
        [double]$selectedItem.scale_ref.y -ne $ExpectedSourceHeight) {
        throw ("RealGame scene item 必须以 1:1、无旋转/二次裁剪方式放置在 Program 原点：" +
            "actual_pos=($($selectedItem.pos.x),$($selectedItem.pos.y))")
    }
    $sourceSettings = $selectedSource.settings
    $monitorFieldsAvailable = $null -ne $sourceSettings -and
        $sourceSettings.PSObject.Properties.Name -contains "monitor_id" -and
        $sourceSettings.PSObject.Properties.Name -contains "capture_cursor"
    if (-not $monitorFieldsAvailable -or
        [string]::IsNullOrWhiteSpace([string]$sourceSettings.monitor_id) -or
        [bool]$sourceSettings.capture_cursor) {
        throw ("RealGame monitor_capture 必须绑定非空 monitor_id 且 " +
            "capture_cursor=false：$SelectedSourceName")
    }
    $sourceFilters = @(Get-OptionalProperty $selectedSource "filters")
    if ($sourceFilters.Count -ne 1 -or
        [string]$sourceFilters[0].id -ne "crop_filter" -or
        -not ($sourceFilters[0].PSObject.Properties.Name -contains "enabled") -or
        -not [bool]$sourceFilters[0].enabled) {
        throw ("RealGame monitor_capture 必须且只能包含一个启用的 " +
            "crop_filter：$SelectedSourceName")
    }
    $cropFilter = $sourceFilters[0]
    $cropSettings = $cropFilter.settings
    $requiredCropSettings = @("left", "top", "cx", "cy", "relative")
    $missingCropSettings = @($requiredCropSettings | Where-Object {
        $null -eq $cropSettings -or
            $cropSettings.PSObject.Properties.Name -notcontains $_
    })
    $cropGeometryInvalid = $missingCropSettings.Count -ne 0 -or
        [int]$cropSettings.left -ne $expectedRoiX -or
        [int]$cropSettings.top -ne $expectedRoiY -or
        [int]$cropSettings.cx -ne $ExpectedRoiWidth -or
        [int]$cropSettings.cy -ne $ExpectedRoiHeight -or
        [bool]$cropSettings.relative
    if ($cropGeometryInvalid) {
        throw ("RealGame crop_filter 必须是绝对 1:1 中心 ROI：" +
            "expected=($expectedRoiX,$expectedRoiY," +
            "$ExpectedRoiWidth,$ExpectedRoiHeight)")
    }
    $selectedSourceSnapshot = [ordered]@{
        name = [string]$selectedSource.name
        uuid = [string]$selectedSource.uuid
        id = [string]$selectedSource.id
        monitor_id = [string]$sourceSettings.monitor_id
        capture_cursor = [bool]$sourceSettings.capture_cursor
        crop_filter = $cropFilter | Select-Object `
            name, uuid, id, enabled, settings
    }
}
$binding = [ordered]@{
    schema_version = 2
    evidence_type = "obs_source_binding"
    binding_mode = if ($BindingMode -eq "RealGame") {
        "real_game"
    } else { "fixed_media" }
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
        active_profile_directory = $activeProfileDirectory
        active_scene_collection_file = $activeSceneCollectionFile
    }
    obs_profile_config = [ordered]@{
        path = $obsProfileConfig.path
        size = $obsProfileConfig.size
        sha256 = $obsProfileConfig.sha256
        last_write_utc = $obsProfileConfig.last_write_utc
    }
    ndi_main_output = [ordered]@{
        enabled = $true
        name = [string]$ndiPlugin.MainOutputName
    }
    program_geometry = [ordered]@{
        mapping = if ($BindingMode -eq "RealGame") {
            "monitor_crop_filter_1_to_1"
        } else { "center_crop_1_to_1" }
        source_width = $ExpectedSourceWidth
        source_height = $ExpectedSourceHeight
        roi_width = $ExpectedRoiWidth
        roi_height = $ExpectedRoiHeight
        roi_x = $expectedRoiX
        roi_y = $expectedRoiY
        profile_base_width = $profileBaseWidth
        profile_base_height = $profileBaseHeight
        profile_output_width = $profileOutputWidth
        profile_output_height = $profileOutputHeight
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
