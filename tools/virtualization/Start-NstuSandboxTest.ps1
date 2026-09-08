#requires -Version 5.1

[CmdletBinding(DefaultParameterSetName = "Build")]
param(
    [Parameter(Mandatory = $true, ParameterSetName = "Build", Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$BuildDirectory,

    [Parameter(Mandatory = $true, ParameterSetName = "Installer", Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$InstallerPath,

    [Parameter(Mandatory = $true, ParameterSetName = "Package", Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$PackageRoot,

    [string]$RepositoryRoot = "",
    [string]$OutputRoot = "",
    [string]$RuntimeRoot = "",
    [ValidateRange(2048, 16384)]
    [int]$MemoryInMB = 4096,
    [switch]$KeepSandboxOpen,
    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot "..\.."
}

$launcher = Join-Path $PSScriptRoot "run-sandbox-client-lifecycle.ps1"
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "The Sandbox launcher is missing: $launcher"
}

$invoke = @{
    RepositoryRoot = $RepositoryRoot
    MemoryInMB = $MemoryInMB
}
if (-not [string]::IsNullOrWhiteSpace($OutputRoot)) {
    $invoke.OutputRoot = $OutputRoot
}
if (-not [string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    $invoke.RuntimeRoot = $RuntimeRoot
}
if ($KeepSandboxOpen) {
    $invoke.KeepSandboxOpen = $true
}
if ($NoLaunch) {
    $invoke.NoLaunch = $true
}

switch ($PSCmdlet.ParameterSetName) {
    "Installer" {
        $invoke.InstallerPath = $InstallerPath
    }
    "Package" {
        $invoke.PackageRoot = $PackageRoot
    }
    "Build" {
        $buildFull = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $BuildDirectory -ErrorAction Stop).Path)
        $clientDirectory = Join-Path $buildFull "client"
        $stagedClientDirectory = Join-Path $buildFull "unified-installer-stage\client"
        $installer = Get-ChildItem -LiteralPath $buildFull -Filter "nstu-*-setup.exe" `
            -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notin @("nstu-service.exe", "nstu-agent.exe") } |
            Sort-Object FullName |
            Select-Object -First 1
        if (Test-Path -LiteralPath $stagedClientDirectory -PathType Container) {
            $invoke.PackageRoot = $stagedClientDirectory
            Write-Host "Using staged client payload: $stagedClientDirectory"
        } elseif (Test-Path -LiteralPath $clientDirectory -PathType Container) {
            $invoke.PackageRoot = $clientDirectory
            Write-Host "Using built client payload: $clientDirectory"
        } elseif ($null -ne $installer) {
            $invoke.InstallerPath = $installer.FullName
            Write-Host "Selected unified installer: $($installer.FullName)"
        } else {
            throw "BuildDirectory does not contain a unified installer or client directory: $buildFull"
        }
    }
}

$launcherExitCode = 0
& $launcher @invoke

# A PowerShell child script may finish without invoking a native process, in
# which case Windows PowerShell 5.1 leaves LASTEXITCODE undefined. Preserve a
# deterministic wrapper result instead of failing under StrictMode while
# reading an uninitialized automatic variable.
if (Test-Path -LiteralPath "variable:LASTEXITCODE") {
    $launcherExitCode = [int]$LASTEXITCODE
} elseif (-not $?) {
    $launcherExitCode = 1
}
exit $launcherExitCode
