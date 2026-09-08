param(
    [string]$InstallRoot = "",
    [switch]$AfterRestart
)

$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
$deepFreezeServiceNames = @("DFServ", "DeepFrz")

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $clientDirectory = Get-Item -LiteralPath $PSScriptRoot -ErrorAction Stop
    if (-not [string]::Equals($clientDirectory.Name, "client",
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw "InstallRoot must be supplied when the uninstaller is not under the installed client layout."
    }
    $InstallRoot = $clientDirectory.FullName
}
$installRoot = [IO.Path]::GetFullPath($InstallRoot)
$installRootName = [IO.Path]::GetPathRoot($installRoot)
if ([string]::IsNullOrWhiteSpace($installRootName) -or
    $installRoot.TrimEnd('\') -eq $installRootName.TrimEnd('\')) {
    throw "InstallRoot must be an absolute NSTU client subdirectory, not a drive root."
}
$layoutMarkers = @(
    (Join-Path $installRoot "nstu-service.exe"),
    (Join-Path $installRoot "nstu-agent.exe"),
    (Join-Path $installRoot "uninstall-client-service.ps1")
)
if (@($layoutMarkers | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    }).Count -lt 3) {
    throw "InstallRoot does not contain a complete NSTU client installation marker: $installRoot"
}
$expectedServiceBinary = [IO.Path]::GetFullPath(
    (Join-Path $installRoot "nstu-service.exe"))

function Get-NstuService {
    param([Parameter(Mandatory = $true)][string]$Name)

    try {
        return Get-Service -Name $Name -ErrorAction Stop
    } catch {
        if ($_.FullyQualifiedErrorId -like "NoServiceFoundForGivenName*" -or
            $_.Exception.Message -match "cannot find any service with service name") {
            return $null
        }
        throw "Unable to query service '$Name': $($_.Exception.Message)"
    }
}

function Invoke-NstuSc {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Operation
    )

    & sc.exe @Arguments | Out-Null
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "sc.exe $Operation failed with exit code $exitCode."
    }
}

function Wait-NstuServiceDeleted {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutMilliseconds = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $service = $null
        try {
            $service = Get-NstuService -Name $Name
            if ($null -eq $service) {
                return $true
            }
        } finally {
            if ($null -ne $service) {
                $service.Dispose()
            }
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

if (-not ("NstuNativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NstuNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string existingFileName,
        string newFileName, int flags);
}
"@
}

function Wait-NstuProcessExit {
    param(
        [Parameter(Mandatory = $true)]
        [uint32]$ProcessId,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedPath,
        [int]$TimeoutMilliseconds = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $current = @(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
        if ($current.Count -eq 0) {
            return $true
        }
        try {
            $currentPath = $current[0].Path
            if ([string]::IsNullOrWhiteSpace($currentPath)) {
                Start-Sleep -Milliseconds 100
                continue
            }
            $normalizedCurrentPath = [IO.Path]::GetFullPath($currentPath)
        } catch {
            Start-Sleep -Milliseconds 100
            continue
        } finally {
            $current[0].Dispose()
        }
        if (-not [string]::Equals($normalizedCurrentPath, $ExpectedPath,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Stop-NstuProcessById {
    param(
        [Parameter(Mandatory = $true)][uint32]$ProcessId,
        [Parameter(Mandatory = $true)][string]$ExpectedPath,
        [Parameter(Mandatory = $true)][string]$DisplayName
    )

    $actualPath = $null
    try {
        $processInfo = @(Get-CimInstance Win32_Process `
            -Filter "ProcessId = $ProcessId" -ErrorAction Stop)
        if ($processInfo.Count -gt 0) {
            $actualPath = $processInfo[0].ExecutablePath
        }
    } catch {
        # Hardened guests can deny the CIM process provider even to an
        # elevated administrator. Get-Process is sufficient for path
        # verification in that case.
    }
    if ([string]::IsNullOrWhiteSpace($actualPath)) {
        $process = $null
        try {
            $process = Get-Process -Id $ProcessId -ErrorAction Stop
            $actualPath = $process.Path
        } catch {
            if ($_.Exception.Message -match "cannot find|not found|does not exist") {
                return
            }
        } finally {
            if ($null -ne $process) {
                $process.Dispose()
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($actualPath)) {
        throw "Cannot verify the executable path for $DisplayName PID $ProcessId."
    }
    $normalizedPath = [IO.Path]::GetFullPath($actualPath)
    if (-not [string]::Equals($normalizedPath, $ExpectedPath,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw "$DisplayName PID $ProcessId points at an unexpected executable."
    }
    Stop-Process -Id $ProcessId -Force -ErrorAction Stop
    if (-not (Wait-NstuProcessExit -ProcessId $ProcessId `
              -ExpectedPath $ExpectedPath)) {
        throw "$DisplayName PID $ProcessId is still running."
    }
}

function Get-NstuServiceProcessId {
    param([Parameter(Mandatory = $true)][string]$Name)

    $query = @(& sc.exe queryex $Name 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 1060) {
        return [uint32]0
    }
    if ($exitCode -ne 0) {
        throw "sc.exe queryex '$Name' failed with exit code $exitCode."
    }
    foreach ($line in $query) {
        if ([string]$line -match '^\s*PID\s*:\s*(\d+)') {
            return [uint32]$matches[1]
        }
    }
    throw "sc.exe queryex '$Name' did not report a process ID."
}

function Disable-NstuServiceRecovery {
    param([Parameter(Mandatory = $true)][string]$Name)

    # Remove restart actions before a forced process termination. Otherwise
    # SCM can launch a replacement while the package files are being removed.
    Invoke-NstuSc -Arguments @(
        "failure", $Name, "reset=", "0", "actions=", ""
    ) -Operation "clear recovery actions for '$Name'"
    Invoke-NstuSc -Arguments @(
        "failureflag", $Name, "0"
    ) -Operation "disable recovery for '$Name'"
}

function Stop-NstuProcesses {
    param([string[]]$Names)

    $failures = [Collections.Generic.List[string]]::new()
    foreach ($name in $Names) {
        $expectedPath = [IO.Path]::GetFullPath((Join-Path $installRoot "$name.exe"))
        $processes = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
        foreach ($process in $processes) {
            try {
                try {
                    $actualPath = [IO.Path]::GetFullPath($process.Path)
                } catch {
                    $failures.Add("Cannot verify the executable path for PID $($process.Id) ($name).")
                    continue
                }
                if (-not [string]::Equals($actualPath, $expectedPath,
                                          [StringComparison]::OrdinalIgnoreCase)) {
                    continue
                }
                try {
                    Stop-Process -Id $process.Id -Force -ErrorAction Stop
                } catch {
                    $failures.Add("Could not terminate $name PID $($process.Id): $($_.Exception.Message)")
                    continue
                }
                if (-not (Wait-NstuProcessExit -ProcessId $process.Id `
                          -ExpectedPath $expectedPath)) {
                    $failures.Add("$name PID $($process.Id) is still running.")
                }
            } finally {
                $process.Dispose()
            }
        }
    }
    if ($failures.Count -gt 0) {
        throw ($failures -join " ")
    }
}

function Schedule-DeleteAtReboot {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $true }
    return [NstuNativeMethods]::MoveFileEx($Path, $null, 4)
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU client uninstallation requires Administrator privileges."
}
if (-not $AfterRestart) {
    throw "NSTU client removal must be staged by the unified uninstaller and completed after Windows restarts."
}
$packageRoot = [IO.Path]::GetFullPath((Split-Path -Parent $installRoot))
$finalizeScript = Join-Path $packageRoot "docs\deployment\finalize-uninstall.ps1"
if (-not (Test-Path -LiteralPath $finalizeScript -PathType Leaf)) {
    throw "The installed staged-removal validator was not found: $finalizeScript"
}
& $finalizeScript -Role Client -InstallRoot $packageRoot

$activeDeepFreezeServiceNames = [Collections.Generic.List[string]]::new()
foreach ($deepFreezeServiceName in $deepFreezeServiceNames) {
    $deepFreezeService = $null
    try {
        $deepFreezeService = Get-NstuService -Name $deepFreezeServiceName
        if ($null -ne $deepFreezeService -and
            ($deepFreezeService.Status -ne
                [System.ServiceProcess.ServiceControllerStatus]::Stopped -or
             $deepFreezeService.StartType -ne
                [System.ServiceProcess.ServiceStartMode]::Disabled)) {
            [void]$activeDeepFreezeServiceNames.Add($deepFreezeServiceName)
        }
    } finally {
        if ($null -ne $deepFreezeService) {
            $deepFreezeService.Dispose()
        }
    }
}
if ($activeDeepFreezeServiceNames.Count -gt 0) {
    $detected = $activeDeepFreezeServiceNames -join ", "
    throw "Deep Freeze protection appears active ($detected). Boot the computer Thawed, disable Deep Freeze protection, restart Windows, and then run the NSTU uninstaller again."
}

$existing = Get-NstuService -Name $serviceName
if ($null -ne $existing) {
    try {
        try {
            & sc.exe stop $serviceName | Out-Null
            $stopExitCode = $LASTEXITCODE
            # ERROR_SERVICE_NOT_ACTIVE (1062) is benign; every other SCM
            # failure must be surfaced before attempting file removal.
            if ($stopExitCode -ne 0 -and $stopExitCode -ne 1062) {
                throw "sc.exe stop failed with exit code $stopExitCode"
            }
        } finally {
            Disable-NstuServiceRecovery -Name $serviceName
        }
        try {
            $existing.WaitForStatus(
                [System.ServiceProcess.ServiceControllerStatus]::Stopped,
                [TimeSpan]::FromSeconds(15))
        } catch {
            Write-Warning "NSTU service did not stop within 15 seconds; forcing the installed service process to exit."
        }
        $servicePid = Get-NstuServiceProcessId -Name $serviceName
        if ($servicePid -ne 0) {
            Stop-NstuProcessById -ProcessId $servicePid `
                -ExpectedPath $expectedServiceBinary -DisplayName "nstu-service"
        }
    } finally {
        $existing.Dispose()
    }
    Invoke-NstuSc -Arguments @("delete", $serviceName) `
        -Operation "delete '$serviceName'"
    if (-not (Wait-NstuServiceDeleted -Name $serviceName)) {
        throw "Service '$serviceName' is still present after deletion."
    }
}

# Stop the privileged supervisor before terminating the interactive agent so it
# cannot launch a replacement during uninstall.
Stop-NstuProcesses @("nstu-agent")

$pendingFiles = [Collections.Generic.List[string]]::new()
$scheduleFailures = [Collections.Generic.List[string]]::new()
try {
    $packageFiles = @(Get-ChildItem -LiteralPath $installRoot -File -Recurse -Force -ErrorAction Stop)
} catch {
    throw "Unable to enumerate NSTU package files for removal: $($_.Exception.Message)"
}
$packageFiles | ForEach-Object {
        try {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop
        } catch {
            if (Schedule-DeleteAtReboot $_.FullName) {
                $pendingFiles.Add($_.FullName)
            } else {
                $scheduleFailures.Add($_.FullName)
            }
        }
    }

if ($scheduleFailures.Count -gt 0) {
    throw ("Could not remove or schedule deletion for: " +
        ($scheduleFailures -join ", "))
}

if ($pendingFiles.Count -gt 0) {
    Write-Host "NSTU files are locked and have been scheduled for deletion at the next Windows restart."
} else {
    Write-Host "NSTU client files and service removed. Restart Windows to finalize removal."
}
