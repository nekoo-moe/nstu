#requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SandboxMarker,

    [Parameter(Mandatory = $true)]
    [ValidateSet("Installer", "Package")]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$InstallRoot,

    [string]$DataRoot = "C:\ProgramData\NSTU-Sandbox",

    [switch]$ShutdownWhenComplete
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Set-Variable -Name ExpectedMarker -Option ReadOnly -Value `
    "NSTU-WINDOWS-SANDBOX-CLIENT-LIFECYCLE-v1"
Set-Variable -Name RequiredRuntimeNames -Option ReadOnly -Value @(
    "libstdc++-6.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll"
)

$script:Results = New-Object System.Collections.Generic.List[object]
$script:FailureCount = 0
$script:SkipCount = 0
$script:TranscriptStarted = $false
$script:TranscriptPath = $null
$script:EffectiveOutputRoot = $null
$script:MappedOutputWritable = $false
$script:SandboxValidated = $false
$script:StartedUtc = [DateTimeOffset]::UtcNow

function Get-UtcText {
    return [DateTimeOffset]::UtcNow.ToString("o")
}

function Add-StepResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)]
        [ValidateSet("Pass", "Fail", "Skip", "Info")]
        [string]$Status,
        [Parameter(Mandatory = $true)][string]$Message,
        [object]$Data = $null
    )

    $item = [ordered]@{
        name = $Name
        status = $Status
        message = $Message
        utc = Get-UtcText
    }
    if ($null -ne $Data) {
        $item.data = $Data
    }
    [void]$script:Results.Add([pscustomobject]$item)
    if ($Status -eq "Fail") {
        $script:FailureCount++
    } elseif ($Status -eq "Skip") {
        $script:SkipCount++
    }
    Write-Host ("[{0}] {1}: {2}" -f $Status.ToUpperInvariant(), $Name, $Message)
}

function ConvertTo-SafeText {
    param(
        [AllowNull()][object]$Value,
        [int]$MaximumLength = 4000
    )

    if ($null -eq $Value) {
        return ""
    }
    $text = [string]$Value
    if ($text.Length -le $MaximumLength) {
        return $text
    }
    return $text.Substring(0, $MaximumLength) + "..."
}

function Get-RegistryPropertySafe {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    try {
        $item = Get-ItemProperty -LiteralPath $Path -Name $Name `
            -ErrorAction Stop
        return [string]$item.$Name
    } catch {
        return ""
    }
}

function Quote-ProcessArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-ChildPowerShell {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [int]$TimeoutSeconds = 120
    )

    $result = [ordered]@{
        name = $Name
        script = $ScriptPath
        exit_code = $null
        stdout = ""
        stderr = ""
        succeeded = $false
    }
    if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
        $result.stderr = "Script was not found: $ScriptPath"
        return [pscustomobject]$result
    }

    $safeName = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $stdoutPath = Join-Path $script:EffectiveOutputRoot ($safeName + ".stdout.txt")
    $stderrPath = Join-Path $script:EffectiveOutputRoot ($safeName + ".stderr.txt")
    $powershellPath = Join-Path $env:SystemRoot `
        "System32\WindowsPowerShell\v1.0\powershell.exe"
    $argumentParts = @(
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        (Quote-ProcessArgument $ScriptPath)
    )
    foreach ($argument in $Arguments) {
        $argumentParts += (Quote-ProcessArgument ([string]$argument))
    }

    try {
        $child = Start-Process -FilePath $powershellPath `
            -ArgumentList ($argumentParts -join " ") `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        if (-not $child.WaitForExit($TimeoutSeconds * 1000)) {
            $child.Kill()
            [void]$child.WaitForExit(5000)
            $result.stderr = "Timed out after $TimeoutSeconds seconds."
        }
        $result.exit_code = $child.ExitCode
        if (Test-Path -LiteralPath $stdoutPath) {
            $result.stdout = ConvertTo-SafeText (Get-Content -LiteralPath $stdoutPath -Raw)
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $result.stderr = ConvertTo-SafeText (Get-Content -LiteralPath $stderrPath -Raw)
        }
        $result.succeeded = ($child.ExitCode -eq 0)
    } catch {
        $result.stderr = $_.Exception.Message
    }
    return [pscustomobject]$result
}

function Invoke-Executable {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [int]$TimeoutSeconds = 120
    )

    $result = [ordered]@{
        name = $Name
        file = $FilePath
        exit_code = $null
        succeeded = $false
        stdout = ""
        stderr = ""
    }
    $safeName = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $stdoutPath = Join-Path $script:EffectiveOutputRoot ($safeName + ".stdout.txt")
    $stderrPath = Join-Path $script:EffectiveOutputRoot ($safeName + ".stderr.txt")
    $argumentParts = @()
    foreach ($argument in $Arguments) {
        $argumentParts += (Quote-ProcessArgument ([string]$argument))
    }
    try {
        $child = Start-Process -FilePath $FilePath `
            -ArgumentList ($argumentParts -join " ") `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        if (-not $child.WaitForExit($TimeoutSeconds * 1000)) {
            $child.Kill()
            [void]$child.WaitForExit(5000)
            $result.stderr = "Timed out after $TimeoutSeconds seconds."
        }
        $result.exit_code = $child.ExitCode
        if (Test-Path -LiteralPath $stdoutPath) {
            $result.stdout = ConvertTo-SafeText (Get-Content -LiteralPath $stdoutPath -Raw)
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $result.stderr = ConvertTo-SafeText (Get-Content -LiteralPath $stderrPath -Raw)
        }
        $result.succeeded = ($child.ExitCode -eq 0)
    } catch {
        $result.stderr = $_.Exception.Message
    }
    return [pscustomobject]$result
}

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Condition,
        [int]$TimeoutSeconds = 15,
        [int]$PollMilliseconds = 250
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $value = & $Condition
            if ($null -ne $value -and [bool]$value) {
                return $value
            }
        } catch {
            # The condition is expected to be transient while a service starts/stops.
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Get-ClientRoot {
    param([Parameter(Mandatory = $true)][string]$Root)

    $direct = Join-Path $Root "nstu-service.exe"
    if (Test-Path -LiteralPath $direct -PathType Leaf) {
        return [IO.Path]::GetFullPath($Root)
    }
    $nested = Join-Path $Root "client\nstu-service.exe"
    if (Test-Path -LiteralPath $nested -PathType Leaf) {
        return [IO.Path]::GetFullPath((Join-Path $Root "client"))
    }
    $match = Get-ChildItem -LiteralPath $Root -Filter "nstu-service.exe" `
        -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $match) {
        return [IO.Path]::GetFullPath($match.DirectoryName)
    }
    return $null
}

function Get-AgentProcesses {
    param([Parameter(Mandatory = $true)][string]$ExpectedRoot)

    $expected = [IO.Path]::GetFullPath((Join-Path $ExpectedRoot "nstu-agent.exe"))
    $matches = @()
    foreach ($process in @(Get-Process -Name "nstu-agent" `
            -ErrorAction SilentlyContinue)) {
        try {
            $path = [IO.Path]::GetFullPath($process.Path)
            if ([string]::Equals($path, $expected,
                    [StringComparison]::OrdinalIgnoreCase)) {
                $matches += [pscustomobject][ordered]@{
                    ProcessId = [uint32]$process.Id
                    SessionId = [int]$process.SessionId
                    ExecutablePath = $path
                }
            }
        } catch {
            # Ignore processes whose image path cannot be verified.
        } finally {
            $process.Dispose()
        }
    }
    return $matches
}

function Get-ProcessSnapshotById {
    param([Parameter(Mandatory = $true)][uint32]$ProcessId)

    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        return $null
    }
    try {
        return [pscustomobject][ordered]@{
            ProcessId = [uint32]$process.Id
            SessionId = [int]$process.SessionId
            ExecutablePath = [string]$process.Path
        }
    } finally {
        $process.Dispose()
    }
}

function Get-ServiceSnapshot {
    param([Parameter(Mandatory = $true)][string]$Name)

    try {
        $service = Get-CimInstance Win32_Service -Filter "Name='$Name'" `
            -ErrorAction Stop
        if ($null -ne $service) {
            return $service
        }
    } catch {
        # Fall back to SCM state plus its protected registry configuration.
    }

    $controller = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($null -eq $controller) {
        return $null
    }
    try {
        $registryPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$Name"
        $registry = Get-ItemProperty -LiteralPath $registryPath -ErrorAction Stop
        $startMode = switch ([int]$registry.Start) {
            2 { "Auto" }
            3 { "Manual" }
            4 { "Disabled" }
            default { "Unknown" }
        }
        $serviceProcess = Get-Process -Name $Name -ErrorAction SilentlyContinue |
            Select-Object -First 1
        $processId = 0
        if ($null -ne $serviceProcess) {
            try { $processId = [uint32]$serviceProcess.Id } finally {
                $serviceProcess.Dispose()
            }
        }
        return [pscustomobject][ordered]@{
            Name = $Name
            State = [string]$controller.Status
            StartMode = $startMode
            StartName = [string]$registry.ObjectName
            ProcessId = $processId
            PathName = [string]$registry.ImagePath
        }
    } finally {
        $controller.Dispose()
    }
}

function Get-AuthoritativeServiceProcessId {
    param([Parameter(Mandatory = $true)][string]$Name)

    # Prefer the SCM's own extended query.  A process-name lookup is not
    # authoritative because another executable can legitimately share the
    # same image name, and a PID can be reused after a service transition.
    try {
        $query = @(Get-CimInstance Win32_Service -Filter ("Name='{0}'" -f $Name) `
            -ErrorAction Stop | Select-Object -First 1)
        if ($query.Count -gt 0 -and [uint32]$query[0].ProcessId -ne 0) {
            return [uint32]$query[0].ProcessId
        }
    } catch {
        # The Sandbox image can deny the CIM provider; fall through to sc.exe.
    }

    try {
        $scOutput = @(& (Join-Path $env:SystemRoot "System32\sc.exe") `
            queryex $Name 2>&1)
        if ((Test-Path -LiteralPath "variable:LASTEXITCODE") -and
            $LASTEXITCODE -ne 0) {
            return $null
        }
        foreach ($line in $scOutput) {
            if ([string]$line -match '(?i)\bPID\s*:\s*(\d+)') {
                $parsed = [uint64]$Matches[1]
                if ($parsed -gt 0 -and $parsed -le [uint32]::MaxValue) {
                    return [uint32]$parsed
                }
            }
        }
    } catch {
        # Preserve an explicit null so callers can fail closed on ambiguity.
    }
    return $null
}

function Get-ActiveConsoleSessionId {
    try {
        if (-not ("NstuSandboxNativeMethods" -as [type])) {
            [void](Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NstuSandboxNativeMethods {
    [DllImport("kernel32.dll")]
    public static extern UInt32 WTSGetActiveConsoleSessionId();

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WtsSessionInfo {
        public Int32 SessionId;
        public IntPtr WinStationName;
        public Int32 State;
    }

    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode,
        SetLastError = true)]
    private static extern bool WTSEnumerateSessionsW(
        IntPtr hServer, Int32 reserved, Int32 version,
        out IntPtr sessionInfo, out Int32 count);

    [DllImport("wtsapi32.dll")]
    private static extern void WTSFreeMemory(IntPtr memory);

    public static UInt32[] GetInteractiveSessionIds() {
        var ids = new System.Collections.Generic.List<UInt32>();
        IntPtr buffer = IntPtr.Zero;
        Int32 count = 0;
        if (WTSEnumerateSessionsW(IntPtr.Zero, 0, 1, out buffer, out count)) {
            try {
                var size = Marshal.SizeOf(typeof(WtsSessionInfo));
                for (var index = 0; index < count; ++index) {
                    var address = IntPtr.Add(buffer, index * size);
                    var session = (WtsSessionInfo)Marshal.PtrToStructure(
                        address, typeof(WtsSessionInfo));
                    // WTSActive=0 and WTSConnected=1.  Connected sessions
                    // are candidates used by the service during reconnect.
                    if ((session.State == 0 || session.State == 1) &&
                        session.SessionId >= 0 &&
                        !ids.Contains((UInt32)session.SessionId)) {
                        ids.Add((UInt32)session.SessionId);
                    }
                }
            } finally {
                WTSFreeMemory(buffer);
            }
        }
        var console = WTSGetActiveConsoleSessionId();
        if (console != UInt32.MaxValue && !ids.Contains(console)) {
            ids.Insert(0, console);
        }
        return ids.ToArray();
    }
}
"@)
        }
        $sessionId = [NstuSandboxNativeMethods]::WTSGetActiveConsoleSessionId()
        if ($sessionId -eq [UInt32]::MaxValue) {
            return $null
        }
        return [int]$sessionId
    } catch {
        return $null
    }
}

function Get-ActiveInteractiveSessionIds {
    try {
        if (-not ("NstuSandboxNativeMethods" -as [type])) {
            # Loading the type through Get-ActiveConsoleSessionId also keeps
            # this function compatible with Windows PowerShell 5.1.
            [void](Get-ActiveConsoleSessionId)
        }
        return @([NstuSandboxNativeMethods]::GetInteractiveSessionIds() |
            ForEach-Object { [int]$_ })
    } catch {
        $console = Get-ActiveConsoleSessionId
        if ($null -eq $console) {
            return @()
        }
        return @([int]$console)
    }
}

function Get-ProcessOwnerInfo {
    param([Parameter(Mandatory = $true)][uint32]$ProcessId)

    try {
        $process = Get-CimInstance Win32_Process `
            -Filter ("ProcessId = {0}" -f $ProcessId) -ErrorAction Stop |
            Select-Object -First 1
        if ($null -eq $process) {
            return [pscustomobject][ordered]@{
                available = $false
                owner = $null
                error = "The process no longer exists."
            }
        }
        $ownerResult = Invoke-CimMethod -InputObject $process `
            -MethodName GetOwner -ErrorAction Stop
        if ($null -eq $ownerResult -or $ownerResult.ReturnValue -ne 0 -or
            [string]::IsNullOrWhiteSpace($ownerResult.User)) {
            return [pscustomobject][ordered]@{
                available = $false
                owner = $null
                error = "Win32_Process.GetOwner returned no owner."
            }
        }
        $owner = [string]$ownerResult.User
        if (-not [string]::IsNullOrWhiteSpace($ownerResult.Domain)) {
            $owner = "{0}\{1}" -f $ownerResult.Domain, $ownerResult.User
        }
        return [pscustomobject][ordered]@{
            available = $true
            owner = $owner
            error = $null
        }
    } catch {
        $cimError = $_.Exception.Message
        try {
            $fallback = Get-Process -Id $ProcessId -IncludeUserName `
                -ErrorAction Stop
            try {
                if (-not [string]::IsNullOrWhiteSpace($fallback.UserName)) {
                    return [pscustomobject][ordered]@{
                        available = $true
                        owner = [string]$fallback.UserName
                        error = $null
                    }
                }
            } finally {
                $fallback.Dispose()
            }
        } catch {
            return [pscustomobject][ordered]@{
                available = $false
                owner = $null
                error = "$cimError Fallback: $($_.Exception.Message)"
            }
        }
        return [pscustomobject][ordered]@{
            available = $false
            owner = $null
            error = $cimError
        }
    }
}

function Test-SystemOwner {
    param([AllowNull()][string]$Owner)

    if ([string]::IsNullOrWhiteSpace($Owner)) {
        return $false
    }
    return $Owner.Trim() -in @("SYSTEM", "NT AUTHORITY\SYSTEM")
}

function Get-AgentSessionReport {
    param(
        [AllowNull()][object[]]$Agents,
        [AllowNull()][object[]]$ActiveSessionIds
    )

    $activeIds = @($ActiveSessionIds | Where-Object { $null -ne $_ } |
        ForEach-Object { [int]$_ } | Select-Object -Unique)
    $records = @()
    foreach ($agent in @($Agents | Where-Object { $null -ne $_ })) {
        $sessionId = [int]$agent.SessionId
        $activeMatch = $null
        if ($activeIds.Count -gt 0) {
            $activeMatch = ($activeIds -contains $sessionId)
        }
        $records += [pscustomobject][ordered]@{
            process_id = [uint32]$agent.ProcessId
            session_id = $sessionId
            outside_session0 = ($sessionId -ne 0)
            active_session_match = $activeMatch
        }
    }
    $invalid = @($records | Where-Object {
        -not $_.outside_session0 -or
        ($activeIds.Count -gt 0 -and -not $_.active_session_match)
    })
    return [pscustomobject][ordered]@{
        valid = ($records.Count -gt 0 -and $invalid.Count -eq 0)
        active_interactive_session_ids = $activeIds
        records = $records
    }
}

function Get-PendingRenameOperations {
    $path = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager"
    try {
        $property = Get-ItemProperty -LiteralPath $path `
            -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
        if ($null -eq $property) {
            return @()
        }
        return @($property.PendingFileRenameOperations)
    } catch {
        return @()
    }
}

function Normalize-PendingPath {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )
    $normalized = $Value.Trim()
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        return ""
    }
    $normalized = $normalized -replace '^\\\?\?\\', ''
    $normalized = $normalized -replace '^\\\\\?\\', ''
    return $normalized.TrimEnd("\").ToLowerInvariant()
}

function Ensure-ClientScripts {
    param([Parameter(Mandatory = $true)][string]$ClientRoot)

    $scriptNames = @(
        "install-client-service.ps1",
        "uninstall-client-service.ps1",
        "configure-data-root.ps1",
        "test-system-setup.ps1"
    )
    $sourceRoots = @(
        (Join-Path $RepositoryRoot "packaging"),
        (Join-Path $RepositoryRoot "docs\deployment")
    )
    foreach ($name in $scriptNames) {
        $destination = Join-Path $ClientRoot $name
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            continue
        }
        $source = $null
        foreach ($sourceRoot in $sourceRoots) {
            $candidate = Join-Path $sourceRoot $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $source = $candidate
                break
            }
        }
        if ($null -eq $source) {
            Add-StepResult -Name ("stage-script-" + $name) -Status "Fail" `
                -Message "Required lifecycle script is missing: $name"
            continue
        }
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
}

function Create-StandardUserProbe {
    param(
        [Parameter(Mandatory = $true)][string]$ProbePath,
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$ExpectedClientRoot,
        [Parameter(Mandatory = $true)][uint32]$ServiceProcessId
    )

    $probe = @'
param(
    [Parameter(Mandatory = $true)][string]$ResultPath,
    [Parameter(Mandatory = $true)][string]$ExpectedClientRoot,
    [Parameter(Mandatory = $true)][uint32]$ServiceProcessId
)
$ErrorActionPreference = "Continue"
$report = [ordered]@{
    service_pid = $ServiceProcessId
    stop_service = "not-run"
    delete_service = "not-run"
    control_lock = "not-run"
    control_unlock = "not-run"
    terminate_service_process = "not-run"
    terminate_agent = "not-run"
}
try {
    Stop-Service -Name "nstu-service" -ErrorAction Stop
    $report.stop_service = "succeeded"
} catch {
    $message = $_.Exception.Message
    if ($message -match '(?i)(access is denied|unauthorized|not permitted)') {
        $report.stop_service = "denied:$message"
    } else {
        $report.stop_service = "ambiguous:$message"
    }
}
try {
    & sc.exe delete nstu-service *> $null
    if ($LASTEXITCODE -eq 0) {
        $report.delete_service = "succeeded"
    } elseif ($LASTEXITCODE -eq 5) {
        $report.delete_service = "denied:exit:$LASTEXITCODE"
    } else {
        $report.delete_service = "ambiguous:exit:$LASTEXITCODE"
    }
} catch {
    $report.delete_service = "ambiguous:$($_.Exception.Message)"
}
try {
    & sc.exe control nstu-service 128 *> $null
    if ($LASTEXITCODE -eq 0) {
        $report.control_lock = "succeeded"
    } elseif ($LASTEXITCODE -eq 5) {
        $report.control_lock = "denied:exit:$LASTEXITCODE"
    } else {
        $report.control_lock = "ambiguous:exit:$LASTEXITCODE"
    }
} catch {
    $report.control_lock = "ambiguous:$($_.Exception.Message)"
}
try {
    & sc.exe control nstu-service 129 *> $null
    if ($LASTEXITCODE -eq 0) {
        $report.control_unlock = "succeeded"
    } elseif ($LASTEXITCODE -eq 5) {
        $report.control_unlock = "denied:exit:$LASTEXITCODE"
    } else {
        $report.control_unlock = "ambiguous:exit:$LASTEXITCODE"
    }
} catch {
    $report.control_unlock = "ambiguous:$($_.Exception.Message)"
}
try {
    if ($ServiceProcessId -eq 0) {
        $report.terminate_service_process = "ambiguous:service PID was zero"
    } else {
        $serviceProcess = Get-Process -Id $ServiceProcessId -ErrorAction Stop
        if ($null -eq $serviceProcess) {
            $report.terminate_service_process = "ambiguous:service PID was not found"
        } else {
            try {
                Stop-Process -Id $ServiceProcessId -Force -ErrorAction Stop
                $report.terminate_service_process = "succeeded"
            } finally {
                $serviceProcess.Dispose()
            }
        }
    }
} catch {
    $message = $_.Exception.Message
    if ($message -match '(?i)(access is denied|unauthorized|not permitted)') {
        $report.terminate_service_process = "denied:$message"
    } else {
        $report.terminate_service_process = "ambiguous:$message"
    }
}
try {
    $expected = [IO.Path]::GetFullPath((Join-Path $ExpectedClientRoot "nstu-agent.exe"))
    $agents = @(Get-Process -Name "nstu-agent" -ErrorAction SilentlyContinue)
    $terminated = $false
    foreach ($agent in $agents) {
        if (-not [string]::IsNullOrWhiteSpace($agent.Path) -and
            [string]::Equals([IO.Path]::GetFullPath($agent.Path), $expected,
                [StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $agent.Id -Force -ErrorAction Stop
            $terminated = $true
        }
    }
    if ($terminated) {
        $report.terminate_agent = "succeeded"
    } else {
        $report.terminate_agent = "no-process"
    }
} catch {
    $report.terminate_agent = $_.Exception.Message
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ResultPath -Encoding UTF8
$serviceOperations = @(
    [string]$report.stop_service,
    [string]$report.delete_service,
    [string]$report.control_lock,
    [string]$report.control_unlock,
    [string]$report.terminate_service_process
)
$dangerousOperation = @($serviceOperations | Where-Object {
    $_.StartsWith("succeeded", [StringComparison]::OrdinalIgnoreCase)
}).Count -gt 0
$ambiguousOperation = @($serviceOperations | Where-Object {
    $_.StartsWith("ambiguous:", [StringComparison]::OrdinalIgnoreCase) -or
    $_ -eq "not-run"
}).Count -gt 0
if ($dangerousOperation -or $ambiguousOperation) {
    exit 1
}
exit 0
'@
    Set-Content -LiteralPath $ProbePath -Value $probe -Encoding UTF8
}

function Create-LockHolder {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][string]$LockedPath,
        [Parameter(Mandatory = $true)][string]$ReadyPath
    )

    $holder = @'
param(
    [Parameter(Mandatory = $true)][string]$LockedPath,
    [Parameter(Mandatory = $true)][string]$ReadyPath
)
$ErrorActionPreference = "Stop"
$stream = [IO.File]::Open($LockedPath, [IO.FileMode]::OpenOrCreate,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
[IO.File]::WriteAllText($ReadyPath, [string]$PID)
try {
    while ($true) {
        Start-Sleep -Seconds 1
    }
} finally {
    $stream.Dispose()
}
'@
    Set-Content -LiteralPath $ScriptPath -Value $holder -Encoding UTF8
    $powershellPath = Join-Path $env:SystemRoot `
        "System32\WindowsPowerShell\v1.0\powershell.exe"
    $arguments = @(
        "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", (Quote-ProcessArgument $ScriptPath),
        "-LockedPath", (Quote-ProcessArgument $LockedPath),
        "-ReadyPath", (Quote-ProcessArgument $ReadyPath)
    )
    return Start-Process -FilePath $powershellPath `
        -ArgumentList ($arguments -join " ") -PassThru -WindowStyle Hidden
}

function Remove-TestUser {
    param([Parameter(Mandatory = $true)][string]$UserName)
    try {
        if (Get-Command Remove-LocalUser -ErrorAction SilentlyContinue) {
            Remove-LocalUser -Name $UserName -ErrorAction SilentlyContinue
        } else {
            & net.exe user $UserName /delete *> $null
        }
    } catch {
        # The guest is disposable; leave cleanup details in the transcript.
    }
}

$exitCode = 1
$localOutputRoot = Join-Path $env:TEMP ("nstu-sandbox-local-" + $PID)
$transcriptPath = $null
$clientRoot = $null
$serviceStarted = $false
$serviceVerifiedRunning = $false
$serviceProcessId = 0
$serviceProcessIdSource = "unavailable"
$lockProcess = $null
$testUserName = $null
$probePublicRoot = $null

try {
    New-Item -ItemType Directory -Path $localOutputRoot -Force | Out-Null
    try {
        New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
        $probePath = Join-Path $OutputRoot ".write-probe-$PID"
        Set-Content -LiteralPath $probePath -Value "ok" -Encoding ASCII
        Remove-Item -LiteralPath $probePath -Force
        $script:EffectiveOutputRoot = [IO.Path]::GetFullPath($OutputRoot)
        $script:MappedOutputWritable = $true
    } catch {
        $script:EffectiveOutputRoot = $localOutputRoot
        Add-StepResult -Name "mapped-output" -Status "Fail" `
            -Message ("Mapped output is not writable; results remain local: " + $_.Exception.Message)
    }
    New-Item -ItemType Directory -Path $script:EffectiveOutputRoot -Force | Out-Null
    $script:TranscriptPath = Join-Path $script:EffectiveOutputRoot "transcript.txt"
    try {
        Start-Transcript -LiteralPath $script:TranscriptPath -Force | Out-Null
        $script:TranscriptStarted = $true
    } catch {
        Add-StepResult -Name "transcript" -Status "Info" `
            -Message ("Transcript could not be started: " + $_.Exception.Message)
    }

    if ($SandboxMarker -ne $ExpectedMarker) {
        throw "The required sandbox marker is missing or invalid. This harness refuses to run outside its launcher."
    }

    $computer = $null
    $bios = $null
    $inventoryError = ""
    try {
        $computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
        $bios = Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue
    } catch {
        # `wsb exec` can run under a restricted remote provider that denies
        # CIM/WMI access even though the Sandbox LogonCommand is elevated.
        # Fall back to read-only firmware registry values; the virtualization
        # marker and WDAG account checks below remain mandatory.
        $inventoryError = $_.Exception.Message
    }
    $biosManufacturer = ""
    $biosVersion = ""
    if ($null -ne $bios) {
        $biosManufacturer = $bios.Manufacturer
        $biosVersion = $bios.Version
    }
    $systemInformationPath =
        "HKLM:\SYSTEM\CurrentControlSet\Control\SystemInformation"
    $biosInformationPath = "HKLM:\HARDWARE\DESCRIPTION\System\BIOS"
    $registryManufacturer = Get-RegistryPropertySafe `
        -Path $systemInformationPath -Name "SystemManufacturer"
    $registryModel = Get-RegistryPropertySafe `
        -Path $systemInformationPath -Name "SystemProductName"
    if ([string]::IsNullOrWhiteSpace($registryManufacturer)) {
        $registryManufacturer = Get-RegistryPropertySafe `
            -Path $biosInformationPath -Name "SystemManufacturer"
    }
    if ([string]::IsNullOrWhiteSpace($registryModel)) {
        $registryModel = Get-RegistryPropertySafe `
            -Path $biosInformationPath -Name "SystemProductName"
    }
    if ([string]::IsNullOrWhiteSpace($biosManufacturer)) {
        $biosManufacturer = Get-RegistryPropertySafe `
            -Path $biosInformationPath -Name "BIOSVendor"
    }
    if ([string]::IsNullOrWhiteSpace($biosVersion)) {
        $biosVersion = Get-RegistryPropertySafe `
            -Path $biosInformationPath -Name "BIOSVersion"
    }
    $computerManufacturer = $registryManufacturer
    $computerModel = $registryModel
    $hypervisorPresent = $null
    if ($null -ne $computer) {
        $computerManufacturer = [string]$computer.Manufacturer
        $computerModel = [string]$computer.Model
        $hypervisorPresent = $computer.HypervisorPresent
    }
    $virtualText = "{0} {1} {2} {3}" -f $computerManufacturer,
        $computerModel, $biosManufacturer, $biosVersion
    $virtualHardware = $virtualText -match `
        "(?i)(virtual|vmware|virtualbox|qemu|kvm|xen|sandbox)"
    $sandboxAccount = ($env:USERNAME -eq "WDAGUtilityAccount")
    if (-not $virtualHardware -or -not $sandboxAccount) {
        throw "This destructive harness requires the default Windows Sandbox identity (WDAGUtilityAccount plus virtual hardware); detected '$env:USERNAME' / '$virtualText'."
    }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object -TypeName Security.Principal.WindowsPrincipal `
        -ArgumentList $identity
    if (-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "The guest test must run with an elevated administrator token."
    }

    $environmentBios = $null
    if ($null -ne $bios) {
        $environmentBios = [ordered]@{
            manufacturer = $bios.Manufacturer
            version = $bios.SMBIOSBIOSVersion
            serial = $bios.SerialNumber
        }
    }
    $osSnapshot = $null
    try {
        $osSnapshot = Get-CimInstance Win32_OperatingSystem |
            Select-Object Caption, Version, BuildNumber, OSArchitecture
    } catch {
        $osRegistryPath =
            "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
        $architecture = "32-bit"
        if ([Environment]::Is64BitOperatingSystem) {
            $architecture = "64-bit"
        }
        $osSnapshot = [ordered]@{
            Caption = Get-RegistryPropertySafe -Path $osRegistryPath `
                -Name "ProductName"
            Version = Get-RegistryPropertySafe -Path $osRegistryPath `
                -Name "CurrentVersion"
            BuildNumber = Get-RegistryPropertySafe -Path $osRegistryPath `
                -Name "CurrentBuild"
            OSArchitecture = $architecture
        }
    }
    $environment = [ordered]@{
        utc = Get-UtcText
        user = $env:USERNAME
        user_domain = $env:USERDOMAIN
        computer_name = $env:COMPUTERNAME
        manufacturer = $computerManufacturer
        model = $computerModel
        hypervisor_present = $hypervisorPresent
        bios = $environmentBios
        os = $osSnapshot
        data_root = $DataRoot
        install_root = $InstallRoot
        mode = $Mode
        input_path = $InputPath
        mapped_output_root = $OutputRoot
        effective_output_root = $script:EffectiveOutputRoot
    }
    $environment | ConvertTo-Json -Depth 8 | Set-Content `
        -LiteralPath (Join-Path $script:EffectiveOutputRoot "environment.json") `
        -Encoding UTF8
    Add-StepResult -Name "sandbox-identity" -Status "Pass" `
        -Message "Virtual-machine identity and launcher marker validated." `
        -Data ([ordered]@{
            marker = $SandboxMarker
            virtual_hardware = $virtualHardware
            sandbox_account = $sandboxAccount
            hypervisor_present = $hypervisorPresent
        })
    if (-not [string]::IsNullOrWhiteSpace($inventoryError)) {
        Add-StepResult -Name "sandbox-inventory" -Status "Info" `
            -Message ("CIM inventory was unavailable; registry fallback was used: " +
                $inventoryError)
    }
    Add-StepResult -Name "administrator-token" -Status "Pass" `
        -Message "The lifecycle test is running with an administrator token."
    $script:SandboxValidated = $true

    New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
    $workingRoot = Join-Path $script:EffectiveOutputRoot "working"
    New-Item -ItemType Directory -Path $workingRoot -Force | Out-Null

    if ($Mode -eq "Installer") {
        if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
            throw "Mapped installer was not found: $InputPath"
        }
        $localInstaller = Join-Path $workingRoot (Split-Path -Leaf $InputPath)
        Copy-Item -LiteralPath $InputPath -Destination $localInstaller -Force
        $installerResult = Invoke-Executable -Name "client-installer" `
            -FilePath $localInstaller -Arguments @(
                "/S",
                ("/D=" + $InstallRoot)
            )
        if ($installerResult.succeeded) {
            Add-StepResult -Name "install-client" -Status "Pass" `
                -Message "The client installer completed in silent mode." `
                -Data $installerResult
        } else {
            Add-StepResult -Name "install-client" -Status "Fail" `
                -Message "The client installer returned a non-zero exit code." `
                -Data $installerResult
        }
    } else {
        if (-not (Test-Path -LiteralPath $InputPath -PathType Container)) {
            throw "Mapped package directory was not found: $InputPath"
        }
        foreach ($sourceItem in @(Get-ChildItem -LiteralPath $InputPath -Force)) {
            Copy-Item -LiteralPath $sourceItem.FullName -Destination $InstallRoot `
                -Recurse -Force
        }
        Add-StepResult -Name "stage-client" -Status "Pass" `
            -Message "The read-only package mapping was copied to local guest storage."
    }

    $clientRoot = Get-ClientRoot $InstallRoot
    if ([string]::IsNullOrWhiteSpace($clientRoot)) {
        throw "nstu-service.exe was not found below $InstallRoot after installation."
    }
    if ($Mode -eq "Package") {
        Ensure-ClientScripts $clientRoot
    }

    $runtimeRoot = "C:\NSTU-Sandbox\Runtime"
    $runtimeCopied = New-Object System.Collections.Generic.List[string]
    if ($Mode -eq "Package" -and
        (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
        foreach ($runtimeName in $RequiredRuntimeNames) {
            $runtimePath = Join-Path $runtimeRoot $runtimeName
            if (Test-Path -LiteralPath $runtimePath -PathType Leaf) {
                Copy-Item -LiteralPath $runtimePath `
                    -Destination (Join-Path $clientRoot $runtimeName) -Force
                [void]$runtimeCopied.Add($runtimeName)
            }
        }
    }
    $missingRuntime = @($RequiredRuntimeNames | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $clientRoot $_) -PathType Leaf)
    })
    if ($missingRuntime.Count -eq 0) {
        Add-StepResult -Name "runtime-prerequisites" -Status "Pass" `
            -Message "All required MinGW runtime DLLs are beside the client binaries." `
            -Data ([ordered]@{ staged = @($runtimeCopied.ToArray()); missing = @() })
    } elseif ($Mode -eq "Package") {
        Add-StepResult -Name "runtime-prerequisites" -Status "Fail" `
            -Message ("Required MinGW runtime DLLs are missing: " +
                ($missingRuntime -join ", ")) `
            -Data ([ordered]@{ staged = @($runtimeCopied.ToArray()); missing = $missingRuntime })
    } else {
        Add-StepResult -Name "runtime-prerequisites" -Status "Info" `
            -Message ("Installer mode did not stage all optional MinGW runtime DLLs; service startup will determine whether they are required: " +
                ($missingRuntime -join ", ")) `
            -Data ([ordered]@{ staged = @($runtimeCopied.ToArray()); missing = $missingRuntime })
    }

    $serviceBinary = Join-Path $clientRoot "nstu-service.exe"
    $agentBinary = Join-Path $clientRoot "nstu-agent.exe"
    if (-not (Test-Path -LiteralPath $serviceBinary -PathType Leaf) -or
        -not (Test-Path -LiteralPath $agentBinary -PathType Leaf)) {
        throw "Client service/agent binaries are incomplete below $clientRoot."
    }

    $installScript = Join-Path $clientRoot "install-client-service.ps1"
    if ($Mode -eq "Package" -and
        (Test-Path -LiteralPath $installScript -PathType Leaf)) {
        $installResult = Invoke-ChildPowerShell -Name "register-client-service" `
            -ScriptPath $installScript -Arguments @(
                "-InstallRoot", $clientRoot,
                "-DataRoot", $DataRoot
            )
        if (-not $installResult.succeeded -and
            $installResult.stderr -match "parameter cannot be found|A parameter cannot be found") {
            $installResult = Invoke-ChildPowerShell -Name "register-client-service-compat" `
                -ScriptPath $installScript -Arguments @("-InstallRoot", $clientRoot)
        }
        if ($installResult.succeeded) {
            Add-StepResult -Name "register-client-service" -Status "Pass" `
                -Message "The client lifecycle script registered the service." `
                -Data $installResult
        } else {
            Add-StepResult -Name "register-client-service" -Status "Fail" `
                -Message "The client lifecycle script could not register the service." `
                -Data $installResult
        }
    } elseif ($Mode -eq "Installer") {
        Add-StepResult -Name "register-client-service" -Status "Info" `
            -Message "Installer mode leaves service registration to the installer; the resulting SCM state is verified below."
    } else {
        Add-StepResult -Name "register-client-service" -Status "Info" `
            -Message "No lifecycle script was packaged; checking for a pre-registered service."
    }

    $service = Get-ServiceSnapshot -Name "nstu-service"
    if ($null -eq $service) {
        Add-StepResult -Name "service-account" -Status "Fail" `
            -Message "nstu-service is not present in the Service Control Manager."
    } else {
        $servicePath = $service.PathName.Trim()
        if ($servicePath.StartsWith('"')) {
            $closingQuote = $servicePath.IndexOf('"', 1)
            if ($closingQuote -gt 1) {
                $servicePath = $servicePath.Substring(1, $closingQuote - 1)
            }
        } else {
            $servicePath = ($servicePath -split '\s+', 2)[0]
        }
        $servicePath = [IO.Path]::GetFullPath($servicePath)
        $expectedServicePath = [IO.Path]::GetFullPath($serviceBinary)
        $pathMatches = [string]::Equals(
            $servicePath, $expectedServicePath,
            [StringComparison]::OrdinalIgnoreCase)
        $accountMatches = $service.StartName -in @(
            "LocalSystem", "NT AUTHORITY\SYSTEM"
        )
        $autoMatches = $service.StartMode -eq "Auto"
        $serviceData = [ordered]@{
            name = $service.Name
            state = $service.State
            start_mode = $service.StartMode
            start_name = $service.StartName
            process_id = $service.ProcessId
            path_name = $service.PathName
            path_matches = $pathMatches
            account_matches = $accountMatches
            automatic_start = $autoMatches
        }
        if ($accountMatches -and $autoMatches -and $pathMatches) {
            Add-StepResult -Name "service-account" -Status "Pass" `
                -Message "nstu-service is LocalSystem, automatic, and points at the staged binary." `
                -Data $serviceData
        } else {
            Add-StepResult -Name "service-account" -Status "Fail" `
                -Message "Service account, startup mode, or binary path does not match the required policy." `
                -Data $serviceData
        }

        $sdResult = Invoke-Executable -Name "service-security-descriptor" `
            -FilePath (Join-Path $env:SystemRoot "System32\sc.exe") `
            -Arguments @("sdshow", "nstu-service")
        $sddl = ($sdResult.stdout + $sdResult.stderr).Trim()
        $auMatches = [regex]::Matches($sddl, '\(A;;([^;]+);;;AU\)')
        $auRights = @($auMatches | ForEach-Object {
            $_.Groups[1].Value
        })
        $auQueryOnly = ($auRights.Count -gt 0 -and
            (@($auRights | Where-Object { $_ -ne "LCLORC" }).Count -eq 0))
        $sddlData = [ordered]@{
            exit_code = $sdResult.exit_code
            descriptor = ConvertTo-SafeText $sddl 2000
            has_system = ($sddl -match ";;;SY")
            has_admin = ($sddl -match ";;;BA")
            authenticated_user_rights = $auRights
            has_authenticated_query = $auQueryOnly
            authenticated_user_is_query_only = $auQueryOnly
        }
        if ($sdResult.succeeded -and $sddlData.has_system -and
            $sddlData.has_admin -and $sddlData.has_authenticated_query -and
            $sddlData.authenticated_user_is_query_only) {
            Add-StepResult -Name "service-dacl" -Status "Pass" `
                -Message "The service security descriptor contains the expected SYSTEM/Admin/query principals." `
                -Data $sddlData
        } else {
            Add-StepResult -Name "service-dacl" -Status "Fail" `
                -Message "The service security descriptor could not be validated." `
                -Data $sddlData
        }
    }

    try {
        Start-Service -Name "nstu-service" -ErrorAction Stop
        $serviceStarted = $true
        $running = Wait-Until -TimeoutSeconds 20 -Condition {
            $candidate = Get-Service -Name "nstu-service" -ErrorAction SilentlyContinue
            $null -ne $candidate -and $candidate.Status -eq "Running"
        }
        if ($null -ne $running) {
            $serviceVerifiedRunning = $true
            Add-StepResult -Name "service-start" -Status "Pass" `
                -Message "nstu-service reached the Running state."
        } else {
            Add-StepResult -Name "service-start" -Status "Fail" `
                -Message "nstu-service did not reach Running within 20 seconds."
        }
    } catch {
        Add-StepResult -Name "service-start" -Status "Fail" `
            -Message ("Could not start nstu-service: " + $_.Exception.Message)
    }

    $serviceAfterStart = Get-ServiceSnapshot -Name "nstu-service"
    if ($serviceVerifiedRunning) {
        $authoritativePid = Get-AuthoritativeServiceProcessId -Name "nstu-service"
        if ($null -ne $authoritativePid -and [uint32]$authoritativePid -ne 0) {
            $serviceProcessId = [uint32]$authoritativePid
            $serviceProcessIdSource = "scm"
        }
    }
    if ($serviceVerifiedRunning -and $serviceProcessId -ne 0) {
        Add-StepResult -Name "service-process-id" -Status "Pass" `
            -Message ("SCM reported the running service PID {0} ({1})." -f `
                $serviceProcessId, $serviceProcessIdSource) `
            -Data ([ordered]@{
                process_id = $serviceProcessId
                source = $serviceProcessIdSource
            })
        $serviceProcess = Get-ProcessSnapshotById `
            -ProcessId $serviceProcessId
        $serviceSession = $null
        if ($null -ne $serviceProcess) {
            $serviceSession = $serviceProcess.SessionId
        }
        if ($serviceSession -eq 0) {
            Add-StepResult -Name "service-session" -Status "Pass" `
                -Message "The running client service is hosted in Session 0." `
                -Data ([ordered]@{
                    process_id = $serviceProcessId
                    session_id = $serviceSession
                })
        } else {
            Add-StepResult -Name "service-session" -Status "Fail" `
                -Message "The running client service is not in Session 0." `
                -Data ([ordered]@{
                    process_id = $serviceProcessId
                    session_id = $serviceSession
                })
        }

        $serviceOwner = Get-ProcessOwnerInfo -ProcessId $serviceProcessId
        if (-not $serviceOwner.available) {
            Add-StepResult -Name "service-process-owner" -Status "Skip" `
                -Message ("Could not query the running service process owner; " +
                    "the SCM StartName assertion remains authoritative: " +
                    $serviceOwner.error) `
                -Data ([ordered]@{
                    process_id = $serviceProcessId
                    owner = $serviceOwner.owner
                    lookup_error = $serviceOwner.error
                })
        } elseif (Test-SystemOwner -Owner $serviceOwner.owner) {
            Add-StepResult -Name "service-process-owner" -Status "Pass" `
                -Message "The running service process is owned by LocalSystem." `
                -Data ([ordered]@{
                    process_id = $serviceProcessId
                    owner = $serviceOwner.owner
                })
        } else {
            Add-StepResult -Name "service-process-owner" -Status "Fail" `
                -Message "The running service process is not owned by LocalSystem." `
                -Data ([ordered]@{
                    process_id = $serviceProcessId
                    owner = $serviceOwner.owner
                })
        }
    } else {
        if ($serviceVerifiedRunning) {
            Add-StepResult -Name "service-process-id" -Status "Fail" `
                -Message "The service is running but its authoritative SCM PID could not be established; tamper results cannot be trusted."
        }
        Add-StepResult -Name "service-session" -Status "Skip" `
            -Message "No running service process was available for Session 0 validation."
        Add-StepResult -Name "service-process-owner" -Status "Skip" `
            -Message "No running service process was available for owner validation."
    }

    if ($serviceVerifiedRunning) {
        $qfailureResult = Invoke-Executable -Name "service-recovery-policy" `
            -FilePath (Join-Path $env:SystemRoot "System32\sc.exe") `
            -Arguments @("qfailure", "nstu-service")
        $qfailureText = ($qfailureResult.stdout + $qfailureResult.stderr).Trim()
        if ($qfailureResult.succeeded -and $qfailureText -match "(?i)RESTART") {
            Add-StepResult -Name "service-recovery-policy" -Status "Pass" `
                -Message "Service recovery actions include restart." `
                -Data ([ordered]@{
                    exit_code = $qfailureResult.exit_code
                    output = ConvertTo-SafeText $qfailureText 2000
                })
        } else {
            Add-StepResult -Name "service-recovery-policy" -Status "Fail" `
                -Message "Service recovery actions could not be confirmed." `
                -Data ([ordered]@{
                    exit_code = $qfailureResult.exit_code
                    output = ConvertTo-SafeText $qfailureText 2000
                })
        }
    } else {
        Add-StepResult -Name "service-recovery-policy" -Status "Skip" `
            -Message "Service recovery policy was not queried because the service did not start."
    }

    $initialAgents = Wait-Until -TimeoutSeconds 20 -Condition {
        $candidate = @(Get-AgentProcesses $clientRoot)
        if ($candidate.Count -gt 0) { $candidate } else { $null }
    }
    if ($null -eq $initialAgents) {
        $initialAgents = @()
    } else {
        $initialAgents = @($initialAgents | Where-Object {
            $null -ne $_
        })
    }
    if ($initialAgents.Count -eq 0) {
        Add-StepResult -Name "agent-launch" -Status "Fail" `
            -Message "No agent process appeared within 20 seconds in the Windows Sandbox interactive session."
    } else {
        Add-StepResult -Name "agent-launch" -Status "Pass" `
            -Message "The service launched an agent in the interactive session." `
            -Data ([ordered]@{
                pids = @($initialAgents | Select-Object -ExpandProperty ProcessId)
                sessions = @($initialAgents | Select-Object -ExpandProperty SessionId)
            })
        $activeInteractiveSessionIds = @(Get-ActiveInteractiveSessionIds)
        $agentSessionReport = Get-AgentSessionReport `
            -Agents $initialAgents -ActiveSessionIds $activeInteractiveSessionIds
        if ($agentSessionReport.valid) {
            if ($activeInteractiveSessionIds.Count -eq 0) {
                $agentSessionMessage = "Every observed agent runs outside Session 0; no active interactive session could be enumerated."
            } else {
                $agentSessionMessage = (
                    "Every observed agent runs outside Session 0 and matches one of " +
                    ("active interactive sessions {0}." -f
                        ($activeInteractiveSessionIds -join ", ")))
            }
            Add-StepResult -Name "agent-session" -Status "Pass" `
                -Message $agentSessionMessage -Data $agentSessionReport
        } else {
            Add-StepResult -Name "agent-session" -Status "Fail" `
                -Message "An observed agent is in Session 0 or outside every active interactive session." `
                -Data $agentSessionReport
        }
        $oldPids = @($initialAgents | Select-Object -ExpandProperty ProcessId)
        $killResult = Invoke-Executable -Name "end-task-agent" `
            -FilePath (Join-Path $env:SystemRoot "System32\taskkill.exe") `
            -Arguments @("/PID", [string]$oldPids[0], "/F")
        $gone = Wait-Until -TimeoutSeconds 10 -Condition {
            $current = @(Get-AgentProcesses $clientRoot)
            (@($current | Where-Object { $oldPids -contains $_.ProcessId }).Count -eq 0)
        }
        $replacement = Wait-Until -TimeoutSeconds 20 -Condition {
            $current = @(Get-AgentProcesses $clientRoot)
            @($current | Where-Object { $oldPids -notcontains $_.ProcessId })
        }
        $replacementSessionReport = $null
        if ($null -ne $replacement) {
            $replacementAgents = @($replacement | Where-Object { $null -ne $_ })
            if ($replacementAgents.Count -gt 0) {
                $replacementSessionReport = Get-AgentSessionReport `
                    -Agents $replacementAgents -ActiveSessionIds $activeInteractiveSessionIds
            }
        }
        $replacementSessionValid = ($null -ne $replacementSessionReport -and
            $replacementSessionReport.valid)
        if ($killResult.succeeded -and $null -ne $gone -and
            $null -ne $replacement -and $replacementSessionValid) {
            Add-StepResult -Name "agent-supervision" -Status "Pass" `
                -Message "Force-terminating the agent produced a replacement PID within the watchdog window." `
                -Data ([ordered]@{
                    killed_pids = $oldPids
                    replacement_pids = @($replacement | Select-Object -ExpandProperty ProcessId)
                    taskkill_exit_code = $killResult.exit_code
                    replacement_session = $replacementSessionReport
                })
        } else {
            Add-StepResult -Name "agent-supervision" -Status "Fail" `
                -Message "The agent was not observed to recover after End Task." `
                -Data ([ordered]@{
                    killed_pids = $oldPids
                    taskkill = $killResult
                    disappeared = ($null -ne $gone)
                    replacement = $replacement
                    replacement_session = $replacementSessionReport
                })
        }
    }

    $probeUserSupported = $serviceVerifiedRunning -and
        ($null -ne (Get-Command New-LocalUser `
        -ErrorAction SilentlyContinue))
    if (-not $serviceVerifiedRunning) {
        Add-StepResult -Name "standard-user-tamper" -Status "Skip" `
            -Message "The standard-user probe was skipped because the service was not confirmed running."
    } elseif ($serviceProcessId -eq 0) {
        Add-StepResult -Name "standard-user-tamper" -Status "Fail" `
            -Message "The standard-user probe was not run because the authoritative service PID was unavailable."
    } elseif (-not $probeUserSupported) {
        Add-StepResult -Name "standard-user-tamper" -Status "Skip" `
            -Message "New-LocalUser is unavailable in this guest image."
    } else {
        $testUserName = "nstu-sbx-" + (Get-Random -Minimum 10000 -Maximum 99999)
        $plainPassword = "Nstu-" + [Guid]::NewGuid().ToString("N") + "!a9"
        $securePassword = ConvertTo-SecureString $plainPassword -AsPlainText -Force
        $probePublicRoot = Join-Path $env:PUBLIC ("NSTU-Sandbox-Probe-" + $PID)
        New-Item -ItemType Directory -Path $probePublicRoot -Force | Out-Null
        try {
            $probeAcl = Get-Acl -LiteralPath $probePublicRoot
            $probeRule = New-Object -TypeName `
                System.Security.AccessControl.FileSystemAccessRule -ArgumentList @(
                    "BUILTIN\Users",
                    [System.Security.AccessControl.FileSystemRights]::Modify,
                    ([System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
                        [System.Security.AccessControl.InheritanceFlags]::ObjectInherit),
                    [System.Security.AccessControl.PropagationFlags]::None,
                    [System.Security.AccessControl.AccessControlType]::Allow
                )
            $probeAcl.SetAccessRule($probeRule)
            Set-Acl -LiteralPath $probePublicRoot -AclObject $probeAcl
        } catch {
            # Public is normally writable; retain the probe and report any failure below.
        }
        $probeScript = Join-Path $probePublicRoot "standard-user-probe.ps1"
        $probeResultPath = Join-Path $probePublicRoot "standard-user-result.json"
        Create-StandardUserProbe -ProbePath $probeScript `
            -ResultPath $probeResultPath -ExpectedClientRoot $clientRoot `
            -ServiceProcessId $serviceProcessId
        try {
            New-LocalUser -Name $testUserName -Password $securePassword `
                -Description "Disposable NSTU Sandbox tamper test account" `
                -PasswordNeverExpires -UserMayNotChangePassword -ErrorAction Stop | Out-Null
            $credential = New-Object -TypeName System.Management.Automation.PSCredential `
                -ArgumentList @($testUserName, $securePassword)
            $probeResult = Start-Process -FilePath (Join-Path $env:SystemRoot `
                    "System32\WindowsPowerShell\v1.0\powershell.exe") `
                -Credential $credential -LoadUserProfile -Wait -PassThru `
                -WindowStyle Hidden -ArgumentList @(
                    "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy",
                    "Bypass", "-File", (Quote-ProcessArgument $probeScript),
                    "-ResultPath", (Quote-ProcessArgument $probeResultPath),
                    "-ExpectedClientRoot", (Quote-ProcessArgument $clientRoot),
                    "-ServiceProcessId", [string]$serviceProcessId
                )
            $probeData = $null
            if (Test-Path -LiteralPath $probeResultPath -PathType Leaf) {
                $probeData = Get-Content -LiteralPath $probeResultPath -Raw |
                    ConvertFrom-Json
            }
            if ($probeResult.ExitCode -eq 0 -and $null -ne $probeData) {
                Add-StepResult -Name "standard-user-tamper" -Status "Pass" `
                    -Message "A temporary standard user could not stop, delete, control, or terminate the protected service." `
                    -Data $probeData
            } else {
                Add-StepResult -Name "standard-user-tamper" -Status "Fail" `
                    -Message "The standard-user tamper probe reported a successful or ambiguous privileged operation." `
                    -Data ([ordered]@{
                        exit_code = $probeResult.ExitCode
                        report = $probeData
                    })
            }
        } catch {
            Add-StepResult -Name "standard-user-tamper" -Status "Skip" `
                -Message ("Could not create or run the temporary standard user: " + $_.Exception.Message)
        } finally {
            Remove-TestUser $testUserName
            $testUserName = $null
            if (Test-Path -LiteralPath $probePublicRoot) {
                Remove-Item -LiteralPath $probePublicRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $lockedPath = Join-Path $clientRoot "nstu-sandbox-locked.dat"
    $readyPath = Join-Path $workingRoot "lock-holder.ready"
    $lockScriptPath = Join-Path $workingRoot "hold-lock.ps1"
    [IO.File]::WriteAllText($lockedPath, "NSTU Sandbox lock test")
    $lockProcess = Create-LockHolder -ScriptPath $lockScriptPath `
        -LockedPath $lockedPath -ReadyPath $readyPath
    $lockReady = Wait-Until -TimeoutSeconds 10 -Condition {
        Test-Path -LiteralPath $readyPath -PathType Leaf
    }
    if ($null -eq $lockReady) {
        Add-StepResult -Name "locked-file" -Status "Fail" `
            -Message "The lock-holder process did not acquire the test file."
    } else {
        $pendingBefore = @(Get-PendingRenameOperations)
        $uninstallScript = Join-Path $clientRoot "uninstall-client-service.ps1"
        if (-not (Test-Path -LiteralPath $uninstallScript -PathType Leaf)) {
            Add-StepResult -Name "uninstall" -Status "Fail" `
                -Message "The client uninstaller script is missing: $uninstallScript"
        } else {
            $serviceBefore = Get-ServiceSnapshot -Name "nstu-service"
            $uninstallResult = Invoke-ChildPowerShell -Name "uninstall-before-restart" `
                -ScriptPath $uninstallScript -Arguments @(
                    "-InstallRoot", $clientRoot
                )
            if ($null -ne $lockProcess) {
                Stop-Process -Id $lockProcess.Id -Force -ErrorAction SilentlyContinue
                $lockProcess = $null
            }
            Start-Sleep -Milliseconds 500
            $pendingAfter = @(Get-PendingRenameOperations)
            $normalizedLocked = Normalize-PendingPath $lockedPath
            $pendingLocked = @($pendingAfter | Where-Object {
                (Normalize-PendingPath ([string]$_)) -eq $normalizedLocked
            }).Count -gt 0
            $lockedStillExists = Test-Path -LiteralPath $lockedPath -PathType Leaf
            $serviceAfter = Get-ServiceSnapshot -Name "nstu-service"
            $serviceUnchanged = $null -ne $serviceBefore -and
                $null -ne $serviceAfter -and
                $serviceBefore.PathName -eq $serviceAfter.PathName
            $pendingUnchanged = @(Compare-Object -ReferenceObject $pendingBefore `
                -DifferenceObject $pendingAfter).Count -eq 0
            $uninstallData = [ordered]@{
                exit_code = $uninstallResult.exit_code
                succeeded = $uninstallResult.succeeded
                stdout = $uninstallResult.stdout
                stderr = $uninstallResult.stderr
                service_unchanged = $serviceUnchanged
                locked_file = $lockedPath
                locked_file_still_exists = $lockedStillExists
                pending_locked_file = $pendingLocked
                pending_rename_count_unchanged = $pendingUnchanged
                pending_before = $pendingBefore
                pending_after = $pendingAfter
            }
            if (-not $uninstallResult.succeeded -and $serviceUnchanged -and
                $lockedStillExists -and -not $pendingLocked -and
                $pendingUnchanged -and
                $uninstallResult.stderr -match "completed after Windows restarts") {
                Add-StepResult -Name "uninstall" -Status "Pass" `
                    -Message "Pre-reboot removal was rejected without changing the service, files, or pending-delete state." `
                    -Data $uninstallData
            } else {
                Add-StepResult -Name "uninstall" -Status "Fail" `
                    -Message "The mandatory-restart uninstall guard changed state or returned an unexpected result." `
                    -Data $uninstallData
            }
        }
    }

    $remainingService = Get-ServiceSnapshot -Name "nstu-service"
    $remainingProcesses = @()
    foreach ($remainingProcess in @(Get-Process -Name "nstu-service" `
            -ErrorAction SilentlyContinue)) {
        try {
            $remainingProcesses += [pscustomobject]@{
                ProcessId = [uint32]$remainingProcess.Id
            }
        } finally {
            $remainingProcess.Dispose()
        }
    }
    $remainingProcesses += @(Get-AgentProcesses $clientRoot)
    Add-StepResult -Name "post-uninstall-state" -Status "Info" `
        -Message "Captured residual service/process state without rebooting." `
        -Data ([ordered]@{
            service_present = ($null -ne $remainingService)
            process_ids = @($remainingProcesses | Select-Object -ExpandProperty ProcessId)
        })
    Add-StepResult -Name "reboot-validation" -Status "Skip" `
        -Message "Windows Sandbox cannot validate next-boot deletion or first-boot service activation; use a persistent VM for that gate."

    if ($script:FailureCount -eq 0) {
        $exitCode = 0
    } else {
        $exitCode = 1
    }
} catch {
    Add-StepResult -Name "fatal" -Status "Fail" -Message $_.Exception.Message
    $exitCode = 1
} finally {
    if ($null -ne $lockProcess) {
        Stop-Process -Id $lockProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $testUserName) {
        Remove-TestUser $testUserName
    }
    if ($serviceStarted) {
        Stop-Service -Name "nstu-service" -Force -ErrorAction SilentlyContinue
    }
    $finalStatus = "Failed"
    if ($script:FailureCount -eq 0) {
        if ($script:SkipCount -gt 0) {
            $finalStatus = "PassedWithSkips"
        } else {
            $finalStatus = "Passed"
        }
    }
    $summary = [ordered]@{
        schema = 1
        started_utc = $script:StartedUtc.ToString("o")
        completed_utc = [DateTimeOffset]::UtcNow.ToString("o")
        marker = $SandboxMarker
        mode = $Mode
        input_path = $InputPath
        install_root = $InstallRoot
        data_root = $DataRoot
        effective_output_root = $script:EffectiveOutputRoot
        mapped_output_writable = $script:MappedOutputWritable
        final_status = $finalStatus
        failure_count = $script:FailureCount
        skip_count = $script:SkipCount
        reboot_validation = [ordered]@{
            performed = $false
            status = "NotRun"
            reason = "Windows Sandbox is destroyed on close/restart; use a persistent VM to validate PendingFileRenameOperations on boot."
        }
        sandbox_shutdown = [ordered]@{
            requested_after_report = ($script:SandboxValidated -and
                $ShutdownWhenComplete)
            mechanism = "shutdown.exe /s /t 5 /f"
        }
        results = @($script:Results.ToArray())
    }
    try {
        if ($null -ne $script:EffectiveOutputRoot) {
            $summary | ConvertTo-Json -Depth 14 | Set-Content `
                -LiteralPath (Join-Path $script:EffectiveOutputRoot "summary.json") `
                -Encoding UTF8
            @($script:Results.ToArray()) | ConvertTo-Json -Depth 10 | Set-Content `
                -LiteralPath (Join-Path $script:EffectiveOutputRoot "steps.json") `
                -Encoding UTF8
        }
    } catch {
        # Preserve the original test exit code if report serialization fails.
    }
    if ($script:TranscriptStarted) {
        try { Stop-Transcript | Out-Null } catch { }
    }
    if ($script:SandboxValidated -and $ShutdownWhenComplete) {
        try {
            Start-Sleep -Milliseconds 500
            Start-Process -FilePath (Join-Path $env:SystemRoot "System32\shutdown.exe") `
                -ArgumentList @("/s", "/t", "5", "/f") `
                -WindowStyle Hidden | Out-Null
        } catch {
            # The summary already records that shutdown was requested.
        }
    }
}

exit $exitCode
