param(
    [Parameter(Mandatory = $true)][ValidateSet("Client", "Server")]
    [string]$Role,
    [Parameter(Mandatory = $true)][string]$InstallRoot,
    [switch]$Complete
)

$ErrorActionPreference = "Stop"
$taskName = "NSTU-FinalizeUninstall"
$statePath = Join-Path (Join-Path $env:ProgramData "NSTU") "uninstall-state.json"
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($identity.User.Value -ne "S-1-5-18") {
    throw "Final NSTU removal must run as LocalSystem after a Windows restart."
}
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
    throw "No staged NSTU removal was found."
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
$expectedRoot = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$stagedRoot = [IO.Path]::GetFullPath([string]$state.install_root).TrimEnd('\')
if ([int]$state.schema -ne 2 -or
    -not [string]::Equals([string]$state.role, $Role,
                          [StringComparison]::OrdinalIgnoreCase) -or
    -not [string]::Equals($stagedRoot, $expectedRoot,
                          [StringComparison]::OrdinalIgnoreCase)) {
    throw "The staged removal does not match this installation."
}
try {
    $stagedBootUtc = [DateTimeOffset]::Parse(
        [string]$state.boot_utc,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind)
} catch {
    throw "The staged removal does not contain a valid boot identity."
}
$currentBootUtc = [DateTimeOffset]::UtcNow.AddMilliseconds(
    -[Environment]::TickCount64)
if ($currentBootUtc -le $stagedBootUtc.AddSeconds(1)) {
    throw "Windows has not restarted since NSTU removal was staged."
}
if ($Complete) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction Stop
    Remove-Item -LiteralPath $statePath -Force -ErrorAction Stop
}
