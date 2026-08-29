param(
    [Parameter(Mandatory = $true)]
    [string]$BindingScript,
    [Parameter(Mandatory = $true)]
    [string]$TestRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Utf8NoBom([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
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

$root = [IO.Path]::GetFullPath($TestRoot)
if ($root -match '^[A-Za-z]:\\?$' -or $root -eq '\') {
    throw "拒绝在文件系统根目录执行 OBS binding 测试"
}
if (Test-Path -LiteralPath $root) {
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null

try {
    $static = Join-Path $root "静止.mp4"
    $jump = Join-Path $root "原地跳跃.mp4"
    $move = Join-Path $root "左右横移.mp4"
    Write-Utf8NoBom $static "static-video"
    Write-Utf8NoBom $jump "jump-video"
    Write-Utf8NoBom $move "move-video"
    $collection = Join-Path $root "未命名.json"
    Write-SceneCollection $collection $static $jump $move $true $false
    $obsUserConfig = Join-Path $root "user.ini"
    Write-Utf8NoBom $obsUserConfig @"
[NDIPlugin]
MainOutputEnabled=true
MainOutputName=Xen-ROI-320
PreviewOutputEnabled=false
"@

    $output = Join-Path $root "binding.json"
    & $BindingScript `
        -SceneCollectionPath $collection `
        -ObsUserConfigPath $obsUserConfig `
        -ExpectedNdiOutputName "Xen-ROI-320" `
        -SceneName "固定测试输出" `
        -SourceNames @("静止", "原地跳跃", "左右横移") `
        -SelectedSourceName "静止" `
        -OutputPath $output
    $binding = Get-Content -LiteralPath $output -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($binding.schema_version -ne 2 -or
        $binding.evidence_type -ne "obs_source_binding" -or
        $binding.physical_output_capability -ne $false -or
        $binding.scene_collection_sha256 -ne
            (Get-TestFileSha256 $collection) -or
        $binding.obs_user_config.sha256 -ne
            (Get-TestFileSha256 $obsUserConfig) -or
        $binding.ndi_main_output.enabled -ne $true -or
        $binding.ndi_main_output.name -ne "Xen-ROI-320" -or
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

    $existing = Get-Content -LiteralPath $output -Raw -Encoding UTF8
    $duplicateRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $obsUserConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -OutputPath $output
    } catch {
        $duplicateRejected = $true
    }
    if (-not $duplicateRejected -or
        (Get-Content -LiteralPath $output -Raw -Encoding UTF8) -ne $existing) {
        throw "既有 binding 必须拒绝覆盖且内容不变"
    }

    $ambiguous = Join-Path $root "ambiguous.json"
    Write-SceneCollection $ambiguous $static $jump $move $true $true
    $ambiguousRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $ambiguous `
            -ObsUserConfigPath $obsUserConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -OutputPath (Join-Path $root "ambiguous-binding.json")
    } catch {
        $ambiguousRejected = $true
    }
    if (-not $ambiguousRejected) {
        throw "候选中有多个可见源时必须失败封闭"
    }

    # 公开 red：selected 候选并非 Program scene 中唯一可见的出像素项时，
    # 即使额外项不在 SourceNames 候选列表内，也不能把 saved 配置导出为
    # selected 媒体的排他性 binding。
    $nonCandidateVisible = Join-Path $root "non-candidate-visible.json"
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
    $nonCandidateVisibleRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $nonCandidateVisible `
            -ObsUserConfigPath $obsUserConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -OutputPath (Join-Path $root `
                "non-candidate-visible-binding.json")
    } catch {
        $nonCandidateVisibleRejected = $true
    }
    if (-not $nonCandidateVisibleRejected) {
        throw "所选源之外存在可见 Program scene item 时必须失败封闭"
    }

    $hiddenRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $collection `
            -ObsUserConfigPath $obsUserConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "原地跳跃" `
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
            -ExpectedNdiOutputName "unexpected-output" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -OutputPath (Join-Path $root "ndi-mismatch-binding.json")
    } catch {
        $ndiMismatchRejected = $true
    }
    if (-not $ndiMismatchRejected) {
        throw "NDI 主输出名不一致时必须失败封闭"
    }

    $notLooping = Join-Path $root "not-looping.json"
    $notLoopingDocument = Get-Content -LiteralPath $collection -Raw `
        -Encoding UTF8 | ConvertFrom-Json
    ($notLoopingDocument.sources |
        Where-Object { $_.name -eq "静止" }).settings.looping = $false
    Write-Utf8NoBom $notLooping `
        ($notLoopingDocument | ConvertTo-Json -Depth 12)
    $notLoopingRejected = $false
    try {
        & $BindingScript `
            -SceneCollectionPath $notLooping `
            -ObsUserConfigPath $obsUserConfig `
            -ExpectedNdiOutputName "Xen-ROI-320" `
            -SceneName "固定测试输出" `
            -SourceNames @("静止", "原地跳跃", "左右横移") `
            -SelectedSourceName "静止" `
            -OutputPath (Join-Path $root "not-looping-binding.json")
    } catch {
        $notLoopingRejected = $true
    }
    if (-not $notLoopingRejected) {
        throw "固定素材未循环时必须失败封闭"
    }
    Write-Host "OBS source/transform/file/NDI binding 测试通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
