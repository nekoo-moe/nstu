param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Client", "Server")]
    [string]$Role,
    [string]$InstallRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $scriptRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd("\")
    $scriptLeaf = Split-Path -Leaf $scriptRoot
    $parentRoot = Split-Path -Parent $scriptRoot
    $parentLeaf = Split-Path -Leaf $parentRoot
    if ([string]::Equals($scriptLeaf, "deployment",
                         [StringComparison]::OrdinalIgnoreCase) -and
        [string]::Equals($parentLeaf, "docs",
                         [StringComparison]::OrdinalIgnoreCase)) {
        $InstallRoot = Join-Path $scriptRoot "..\.."
    } elseif ([string]::Equals($scriptLeaf, "packaging",
                               [StringComparison]::OrdinalIgnoreCase)) {
        $InstallRoot = Join-Path $scriptRoot ".."
    } else {
        throw "InstallRoot must be supplied when this script is outside the packaging or installed docs\deployment layout."
    }
}

$resolvedRoot = [IO.Path]::GetFullPath($InstallRoot)
$servicePresent = $false
$service = $null
try {
    try {
        $service = Get-Service -Name "nstu-service" -ErrorAction Stop
        $servicePresent = $null -ne $service
    } catch {
        if ($_.FullyQualifiedErrorId -notlike "NoServiceFoundForGivenName*") {
            throw "Unable to query nstu-service: $($_.Exception.Message)"
        }
    }
} finally {
    if ($null -ne $service) {
        $service.Dispose()
    }
}
$clientBinary = Join-Path $resolvedRoot "client\nstu-service.exe"
$agentBinary = Join-Path $resolvedRoot "client\nstu-agent.exe"
$serverBinary = Join-Path $resolvedRoot "server\nstu-server.exe"

if ($Role -eq "Server") {
    if ($servicePresent -or (Test-Path -LiteralPath $clientBinary) -or
        (Test-Path -LiteralPath $agentBinary)) {
        throw "NSTU Server cannot be installed while an NSTU Client is present. Uninstall the client and restart Windows first."
    }
} else {
    if (Test-Path -LiteralPath $serverBinary) {
        throw "NSTU Client cannot be installed while an NSTU Server is present. Uninstall the server first."
    }
}

Write-Host "NSTU $Role installation role check passed."
