param(
    [Parameter(Mandatory = $true)]
    [string]$BindingScript,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot "..\scripts\path_safety.psm1") -Force

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function Write-ObsUserConfig(
        [string]$Path,
        [string]$ProfileDirectory,
        [string]$SceneCollectionFile,
        [string]$NdiOutputName = "Xen-ROI-320") {
    Write-Utf8NoBom $Path @"
[Basic]
Profile=$ProfileDirectory
ProfileDir=$ProfileDirectory
SceneCollection=$SceneCollectionFile
SceneCollectionFile=$SceneCollectionFile

[NDIPlugin]
MainOutputEnabled=true
MainOutputName=$NdiOutputName
PreviewOutputEnabled=false
"@
}

function Get-TestFileSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).
            Replace("-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Write-SceneCollection(
        [string]$Path,
        [string]$StaticPath,
        [string]$JumpPath,
        [string]$MovePath,
        [bool]$StaticVisible,
        [bool]$JumpVisible) {
    $document = [ordered]@{
        name = "固定证据集合"
        current_scene = "固定测试输出"
        current_program_scene = "固定测试输出"
        resolution = [ordered]@{ x = 320; y = 320 }
        sources = @(
            [ordered]@{
                name = "静止"
                uuid = "source-static"
                id = "ffmpeg_source"
                settings = [ordered]@{
                    local_file = $StaticPath
                    looping = $true
                    restart_on_activate = $true
                }
            },
            [ordered]@{
                name = "原地跳跃"
                uuid = "source-jump"
                id = "ffmpeg_source"
                settings = [ordered]@{
                    local_file = $JumpPath
                    looping = $true
                    restart_on_activate = $true
                }
            },
            [ordered]@{
                name = "左右横移"
                uuid = "source-move"
                id = "ffmpeg_source"
                settings = [ordered]@{
                    local_file = $MovePath
                    looping = $true
                    restart_on_activate = $true
                }
            },
            [ordered]@{
                name = "固定测试输出"
                uuid = "scene-output"
                id = "scene"
                settings = [ordered]@{
                    items = @(
                        [ordered]@{
                            name = "静止"
                            source_uuid = "source-static"
                            visible = $StaticVisible
                            locked = $true
                            rot = 0.0
                            scale_ref = [ordered]@{ x = 320.0; y = 320.0 }
                            align = 5
                            bounds_type = 0
                            bounds_align = 0
                            bounds_crop = $false
                            crop_left = 0
                            crop_top = 0
                            crop_right = 0
                            crop_bottom = 0
                            id = 1
                            pos = [ordered]@{ x = -1120.0; y = -560.0 }
                            scale = [ordered]@{ x = 1.0; y = 1.0 }
                            scale_filter = "disable"
                            blend_method = "default"
                            blend_type = "normal"
                        },
                        [ordered]@{
                            name = "原地跳跃"
                            source_uuid = "source-jump"
                            visible = $JumpVisible
                            locked = $true
                            rot = 0.0
                            scale_ref = [ordered]@{ x = 320.0; y = 320.0 }
                            align = 5
                            bounds_type = 0
                            bounds_align = 0
                            bounds_crop = $false
                            crop_left = 0
                            crop_top = 0
                            crop_right = 0
                            crop_bottom = 0
                            id = 2
                            pos = [ordered]@{ x = -1120.0; y = -560.0 }
                            scale = [ordered]@{ x = 1.0; y = 1.0 }
                            scale_filter = "disable"
                            blend_method = "default"
                            blend_type = "normal"
                        },
                        [ordered]@{
                            name = "左右横移"
                            source_uuid = "source-move"
                            visible = $false
                            locked = $true
                            rot = 0.0
                            scale_ref = [ordered]@{ x = 320.0; y = 320.0 }
                            align = 5
                            bounds_type = 0
                            bounds_align = 0
                            bounds_crop = $false
                            crop_left = 0
                            crop_top = 0
                            crop_right = 0
                            crop_bottom = 0
                            id = 3
                            pos = [ordered]@{ x = -1120.0; y = -560.0 }
                            scale = [ordered]@{ x = 1.0; y = 1.0 }
                            scale_filter = "disable"
                            blend_method = "default"
                            blend_type = "normal"
                        }
                    )
                }
            }
        )
    }
    Write-Utf8NoBom $Path ($document | ConvertTo-Json -Depth 12)
}

function Write-RealGameSceneCollection([string]$Path) {
    $document = [ordered]@{
        name = "实际游戏集合"
        current_scene = "主画面裁剪输出"
        current_program_scene = "主画面裁剪输出"
        resolution = [ordered]@{ x = 320; y = 320 }
        sources = @(
            [ordered]@{
                name = "主画面"
                uuid = "source-main"
                id = "monitor_capture"
                settings = [ordered]@{
                    monitor_id = "fixture-monitor-2560x1440"
                    capture_cursor = $false
                }
                filters = @(
                    [ordered]@{
                        name = "裁剪/填充"
                        uuid = "filter-crop"
                        id = "crop_filter"
                        enabled = $true
                        settings = [ordered]@{
                            left = 1120
                            top = 560
                            cx = 320
                            cy = 320
                            relative = $false
                        }
                    }
                )
            },
            [ordered]@{
                name = "主画面裁剪输出"
                uuid = "scene-main"
                id = "scene"
                settings = [ordered]@{
                    items = @(
                        [ordered]@{
                            name = "主画面"
                            source_uuid = "source-main"
                            visible = $true
                            locked = $true
                            rot = 0.0
                            scale_ref = [ordered]@{ x = 2560.0; y = 1440.0 }
                            align = 5
                            bounds_type = 0
                            bounds_align = 0
                            bounds_crop = $false
                            crop_left = 0
                            crop_top = 0
                            crop_right = 0
                            crop_bottom = 0
                            id = 4
                            pos = [ordered]@{ x = 0.0; y = 0.0 }
                            scale = [ordered]@{ x = 1.0; y = 1.0 }
                            scale_filter = "disable"
                            blend_method = "default"
                            blend_type = "normal"
                        }
                    )
                }
            }
        )
    }
    Write-Utf8NoBom $Path ($document | ConvertTo-Json -Depth 12)
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $repositoryRoot
$root = $ownedTest.RootPath

try {
    $static = Join-Path $root "静止.mp4"
    $jump = Join-Path $root "原地跳跃.mp4"
    $move = Join-Path $root "左右横移.mp4"
    Write-Utf8NoBom $static "static-video"
    Write-Utf8NoBom $jump "jump-video"
    Write-Utf8NoBom $move "move-video"
    $obsConfigRoot = Join-Path $root "obs-studio"
    $collection = Join-Path $obsConfigRoot "basic\scenes\未命名.json"
    Write-SceneCollection $collection $static $jump $move $true $false
    $obsUserConfig = Join-Path $obsConfigRoot "user.ini"
    Write-ObsUserConfig $obsUserConfig "未命名" "未命名.json"
    $obsProfileConfig = Join-Path $obsConfigRoot `
        "basic\profiles\未命名\basic.ini"
    Write-Utf8NoBom $obsProfileConfig @"
[Video]
BaseCX=320
BaseCY=320
OutputCX=320
OutputCY=320
"@

    $output = Join-Path $root "binding.json"
    & $BindingScript `
        -SceneCollectionPath $collection `
        -ObsUserConfigPath $obsUserConfig `
        -ObsProfileConfigPath $obsProfileConfig `
        -ExpectedNdiOutputName "Xen-ROI-320" `
        -SceneName "固定测试输出" `
        -SourceNames @("静止", "原地跳跃", "左右横移") `
        -SelectedSourceName "静止" `
        -ExpectedSourceWidth 2560 `
        -ExpectedSourceHeight 1440 `
        -ExpectedRoiWidth 320 `
        -ExpectedRoiHeight 320 `
        -OutputPath $output
    $binding = Get-Content -LiteralPath $output -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($binding.schema_version -ne 2 -or
        $binding.evidence_type -ne "obs_source_binding" -or
        $binding.binding_mode -ne "fixed_media" -or
        $binding.physical_output_capability -ne $false -or
        $binding.scene_collection_sha256 -ne
            (Get-TestFileSha256 $collection) -or
        $binding.obs_user_config.sha256 -ne
            (Get-TestFileSha256 $obsUserConfig) -or
        $binding.obs_profile_config.sha256 -ne
            (Get-TestFileSha256 $obsProfileConfig) -or
        $binding.ndi_main_output.enabled -ne $true -or
        $binding.ndi_main_output.name -ne "Xen-ROI-320" -or
        $binding.program_geometry.source_width -ne 2560 -or
        $binding.program_geometry.source_height -ne 1440 -or
        $binding.program_geometry.roi_width -ne 320 -or
        $binding.program_geometry.roi_height -ne 320 -or
        $binding.program_geometry.roi_x -ne 1120 -or
        $binding.program_geometry.roi_y -ne 560 -or
        $binding.selected_source.name -ne "静止" -or
        $binding.selected_source.looping -ne $true -or
        $binding.selected_source.restart_on_activate -ne $true -or
        $binding.selected_source.media_file_sha256 -ne
            (Get-TestFileSha256 $static) -or
        $binding.selected_scene_item.id -ne 1 -or
        $binding.selected_scene_item.pos.x -ne -1120.0 -or
        $binding.selected_scene_item.scale.x -ne 1.0 -or
        @($binding.candidate_visibility | Where-Object { $_.visible }).Count -ne 1) {
        throw "合法 OBS binding 的身份、安全声明或变换不正确"
    }

    # 公开 red：正式人工验收绑定的是实际游戏 monitor_capture，经源级
    # crop_filter 输出 320x320 ROI；不能继续复用固定媒体的场景位移合同。
    $realGameCollection = Join-Path $obsConfigRoot `
        "basic\scenes\real-game.json"
    Write-RealGameSceneCollection $realGameCollection
    $realGameUserConfig = Join-Path $obsConfigRoot "real-game-user.ini"
    Write-ObsUserConfig $realGameUserConfig "未命名" "real-game.json"
    $realGameOutput = Join-Path $root "real-game-binding.json"
    & $BindingScript `
        -BindingMode RealGame `
        -SceneCollectionPath $realGameCollection `
        -ObsUserConfigPath $realGameUserConfig `
        -ObsProfileConfigPath $obsProfileConfig `
        -ExpectedNdiOutputName "Xen-ROI-320" `
        -SceneName "主画面裁剪输出" `
        -SourceNames @("主画面") `
        -SelectedSourceName "主画面" `
        -ExpectedSourceWidth 2560 `
        -ExpectedSourceHeight 1440 `
        -ExpectedRoiWidth 320 `
        -ExpectedRoiHeight 320 `
        -OutputPath $realGameOutput
    $realGameBinding = Get-Content -LiteralPath $realGameOutput -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    if ($realGameBinding.binding_mode -ne "real_game" -or
        $realGameBinding.program_geometry.mapping -ne
            "monitor_crop_filter_1_to_1" -or
        $realGameBinding.selected_source.id -ne "monitor_capture" -or
        $realGameBinding.selected_source.monitor_id -ne
            "fixture-monitor-2560x1440" -or
        $realGameBinding.selected_source.capture_cursor -ne $false -or
        $realGameBinding.selected_source.crop_filter.settings.left -ne 1120 -or
        $realGameBinding.selected_source.crop_filter.settings.top -ne 560 -or
        $realGameBinding.selected_source.crop_filter.settings.cx -ne 320 -or
        $realGameBinding.selected_source.crop_filter.settings.cy -ne 320 -or
        $realGameBinding.selected_source.crop_filter.settings.relative -ne
            $false -or
        $realGameBinding.selected_scene_item.pos.x -ne 0.0 -or
        $realGameBinding.selected_scene_item.pos.y -ne 0.0) {
        throw "实际游戏 OBS binding 没有绑定 monitor、crop_filter 或 Program 几何。"
    }

    # 可变红回归：实际游戏 crop_filter 左边界只差一个像素也必须在公开
    # exporter seam 失败，且不得发布看似合法的 binding。
    $wrongRealGameCrop = Join-Path $obsConfigRoot `
        "basic\scenes\real-game-wrong-crop.json"
    $wrongRealGameCropDocument = Get-Content -LiteralPath `
        $realGameCollection -Raw -Encoding UTF8 | ConvertFrom-Json
    (($wrongRealGameCropDocument.sources |
        Where-Object { $_.name -eq "主画面" }).filters |
        Where-Object { $_.id -eq "crop_filter" }).settings.left = 1119
    Write-Utf8NoBom $wrongRealGameCrop `
        ($wrongRealGameCropDocument | ConvertTo-Json -Depth 12)
    $wrongRealGameCropUserConfig = Join-Path $obsConfigRoot `
        "real-game-wrong-crop-user.ini"
    Write-ObsUserConfig $wrongRealGameCropUserConfig "未命名" `
        "real-game-wrong-crop.json"
    $wrongRealGameCropOutput = Join-Path $root `
        "real-game-wrong-crop-binding.json"
    $wrongRealGameCropRejected = $false
    $wrongRealGameCropError = ""
    try {
        & $BindingScript `
            -BindingMode RealGame `
            -SceneCollectionPath $wrongRealGameCrop `
            -ObsUserConfigPath $wrongRealGameCropUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "主画面裁剪输出" `
            -SourceNames @("主画面") `
            -SelectedSourceName "主画面" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath $wrongRealGameCropOutput
    } catch {
        $wrongRealGameCropRejected = $true
        $wrongRealGameCropError = $_.Exception.Message
    }
    if (-not $wrongRealGameCropRejected -or
        $wrongRealGameCropError -notlike
            "RealGame crop_filter 必须是绝对 1:1 中心 ROI*" -or
        (Test-Path -LiteralPath $wrongRealGameCropOutput)) {
        throw "实际游戏 crop_filter 偏离中心 ROI 一像素时必须失败封闭。"
    }

    $existing = Get-Content -LiteralPath $output -Raw -Encoding UTF8
    $duplicateRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $obsUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath $output
    } catch {
        $duplicateRejected = $true
    }
    if (-not $duplicateRejected -or
        (Get-Content -LiteralPath $output -Raw -Encoding UTF8) -ne $existing) {
        throw "既有 binding 必须拒绝覆盖且内容不变"
    }

    $ambiguous = Join-Path $obsConfigRoot `
        "basic\scenes\ambiguous.json"
    Write-SceneCollection $ambiguous $static $jump $move $true $true
    $ambiguousUserConfig = Join-Path $obsConfigRoot "ambiguous-user.ini"
    Write-ObsUserConfig $ambiguousUserConfig "未命名" "ambiguous.json"
    $ambiguousRejected = $false
    $ambiguousError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $ambiguous `
            -ObsUserConfigPath $ambiguousUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "ambiguous-binding.json")
    } catch {
        $ambiguousRejected = $true
        $ambiguousError = $_.Exception.Message
    }
    if (-not $ambiguousRejected -or
        $ambiguousError -notlike "候选源必须只有所选源可见*") {
        throw "候选中有多个可见源时必须失败封闭"
    }

    # 公开 red：selected 候选并非 Program scene 中唯一可见的出像素项时，
    # 即使额外项不在 SourceNames 候选列表内，也不能把 saved 配置导出为
    # selected 媒体的排他性 binding。
    $nonCandidateVisible = Join-Path $obsConfigRoot `
        "basic\scenes\non-candidate-visible.json"
    $nonCandidateVisibleDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $nonCandidateVisibleDocument.sources += [pscustomobject][ordered]@{
        name = "主画面"
        uuid = "source-main"
        id = "monitor_capture"
        settings = [pscustomobject][ordered]@{ monitor = 0 }
    }
    $nonCandidateProgramScene = $nonCandidateVisibleDocument.sources |
        Where-Object { $_.name -eq "固定测试输出" }
    $nonCandidateProgramScene.settings.items += [pscustomobject][ordered]@{
        name = "主画面"
        source_uuid = "source-main"
        visible = $true
        locked = $true
        rot = 0.0
        scale_ref = [pscustomobject][ordered]@{ x = 320.0; y = 320.0 }
        align = 5
        bounds_type = 0
        bounds_align = 0
        bounds_crop = $false
        crop_left = 1120
        crop_top = 560
        crop_right = 0
        crop_bottom = 0
        id = 4
        pos = [pscustomobject][ordered]@{ x = 0.0; y = 0.0 }
        scale = [pscustomobject][ordered]@{ x = 1.0; y = 1.0 }
        scale_filter = "disable"
        blend_method = "default"
        blend_type = "normal"
    }
    Write-Utf8NoBom $nonCandidateVisible `
        ($nonCandidateVisibleDocument | ConvertTo-Json -Depth 12)
    $nonCandidateUserConfig = Join-Path $obsConfigRoot `
        "non-candidate-visible-user.ini"
    Write-ObsUserConfig $nonCandidateUserConfig "未命名" `
        "non-candidate-visible.json"
    $nonCandidateVisibleRejected = $false
    $nonCandidateVisibleError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $nonCandidateVisible `
            -ObsUserConfigPath $nonCandidateUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root `
                "non-candidate-visible-binding.json")
    } catch {
        $nonCandidateVisibleRejected = $true
        $nonCandidateVisibleError = $_.Exception.Message
    }
    if (-not $nonCandidateVisibleRejected -or
        $nonCandidateVisibleError -notlike
            "OBS 保存的 Program Scene 必须只有所选源可见*") {
        throw "所选源之外存在可见 Program scene item 时必须失败封闭"
    }

    $hiddenRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $obsUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "原地跳跃" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "hidden-binding.json")
    } catch {
        $hiddenRejected = $true
    }
    if (-not $hiddenRejected) {
        throw "所选源隐藏时必须失败封闭"
    }

    $ndiMismatchRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $obsUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "unexpected-output" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "ndi-mismatch-binding.json")
    } catch {
        $ndiMismatchRejected = $true
    }
    if (-not $ndiMismatchRejected) {
        throw "NDI 主输出名不一致时必须失败封闭"
    }

    # 公开 red：传入的 profile/scene collection 必须就是 user.ini [Basic]
    # 指向的当前活动文件，不能把活动 NDI 名与另一套同尺寸配置拼接。
    $wrongActiveProfileConfig = Join-Path $obsConfigRoot `
        "wrong-active-profile-user.ini"
    Write-ObsUserConfig $wrongActiveProfileConfig "其他" "未命名.json"
    $wrongActiveProfileRejected = $false
    $wrongActiveProfileError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $wrongActiveProfileConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root `
                "wrong-active-profile-binding.json")
    } catch {
        $wrongActiveProfileRejected = $true
        $wrongActiveProfileError = $_.Exception.Message
    }
    if (-not $wrongActiveProfileRejected -or
        $wrongActiveProfileError -notlike
            "传入 OBS profile 不是 user.ini 指向的当前活动 profile*") {
        throw "传入 profile 不是 OBS 当前活动 profile 时必须失败封闭"
    }

    $wrongActiveCollectionConfig = Join-Path $obsConfigRoot `
        "wrong-active-collection-user.ini"
    Write-ObsUserConfig $wrongActiveCollectionConfig "未命名" "其他.json"
    $wrongActiveCollectionRejected = $false
    $wrongActiveCollectionError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $wrongActiveCollectionConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root `
                "wrong-active-collection-binding.json")
    } catch {
        $wrongActiveCollectionRejected = $true
        $wrongActiveCollectionError = $_.Exception.Message
    }
    if (-not $wrongActiveCollectionRejected -or
        $wrongActiveCollectionError -notlike
            "传入 OBS scene collection 不是 user.ini 指向的当前活动集合*") {
        throw "传入 scene collection 不是 OBS 当前活动集合时必须失败封闭"
    }

    $notLooping = Join-Path $obsConfigRoot `
        "basic\scenes\not-looping.json"
    $notLoopingDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    ($notLoopingDocument.sources |
        Where-Object { $_.name -eq "静止" }).settings.looping = $false
    Write-Utf8NoBom $notLooping `
        ($notLoopingDocument | ConvertTo-Json -Depth 12)
    $notLoopingUserConfig = Join-Path $obsConfigRoot "not-looping-user.ini"
    Write-ObsUserConfig $notLoopingUserConfig "未命名" "not-looping.json"
    $notLoopingRejected = $false
    $notLoopingError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $notLooping `
            -ObsUserConfigPath $notLoopingUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "not-looping-binding.json")
    } catch {
        $notLoopingRejected = $true
        $notLoopingError = $_.Exception.Message
    }
    if (-not $notLoopingRejected -or
        $notLoopingError -notlike "固定回放源必须启用 looping*") {
        throw "固定素材未循环时必须失败封闭"
    }

    # 公开 red：NDI Main Output 的 profile 不是 320x320 时，不能把
    # 2560x1440 媒体的中心 ROI 声明为已绑定。
    $wrongProfile = Join-Path $obsConfigRoot `
        "basic\profiles\wrong-profile\basic.ini"
    Write-Utf8NoBom $wrongProfile @"
[Video]
BaseCX=320
BaseCY=320
OutputCX=640
OutputCY=360
"@
    $wrongProfileUserConfig = Join-Path $obsConfigRoot `
        "wrong-profile-user.ini"
    Write-ObsUserConfig $wrongProfileUserConfig "wrong-profile" `
        "未命名.json"
    $wrongProfileRejected = $false
    $wrongProfileError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $wrongProfileUserConfig `
            -ObsProfileConfigPath $wrongProfile `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "wrong-profile-binding.json")
    } catch {
        $wrongProfileRejected = $true
        $wrongProfileError = $_.Exception.Message
    }
    if (-not $wrongProfileRejected -or
        $wrongProfileError -notlike
            "OBS Program profile 必须与预期 ROI 完全一致*") {
        throw "OBS Program profile 不是预期 ROI 尺寸时必须失败封闭"
    }

    $wrongResolution = Join-Path $obsConfigRoot `
        "basic\scenes\wrong-resolution.json"
    $wrongResolutionDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $wrongResolutionDocument.resolution.x = 640
    Write-Utf8NoBom $wrongResolution `
        ($wrongResolutionDocument | ConvertTo-Json -Depth 12)
    $wrongResolutionUserConfig = Join-Path $obsConfigRoot `
        "wrong-resolution-user.ini"
    Write-ObsUserConfig $wrongResolutionUserConfig "未命名" `
        "wrong-resolution.json"
    $wrongResolutionRejected = $false
    $wrongResolutionError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $wrongResolution `
            -ObsUserConfigPath $wrongResolutionUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "wrong-resolution-binding.json")
    } catch {
        $wrongResolutionRejected = $true
        $wrongResolutionError = $_.Exception.Message
    }
    if (-not $wrongResolutionRejected -or
        $wrongResolutionError -notlike
            "OBS 场景集合 resolution 必须与预期 ROI 完全一致*") {
        throw "OBS 场景集合不是预期 ROI 尺寸时必须失败封闭"
    }

    # 公开 red：所选视频虽是唯一可见项，但偏离中心 ROI 一像素时也必须拒绝。
    $wrongTransform = Join-Path $obsConfigRoot `
        "basic\scenes\wrong-transform.json"
    $wrongTransformDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $wrongTransformScene = $wrongTransformDocument.sources |
        Where-Object { $_.name -eq "固定测试输出" }
    ($wrongTransformScene.settings.items |
        Where-Object { $_.name -eq "静止" }).pos.x = -1119.0
    Write-Utf8NoBom $wrongTransform `
        ($wrongTransformDocument | ConvertTo-Json -Depth 12)
    $wrongTransformUserConfig = Join-Path $obsConfigRoot `
        "wrong-transform-user.ini"
    Write-ObsUserConfig $wrongTransformUserConfig "未命名" `
        "wrong-transform.json"
    $wrongTransformRejected = $false
    $wrongTransformError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $wrongTransform `
            -ObsUserConfigPath $wrongTransformUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "wrong-transform-binding.json")
    } catch {
        $wrongTransformRejected = $true
        $wrongTransformError = $_.Exception.Message
    }
    if (-not $wrongTransformRejected -or
        $wrongTransformError -notlike
            "所选视频必须以 1:1、无旋转/二次裁剪方式对齐*") {
        throw "所选视频没有对齐中心 ROI 时必须失败封闭"
    }

    $unexpectedCrop = Join-Path $obsConfigRoot `
        "basic\scenes\unexpected-crop.json"
    $unexpectedCropDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    $unexpectedCropScene = $unexpectedCropDocument.sources |
        Where-Object { $_.name -eq "固定测试输出" }
    ($unexpectedCropScene.settings.items |
        Where-Object { $_.name -eq "静止" }).crop_left = 1
    Write-Utf8NoBom $unexpectedCrop `
        ($unexpectedCropDocument | ConvertTo-Json -Depth 12)
    $unexpectedCropUserConfig = Join-Path $obsConfigRoot `
        "unexpected-crop-user.ini"
    Write-ObsUserConfig $unexpectedCropUserConfig "未命名" `
        "unexpected-crop.json"
    $unexpectedCropRejected = $false
    $unexpectedCropError = ""
    try {
        & $BindingScript `
            -SceneCollectionPath $unexpectedCrop `
            -ObsUserConfigPath $unexpectedCropUserConfig `
            -ObsProfileConfigPath $obsProfileConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -ExpectedSourceWidth 2560 `
            -ExpectedSourceHeight 1440 `
            -ExpectedRoiWidth 320 `
            -ExpectedRoiHeight 320 `
            -OutputPath (Join-Path $root "unexpected-crop-binding.json")
    } catch {
        $unexpectedCropRejected = $true
        $unexpectedCropError = $_.Exception.Message
    }
    if (-not $unexpectedCropRejected -or
        $unexpectedCropError -notlike
            "所选视频必须以 1:1、无旋转/二次裁剪方式对齐*") {
        throw "所选视频存在二次 scene-item 裁剪时必须失败封闭"
    }
    Write-Host "OBS source/transform/file/NDI binding 测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-XenOwnedTestDirectory -RootPath $root `
            -BasePath $ownedTest.BasePath `
            -RepositoryRoot $repositoryRoot `
            -OwnerId $ownedTest.OwnerId
    }
}
