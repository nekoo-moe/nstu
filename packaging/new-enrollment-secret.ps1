param(
    [Parameter(Mandatory = $true)]
    [string]$ExportPath,
    [string]$DataRoot = ""
)

$ErrorActionPreference = "Stop"
if ($null -eq ("System.Security.Cryptography.ProtectedData" -as [type])) {
    Add-Type -AssemblyName System.Security -ErrorAction Stop
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Creating the NSTU enrollment secret requires Administrator privileges."
}
if ([string]::IsNullOrWhiteSpace($DataRoot)) {
    $DataRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\NSTU" -Name DataRoot -ErrorAction SilentlyContinue).DataRoot
}
if ([string]::IsNullOrWhiteSpace($DataRoot)) {
    $DataRoot = Join-Path $env:ProgramData "NSTU"
}
New-Item -ItemType Directory -Path $DataRoot -Force | Out-Null

$secret = [byte[]]::new(32)
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
try {
    $rng.GetBytes($secret)
} finally {
    $rng.Dispose()
}
try {
    $protected = [System.Security.Cryptography.ProtectedData]::Protect(
        $secret, $null, [System.Security.Cryptography.DataProtectionScope]::LocalMachine)
    $protectedPath = Join-Path $DataRoot "server-enrollment.bin"
    [IO.File]::WriteAllBytes($protectedPath, $protected)
    [IO.File]::WriteAllBytes([IO.Path]::GetFullPath($ExportPath), $secret)

    foreach ($path in @($protectedPath, [IO.Path]::GetFullPath($ExportPath))) {
        $acl = [Security.AccessControl.FileSecurity]::new()
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($account in @(
            "NT AUTHORITY\SYSTEM",
            "BUILTIN\Administrators",
            $identity.Name)) {
            $rule = [Security.AccessControl.FileSystemAccessRule]::new(
                $account, [Security.AccessControl.FileSystemRights]::FullControl,
                [Security.AccessControl.AccessControlType]::Allow)
            $acl.AddAccessRule($rule)
        }
        Set-Acl -LiteralPath $path -AclObject $acl
    }
    Write-Host "Server enrollment secret installed in $protectedPath"
    Write-Host "One-time client enrollment secret exported to $ExportPath"
    Write-Warning "Distribute the export securely and delete every copy after enrollment."
} finally {
    [Array]::Clear($secret, 0, $secret.Length)
}
