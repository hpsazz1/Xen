param(
    [string]$TestRoot = (Join-Path $PSScriptRoot `
        "..\cache\release-transfer-aim-manual-test"),
    [switch]$ReleasePathSafetyOnly,
    [switch]$HostContractOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne "Desktop" -or
    $PSVersionTable.PSVersion.Major -ne 5 -or
    $PSVersionTable.PSVersion.Minor -lt 1) {
    throw "XEN_PS_HOST_UNSUPPORTED: requires Windows PowerShell 5.1."
}
if ($HostContractOnly) {
    Write-Host "XEN_PS_HOST_SUPPORTED: Windows PowerShell 5.1."
    return
}
Import-Module (Join-Path $PSScriptRoot "path_safety.psm1") -Force

function Write-Utf8([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText(
        $Path, $Content, [System.Text.UTF8Encoding]::new($false))
}

function New-CSharpFixtureExecutable([string]$Path, [string]$Source) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Add-Type -TypeDefinition $Source -Language CSharp `
        -OutputAssembly $Path -OutputType ConsoleApplication | Out-Null
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "无法生成测试夹具可执行文件：$Path"
    }
}

function Add-ManifestFile(
        [System.Collections.Generic.List[object]]$Files,
        [string]$Root,
        [string]$RelativePath,
        [string]$Runtime = "") {
    $path = Join-Path $Root $RelativePath
    $file = Get-Item -LiteralPath $path
    $Files.Add([ordered]@{
        path = $RelativePath.Replace('\', '/')
        runtime = $Runtime
        size = [long]$file.Length
        sha256 = (Get-FileHash -LiteralPath $path `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        source = "fixture/$RelativePath"
    })
}

function New-Schema13AimSample() {
    return [ordered]@{
        sequence = 541
        aim_status = "SUCCESS"
        mouse_sent = $true
        aim_has_target = $true
        aim_has_command = $true
        aim_track_id = 1
        aim_track_state = "CONFIRMED"
        aim_track_predicted = $false
        aim_lead_active = $false
        aim_base_point_inside_box = $true
        aim_prediction_point_outside_box = $false
        aim_command_toward_target = $true
        aim_acquisition_range_radius = 144.0
        aim_active_range_radius = 85.9412918
        aim_range_locked = $true
        aim_range_allows_control = $true
        aim_box = @(218.163315, 154.180466, 239.103516, 215.603760)
        aim_base_point = @(227.931946, 175.677979)
        aim_delay_compensated_point = @(227.931946, 175.677979)
        aim_final_point = @(227.931946, 175.677979)
        aim_lead = @(0.0, 0.0)
        aim_delay_compensation = @(0.0, 0.0)
        aim_delay_compensation_active = $false
        aim_delay_compensation_ms_x = 0.0
        aim_delay_compensation_ms_y = 0.0
        aim_delay_compensation_ms = 0.0
        aim_observation_age_ms = 1.65289998
        aim_command = @(13, 3)
        aim_control_center_x = 160.0
        source_pixels_per_pixel_x = 1.0
        aim_control_evaluated = $true
        aim_controller_dt_ms = 5.97350025
        aim_desired_x_counts = 13.680974
        aim_pending_absolute_x_counts = 0.0
        aim_reverse_candidate_x = $false
        aim_reverse_previous_direction_pending_x = $false
        aim_reverse_deformation_active_x = $false
        aim_reverse_evidence_ratio_seconds_x = 0.0
        aim_reverse_position_ratio_seconds_x = 0.0
        aim_reverse_required_evidence_ratio_seconds_x = 0.0
        aim_reverse_required_position_ratio_seconds_x = 0.0
        aim_reverse_evidence_ready_x = $false
        aim_reverse_position_ready_x = $false
        aim_reverse_gate_blocked_x = $false
        aim_pending_inventory_hold_blocked_x = $false
        aim_deadzone_quiet = $false
        aim_shaper_direction_reset_x = $false
        aim_post_alignment_sign_change_blocked_x = $false
        aim_post_alignment_growth_limited_x = $false
        aim_closing_response_tapered_x = $false
        aim_integer_direction_blocked_x = $false
        aim_command_sign_change_blocked_x = $false
        aim_quantization_zero_x = $false
        aim_reverse_probe_direction_x = 0
        aim_reverse_probe_age_ms_x = 0.0
        aim_reverse_probe_active_x = $false
        aim_reverse_probe_limited_x = $false
        aim_reverse_translation_seconds_x = 0.0
        aim_reverse_translation_ready_x = $false
        aim_reverse_output_direction_x = 0
        aim_reverse_translation_raw_left_x_roi_pixels = 0.0799713135
        aim_reverse_translation_raw_right_x_roi_pixels = -0.0196380615
        aim_reverse_translation_raw_common_x_roi_pixels = 0.0
        aim_reverse_translation_control_evidence_x = 0.0
        aim_reverse_translation_gap_seconds_x = 0.0
        aim_reverse_translation_fresh_evidence_x = $false
        aim_reverse_translation_reset_reason_x = "NONE"
        aim_reverse_position_peak_error_x = 0.0
        aim_reverse_position_improvement_reset_x = $false
        aim_modelled_response_x_counts = 0.0
        aim_observer_phase_command_x_counts = 0.0
        aim_observer_consistency_weight_x = 0.0
        mouse_completion_timing_valid = $true
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$ownedTest = New-XenOwnedTestDirectory -BasePath $TestRoot `
    -RepositoryRoot $repositoryRoot
$root = $ownedTest.RootPath

try {
    $package = Join-Path $root "Xen-fixture"
    foreach ($directory in @(
            "models", "runtimes\nvidia", "runtimes\directml",
            "runtimes\openvino", "tools", "cache", "logs", "licenses")) {
        New-Item -ItemType Directory -Path (Join-Path $package $directory) `
            -Force | Out-Null
    }
    New-CSharpFixtureExecutable (Join-Path $package "XenLauncher.exe") @'
using System;
using System.IO;
using System.Threading;

namespace XenAimManualLauncherFixture {
    public static class Program {
        private static void WriteMarker(
                string path, string sessionId, int activationEpoch,
                int sequence) {
            string temporary = path + ".tmp." + Guid.NewGuid().ToString("N");
            File.WriteAllText(temporary,
                "{\"schema\":2,\"session_id\":\"" + sessionId +
                "\",\"gate\":\"AIM_LOCK_ACTIVE\"," +
                "\"activation_epoch\":" + activationEpoch +
                ",\"sequence\":" + sequence + "}");
            if (File.Exists(path)) {
                File.Replace(temporary, path, null);
            } else {
                File.Move(temporary, path);
            }
        }

        public static int Main(string[] args) {
            string started = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_STARTED");
            string ready = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_READY");
            string success = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_SUCCESS");
            string reportSource = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_REPORT_SOURCE");
            string reportName = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_REPORT_NAME");
            string runtimeRoot = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_ROOT");
            string runtimeSessionId = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_SESSION_ID");
            string runtimeHoldText = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS");
            string recordingStarted = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_RECORDING_STARTED");
            string publishingStarted = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_PUBLISHING_STARTED");
            string dropMarkerPhase = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_DROP_MARKER_PHASE");
            string dropMarkerGraceText = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_DROP_MARKER_GRACE_MS");
            string dropMarkerMode = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_DROP_MARKER_MODE");
            string restoreMarkerText = Environment.GetEnvironmentVariable(
                "XEN_TEST_RUNTIME_RESTORE_MARKER_MS");
            string restoreMarkerSessionId =
                Environment.GetEnvironmentVariable(
                    "XEN_TEST_RUNTIME_RESTORE_MARKER_SESSION_ID");
            string restoreMarkerEpochText =
                Environment.GetEnvironmentVariable(
                    "XEN_TEST_RUNTIME_RESTORE_MARKER_EPOCH");
            string exitAfterPublishingText =
                Environment.GetEnvironmentVariable(
                    "XEN_TEST_RUNTIME_EXIT_AFTER_PUBLISHING_MS");
            if (String.IsNullOrEmpty(started) || String.IsNullOrEmpty(ready) ||
                String.IsNullOrEmpty(success) ||
                String.IsNullOrEmpty(reportSource) ||
                String.IsNullOrEmpty(reportName) ||
                String.IsNullOrEmpty(runtimeRoot) ||
                String.IsNullOrEmpty(runtimeSessionId)) {
                return 20;
            }
            string runtimeMarker = Path.Combine(
                runtimeRoot, reportName + ".aim-lock-active");
            try {
                Thread.Sleep(250);
                Directory.CreateDirectory(runtimeRoot);
                int sequence = 1;
                string markerSessionId = runtimeSessionId;
                int markerActivationEpoch = 1;
                WriteMarker(runtimeMarker, markerSessionId,
                    markerActivationEpoch, sequence);
                File.WriteAllText(ready, "ready");
                File.Copy(reportSource,
                    Path.Combine(runtimeRoot, reportName), true);
                DateTime deadline = DateTime.UtcNow.AddSeconds(5);
                while (!File.Exists(started) && DateTime.UtcNow < deadline) {
                    Thread.Sleep(100);
                    WriteMarker(runtimeMarker, markerSessionId,
                        markerActivationEpoch, ++sequence);
                }
                if (!File.Exists(started)) return 21;
                int dropMarkerGraceMilliseconds = 500;
                int parsedDropMarkerGraceMilliseconds = 0;
                if (Int32.TryParse(dropMarkerGraceText,
                        out parsedDropMarkerGraceMilliseconds)) {
                    dropMarkerGraceMilliseconds =
                        parsedDropMarkerGraceMilliseconds;
                }
                if (dropMarkerGraceMilliseconds < 0) {
                    dropMarkerGraceMilliseconds = 0;
                }
                int restoreMarkerMilliseconds = -1;
                int parsedRestoreMarkerMilliseconds = 0;
                if (Int32.TryParse(restoreMarkerText,
                        out parsedRestoreMarkerMilliseconds)) {
                    restoreMarkerMilliseconds =
                        parsedRestoreMarkerMilliseconds;
                }
                int restoreMarkerEpoch = 1;
                int parsedRestoreMarkerEpoch = 0;
                if (Int32.TryParse(restoreMarkerEpochText,
                        out parsedRestoreMarkerEpoch) &&
                    parsedRestoreMarkerEpoch > 0) {
                    restoreMarkerEpoch = parsedRestoreMarkerEpoch;
                }
                int exitAfterPublishingMilliseconds = -1;
                int parsedExitAfterPublishingMilliseconds = 0;
                if (Int32.TryParse(exitAfterPublishingText,
                        out parsedExitAfterPublishingMilliseconds)) {
                    exitAfterPublishingMilliseconds =
                        parsedExitAfterPublishingMilliseconds;
                }
                DateTime dropMarkerDeadline = DateTime.MaxValue;
                DateTime restoreMarkerDeadline = DateTime.MaxValue;
                DateTime exitAfterPublishingDeadline = DateTime.MaxValue;
                bool markerDropped = false;
                bool markerDropPerformed = false;
                while (!File.Exists(success) && DateTime.UtcNow < deadline) {
                    Thread.Sleep(100);
                    bool dropSignalObserved =
                        String.Equals(dropMarkerPhase, "RECORDING",
                            StringComparison.Ordinal) &&
                        !String.IsNullOrEmpty(recordingStarted) &&
                        File.Exists(recordingStarted) ||
                        String.Equals(dropMarkerPhase, "PUBLISHING",
                            StringComparison.Ordinal) &&
                        !String.IsNullOrEmpty(publishingStarted) &&
                        File.Exists(publishingStarted);
                    if (exitAfterPublishingMilliseconds >= 0 &&
                        !String.IsNullOrEmpty(publishingStarted) &&
                        File.Exists(publishingStarted)) {
                        if (exitAfterPublishingDeadline == DateTime.MaxValue) {
                            exitAfterPublishingDeadline =
                                DateTime.UtcNow.AddMilliseconds(
                                    exitAfterPublishingMilliseconds);
                        }
                        if (DateTime.UtcNow >= exitAfterPublishingDeadline) {
                            return 0;
                        }
                    }
                    if (!markerDropPerformed && dropSignalObserved &&
                        dropMarkerDeadline == DateTime.MaxValue) {
                        dropMarkerDeadline = DateTime.UtcNow.AddMilliseconds(
                            dropMarkerGraceMilliseconds);
                    }
                    if (!markerDropPerformed &&
                        DateTime.UtcNow >= dropMarkerDeadline) {
                        if (String.Equals(dropMarkerMode, "READ_ERROR",
                                StringComparison.Ordinal)) {
                            File.WriteAllText(runtimeMarker, "{");
                        } else if (File.Exists(runtimeMarker)) {
                            File.Delete(runtimeMarker);
                        }
                        markerDropped = true;
                        markerDropPerformed = true;
                        if (restoreMarkerMilliseconds >= 0) {
                            restoreMarkerDeadline =
                                DateTime.UtcNow.AddMilliseconds(
                                    restoreMarkerMilliseconds);
                        }
                    } else if (markerDropped &&
                        DateTime.UtcNow >= restoreMarkerDeadline) {
                        if (!String.IsNullOrEmpty(restoreMarkerSessionId)) {
                            markerSessionId = restoreMarkerSessionId;
                        }
                        markerActivationEpoch = restoreMarkerEpoch;
                        WriteMarker(runtimeMarker, markerSessionId,
                            markerActivationEpoch, ++sequence);
                        markerDropped = false;
                    } else if (!markerDropped) {
                        WriteMarker(runtimeMarker, markerSessionId,
                            markerActivationEpoch, ++sequence);
                    }
                }
                if (!File.Exists(success)) return 22;
                int holdMilliseconds = 300;
                int parsedHoldMilliseconds = 0;
                if (Int32.TryParse(runtimeHoldText,
                        out parsedHoldMilliseconds)) {
                    holdMilliseconds = parsedHoldMilliseconds;
                }
                if (holdMilliseconds < 0) holdMilliseconds = 0;
                deadline = DateTime.UtcNow.AddMilliseconds(holdMilliseconds);
                while (DateTime.UtcNow < deadline) {
                    Thread.Sleep(100);
                    if (!markerDropped) {
                        WriteMarker(runtimeMarker, markerSessionId,
                            markerActivationEpoch, ++sequence);
                    }
                }
                return 0;
            } finally {
                if (File.Exists(ready)) File.Delete(ready);
                if (File.Exists(runtimeMarker)) File.Delete(runtimeMarker);
            }
        }
    }
}
'@
    Write-Utf8 (Join-Path $package "config.ini") "fixture-config"
    Write-Utf8 (Join-Path $package "models\14wv11.onnx") "model"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\Xen.exe") "nvidia"
    Write-Utf8 (Join-Path $package "runtimes\nvidia\nvinfer_10.dll") "trt"
    Write-Utf8 (Join-Path $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll") "ndi"
    Write-Utf8 (Join-Path $package "runtimes\directml\Xen.exe") "dml"
    Write-Utf8 (Join-Path $package "runtimes\openvino\Xen.exe") "ov"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "aim_report.ps1") `
        -Destination (Join-Path $package "tools\aim_report.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "aim_control_diagnostics.ps1") `
        -Destination (Join-Path $package `
            "tools\aim_control_diagnostics.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "aim_fixed_scene_analysis.ps1") `
        -Destination (Join-Path $package `
            "tools\aim_fixed_scene_analysis.ps1")
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot `
        "invoke_aim_manual_acceptance.ps1") `
        -Destination (Join-Path $package `
            "tools\invoke_aim_manual_acceptance.ps1")

    $files = [System.Collections.Generic.List[object]]::new()
    Add-ManifestFile $files $package "XenLauncher.exe"
    Add-ManifestFile $files $package "config.ini"
    Add-ManifestFile $files $package "models\14wv11.onnx"
    Add-ManifestFile $files $package "runtimes\nvidia\Xen.exe" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\nvinfer_10.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\nvidia\Processing.NDI.Lib.x64.dll" "nvidia"
    Add-ManifestFile $files $package `
        "runtimes\directml\Xen.exe" "directml"
    Add-ManifestFile $files $package `
        "runtimes\openvino\Xen.exe" "openvino"
    Add-ManifestFile $files $package "tools\aim_report.ps1"
    Add-ManifestFile $files $package "tools\aim_control_diagnostics.ps1"
    Add-ManifestFile $files $package "tools\aim_fixed_scene_analysis.ps1"
    Add-ManifestFile $files $package `
        "tools\invoke_aim_manual_acceptance.ps1"
    [ordered]@{
        schema = 1
        product = "Xen"
        git_commit = "1" * 40
        runtimes = @(
            [ordered]@{
                id = "nvidia"
                executable = "runtimes/nvidia/Xen.exe"
                backends = @("cpu", "cuda", "tensorrt")
            },
            [ordered]@{
                id = "directml"
                executable = "runtimes/directml/Xen.exe"
                backends = @("directml")
            },
            [ordered]@{
                id = "openvino"
                executable = "runtimes/openvino/Xen.exe"
                backends = @("openvino")
            }
        )
        files = @($files)
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $package "manifest.json") `
            -Encoding utf8

    $destination = Join-Path $root "remote"
    New-Item -ItemType Directory -Path $destination | Out-Null
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $PSScriptRoot "transfer_release_bundle.ps1") `
        -PackagePath $package -DestinationRoot $destination
    if ($LASTEXITCODE -ne 0) {
        throw "完整发布包传输夹具失败。"
    }
    $published = Join-Path $destination "releases\Xen-fixture"
    if (-not (Test-Path -LiteralPath $published -PathType Container) -or
        @(Get-ChildItem -LiteralPath (Join-Path $destination "releases") `
            -Force | Where-Object { $_.Name -like ".incoming-*" }).Count -ne 0) {
        throw "完整发布包传输未原子收口。"
    }

    $transferSentinel = Join-Path $root "transfer-path-sentinel.txt"
    Write-Utf8 $transferSentinel "must-stay-unchanged"
    $transferSentinelHash = (Get-FileHash -LiteralPath $transferSentinel `
        -Algorithm SHA256).Hash
    $invalidDestinationDirectories = @(
        "..\escape",
        (Join-Path $root "absolute-destination"),
        "nested\releases")
    foreach ($invalidDestinationDirectory in $invalidDestinationDirectories) {
        $directoriesBefore = @(
            Get-ChildItem -LiteralPath $root -Directory -Recurse -Force |
                ForEach-Object { $_.FullName } | Sort-Object)
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $failure = (& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $PSScriptRoot "transfer_release_bundle.ps1") `
            -PackagePath $package `
            -DestinationRoot $destination `
            -DestinationDirectory $invalidDestinationDirectory 2>&1) -join `
                [Environment]::NewLine
        $failureExitCode = $LASTEXITCODE
        $ErrorActionPreference = $savedErrorActionPreference
        $directoriesAfter = @(
            Get-ChildItem -LiteralPath $root -Directory -Recurse -Force |
                ForEach-Object { $_.FullName } | Sort-Object)
        if ($failureExitCode -eq 0 -or
            $failure -notmatch "safe basename" -or
            (Compare-Object $directoriesBefore $directoriesAfter) -or
            (Get-FileHash -LiteralPath $transferSentinel `
                -Algorithm SHA256).Hash -ne $transferSentinelHash) {
            throw "DestinationDirectory 未在 mkdir 前失败封闭：$invalidDestinationDirectory；$failure"
        }
    }

    $junctionDestination = Join-Path $root "remote-junction"
    $junctionTarget = Join-Path $root "remote-junction-target"
    New-Item -ItemType Directory -Path $junctionDestination | Out-Null
    New-Item -ItemType Directory -Path $junctionTarget | Out-Null
    $releaseJunction = Join-Path $junctionDestination "releases"
    New-Item -ItemType Junction -Path $releaseJunction `
        -Value $junctionTarget -ErrorAction Stop | Out-Null
    $junctionAttributes = [IO.File]::GetAttributes($releaseJunction)
    if (($junctionAttributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        throw "Transfer junction fixture was not created as a reparse point."
    }
    $junctionSentinel = Join-Path $junctionTarget "keep.txt"
    Write-Utf8 $junctionSentinel "must-stay-unchanged"
    $junctionSentinelHash = (Get-FileHash -LiteralPath $junctionSentinel `
        -Algorithm SHA256).Hash
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $failure = (& powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $PSScriptRoot "transfer_release_bundle.ps1") `
            -PackagePath $package `
            -DestinationRoot $junctionDestination 2>&1) -join `
                [Environment]::NewLine
        $failureExitCode = $LASTEXITCODE
        $ErrorActionPreference = $savedErrorActionPreference
        $targetDirectories = @(
            Get-ChildItem -LiteralPath $junctionTarget -Directory -Force)
        if ($failureExitCode -eq 0 -or
            $failure -notmatch "reparse" -or
            $targetDirectories.Count -ne 0 -or
            (Get-FileHash -LiteralPath $junctionSentinel `
                -Algorithm SHA256).Hash -ne $junctionSentinelHash) {
            throw "DestinationDirectory junction 未在 mkdir/copy 前失败封闭：$failure"
        }
    } finally {
        if (Test-Path -LiteralPath $releaseJunction) {
            $junctionAttributes = [IO.File]::GetAttributes($releaseJunction)
            if (($junctionAttributes -band [IO.FileAttributes]::ReparsePoint) `
                    -eq 0) {
                throw "拒绝清理非 junction 的 Transfer 测试路径。"
            }
            [IO.Directory]::Delete($releaseJunction, $false)
        }
    }
    if ($ReleasePathSafetyOnly) {
        Write-Host "完整包传输与 DestinationDirectory 失败封闭回归通过。"
        return
    }

    Write-Utf8 (Join-Path $published "cache\tensorrt\fixture.engine") `
        "runtime provider cache"
    Write-Utf8 (Join-Path $published "logs\xen.log") "runtime log"

    Write-Utf8 (Join-Path $published "notes\local.txt") `
        "与本轮人工任务无关的本地文件"
    $directMlWorker = Join-Path $published "runtimes\directml\Xen.exe"
    Write-Utf8 $directMlWorker "dml-not-transferred-this-round"
    $taskScopedRoot = Join-Path $root "task-scoped-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -RequireSourceTiming `
        -PackageRoot $published -RunDirectory $taskScopedRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "任务范围校验不应扫描无关文件或重复哈希 DirectML Worker。"
    }
    $taskScopedTask = Get-Content -LiteralPath `
        (Join-Path $taskScopedRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    $taskScopedMarkdown = Get-Content -LiteralPath `
        (Join-Path $taskScopedRoot "TASK.md") -Raw -Encoding utf8
    $sourceClockCommand =
        'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\Xen\scripts\run_ndi_clock_source.ps1"'
    if ([string]$taskScopedTask.package_validation -ne "task_scoped" -or
        -not [bool]$taskScopedTask.require_source_timing -or
        $taskScopedMarkdown -notmatch [regex]::Escape($sourceClockCommand) -or
        $taskScopedMarkdown -match '填写 OBSERVATION\.md' -or
        $taskScopedMarkdown -notmatch '直接发送到当前对话' -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.worker.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.acceptance_script.sha256) -or
        [string]::IsNullOrWhiteSpace($taskScopedTask.aim_report.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.aim_control_diagnostics.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $taskScopedTask.aim_fixed_scene_analysis.sha256)) {
        throw "人工任务没有绑定可执行 source timing、对话回收、任务范围校验模式、Worker 或通用固定场景报告工具哈希。"
    }

    $pixelToolRoot = Join-Path $root "pixel-sidecar"
    New-Item -ItemType Directory -Path $pixelToolRoot | Out-Null
    New-CSharpFixtureExecutable `
        (Join-Path $pixelToolRoot "XenCaptureEvidence.exe") @'
using System;
using System.IO;

namespace XenAimManualPixelFixture {
    public static class Program {
        private static string ArgumentValue(string[] args, string name) {
            for (int index = 0; index + 1 < args.Length; ++index) {
                if (args[index] == name) return args[index + 1];
            }
            return String.Empty;
        }

        private static void CopyDirectory(string source, string destination) {
            Directory.CreateDirectory(destination);
            foreach (string file in Directory.GetFiles(source)) {
                File.Copy(file, Path.Combine(destination,
                    Path.GetFileName(file)), true);
            }
            foreach (string directory in Directory.GetDirectories(source)) {
                CopyDirectory(directory, Path.Combine(destination,
                    Path.GetFileName(directory)));
            }
        }

        public static int Main(string[] args) {
            string started = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_STARTED");
            string ready = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_READY");
            string success = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_SUCCESS");
            string counter = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_COUNTER");
            string evidenceSource = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_EVIDENCE_SOURCE");
            string holdText = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_HOLD_AFTER_SUCCESS_MS");
            string recordingStarted = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_RECORDING_STARTED");
            string publishingStarted = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_PUBLISHING_STARTED");
            string recordingHoldText = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_RECORDING_HOLD_MS");
            string publishHoldText = Environment.GetEnvironmentVariable(
                "XEN_TEST_PIXEL_PUBLISH_DELAY_MS");
            string output = ArgumentValue(args, "--output");
            if (String.IsNullOrEmpty(started) || String.IsNullOrEmpty(ready) ||
                String.IsNullOrEmpty(success) ||
                String.IsNullOrEmpty(counter) ||
                String.IsNullOrEmpty(evidenceSource) ||
                String.IsNullOrEmpty(output)) {
                return 30;
            }
            int attempt = 0;
            if (File.Exists(counter)) {
                Int32.TryParse(File.ReadAllText(counter), out attempt);
            }
            ++attempt;
            File.WriteAllText(counter, attempt.ToString());
            File.WriteAllText(started, "started");
            if (attempt == 1 || !File.Exists(ready)) {
                Console.Error.WriteLine(
                    "NDI Capture 失败：status=ACCESS_LOST；error=NDI 发现超时，未找到唯一匹配的源");
                return 1;
            }
            if (!String.IsNullOrEmpty(recordingStarted)) {
                File.WriteAllText(recordingStarted, "recording");
            }
            int recordingHoldMilliseconds = 0;
            if (Int32.TryParse(recordingHoldText,
                    out recordingHoldMilliseconds) &&
                recordingHoldMilliseconds > 0) {
                System.Threading.Thread.Sleep(recordingHoldMilliseconds);
            }
            if (!String.IsNullOrEmpty(publishingStarted)) {
                string outputName = Path.GetFileName(output);
                string incoming = Path.Combine(Path.GetDirectoryName(output),
                    "." + outputName + ".incoming-" +
                    System.Diagnostics.Process.GetCurrentProcess().Id +
                    "-fixture-0");
                CopyDirectory(evidenceSource, incoming);
                string frames = Path.Combine(incoming, "frames");
                Directory.CreateDirectory(frames);
                File.WriteAllText(Path.Combine(frames, "000000.png"),
                    "publishing");
                File.WriteAllText(publishingStarted, "publishing");
                int publishHoldMilliseconds = 0;
                if (Int32.TryParse(publishHoldText,
                        out publishHoldMilliseconds) &&
                    publishHoldMilliseconds > 0) {
                    System.Threading.Thread.Sleep(publishHoldMilliseconds);
                }
                Directory.Move(incoming, output);
            } else {
                CopyDirectory(evidenceSource, output);
            }
            File.WriteAllText(success, "success");
            Console.WriteLine("output-off NDI evidence fixture published");
            int holdMilliseconds = 0;
            if (Int32.TryParse(holdText, out holdMilliseconds) &&
                holdMilliseconds > 0) {
                System.Threading.Thread.Sleep(holdMilliseconds);
            }
            return 0;
        }
    }
}
'@
    Write-Utf8 (Join-Path $pixelToolRoot "opencv_world4140.dll") `
        "opencv runtime"
    Write-Utf8 (Join-Path $pixelToolRoot "Processing.NDI.Lib.x64.dll") `
        "ndi runtime"
    $pixelBinding = Join-Path $root "obs-source-binding.json"
    Write-Utf8 $pixelBinding (@'
{"schema_version":2,"evidence_type":"obs_source_binding","binding_mode":"real_game","physical_output_capability":false,"state_basis":"obs_saved_scene_collection","ndi_main_output":{"enabled":true,"name":"Xen-ROI-320"},"selected_source":{"name":"主画面","uuid":"source-main","id":"monitor_capture","monitor_id":"fixture-monitor-2560x1440","capture_cursor":false,"crop_filter":{"id":"crop_filter","enabled":true,"settings":{"left":1120,"top":560,"cx":320,"cy":320,"relative":false}}},"program_geometry":{"mapping":"monitor_crop_filter_1_to_1","source_width":2560,"source_height":1440,"roi_width":320,"roi_height":320,"roi_x":1120,"roi_y":560},"selected_scene_item":{"source_uuid":"source-main","id":4,"visible":true,"rot":0.0,"align":5,"bounds_type":0,"bounds_crop":false,"crop_left":0,"crop_top":0,"crop_right":0,"crop_bottom":0,"pos":{"x":0.0,"y":0.0},"scale":{"x":1.0,"y":1.0}},"candidate_visibility":[{"name":"主画面","source_uuid":"source-main","scene_item_id":4,"visible":true}]}
'@)
    $validPixelBindingText = Get-Content -LiteralPath $pixelBinding -Raw `
        -Encoding UTF8
    # 公开 red：真实 KMBOX 人工验收不得绑定固定媒体回放。固定媒体可用于
    # output-off 工具诊断，但不能代替实际游戏画面进入正式 Prepare。
    $fixedMediaBinding = Join-Path $root "obs-source-binding-fixed-media.json"
    Write-Utf8 $fixedMediaBinding (@'
{"schema_version":2,"evidence_type":"obs_source_binding","binding_mode":"fixed_media","physical_output_capability":false,"ndi_main_output":{"enabled":true,"name":"Xen-ROI-320"}}
'@)
    $fixedMediaTaskRoot = Join-Path $root "pixel-sidecar-fixed-media-task"
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $fixedMediaPrepareOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $fixedMediaBinding `
            -PackageRoot $published -RunDirectory $fixedMediaTaskRoot 2>&1)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $fixedMediaPrepareText = $fixedMediaPrepareOutput -join "`n"
    if ($LASTEXITCODE -eq 0 -or
        (Test-Path -LiteralPath $fixedMediaTaskRoot) -or
        $fixedMediaPrepareText -notlike
            "*OBS source binding 必须绑定实际游戏主画面：binding_mode=real_game*") {
        throw "正式人工验收必须在公开 Prepare seam 拒绝固定媒体 OBS binding。"
    }
    # 可变红回归：不能只把 fixed_media 标签改成 real_game 就进入正式 Run。
    $forgedRealGameBinding = Join-Path $root `
        "obs-source-binding-forged-real-game.json"
    Write-Utf8 $forgedRealGameBinding (@'
{"schema_version":2,"evidence_type":"obs_source_binding","binding_mode":"real_game","physical_output_capability":false,"ndi_main_output":{"enabled":true,"name":"Xen-ROI-320"},"selected_source":{"name":"原地跳跃","id":"ffmpeg_source"},"program_geometry":{"mapping":"center_crop_1_to_1"}}
'@)
    $forgedRealGameTaskRoot = Join-Path $root `
        "pixel-sidecar-forged-real-game-task"
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $forgedRealGamePrepareOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $forgedRealGameBinding `
            -PackageRoot $published `
            -RunDirectory $forgedRealGameTaskRoot 2>&1)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    $forgedRealGamePrepareText = $forgedRealGamePrepareOutput -join "`n"
    if ($LASTEXITCODE -eq 0 -or
        (Test-Path -LiteralPath $forgedRealGameTaskRoot) -or
        $forgedRealGamePrepareText -notlike
            "*OBS source binding 实际游戏几何合同不匹配*") {
        throw "正式人工验收必须拒绝只改 real_game 标签的固定媒体 binding。"
    }
    $pixelTaskRoot = Join-Path $root "pixel-sidecar-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelTaskRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "同步像素 sidecar 的 Prepare 合同必须可生成。"
    }
    $pixelTask = Get-Content -LiteralPath `
        (Join-Path $pixelTaskRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    $pixelTaskMarkdown = Get-Content -LiteralPath `
        (Join-Path $pixelTaskRoot "TASK.md") -Raw -Encoding utf8
    if (-not [bool]$pixelTask.pixel_evidence.enabled -or
        [string]$pixelTask.pixel_evidence.binding_mode -ne "real_game" -or
        [int]$pixelTask.pixel_evidence.frames -ne 2400 -or
        [int]$pixelTask.pixel_evidence.max_seconds -ne 30 -or
        [string]$pixelTask.pixel_evidence.output_relative_path -ne
            "pixel-evidence" -or
        [string]::IsNullOrWhiteSpace(
            $pixelTask.pixel_evidence.executable.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $pixelTask.pixel_evidence.opencv_runtime.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $pixelTask.pixel_evidence.ndi_runtime.sha256) -or
        [string]::IsNullOrWhiteSpace(
            $pixelTask.pixel_evidence.source_binding.sha256) -or
        [int]$pixelTask.pixel_evidence.runtime_alignment.marker_schema -ne 2 -or
        [int]$pixelTask.pixel_evidence.runtime_alignment.max_marker_age_ms -ne
            1000 -or
        $pixelTaskMarkdown -notmatch "同步 NDI 像素 sidecar" -or
        $pixelTaskMarkdown -notmatch "至少 15 秒" -or
        $pixelTaskMarkdown -notmatch "sidecar 已完成，可以松开右键" -or
        $pixelTaskMarkdown -notmatch
            "sidecar 未完成或已停止，可以松开右键" -or
        $pixelTaskMarkdown -match "按住右键约 5 秒再松开" -or
        $pixelTaskMarkdown -notmatch "-CapturePixelEvidence" -or
        $pixelTaskMarkdown -notmatch "-PixelEvidenceFrames 2400" -or
        $pixelTaskMarkdown -notmatch "-PixelEvidenceMaxSeconds 30") {
        throw "同步像素 sidecar 没有绑定工具闭包、source binding、输出目录或唯一 Launch 参数。"
    }
    $invalidPixelTaskRoot = Join-Path $root "pixel-sidecar-without-timing"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PackageRoot $published -RunDirectory $invalidPixelTaskRoot | Out-Null
    if ($LASTEXITCODE -eq 0 -or
        (Test-Path -LiteralPath $invalidPixelTaskRoot)) {
        throw "同步像素 sidecar 缺少 RequireSourceTiming 时必须 fail-closed，且不得创建 Run。"
    }
    $pixelAutomaticRoot = Join-Path $pixelTaskRoot "automatic"
    New-Item -ItemType Directory -Path $pixelAutomaticRoot -Force |
        Out-Null
    $pixelSchema13Report = [ordered]@{
        schema = 13
        session_id = "pixel-schema13"
        provider = "TensorrtExecutionProvider"
        capture_backend = "NDI"
        mouse_backend = "kmbox_net"
        sample_count = 1
        successful_samples = 1
        failed_samples = 0
        report_samples_dropped = 0
        runtime_samples_dropped = 0
        samples = @(New-Schema13AimSample)
    }
    Write-Utf8 (Join-Path $pixelAutomaticRoot "schema13.json") `
        (($pixelSchema13Report | ConvertTo-Json -Depth 10) + "`n")
    $pixelOutputRoot = Join-Path $pixelTaskRoot "pixel-evidence"
    New-Item -ItemType Directory -Path $pixelOutputRoot | Out-Null
    $pixelFrames = @(for ($frame = 0; $frame -lt 2400; ++$frame) {
        [ordered]@{
            source_time_timing_valid = $true
            source_clock_status = "VALID"
        }
    })
    $pixelManifest = [ordered]@{
        schema_version = 1
        evidence_type = "output_off_capture"
        physical_output_capability = $false
        capture_backend = "NDI"
        capture_source_name = "HPSAZZ (Xen-ROI-320)"
        capture_config = [ordered]@{ require_source_timing = $true }
        requested_frame_count = 2400
        recorded_frame_count = 2400
        source_binding = [ordered]@{
            sha256 = [string]$pixelTask.pixel_evidence.source_binding.sha256
        }
        frames = $pixelFrames
    }
    $validPixelManifestBindingHash =
        [string]$pixelManifest.source_binding.sha256
    Write-Utf8 (Join-Path $pixelOutputRoot "manifest.json") `
        (($pixelManifest | ConvertTo-Json -Depth 5) + "`n")
    Copy-Item -LiteralPath $pixelBinding -Destination `
        (Join-Path $pixelOutputRoot "source-binding.json")
    $pixelRecoverOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelTaskRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $pixelRecoverOutput | ForEach-Object { Write-Host $_ }
        throw "同步像素 sidecar 离线回收失败。"
    }
    $pixelRecoveredSummary = Get-Content -LiteralPath `
        (Join-Path $pixelTaskRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not [bool]$pixelRecoveredSummary.pixel_evidence.enabled -or
        [bool]$pixelRecoveredSummary.pixel_evidence.gate_passed -or
        [string]$pixelRecoveredSummary.pixel_evidence.diagnostic -ne
            "RUNTIME_ALIGNMENT_FAILED" -or
        [string]$pixelRecoveredSummary.pixel_evidence.execution_error `
            -notmatch '缺少新 sidecar 生命周期合同' -or
        [int]$pixelRecoveredSummary.pixel_evidence.recorded_frames -ne 2400 -or
        [int]$pixelRecoveredSummary.pixel_evidence.source_timing_valid_frames -ne
            2400 -or
        [bool]$pixelRecoveredSummary.automatic_complete) {
        throw "新 sidecar 合同缺 attempts 证据时 Recover 必须 fail-closed。"
    }

    # 公开 red：sidecar 已完成 PNG/binding/manifest，只在最终目录 rename
    # 上失败时，Recover 必须验证并原子发布保留下来的 incoming；不得要求
    # 再次 Launch，也不得把原始失败 attempt 改写成成功。
    $pixelPreservedRoot = Join-Path $root `
        "pixel-sidecar-preserved-incoming-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 1 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelPreservedRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "完整 incoming Recover 正例必须可 Prepare。"
    }
    $pixelPreservedTask = Get-Content -LiteralPath `
        (Join-Path $pixelPreservedRoot "task.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $pixelPreservedAutomaticRoot = Join-Path $pixelPreservedRoot "automatic"
    New-Item -ItemType Directory -Path $pixelPreservedAutomaticRoot -Force |
        Out-Null
    Copy-Item -LiteralPath (Join-Path $pixelAutomaticRoot "schema13.json") `
        -Destination (Join-Path $pixelPreservedAutomaticRoot "schema13.json")
    $pixelPreservedIncoming = Join-Path $pixelPreservedRoot `
        ".pixel-evidence.incoming-4242-123456-0"
    $pixelPreservedFrames = Join-Path $pixelPreservedIncoming "frames"
    New-Item -ItemType Directory -Path $pixelPreservedFrames -Force |
        Out-Null
    $pixelPreservedFramePath = Join-Path $pixelPreservedFrames "000000.png"
    Write-Utf8 $pixelPreservedFramePath "fixture-png"
    $pixelPreservedFrameHash = (Get-FileHash -LiteralPath `
        $pixelPreservedFramePath -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $pixelBinding -Destination `
        (Join-Path $pixelPreservedIncoming "source-binding.json")
    $pixelPreservedManifest = [ordered]@{
        schema_version = 1
        evidence_type = "output_off_capture"
        physical_output_capability = $false
        capture_backend = "NDI"
        capture_source_name = "HPSAZZ (Xen-ROI-320)"
        capture_config = [ordered]@{ require_source_timing = $true }
        requested_frame_count = 1
        recorded_frame_count = 1
        source_binding = [ordered]@{
            sha256 = [string]$pixelPreservedTask.pixel_evidence.source_binding.sha256
        }
        frames = @([ordered]@{
            file = "frames/000000.png"
            png_sha256 = "0" * 64
            source_time_timing_valid = $true
            source_clock_status = "VALID"
        })
    }
    Write-Utf8 (Join-Path $pixelPreservedIncoming "manifest.json") `
        (($pixelPreservedManifest | ConvertTo-Json -Depth 8) + "`n")
    $pixelPreservedStderrName = `
        "pixel-evidence.attempt-01.stderr.log"
    $pixelPreservedStdoutName = `
        "pixel-evidence.attempt-01.stdout.log"
    $pixelPreservedFinal = Join-Path $pixelPreservedRoot "pixel-evidence"
    $pixelPreservedReportedRun = Join-Path `
        "C:\XenLab\reports\aim-dual-manual" `
        (Split-Path -Leaf $pixelPreservedRoot)
    $pixelPreservedReportedIncoming = Join-Path `
        $pixelPreservedReportedRun `
        (Split-Path -Leaf $pixelPreservedIncoming)
    $pixelPreservedReportedFinal = Join-Path `
        $pixelPreservedReportedRun "pixel-evidence"
    Write-Utf8 (Join-Path $pixelPreservedRoot $pixelPreservedStdoutName) ""
    Write-Utf8 (Join-Path $pixelPreservedRoot $pixelPreservedStderrName) `
        ("发布证据失败：无法完成证据目录原子发布：Access is denied." +
        "；code=5；attempts=301；elapsed_ms=30069" +
        "；incoming=$pixelPreservedReportedIncoming" +
        "；final=$pixelPreservedReportedFinal`n")
    $pixelPreservedAttempts = [ordered]@{
        schema = 2
        max_attempts = [int]$pixelPreservedTask.pixel_evidence.max_attempts
        process_exit_code = 1
        execution_error = ""
        runtime_alignment = [ordered]@{
            required = $true
            gate = "AIM_LOCK_ACTIVE"
            gate_passed = $true
            session_id = "pixel-schema13"
            activation_epoch = [uint64]1
            marker = $null
        }
        attempts = @([ordered]@{
            attempt = 1
            started_utc = "2026-08-30T03:32:31.2214011Z"
            ended_utc = "2026-08-30T03:33:39.2757043Z"
            exit_code = 1
            diagnostic = "FAILED"
            succeeded = $false
            retryable = $false
            manifest_published = $false
            runtime_active_at_start = $true
            runtime_active_at_recording_completion = $true
            runtime_active_at_completion = $false
            stdout_log = $pixelPreservedStdoutName
            stderr_log = $pixelPreservedStderrName
        })
    }
    Write-Utf8 (Join-Path $pixelPreservedRoot `
        "pixel-evidence-attempts.json") `
        (($pixelPreservedAttempts | ConvertTo-Json -Depth 10) + "`n")
    $pixelTamperedRecoverOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 1 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelPreservedRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $pixelTamperedRecoverOutput | ForEach-Object { Write-Host $_ }
        throw "被篡改 incoming 的 fail-closed Recover 执行失败。"
    }
    $pixelTamperedSummary = Get-Content -LiteralPath `
        (Join-Path $pixelPreservedRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ((Test-Path -LiteralPath $pixelPreservedFinal) -or
        -not (Test-Path -LiteralPath $pixelPreservedIncoming `
            -PathType Container) -or
        [string]$pixelTamperedSummary.pixel_evidence.publication_recovery.diagnostic `
            -ne "INCOMING_VALIDATION_FAILED" -or
        [string]$pixelTamperedSummary.pixel_evidence.publication_recovery.error `
            -notmatch "PNG 哈希不一致") {
        $pixelTamperedRecoverOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($pixelTamperedSummary.pixel_evidence |
            ConvertTo-Json -Depth 10)
        throw "PNG 哈希不一致的 incoming 必须保留原位并拒绝发布。"
    }
    $pixelPreservedManifest.frames[0].png_sha256 =
        $pixelPreservedFrameHash
    Write-Utf8 (Join-Path $pixelPreservedIncoming "manifest.json") `
        (($pixelPreservedManifest | ConvertTo-Json -Depth 8) + "`n")
    $pixelPreservedRecoverOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 1 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelPreservedRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $pixelPreservedRecoverOutput | ForEach-Object { Write-Host $_ }
        throw "完整 incoming 离线 Recover 执行失败。"
    }
    $pixelPreservedSummary = Get-Content -LiteralPath `
        (Join-Path $pixelPreservedRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $pixelPreservedRecoveryFields =
        $pixelPreservedSummary.pixel_evidence.PSObject.Properties.Name
    if (-not (Test-Path -LiteralPath (Join-Path $pixelPreservedFinal `
                "manifest.json") -PathType Leaf) -or
        (Test-Path -LiteralPath $pixelPreservedIncoming) -or
        $pixelPreservedRecoveryFields -notcontains "publication_recovered" -or
        -not [bool]$pixelPreservedSummary.pixel_evidence.publication_recovered -or
        -not [bool]$pixelPreservedSummary.pixel_evidence.gate_passed -or
        [string]$pixelPreservedSummary.pixel_evidence.diagnostic -ne
            "VALID_RECOVERED" -or
        [bool]$pixelPreservedSummary.pixel_evidence.attempts[0].succeeded -or
        [bool]$pixelPreservedSummary.pixel_evidence.attempts[0].manifest_published) {
        $pixelPreservedRecoverOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($pixelPreservedSummary.pixel_evidence |
            ConvertTo-Json -Depth 10)
        throw "完整 incoming 必须由 Recover 验证后原子发布并保留原始失败 attempt。"
    }

    # 模拟 binding_mode/max_attempts 字段加入前的历史 task 和内嵌 binding，
    # 证明旧 Run 仍可按 manifest 离线 Recover；新 Prepare 不得走兼容路径。
    [void]$pixelTask.pixel_evidence.PSObject.Properties.Remove("max_attempts")
    [void]$pixelTask.pixel_evidence.PSObject.Properties.Remove(
        "runtime_alignment")
    [void]$pixelTask.pixel_evidence.PSObject.Properties.Remove("binding_mode")
    $legacyBinding = $validPixelBindingText | ConvertFrom-Json
    [void]$legacyBinding.PSObject.Properties.Remove("binding_mode")
    $legacyBindingText = ($legacyBinding | ConvertTo-Json -Depth 12) + "`n"
    try {
        Write-Utf8 $pixelBinding $legacyBindingText
        Write-Utf8 (Join-Path $pixelOutputRoot "source-binding.json") `
            $legacyBindingText
        $legacyBindingEvidence = Get-Item -LiteralPath $pixelBinding
        $legacyBindingHash = (Get-FileHash -LiteralPath $pixelBinding `
            -Algorithm SHA256).Hash
        $pixelTask.pixel_evidence.source_binding.length =
            [long]$legacyBindingEvidence.Length
        $pixelTask.pixel_evidence.source_binding.sha256 = $legacyBindingHash
        $pixelManifest.source_binding.sha256 = $legacyBindingHash
        Write-Utf8 (Join-Path $pixelOutputRoot "manifest.json") `
            (($pixelManifest | ConvertTo-Json -Depth 5) + "`n")
        Write-Utf8 (Join-Path $pixelTaskRoot "task.json") `
            (($pixelTask | ConvertTo-Json -Depth 10) + "`n")
        $pixelLegacyRecoverOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelTaskRoot 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $pixelLegacyRecoverOutput | ForEach-Object { Write-Host $_ }
            throw "旧 sidecar task 的兼容 Recover 失败。"
        }
        $pixelLegacyRecoveredSummary = Get-Content -LiteralPath `
            (Join-Path $pixelTaskRoot "automatic-summary.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        if (-not [bool]$pixelLegacyRecoveredSummary.pixel_evidence.gate_passed -or
            [string]$pixelLegacyRecoveredSummary.pixel_evidence.diagnostic -ne
                "VALID" -or
            [int]$pixelLegacyRecoveredSummary.pixel_evidence.recorded_frames -ne
                2400 -or
            [bool]$pixelLegacyRecoveredSummary.automatic_complete) {
            throw "旧 task 兼容 Recover 必须保留有效像素 manifest，但不得覆盖 Runtime timing 失败。"
        }
    } finally {
        Write-Utf8 $pixelBinding $validPixelBindingText
        $pixelManifest.source_binding.sha256 =
            $validPixelManifestBindingHash
    }

    $pixelLifecycleRoot = Join-Path $root "pixel-sidecar-lifecycle-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelLifecycleRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "同步像素 sidecar 生命周期测试任务必须可 Prepare。"
    }
    $pixelLifecycleTask = Get-Content -LiteralPath `
        (Join-Path $pixelLifecycleRoot "task.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ([int]$pixelLifecycleTask.pixel_evidence.max_attempts -ne 6) {
        throw "新 Prepare 必须把 sidecar 有界重试次数固化进 task.json。"
    }
    $pixelLifecycleEvidenceSource = Join-Path $root `
        "pixel-lifecycle-valid-evidence"
    New-Item -ItemType Directory -Path $pixelLifecycleEvidenceSource |
        Out-Null
    Write-Utf8 (Join-Path $pixelLifecycleEvidenceSource "manifest.json") `
        (($pixelManifest | ConvertTo-Json -Depth 5) + "`n")
    Copy-Item -LiteralPath $pixelBinding -Destination `
        (Join-Path $pixelLifecycleEvidenceSource "source-binding.json")
    $pixelLifecycleReportSource = Join-Path $root `
        "pixel-lifecycle-runtime.json"
    Write-Utf8 $pixelLifecycleReportSource `
        (($pixelSchema13Report | ConvertTo-Json -Depth 10) + "`n")
    $pixelLifecycleStarted = Join-Path $root "pixel-lifecycle.started"
    $pixelLifecycleReady = Join-Path $root "pixel-lifecycle.ready"
    $pixelLifecycleSuccess = Join-Path $root "pixel-lifecycle.success"
    $pixelLifecycleCounter = Join-Path $root "pixel-lifecycle.count"
    $savedFixtureEnvironment = @{}
    $fixtureEnvironment = [ordered]@{
        XEN_TEST_PIXEL_STARTED = $pixelLifecycleStarted
        XEN_TEST_PIXEL_READY = $pixelLifecycleReady
        XEN_TEST_PIXEL_SUCCESS = $pixelLifecycleSuccess
        XEN_TEST_PIXEL_COUNTER = $pixelLifecycleCounter
        XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
        XEN_TEST_PIXEL_HOLD_AFTER_SUCCESS_MS = "1500"
        XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "2000"
        XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
        XEN_TEST_RUNTIME_REPORT_NAME = "pixel-lifecycle-success.json"
        XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
        XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
    }
    foreach ($name in $fixtureEnvironment.Keys) {
        $savedFixtureEnvironment[$name] = [Environment]::GetEnvironmentVariable(
            $name, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            $name, [string]$fixtureEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $pixelLifecycleOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelLifecycleRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $pixelLifecycleOutput | ForEach-Object { Write-Host $_ }
            throw "无设备能力的 sidecar 生命周期 Launch 夹具执行失败。"
        }
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        foreach ($name in $fixtureEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name, $savedFixtureEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
    }
    $pixelLifecycleSummary = Get-Content -LiteralPath `
        (Join-Path $pixelLifecycleRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $pixelLifecycleAttempts = if (Test-Path -LiteralPath `
            $pixelLifecycleCounter -PathType Leaf) {
        [int](Get-Content -LiteralPath $pixelLifecycleCounter -Raw)
    } else { 0 }
    $pixelLifecycleAttemptEvidence = Get-Content -LiteralPath `
        (Join-Path $pixelLifecycleRoot "pixel-evidence-attempts.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not [bool]$pixelLifecycleSummary.pixel_evidence.gate_passed -or
        [string]$pixelLifecycleSummary.pixel_evidence.diagnostic -ne "VALID" -or
        $pixelLifecycleAttempts -lt 2 -or
        [int]$pixelLifecycleSummary.pixel_evidence.attempt_count -lt 2 -or
        @($pixelLifecycleAttemptEvidence.attempts).Count -lt 2 -or
        [string]$pixelLifecycleAttemptEvidence.attempts[0].diagnostic -ne
            "ACCESS_LOST" -or
        [bool]$pixelLifecycleAttemptEvidence.attempts[0].succeeded -or
        -not [bool]$pixelLifecycleAttemptEvidence.attempts[0].retryable -or
        -not [bool]$pixelLifecycleAttemptEvidence.attempts[-1].succeeded -or
        [bool]$pixelLifecycleAttemptEvidence.attempts[-1].retryable -or
        -not [bool]$pixelLifecycleAttemptEvidence.attempts[-1].manifest_published -or
        -not [bool]$pixelLifecycleSummary.pixel_evidence.runtime_alignment.required -or
        -not [bool]$pixelLifecycleSummary.pixel_evidence.runtime_alignment.gate_passed -or
        [string]$pixelLifecycleSummary.pixel_evidence.runtime_alignment.gate -ne
            "AIM_LOCK_ACTIVE" -or
        [string]$pixelLifecycleSummary.pixel_evidence.runtime_alignment.session_id -ne
            "pixel-schema13" -or
        [uint64]$pixelLifecycleSummary.pixel_evidence.runtime_alignment.activation_epoch -ne
            1 -or
        -not [bool]$pixelLifecycleAttemptEvidence.runtime_alignment.gate_passed -or
        [string]$pixelLifecycleAttemptEvidence.runtime_alignment.session_id -ne
            "pixel-schema13" -or
        [uint64]$pixelLifecycleAttemptEvidence.runtime_alignment.activation_epoch -ne
            1 -or
        ($pixelLifecycleOutput -join "`n") -notmatch
            "sidecar 已完成，可以松开右键" -or
        @($pixelLifecycleAttemptEvidence.attempts | Where-Object {
            -not [bool]$_.runtime_active_at_start -or
            -not [bool]$_.runtime_active_at_completion
        }).Count -ne 0 -or
        -not (Test-Path -LiteralPath (Join-Path $pixelLifecycleRoot `
                "pixel-evidence.stderr.log") -PathType Leaf)) {
        $pixelLifecycleOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($pixelLifecycleSummary.pixel_evidence |
            ConvertTo-Json -Depth 8)
        Write-Host "pixel_lifecycle_attempts=$pixelLifecycleAttempts"
        Get-ChildItem -LiteralPath $pixelLifecycleRoot -File |
            Where-Object { $_.Name -like "pixel-evidence*.log" } |
            Sort-Object Name | ForEach-Object {
                Write-Host "[$($_.Name)]"
                Get-Content -LiteralPath $_.FullName | ForEach-Object {
                    Write-Host $_
                }
        }
        throw "同一次 Launch 中 source 稍后可用时，sidecar 必须重试并发布 VALID manifest。"
    }

    function Assert-PixelMarkerTransientRecoveryValid(
            [string]$CaseName,
            [string]$DropMode,
            [string]$ExpectedReason) {
        $caseRoot = Join-Path $root `
            "pixel-sidecar-transient-$CaseName-task"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $caseRoot |
            Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "sidecar 短暂 marker 故障回归必须可 Prepare：$CaseName"
        }
        $caseCounter = Join-Path $root "pixel-transient-$CaseName.count"
        # 跳过夹具固定的首次 ACCESS_LOST，只隔离 marker lease。
        Write-Utf8 $caseCounter "1"
        $caseEnvironment = [ordered]@{
            XEN_TEST_PIXEL_STARTED = (Join-Path $root `
                "pixel-transient-$CaseName.started")
            XEN_TEST_PIXEL_READY = (Join-Path $root `
                "pixel-transient-$CaseName.ready")
            XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
                "pixel-transient-$CaseName.success")
            XEN_TEST_PIXEL_COUNTER = $caseCounter
            XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
            XEN_TEST_PIXEL_RECORDING_STARTED = (Join-Path $root `
                "pixel-transient-$CaseName.recording")
            XEN_TEST_PIXEL_PUBLISHING_STARTED = (Join-Path $root `
                "pixel-transient-$CaseName.publishing")
            XEN_TEST_PIXEL_RECORDING_HOLD_MS = "1800"
            XEN_TEST_PIXEL_PUBLISH_DELAY_MS = "300"
            XEN_TEST_RUNTIME_DROP_MARKER_PHASE = "RECORDING"
            XEN_TEST_RUNTIME_DROP_MARKER_GRACE_MS = "400"
            XEN_TEST_RUNTIME_DROP_MARKER_MODE = $DropMode
            XEN_TEST_RUNTIME_RESTORE_MARKER_MS = "200"
            XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "1000"
            XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
            XEN_TEST_RUNTIME_REPORT_NAME = "pixel-transient-$CaseName.json"
            XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
            XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
        }
        $savedEnvironment = @{}
        foreach ($name in $caseEnvironment.Keys) {
            $savedEnvironment[$name] =
                [Environment]::GetEnvironmentVariable(
                    $name, [EnvironmentVariableTarget]::Process)
            [Environment]::SetEnvironmentVariable(
                $name, [string]$caseEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
        try {
            $savedErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $caseOutput = @(& powershell.exe -NoProfile `
                -ExecutionPolicy Bypass -File `
                (Join-Path $published `
                    "tools\invoke_aim_manual_acceptance.ps1") `
                -TaskId AIM-SUPERJUMP-ACCEPT-001 `
                -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
                -Profile tracking -RequireSourceTiming `
                -CapturePixelEvidence `
                -PixelEvidenceToolRoot $pixelToolRoot `
                -PixelEvidenceBindingPath $pixelBinding `
                -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
                -PackageRoot $published -RunDirectory $caseRoot `
                -AllowPhysicalOutput `
                -PhysicalOutputConfirmation `
                    XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
            foreach ($name in $caseEnvironment.Keys) {
                [Environment]::SetEnvironmentVariable(
                    $name, $savedEnvironment[$name],
                    [EnvironmentVariableTarget]::Process)
            }
        }
        $summary = Get-Content -LiteralPath `
            (Join-Path $caseRoot "automatic-summary.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $attemptEvidence = Get-Content -LiteralPath `
            (Join-Path $caseRoot "pixel-evidence-attempts.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $attempts = @($attemptEvidence.attempts)
        $lastAttempt = if ($attempts.Count -gt 0) {
            $attempts[-1]
        } else { $null }
        $lastTransientFailure = if ($null -ne $lastAttempt -and
                $lastAttempt.PSObject.Properties.Name -contains
                    "runtime_marker_last_transient_failure") {
            $lastAttempt.runtime_marker_last_transient_failure
        } else { $null }
        $probeFieldsAvailable =
            $null -ne $lastAttempt -and
            $lastAttempt.PSObject.Properties.Name -contains
                "runtime_marker_transient_failure_count" -and
            $lastAttempt.PSObject.Properties.Name -contains
                "runtime_marker_last_valid_sequence" -and
            $null -ne $lastTransientFailure -and
            $lastTransientFailure.PSObject.Properties.Name -contains
                "last_valid_age_ms"
        if (-not [bool]$summary.pixel_evidence.gate_passed -or
            [string]$summary.pixel_evidence.diagnostic -ne "VALID" -or
            -not [bool]$attemptEvidence.runtime_alignment.gate_passed -or
            $attempts.Count -ne 1 -or
            -not [bool]$lastAttempt.succeeded -or
            -not [bool]$lastAttempt.manifest_published -or
            -not [bool]$lastAttempt.runtime_active_at_start -or
            -not [bool]$lastAttempt.runtime_active_at_recording_completion -or
            -not $probeFieldsAvailable -or
            ($probeFieldsAvailable -and
                [int]$lastAttempt.runtime_marker_transient_failure_count -lt
                    1) -or
            ($probeFieldsAvailable -and
                [string]$lastTransientFailure.reason -ne $ExpectedReason) -or
            ($probeFieldsAvailable -and
                [uint64]$lastTransientFailure.last_valid_sequence -lt 1) -or
            ($probeFieldsAvailable -and
                [double]$lastTransientFailure.last_valid_age_ms -ge 1000.0) -or
            ($probeFieldsAvailable -and
                [uint64]$lastAttempt.runtime_marker_last_valid_sequence -le
                    [uint64]$lastTransientFailure.last_valid_sequence) -or
            -not [string]::IsNullOrWhiteSpace(
                [string]$attemptEvidence.execution_error) -or
            ($caseOutput -join "`n") -notmatch
                "sidecar 已完成，可以松开右键") {
            $caseOutput | ForEach-Object { Write-Host $_ }
            Write-Host ($summary.pixel_evidence | ConvertTo-Json -Depth 8)
            Write-Host ($attemptEvidence | ConvertTo-Json -Depth 8)
            throw ("同一 activation 的短暂 marker 故障恢复后必须 " +
                "VALID：$CaseName/$ExpectedReason")
        }
    }

    # recording 中短暂 MISSING 或 READ_ERROR 不等于 activation 已结束；
    # 同一 session/epoch 在既有 lease 内以递增 sequence 恢复时继续。
    Assert-PixelMarkerTransientRecoveryValid "missing" "MISSING" "MISSING"
    Assert-PixelMarkerTransientRecoveryValid `
        "read-error" "READ_ERROR" "READ_ERROR"

    function Assert-PixelMarkerIdentitySwitchFails(
            [string]$CaseName,
            [string]$RestoreSessionId,
            [string]$RestoreEpoch,
            [string]$ExpectedReason) {
        $caseRoot = Join-Path $root "pixel-sidecar-$CaseName-task"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $caseRoot |
            Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "sidecar marker 身份切换负例必须可 Prepare：$CaseName"
        }
        $caseCounter = Join-Path $root "pixel-$CaseName.count"
        # 跳过夹具固定的首次 ACCESS_LOST，只隔离 marker 身份切换。
        Write-Utf8 $caseCounter "1"
        $caseEnvironment = [ordered]@{
            XEN_TEST_PIXEL_STARTED = (Join-Path $root `
                "pixel-$CaseName.started")
            XEN_TEST_PIXEL_READY = (Join-Path $root `
                "pixel-$CaseName.ready")
            XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
                "pixel-$CaseName.success")
            XEN_TEST_PIXEL_COUNTER = $caseCounter
            XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
            XEN_TEST_PIXEL_RECORDING_STARTED = (Join-Path $root `
                "pixel-$CaseName.recording")
            XEN_TEST_PIXEL_PUBLISHING_STARTED = (Join-Path $root `
                "pixel-$CaseName.publishing")
            XEN_TEST_PIXEL_RECORDING_HOLD_MS = "1800"
            XEN_TEST_PIXEL_PUBLISH_DELAY_MS = "300"
            XEN_TEST_RUNTIME_DROP_MARKER_PHASE = "RECORDING"
            XEN_TEST_RUNTIME_DROP_MARKER_GRACE_MS = "400"
            XEN_TEST_RUNTIME_RESTORE_MARKER_MS = "200"
            XEN_TEST_RUNTIME_RESTORE_MARKER_SESSION_ID = $RestoreSessionId
            XEN_TEST_RUNTIME_RESTORE_MARKER_EPOCH = $RestoreEpoch
            XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "1000"
            XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
            XEN_TEST_RUNTIME_REPORT_NAME = "pixel-$CaseName.json"
            XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
            XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
        }
        $savedEnvironment = @{}
        foreach ($name in $caseEnvironment.Keys) {
            $savedEnvironment[$name] =
                [Environment]::GetEnvironmentVariable(
                    $name, [EnvironmentVariableTarget]::Process)
            [Environment]::SetEnvironmentVariable(
                $name, [string]$caseEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
        try {
            $savedErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $caseOutput = @(& powershell.exe -NoProfile `
                -ExecutionPolicy Bypass -File `
                (Join-Path $published `
                    "tools\invoke_aim_manual_acceptance.ps1") `
                -TaskId AIM-SUPERJUMP-ACCEPT-001 `
                -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
                -Profile tracking -RequireSourceTiming `
                -CapturePixelEvidence `
                -PixelEvidenceToolRoot $pixelToolRoot `
                -PixelEvidenceBindingPath $pixelBinding `
                -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
                -PackageRoot $published -RunDirectory $caseRoot `
                -AllowPhysicalOutput `
                -PhysicalOutputConfirmation `
                    XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
            foreach ($name in $caseEnvironment.Keys) {
                [Environment]::SetEnvironmentVariable(
                    $name, $savedEnvironment[$name],
                    [EnvironmentVariableTarget]::Process)
            }
        }
        $summary = Get-Content -LiteralPath `
            (Join-Path $caseRoot "automatic-summary.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $attemptEvidence = Get-Content -LiteralPath `
            (Join-Path $caseRoot "pixel-evidence-attempts.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        $attempts = @($attemptEvidence.attempts)
        $lastAttempt = if ($attempts.Count -gt 0) {
            $attempts[-1]
        } else { $null }
        if ($attempts.Count -eq 0 -or
            [bool]$summary.pixel_evidence.gate_passed -or
            [bool]$attemptEvidence.runtime_alignment.gate_passed -or
            [bool]$lastAttempt.succeeded -or
            [bool]$lastAttempt.manifest_published -or
            [string]$lastAttempt.runtime_marker_terminal_probe.reason -ne
                $ExpectedReason -or
            [string]$attemptEvidence.execution_error -notmatch
                [regex]::Escape($ExpectedReason) -or
            (Test-Path -LiteralPath (Join-Path $caseRoot `
                "pixel-evidence\manifest.json") -PathType Leaf) -or
            ($caseOutput -join "`n") -notmatch
                "sidecar 未完成或已停止，可以松开右键") {
            $caseOutput | ForEach-Object { Write-Host $_ }
            Write-Host ($summary.pixel_evidence | ConvertTo-Json -Depth 8)
            Write-Host ($attemptEvidence | ConvertTo-Json -Depth 8)
            throw ("marker 身份切换必须立即 fail closed 且保留原因：" +
                "$CaseName/$ExpectedReason")
        }
    }

    # MISSING/READ_ERROR 的 lease 只允许同一身份恢复；session 或
    # activation epoch 变化均代表真正 activation 切换，必须立即终止。
    Assert-PixelMarkerIdentitySwitchFails `
        "session-switch" "pixel-schema13-switched" "1" "SESSION_MISMATCH"
    Assert-PixelMarkerIdentitySwitchFails `
        "epoch-switch" "" "2" "ACTIVATION_EPOCH_MISMATCH"

    # marker 在录制阶段永久消失时只能消耗既有 lease；lease 到期后即使
    # Launcher 仍存活也必须 fail closed，且不得扩大 publishing 窗口。
    $pixelRecordingLossRoot = Join-Path $root `
        "pixel-sidecar-recording-marker-loss-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelRecordingLossRoot |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sidecar 录制阶段 marker 丢失负例必须可 Prepare。"
    }
    $recordingLossEnvironment = [ordered]@{
        XEN_TEST_PIXEL_STARTED = (Join-Path $root `
            "pixel-recording-loss.started")
        XEN_TEST_PIXEL_READY = (Join-Path $root `
            "pixel-recording-loss.ready")
        XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
            "pixel-recording-loss.success")
        XEN_TEST_PIXEL_COUNTER = (Join-Path $root `
            "pixel-recording-loss.count")
        XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
        XEN_TEST_PIXEL_RECORDING_STARTED = (Join-Path $root `
            "pixel-recording-loss.recording")
        XEN_TEST_PIXEL_PUBLISHING_STARTED = (Join-Path $root `
            "pixel-recording-loss.publishing")
        XEN_TEST_PIXEL_RECORDING_HOLD_MS = "1800"
        XEN_TEST_PIXEL_PUBLISH_DELAY_MS = "300"
        XEN_TEST_RUNTIME_DROP_MARKER_PHASE = "RECORDING"
        XEN_TEST_RUNTIME_DROP_MARKER_GRACE_MS = "400"
        XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "1000"
        XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
        XEN_TEST_RUNTIME_REPORT_NAME = "pixel-recording-loss.json"
        XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
        XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
    }
    $recordingLossSavedEnvironment = @{}
    foreach ($name in $recordingLossEnvironment.Keys) {
        $recordingLossSavedEnvironment[$name] =
            [Environment]::GetEnvironmentVariable(
                $name, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            $name, [string]$recordingLossEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $recordingLossOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelRecordingLossRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        foreach ($name in $recordingLossEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name, $recordingLossSavedEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
    }
    $recordingLossSummary = Get-Content -LiteralPath `
        (Join-Path $pixelRecordingLossRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $recordingLossAttemptEvidence = Get-Content -LiteralPath `
        (Join-Path $pixelRecordingLossRoot `
            "pixel-evidence-attempts.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $recordingLossAttempts = @($recordingLossAttemptEvidence.attempts)
    $recordingLossLastAttempt = if ($recordingLossAttempts.Count -gt 0) {
        $recordingLossAttempts[-1]
    } else { $null }
    $recordingLossTerminalProbe = if ($null -ne $recordingLossLastAttempt) {
        $recordingLossLastAttempt.runtime_marker_terminal_probe
    } else { $null }
    $recordingLossTransientProbe = if (
            $null -ne $recordingLossLastAttempt) {
        $recordingLossLastAttempt.runtime_marker_last_transient_failure
    } else { $null }
    if ($recordingLossAttempts.Count -eq 0 -or
        [bool]$recordingLossSummary.pixel_evidence.gate_passed -or
        [string]$recordingLossSummary.pixel_evidence.execution_error `
            -notmatch 'MISSING_LEASE_EXPIRED' -or
        [bool]$recordingLossAttemptEvidence.runtime_alignment.gate_passed -or
        [bool]$recordingLossLastAttempt.succeeded -or
        [bool]$recordingLossLastAttempt.manifest_published -or
        [int]$recordingLossLastAttempt.runtime_marker_transient_failure_count `
            -lt 1 -or
        [string]$recordingLossTransientProbe.reason -ne "MISSING" -or
        [string]$recordingLossTerminalProbe.reason -ne
            "MISSING_LEASE_EXPIRED" -or
        [double]$recordingLossTerminalProbe.age_ms -lt 1000.0 -or
        [uint64]$recordingLossTerminalProbe.sequence -lt 1 -or
        (Test-Path -LiteralPath (Join-Path $pixelRecordingLossRoot `
            "pixel-evidence\manifest.json") -PathType Leaf) -or
        ($recordingLossOutput -join "`n") -notmatch
            "sidecar 未完成或已停止，可以松开右键") {
        $recordingLossOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($recordingLossAttemptEvidence | ConvertTo-Json -Depth 8)
        throw "marker 永久丢失必须在既有 lease 到期后终止且不发布 manifest。"
    }

    # 公开 red：绑定 sidecar 已录满并在精确 PID incoming 目录
    # 写入首张 PNG，证明已进入 publishing。此后 marker 消失不得
    # 中断无物理能力的 PNG/hash/manifest 原子发布。
    $pixelPublishingLossRoot = Join-Path $root `
        "pixel-sidecar-publishing-marker-loss-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelPublishingLossRoot |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sidecar publishing 阶段 marker 丢失正例必须可 Prepare。"
    }
    $publishingLossEnvironment = [ordered]@{
        XEN_TEST_PIXEL_STARTED = (Join-Path $root `
            "pixel-publishing-loss.started")
        XEN_TEST_PIXEL_READY = (Join-Path $root `
            "pixel-publishing-loss.ready")
        XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
            "pixel-publishing-loss.success")
        XEN_TEST_PIXEL_COUNTER = (Join-Path $root `
            "pixel-publishing-loss.count")
        XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
        XEN_TEST_PIXEL_RECORDING_STARTED = (Join-Path $root `
            "pixel-publishing-loss.recording")
        XEN_TEST_PIXEL_PUBLISHING_STARTED = (Join-Path $root `
            "pixel-publishing-loss.publishing")
        XEN_TEST_PIXEL_RECORDING_HOLD_MS = "0"
        XEN_TEST_PIXEL_PUBLISH_DELAY_MS = "1800"
        XEN_TEST_RUNTIME_DROP_MARKER_PHASE = "PUBLISHING"
        XEN_TEST_RUNTIME_DROP_MARKER_GRACE_MS = "500"
        XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "1200"
        XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
        XEN_TEST_RUNTIME_REPORT_NAME = "pixel-publishing-loss.json"
        XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
        XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
    }
    $publishingLossSavedEnvironment = @{}
    foreach ($name in $publishingLossEnvironment.Keys) {
        $publishingLossSavedEnvironment[$name] =
            [Environment]::GetEnvironmentVariable(
                $name, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            $name, [string]$publishingLossEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $publishingLossOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelPublishingLossRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        foreach ($name in $publishingLossEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name, $publishingLossSavedEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
    }
    $publishingLossSummary = Get-Content -LiteralPath `
        (Join-Path $pixelPublishingLossRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $publishingLossAttemptEvidence = Get-Content -LiteralPath `
        (Join-Path $pixelPublishingLossRoot `
            "pixel-evidence-attempts.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $publishingLossLastAttempt =
        @($publishingLossAttemptEvidence.attempts)[-1]
    $hasRecordingCompletionField =
        $publishingLossLastAttempt.PSObject.Properties.Name -contains
            "runtime_active_at_recording_completion"
    if (-not [bool]$publishingLossSummary.pixel_evidence.gate_passed -or
        [string]$publishingLossSummary.pixel_evidence.diagnostic -ne "VALID" -or
        -not [bool]$publishingLossAttemptEvidence.runtime_alignment.gate_passed -or
        -not [bool]$publishingLossLastAttempt.runtime_active_at_start -or
        -not $hasRecordingCompletionField -or
        ($hasRecordingCompletionField -and
            -not [bool]$publishingLossLastAttempt.runtime_active_at_recording_completion) -or
        [bool]$publishingLossLastAttempt.runtime_active_at_completion -or
        -not [bool]$publishingLossLastAttempt.succeeded -or
        -not [bool]$publishingLossLastAttempt.manifest_published -or
        -not [string]::IsNullOrWhiteSpace(
            [string]$publishingLossAttemptEvidence.execution_error) -or
        -not (Test-Path -LiteralPath (Join-Path $pixelPublishingLossRoot `
            "pixel-evidence\manifest.json") -PathType Leaf) -or
        ($publishingLossOutput -join "`n") -notmatch
            "sidecar 已完成，可以松开右键") {
        $publishingLossOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($publishingLossSummary.pixel_evidence |
            ConvertTo-Json -Depth 8)
        Write-Host ($publishingLossAttemptEvidence |
            ConvertTo-Json -Depth 8)
        throw "sidecar 已进入 publishing 后 marker 消失仍必须完成原子发布。"
    }

    # 公开 red：sidecar 已在当前 PID incoming 目录写入 PNG，且监督器有
    # 足够时间在前后双 marker 有效时确认 PUBLISHING。此后 Launcher 可先
    # 退出；无物理能力的 sidecar 仍必须在独立于 1 秒采集上限的有界
    # publishing 等待内完成 2.2 秒原子发布。
    $pixelPublishingLauncherExitRoot = Join-Path $root `
        "pixel-sidecar-publishing-launcher-exit-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 1 `
        -PackageRoot $published `
        -RunDirectory $pixelPublishingLauncherExitRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sidecar publishing 后 Launcher 先退出正例必须可 Prepare。"
    }
    $publishingLauncherExitEnvironment = [ordered]@{
        XEN_TEST_PIXEL_STARTED = (Join-Path $root `
            "pixel-publishing-launcher-exit.started")
        XEN_TEST_PIXEL_READY = (Join-Path $root `
            "pixel-publishing-launcher-exit.ready")
        XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
            "pixel-publishing-launcher-exit.success")
        XEN_TEST_PIXEL_COUNTER = (Join-Path $root `
            "pixel-publishing-launcher-exit.count")
        XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
        XEN_TEST_PIXEL_RECORDING_STARTED = (Join-Path $root `
            "pixel-publishing-launcher-exit.recording")
        XEN_TEST_PIXEL_PUBLISHING_STARTED = (Join-Path $root `
            "pixel-publishing-launcher-exit.publishing")
        XEN_TEST_PIXEL_RECORDING_HOLD_MS = "0"
        XEN_TEST_PIXEL_PUBLISH_DELAY_MS = "2200"
        XEN_TEST_RUNTIME_EXIT_AFTER_PUBLISHING_MS = "800"
        XEN_TEST_RUNTIME_HOLD_AFTER_SUCCESS_MS = "0"
        XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
        XEN_TEST_RUNTIME_REPORT_NAME =
            "pixel-publishing-launcher-exit.json"
        XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
        XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
    }
    $publishingLauncherExitSavedEnvironment = @{}
    foreach ($name in $publishingLauncherExitEnvironment.Keys) {
        $publishingLauncherExitSavedEnvironment[$name] =
            [Environment]::GetEnvironmentVariable(
                $name, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            $name, [string]$publishingLauncherExitEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $publishingLauncherExitOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 1 `
            -PackageRoot $published `
            -RunDirectory $pixelPublishingLauncherExitRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        foreach ($name in $publishingLauncherExitEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name, $publishingLauncherExitSavedEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
    }
    $publishingLauncherExitSummary = Get-Content -LiteralPath `
        (Join-Path $pixelPublishingLauncherExitRoot `
            "automatic-summary.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $publishingLauncherExitAttemptEvidence = Get-Content -LiteralPath `
        (Join-Path $pixelPublishingLauncherExitRoot `
            "pixel-evidence-attempts.json") -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $publishingLauncherExitLastAttempt =
        @($publishingLauncherExitAttemptEvidence.attempts)[-1]
    if (-not [bool]$publishingLauncherExitSummary.pixel_evidence.gate_passed -or
        [string]$publishingLauncherExitSummary.pixel_evidence.diagnostic -ne
            "VALID" -or
        -not [bool]$publishingLauncherExitAttemptEvidence.runtime_alignment.gate_passed -or
        -not [bool]$publishingLauncherExitLastAttempt.runtime_active_at_start -or
        -not [bool]$publishingLauncherExitLastAttempt.runtime_active_at_recording_completion -or
        [bool]$publishingLauncherExitLastAttempt.runtime_active_at_completion -or
        -not [bool]$publishingLauncherExitLastAttempt.succeeded -or
        -not [bool]$publishingLauncherExitLastAttempt.manifest_published -or
        -not [string]::IsNullOrWhiteSpace(
            [string]$publishingLauncherExitAttemptEvidence.execution_error) -or
        -not (Test-Path -LiteralPath `
            (Join-Path $pixelPublishingLauncherExitRoot `
                "pixel-evidence\manifest.json") -PathType Leaf) -or
        ($publishingLauncherExitOutput -join "`n") -notmatch
            "sidecar 已录满并进入 publishing" -or
        ($publishingLauncherExitOutput -join "`n") -notmatch
            "sidecar 已完成，可以松开右键") {
        $publishingLauncherExitOutput | ForEach-Object { Write-Host $_ }
        Write-Host ($publishingLauncherExitSummary.pixel_evidence |
            ConvertTo-Json -Depth 8)
        Write-Host ($publishingLauncherExitAttemptEvidence |
            ConvertTo-Json -Depth 8)
        throw "sidecar 已进入 PUBLISHING 后 Launcher 先退出仍必须完成原子发布。"
    }

    $pixelLifecycleTaskPath = Join-Path $pixelLifecycleRoot "task.json"
    $validPixelLifecycleTask = Get-Content -LiteralPath `
        $pixelLifecycleTaskPath -Raw -Encoding UTF8
    try {
        $wrongSourceTask = $validPixelLifecycleTask | ConvertFrom-Json
        $wrongSourceTask.capture.source = "HPSAZZ (Wrong-Output)"
        Write-Utf8 $pixelLifecycleTaskPath `
            (($wrongSourceTask | ConvertTo-Json -Depth 12) + "`n")
        $savedErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $wrongSourceRecoverOutput = @(& powershell.exe -NoProfile `
                -ExecutionPolicy Bypass -File `
                (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
                -TaskId AIM-SUPERJUMP-ACCEPT-001 `
                -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
                -Profile tracking -RequireSourceTiming `
                -CapturePixelEvidence `
                -PixelEvidenceToolRoot $pixelToolRoot `
                -PixelEvidenceBindingPath $pixelBinding `
                -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
                -PackageRoot $published -RunDirectory $pixelLifecycleRoot 2>&1)
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        if ($LASTEXITCODE -eq 0) {
            throw "Recover 必须直接拒绝被改写为非 HPSAZZ 精确源的 task。"
        }
    } finally {
        Write-Utf8 $pixelLifecycleTaskPath $validPixelLifecycleTask
    }

    $embeddedPixelBinding = Join-Path $pixelLifecycleRoot `
        "pixel-evidence\source-binding.json"
    $validEmbeddedPixelBinding = Get-Content -LiteralPath `
        $embeddedPixelBinding -Raw -Encoding UTF8
    try {
        Write-Utf8 $embeddedPixelBinding (@'
{"schema_version":2,"evidence_type":"obs_source_binding","binding_mode":"real_game","physical_output_capability":false,"ndi_main_output":{"enabled":true,"name":"Wrong-Output"}}
'@)
        $bindingMismatchRecoverOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelLifecycleRoot 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $bindingMismatchRecoverOutput | ForEach-Object { Write-Host $_ }
            throw "嵌入 binding 不匹配的 Run 仍应可无物理 Recover。"
        }
        $bindingMismatchSummary = Get-Content -LiteralPath `
            (Join-Path $pixelLifecycleRoot "automatic-summary.json") `
            -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([bool]$bindingMismatchSummary.pixel_evidence.gate_passed -or
            [string]$bindingMismatchSummary.pixel_evidence.diagnostic -ne
                "SOURCE_BINDING_MISMATCH") {
            throw "Recover 必须拒绝被改写或输出名不匹配的 Run 内嵌 OBS binding。"
        }
    } finally {
        Write-Utf8 $embeddedPixelBinding $validEmbeddedPixelBinding
    }

    $pixelLifecycleLateRoot = Join-Path $root `
        "pixel-sidecar-lifecycle-late-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelLifecycleLateRoot |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "sidecar 同窗退出测试任务必须可 Prepare。"
    }
    $lateFixtureEnvironment = [ordered]@{
        XEN_TEST_PIXEL_STARTED = (Join-Path $root `
            "pixel-lifecycle-late.started")
        XEN_TEST_PIXEL_READY = (Join-Path $root `
            "pixel-lifecycle-late.ready")
        XEN_TEST_PIXEL_SUCCESS = (Join-Path $root `
            "pixel-lifecycle-late.success")
        XEN_TEST_PIXEL_COUNTER = (Join-Path $root `
            "pixel-lifecycle-late.count")
        XEN_TEST_PIXEL_EVIDENCE_SOURCE = $pixelLifecycleEvidenceSource
        XEN_TEST_PIXEL_HOLD_AFTER_SUCCESS_MS = "1500"
        XEN_TEST_RUNTIME_REPORT_SOURCE = $pixelLifecycleReportSource
        XEN_TEST_RUNTIME_REPORT_NAME = "pixel-lifecycle-late.json"
        XEN_TEST_RUNTIME_ROOT = (Join-Path $published "cache\runtime")
        XEN_TEST_RUNTIME_SESSION_ID = "pixel-schema13"
    }
    $lateSavedFixtureEnvironment = @{}
    foreach ($name in $lateFixtureEnvironment.Keys) {
        $lateSavedFixtureEnvironment[$name] =
            [Environment]::GetEnvironmentVariable(
                $name, [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            $name, [string]$lateFixtureEnvironment[$name],
            [EnvironmentVariableTarget]::Process)
    }
    try {
        $savedErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $pixelLifecycleLateOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -RequireSourceTiming `
            -CapturePixelEvidence `
            -PixelEvidenceToolRoot $pixelToolRoot `
            -PixelEvidenceBindingPath $pixelBinding `
            -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
            -PackageRoot $published -RunDirectory $pixelLifecycleLateRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $pixelLifecycleLateOutput | ForEach-Object { Write-Host $_ }
            throw "无设备能力的 sidecar 同窗退出 Launch 夹具执行失败。"
        }
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        foreach ($name in $lateFixtureEnvironment.Keys) {
            [Environment]::SetEnvironmentVariable(
                $name, $lateSavedFixtureEnvironment[$name],
                [EnvironmentVariableTarget]::Process)
        }
    }
    $pixelLifecycleLateSummary = Get-Content -LiteralPath `
        (Join-Path $pixelLifecycleLateRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([bool]$pixelLifecycleLateSummary.pixel_evidence.gate_passed -or
        [string]$pixelLifecycleLateSummary.pixel_evidence.execution_error `
            -notmatch 'Launcher 已退出' -or
        ($pixelLifecycleLateOutput -join "`n") -notmatch
            "sidecar 未完成或已停止，可以松开右键") {
        throw "Launcher 退出前未确认 sidecar 完成时必须保留生命周期失败。"
    }

    $pixelLifecycleLateRecoverOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -RequireSourceTiming `
        -CapturePixelEvidence `
        -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding `
        -PixelEvidenceFrames 2400 -PixelEvidenceMaxSeconds 30 `
        -PackageRoot $published -RunDirectory $pixelLifecycleLateRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $pixelLifecycleLateRecoverOutput | ForEach-Object { Write-Host $_ }
        throw "sidecar 生命周期失败 Run 必须可无物理 Recover。"
    }
    $pixelLifecycleLateRecoveredSummary = Get-Content -LiteralPath `
        (Join-Path $pixelLifecycleLateRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([bool]$pixelLifecycleLateRecoveredSummary.pixel_evidence.gate_passed -or
        [string]$pixelLifecycleLateRecoveredSummary.pixel_evidence.execution_error `
            -notmatch 'Launcher 已退出') {
        throw "Recover 不得把 sidecar 生命周期失败翻转成 VALID。"
    }
    Write-Utf8 $directMlWorker "dml"

    $controlDiagnosticsTool = Join-Path $published `
        "tools\aim_control_diagnostics.ps1"
    $controlDiagnosticsBytes = [System.IO.File]::ReadAllBytes(
        $controlDiagnosticsTool)
    try {
        [System.IO.File]::AppendAllText(
            $controlDiagnosticsTool, "`n# corrupt", `
            [System.Text.UTF8Encoding]::new($false))
        $invalidControlDiagnosticsRoot = Join-Path $root `
            "invalid-task-control-diagnostics"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published `
            -RunDirectory $invalidControlDiagnosticsRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝控制诊断工具哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes(
            $controlDiagnosticsTool, $controlDiagnosticsBytes)
    }

    $nvidiaWorker = Join-Path $published "runtimes\nvidia\Xen.exe"
    Write-Utf8 $nvidiaWorker "nvidia-corrupt"
    $invalidWorkerRoot = Join-Path $root "invalid-task-worker"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $invalidWorkerRoot
    if ($LASTEXITCODE -eq 0) {
        throw "任务范围校验必须拒绝本轮使用的 NVIDIA Worker 哈希变化。"
    }
    Write-Utf8 $nvidiaWorker "nvidia"

    $modelPath = Join-Path $published "models\14wv11.onnx"
    $modelBytes = [System.IO.File]::ReadAllBytes($modelPath)
    try {
        [System.IO.File]::AppendAllText(
            $modelPath, "`ncorrupt", [System.Text.UTF8Encoding]::new($false))
        $invalidModelRoot = Join-Path $root "invalid-task-model"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published -RunDirectory $invalidModelRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝本轮使用的模型哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($modelPath, $modelBytes)
    }

    $launcherPath = Join-Path $published "XenLauncher.exe"
    $launcherBytes = [System.IO.File]::ReadAllBytes($launcherPath)
    try {
        [System.IO.File]::AppendAllText(
            $launcherPath, "`ncorrupt",
            [System.Text.UTF8Encoding]::new($false))
        $invalidLauncherRoot = Join-Path $root "invalid-task-launcher"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-LATENCY-COMP-001 `
            -Mode Prepare -Scenario Static -Profile tracking `
            -PackageRoot $published -RunDirectory $invalidLauncherRoot
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围校验必须拒绝本轮使用的 Launcher 哈希变化。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($launcherPath, $launcherBytes)
    }

    $prepareAuthorizationRoot = Join-Path $root "invalid-prepare-authorization"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Prepare -Scenario Static -Profile tracking `
        -PackageRoot $published -RunDirectory $prepareAuthorizationRoot `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
    if ($LASTEXITCODE -eq 0) {
        throw "Prepare 不得接受真实物理输出授权。"
    }

    $trackingRoot = Join-Path $root "tracking-task"
    $trackingOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -Smoothing 0.50 `
        -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
        -MaxCountsPerFrame 14.0 `
        -EnableDelayCompensation -ControlDelayMs 7.5 `
        -MaxDelayCompensationMs 18.0 `
        -MaxDelayCompensationPercent 12.0 `
        -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "tracking 任务准备失败。" }
    $trackingOutput | ForEach-Object { Write-Host $_ }
    $trackingOutputText = $trackingOutput -join "`n"
    if ($trackingOutputText -notmatch
            ('(?m)^powershell\.exe .* -TaskId AIM-SUPERJUMP-ACCEPT-001 ' +
             '-Mode Launch -Scenario SuperJump -SuperJumpCase Static ' +
             '-Profile tracking .* ' +
             '-Smoothing 0\.500000 -CountsPerPixelX 0\.450000 ' +
             '-CountsPerPixelY 0\.400000 ' +
             '-MaxCountsPerFrame 14\.000000 ' +
             '-EnableDelayCompensation ' +
             '-ControlDelayMs 7\.500000 ' +
             '-MaxDelayCompensationMs 18\.000000 ' +
             '-MaxDelayCompensationPercent 12\.000000 ' +
             '-AllowPhysicalOutput ' +
             '-PhysicalOutputConfirmation ' +
             'XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT\r?$')) {
        throw "Prepare 前台没有输出可直接复制的完整 Launch 命令。"
    }
    $trackingConfig = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "config.ini") -Raw -Encoding utf8
    if ($trackingConfig -notmatch '(?m)^backend=tensorrt\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=ndi\r?$' -or
        $trackingConfig -notmatch '(?m)^enable_prediction=false\r?$' -or
        $trackingConfig -notmatch '(?m)^smoothing=0\.500000\r?$' -or
        $trackingConfig -notmatch '(?m)^enable_delay_compensation=true\r?$' -or
        $trackingConfig -notmatch '(?m)^control_delay_ms=7\.500000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_delay_compensation_ms=18\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_delay_compensation_percent=12\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^counts_per_pixel_x=0\.450000\r?$' -or
        $trackingConfig -notmatch '(?m)^counts_per_pixel_y=0\.400000\r?$' -or
        $trackingConfig -notmatch '(?m)^max_counts_per_frame=14\.000000\r?$' -or
        $trackingConfig -notmatch '(?m)^backend=kmbox_net\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_ip=192\.168\.2\.188\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_port=13384\r?$' -or
        $trackingConfig -notmatch '(?m)^kmbox_uuid=7679E04E\r?$' -or
        $trackingConfig -notmatch '(?m)^allow_send_input=true\r?$') {
        throw "tracking 配置没有固定 NDI、TensorRT 或 KMBOX 契约。"
    }
    $trackingTask = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    if ([string]$trackingTask.task_id -ne "AIM-SUPERJUMP-ACCEPT-001" -or
        [string]$trackingTask.scenario -ne "SuperJump" -or
        [string]$trackingTask.superjump_case -ne "Static" -or
        [double]$trackingTask.aim.smoothing -ne 0.50 -or
        [double]$trackingTask.aim.counts_per_pixel_x -ne 0.45 -or
        [double]$trackingTask.aim.counts_per_pixel_y -ne 0.40 -or
        [double]$trackingTask.aim.max_counts_per_frame -ne 14.0 -or
        [bool]$trackingTask.aim.delay_compensation_enabled -ne $true -or
        [string]$trackingTask.package_validation -ne "task_scoped" -or
        [double]$trackingTask.aim.control_delay_ms -ne 7.5 -or
        [double]$trackingTask.aim.max_delay_compensation_ms -ne 18.0 -or
        [double]$trackingTask.aim.max_delay_compensation_percent -ne 12.0) {
        throw "task.json 没有固化任务 ID、平滑或延迟补偿参数。"
    }

    $superJumpCases = [ordered]@{
        Static = [ordered]@{
            root = $trackingRoot
            marker = "本 Run 唯一动作：X 静止"
        }
        SustainedMove = [ordered]@{
            root = Join-Path $root "superjump-sustained-move-task"
            marker = "本 Run 唯一动作：X 持续横移"
        }
        RandomMove = [ordered]@{
            root = Join-Path $root "superjump-random-move-task"
            marker = "本 Run 唯一动作：冲刺超级跳随机左右移动"
        }
        Stop = [ordered]@{
            root = Join-Path $root "superjump-stop-task"
            marker = "本 Run 唯一动作：X 横移后停止"
        }
        Reverse = [ordered]@{
            root = Join-Path $root "superjump-reverse-task"
            marker = "本 Run 唯一动作：X 横移后换向"
        }
    }
    $preparedManifestHashes = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($caseName in $superJumpCases.Keys) {
        $caseSpec = $superJumpCases[$caseName]
        if ($caseName -ne "Static") {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
                (Join-Path $published `
                    "tools\invoke_aim_manual_acceptance.ps1") `
                -TaskId AIM-SUPERJUMP-ACCEPT-001 `
                -Mode Prepare -Scenario SuperJump `
                -SuperJumpCase $caseName -Profile tracking -Smoothing 0.50 `
                -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
                -MaxCountsPerFrame 14.0 -EnableDelayCompensation `
                -ControlDelayMs 7.5 -MaxDelayCompensationMs 18.0 `
                -MaxDelayCompensationPercent 12.0 `
                -PackageRoot $published -RunDirectory $caseSpec.root | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "SuperJump $caseName 独立任务准备失败。"
            }
        }
        $caseFiles = @(Get-ChildItem -LiteralPath $caseSpec.root -File)
        $caseDirectories = @(Get-ChildItem -LiteralPath $caseSpec.root -Directory)
        if ($caseFiles.Count -ne 4 -or $caseDirectories.Count -ne 0 -or
            (@($caseFiles.Name | Sort-Object) -join ',') -ne
                'config.ini,OBSERVATION.md,task.json,TASK.md') {
            throw "SuperJump $caseName 必须生成严格独立的 Run 四文件。"
        }
        $caseTask = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "task.json") -Raw -Encoding utf8 |
            ConvertFrom-Json
        if ([string]$caseTask.scenario -ne "SuperJump" -or
            [string]$caseTask.superjump_case -ne $caseName) {
            throw "SuperJump $caseName task.json 没有绑定唯一动作。"
        }
        [void]$preparedManifestHashes.Add(
            [string]$caseTask.package_manifest.sha256)
        $caseTaskMarkdown = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "TASK.md") -Raw -Encoding utf8
        $caseObservation = Get-Content -LiteralPath `
            (Join-Path $caseSpec.root "OBSERVATION.md") -Raw -Encoding utf8
        foreach ($candidate in $superJumpCases.Values.marker) {
            $expectedCount = if ($candidate -eq $caseSpec.marker) { 2 } else { 0 }
            $actualCount = @(
                $caseTaskMarkdown, $caseObservation |
                    Where-Object { $_ -match [regex]::Escape($candidate) }
            ).Count
            if ($actualCount -ne $expectedCount) {
                throw "SuperJump $caseName 混入其他动作或缺少唯一动作标记：$candidate"
            }
        }
    }

    $sprintPixelRoot = Join-Path $root "superjump-random-move-pixel-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Prepare -Scenario SuperJump -SuperJumpCase RandomMove `
        -Profile tracking -Smoothing 0.50 `
        -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
        -MaxCountsPerFrame 14.0 -EnableDelayCompensation `
        -ControlDelayMs 7.5 -MaxDelayCompensationMs 18.0 `
        -MaxDelayCompensationPercent 12.0 -RequireSourceTiming `
        -CapturePixelEvidence -PixelEvidenceToolRoot $pixelToolRoot `
        -PixelEvidenceBindingPath $pixelBinding -PixelEvidenceFrames 2400 `
        -PixelEvidenceMaxSeconds 60 -PackageRoot $published `
        -RunDirectory $sprintPixelRoot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "冲刺超级跳同步像素任务准备失败。"
    }
    $sprintPixelTaskMarkdown = Get-Content -LiteralPath `
        (Join-Path $sprintPixelRoot "TASK.md") -Raw -Encoding utf8
    $sprintPixelTask = Get-Content -LiteralPath `
        (Join-Path $sprintPixelRoot "task.json") -Raw -Encoding utf8 |
        ConvertFrom-Json
    $expectedSprintHoldAction =
        "目标进入后按住右键至少 15 秒，保持冲刺超级跳和大幅 X/Y 联动；" +
        "此后继续按住，直到前台终端提示 sidecar 已完成或未完成、可以松开右键，" +
        "不人为限制方向或换向次数。"
    if ($sprintPixelTaskMarkdown -notmatch
            [regex]::Escape($expectedSprintHoldAction)) {
        throw "随机左右冲刺超级跳操作步骤必须覆盖 2400 帧 sidecar 的完整有效锁定窗口。"
    }
    $expectedRandomAction =
        "选择单个目标，人物在冲刺超级跳过程中随机向左或向右移动，方向和换向时刻不预设。"
    $expectedRandomObservation =
        "随机换向时 X 跟随是否落后、错向、过冲、细碎往返或出框"
    if ([string]$sprintPixelTask.superjump_case -ne "RandomMove" -or
        $sprintPixelTaskMarkdown -notmatch [regex]::Escape($expectedRandomAction) -or
        $sprintPixelTaskMarkdown -notmatch [regex]::Escape($expectedRandomObservation) -or
        $sprintPixelTaskMarkdown -notmatch
            [regex]::Escape("Y 起跳、腾空和落地跟随是否及时稳定") -or
        $sprintPixelTaskMarkdown -notmatch
            [regex]::Escape("X/Y 同时大幅位移时是否出现跨轴压制或方向错误") -or
        $sprintPixelTaskMarkdown -match "单方向|期间不停止、不换向") {
        throw "随机左右冲刺超级跳任务没有固化随机方向、换向与大幅 X/Y 联动语义。"
    }
    if ($sprintPixelTaskMarkdown.Contains([char]7) -or
        $sprintPixelTaskMarkdown -notmatch
            [regex]::Escape('`aim_lock_active`') -or
        $sprintPixelTaskMarkdown -notmatch
            [regex]::Escape('`physical_output_capability=false`') -or
        $sprintPixelTaskMarkdown -notmatch
            [regex]::Escape('`HPSAZZ (Xen-ROI-320)`') -or
        $sprintPixelTaskMarkdown.Contains('$ndiSourceName')) {
        throw "同步像素任务必须原样显示 marker、能力和精确 NDI 源。"
    }
    $activeManifestHash = (Get-FileHash -LiteralPath `
        (Join-Path $published "manifest.json") -Algorithm SHA256).Hash
    if ($preparedManifestHashes.Count -ne 1 -or
        -not $preparedManifestHashes.Contains($activeManifestHash)) {
        throw "全部 SuperJump Run 必须共享同一活动 manifest 身份。"
    }

    $automaticRoot = Join-Path $trackingRoot "automatic"
    New-Item -ItemType Directory -Path $automaticRoot -Force | Out-Null
    Write-Utf8 (Join-Path $automaticRoot "schema13.csv") `
        "sequence,success`n541,true`n"
    $schema13Report = [ordered]@{
        schema = 13
        session_id = "schema13"
        provider = "TensorrtExecutionProvider"
        capture_backend = "NDI"
        mouse_backend = "kmbox_net"
        sample_count = 1
        successful_samples = 1
        failed_samples = 0
        report_samples_dropped = 0
        runtime_samples_dropped = 0
        samples = @(New-Schema13AimSample)
    }
    Write-Utf8 (Join-Path $automaticRoot "schema13.json") `
        (($schema13Report | ConvertTo-Json -Depth 10) + "`n")
    $publishedManifest = Join-Path $published "manifest.json"
    $manifestBytes = [System.IO.File]::ReadAllBytes($publishedManifest)
    try {
        [System.IO.File]::AppendAllText(
            $publishedManifest, "`n", [System.Text.UTF8Encoding]::new($false))
        $recoverOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -Smoothing 0.50 `
            -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
            -MaxCountsPerFrame 14.0 `
            -EnableDelayCompensation -ControlDelayMs 7.5 `
            -MaxDelayCompensationMs 18.0 `
            -MaxDelayCompensationPercent 12.0 `
            -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $recoverOutput | ForEach-Object { Write-Host $_ }
            throw "schema 13 离线回收失败。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($publishedManifest, $manifestBytes)
    }
    $recoveredSummary = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$recoveredSummary.scenario -ne "SuperJump" -or
        -not [bool]$recoveredSummary.fixed_scene_analysis_required) {
        throw "SuperJump/Static Recover 必须保留场景并要求固定场景分析。"
    }
    if ([string]$recoveredSummary.collection_mode -ne "Recover" -or
        @($recoveredSummary.runtime_report_schemas).Count -ne 1 -or
        [int]@($recoveredSummary.runtime_report_schemas)[0] -ne 13 -or
        [uint64]$recoveredSummary.sample_count -ne 1 -or
        [uint64]$recoveredSummary.successful_samples -ne 1 -or
        [uint64]$recoveredSummary.failed_samples -ne 0 -or
        [uint64]$recoveredSummary.source_timing_valid_samples -ne 0 -or
        [string]$recoveredSummary.source_timing_diagnostic -ne
            "REPORT_FIELDS_UNAVAILABLE" -or
        $null -ne $recoveredSummary.source_clock_sample_count_max -or
        [uint64]$recoveredSummary.mouse_backend_completion_samples -ne 1 -or
        [uint64]$recoveredSummary.mouse_protocol_ack_samples -ne 0 -or
        [uint64]$recoveredSummary.mouse_physical_effect_samples -ne 0 -or
        -not [bool]$recoveredSummary.automatic_complete) {
        throw "schema 13 回收没有保留已知证据并把缺失时序层降级为 unknown。"
    }

    $schema17Sample = New-Schema13AimSample
    $schema17Sample["aim_matched_observation_valid"] = $true
    $schema17Sample["aim_matched_observation_box"] = @(
        218.163315, 154.180466, 239.103516, 215.603760)
    $schema17Sample["aim_matched_observation_head_only"] = $false
    $schema17Sample["aim_matched_observation_aim_from_head"] = $true
    $schema17Sample["aim_control_center_y"] = 160.0
    $schema13Report.schema = 17
    $schema13Report.session_id = "schema17"
    $schema13Report.samples = @($schema17Sample)
    Write-Utf8 (Join-Path $automaticRoot "schema13.json") `
        (($schema13Report | ConvertTo-Json -Depth 10) + "`n")
    $manifestBytes = [System.IO.File]::ReadAllBytes($publishedManifest)
    try {
        [System.IO.File]::AppendAllText(
            $publishedManifest, "`n", [System.Text.UTF8Encoding]::new($false))
        $schema17RecoverOutput = @(& powershell.exe -NoProfile `
            -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -Smoothing 0.50 `
            -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
            -MaxCountsPerFrame 14.0 `
            -EnableDelayCompensation -ControlDelayMs 7.5 `
            -MaxDelayCompensationMs 18.0 `
            -MaxDelayCompensationPercent 12.0 `
            -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
        if ($LASTEXITCODE -ne 0) {
            $schema17RecoverOutput | ForEach-Object { Write-Host $_ }
            throw "schema 17 离线回收失败。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($publishedManifest, $manifestBytes)
    }
    $schema17RecoveredSummary = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$schema17RecoveredSummary.scenario -ne "SuperJump" -or
        -not [bool]$schema17RecoveredSummary.fixed_scene_analysis_required -or
        @($schema17RecoveredSummary.runtime_report_schemas).Count -ne 1 -or
        [int]@($schema17RecoveredSummary.runtime_report_schemas)[0] -ne 17) {
        throw "schema 17 Recover 必须保留 SuperJump/Static 场景和固定场景分析要求。"
    }

    # schema 18 的源标识和时刻保持十进制字符串，Recover 不得浮点化原件。
    $schema18Identity = [ordered]@{
        source_sequence = "18446744073709551614"
        source_timecode = "-9223372036854775807"
        source_timestamp = "9223372036854775806"
        source_time_steady_ns = "9007199254740993"
        capture_steady_ns = "9007199254741993"
        aim_observation_steady_ns = "9007199254742993"
        control_steady_ns = "9007199254743993"
    }
    foreach ($field in $schema18Identity.Keys) {
        $schema17Sample[$field] = $schema18Identity[$field]
        $schema17Sample["${field}_valid"] = $true
    }
    $schema17Sample["source_clock_session_id"] = "18446744073709551615"
    $schema18SecondSample = [ordered]@{}
    foreach ($field in $schema17Sample.Keys) {
        $schema18SecondSample[$field] = $schema17Sample[$field]
    }
    $schema18SecondSample.sequence = 542
    $schema18SecondSample.source_sequence = "18446744073709551615"
    $schema18SecondSample.source_timestamp = "9223372036854775807"
    $schema13Report.schema = 18
    $schema13Report.session_id = "schema18"
    $schema13Report.sample_count = 2
    $schema13Report.successful_samples = 2
    $schema13Report.samples = @($schema17Sample, $schema18SecondSample)
    $schema18Path = Join-Path $automaticRoot "schema13.json"
    Write-Utf8 (Join-Path $automaticRoot "schema13.csv") `
        "sequence,success`n541,true`n542,true`n"
    Write-Utf8 $schema18Path `
        (($schema13Report | ConvertTo-Json -Depth 10) + "`n")
    $schema18OriginalHash = (Get-FileHash -LiteralPath $schema18Path `
        -Algorithm SHA256).Hash
    $schema18RecoverOutput = @(& powershell.exe -NoProfile `
        -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-SUPERJUMP-ACCEPT-001 `
        -Mode Recover -Scenario SuperJump -SuperJumpCase Static `
        -Profile tracking -Smoothing 0.50 `
        -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
        -MaxCountsPerFrame 14.0 `
        -EnableDelayCompensation -ControlDelayMs 7.5 `
        -MaxDelayCompensationMs 18.0 -MaxDelayCompensationPercent 12.0 `
        -PackageRoot $published -RunDirectory $trackingRoot 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $schema18RecoverOutput | ForEach-Object { Write-Host $_ }
        throw "schema 18 离线回收失败。"
    }
    $schema18Summary = Get-Content -LiteralPath `
        (Join-Path $trackingRoot "automatic-summary.json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]$schema18Summary.collection_mode -ne "Recover" -or
        @($schema18Summary.runtime_report_schemas).Count -ne 1 -or
        [int]@($schema18Summary.runtime_report_schemas)[0] -ne 18 -or
        -not [bool]$schema18Summary.automatic_complete -or
        [uint64]$schema18Summary.sample_count -ne 2 -or
        [uint64]$schema18Summary.mouse_physical_effect_samples -ne 0 -or
        (Get-FileHash -LiteralPath $schema18Path -Algorithm SHA256).Hash -ne
            $schema18OriginalHash) {
        throw "schema 18 Recover 必须保留原件和独立物理效果证据边界。"
    }
    $schema18Preserved = Get-Content -LiteralPath $schema18Path `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($field in $schema18Identity.Keys) {
        $value = $schema18Preserved.samples[0].$field
        if ($value -isnot [string] -or $value -cne $schema18Identity[$field]) {
            throw "schema 18 Recover 丢失 64 位标识/时刻字符串：$field"
        }
    }
    foreach ($sample in $schema18Preserved.samples) {
        if ($sample.source_clock_session_id -isnot [string] -or
            $sample.source_clock_session_id -cne "18446744073709551615") {
            throw "schema 18 Recover 丢失超过浮点精度的 clock session 字符串。"
        }
    }
    if ($schema18Preserved.samples[1].source_timestamp -isnot [string] -or
        $schema18Preserved.samples[1].source_timestamp -cne "9223372036854775807" -or
        ([int64]$schema18Preserved.samples[1].source_timestamp -
            [int64]$schema18Preserved.samples[0].source_timestamp) -ne 1) {
        throw "schema 18 Recover 合并了相邻的 64 位 source timestamp。"
    }

    $manifestBytes = [System.IO.File]::ReadAllBytes($publishedManifest)
    try {
        [System.IO.File]::AppendAllText(
            $publishedManifest, "`n", [System.Text.UTF8Encoding]::new($false))
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
            -TaskId AIM-SUPERJUMP-ACCEPT-001 `
            -Mode Launch -Scenario SuperJump -SuperJumpCase Static `
            -Profile tracking -Smoothing 0.50 `
            -CountsPerPixelX 0.45 -CountsPerPixelY 0.40 `
            -MaxCountsPerFrame 14.0 `
            -EnableDelayCompensation -ControlDelayMs 7.5 `
            -MaxDelayCompensationMs 18.0 `
            -MaxDelayCompensationPercent 12.0 `
            -PackageRoot $published -RunDirectory $trackingRoot `
            -AllowPhysicalOutput `
            -PhysicalOutputConfirmation `
                XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
        if ($LASTEXITCODE -eq 0) {
            throw "任务范围 Launch 必须拒绝 Prepare 后变化的 manifest。"
        }
    } finally {
        [System.IO.File]::WriteAllBytes($publishedManifest, $manifestBytes)
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -TaskId AIM-LATENCY-COMP-001 `
        -Mode Launch -Scenario Static -Profile tracking -Smoothing 0.35 `
        -CountsPerPixel 0.55 `
        -EnableDelayCompensation -ControlDelayMs 7.5 `
        -MaxDelayCompensationMs 18.0 `
        -MaxDelayCompensationPercent 12.0 `
        -PackageRoot $published -RunDirectory $trackingRoot `
        -AllowPhysicalOutput `
        -PhysicalOutputConfirmation XEN_AIM_DUAL_ACCEPT_SENDS_REAL_KMBOX_INPUT
    if ($LASTEXITCODE -eq 0) {
        throw "Launch 不应接受与 Prepare 快照不一致的 smoothing 参数。"
    }

    $predictionRoot = Join-Path $root "prediction-task"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Prepare -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -ne 0) { throw "prediction 任务准备失败。" }
    $predictionConfig = Get-Content -LiteralPath `
        (Join-Path $predictionRoot "config.ini") -Raw -Encoding utf8
    if ($predictionConfig -notmatch '(?m)^enable_prediction=true\r?$' -or
        $predictionConfig -notmatch `
            '(?m)^max_prediction_lead_percent=35\.000000\r?$') {
        throw "prediction 配置没有启用已验证的预测边界。"
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $published "tools\invoke_aim_manual_acceptance.ps1") `
        -Mode Launch -Scenario Static -Profile prediction `
        -PackageRoot $published -RunDirectory $predictionRoot
    if ($LASTEXITCODE -eq 0) {
        throw "缺少物理输出双重授权时 Launch 不应成功。"
    }
    Write-Host "完整包传输、任务范围校验、Aim 配置生成和授权拒绝回归通过。"
} finally {
    if (Test-Path -LiteralPath $root) {
        for ($attempt = 1; $attempt -le 150; ++$attempt) {
            try {
                Remove-XenOwnedTestDirectory -RootPath $root `
                    -BasePath $ownedTest.BasePath `
                    -RepositoryRoot $repositoryRoot `
                    -OwnerId $ownedTest.OwnerId
                break
            } catch {
                if ($attempt -eq 150) {
                    throw "测试夹具仍被进程占用，无法清理：$root"
                }
                Start-Sleep -Milliseconds 100
            }
        }
    }
}
