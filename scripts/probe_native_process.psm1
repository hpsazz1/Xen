Set-StrictMode -Version Latest
$script:XenProbeOwnedStages = @{}

function Initialize-XenProbeNativeProcess {
    if ('XenProbeNativeProcess' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

public sealed class XenProbeNativeResult {
    public uint ExitCode;
    public string StandardOutput;
    public string StandardError;
}

public static class XenProbeNativeProcess {
    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes {
        public int Length;
        public IntPtr Descriptor;
        [MarshalAs(UnmanagedType.Bool)] public bool Inherit;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo {
        public int Size;
        public string Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
        public short ShowWindow, ReservedSize;
        public IntPtr ReservedBytes, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern bool CreateProcessW(string application, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint flags,
        IntPtr environment, string directory, ref StartupInfo startup, out ProcessInformation process);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CreatePipe(out SafeFileHandle read, out SafeFileHandle write,
        ref SecurityAttributes attributes, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetHandleInformation(SafeFileHandle handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern bool CreateDirectoryW(string path, IntPtr attributes);

    public static void CreateNewDirectory(string path) {
        if (!CreateDirectoryW(path, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot exclusively create staging: " + path);
    }

    private static string Quote(string value) {
        if (value == null || value.IndexOf('\0') >= 0)
            throw new ArgumentException("Native arguments cannot be null or contain NUL.");
        var quoted = new StringBuilder("\"");
        int slashes = 0;
        foreach (char character in value) {
            if (character == '\\') { ++slashes; continue; }
            if (character == '"') {
                quoted.Append('\\', slashes * 2 + 1).Append('"');
            } else {
                quoted.Append('\\', slashes).Append(character);
            }
            slashes = 0;
        }
        return quoted.Append('\\', slashes * 2).Append('"').ToString();
    }

    private static void Pipe(out SafeFileHandle read, out SafeFileHandle write, bool parentReads) {
        var attributes = new SecurityAttributes {
            Length = Marshal.SizeOf(typeof(SecurityAttributes)), Inherit = true
        };
        if (!CreatePipe(out read, out write, ref attributes, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreatePipe failed.");
        if (!SetHandleInformation(parentReads ? read : write, 1, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot restrict pipe inheritance.");
    }

    public static XenProbeNativeResult Run(string executable, string[] arguments,
            string directory, uint timeoutMilliseconds) {
        var commandLine = new StringBuilder(Quote(executable));
        foreach (string argument in arguments) commandLine.Append(' ').Append(Quote(argument));
        if (commandLine.Length >= 32767) throw new ArgumentException("Native command line is too long.");
        SafeFileHandle outputRead = null, outputWrite = null, errorRead = null, errorWrite = null;
        SafeFileHandle inputRead = null, inputWrite = null;
        StreamReader outputReader = null, errorReader = null;
        var process = new ProcessInformation();
        bool exited = false;
        try {
            Pipe(out outputRead, out outputWrite, true);
            Pipe(out errorRead, out errorWrite, true);
            Pipe(out inputRead, out inputWrite, false);
            var startup = new StartupInfo {
                Size = Marshal.SizeOf(typeof(StartupInfo)), Flags = 0x100,
                Input = inputRead.DangerousGetHandle(),
                Output = outputWrite.DangerousGetHandle(), Error = errorWrite.DangerousGetHandle()
            };
            if (!CreateProcessW(executable, commandLine, IntPtr.Zero, IntPtr.Zero, true,
                    0x08000000, IntPtr.Zero, directory, ref startup, out process))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessW failed: " + executable);
            CloseHandle(process.Thread); process.Thread = IntPtr.Zero;
            outputWrite.Dispose(); errorWrite.Dispose(); inputRead.Dispose(); inputWrite.Dispose();
            outputReader = new StreamReader(new FileStream(outputRead, FileAccess.Read), Encoding.UTF8, true);
            errorReader = new StreamReader(new FileStream(errorRead, FileAccess.Read), Encoding.UTF8, true);
            var outputTask = outputReader.ReadToEndAsync();
            var errorTask = errorReader.ReadToEndAsync();
            uint wait = WaitForSingleObject(process.Process, timeoutMilliseconds);
            if (wait == 258) throw new TimeoutException("Native process exceeded its time limit: " + executable);
            if (wait != 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "Native process wait failed.");
            exited = true;
            uint exitCode;
            if (!GetExitCodeProcess(process.Process, out exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read native exit code.");
            return new XenProbeNativeResult {
                ExitCode = exitCode, StandardOutput = outputTask.GetAwaiter().GetResult(),
                StandardError = errorTask.GetAwaiter().GetResult()
            };
        } catch (Exception failure) {
            if (process.Process != IntPtr.Zero && !exited) {
                // Only this invocation's process handle may be terminated.
                bool stopped = TerminateProcess(process.Process, 1) &&
                    WaitForSingleObject(process.Process, 5000) == 0;
                if (!stopped) failure.Data["XenProbeProcessStillRunning"] = true;
            }
            throw;
        } finally {
            if (outputReader != null) outputReader.Dispose();
            if (errorReader != null) errorReader.Dispose();
            foreach (var handle in new[] { outputRead, outputWrite, errorRead, errorWrite, inputRead, inputWrite })
                if (handle != null) handle.Dispose();
            if (process.Thread != IntPtr.Zero) CloseHandle(process.Thread);
            if (process.Process != IntPtr.Zero) CloseHandle(process.Process);
        }
    }
}
'@
}

function Get-XenProbeFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.StartsWith('\\?\') -or
        $Path.StartsWith('\\.\')) { throw 'A normal filesystem path is required.' }
    return [IO.Path]::GetFullPath($Path)
}

function Get-XenProbeApiPath([string]$FullPath) {
    if ($FullPath.StartsWith('\\')) { return '\\?\UNC\' + $FullPath.Substring(2) }
    return '\\?\' + $FullPath
}

function Assert-XenProbePlainPath([string]$Path) {
    for ($current = $Path; $current; $current = [IO.Path]::GetDirectoryName($current)) {
        $attributes = [IO.File]::GetAttributes((Get-XenProbeApiPath $current))
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Rejected a reparse point: $current"
        }
    }
}

function Invoke-XenProbeNativeProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [AllowEmptyCollection()][AllowEmptyString()][string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).ProviderPath,
        [ValidateRange(1, 2147483647)][int]$TimeoutMilliseconds = 30000
    )
    $full = Get-XenProbeFullPath $Executable
    Assert-XenProbePlainPath $full
    $directory = Get-XenProbeFullPath $WorkingDirectory
    Assert-XenProbePlainPath $directory
    Initialize-XenProbeNativeProcess
    return [XenProbeNativeProcess]::Run(
        (Get-XenProbeApiPath $full), $Arguments, $directory, $TimeoutMilliseconds)
}

function Invoke-XenProbeNativeTool {
    param([string]$Executable, [AllowEmptyString()][string[]]$Arguments, [string]$Description)
    $result = Invoke-XenProbeNativeProcess -Executable $Executable -Arguments $Arguments
    if ($result.ExitCode -ne 0) {
        throw "$Description failed; ExitCode=$($result.ExitCode)`n$($result.StandardOutput)$($result.StandardError)"
    }
    if ($result.StandardOutput) { Write-Host $result.StandardOutput.TrimEnd() }
    if ($result.StandardError) { [Console]::Error.Write($result.StandardError) }
}

function New-XenProbeStagingDirectory([string]$Path, [string]$Parent) {
    $full = Get-XenProbeFullPath $Path
    $parentFull = Get-XenProbeFullPath $Parent
    $name = [IO.Path]::GetFileName($full)
    if ([IO.Path]::GetDirectoryName($full) -ine $parentFull -or
        $name -notmatch '^\..+\.incoming-[0-9a-f]{32}$') {
        throw 'Staging must be a direct incoming-GUID child of its parent.'
    }
    Assert-XenProbePlainPath $parentFull
    Initialize-XenProbeNativeProcess
    [XenProbeNativeProcess]::CreateNewDirectory((Get-XenProbeApiPath $full))
    $owner = [pscustomobject]@{ Path = $full; Parent = $parentFull; Name = $name }
    $script:XenProbeOwnedStages[$full] = $owner
    return $owner
}

function Assert-XenProbeStagingOwner([object]$Owner) {
    $full = Get-XenProbeFullPath ([string]$Owner.Path)
    if (-not $script:XenProbeOwnedStages.ContainsKey($full) -or
        -not [object]::ReferenceEquals($script:XenProbeOwnedStages[$full], $Owner) -or
        [IO.Path]::GetDirectoryName($full) -ine [string]$Owner.Parent -or
        [IO.Path]::GetFileName($full) -cne [string]$Owner.Name -or
        [string]$Owner.Name -notmatch '^\..+\.incoming-[0-9a-f]{32}$') {
        throw 'Staging ownership does not match its direct parent and name.'
    }
    Assert-XenProbePlainPath $full
    return $full
}

function Complete-XenProbeStagingDirectory([object]$Owner, [string]$Destination) {
    $full = Assert-XenProbeStagingOwner $Owner
    $target = Get-XenProbeFullPath $Destination
    if ([IO.Path]::GetDirectoryName($target) -ine [string]$Owner.Parent) {
        throw 'Published destination must share the owned staging parent.'
    }
    Assert-XenProbePlainPath ([string]$Owner.Parent)
    # Directory.Move refuses an existing destination, including empty directories.
    [IO.Directory]::Move((Get-XenProbeApiPath $full), (Get-XenProbeApiPath $target))
    $script:XenProbeOwnedStages.Remove($full)
}

function Remove-XenProbeStagingDirectory([object]$Owner) {
    $full = Assert-XenProbeStagingOwner $Owner
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue((Get-XenProbeApiPath $full))
    while ($pending.Count -gt 0) {
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($pending.Dequeue())) {
            $attributes = [IO.File]::GetAttributes($entry)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Rejected a reparse point in staging: $entry"
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) { $pending.Enqueue($entry) }
        }
    }
    Assert-XenProbePlainPath $full
    [IO.Directory]::Delete((Get-XenProbeApiPath $full), $true)
    $script:XenProbeOwnedStages.Remove($full)
}

function Undo-XenProbeStagingDirectory([object]$Owner, [object]$Failure) {
    try {
        for ($cause = $Failure.Exception; $null -ne $cause; $cause = $cause.InnerException) {
            if ($cause.Data.Contains('XenProbeProcessStillRunning')) {
                throw 'Child process exit is unconfirmed; staging retained.'
            }
        }
        Remove-XenProbeStagingDirectory $Owner
    } catch {
        # Diagnostic output must not replace the caller's original ErrorRecord.
        try {
            [Console]::Error.WriteLine("Staging cleanup failed; retained path={0}; {1}", $Owner.Path, $_)
        } catch { }
    }
}

Export-ModuleMember -Function @(
    'Invoke-XenProbeNativeProcess', 'Invoke-XenProbeNativeTool',
    'New-XenProbeStagingDirectory', 'Complete-XenProbeStagingDirectory',
    'Remove-XenProbeStagingDirectory', 'Undo-XenProbeStagingDirectory')
