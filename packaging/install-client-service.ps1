param(
    [string]$InstallRoot = "",
    [string]$DataRoot = (Join-Path $env:ProgramData "NSTU")
)

$ErrorActionPreference = "Stop"
$serviceName = "nstu-service"
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = $PSScriptRoot
}
$serviceBinary = [IO.Path]::GetFullPath((Join-Path $InstallRoot "nstu-service.exe"))

function Invoke-ScCommand {
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

function Get-ServiceState {
    param([Parameter(Mandatory = $true)][string]$Name)

    $service = $null
    try {
        try {
            $service = Get-Service -Name $Name -ErrorAction Stop
        } catch {
            if ($_.FullyQualifiedErrorId -like "NoServiceFoundForGivenName*" -or
                $_.Exception.Message -match "(?i)cannot find any service with service name") {
                return $null
            }
            throw "Unable to query service '$Name': $($_.Exception.Message)"
        }
        if ($null -eq $service) {
            return $null
        }
        return $service.Status
    } finally {
        if ($null -ne $service) {
            $service.Dispose()
        }
    }
}

function Get-ServiceConfiguration {
    param([Parameter(Mandatory = $true)][string]$Name)

    try {
        $configuration = Get-CimInstance Win32_Service -Filter "Name='$Name'" `
            -ErrorAction Stop
        if ($null -ne $configuration) {
            return $configuration
        }
    } catch {
        # WMI/CIM can be unavailable in hardened or remotely controlled setup
        # sessions. The SCM's protected registry configuration is authoritative.
    }

    $serviceRegistryPath =
        "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
    $registry = Get-ItemProperty -LiteralPath $serviceRegistryPath `
        -ErrorAction Stop
    $startMode = switch ([int]$registry.Start) {
        2 { "Auto" }
        3 { "Manual" }
        4 { "Disabled" }
        default { "Unknown" }
    }
    return [pscustomobject]@{
        PathName = [string]$registry.ImagePath
        StartName = [string]$registry.ObjectName
        StartMode = $startMode
    }
}

function Wait-ServiceStopped {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutMilliseconds = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $status = Get-ServiceState -Name $Name
        if ($null -eq $status) {
            throw "The service '$Name' disappeared while waiting for it to stop."
        }
        if ($status -eq [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Wait-ServiceDeleted {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutMilliseconds = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Get-ServiceState -Name $Name)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Ensure-ServiceStopped {
    param([Parameter(Mandatory = $true)][string]$Name)

    $status = Get-ServiceState -Name $Name
    if ($null -eq $status -or
        $status -eq [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        return
    }
    Invoke-ScCommand -Arguments @("stop", $Name) -Operation "stop '$Name'"
    if (-not (Wait-ServiceStopped -Name $Name)) {
        throw "Existing NSTU service '$Name' did not stop within 15 seconds; refusing to reconfigure it. Close dependent processes or reboot, then retry."
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "NSTU client installation requires Administrator privileges."
}

if (-not (Test-Path -LiteralPath $serviceBinary)) {
    throw "NSTU service binary was not found: $serviceBinary"
}

& (Join-Path $PSScriptRoot "test-system-setup.ps1") `
    -Role Client -DataRoot $DataRoot
& (Join-Path $PSScriptRoot "configure-data-root.ps1") -DataRoot $DataRoot

$escapedBinary = '"' + $serviceBinary.Replace('"', '\"') + '"'
$createdService = $false

try {
    $existingStatus = Get-ServiceState -Name $serviceName
    if ($null -ne $existingStatus) {
        Ensure-ServiceStopped -Name $serviceName
        Invoke-ScCommand -Arguments @(
            "config", $serviceName, "binPath=", $escapedBinary,
            "start=", "auto", "obj=", "LocalSystem",
            "DisplayName=", "NSTU Client Service"
        ) -Operation "config '$serviceName'"
    } else {
        Invoke-ScCommand -Arguments @(
            "create", $serviceName, "binPath=", $escapedBinary,
            "start=", "auto", "obj=", "LocalSystem",
            "DisplayName=", "NSTU Client Service"
        ) -Operation "create '$serviceName'"
        $createdService = $true
    }

    $serviceConfig = Get-ServiceConfiguration -Name $serviceName
    if ($null -eq $serviceConfig) {
        throw "NSTU service was not found after SCM configuration."
    }
    $configuredPath = $serviceConfig.PathName.Trim()
    if ($configuredPath.StartsWith('"') -and $configuredPath.EndsWith('"')) {
        $configuredPath = $configuredPath.Substring(1, $configuredPath.Length - 2)
    }
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath($configuredPath), $serviceBinary,
            [StringComparison]::OrdinalIgnoreCase) -or
        $serviceConfig.StartName -notin @('LocalSystem', 'NT AUTHORITY\SYSTEM') -or
        $serviceConfig.StartMode -ne 'Auto') {
        throw "NSTU service configuration is not LocalSystem/automatic or points at an unexpected binary."
    }

    Invoke-ScCommand -Arguments @(
        "description", $serviceName, "NSTU classroom client service"
    ) -Operation "set description for '$serviceName'"
    Invoke-ScCommand -Arguments @(
        "failure", $serviceName, "reset=", "86400",
        "actions=", "restart/5000/restart/15000/restart/60000"
    ) -Operation "configure recovery for '$serviceName'"
    Invoke-ScCommand -Arguments @("failureflag", $serviceName, "1") `
        -Operation "enable recovery for '$serviceName'"
    $serviceSddl = "D:P(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)" +
        "(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)" +
        "(A;;LCLORC;;;AU)"
    Invoke-ScCommand -Arguments @("sdset", $serviceName, $serviceSddl) `
        -Operation "set security descriptor for '$serviceName'"

    # Installation intentionally leaves the service stopped. The reboot that
    # the installer requests is the activation boundary for the client.
    if (-not (Wait-ServiceStopped -Name $serviceName)) {
        Ensure-ServiceStopped -Name $serviceName
    }
} catch {
    $originalError = $_
    if ($createdService) {
        & sc.exe delete $serviceName | Out-Null
        $cleanupExitCode = $LASTEXITCODE
        if ($cleanupExitCode -ne 0) {
            Write-Warning "Rollback could not delete '$serviceName' (sc.exe exit $cleanupExitCode)."
        } elseif (-not (Wait-ServiceDeleted -Name $serviceName)) {
            Write-Warning "Rollback left '$serviceName' marked for deletion; restart Windows before retrying installation."
        }
    }
    throw $originalError
}

Write-Host "NSTU client service registered. Restart Windows to activate it."
Write-Host "The service will not be started by this script; restart Windows before using the client."
