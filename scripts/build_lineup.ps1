param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build/lineup'),
    [string]$DependencyCache = (Join-Path $PSScriptRoot '../build/nvidia/CMakeCache.txt'),
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)
$cache = @{}
foreach ($line in Get-Content -LiteralPath $DependencyCache) {
    if ($line -match '^([^/#][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
if ($buildRoot -eq [IO.Path]::GetFullPath((Split-Path -Parent $DependencyCache))) {
    throw 'Lineup needs its own build directory; the dependency cache is read-only.'
}
$existingCache = Join-Path $buildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $existingCache) {
    $source = Select-String -LiteralPath $existingCache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$'
    if (-not $source -or [IO.Path]::GetFullPath($source.Matches[0].Groups[1].Value) -ne $repositoryRoot) {
        throw 'Build directory belongs to another source tree.'
    }
}
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source
$arguments = @('-S', $repositoryRoot, '-B', $buildRoot, '-G', $cache['CMAKE_GENERATOR'], '-A', 'x64',
    '-DBUILD_TESTING=ON', '-DXEN_LINEUP_ONLY_RUNTIME=ON', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON')
foreach ($name in @('ONNXRUNTIME_ROOT', 'OpenCV_DIR', 'XEN_CUDA_ROOT', 'XEN_CUDNN_ROOT',
        'XEN_TENSORRT_ROOT', 'XEN_TENSORRT_MAJOR', 'XEN_MSVC_REDIST_ROOT', 'XEN_NDI_SDK_ROOT',
        'XEN_PYTHON_EXECUTABLE', 'XEN_POWERSHELL7_EXECUTABLE', 'CMAKE_CUDA_ARCHITECTURES')) {
    if ($cache.ContainsKey($name) -and $cache[$name]) { $arguments += "-D${name}=$($cache[$name])" }
}
foreach ($dependency in @('IMGUI', 'NLOHMANN_JSON', 'SIMPLEINI', 'SPDLOG')) {
    $name = "FETCHCONTENT_SOURCE_DIR_$dependency"
    $source = $cache[$name]
    if (-not $source -or -not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Existing local dependency source is missing: $name"
    }
    $arguments += "-D${name}=$source"
}
& $cmake @arguments
if ($LASTEXITCODE -ne 0) { throw "Lineup configure failed: $LASTEXITCODE" }
if ($ConfigureOnly) { return }
& $cmake --build $buildRoot --config Release --target lineup_validation --parallel
if ($LASTEXITCODE -ne 0) { throw "Lineup build failed: $LASTEXITCODE" }
$nodeCache = Select-String -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Pattern '^XEN_NODE_EXECUTABLE:FILEPATH=(.*)$'
if (-not $nodeCache -or -not (Test-Path -LiteralPath $nodeCache.Matches[0].Groups[1].Value -PathType Leaf)) { throw '缺少已配置的 Node 运行时，无法生成离线采集选项。' }
& $nodeCache.Matches[0].Groups[1].Value (Join-Path $PSScriptRoot 'build_lineup_capture_options.cjs') --output (Join-Path $buildRoot 'Release/lineup-capture-options.json')
if ($LASTEXITCODE -ne 0) { throw '离线采集选项生成失败。' }
$originalPath = $env:PATH
try {
    $env:PATH = @((Join-Path $env:SystemRoot 'System32'), $env:SystemRoot) -join ';'
    & $ctest --test-dir $buildRoot -C Release -R '^(lineup_calibration_tests|lineup_calibration_tool_tests|lineup_control_ipc_tests|lineup_action_tests|lineup_execution_tests|lineup_config_tests|lineup_practice_tests|lineup_increment_tests|review_run_tests|lineup_tests|lineup_service_tests|lineup_http_tests|lineup_gsi_http_tests|gsi_context_sharing_tests|gsi_context_contract_tests|lineup_web_tests|ndi_capture_contract_tests|capture_evidence_tests|weapon_tests|weapon_timing_tests)$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Lineup tests failed: $LASTEXITCODE" }
    & $ctest --test-dir $buildRoot -C Release -R '^(lineup_host_capture_tests|lineup_host_service_tests|lineup_publication_tests|runtime_lineup_test_execution_tests|runtime_lineup_bridge_tests|lineup_execution_chain_tests|lineup_test_launcher_tests|lineup_capture_panel_tests|lineup_capture_transfer_tests|lineup_capture_shortcut_tests|config_tests)$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "主机采集与独立快捷键测试失败：$LASTEXITCODE" }
} finally { $env:PATH = $originalPath }
Write-Host "Lineup Release artifacts: $(Join-Path $buildRoot 'Release')"
Write-Host 'No capture service, game, remote transfer or publishing was started.'
