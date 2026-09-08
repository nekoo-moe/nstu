param(
    [string]$InstallRoot = "",
    [switch]$AfterRestart
)

$ErrorActionPreference = "Stop"
$deepFreezeServiceNames = @("DFServ", "DeepFrz")

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $deploymentDirectory = Get-Item -LiteralPath $PSScriptRoot -ErrorAction Stop
    if ($deploymentDirectory.Name -ne "deployment" -or
        $null -eq $deploymentDirectory.Parent -or
        $deploymentDirectory.Parent.Name -ne "docs" -or
        $null -eq $deploymentDirectory.Parent.Parent) {
        throw "InstallRoot must be supplied when the uninstaller is not under the installed docs\deployment layout."
    }
    $InstallRoot = $deploymentDirectory.Parent.Parent.FullName
}
$installRoot = [IO.Path]::GetFullPath($InstallRoot)
$installRootName = [IO.Path]::GetPathRoot($installRoot)
if ([string]::IsNullOrWhiteSpace($installRootName) -or
    $installRoot.TrimEnd('\') -eq $installRootName.TrimEnd('\')) {
    throw "InstallRoot must be an absolute NSTU subdirectory, not a drive root."
}
$layoutMarkers = @(
    (Join-Path $installRoot "server\nstu-server.exe"),
    (Join-Path $installRoot "diagnostics\nstu-diagnostics.exe"),
    (Join-Path $installRoot "docs\deployment\uninstall-server.ps1")
)
if (@($layoutMarkers | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    }).Count -lt $layoutMarkers.Count) {
    throw "InstallRoot does not contain a complete NSTU server installation marker: $installRoot"
}

if (-not ("NstuNativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class NstuNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string existingFileName,
        string newFileName, int flags);
}
"@
}

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
        $current = $null
        try {
            $current = Get-Process -Id $ProcessId -ErrorAction Stop
        } catch {
            if ($_.FullyQualifiedErrorId -like "NoProcessFoundForGivenId*") {
                return $true
            }
            Start-Sleep -Milliseconds 100
            continue
        }
        try {
            $currentPath = $current.Path
            if ([string]::IsNullOrWhiteSpace($currentPath)) {
                Start-Sleep -Milliseconds 100
                continue
            }
            $normalizedCurrentPath = [IO.Path]::GetFullPath($currentPath)
        } catch {
            Start-Sleep -Milliseconds 100
            continue
        } finally {
            $current.Dispose()
        }
        if (-not [string]::Equals($normalizedCurrentPath, $ExpectedPath,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU server uninstallation requires Administrator privileges."
}
if (-not $AfterRestart) {
    throw "NSTU server removal must be staged by the unified uninstaller and completed after Windows restarts."
}
$finalizeScript = Join-Path $installRoot "docs\deployment\finalize-uninstall.ps1"
if (-not (Test-Path -LiteralPath $finalizeScript -PathType Leaf)) {
    throw "The installed staged-removal validator was not found: $finalizeScript"
}
& $finalizeScript -Role Server -InstallRoot $installRoot

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

$serverBinary = [IO.Path]::GetFullPath((Join-Path $installRoot "server\nstu-server.exe"))
$terminationFailures = [Collections.Generic.List[string]]::new()
try {
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='nstu-server.exe'" `
        -ErrorAction Stop)
} catch {
    $processes = @()
    $nativeQueryErrors = @()
    $nativeProcesses = @(Get-Process -Name "nstu-server" `
        -ErrorAction SilentlyContinue -ErrorVariable +nativeQueryErrors)
    $unexpectedQueryErrors = @($nativeQueryErrors | Where-Object {
        $_.FullyQualifiedErrorId -notlike "NoProcessFoundForGivenName*"
    })
    if ($unexpectedQueryErrors.Count -gt 0) {
        throw "Unable to inspect NSTU server processes through CIM or Get-Process."
    }
    foreach ($nativeProcess in $nativeProcesses) {
        try {
            $nativePath = $null
            try {
                $nativePath = $nativeProcess.Path
            } catch {
                # The path is validated below and an unavailable path fails
                # closed rather than risking termination of another process.
            }
            $processes += [pscustomobject]@{
                ProcessId = [uint32]$nativeProcess.Id
                ExecutablePath = $nativePath
            }
        } finally {
            $nativeProcess.Dispose()
        }
    }
}
foreach ($process in $processes) {
    if ([string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
        $terminationFailures.Add(
            "Cannot verify the executable path for server PID $($process.ProcessId).")
        continue
    }
    try {
        $actualPath = [IO.Path]::GetFullPath($process.ExecutablePath)
    } catch {
        $terminationFailures.Add(
            "Cannot normalize the executable path for server PID $($process.ProcessId).")
        continue
    }
    if (-not [string]::Equals($actualPath, $serverBinary,
                              [StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    try {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
    } catch {
        $terminationFailures.Add(
            "Could not terminate NSTU server PID $($process.ProcessId): $($_.Exception.Message)")
        continue
    }
    if (-not (Wait-NstuProcessExit -ProcessId $process.ProcessId `
              -ExpectedPath $serverBinary)) {
        $terminationFailures.Add(
            "NSTU server PID $($process.ProcessId) is still running.")
    }
}
if ($terminationFailures.Count -gt 0) {
    throw ($terminationFailures -join " ")
}

$pendingFiles = [Collections.Generic.List[string]]::new()
$scheduleFailures = [Collections.Generic.List[string]]::new()
foreach ($directory in @("server", "diagnostics")) {
    $path = Join-Path $installRoot $directory
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        continue
    }
    try {
        $packageFiles = @(Get-ChildItem -LiteralPath $path -File -Recurse -Force `
            -ErrorAction Stop)
    } catch {
        throw "Unable to enumerate NSTU server package files: $($_.Exception.Message)"
    }
    $packageFiles | ForEach-Object {
            try {
                Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop
            } catch {
                if (-not [NstuNativeMethods]::MoveFileEx($_.FullName, $null, 4)) {
                    $scheduleFailures.Add($_.FullName)
                } else {
                    $pendingFiles.Add($_.FullName)
                }
            }
        }
}

if ($scheduleFailures.Count -gt 0) {
    throw ("Could not remove or schedule deletion for: " +
        ($scheduleFailures -join ", "))
}

if ($pendingFiles.Count -gt 0) {
    Write-Host "NSTU server files are locked and scheduled for deletion at the next Windows restart."
} else {
    Write-Host "NSTU server process and package files removed. Restart Windows to finalize removal."
}
