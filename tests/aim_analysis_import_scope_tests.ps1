param(
    [string]$ScriptsRoot = (Join-Path $PSScriptRoot "..\scripts")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ReportPath = @("caller-report")
$RunDirectory = "caller-run"
$Scenario = "caller-scenario"
$OutputPath = "caller-output"
$RequireXStability = "caller-require-x"
$ErrorActionPreference = "Continue"
$before = [ordered]@{
    report_path = @($ReportPath)
    run_directory = $RunDirectory
    scenario = $Scenario
    output_path = $OutputPath
    require_x_stability = $RequireXStability
    error_action_preference = [string]$ErrorActionPreference
}
$modulePaths = @(
    (Join-Path $ScriptsRoot "aim_report.ps1"),
    (Join-Path $ScriptsRoot "aim_control_diagnostics.ps1"),
    (Join-Path $ScriptsRoot "aim_fixed_scene_analysis.ps1"))
$moduleNames = @(
    "XenAimReportTest",
    "XenAimControlDiagnosticsTest",
    "XenAimFixedSceneAnalysisTest")

function Import-XenAimAnalysisTestModule(
        [string]$Path,
        [string]$Name) {
    $module = New-Module -Name $Name -ArgumentList $Path -ScriptBlock {
        param([string]$AnalysisScriptPath)
        . $AnalysisScriptPath
    }
    Import-Module $module -Force -Scope Local -ErrorAction Stop
}

try {
    for ($index = 0; $index -lt $modulePaths.Count; ++$index) {
        Import-XenAimAnalysisTestModule $modulePaths[$index] `
            $moduleNames[$index]
    }
    $after = [ordered]@{
        report_path = @($ReportPath)
        run_directory = $RunDirectory
        scenario = $Scenario
        output_path = $OutputPath
        require_x_stability = $RequireXStability
        error_action_preference = [string]$ErrorActionPreference
    }
    if (($before | ConvertTo-Json -Compress) -cne
        ($after | ConvertTo-Json -Compress)) {
        throw "Analysis module import mutated caller variables. Before=$($before | ConvertTo-Json -Compress); After=$($after | ConvertTo-Json -Compress)"
    }
    $legacySnapshot = & {
        param([string]$FixedSceneScript)
        $ReportPath = @("legacy-report")
        $RunDirectory = "legacy-run"
        $Scenario = "legacy-scenario"
        $OutputPath = "legacy-output"
        $RequireXStability = $true
        $legacyBefore = "{0}|{1}|{2}|{3}|{4}" -f `
            ($ReportPath -join ","), $RunDirectory, $Scenario, $OutputPath,
            $RequireXStability
        . $FixedSceneScript -Scenario "import-argument"
        $legacyAfter = "{0}|{1}|{2}|{3}|{4}" -f `
            ($ReportPath -join ","), $RunDirectory, $Scenario, $OutputPath,
            $RequireXStability
        return [pscustomobject]@{
            Before = $legacyBefore
            After = $legacyAfter
            LeakedVariables = @(
                Get-Variable -Name "XenFixedScene*" `
                    -ErrorAction SilentlyContinue).Count
        }
    } $modulePaths[2]
    if ($legacySnapshot.Before -cne $legacySnapshot.After -or
        $legacySnapshot.LeakedVariables -ne 0) {
        throw "Legacy fixed-scene import mutated caller variables."
    }
    foreach ($command in @(
            "Get-XenAimReportSummary",
            "Get-XenSourceTimingEvidence",
            "Get-XenAimDistributionSummary",
            "Get-XenAimControlDiagnosticsSummary",
            "Get-XenAimFixedSceneAnalysis")) {
        if ($null -eq (Get-Command $command -CommandType Function `
                -ErrorAction SilentlyContinue)) {
            throw "Analysis module import did not expose $command."
        }
    }
    Write-Host "Aim analysis module import preserved caller scope."
} finally {
    foreach ($moduleName in $moduleNames) {
        Remove-Module $moduleName -Force -ErrorAction SilentlyContinue
    }
}
