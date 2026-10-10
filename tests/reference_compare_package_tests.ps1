param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$GitExecutable = 'git', [string]$TestRoot = (Join-Path ([IO.Path]::GetTempPath()) 'xen-reference-package-tests'))
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $RepositoryRoot 'scripts/path_safety.psm1') -Force
$owned = New-XenOwnedTestDirectory -BasePath $TestRoot -RepositoryRoot $RepositoryRoot
try {
    $repository = Join-Path $owned.RootPath 'repo'
    $scripts = Join-Path $repository 'scripts'
    $release = Join-Path $owned.RootPath 'build/Release'
    foreach ($directory in @($scripts, $release, (Join-Path $repository 'assets/reference_assessment'))) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    foreach ($name in @('package_reference_compare.ps1', 'build_artifact_identity.psm1', 'path_safety.psm1')) {
        Copy-Item -LiteralPath (Join-Path $RepositoryRoot "scripts/$name") -Destination $scripts
    }
    foreach ($name in @('example.json', 'GUIDE.md', 'LICENSE.cs-match-hud.txt', 'UPSTREAM.json')) {
        [IO.File]::WriteAllText((Join-Path $repository "assets/reference_assessment/$name"), 'fixture-resource')
    }
    & $GitExecutable -C $repository init --quiet
    & $GitExecutable -C $repository add --all
    & $GitExecutable -C $repository -c user.name=XenTest -c user.email=xen-test@example.invalid commit --quiet -m fixture
    if ($LASTEXITCODE -ne 0) { throw 'Fixture commit failed' }
    $commit = (& $GitExecutable -C $repository rev-parse HEAD).Trim()
    foreach ($name in @('XenReferenceCompare.exe', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        [IO.File]::WriteAllText((Join-Path $release $name), "fixture-$name")
    }
    $build = Split-Path -Parent $release
    @{ schema = 1; git_commit = $commit; git_dirty = $false; runtime = 'nvidia'; source_root = $repository } |
        ConvertTo-Json | Set-Content (Join-Path $build 'xen-build-identity.json')
    $artifact = Join-Path $release 'XenReferenceCompare.exe'
    $stamp = @{ schema = 1; git_commit = $commit; git_dirty = $false; runtime = 'nvidia'; source_root = $repository;
        configuration = 'Release'; artifact = 'XenReferenceCompare.exe'; size = (Get-Item $artifact).Length;
        sha256 = (Get-FileHash $artifact).Hash.ToLowerInvariant() }
    $stampPath = $artifact + '.identity.json'
    $validStamp = $stamp | ConvertTo-Json
    $publisher = Join-Path $scripts 'package_reference_compare.ps1'
    foreach ($mutation in @(
        @{ field = 'git_commit'; value = ('b' * 40) }, @{ field = 'source_root'; value = $owned.RootPath },
        @{ field = 'configuration'; value = 'Debug' }, @{ field = 'git_dirty'; value = $true })) {
        $invalid = $validStamp | ConvertFrom-Json
        $invalid.($mutation.field) = $mutation.value
        [IO.File]::WriteAllText($stampPath, ($invalid | ConvertTo-Json))
        $output = Join-Path $owned.RootPath ('invalid-' + $mutation.field)
        $rejected = $false
        try { & $publisher -BuildDirectory $build -OutputDirectory $output | Out-Null }
        catch { $rejected = $_.Exception.Message -like '*Linked artifact identity*' }
        if (-not $rejected -or (Test-Path $output)) { throw "F04 reference package accepted invalid linked $($mutation.field)" }
    }
    [IO.File]::WriteAllText($stampPath, $validStamp)
    Remove-Item -LiteralPath $stampPath
    $output = Join-Path $owned.RootPath 'missing-stamp'
    $rejected = $false
    try { & $publisher -BuildDirectory $build -OutputDirectory $output | Out-Null } catch { $rejected = $true }
    if (-not $rejected -or (Test-Path $output)) { throw 'F04 reference package accepted missing linked stamp' }
    [IO.File]::WriteAllText($stampPath, $validStamp)
    $original = [IO.File]::ReadAllText($artifact)
    [IO.File]::WriteAllText($artifact, 'replaced-after-link')
    $output = Join-Path $owned.RootPath 'changed-payload'
    $rejected = $false
    try { & $publisher -BuildDirectory $build -OutputDirectory $output | Out-Null } catch { $rejected = $true }
    if (-not $rejected -or (Test-Path $output)) { throw 'F04 reference package accepted changed EXE' }
    [IO.File]::WriteAllText($artifact, $original)
    $output = Join-Path $owned.RootPath 'valid-package'
    & $publisher -BuildDirectory $build -OutputDirectory $output | Out-Null
    $manifest = Get-Content (Join-Path $output 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.git_commit -cne $commit -or $manifest.physical_output -ne $false) { throw 'Reference package evidence changed' }
    foreach ($file in $manifest.files) {
        if ((Get-FileHash (Join-Path $output $file.name)).Hash -cne $file.sha256) { throw 'Reference manifest hash mismatch' }
    }
    Write-Host 'F04 reference package: 6 rejection cases and valid payload PASS'
} finally {
    Remove-XenOwnedTestDirectory -RootPath $owned.RootPath -BasePath $owned.BasePath `
        -RepositoryRoot $RepositoryRoot -OwnerId $owned.OwnerId
}
