[CmdletBinding()]
param(
    [string]$TrialRoot = "",
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
$createdRoot = $false

if ([string]::IsNullOrWhiteSpace($TrialRoot)) {
    $TrialRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ("nstu-update-trial-" + [guid]::NewGuid().ToString("N"))
    $createdRoot = $true
} else {
    $TrialRoot = [IO.Path]::GetFullPath($TrialRoot)
    if (Test-Path -LiteralPath $TrialRoot) {
        throw "TrialRoot already exists; choose a new temporary directory: $TrialRoot"
    }
}

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
    Write-Host "PASS: $Message"
}

function Get-PayloadHash {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-Payload {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][bool]$Healthy
    )
    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    $path = Join-Path $Directory "nstu-service.exe"
    $health = if ($Healthy) { "pass" } else { "fail" }
    Set-Content -LiteralPath $path -Value @(
        "role=client"
        "version=$Version"
        "health=$health"
    ) -Encoding ASCII
    return $path
}

function Read-PayloadHealth {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Select-String -LiteralPath $Path -Pattern '^health=(.+)$').Matches.Groups[1].Value
}

function Apply-StagedPayload {
    param(
        [Parameter(Mandatory = $true)][string]$Active,
        [Parameter(Mandatory = $true)][string]$Staged,
        [Parameter(Mandatory = $true)][string]$Backup
    )
    if (Test-Path -LiteralPath $Backup) {
        Remove-Item -LiteralPath $Backup -Recurse -Force
    }
    Move-Item -LiteralPath $Active -Destination $Backup
    Move-Item -LiteralPath $Staged -Destination $Active
    $activeBinary = Join-Path $Active "nstu-service.exe"
    if ((Read-PayloadHealth -Path $activeBinary) -ne "pass") {
        Move-Item -LiteralPath $Active -Destination ($Backup + ".failed")
        Move-Item -LiteralPath $Backup -Destination $Active
        Remove-Item -LiteralPath ($Backup + ".failed") -Recurse -Force
        return $false
    }
    return $true
}

try {
    New-Item -ItemType Directory -Path $TrialRoot -Force | Out-Null
    $active = Join-Path $TrialRoot "active"
    $backup = Join-Path $TrialRoot "backup"
    $staged = Join-Path $TrialRoot "staged"
    $incoming = Join-Path $TrialRoot "incoming"

    $currentBinary = New-Payload -Directory $active -Version "0.1.0" -Healthy $true
    $goodBinary = New-Payload -Directory $incoming -Version "0.1.1" -Healthy $true
    $manifestHash = Get-PayloadHash -Path $goodBinary
    $manifest = Join-Path $incoming "manifest.txt"
    Set-Content -LiteralPath $manifest -Value "sha256=$manifestHash" -Encoding ASCII

    $tamperedBinary = Join-Path $incoming "nstu-service.exe"
    Add-Content -LiteralPath $tamperedBinary -Value "tampered=true" -Encoding ASCII
    $observedTamperedHash = Get-PayloadHash -Path $tamperedBinary
    Assert-Condition ($observedTamperedHash -ne $manifestHash) `
        "tampered payload is rejected by SHA-256 verification"

    $goodBinary = New-Payload -Directory $staged -Version "0.1.1" -Healthy $true
    $stagedHash = Get-PayloadHash -Path $goodBinary
    Assert-Condition ($stagedHash -eq $manifestHash) `
        "verified payload hash matches the release manifest"
    Assert-Condition (Apply-StagedPayload -Active $active -Staged $staged -Backup $backup) `
        "healthy payload stages and passes the post-reboot health check"

    $badStaged = Join-Path $TrialRoot "staged-bad"
    New-Payload -Directory $badStaged -Version "0.1.2" -Healthy $false | Out-Null
    Assert-Condition (-not (Apply-StagedPayload -Active $active -Staged $badStaged -Backup $backup)) `
        "failed health check rolls back to the previous payload"
    $activeAfterRollback = Join-Path $active "nstu-service.exe"
    Assert-Condition ((Select-String -LiteralPath $activeAfterRollback -Pattern '^version=0.1.1$') -ne $null) `
        "rollback preserves the last healthy version"

    Write-Host "NSTU update-cycle trial passed. No service, reboot, installed path, network endpoint, or Deep Freeze state was touched."
} finally {
    if ($createdRoot -and -not $KeepArtifacts -and (Test-Path -LiteralPath $TrialRoot)) {
        Remove-Item -LiteralPath $TrialRoot -Recurse -Force
    }
}
