param(
    [Parameter(Mandatory = $true)][ValidateSet("Client", "Server")]
    [string]$Role,
    [Parameter(Mandatory = $true)][string]$InstallRoot,
    [Parameter(Mandatory = $true)][string]$UninstallerPath
)

$ErrorActionPreference = "Stop"
$taskName = "NSTU-FinalizeUninstall"
$stateRoot = Join-Path $env:ProgramData "NSTU"
$statePath = Join-Path $stateRoot "uninstall-state.json"

$installRootFull = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$uninstallerFull = [IO.Path]::GetFullPath($UninstallerPath)
$installRootName = [IO.Path]::GetPathRoot($installRootFull)
if ([string]::IsNullOrWhiteSpace($installRootName) -or
    $installRootFull -eq $installRootName.TrimEnd('\')) {
    throw "InstallRoot must be an absolute NSTU installation directory, not a drive root."
}
if (-not $uninstallerFull.StartsWith($installRootFull + '\',
        [StringComparison]::OrdinalIgnoreCase) -or
    -not [string]::Equals((Split-Path -Leaf $uninstallerFull),
                          "Uninstall.exe",
                          [StringComparison]::OrdinalIgnoreCase)) {
    throw "UninstallerPath must be the installed Uninstall.exe below InstallRoot."
}
if (-not (Test-Path -LiteralPath $uninstallerFull -PathType Leaf)) {
    throw "Installed uninstaller was not found: $uninstallerFull"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Staging NSTU removal requires Administrator privileges."
}

$active = [Collections.Generic.List[string]]::new()
foreach ($name in @("DFServ", "DeepFrz")) {
    $service = $null
    try {
        $service = Get-Service -Name $name -ErrorAction Stop
    } catch {
        if ($_.FullyQualifiedErrorId -like "NoServiceFoundForGivenName*" -or
            $_.Exception.Message -match "cannot find any service with service name") {
            continue
        }
        throw "Unable to query Deep Freeze service '$name': $($_.Exception.Message)"
    }
    try {
        if ($service.Status -ne "Stopped" -or
            $service.StartType -ne "Disabled") {
            [void]$active.Add($name)
        }
    } finally {
        $service.Dispose()
    }
}
if ($active.Count -gt 0) {
    throw "Deep Freeze protection appears active. Boot Thawed, disable protection, and retry."
}

New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
$state = [ordered]@{
    schema = 2
    role = $Role
    install_root = $installRootFull
    uninstaller = $uninstallerFull
    staged_utc = [DateTimeOffset]::UtcNow.ToString("o")
    boot_utc = [DateTimeOffset]::UtcNow.AddMilliseconds(
        -[Environment]::TickCount64).ToString("o")
    uptime_ms = [Environment]::TickCount64
}
try {
    $state | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $statePath -Encoding UTF8
    $action = New-ScheduledTaskAction -Execute $state.uninstaller `
        -Argument "/S /AFTERRESTART=1"
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $system = New-ScheduledTaskPrincipal -UserId "SYSTEM" `
        -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $taskName -Action $action `
        -Trigger $trigger -Principal $system -Force | Out-Null
} catch {
    Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
    throw
}
Write-Host "NSTU removal is staged. No service, process, or package file was changed. Restart Windows to continue."
