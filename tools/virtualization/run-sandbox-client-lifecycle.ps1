#requires -Version 5.1

[CmdletBinding(DefaultParameterSetName = "Installer")]
param(
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

Set-Variable -Name ExpectedMarker -Option ReadOnly -Value `
    "NSTU-WINDOWS-SANDBOX-CLIENT-LIFECYCLE-v1"
Set-Variable -Name RequiredRuntimeNames -Option ReadOnly -Value @(
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll"
)

function Resolve-ExistingPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    return [IO.Path]::GetFullPath($resolved.Path)
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd("\")
    $targetFull = [IO.Path]::GetFullPath($Target).TrimEnd("\")
    if ([string]::Equals($rootFull, $targetFull,
                         [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $targetFull.StartsWith($rootFull + "\",
                                  [StringComparison]::OrdinalIgnoreCase)
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd("\")
    $targetFull = [IO.Path]::GetFullPath($Target).TrimEnd("\")
    if ([string]::Equals($rootFull, $targetFull,
                         [StringComparison]::OrdinalIgnoreCase)) {
        return ""
    }
    if (-not $targetFull.StartsWith($rootFull + "\",
                                    [StringComparison]::OrdinalIgnoreCase)) {
        throw "Target '$Target' is not below root '$Root'."
    }
    return $targetFull.Substring($rootFull.Length + 1)
}

function Add-MappedFolder {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IList]$Mappings,
        [Parameter(Mandatory = $true)][string]$HostFolder,
        [Parameter(Mandatory = $true)][string]$GuestFolder,
        [Parameter(Mandatory = $true)][bool]$ReadOnly
    )

    foreach ($mapping in $Mappings) {
        if ([string]::Equals($mapping.HostFolder, $HostFolder,
                             [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }
    [void]$Mappings.Add([pscustomobject]@{
        HostFolder = $HostFolder
        GuestFolder = $GuestFolder
        ReadOnly = $ReadOnly
    })
}

function Find-RuntimeFiles {
    param(
        [Parameter(Mandatory = $true)][string[]]$SearchRoots,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    $found = @{}
    foreach ($root in $SearchRoots) {
        if ([string]::IsNullOrWhiteSpace($root) -or
            -not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        foreach ($name in $Names) {
            if ($found.ContainsKey($name)) {
                continue
            }
            $candidate = Join-Path $root $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                [void]($found[$name] = (Resolve-ExistingPath $candidate))
            }
        }
    }
    # Prevent Windows PowerShell from unrolling the IDictionary when this
    # function is consumed by a caller that expects ContainsKey/Keys.
    return ,$found
}

function Escape-XmlText {
    param([Parameter(Mandatory = $true)][string]$Value)
    return [System.Security.SecurityElement]::Escape($Value)
}

function Quote-ProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + $Value.Replace('"', '\"') + '"'
}

$mode = $PSCmdlet.ParameterSetName
$repositoryFull = Resolve-ExistingPath $RepositoryRoot
$harnessFull = Resolve-ExistingPath $PSScriptRoot
$sandboxExe = Join-Path $env:WINDIR "System32\WindowsSandbox.exe"
if (-not (Test-Path -LiteralPath $sandboxExe -PathType Leaf)) {
    throw "Windows Sandbox is not available at '$sandboxExe'. Enable the Windows Sandbox optional feature first."
}

$timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmssZ")
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $outputBase = Join-Path ([IO.Path]::GetTempPath()) "nstu-sandbox-results"
} else {
    if (Test-Path -LiteralPath $OutputRoot -PathType Leaf) {
        throw "OutputRoot must be a directory: $OutputRoot"
    }
    if (Test-Path -LiteralPath $OutputRoot) {
        $outputBase = Resolve-ExistingPath $OutputRoot
    } else {
        $outputBase = [IO.Path]::GetFullPath($OutputRoot)
        New-Item -ItemType Directory -Path $outputBase -Force | Out-Null
    }
}
New-Item -ItemType Directory -Path $outputBase -Force | Out-Null
$outputBase = [IO.Path]::GetFullPath($outputBase)
if (((Test-PathWithin $repositoryFull $outputBase) -or
     (Test-PathWithin $outputBase $repositoryFull))) {
    throw "OutputRoot cannot contain or be contained by RepositoryRoot; use a separate writable results directory."
}
$runRoot = Join-Path $outputBase "run-$timestamp"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null

if (Test-PathWithin $repositoryFull $runRoot) {
    throw "OutputRoot must be outside RepositoryRoot so the repository can remain read-only."
}

if ($mode -eq "Installer") {
    $inputFull = Resolve-ExistingPath $InstallerPath
    if (-not (Test-Path -LiteralPath $inputFull -PathType Leaf)) {
        throw "InstallerPath must identify a file: $inputFull"
    }
    $inputParent = Split-Path -Parent $inputFull
} else {
    $inputFull = Resolve-ExistingPath $PackageRoot
    if (-not (Test-Path -LiteralPath $inputFull -PathType Container)) {
        throw "PackageRoot must identify a directory: $inputFull"
    }
    $inputParent = $inputFull
}

if (Test-PathWithin $inputParent $runRoot) {
    throw "OutputRoot cannot be inside the mapped artifact source."
}
if (Test-PathWithin $runRoot $inputParent) {
    throw "The mapped artifact source cannot be inside OutputRoot."
}

$runtimeCandidates = New-Object System.Collections.Generic.List[string]
if (-not [string]::IsNullOrWhiteSpace($RuntimeRoot)) {
    [void]$runtimeCandidates.Add((Resolve-ExistingPath $RuntimeRoot))
}
foreach ($candidate in @(
        (Join-Path $env:SystemDrive "msys64\ucrt64\bin"),
        (Join-Path $env:SystemDrive "msys64\mingw64\bin")
    )) {
    if (-not $runtimeCandidates.Contains($candidate)) {
        [void]$runtimeCandidates.Add($candidate)
    }
}
[void]$runtimeCandidates.Add($inputParent)

$runtimeFiles = Find-RuntimeFiles -SearchRoots $runtimeCandidates.ToArray() `
    -Names $RequiredRuntimeNames
$runtimeFileNames = @()
$missingRuntime = @()
foreach ($runtimeName in $RequiredRuntimeNames) {
    if ($runtimeFiles.ContainsKey($runtimeName)) {
        $runtimeFileNames += $runtimeName
    } else {
        $missingRuntime += $runtimeName
    }
}
if ($mode -eq "Package" -and $missingRuntime.Count -gt 0) {
    throw ("Package mode requires the MinGW runtime DLLs beside the client " +
        "payload or in -RuntimeRoot. Missing: " +
        ($missingRuntime -join ", ") + ". Provide -RuntimeRoot (for example " +
        "C:\msys64\ucrt64\bin) or build a self-contained package.")
}

$runtimeStage = $null
if ($runtimeFiles.Count -gt 0) {
    $runtimeStage = Join-Path $outputBase "runtime-$timestamp"
    New-Item -ItemType Directory -Path $runtimeStage -Force | Out-Null
    foreach ($name in $runtimeFiles.Keys) {
        Copy-Item -LiteralPath $runtimeFiles[$name] `
            -Destination (Join-Path $runtimeStage $name) -Force
    }
}

if (($null -ne $runtimeStage) -and
    ((Test-PathWithin $runRoot $runtimeStage) -or
     (Test-PathWithin $runtimeStage $runRoot))) {
    throw "The staged runtime directory overlaps the writable results directory."
}

$mappings = New-Object System.Collections.Generic.List[object]
$guestRepository = "C:\NSTU-Sandbox\Repository"
$guestInput = $null

Add-MappedFolder -Mappings $mappings -HostFolder $repositoryFull `
    -GuestFolder $guestRepository -ReadOnly $true

$harnessGuest = $null
if (Test-PathWithin $repositoryFull $harnessFull) {
    $relativeHarness = Get-RelativePath $repositoryFull $harnessFull
    if ([string]::IsNullOrWhiteSpace($relativeHarness)) {
        $harnessGuest = $guestRepository
    } else {
        $harnessGuest = Join-Path $guestRepository $relativeHarness
    }
} else {
    $harnessGuest = "C:\NSTU-Sandbox\Harness"
    Add-MappedFolder -Mappings $mappings -HostFolder $harnessFull `
        -GuestFolder $harnessGuest -ReadOnly $true
}

if (Test-PathWithin $repositoryFull $inputFull) {
    $relativeInput = Get-RelativePath $repositoryFull $inputFull
    if ([string]::IsNullOrWhiteSpace($relativeInput)) {
        $guestInput = $guestRepository
    } else {
        $guestInput = Join-Path $guestRepository $relativeInput
    }
} else {
    $guestInput = "C:\NSTU-Sandbox\Input"
    Add-MappedFolder -Mappings $mappings -HostFolder $inputParent `
        -GuestFolder $guestInput -ReadOnly $true
    if ($mode -eq "Installer") {
        $guestInput = Join-Path $guestInput (Split-Path -Leaf $inputFull)
    }
}

if ($null -ne $runtimeStage) {
    Add-MappedFolder -Mappings $mappings -HostFolder $runtimeStage `
        -GuestFolder "C:\NSTU-Sandbox\Runtime" -ReadOnly $true
}
Add-MappedFolder -Mappings $mappings -HostFolder $runRoot `
    -GuestFolder "C:\NSTU-Sandbox\Output" -ReadOnly $false

$guestScript = Join-Path $harnessGuest "run-client-lifecycle-in-sandbox.ps1"
$guestCommand = @(
    "powershell.exe",
    "-NoLogo",
    "-NoProfile",
    "-ExecutionPolicy Bypass",
    "-File `"$guestScript`"",
    "-SandboxMarker `"$ExpectedMarker`"",
    "-Mode `"$mode`"",
        "-InputPath `"$guestInput`"",
    "-RepositoryRoot `"$guestRepository`"",
    "-OutputRoot `"C:\NSTU-Sandbox\Output`"",
    "-InstallRoot `"C:\NSTU-Sandbox\Installed`"",
    "-DataRoot `"C:\ProgramData\NSTU-Sandbox`""
) -join " "
if (-not $KeepSandboxOpen) {
    $guestCommand += " -ShutdownWhenComplete"
}

$xmlSettings = New-Object System.Xml.XmlWriterSettings
$xmlSettings.Indent = $true
$xmlSettings.Encoding = New-Object System.Text.UTF8Encoding($false)
$xmlSettings.OmitXmlDeclaration = $true
$xmlBuilder = New-Object System.Text.StringBuilder
$xmlWriter = [System.Xml.XmlWriter]::Create($xmlBuilder, $xmlSettings)
$xmlWriter.WriteStartElement("Configuration")
$xmlWriter.WriteElementString("vGPU", "Disable")
$xmlWriter.WriteElementString("Networking", "Disable")
$xmlWriter.WriteElementString("MemoryInMB", [string]$MemoryInMB)
$xmlWriter.WriteStartElement("MappedFolders")
foreach ($mapping in $mappings) {
    $xmlWriter.WriteStartElement("MappedFolder")
    $xmlWriter.WriteElementString("HostFolder", $mapping.HostFolder)
    $xmlWriter.WriteElementString("SandboxFolder", $mapping.GuestFolder)
    $xmlWriter.WriteElementString("ReadOnly", ([string]$mapping.ReadOnly).ToLowerInvariant())
    $xmlWriter.WriteEndElement()
}
$xmlWriter.WriteEndElement()
$xmlWriter.WriteStartElement("LogonCommand")
$xmlWriter.WriteElementString("Command", $guestCommand)
$xmlWriter.WriteEndElement()
$xmlWriter.WriteEndElement()
$xmlWriter.Flush()
$xmlWriter.Close()

$configPath = Join-Path $runRoot "nstu-client-lifecycle.wsb"
$xmlText = $xmlBuilder.ToString() -replace 'encoding="utf-16"', 'encoding="utf-8"'
[IO.File]::WriteAllText($configPath, $xmlText,
    (New-Object System.Text.UTF8Encoding($false)))

$hostManifest = [ordered]@{
    schema = 1
    generated_utc = [DateTimeOffset]::UtcNow.ToString("o")
    marker = $ExpectedMarker
    mode = $mode
    host = [ordered]@{
        repository_root = $repositoryFull
        input_path = $inputFull
        output_root = $runRoot
        runtime_root_candidates = $runtimeCandidates.ToArray()
        runtime_staged = ($null -ne $runtimeStage)
        runtime_files = $runtimeFileNames
        runtime_missing = $missingRuntime
    }
    guest = [ordered]@{
        repository_root = $guestRepository
        input_path = $guestInput
        output_root = "C:\NSTU-Sandbox\Output"
        install_root = "C:\NSTU-Sandbox\Installed"
        data_root = "C:\ProgramData\NSTU-Sandbox"
        script = $guestScript
    }
    sandbox = [ordered]@{
        executable = $sandboxExe
        memory_mb = $MemoryInMB
        networking = "Disabled"
        vgpu = "Disabled"
        reboot_validation = "Not available in Windows Sandbox"
    }
    mapped_folders = $mappings.ToArray()
    config_path = $configPath
    launch_requested = (-not $NoLaunch)
        keep_sandbox_open = [bool]$KeepSandboxOpen
}
$manifestPath = Join-Path $runRoot "host-manifest.json"
$hostManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath `
    -Encoding UTF8

Write-Host "Windows Sandbox configuration written to: $configPath"
Write-Host "Results will be written to: $runRoot"
if ($NoLaunch) {
    Write-Host "-NoLaunch specified; inspect the .wsb file and launch it manually."
    exit 0
}

try {
    $sandboxProcess = Start-Process -FilePath $sandboxExe `
        -ArgumentList (Quote-ProcessArgument $configPath) -Wait -PassThru
    $hostManifest.sandbox.process_exit_code = $sandboxProcess.ExitCode
    $hostManifest.sandbox.completed_utc = [DateTimeOffset]::UtcNow.ToString("o")
    $hostManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath `
        -Encoding UTF8
} catch {
    $hostManifest.sandbox.launch_error = $_.Exception.Message
    $hostManifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath `
        -Encoding UTF8
    throw
}

$summaryPath = Join-Path $runRoot "summary.json"
if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
    Write-Host "Guest summary:"
    Get-Content -LiteralPath $summaryPath
    try {
        $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        if ($summary.final_status -eq "Failed" -or
            [int]$summary.failure_count -gt 0) {
            exit 1
        }
    } catch {
        Write-Warning ("The guest summary could not be parsed: " + $_.Exception.Message)
        exit 1
    }
} else {
    Write-Warning "The Sandbox exited without producing summary.json. Check the generated transcript and Windows Sandbox event log."
    exit 2
}

exit 0
