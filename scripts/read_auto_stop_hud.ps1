[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [ValidateRange(1, 1000)][int]$Step = 1
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Windows PowerShell 5.1利用系统WinRT投影，无需安装OCR包或调用网络服务。
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'DEPENDENCY_FAILED: 请使用Windows PowerShell 5.1运行WinRT OCR。' }
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
    $null = [Windows.Storage.Streams.IRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
    $null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
    $null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
    $null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
    $null = [Windows.Media.Ocr.OcrResult, Windows.Foundation, ContentType = WindowsRuntime]
    $null = [Windows.Globalization.Language, Windows.Globalization, ContentType = WindowsRuntime]
    $awaitMethod = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1
    if ($null -eq $awaitMethod) { throw 'WinRT AsTask未找到' }
    $language = [Windows.Globalization.Language]::new('en-US')
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
    if ($null -eq $engine) { throw '系统未安装英语OCR识别语言' }
} catch { throw ('DEPENDENCY_FAILED: ' + $_.Exception.Message) }

function Wait-WinRt($Operation, [Type]$ResultType) {
    $task = $awaitMethod.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    $task.GetAwaiter().GetResult()
}
function Get-HudNumber([string]$Text, [string]$Pattern, [int]$Group = 1) {
    $match = [regex]::Match($Text, $Pattern, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (!$match.Success) { return $null }
    $value = 0.0
    if ([double]::TryParse($match.Groups[$Group].Value, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) { return $value }
    return $null
}

$inputRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
$destination = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $destination) { throw '输出文件已存在，拒绝覆盖原始证据。' }
$files = @(Get-ChildItem -LiteralPath $inputRoot -Filter '*.png' -File | Sort-Object @{Expression = { if ($_.BaseName -match '(\d+)$') { [long]$Matches[1] } else { [long]::MaxValue } }}, Name)
if ($files.Count -eq 0) { throw '输入目录没有PNG文件。' }
$rows = [Collections.Generic.List[object]]::new()
$number = '([-+]?(?:\d+(?:\.\d+)?|\.\d+))'
$positionPattern = '(?m)^\s*pos\s*[:=]?\s*' + $number + '\s+' + $number + '\s+' + $number + '(?![\d.])'
$watch = [Diagnostics.Stopwatch]::StartNew()
for ($index = 0; $index -lt $files.Count; $index += $Step) {
    $stream = $null
    $bitmap = $null
    $row = [ordered]@{ file = $files[$index].Name; source_index = $index; status = 'OCR_FAILED'; text = $null;
        lines = @(); visual_lines = @(); game_time_roi_text = $null; game_time_geometry_text = $null; game_time_source = $null; game_time_whitespace_normalized = $false; posX = $null; posY = $null; posZ = $null; vel = $null; vel_peak_3s = $null; GameTime = $null; error = $null }
    try {
        $file = Wait-WinRt ([Windows.Storage.StorageFile]::GetFileFromPathAsync($files[$index].FullName)) ([Windows.Storage.StorageFile])
        $stream = Wait-WinRt ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $decoder = Wait-WinRt ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bitmap = Wait-WinRt ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $result = Wait-WinRt ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
        $row.text = $result.Text
        $row.lines = @($result.Lines | ForEach-Object { $_.Text })
        # OCR可能先读左列标签再读右列数值，按词框中心重建同一水平行。
        $words = @($result.Lines | ForEach-Object { $_.Words } | ForEach-Object {
            [pscustomobject]@{ text = $_.Text; x = $_.BoundingRect.X; center = $_.BoundingRect.Y + $_.BoundingRect.Height / 2; height = $_.BoundingRect.Height }
        } | Sort-Object center)
        $visualRows = [Collections.Generic.List[object]]::new()
        foreach ($word in $words) {
            $matchedRow = $null
            foreach ($visualRow in $visualRows) {
                if ([math]::Abs($word.center - $visualRow.center) -le [math]::Min($word.height, $visualRow.height) * 0.5) { $matchedRow = $visualRow; break }
            }
            if ($null -eq $matchedRow) {
                $matchedRow = [pscustomobject]@{ center = $word.center; height = $word.height; words = [Collections.Generic.List[object]]::new() }
                $visualRows.Add($matchedRow)
            }
            $matchedRow.words.Add($word)
        }
        $row.visual_lines = @($visualRows | Sort-Object center | ForEach-Object { (@($_.words | Sort-Object x | ForEach-Object { $_.text }) -join ' ') })
        $linesText = $row.visual_lines -join "`n"
        $row.posX = Get-HudNumber $linesText $positionPattern 1
        $row.posY = Get-HudNumber $linesText $positionPattern 2
        $row.posZ = Get-HudNumber $linesText $positionPattern 3
        $row.vel = Get-HudNumber $linesText ('(?m)^\s*vel\s*[:=]?\s*' + $number + '(?![\d.])')
        $row.vel_peak_3s = Get-HudNumber $linesText ('(?m)^\s*vel\s*[:=]?\s*' + $number + '\s*\(\s*' + $number + '\s*\)') 2
        $row.GameTime = Get-HudNumber $linesText ('(?m)^\s*Game\s*Time\s*[:=]?\s*' + $number + '(?![\d.])')
        if ($null -ne $row.GameTime) { $row.game_time_source = 'labeled_line' }
        if ($null -eq $row.GameTime -and $bitmap.PixelWidth -eq 650 -and $bitmap.PixelHeight -eq 310 -and
            $null -ne $row.posX -and $null -ne $row.vel) {
            # 小数点词框偏低，按固定底行区域和X顺序收齐，不依赖OCR行分组。
            $row.game_time_geometry_text = (@($words | Where-Object { $_.center -ge 260 } | Sort-Object x | ForEach-Object { $_.text }) -join ' ')
            $numericText = $row.game_time_geometry_text
            if ($numericText -match '^\s*[-+]?\d+\s*\.\s*\d+\s*$') {
                $normalizedText = $numericText -replace '\s*\.\s*', '.'
                $row.game_time_whitespace_normalized = $normalizedText -ne $numericText
                $row.GameTime = Get-HudNumber $normalizedText ('^\s*' + $number + '\s*$')
                if ($null -ne $row.GameTime) { $row.game_time_source = 'fixed_hud_words_0_260_650_50' }
            }
        }
        if ($null -eq $row.GameTime -and $bitmap.PixelWidth -eq 650 -and $bitmap.PixelHeight -eq 310) {
            $crop = $null
            try {
                $bounds = [Windows.Graphics.Imaging.BitmapBounds]::new()
                $bounds.X = 0; $bounds.Y = 260; $bounds.Width = 650; $bounds.Height = 50
                $transform = [Windows.Graphics.Imaging.BitmapTransform]::new()
                $transform.Bounds = $bounds
                $crop = Wait-WinRt ($decoder.GetSoftwareBitmapAsync([Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,
                    [Windows.Graphics.Imaging.BitmapAlphaMode]::Ignore, $transform,
                    [Windows.Graphics.Imaging.ExifOrientationMode]::IgnoreExifOrientation,
                    [Windows.Graphics.Imaging.ColorManagementMode]::DoNotColorManage)) ([Windows.Graphics.Imaging.SoftwareBitmap])
                $cropResult = Wait-WinRt ($engine.RecognizeAsync($crop)) ([Windows.Media.Ocr.OcrResult])
                $row.game_time_roi_text = $cropResult.Text
                $row.GameTime = Get-HudNumber $cropResult.Text ('^\s*Game\s*Time\s*[:=]?\s*' + $number + '\s*$')
                if ($null -ne $row.GameTime) { $row.game_time_source = 'labeled_roi' }
                elseif ($null -ne $row.posX -and $null -ne $row.vel) {
                    # 本工具输入契约是650x310固定HUD，底部260..310为GameTime。
                    # 标签漏识别时只接受该区域唯一完整数字，并明确记录几何来源。
                    $numericText = $cropResult.Text
                    if ($numericText -match '^\s*[-+]?\d+\s*\.\s*\d+\s*$') {
                        $numericText = $numericText -replace '\s*\.\s*', '.'
                        $row.game_time_whitespace_normalized = $true
                    }
                    $row.GameTime = Get-HudNumber $numericText '^\s*([-+]?\d+\.\d+)\s*$'
                    if ($null -ne $row.GameTime) { $row.game_time_source = 'fixed_hud_roi_0_260_650_50' }
                }
            } finally { if ($null -ne $crop) { $crop.Dispose() } }
        }
        $row.status = 'OCR_OK'
    } catch { $row.error = $_.Exception.Message }
    finally {
        if ($null -ne $bitmap) { $bitmap.Dispose() }
        if ($null -ne $stream) { $stream.Dispose() }
    }
    $rows.Add([pscustomobject]$row)
}
$watch.Stop()
$report = [ordered]@{ schema_version = 1; engine = 'Windows.Media.Ocr'; language = $engine.RecognizerLanguage.LanguageTag;
    input_directory = $inputRoot; step = $Step; source_count = $files.Count; processed_count = $rows.Count;
    elapsed_ms = $watch.Elapsed.TotalMilliseconds; frames = @($rows.ToArray()) }
$temporary = $destination + '.writing-' + [guid]::NewGuid().ToString('N')
[IO.File]::WriteAllText($temporary, ($report | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
[IO.File]::Move($temporary, $destination)
Write-Output ('OCR完成，帧数={0}，耗时ms={1:F1}；数字仅为识别结果，不推导停稳。' -f $rows.Count, $watch.Elapsed.TotalMilliseconds)
