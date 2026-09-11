param(
    [Parameter(Mandatory = $true)]
    [string]$InstallRoot,
    [Parameter(Mandatory = $true)]
    [string]$ServerAddress,
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$ServerPort,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

function New-Result {
    param(
        [string]$Id,
        [string]$Severity,
        [string]$Detail,
        [int]$ErrorCode = 0
    )
    return [ordered]@{
        id = $Id
        severity = $Severity
        detail = $Detail
        error_code = $ErrorCode
    }
}

function Test-TcpEndpoint {
    param(
        [string]$Address,
        [int]$Port,
        [int]$TimeoutMilliseconds = 2500
    )

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($Address, $Port)
        if (-not $connect.Wait($TimeoutMilliseconds)) {
            return $false
        }
        return $client.Connected
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

$results = [Collections.Generic.List[object]]::new()
$resolvedRoot = [IO.Path]::GetFullPath($InstallRoot)
$serviceBinary = [IO.Path]::GetFullPath((Join-Path $resolvedRoot "nstu-service.exe"))

if (-not (Test-Path -LiteralPath $serviceBinary -PathType Leaf)) {
    [void]$results.Add((New-Result "service" "failure" `
        "The installed nstu-service.exe was not found: $serviceBinary" 1))
} else {
    $service = $null
    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='nstu-service'" `
            -ErrorAction Stop
        if ($null -eq $service) {
            [void]$results.Add((New-Result "service" "failure" `
                "The NSTU client service is not registered." 2))
        } else {
            $configuredPath = [IO.Path]::GetFullPath(([string]$service.PathName).Trim('"'))
            $pathMatches = [string]::Equals(
                $configuredPath, $serviceBinary,
                [StringComparison]::OrdinalIgnoreCase)
            $identityMatches = [string]::Equals(
                [string]$service.StartName, "LocalSystem",
                [StringComparison]::OrdinalIgnoreCase) -or
                [string]::Equals(
                    [string]$service.StartName, "NT AUTHORITY\SYSTEM",
                    [StringComparison]::OrdinalIgnoreCase)
            $automatic = [string]::Equals(
                [string]$service.StartMode, "Auto",
                [StringComparison]::OrdinalIgnoreCase)
            if (-not $pathMatches -or -not $identityMatches -or -not $automatic) {
                [void]$results.Add((New-Result "service" "failure" `
                    "NSTU service configuration is not the expected LocalSystem/automatic installation." 3))
            } else {
                [void]$results.Add((New-Result "service" "pass" `
                    "NSTU service is registered as LocalSystem/automatic and points to the installed binary."))
            }
        }
    } catch {
        [void]$results.Add((New-Result "service" "failure" `
            ("Could not query the NSTU service: " + $_.Exception.Message) 4))
    } finally {
        if ($null -ne $service) {
            $service.Dispose()
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ServerAddress)) {
    [void]$results.Add((New-Result "server" "failure" `
        "The client server address is empty." 5))
} elseif (Test-TcpEndpoint -Address $ServerAddress -Port $ServerPort) {
    [void]$results.Add((New-Result "server" "pass" `
        ("NSTU server TCP endpoint is reachable: {0}:{1}." -f $ServerAddress, $ServerPort)))
} else {
    [void]$results.Add((New-Result "server" "failure" `
        ("NSTU server TCP endpoint is not reachable: {0}:{1}." -f $ServerAddress, $ServerPort) 6))
}

$report = [ordered]@{
    role = "client"
    phase = "post_install"
    server_address = $ServerAddress
    server_port = $ServerPort
    thawed_mode_required = $true
    uwf_mutated = $false
    results = @($results)
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $parent = Split-Path -Parent $ReportPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath `
        -Encoding UTF8
}

$results | ForEach-Object {
    Write-Host ("[{0}] {1}: {2}" -f $_.severity.ToUpperInvariant(), $_.id, $_.detail)
}

if (@($results | Where-Object { $_.severity -eq "failure" }).Count -gt 0) {
    exit 1
}
exit 0
