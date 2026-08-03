function Get-XenRuntimeHardwareInventory {
    [CmdletBinding()]
    param(
        [scriptblock]$CimQuery = {
            param([string]$ClassName)
            Get-CimInstance -ClassName $ClassName -ErrorAction Stop
        }
    )

    $errors = New-Object System.Collections.Generic.List[string]
    $gpu = @()
    try {
        $gpu = @(& $CimQuery "Win32_VideoController" | ForEach-Object {
            [ordered]@{
                name = $_.Name
                driver_version = $_.DriverVersion
                adapter_ram = [long]$_.AdapterRAM
            }
        })
    } catch {
        $errors.Add("Win32_VideoController: $($_.Exception.Message)")
    }

    $os = ""
    try {
        $operatingSystem = @(& $CimQuery "Win32_OperatingSystem") |
            Select-Object -First 1
        if ($null -eq $operatingSystem -or
            [string]::IsNullOrWhiteSpace(
                ([string]$operatingSystem.Caption))) {
            throw "WMI 没有返回操作系统 Caption。"
        }
        $os = [string]$operatingSystem.Caption
    } catch {
        $errors.Add("Win32_OperatingSystem: $($_.Exception.Message)")
        try {
            $windows = Get-ItemProperty -LiteralPath `
                "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion" `
                -ErrorAction Stop
            $parts = @(
                [string]$windows.ProductName,
                [string]$windows.DisplayVersion,
                "build $([string]$windows.CurrentBuildNumber)"
            ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
            $os = $parts -join " "
        } catch {
            $errors.Add("Windows registry: $($_.Exception.Message)")
        }
        if ([string]::IsNullOrWhiteSpace($os)) {
            $os = [System.Environment]::OSVersion.VersionString
        }
    }

    return [ordered]@{
        computer_name = $env:COMPUTERNAME
        os = $os
        gpu = $gpu
        inventory_status = if ($errors.Count -eq 0) {
            "complete"
        } else {
            "partial"
        }
        inventory_errors = @($errors)
    }
}
