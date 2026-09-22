param(
    [switch]$RequireSignedArtifacts,
    [switch]$AllowActiveDeepFreeze
)

$ErrorActionPreference = "Stop"
$failures = [Collections.Generic.List[string]]::new()
$dataRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\NSTU" -Name DataRoot -ErrorAction SilentlyContinue).DataRoot
if ([string]::IsNullOrWhiteSpace($dataRoot)) {
    $dataRoot = Join-Path $env:ProgramData "NSTU"
}
if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
    $failures.Add("NSTU data root is missing: $dataRoot")
}

$deepFreezeNames = @("DFServ", "DeepFrz")
$activeDeepFreeze = @(Get-Service -Name $deepFreezeNames -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Status -ne [ServiceProcess.ServiceControllerStatus]::Stopped -or
        $_.StartType -ne [ServiceProcess.ServiceStartMode]::Disabled
    })
if ($activeDeepFreeze.Count -gt 0 -and -not $AllowActiveDeepFreeze) {
    $names = ($activeDeepFreeze | Select-Object -ExpandProperty Name) -join ", "
    $failures.Add("Deep Freeze appears active ($names); validate only from a Thawed boot.")
}

$installRoots = @(
    (Join-Path $env:ProgramFiles "NSTU\client"),
    (Join-Path $env:ProgramFiles "NSTU\server"))
if ($RequireSignedArtifacts) {
    $expectedBinaries = @(
        (Join-Path $installRoots[0] "nstu-service.exe"),
        (Join-Path $installRoots[0] "nstu-agent.exe"),
        (Join-Path $installRoots[1] "nstu-server.exe"))
    foreach ($path in $expectedBinaries) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $failures.Add("Required installed binary is missing: $path")
            continue
        }
        $binary = Get-Item -LiteralPath $path
        $signature = Get-AuthenticodeSignature -LiteralPath $binary.FullName
        if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
            $failures.Add("Invalid or missing Authenticode signature: $($binary.FullName)")
        }
    }
}

Write-Host "NSTU data root: $dataRoot"
Write-Host "Deep Freeze services detected: $($activeDeepFreeze.Count)"
if ($failures.Count -ne 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Host "Deployment validation passed for the checks available on this machine."
