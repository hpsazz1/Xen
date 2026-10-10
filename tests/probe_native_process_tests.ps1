param(
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [string]$SequenceExecutable,
    [string]$HelperModule,
    [switch]$IoDiagnosticOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
if (-not $HelperModule) {
    $HelperModule = Join-Path $PSScriptRoot '../scripts/probe_native_process.psm1'
}

$base = [IO.Path]::GetFullPath($TestRoot).TrimEnd('\')
$owner = [Guid]::NewGuid().ToString('N')
$root = Join-Path $base ('case-' + $owner)
[void][IO.Directory]::CreateDirectory($root)
$deep = $root
while ($deep.Length -lt 290) {
    $deep += '\deep-space-' + [char]0x4e2d + '-0123456789'
}
$extended = '\\?\' + $deep
$results = @()
function Check-Operation([string]$Name, [scriptblock]$Action) {
    try {
        & $Action | Out-Null
        $script:results += [pscustomobject]@{ operation = $Name; success = $true }
    } catch {
        $script:results += [pscustomobject]@{
            operation = $Name; success = $false; error = $_.Exception.Message
        }
    }
}
Check-Operation 'Directory.CreateDirectory extended' {
    [void][IO.Directory]::CreateDirectory($extended)
}
Check-Operation 'File.WriteAllText extended' {
    [IO.File]::WriteAllText($extended + '\text.txt', 'fixture ' + [char]0x4e2d,
        [Text.UTF8Encoding]::new($false))
}
Check-Operation 'File.ReadAllText extended' {
    if ([IO.File]::ReadAllText($extended + '\text.txt') -cne ('fixture ' + [char]0x4e2d)) {
        throw 'Text round trip mismatch'
    }
}
Check-Operation 'Get-Content ordinary' {
    $null = Get-Content -LiteralPath ($deep + '\text.txt') -Raw -Encoding utf8
}
Check-Operation 'Get-Content extended' {
    $null = Get-Content -LiteralPath ($extended + '\text.txt') -Raw -Encoding utf8
}
Check-Operation 'Get-Item ordinary' { $null = Get-Item -LiteralPath ($deep + '\text.txt') }
Check-Operation 'Get-Item extended' { $null = Get-Item -LiteralPath ($extended + '\text.txt') }
Check-Operation 'File.ReadAllText ordinary' { $null = [IO.File]::ReadAllText($deep + '\text.txt') }
Check-Operation 'Directory.Move extended' {
    [IO.Directory]::Move($extended, $extended + '-moved')
    [IO.Directory]::Move($extended + '-moved', $extended)
}
Check-Operation 'File.Delete extended' { [IO.File]::Delete($extended + '\text.txt') }
# Only the exact direct GUID child created above belongs to this invocation.
if ([IO.Path]::GetDirectoryName($root) -cne $base -or
    [IO.Path]::GetFileName($root) -cne ('case-' + $owner)) {
    throw 'Invalid diagnostic cleanup ownership'
}
Check-Operation 'Directory.Delete extended owned root' {
    [IO.Directory]::Delete('\\?\' + $root, $true)
}
[pscustomobject]@{
    powershell = $PSVersionTable.PSVersion.ToString()
    deep_path_length = $deep.Length
    root = $root
    results = $results
} | ConvertTo-Json -Depth 5
if ($IoDiagnosticOnly) { return }
if (@($results | Where-Object { -not $_.success }).Count -ne 0) {
    throw 'PS5.1 long-path I/O prerequisites failed'
}

Import-Module $HelperModule -ErrorAction Stop
if (-not $SequenceExecutable) { throw 'SequenceExecutable is required for the real deep native EXE regression' }
function Get-FixtureHash([string]$Path) {
    $stream = [IO.File]::OpenRead('\\?\' + [IO.Path]::GetFullPath($Path))
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)) }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
$testOwner = New-XenProbeStagingDirectory -Parent $base -Path (
    Join-Path $base ('.native-tests.incoming-' + [Guid]::NewGuid().ToString('N')))
try {
    $fixture = Join-Path $testOwner.Path 'arguments.exe'
    Add-Type -OutputAssembly $fixture -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Text;
using System.Threading;
public static class NativeArgumentsFixture {
    public static int Main(string[] args) {
        if (args.Length > 1 && args[1] == "sleep") Thread.Sleep(5000);
        else Thread.Sleep(150);
        if (args.Length > 1 && args[1] == "flood") {
            for (int i = 0; i < 2000; ++i) {
                Console.Out.WriteLine(new string('o', 100));
                Console.Error.WriteLine(new string('e', 100));
            }
        }
        foreach (string arg in args)
            Console.WriteLine("ARG:" + Convert.ToBase64String(Encoding.UTF8.GetBytes(arg)));
        Console.Error.WriteLine("fixture-stderr");
        return Int32.Parse(args[0]);
    }
}
'@
    $deepExecutableDirectory = $testOwner.Path
    while ($deepExecutableDirectory.Length -lt 290) {
        $deepExecutableDirectory += '\space-' + [char]0x4e2d + '-0123456789'
    }
    [void][IO.Directory]::CreateDirectory('\\?\' + $deepExecutableDirectory)
    $deepExecutable = $deepExecutableDirectory + '\arguments.exe'
    [IO.File]::Copy([IO.Path]::GetFullPath($SequenceExecutable), '\\?\' + $deepExecutable, $false)
    if ((Get-FixtureHash $SequenceExecutable) -cne (Get-FixtureHash $deepExecutable)) {
        throw 'Frozen deep native executable hash differs from its source'
    }
    $expectedArguments = @('37', 'space value', ([string][char]0x4e2d + [char]0x6587),
        '', 'quote"inside', 'tail\', 'before\"quote', '\\', ' ')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    # The managed CLR fixture has its own executable-path limit. Use it only for
    # argv semantics; the frozen native Sequence below exercises the real deep EXE.
    $result = Invoke-XenProbeNativeProcess -Executable $fixture -Arguments $expectedArguments
    if ($result.ExitCode -ne 37 -or $watch.ElapsedMilliseconds -lt 100 -or
        -not $result.StandardError.Contains('fixture-stderr')) {
        throw 'Native wait, exit code, or stderr regression'
    }
    $actualArguments = @($result.StandardOutput -split '\r?\n' | Where-Object {
        $_.StartsWith('ARG:')
    } | ForEach-Object { [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($_.Substring(4))) })
    if ($actualArguments.Count -ne $expectedArguments.Count) { throw 'Native argument count mismatch' }
    for ($index = 0; $index -lt $actualArguments.Count; $index++) {
        if ($actualArguments[$index] -cne $expectedArguments[$index]) { throw "Native argv mismatch at $index" }
    }
    $zero = Invoke-XenProbeNativeProcess -Executable $fixture -Arguments @('0')
    if ($zero.ExitCode -ne 0) { throw 'Native zero exit code mismatch' }
    $flood = Invoke-XenProbeNativeProcess -Executable $fixture -Arguments @('0', 'flood')
    if ($flood.StandardOutput.Length -lt 200000 -or $flood.StandardError.Length -lt 200000) {
        throw 'Native output pipes were not drained'
    }
    $missingRejected = $false
    try { Invoke-XenProbeNativeProcess -Executable ($deepExecutable + '.missing') | Out-Null }
    catch { $missingRejected = $true }
    if (-not $missingRejected) { throw 'Missing executable accepted' }
    $invalidExecutable = $deepExecutableDirectory + '\not-an-executable.exe'
    [IO.File]::WriteAllText('\\?\' + $invalidExecutable, 'fixture, not an executable')
    $invalidRejected = $false
    try { Invoke-XenProbeNativeProcess -Executable $invalidExecutable | Out-Null }
    catch { $invalidRejected = $_.Exception.ToString().Contains('CreateProcessW failed') }
    if (-not $invalidRejected) { throw 'Native process creation error was not preserved' }
    $timeoutRejected = $false
    try { Invoke-XenProbeNativeProcess -Executable $fixture -Arguments @('0', 'sleep') -TimeoutMilliseconds 50 | Out-Null }
    catch { $timeoutRejected = $_.Exception.ToString().Contains('time limit') }
    if (-not $timeoutRejected) { throw 'Native process timeout was not reported' }
    $nonzeroRejected = $false
    try { Invoke-XenProbeNativeTool -Executable $fixture -Arguments @('37') -Description 'fixture' }
    catch { $nonzeroRejected = $_.ToString().Contains('ExitCode=37') -and $_.ToString().Contains('fixture-stderr') }
    if (-not $nonzeroRejected) { throw 'Nonzero process diagnostics were lost' }

    $nativeHelp = Invoke-XenProbeNativeProcess -Executable $deepExecutable -Arguments @('--help')
    if ($nativeHelp.ExitCode -ne 0 -or -not $nativeHelp.StandardOutput.Contains('XenMouseEffectProbeSequence')) {
        throw 'Real deep native EXE help failed'
    }
    $nativeBad = Invoke-XenProbeNativeProcess -Executable $deepExecutable -Arguments @('--invalid', 'value')
    if ($nativeBad.ExitCode -eq 0 -or -not $nativeBad.StandardError) {
        throw 'Real deep native EXE argument failure was lost'
    }
    $outputPath = $deepExecutableDirectory + '\sequence space-' + [char]0x6587 + '.json'
    $nativeArguments = @('--output', $outputPath, '--baseline-samples', '64',
        '--response-samples', '48', '--guard-samples', '32')
    $native = Invoke-XenProbeNativeProcess -Executable $deepExecutable -Arguments $nativeArguments
    if ($native.ExitCode -ne 0 -or -not [IO.File]::Exists('\\?\' + $outputPath)) {
        throw "Real deep native EXE generation failed: $($native.StandardError)"
    }
    $beforeOutput = [IO.File]::ReadAllText('\\?\' + $outputPath)
    $existingOutput = Invoke-XenProbeNativeProcess -Executable $deepExecutable -Arguments $nativeArguments
    if ($existingOutput.ExitCode -eq 0 -or [IO.File]::ReadAllText('\\?\' + $outputPath) -cne $beforeOutput) {
        throw 'Real deep native EXE overwrote an existing output'
    }

    $stage = New-XenProbeStagingDirectory -Parent $deepExecutableDirectory -Path (
        $deepExecutableDirectory + '\.prepare.incoming-' + [Guid]::NewGuid().ToString('N'))
    [IO.File]::WriteAllText('\\?\' + $stage.Path + '\frozen.txt', 'frozen')
    $existingRejected = $false
    try { New-XenProbeStagingDirectory -Parent $deepExecutableDirectory -Path $stage.Path | Out-Null }
    catch { $existingRejected = $true }
    if (-not $existingRejected) { throw 'Existing staging was adopted' }
    $forged = [pscustomobject]@{ Path = $stage.Path; Parent = $stage.Parent; Name = $stage.Name }
    $forgedRejected = $false
    try { Remove-XenProbeStagingDirectory $forged }
    catch { $forgedRejected = $true }
    if (-not $forgedRejected -or -not [IO.File]::Exists('\\?\' + $stage.Path + '\frozen.txt')) {
        throw 'Forged staging ownership was accepted'
    }
    $destination = $deepExecutableDirectory + '\published'
    [void][IO.Directory]::CreateDirectory('\\?\' + $destination)
    [IO.File]::WriteAllText('\\?\' + $destination + '\existing.txt', 'existing')
    $publishRejected = $false
    try { Complete-XenProbeStagingDirectory $stage $destination }
    catch { $publishRejected = $true }
    if (-not $publishRejected -or [IO.File]::ReadAllText('\\?\' + $destination + '\existing.txt') -cne 'existing') {
        throw 'Existing published directory was changed'
    }
    $existingFile = $deepExecutableDirectory + '\published-file'
    [IO.File]::WriteAllText('\\?\' + $existingFile, 'existing-file')
    $fileRejected = $false
    try { Complete-XenProbeStagingDirectory $stage $existingFile }
    catch { $fileRejected = $true }
    if (-not $fileRejected -or [IO.File]::ReadAllText('\\?\' + $existingFile) -cne 'existing-file') {
        throw 'Existing published file was changed'
    }
    $held = [IO.File]::Open('\\?\' + $stage.Path + '\frozen.txt',
        [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        $primaryPreserved = $false
        try {
            try { throw 'primary-fixture-failure' }
            catch {
                $failure = $_
                Undo-XenProbeStagingDirectory $stage $failure
                throw $failure
            }
        } catch { $primaryPreserved = $_.ToString().Contains('primary-fixture-failure') }
        if (-not $primaryPreserved -or -not [IO.Directory]::Exists('\\?\' + $stage.Path)) {
            throw 'Cleanup hid the primary error or deleted a locked staging'
        }
    } finally { $held.Dispose() }
    Remove-XenProbeStagingDirectory $stage
    if ([IO.Directory]::Exists('\\?\' + $stage.Path)) { throw 'Owned staging was not removed' }
    $ready = New-XenProbeStagingDirectory -Parent $deepExecutableDirectory -Path (
        $deepExecutableDirectory + '\.ready.incoming-' + [Guid]::NewGuid().ToString('N'))
    [IO.File]::WriteAllText('\\?\' + $ready.Path + '\frozen.txt', 'ready')
    $published = $deepExecutableDirectory + '\complete'
    Complete-XenProbeStagingDirectory $ready $published
    if ([IO.Directory]::Exists('\\?\' + $ready.Path) -or
        [IO.File]::ReadAllText('\\?\' + $published + '\frozen.txt') -cne 'ready') {
        throw 'Owned staging publication did not preserve its complete contents'
    }
    $oldOwnerRejected = $false
    try { Remove-XenProbeStagingDirectory $ready }
    catch { $oldOwnerRejected = $true }
    if (-not $oldOwnerRejected -or -not [IO.File]::Exists('\\?\' + $published + '\frozen.txt')) {
        throw 'An old staging owner deleted published content'
    }
    Write-Host "Native helper passed: PowerShell=$($PSVersionTable.PSVersion); deep_executable_length=$($deepExecutable.Length); argv=$($expectedArguments.Count)"
} finally {
    Remove-XenProbeStagingDirectory $testOwner
}
