[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath,
    [Parameter(Mandatory = $true)]
    [string]$PublishRoot,
    [string]$PackageId = "",
    [string]$ExpectedArchiveSha256 = "",
    [string]$ExpectedContentSha256 = "",
    [UInt64]$MaxArchiveBytes = 268435456,
    [UInt64]$MaxExpandedBytes = 134217728,
    [UInt32]$MaxEntries = 4096,
    [UInt64]$MaxEntryBytes = 67108864,
    [UInt32]$MaxCompressionRatio = 1000,
    [switch]$AllowUnsigned,
    [string]$TrustedPublisherThumbprint = "",
    [switch]$RequireAuthenticode,
    [switch]$AllowNonElevatedTest
)

$ErrorActionPreference = "Stop"
try {
    Add-Type -AssemblyName System.IO.Compression -ErrorAction Stop
} catch {
    throw "System.IO.Compression is unavailable on this Windows image."
}
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
} catch {
    # .NET Core exposes ZipArchive from the primary compression assembly.
}
if ($null -eq ("System.IO.Compression.ZipArchive" -as [type])) {
    throw "System.IO.Compression.ZipArchive is unavailable on this Windows image."
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -and
    -not $AllowNonElevatedTest) {
    throw "Exam package staging requires Administrator privileges."
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "A path is required."
    }
    # Do not canonicalize first: Path.GetFullPath would turn a relative input
    # into an absolute path and defeat this boundary check. Accept only a
    # fully-qualified Windows drive or UNC path (including the long-path form).
    if ($Path -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)') {
        throw "Path must be absolute: $Path"
    }
    try {
        $full = [IO.Path]::GetFullPath($Path)
    } catch {
        throw "Invalid path '$Path': $($_.Exception.Message)"
    }
    if (-not [IO.Path]::IsPathRooted($full)) {
        throw "Path must be absolute: $Path"
    }
    $root = [IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw "Path has no Windows root: $Path"
    }
    if ($full.Length -le $root.Length) {
        return $root
    }
    return $full.TrimEnd('\', '/')
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Candidate
    )
    $rootFull = Get-FullPath $Root
    $candidateFull = Get-FullPath $Candidate
    if ([string]::Equals($rootFull, $candidateFull,
                         [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $rootFull
    if (-not $prefix.EndsWith('\')) {
        $prefix += '\'
    }
    return $candidateFull.StartsWith(
        $prefix, [StringComparison]::OrdinalIgnoreCase)
}

# Check every existing component. A junction or symlink in a parent must not
# be hidden by a later canonicalization step.
function Test-ExistingReparsePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-FullPath $Path
    $root = [IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root)) {
        return $true
    }
    $current = $root
    try {
        if (Test-Path -LiteralPath $current) {
            $attributes = [IO.File]::GetAttributes($current)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                return $true
            }
        }
        $remainder = $full.Substring($root.Length)
        foreach ($part in ($remainder -split '[\\/]+')) {
            if ([string]::IsNullOrEmpty($part)) {
                continue
            }
            $current = Join-Path -Path $current -ChildPath $part
            if (-not (Test-Path -LiteralPath $current)) {
                break
            }
            $attributes = [IO.File]::GetAttributes($current)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                return $true
            }
        }
    } catch {
        throw "Could not inspect reparse state for '$Path': $($_.Exception.Message)"
    }
    return $false
}

function Normalize-Hash {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Name must be a 64-character SHA-256 value."
    }
    return $Value.ToLowerInvariant()
}

function Get-RelativeZipName {
    param([Parameter(Mandatory = $true)][string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name.Length -gt 512) {
        throw "ZIP entry name is empty or too long."
    }
    if ($Name.IndexOf([char]0) -ge 0 -or
        $Name.IndexOf([char]0xfffd) -ge 0) {
        throw "ZIP entry name contains invalid UTF-8."
    }
    if ($Name.Contains('\') -or $Name.StartsWith('/') -or
        $Name.StartsWith('//') -or
        ($Name.Length -ge 2 -and $Name[1] -eq ':')) {
        throw "ZIP entry '$Name' is not a portable relative path."
    }
    $directory = $Name.EndsWith('/')
    $trimmed = if ($directory) {
        $Name.Substring(0, $Name.Length - 1)
    } else {
        $Name
    }
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        throw "ZIP entry has an empty path."
    }
    $parts = $trimmed.Split('/')
    foreach ($part in $parts) {
        if ([string]::IsNullOrEmpty($part) -or $part -eq '.' -or
            $part -eq '..' -or $part.EndsWith('.') -or
            $part.EndsWith(' ')) {
            throw "ZIP entry '$Name' contains an unsafe path component."
        }
        if ($part.IndexOf(':') -ge 0 -or
            $part.IndexOfAny([char[]]('*?"<>|')) -ge 0) {
            throw "ZIP entry '$Name' contains an invalid Windows character."
        }
        if ($part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$') {
            throw "ZIP entry '$Name' uses a reserved Windows device name."
        }
        foreach ($character in $part.ToCharArray()) {
            if ([int][char]$character -lt 0x20 -or
                [int][char]$character -eq 0x7f) {
                throw "ZIP entry '$Name' contains a control character."
            }
        }
    }
    return [pscustomobject]@{
        Relative = ($parts -join '/')
        IsDirectory = $directory
        Parts = $parts
    }
}

function Test-ZipEntryAttributes {
    param([Parameter(Mandatory = $true)]$Entry)
    $attributes = [UInt32]$Entry.ExternalAttributes
    $unixType = ($attributes -shr 16) -band 0xf000
    if ($unixType -eq 0xa000 -or $unixType -eq 0xc000 -or
        $unixType -eq 0x6000) {
        throw "ZIP entry '$($Entry.FullName)' is a link or special file."
    }
    $dosAttributes = $attributes -band 0xffff
    if (($dosAttributes -band 0x400) -ne 0) {
        throw "ZIP entry '$($Entry.FullName)' is marked as a reparse point."
    }
}

function Read-ZipEntryBytes {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][UInt64]$Limit
    )
    if ($Entry.Length -lt 0 -or [UInt64]$Entry.Length -gt $Limit) {
        throw "ZIP entry '$($Entry.FullName)' exceeds its size limit."
    }
    $inputStream = $null
    $memory = $null
    try {
        $inputStream = $Entry.Open()
        $memory = [IO.MemoryStream]::new()
        $buffer = New-Object byte[] 65536
        [UInt64]$total = 0
        while (($read = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            if ([UInt64]$read -gt ($Limit - $total)) {
                throw "ZIP entry '$($Entry.FullName)' expanded beyond its limit."
            }
            $memory.Write($buffer, 0, $read)
            $total += [UInt64]$read
        }
        if ($total -ne [UInt64]$Entry.Length) {
            throw "ZIP entry '$($Entry.FullName)' length or CRC validation failed."
        }
        return $memory.ToArray()
    } catch {
        throw "Could not read ZIP entry '$($Entry.FullName)': $($_.Exception.Message)"
    } finally {
        if ($null -ne $inputStream) {
            $inputStream.Dispose()
        }
        if ($null -ne $memory) {
            $memory.Dispose()
        }
    }
}

function Test-JsonObject {
    param([Parameter(Mandatory = $true)]$Value)
    return $null -ne $Value -and $Value -is [PSCustomObject]
}

function Test-JsonPropertyPresent {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if (-not (Test-JsonObject $Object)) {
        return $false
    }
    return @($Object.PSObject.Properties |
        Where-Object { $_.Name -ceq $Name }).Count -eq 1
}

function Get-JsonPropertyValue {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$Required
    )
    if (-not (Test-JsonObject $Object)) {
        throw "JSON value containing '$Name' must be an object."
    }
    $matches = @($Object.PSObject.Properties |
        Where-Object { $_.Name -ceq $Name })
    if ($matches.Count -eq 0) {
        if ($Required) {
            throw "JSON object is missing required property '$Name'."
        }
        return $null
    }
    if ($matches.Count -ne 1) {
        throw "JSON object contains duplicate property '$Name'."
    }
    # Preserve arrays as one value without Write-Output's wrapper object. In
    # PowerShell 7, Write-Output -NoEnumerate can surface scalars as a generic
    # List[object], which breaks strict type validation of manifest fields.
    return ,$matches[0].Value
}

function Assert-JsonProperties {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][string[]]$Allowed
    )
    if (-not (Test-JsonObject $Object)) {
        throw "$Context must be a JSON object."
    }
    foreach ($property in @($Object.PSObject.Properties)) {
        # JSON property names are case-sensitive. This also rejects a casing
        # variant that PowerShell's normal member lookup would otherwise merge.
        if (-not ($Allowed -ccontains [string]$property.Name)) {
            throw "$Context contains unsupported property '$($property.Name)'."
        }
    }
}

function Assert-JsonString {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Context,
        [int]$MinimumLength = 0,
        [int]$MaximumLength = 2147483647
    )
    if ($Value -isnot [string] -or $Value.Length -lt $MinimumLength -or
        $Value.Length -gt $MaximumLength) {
        throw "$Context must be a string of length $MinimumLength..$MaximumLength."
    }
}

function Get-JsonArray {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Context,
        [int]$MinimumCount = 0,
        [int]$MaximumCount = 2147483647
    )
    if ($Value -is [string] -or $Value -isnot [Collections.IEnumerable]) {
        throw "$Context must be an array."
    }
    $items = @($Value)
    if ($items.Count -lt $MinimumCount -or $items.Count -gt $MaximumCount) {
        throw "$Context contains an invalid number of items."
    }
    return ,$items
}

function Get-JsonInteger {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Context,
        [Int64]$Minimum = [Int64]::MinValue,
        [Int64]$Maximum = [Int64]::MaxValue
    )
    if ($Value -is [bool] -or
        ($Value -isnot [byte] -and $Value -isnot [sbyte] -and
         $Value -isnot [int16] -and $Value -isnot [uint16] -and
         $Value -isnot [int32] -and $Value -isnot [uint32] -and
         $Value -isnot [int64] -and $Value -isnot [uint64] -and
         $Value -isnot [single] -and $Value -isnot [double] -and
         $Value -isnot [decimal])) {
        throw "$Context must be an integer."
    }
    try {
        $decimal = [decimal]$Value
    } catch {
        throw "$Context must be an integer."
    }
    if ($decimal -ne [decimal]::Truncate($decimal) -or
        $decimal -lt $Minimum -or $decimal -gt $Maximum) {
        throw "$Context must be an integer in the range $Minimum..$Maximum."
    }
    return [Int64]$decimal
}

function Add-ManifestAssetReference {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)]$EntryKinds,
        [Parameter(Mandatory = $true)]$AssetSet
    )
    Assert-JsonString -Value $Value -Context $Context -MinimumLength 1 -MaximumLength 512
    if ($Value.Contains('?') -or $Value.Contains('#') -or
        $Value -match '%(?:2f|2F|5c|5C|23|3f|3F)') {
        throw "$Context contains a query, fragment, or encoded separator."
    }
    $normalized = Get-RelativeZipName $Value
    if ($normalized.IsDirectory -or
        $normalized.Relative -in @('manifest.json', 'manifest.p7s')) {
        throw "$Context must reference a regular package asset, not package metadata."
    }
    if (-not $EntryKinds.ContainsKey($normalized.Relative) -or
        $EntryKinds[$normalized.Relative] -ne 'file') {
        throw "$Context references a missing package file: $Value"
    }
    $AssetSet.Add($normalized.Relative) | Out-Null
}

function Test-ExamManifest {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)]$EntryKinds
    )
    Assert-JsonProperties -Object $Manifest -Context 'Exam manifest' -Allowed @(
        'id', 'title', 'subject', 'candidate', 'durationSeconds', 'documents', 'questions')

    $id = Get-JsonPropertyValue -Object $Manifest -Name 'id' -Required
    Assert-JsonString -Value $id -Context 'Exam manifest id' -MinimumLength 1 -MaximumLength 96
    if ($id -notmatch '^[A-Za-z0-9._-]+$') {
        throw 'Exam manifest id contains unsupported characters.'
    }
    $title = Get-JsonPropertyValue -Object $Manifest -Name 'title' -Required
    Assert-JsonString -Value $title -Context 'Exam manifest title' -MinimumLength 1 -MaximumLength 240
    $duration = Get-JsonPropertyValue -Object $Manifest -Name 'durationSeconds' -Required
    [void](Get-JsonInteger -Value $duration -Context 'Exam durationSeconds' -Minimum 60 -Maximum 86400)
    $questionsValue = Get-JsonPropertyValue -Object $Manifest -Name 'questions' -Required
    $questions = Get-JsonArray -Value $questionsValue -Context 'Exam questions' -MinimumCount 1 -MaximumCount 500

    foreach ($optional in @(@('subject', 120), @('candidate', 160))) {
        if (Test-JsonPropertyPresent -Object $Manifest -Name $optional[0]) {
            $value = Get-JsonPropertyValue -Object $Manifest -Name $optional[0]
            Assert-JsonString -Value $value -Context "Exam manifest $($optional[0])" -MaximumLength $optional[1]
        }
    }

    $assets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if (Test-JsonPropertyPresent -Object $Manifest -Name 'documents') {
        $documents = Get-JsonArray -Value (Get-JsonPropertyValue -Object $Manifest -Name 'documents') `
            -Context 'Exam documents' -MaximumCount 128
        $documentIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($document in $documents) {
            Assert-JsonProperties -Object $document -Context 'Exam document' -Allowed @('id', 'url', 'title')
            $documentId = Get-JsonPropertyValue -Object $document -Name 'id' -Required
            Assert-JsonString -Value $documentId -Context 'Exam document id' -MinimumLength 1 -MaximumLength 96
            if ($documentId -notmatch '^[A-Za-z0-9._-]+$' -or
                -not $documentIds.Add($documentId)) {
                throw "Exam document id is invalid or duplicated: $documentId"
            }
            $documentUrl = Get-JsonPropertyValue -Object $document -Name 'url' -Required
            Add-ManifestAssetReference -Value $documentUrl -Context 'Exam document url' `
                -EntryKinds $EntryKinds -AssetSet $assets
            if (Test-JsonPropertyPresent -Object $document -Name 'title') {
                Assert-JsonString -Value (Get-JsonPropertyValue -Object $document -Name 'title') `
                    -Context 'Exam document title' -MaximumLength 240
            }
        }
    }

    $questionIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($question in $questions) {
        Assert-JsonProperties -Object $question -Context 'Exam question' -Allowed @(
            'id', 'type', 'prompt', 'instruction', 'points', 'options', 'audio', 'pdf',
            'passage', 'wordLimit', 'answerPlaceholder')
        $questionId = Get-JsonPropertyValue -Object $question -Name 'id' -Required
        Assert-JsonString -Value $questionId -Context 'Exam question id' -MinimumLength 1 -MaximumLength 96
        if ($questionId -notmatch '^[A-Za-z0-9._-]+$' -or
            -not $questionIds.Add($questionId)) {
            throw "Exam question id is invalid or duplicated: $questionId"
        }
        $type = Get-JsonPropertyValue -Object $question -Name 'type' -Required
        Assert-JsonString -Value $type -Context 'Exam question type' -MinimumLength 1 -MaximumLength 32
        if ($type -notin @('multiple_choice', 'short_answer', 'essay', 'listening', 'reading')) {
            throw "Exam question type is unsupported: $type"
        }
        $prompt = Get-JsonPropertyValue -Object $question -Name 'prompt' -Required
        Assert-JsonString -Value $prompt -Context 'Exam question prompt' -MinimumLength 1 -MaximumLength 8000
        foreach ($optional in @(@('instruction', 4000), @('passage', 30000), @('answerPlaceholder', 240))) {
            if (Test-JsonPropertyPresent -Object $question -Name $optional[0]) {
                Assert-JsonString -Value (Get-JsonPropertyValue -Object $question -Name $optional[0]) `
                    -Context "Exam question $($optional[0])" -MaximumLength $optional[1]
            }
        }
        if (Test-JsonPropertyPresent -Object $question -Name 'points') {
            [void](Get-JsonInteger -Value (Get-JsonPropertyValue -Object $question -Name 'points') `
                -Context 'Exam question points' -Minimum 0 -Maximum 1000)
        }
        if (Test-JsonPropertyPresent -Object $question -Name 'wordLimit') {
            [void](Get-JsonInteger -Value (Get-JsonPropertyValue -Object $question -Name 'wordLimit') `
                -Context 'Exam question wordLimit' -Minimum 1 -Maximum 5000)
        }
        $needsOptions = $type -in @('multiple_choice', 'listening', 'reading')
        if (Test-JsonPropertyPresent -Object $question -Name 'options') {
            $options = Get-JsonArray -Value (Get-JsonPropertyValue -Object $question -Name 'options') `
                -Context 'Exam question options' -MinimumCount 2 -MaximumCount 12
            foreach ($option in $options) {
                Assert-JsonString -Value $option -Context 'Exam question option' -MinimumLength 1 -MaximumLength 2000
            }
        } elseif ($needsOptions) {
            throw "Exam question type '$type' requires options."
        }
        foreach ($field in @('audio', 'pdf')) {
            if (Test-JsonPropertyPresent -Object $question -Name $field) {
                $asset = Get-JsonPropertyValue -Object $question -Name $field
                Assert-JsonString -Value $asset -Context "Exam question $field" -MaximumLength 512
                if ($asset.Length -gt 0) {
                    Add-ManifestAssetReference -Value $asset -Context "Exam question $field" `
                        -EntryKinds $EntryKinds -AssetSet $assets
                }
            }
        }
    }
    return [pscustomobject]@{ Id = $id; AssetPaths = @($assets) }
}

function Write-UInt64LittleEndian {
    param(
        [Parameter(Mandatory = $true)][Security.Cryptography.HashAlgorithm]$Hash,
        [Parameter(Mandatory = $true)][UInt64]$Value
    )
    $bytes = New-Object byte[] 8
    for ($index = 0; $index -lt 8; ++$index) {
        $bytes[$index] = [byte](($Value -shr (8 * $index)) -band 0xff)
    }
    $Hash.TransformBlock($bytes, 0, $bytes.Length, $bytes, 0) | Out-Null
}

function Add-FileToHash {
    param(
        [Parameter(Mandatory = $true)][Security.Cryptography.HashAlgorithm]$Hash,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
                                  [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $buffer = New-Object byte[] 65536
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $Hash.TransformBlock($buffer, 0, $read, $buffer, 0) | Out-Null
        }
    } finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Get-StreamSha256 {
    param(
        [Parameter(Mandatory = $true)][IO.Stream]$Stream
    )
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $Stream.Position = 0
        $buffer = New-Object byte[] 65536
        while (($read = $Stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $hash.TransformBlock($buffer, 0, $read, $buffer, 0) | Out-Null
        }
        $hash.TransformFinalBlock([byte[]]::new(0), 0, 0) | Out-Null
        return ([BitConverter]::ToString($hash.Hash)).Replace('-', '').ToLowerInvariant()
    } finally {
        $hash.Dispose()
        $Stream.Position = 0
    }
}

function Get-ContentDigest {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][UInt64]$MaximumBytes,
        [Parameter(Mandatory = $true)][UInt32]$MaximumFiles
    )
    $rootFull = Get-FullPath $Root
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container) -or
        (Test-ExistingReparsePath $rootFull)) {
        throw "Package root is unavailable or reparse-backed."
    }
    $filesByName = [Collections.Generic.SortedDictionary[string, object]]::new(
        [StringComparer]::Ordinal)
    [UInt64]$totalBytes = 0
    $allItems = @(Get-ChildItem -LiteralPath $rootFull -Recurse -Force)
    foreach ($item in $allItems) {
        if (Test-ExistingReparsePath $item.FullName) {
            throw "Package contains a reparse point: $($item.FullName)"
        }
        if ($item.PSIsContainer) {
            continue
        }
        if (-not ($item -is [IO.FileInfo])) {
            throw "Package contains a non-regular file: $($item.FullName)"
        }
        $relative = $item.FullName.Substring($rootFull.Length).TrimStart('\', '/')
        $relative = $relative.Replace('\', '/')
        $normalized = Get-RelativeZipName $relative
        if ($normalized.IsDirectory) {
            throw "Package file resolved to a directory name: $relative"
        }
        if ($filesByName.ContainsKey($normalized.Relative)) {
            throw "Package contains a duplicate file path: $relative"
        }
        $size = [UInt64]$item.Length
        if ($size -gt $MaximumBytes - $totalBytes) {
            throw "Package exceeds its expanded byte quota."
        }
        $totalBytes += $size
        if ($filesByName.Count -ge $MaximumFiles) {
            throw "Package exceeds its file-count quota."
        }
        $filesByName.Add($normalized.Relative, $item)
    }
    if ($filesByName.Count -eq 0) {
        throw "Package contains no regular files."
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($pair in $filesByName.GetEnumerator()) {
            $nameBytes = [Text.Encoding]::UTF8.GetBytes([string]$pair.Key)
            Write-UInt64LittleEndian -Hash $hash -Value ([UInt64]$nameBytes.Length)
            if ($nameBytes.Length -gt 0) {
                $hash.TransformBlock($nameBytes, 0, $nameBytes.Length,
                                     $nameBytes, 0) | Out-Null
            }
            $size = [UInt64]$pair.Value.Length
            Write-UInt64LittleEndian -Hash $hash -Value $size
            Add-FileToHash -Hash $hash -Path $pair.Value.FullName
        }
        $hash.TransformFinalBlock([byte[]]::new(0), 0, 0) | Out-Null
        $digest = ([BitConverter]::ToString($hash.Hash)).Replace('-', '')
        return [pscustomobject]@{
            Digest = $digest.ToLowerInvariant()
            FileCount = [UInt32]$filesByName.Count
            ExpandedBytes = $totalBytes
        }
    } finally {
        $hash.Dispose()
    }
}

function Set-PrivateDirectoryAcl {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$AllowUsersRead
    )
    if ($AllowNonElevatedTest) {
        return
    }
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $propagation = [Security.AccessControl.PropagationFlags]::None
    $full = [Security.AccessControl.FileSystemRights]::FullControl
    $read = [Security.AccessControl.FileSystemRights]::ReadAndExecute
    $allow = [Security.AccessControl.AccessControlType]::Allow
    foreach ($sidText in @("S-1-5-18", "S-1-5-32-544")) {
        $sid = [Security.Principal.SecurityIdentifier]::new($sidText)
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            $sid, $full, $inheritance, $propagation, $allow))
    }
    if ($AllowUsersRead) {
        $users = [Security.Principal.SecurityIdentifier]::new("S-1-5-32-545")
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            $users, $read, $inheritance, $propagation, $allow))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Add-NativeMoveType {
    if ($null -eq ("NstuAtomicMove" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class NstuAtomicMove {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileEx(
        string existingFileName, string newFileName, uint flags);
    public static void Move(string source, string destination) {
        const uint MOVEFILE_WRITE_THROUGH = 0x00000008;
        // MoveFileExW refuses an existing destination by default. In
        // particular, 0x1 is MOVEFILE_REPLACE_EXISTING, not a fail-if-exists
        // flag: passing it would overwrite a concurrently published sidecar.
        if (!MoveFileEx(source, destination, MOVEFILE_WRITE_THROUGH)) {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "MoveFileExW failed");
        }
    }
}
"@
    }
}

function Move-Atomically {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Add-NativeMoveType
    [NstuAtomicMove]::Move($Source, $Destination)
}

function Open-PublishLock {
    param([Parameter(Mandatory = $true)][string]$Path)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            return [IO.File]::Open($Path, [IO.FileMode]::OpenOrCreate,
                                   [IO.FileAccess]::ReadWrite,
                                   [IO.FileShare]::None)
        } catch [IO.IOException] {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Timed out waiting for the package publication lock: $Path"
}

function Write-DurableUtf8Text {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew,
                                  [IO.FileAccess]::Write,
                                  [IO.FileShare]::None)
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
        if ($bytes.Length -gt 0) {
            $stream.Write($bytes, 0, $bytes.Length)
        }
        $stream.Flush($true)
    } finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Read-PackageMetadata {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$PackageId,
        [Parameter(Mandatory = $true)][string]$ContentDigest,
        [Parameter(Mandatory = $true)][string]$ArchiveDigest,
        [Parameter(Mandatory = $true)][bool]$RequireSignature,
        [Parameter(Mandatory = $true)][UInt32]$FileCount,
        [Parameter(Mandatory = $true)][UInt64]$ExpandedBytes
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Test-ExistingReparsePath $Path)) {
        throw "Package metadata sidecar is unavailable or reparse-backed."
    }
    try {
        $encoding = [Text.UTF8Encoding]::new($false, $true)
        $text = [IO.File]::ReadAllText($Path, $encoding)
        $metadata = $text | ConvertFrom-Json
    } catch {
        throw "Package metadata sidecar is not valid UTF-8 JSON: $($_.Exception.Message)"
    }
    Assert-JsonProperties -Object $metadata -Context 'Package metadata' -Allowed @(
        'schema', 'package_id', 'content_sha256', 'archive_sha256', 'file_count',
        'expanded_bytes', 'signature_verified', 'published_utc')
    $schema = Get-JsonPropertyValue -Object $metadata -Name 'schema' -Required
    if ((Get-JsonInteger -Value $schema -Context 'Package metadata schema' -Minimum 1 -Maximum 1) -ne 1) {
        throw 'Package metadata schema is unsupported.'
    }
    $metadataPackageId = Get-JsonPropertyValue -Object $metadata -Name 'package_id' -Required
    Assert-JsonString -Value $metadataPackageId -Context 'Package metadata package_id' -MinimumLength 1 -MaximumLength 96
    if (-not [string]::Equals($metadataPackageId, $PackageId,
                               [StringComparison]::Ordinal)) {
        throw 'Package metadata package_id does not match the staged package.'
    }
    $metadataContent = Normalize-Hash -Value (Get-JsonPropertyValue -Object $metadata -Name 'content_sha256' -Required) -Name 'Package metadata content_sha256'
    $metadataArchive = Normalize-Hash -Value (Get-JsonPropertyValue -Object $metadata -Name 'archive_sha256' -Required) -Name 'Package metadata archive_sha256'
    if ($metadataContent -ne $ContentDigest -or $metadataArchive -ne $ArchiveDigest) {
        throw 'Package metadata hashes do not match the staged package.'
    }
    $metadataFiles = Get-JsonInteger -Value (Get-JsonPropertyValue -Object $metadata -Name 'file_count' -Required) `
        -Context 'Package metadata file_count' -Minimum 1 -Maximum $MaxEntries
    $metadataBytes = Get-JsonInteger -Value (Get-JsonPropertyValue -Object $metadata -Name 'expanded_bytes' -Required) `
        -Context 'Package metadata expanded_bytes' -Minimum 1 -Maximum $MaxExpandedBytes
    if ($metadataFiles -ne $FileCount -or [UInt64]$metadataBytes -ne $ExpandedBytes) {
        throw 'Package metadata size accounting does not match the staged package.'
    }
    $signature = Get-JsonPropertyValue -Object $metadata -Name 'signature_verified' -Required
    if ($signature -isnot [bool]) {
        throw 'Package metadata signature_verified must be a boolean.'
    }
    if ($RequireSignature -and -not [bool]$signature) {
        throw 'Package metadata does not record a verified publisher signature.'
    }
    $published = Get-JsonPropertyValue -Object $metadata -Name 'published_utc' -Required
    # ConvertFrom-Json on Windows PowerShell and pwsh may materialize an
    # ISO-8601 JSON string as DateTime. Accept that parser representation only
    # after converting it back to a bounded, round-trip-valid timestamp; other
    # JSON types remain invalid.
    if ($published -is [DateTime]) {
        $publishedText = $published.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
    } elseif ($published -is [DateTimeOffset]) {
        $publishedText = $published.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
    } elseif ($published -is [string]) {
        $publishedText = $published
    } else {
        throw 'Package metadata published_utc must be an ISO-8601 string.'
    }
    Assert-JsonString -Value $publishedText -Context 'Package metadata published_utc' -MinimumLength 1 -MaximumLength 128
    try {
        [void][DateTimeOffset]::Parse($publishedText,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    } catch {
        throw 'Package metadata published_utc is not a valid ISO-8601 timestamp.'
    }
    return $metadata
}

function Test-ManifestSignature {
    param(
        [Parameter(Mandatory = $true)][byte[]]$ManifestBytes,
        [Parameter(Mandatory = $true)][byte[]]$SignatureBytes,
        [Parameter(Mandatory = $true)][string]$Thumbprint
    )
    if ([string]::IsNullOrWhiteSpace($Thumbprint)) {
        throw "A trusted publisher thumbprint is required for signed packages."
    }
    try {
        Add-Type -AssemblyName System.Security.Cryptography.Pkcs
    } catch {
        try {
            Add-Type -AssemblyName System.Security
        } catch {
            throw "CMS signature support is unavailable in this PowerShell runtime."
        }
    }
    try {
        $contentInfo = [System.Security.Cryptography.Pkcs.ContentInfo]::new(
            $ManifestBytes)
        $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new(
            $contentInfo, $true)
        $cms.Decode($SignatureBytes)
        $cms.CheckSignature($true)
        if ($cms.SignerInfos.Count -ne 1) {
            throw "The package signature must contain exactly one signer."
        }
        $certificate = $cms.SignerInfos[0].Certificate
        if ($null -eq $certificate) {
            throw "The package signature does not contain a signer certificate."
        }
        $expected = $Thumbprint.Replace(' ', '').ToLowerInvariant()
        $actual = $certificate.Thumbprint.Replace(' ', '').ToLowerInvariant()
        if (-not [string]::Equals($actual, $expected,
                                   [StringComparison]::OrdinalIgnoreCase)) {
            throw "The package signer is not the configured publisher."
        }
    } catch {
        throw "Package publisher signature verification failed: $($_.Exception.Message)"
    }
}

function Test-AuthenticodeFiles {
    param([Parameter(Mandatory = $true)][string]$Root)
    $binaries = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
        Where-Object { $_.Extension -in @('.exe', '.dll') })
    if ($binaries.Count -eq 0) {
        throw "Authenticode verification was requested but the package has no PE files."
    }
    foreach ($binary in $binaries) {
        $signature = Get-AuthenticodeSignature -LiteralPath $binary.FullName
        if ($signature.Status -ne
            [Management.Automation.SignatureStatus]::Valid) {
            throw "Invalid or missing Authenticode signature: $($binary.FullName)"
        }
    }
}

$archiveFull = Get-FullPath $ArchivePath
$publishFull = Get-FullPath $PublishRoot
$archiveHashExpected = Normalize-Hash -Value $ExpectedArchiveSha256 -Name "ExpectedArchiveSha256"
$contentHashExpected = Normalize-Hash -Value $ExpectedContentSha256 -Name "ExpectedContentSha256"
if ([string]::IsNullOrWhiteSpace($archiveHashExpected)) {
    throw "ExpectedArchiveSha256 is required; refusing to read an unpinned archive."
}
if ([string]::IsNullOrWhiteSpace($contentHashExpected)) {
    throw "ExpectedContentSha256 is required; refusing to publish an unpinned package."
}

if (-not (Test-Path -LiteralPath $archiveFull -PathType Leaf)) {
    throw "Exam package archive was not found: $archiveFull"
}
if (Test-ExistingReparsePath $archiveFull) {
    throw "Exam package archive is reparse-backed."
}
$archiveItem = Get-Item -LiteralPath $archiveFull -Force
if ([UInt64]$archiveItem.Length -gt $MaxArchiveBytes) {
    throw "Exam package archive exceeds the configured byte limit."
}

$publishRootName = [IO.Path]::GetPathRoot($publishFull)
if ([string]::Equals($publishFull.TrimEnd('\', '/'),
                     $publishRootName.TrimEnd('\', '/'),
                     [StringComparison]::OrdinalIgnoreCase)) {
    throw "PublishRoot must be an absolute subdirectory, not a drive root."
}
if (Test-ExistingReparsePath $publishFull) {
    throw "PublishRoot is reparse-backed."
}
New-Item -ItemType Directory -Path $publishFull -Force | Out-Null
if (Test-ExistingReparsePath $publishFull) {
    throw "PublishRoot became reparse-backed."
}

$archiveStream = $null
$archiveLocked = $false
$archive = $null
$stage = $null
$metadataTemp = $null
$publishLock = $null
$finalPath = $null
$sidecarPath = $null
$content = $null
$publishedFinalByThisRun = $false
$metadataPublishedByThisRun = $false
$entryRecords = [Collections.Generic.List[object]]::new()
$pathKinds = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
[UInt64]$expandedEntryBytes = 0

try {
    $archiveStream = [IO.File]::Open($archiveFull, [IO.FileMode]::Open,
                                     [IO.FileAccess]::Read, [IO.FileShare]::Read)
    if ([UInt64]$archiveStream.Length -gt $MaxArchiveBytes) {
        throw "Exam package archive exceeds the configured byte limit."
    }
    if ($archiveStream.Length -gt 0) {
        $archiveStream.Lock(0, $archiveStream.Length)
        $archiveLocked = $true
    }
    $archiveHashObserved = Get-StreamSha256 -Stream $archiveStream
    if (-not [string]::Equals($archiveHashExpected, $archiveHashObserved,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw "Exam package archive SHA-256 does not match its pin."
    }
    $archive = [IO.Compression.ZipArchive]::new(
        $archiveStream, [IO.Compression.ZipArchiveMode]::Read, $true)
    if ($archive.Entries.Count -eq 0 -or
        $archive.Entries.Count -gt $MaxEntries) {
        throw "Exam package entry count exceeds the configured limit."
    }
    foreach ($entry in $archive.Entries) {
        Test-ZipEntryAttributes $entry
        $normalized = Get-RelativeZipName $entry.FullName
        if ($entry.Length -lt 0 -or [UInt64]$entry.Length -gt $MaxEntryBytes) {
            throw "ZIP entry '$($entry.FullName)' exceeds its per-entry limit."
        }
        if ($entry.CompressedLength -lt 0) {
            throw "ZIP entry '$($entry.FullName)' has an invalid compressed length."
        }
        if ($entry.CompressedLength -eq 0 -and $entry.Length -gt 0) {
            throw "ZIP entry '$($entry.FullName)' has an invalid compression ratio."
        }
        if ($entry.CompressedLength -gt 0 -and
            ([UInt64]$entry.Length / [UInt64]$entry.CompressedLength) -gt
            [UInt64]$MaxCompressionRatio) {
            throw "ZIP entry '$($entry.FullName)' exceeds the compression-ratio limit."
        }
        $key = $normalized.Relative
        if ($pathKinds.ContainsKey($key)) {
            throw "ZIP contains a duplicate path (case-insensitive): $key"
        }
        $components = $normalized.Parts
        for ($index = 0; $index -lt ($components.Count - 1); ++$index) {
            $parent = ($components[0..$index] -join '/')
            if ($pathKinds.ContainsKey($parent) -and
                $pathKinds[$parent] -eq "file") {
                throw "ZIP contains a file/directory path collision: $key"
            }
        }
        if (-not $normalized.IsDirectory) {
            $childPrefix = $key + '/'
            foreach ($existing in $pathKinds.Keys) {
                if ($existing.StartsWith($childPrefix,
                    [StringComparison]::OrdinalIgnoreCase)) {
                    throw "ZIP contains a file/directory path collision: $key"
                }
            }
        }
        $kind = if ($normalized.IsDirectory) { "directory" } else { "file" }
        $pathKinds.Add($key, $kind)
        if (-not $normalized.IsDirectory) {
            if ([UInt64]$entry.Length -gt
                ($MaxExpandedBytes - $expandedEntryBytes)) {
                throw "ZIP entries exceed the expanded byte quota."
            }
            $expandedEntryBytes += [UInt64]$entry.Length
        }
        $entryRecords.Add([pscustomobject]@{
            Entry = $entry
            Relative = $normalized.Relative
            IsDirectory = $normalized.IsDirectory
            Parts = $normalized.Parts
        })
    }

    $manifestRecord = @($entryRecords | Where-Object {
        -not $_.IsDirectory -and $_.Relative -eq "manifest.json"
    }) | Select-Object -First 1
    $webRecord = @($entryRecords | Where-Object {
        -not $_.IsDirectory -and $_.Relative -eq "exam/web/index.html"
    }) | Select-Object -First 1
    if ($null -eq $manifestRecord -or $null -eq $webRecord) {
        throw "Exam package must contain manifest.json and exam/web/index.html."
    }
    $manifestBytes = Read-ZipEntryBytes $manifestRecord.Entry 1048576
    try {
        $utf8Strict = [Text.UTF8Encoding]::new($false, $true)
        $manifestText = $utf8Strict.GetString($manifestBytes)
        if ($manifestText.Length -gt 0 -and $manifestText[0] -eq [char]0xfeff) {
            throw "UTF-8 BOM is not permitted; the native host expects BOM-free JSON."
        }
        $manifest = $manifestText | ConvertFrom-Json
    } catch {
        throw "Exam manifest.json is not valid UTF-8 JSON: $($_.Exception.Message)"
    }
    $manifestValidation = Test-ExamManifest -Manifest $manifest -EntryKinds $pathKinds
    $manifestId = $manifestValidation.Id
    if (-not [string]::IsNullOrWhiteSpace($PackageId)) {
        if ($PackageId -notmatch '^[A-Za-z0-9._-]{1,96}$' -or
            $PackageId -in @('.', '..') -or
            -not [string]::Equals($PackageId, $manifestId,
                                  [StringComparison]::Ordinal)) {
            throw "Requested PackageId does not match manifest.id."
        }
    } else {
        $PackageId = $manifestId
    }

    $signatureRecord = @($entryRecords | Where-Object {
        -not $_.IsDirectory -and $_.Relative -eq "manifest.p7s"
    }) | Select-Object -First 1
    [bool]$signatureVerified = $false
    if (-not $AllowUnsigned) {
        if ($null -eq $signatureRecord) {
            throw "Package publisher signature manifest.p7s is missing."
        }
        $signatureBytes = Read-ZipEntryBytes $signatureRecord.Entry 1048576
        Test-ManifestSignature -ManifestBytes $manifestBytes -SignatureBytes $signatureBytes -Thumbprint $TrustedPublisherThumbprint
        $signatureVerified = $true
    } elseif ($null -ne $signatureRecord -and
              -not [string]::IsNullOrWhiteSpace($TrustedPublisherThumbprint)) {
        $signatureBytes = Read-ZipEntryBytes $signatureRecord.Entry 1048576
        Test-ManifestSignature -ManifestBytes $manifestBytes -SignatureBytes $signatureBytes -Thumbprint $TrustedPublisherThumbprint
        $signatureVerified = $true
    }

    $packageArea = Join-Path $publishFull $PackageId
    if (Test-ExistingReparsePath $packageArea) {
        throw "Package destination is reparse-backed."
    }
    New-Item -ItemType Directory -Path $packageArea -Force | Out-Null
    if (Test-ExistingReparsePath $packageArea) {
        throw "Package destination became reparse-backed."
    }
    $publishLockPath = Join-Path $packageArea '.publish.lock'
    if (Test-Path -LiteralPath $publishLockPath -ErrorAction SilentlyContinue) {
        if (Test-ExistingReparsePath $publishLockPath) {
            throw "Package publication lock is reparse-backed."
        }
        if (-not (Test-Path -LiteralPath $publishLockPath -PathType Leaf)) {
            throw "Package publication lock is not a regular file."
        }
    }
    # Serialize writers for one package identity. Without this lock, a second
    # stager can observe the directory between its rename and sidecar commit.
    $publishLock = Open-PublishLock -Path $publishLockPath
    $stage = Join-Path $packageArea (".staging-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Set-PrivateDirectoryAcl -Path $stage
    if (Test-ExistingReparsePath $stage) {
        throw "Temporary package directory is reparse-backed."
    }

    foreach ($record in $entryRecords) {
        $destination = Join-Path $stage ($record.Relative.Replace('/', '\'))
        $destinationFull = Get-FullPath $destination
        if (-not (Test-PathWithin -Root $stage -Candidate $destinationFull)) {
            throw "ZIP entry escaped the staging directory."
        }
        if ($record.IsDirectory) {
            New-Item -ItemType Directory -Path $destinationFull -Force | Out-Null
            continue
        }
        $parent = Split-Path -Parent $destinationFull
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        if (Test-ExistingReparsePath $parent) {
            throw "ZIP entry parent became reparse-backed."
        }
        $inputStream = $null
        $outputStream = $null
        try {
            $inputStream = $record.Entry.Open()
            $outputStream = [IO.File]::Open($destinationFull,
                [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
                [IO.FileShare]::None)
            $buffer = New-Object byte[] 65536
            [UInt64]$copied = 0
            while (($read = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                if ([UInt64]$read -gt ($MaxEntryBytes - $copied)) {
                    throw "ZIP entry expanded beyond its per-entry limit."
                }
                $outputStream.Write($buffer, 0, $read)
                $copied += [UInt64]$read
            }
            $outputStream.Flush($true)
            if ($copied -ne [UInt64]$record.Entry.Length) {
                throw "ZIP entry length or CRC validation failed."
            }
        } catch {
            throw "Could not extract ZIP entry '$($record.Relative)': $($_.Exception.Message)"
        } finally {
            if ($null -ne $outputStream) {
                $outputStream.Dispose()
            }
            if ($null -ne $inputStream) {
                $inputStream.Dispose()
            }
        }
        if (Test-ExistingReparsePath $destinationFull) {
            throw "Extracted package file is reparse-backed."
        }
    }

    $content = Get-ContentDigest -Root $stage -MaximumBytes $MaxExpandedBytes -MaximumFiles $MaxEntries
    if ($contentHashExpected -and
        -not [string]::Equals($contentHashExpected, $content.Digest,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw "Extracted package content SHA-256 does not match its pin."
    }
    if ([string]::IsNullOrWhiteSpace($contentHashExpected)) {
        throw "ExpectedContentSha256 is required; refusing to publish an unpinned package."
    }
    if ($RequireAuthenticode) {
        Test-AuthenticodeFiles -Root $stage
    }

    $finalPath = Join-Path $packageArea $content.Digest
    $sidecarPath = $finalPath + ".nstu-package.json"
    if (Test-Path -LiteralPath $finalPath) {
        if (Test-ExistingReparsePath $finalPath) {
            throw "Existing package destination is reparse-backed."
        }
        $existing = Get-ContentDigest -Root $finalPath -MaximumBytes $MaxExpandedBytes -MaximumFiles $MaxEntries
        if (-not [string]::Equals($existing.Digest, $content.Digest,
                                  [StringComparison]::OrdinalIgnoreCase)) {
            throw "A content-addressed package path already contains different bytes."
        }
        Remove-Item -LiteralPath $stage -Recurse -Force
        $stage = $null
    } else {
        # Apply the final read-only student ACL before publication. If this
        # fails, the private staging tree is removed and no public path exists.
        Set-PrivateDirectoryAcl -Path $stage -AllowUsersRead
        try {
            # The native move omits REPLACE_EXISTING, closing the
            # check-and-publish race. If another publisher won, revalidate its
            # bytes while the package lock is held and reuse only an exact match.
            Move-Atomically -Source $stage -Destination $finalPath
            $stage = $null
            $publishedFinalByThisRun = $true
        } catch {
            if (-not (Test-Path -LiteralPath $finalPath -PathType Container) -or
                (Test-ExistingReparsePath $finalPath)) {
                throw
            }
            try {
                $existing = Get-ContentDigest -Root $finalPath `
                    -MaximumBytes $MaxExpandedBytes -MaximumFiles $MaxEntries
            } catch {
                throw "Concurrent package publication created an invalid destination: $($_.Exception.Message)"
            }
            if (-not [string]::Equals($existing.Digest, $content.Digest,
                                      [StringComparison]::OrdinalIgnoreCase)) {
                throw "Concurrent package publication created different bytes at the content path."
            }
            Remove-Item -LiteralPath $stage -Recurse -Force
            $stage = $null
        }
    }

    $metadata = [ordered]@{
        schema = 1
        package_id = $PackageId
        content_sha256 = $content.Digest
        archive_sha256 = $archiveHashObserved
        file_count = $content.FileCount
        expanded_bytes = $content.ExpandedBytes
        signature_verified = $signatureVerified
        published_utc = [DateTimeOffset]::UtcNow.ToString("o")
    }
    $metadataTemp = $sidecarPath + ".tmp-" + [guid]::NewGuid().ToString("N")
    Write-DurableUtf8Text -Path $metadataTemp -Text ($metadata | ConvertTo-Json -Depth 4)
    if (Test-Path -LiteralPath $sidecarPath) {
        Remove-Item -LiteralPath $metadataTemp -Force
        $metadataTemp = $null
        [void](Read-PackageMetadata -Path $sidecarPath -PackageId $PackageId `
            -ContentDigest $content.Digest -ArchiveDigest $archiveHashObserved `
            -RequireSignature (-not $AllowUnsigned) -FileCount $content.FileCount `
            -ExpandedBytes $content.ExpandedBytes)
    } else {
        try {
            Move-Atomically -Source $metadataTemp -Destination $sidecarPath
            $metadataTemp = $null
            $metadataPublishedByThisRun = $true
        } catch {
            # A concurrent publisher may have committed the sidecar after the
            # existence check. Accept it only when every field matches exactly.
            if (-not (Test-Path -LiteralPath $sidecarPath -PathType Leaf) -or
                (Test-ExistingReparsePath $sidecarPath)) {
                throw
            }
            [void](Read-PackageMetadata -Path $sidecarPath -PackageId $PackageId `
                -ContentDigest $content.Digest -ArchiveDigest $archiveHashObserved `
                -RequireSignature (-not $AllowUnsigned) -FileCount $content.FileCount `
                -ExpandedBytes $content.ExpandedBytes)
            Remove-Item -LiteralPath $metadataTemp -Force
            $metadataTemp = $null
        }
    }

    [ordered]@{
        package_id = $PackageId
        content_sha256 = $content.Digest
        archive_sha256 = $archiveHashObserved
        published_root = $finalPath
        metadata_path = $sidecarPath
        file_count = $content.FileCount
        expanded_bytes = $content.ExpandedBytes
        signature_verified = $signatureVerified
    } | ConvertTo-Json -Compress
} catch {
    if ($null -ne $metadataTemp -and (Test-Path -LiteralPath $metadataTemp)) {
        Remove-Item -LiteralPath $metadataTemp -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $stage -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($publishedFinalByThisRun -and -not $metadataPublishedByThisRun -and
        $null -ne $finalPath -and (Test-Path -LiteralPath $finalPath -PathType Container)) {
        # Remove only a directory this invocation published, and only if its
        # content still matches the digest we verified. If that check fails,
        # leave the path for manual quarantine instead of deleting unknown data.
        try {
            $publishedDigest = Get-ContentDigest -Root $finalPath `
                -MaximumBytes $MaxExpandedBytes -MaximumFiles $MaxEntries
            if ($publishedDigest.Digest -eq $content.Digest) {
                Remove-Item -LiteralPath $finalPath -Recurse -Force -ErrorAction SilentlyContinue
            }
        } catch {
            # Preserve an unverifiable directory for operator inspection.
        }
    }
    throw
} finally {
    if ($null -ne $archive) {
        $archive.Dispose()
    }
    if ($archiveLocked -and $null -ne $archiveStream) {
        try {
            $archiveStream.Unlock(0, $archiveStream.Length)
        } catch {
            # The stream is about to close; an unlock failure is non-fatal.
        }
    }
    if ($null -ne $archiveStream) {
        $archiveStream.Dispose()
    }
    if ($null -ne $publishLock) {
        $publishLock.Dispose()
    }
}
