[CmdletBinding()]
param(
    [switch]$KeepArtifacts
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
$stagerType = "System.IO.Compression.ZipArchive" -as [type]
if ($null -eq $stagerType) {
    throw "System.IO.Compression.ZipArchive is unavailable on this Windows image."
}
$stager = Join-Path $PSScriptRoot "stage-exam-package.ps1"
$createdRoot = $false
$root = Join-Path ([IO.Path]::GetTempPath()) (
    "nstu-exam-staging-test-" + [guid]::NewGuid().ToString("N"))

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

function New-TestArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Entries
    )
    $stream = $null
    $archive = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew,
                                  [IO.FileAccess]::ReadWrite,
                                  [IO.FileShare]::None)
        $archive = [IO.Compression.ZipArchive]::new(
            $stream, [IO.Compression.ZipArchiveMode]::Create, $false)
        $records = if ($Entries -is [System.Collections.IDictionary]) {
            @($Entries.Keys | ForEach-Object {
                [pscustomobject]@{ Name = [string]$_; Value = $Entries[$_] }
            })
        } else {
            @($Entries)
        }
        foreach ($record in $records) {
            $name = $record.Name
            $value = $record.Value
            $entry = $archive.CreateEntry([string]$name)
            if ($null -ne $value) {
                $output = $null
                try {
                    $output = $entry.Open()
                    if ($value -is [byte[]]) {
                        $output.Write($value, 0, $value.Length)
                    } else {
                        $bytes = [Text.Encoding]::UTF8.GetBytes([string]$value)
                        $output.Write($bytes, 0, $bytes.Length)
                    }
                } finally {
                    if ($null -ne $output) {
                        $output.Dispose()
                    }
                }
            }
        }
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
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

function Get-ExpectedContentHash {
    param(
        [Parameter(Mandatory = $true)]$Entries
    )
    $files = [Collections.Generic.SortedDictionary[string, byte[]]]::new(
        [StringComparer]::Ordinal)
    foreach ($name in $Entries.Keys) {
        if ([string]$name -match '/$') {
            continue
        }
        $value = $Entries[$name]
        $bytes = if ($value -is [byte[]]) {
            $value
        } else {
            [Text.Encoding]::UTF8.GetBytes([string]$value)
        }
        $files.Add(([string]$name).Replace('\', '/'), $bytes)
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($pair in $files.GetEnumerator()) {
            $nameBytes = [Text.Encoding]::UTF8.GetBytes($pair.Key)
            Write-UInt64LittleEndian -Hash $hash -Value ([UInt64]$nameBytes.Length)
            $hash.TransformBlock($nameBytes, 0, $nameBytes.Length,
                                 $nameBytes, 0) | Out-Null
            Write-UInt64LittleEndian -Hash $hash -Value ([UInt64]$pair.Value.Length)
            if ($pair.Value.Length -gt 0) {
                $hash.TransformBlock($pair.Value, 0, $pair.Value.Length,
                                     $pair.Value, 0) | Out-Null
            }
        }
        $hash.TransformFinalBlock([byte[]]::new(0), 0, 0) | Out-Null
        return ([BitConverter]::ToString($hash.Hash)).Replace('-', '').ToLowerInvariant()
    } finally {
        $hash.Dispose()
    }
}

function Invoke-Stager {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$Publish,
        [Parameter(Mandatory = $true)][string]$ArchiveHash,
        [Parameter(Mandatory = $true)][string]$ContentHash,
        [string[]]$ExtraArguments = @()
    )
    $hostPath = Join-Path $PSHOME "pwsh.exe"
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) {
        $hostPath = (Get-Command powershell.exe -ErrorAction Stop).Source
    }
    $arguments = @(
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", $stager,
        "-ArchivePath", $Archive,
        "-PublishRoot", $Publish,
        "-ExpectedArchiveSha256", $ArchiveHash,
        "-ExpectedContentSha256", $ContentHash,
        "-AllowNonElevatedTest"
    ) + $ExtraArguments
    $stdoutPath = Join-Path $root ("stager-stdout-" + [guid]::NewGuid().ToString("N") + ".txt")
    $stderrPath = Join-Path $root ("stager-stderr-" + [guid]::NewGuid().ToString("N") + ".txt")
    try {
        $previousErrorAction = $ErrorActionPreference
        $ErrorActionPreference = "SilentlyContinue"
        try {
            & $hostPath @arguments 1> $stdoutPath 2> $stderrPath
        } finally {
            $ErrorActionPreference = $previousErrorAction
        }
        $exitCode = $LASTEXITCODE
        $stdout = if (Test-Path -LiteralPath $stdoutPath) {
            [IO.File]::ReadAllText($stdoutPath)
        } else {
            ""
        }
        $stderr = if (Test-Path -LiteralPath $stderrPath) {
            [IO.File]::ReadAllText($stderrPath)
        } else {
            ""
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
    [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($stdout + [Environment]::NewLine + $stderr).Trim()
    }
}

function Invoke-ManifestCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)]$Manifest,
        $AdditionalEntries = $null,
        [string[]]$ExtraArguments = @()
    )
    $entries = [ordered]@{
        "manifest.json" = $Manifest
        "exam/web/index.html" = "<!doctype html><html><body>test</body></html>"
    }
    if ($null -ne $AdditionalEntries) {
        foreach ($entryName in $AdditionalEntries.Keys) {
            if ($entries.Contains([string]$entryName)) {
                throw "Test case '$Name' defines a duplicate archive entry: $entryName"
            }
            $entries[[string]$entryName] = $AdditionalEntries[$entryName]
        }
    }
    $archive = Join-Path $root ($Name + ".nstuexam")
    $publish = Join-Path $root ($Name + "-out")
    New-TestArchive -Path $archive -Entries $entries
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    $contentHash = Get-ExpectedContentHash -Entries $entries
    $result = Invoke-Stager -Archive $archive -Publish $publish -ArchiveHash $archiveHash `
        -ContentHash $contentHash -ExtraArguments (@("-AllowUnsigned") + $ExtraArguments)
    [pscustomobject]@{
        Name = $Name
        Archive = $archive
        Publish = $publish
        ArchiveHash = $archiveHash
        ContentHash = $contentHash
        Result = $result
    }
}

function Assert-NoTransientArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$Publish,
        [string]$ContentDigest = ""
    )
    if (-not (Test-Path -LiteralPath $Publish)) {
        return
    }
    $transient = @(Get-ChildItem -LiteralPath $Publish -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like ".staging-*" -or $_.Name -like "*.tmp-*" })
    Assert-Condition ($transient.Count -eq 0) "failed staging leaves no temporary publication artifacts"
    if (-not [string]::IsNullOrWhiteSpace($ContentDigest)) {
        $published = @(Get-ChildItem -LiteralPath $Publish -Recurse -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.PSIsContainer -and $_.Name -ceq $ContentDigest })
        Assert-Condition ($published.Count -eq 0) "failed staging leaves no published content directory"
    }
}

function Test-AtomicPublicationCollisions {
    param(
        [Parameter(Mandatory = $true)][string]$MetadataPath,
        [Parameter(Mandatory = $true)][string]$PackagePath
    )
    # Exercise the actual native helper at the moment of publication: a
    # destination may appear after the stager's Test-Path precheck. Loading
    # only its type-definition function avoids running another installation
    # and keeps this regression independent of thread scheduling.
    $parseTokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $stager, [ref]$parseTokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) {
        throw "The staging helper must parse before its publication primitive can be tested."
    }
    $initializer = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq "Add-NativeMoveType"
    }, $false)
    if ($null -eq $initializer) {
        throw "The staging helper's native publication initializer was not found."
    }
    & ($initializer.Body.GetScriptBlock())

    $candidateMetadata = Join-Path $root "collision-candidate.nstu-package.json"
    [IO.File]::WriteAllText($candidateMetadata, '{"candidate":"must not replace published metadata"}',
        [Text.UTF8Encoding]::new($false))
    $metadataHash = (Get-FileHash -LiteralPath $MetadataPath -Algorithm SHA256).Hash
    $candidateHash = (Get-FileHash -LiteralPath $candidateMetadata -Algorithm SHA256).Hash
    $fileRejected = $false
    try {
        [NstuAtomicMove]::Move($candidateMetadata, $MetadataPath)
    } catch {
        $fileRejected = $true
    }
    Assert-Condition $fileRejected "atomic publication refuses an existing metadata file"
    Assert-Condition ((Get-FileHash -LiteralPath $MetadataPath -Algorithm SHA256).Hash -eq $metadataHash) "metadata collision preserves the existing file byte-for-byte"
    Assert-Condition ((Get-FileHash -LiteralPath $candidateMetadata -Algorithm SHA256).Hash -eq $candidateHash) "metadata collision preserves the unpublished source bytes"

    $candidateDirectory = Join-Path $root "collision-candidate-package"
    New-Item -ItemType Directory -Path $candidateDirectory | Out-Null
    $candidateFile = Join-Path $candidateDirectory "candidate-only.txt"
    [IO.File]::WriteAllText($candidateFile, "unpublished package bytes", [Text.UTF8Encoding]::new($false))
    $existingManifest = Join-Path $PackagePath "manifest.json"
    $manifestHash = (Get-FileHash -LiteralPath $existingManifest -Algorithm SHA256).Hash
    $directoryRejected = $false
    try {
        [NstuAtomicMove]::Move($candidateDirectory, $PackagePath)
    } catch {
        $directoryRejected = $true
    }
    Assert-Condition $directoryRejected "atomic publication refuses an existing package directory"
    Assert-Condition ((Get-FileHash -LiteralPath $existingManifest -Algorithm SHA256).Hash -eq $manifestHash) "directory collision preserves the published manifest bytes"
    Assert-Condition ((Test-Path -LiteralPath $candidateFile -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-Path $PackagePath "candidate-only.txt"))) "directory collision keeps source and destination trees separate"

    $newMetadata = Join-Path $root "collision-new.nstu-package.json"
    [NstuAtomicMove]::Move($candidateMetadata, $newMetadata)
    Assert-Condition ((Get-FileHash -LiteralPath $newMetadata -Algorithm SHA256).Hash -eq $candidateHash -and
        -not (Test-Path -LiteralPath $candidateMetadata)) "atomic publication succeeds when the file destination is absent"
}

try {
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $createdRoot = $true
    $publish = Join-Path $root "published"
    $archive = Join-Path $root "valid.nstuexam"
    $manifest = '{"id":"staging-test","title":"Staging test","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer"}]}'
    $entries = [ordered]@{
        "manifest.json" = $manifest
        "exam/web/index.html" = "<!doctype html><html><body>test</body></html>"
        "media/audio.txt" = "local media"
    }
    New-TestArchive -Path $archive -Entries $entries
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    $contentHash = Get-ExpectedContentHash -Entries $entries

    $result = Invoke-Stager -Archive $archive -Publish $publish -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($result.ExitCode -eq 0) "valid package stages successfully: $($result.Output)"
    $published = $result.Output | ConvertFrom-Json
    Assert-Condition (Test-Path -LiteralPath $published.published_root -PathType Container) "published directory exists"
    Assert-Condition ((Get-Content -LiteralPath (Join-Path $published.published_root "media/audio.txt") -Raw) -eq "local media") "extracted bytes are intact"
    Assert-Condition ((Get-Content -LiteralPath $published.metadata_path -Raw) -match [regex]::Escape($contentHash)) "sidecar records the content digest"
    Test-AtomicPublicationCollisions -MetadataPath $published.metadata_path -PackagePath $published.published_root

    $relativeArchive = Invoke-Stager -Archive "valid.nstuexam" -Publish $publish `
        -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($relativeArchive.ExitCode -ne 0) "relative archive paths are rejected"
    $relativePublish = Invoke-Stager -Archive $archive -Publish "relative-publish" `
        -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($relativePublish.ExitCode -ne 0) "relative publish paths are rejected"
    $driveRootPublish = Invoke-Stager -Archive $archive -Publish ([IO.Path]::GetPathRoot($publish)) `
        -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($driveRootPublish.ExitCode -ne 0) "drive-root publish paths are rejected"

    $repeat = Invoke-Stager -Archive $archive -Publish $publish -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($repeat.ExitCode -eq 0) "repeating the same content is idempotent"

    $assetManifest = '{"id":"asset-test","title":"Asset test","durationSeconds":60,"documents":[{"id":"brief","url":"documents/brief.pdf"}],"questions":[{"id":"listen","type":"listening","prompt":"Choose an answer","options":["A","B"],"audio":"media/prompt.wav"},{"id":"read","type":"reading","prompt":"Read the document","options":["A","B"],"pdf":"documents/brief.pdf"}]}'
    $assetCase = Invoke-ManifestCase -Name "manifest-assets" -Manifest $assetManifest -AdditionalEntries ([ordered]@{
        "documents/brief.pdf" = [byte[]](37, 80, 68, 70)
        "media/prompt.wav" = [byte[]](82, 73, 70, 70)
    })
    Assert-Condition ($assetCase.Result.ExitCode -eq 0) "manifest-declared document and media assets are staged"
    $assetPublished = $assetCase.Result.Output | ConvertFrom-Json
    Assert-Condition (Test-Path -LiteralPath (Join-Path $assetPublished.published_root "documents/brief.pdf") -PathType Leaf) "declared PDF asset is present"
    Assert-Condition (Test-Path -LiteralPath (Join-Path $assetPublished.published_root "media/prompt.wav") -PathType Leaf) "declared audio asset is present"

    $lockManifest = '{"id":"lock-test","title":"Lock test","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer"}]}'
    $lockEntries = [ordered]@{
        "manifest.json" = $lockManifest
        "exam/web/index.html" = "<!doctype html><html><body>test</body></html>"
    }
    $lockArchive = Join-Path $root "lock-test.nstuexam"
    $lockPublish = Join-Path $root "lock-test-out"
    New-TestArchive -Path $lockArchive -Entries $lockEntries
    $lockArchiveHash = (Get-FileHash -LiteralPath $lockArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $lockContentHash = Get-ExpectedContentHash -Entries $lockEntries
    New-Item -ItemType Directory -Path (Join-Path (Join-Path $lockPublish "lock-test") ".publish.lock") -Force | Out-Null
    $lockResult = Invoke-Stager -Archive $lockArchive -Publish $lockPublish -ArchiveHash $lockArchiveHash `
        -ContentHash $lockContentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($lockResult.ExitCode -ne 0) "non-file publication locks are rejected"
    Assert-NoTransientArtifacts -Publish $lockPublish -ContentDigest $lockContentHash

    $invalidManifestCases = @(
        [pscustomobject]@{
            Name = "manifest-missing-required"
            Manifest = '{"id":"missing-required","title":"Missing","durationSeconds":60}'
        }
        [pscustomobject]@{
            Name = "manifest-unsupported-property"
            Manifest = '{"id":"unsupported","title":"Unsupported","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer"}],"unexpected":true}'
        }
        [pscustomobject]@{
            Name = "manifest-invalid-question-type"
            Manifest = '{"id":"invalid-type","title":"Invalid","durationSeconds":60,"questions":[{"id":"q1","type":"not-a-type","prompt":"Answer"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-duplicate-question-id"
            Manifest = '{"id":"duplicate-id","title":"Duplicate","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"One"},{"id":"q1","type":"short_answer","prompt":"Two"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-invalid-options"
            Manifest = '{"id":"invalid-options","title":"Options","durationSeconds":60,"questions":[{"id":"q1","type":"multiple_choice","prompt":"Choose","options":["Only one"]}]}'
        }
        [pscustomobject]@{
            Name = "manifest-missing-asset"
            Manifest = '{"id":"missing-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"media/missing.wav"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-reserved-asset"
            Manifest = '{"id":"reserved-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"manifest.json"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-traversal-asset"
            Manifest = '{"id":"traversal-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"../secret.wav"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-query-asset"
            Manifest = '{"id":"query-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"media/prompt.wav?download=1"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-network-asset"
            Manifest = '{"id":"network-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"https://example.invalid/prompt.wav"}]}'
        }
        [pscustomobject]@{
            Name = "manifest-directory-asset"
            Manifest = '{"id":"directory-asset","title":"Asset","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer","audio":"media/"}]}'
        }
    )
    foreach ($invalidCase in $invalidManifestCases) {
        $case = Invoke-ManifestCase -Name $invalidCase.Name -Manifest $invalidCase.Manifest
        Assert-Condition ($case.Result.ExitCode -ne 0) "$($invalidCase.Name) is rejected"
        Assert-NoTransientArtifacts -Publish $case.Publish -ContentDigest $case.ContentHash
    }

    $validManifestBytes = [Text.Encoding]::UTF8.GetBytes($manifest)
    $bomManifest = [byte[]](@(0xef, 0xbb, 0xbf) + $validManifestBytes)
    $bomCase = Invoke-ManifestCase -Name "manifest-bom" -Manifest $bomManifest
    Assert-Condition ($bomCase.Result.ExitCode -ne 0) "UTF-8 BOM manifests are rejected"
    Assert-NoTransientArtifacts -Publish $bomCase.Publish -ContentDigest $bomCase.ContentHash

    $invalidUtf8Case = Invoke-ManifestCase -Name "manifest-invalid-utf8" -Manifest ([byte[]](0xff, 0xfe, 0xfd, 0x00))
    Assert-Condition ($invalidUtf8Case.Result.ExitCode -ne 0) "invalid UTF-8 manifests are rejected"
    Assert-NoTransientArtifacts -Publish $invalidUtf8Case.Publish -ContentDigest $invalidUtf8Case.ContentHash

    $badArchiveHash = ('0' * 64)
    $badArchive = Invoke-Stager -Archive $archive -Publish (Join-Path $root "bad-archive") -ArchiveHash $badArchiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($badArchive.ExitCode -ne 0) "archive digest mismatch is rejected"

    $badContent = Invoke-Stager -Archive $archive -Publish (Join-Path $root "bad-content") -ArchiveHash $archiveHash -ContentHash ('1' * 64) -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($badContent.ExitCode -ne 0) "content digest mismatch is rejected"
    Assert-Condition (@(Get-ChildItem -LiteralPath (Join-Path $root "bad-content") -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".staging-*" }).Count -eq 0) "failed content verification leaves no staging directory"

    $unsigned = Invoke-Stager -Archive $archive -Publish (Join-Path $root "unsigned") -ArchiveHash $archiveHash -ContentHash $contentHash
    Assert-Condition ($unsigned.ExitCode -ne 0) "unsigned package is rejected by default"

    $traversalEntries = [ordered]@{
        "manifest.json" = $manifest
        "exam/web/index.html" = "page"
        "../escape.txt" = "must not be written"
    }
    $traversalArchive = Join-Path $root "traversal.nstuexam"
    New-TestArchive -Path $traversalArchive -Entries $traversalEntries
    $traversalResult = Invoke-Stager -Archive $traversalArchive -Publish (Join-Path $root "traversal-out") -ArchiveHash ((Get-FileHash -LiteralPath $traversalArchive -Algorithm SHA256).Hash) -ContentHash ('2' * 64) -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($traversalResult.ExitCode -ne 0) "parent traversal entry is rejected"
    Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $root "escape.txt"))) "traversal cannot create a file outside the staging root"

    $duplicateEntries = @(
        [pscustomobject]@{ Name = "manifest.json"; Value = $manifest }
        [pscustomobject]@{ Name = "exam/web/index.html"; Value = "page" }
        [pscustomobject]@{ Name = "Foo.txt"; Value = "one" }
        [pscustomobject]@{ Name = "foo.txt"; Value = "two" }
    )
    $duplicateArchive = Join-Path $root "duplicate.nstuexam"
    New-TestArchive -Path $duplicateArchive -Entries $duplicateEntries
    $duplicateResult = Invoke-Stager -Archive $duplicateArchive -Publish (Join-Path $root "duplicate-out") -ArchiveHash ((Get-FileHash -LiteralPath $duplicateArchive -Algorithm SHA256).Hash) -ContentHash ('3' * 64) -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($duplicateResult.ExitCode -ne 0) "case-insensitive duplicate paths are rejected"

    $collisionEntries = [ordered]@{
        "manifest.json" = $manifest
        "exam/web/index.html" = "page"
        "collision" = "file"
        "collision/child.txt" = "child"
    }
    $collisionArchive = Join-Path $root "collision.nstuexam"
    New-TestArchive -Path $collisionArchive -Entries $collisionEntries
    $collisionResult = Invoke-Stager -Archive $collisionArchive -Publish (Join-Path $root "collision-out") -ArchiveHash ((Get-FileHash -LiteralPath $collisionArchive -Algorithm SHA256).Hash) -ContentHash ('4' * 64) -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($collisionResult.ExitCode -ne 0) "file/directory path collisions are rejected"

    $ratioEntries = [ordered]@{
        "manifest.json" = $manifest
        "exam/web/index.html" = ("A" * 20000)
    }
    $ratioArchive = Join-Path $root "ratio.nstuexam"
    New-TestArchive -Path $ratioArchive -Entries $ratioEntries
    $ratioResult = Invoke-Stager -Archive $ratioArchive -Publish (Join-Path $root "ratio-out") -ArchiveHash ((Get-FileHash -LiteralPath $ratioArchive -Algorithm SHA256).Hash) -ContentHash (Get-ExpectedContentHash -Entries $ratioEntries) -ExtraArguments @("-AllowUnsigned", "-MaxCompressionRatio", "2")
    Assert-Condition ($ratioResult.ExitCode -ne 0) "compression-ratio limit rejects a zip-bomb-shaped entry"

    $quotaResult = Invoke-Stager -Archive $archive -Publish (Join-Path $root "quota-out") -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned", "-MaxExpandedBytes", "10")
    Assert-Condition ($quotaResult.ExitCode -ne 0) "expanded-byte quota is enforced"

    $truncated = Join-Path $root "truncated.nstuexam"
    [IO.File]::WriteAllBytes($truncated, [byte[]](0x50, 0x4b, 0x03, 0x04, 0x00))
    $truncatedResult = Invoke-Stager -Archive $truncated -Publish (Join-Path $root "truncated-out") -ArchiveHash ((Get-FileHash -LiteralPath $truncated -Algorithm SHA256).Hash) -ContentHash ('5' * 64) -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($truncatedResult.ExitCode -ne 0) "truncated archives are rejected"

    $invalidSignatureEntries = [ordered]@{
        "manifest.json" = $manifest
        "exam/web/index.html" = "page"
        "manifest.p7s" = [byte[]](1, 2, 3, 4)
    }
    $invalidSignatureArchive = Join-Path $root "invalid-signature.nstuexam"
    New-TestArchive -Path $invalidSignatureArchive -Entries $invalidSignatureEntries
    $invalidSignatureResult = Invoke-Stager -Archive $invalidSignatureArchive -Publish (Join-Path $root "invalid-signature-out") -ArchiveHash ((Get-FileHash -LiteralPath $invalidSignatureArchive -Algorithm SHA256).Hash) -ContentHash (Get-ExpectedContentHash -Entries $invalidSignatureEntries)
    Assert-Condition ($invalidSignatureResult.ExitCode -ne 0) "invalid publisher signatures are rejected"

    $metadataObject = Get-Content -LiteralPath $published.metadata_path -Raw | ConvertFrom-Json
    $metadataObject.archive_sha256 = ('f' * 64)
    [IO.File]::WriteAllText($published.metadata_path,
        ($metadataObject | ConvertTo-Json -Depth 4),
        [Text.UTF8Encoding]::new($false))
    $tamperedResult = Invoke-Stager -Archive $archive -Publish $publish -ArchiveHash $archiveHash -ContentHash $contentHash -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($tamperedResult.ExitCode -ne 0) "tampered sidecar metadata is rejected"
    Assert-NoTransientArtifacts -Publish $publish

    $sidecarFailureManifest = '{"id":"sidecar-failure","title":"Sidecar failure","durationSeconds":60,"questions":[{"id":"q1","type":"short_answer","prompt":"Answer"}]}'
    $sidecarFailureEntries = [ordered]@{
        "manifest.json" = $sidecarFailureManifest
        "exam/web/index.html" = "<!doctype html><html><body>test</body></html>"
    }
    $sidecarFailureArchive = Join-Path $root "sidecar-failure.nstuexam"
    $sidecarFailurePublish = Join-Path $root "sidecar-failure-out"
    New-TestArchive -Path $sidecarFailureArchive -Entries $sidecarFailureEntries
    $sidecarFailureArchiveHash = (Get-FileHash -LiteralPath $sidecarFailureArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $sidecarFailureContentHash = Get-ExpectedContentHash -Entries $sidecarFailureEntries
    $sidecarFailurePath = Join-Path (Join-Path $sidecarFailurePublish "sidecar-failure") $sidecarFailureContentHash
    $sidecarFailurePath = $sidecarFailurePath + ".nstu-package.json"
    New-Item -ItemType Directory -Path $sidecarFailurePath -Force | Out-Null
    $sidecarFailureResult = Invoke-Stager -Archive $sidecarFailureArchive -Publish $sidecarFailurePublish `
        -ArchiveHash $sidecarFailureArchiveHash -ContentHash $sidecarFailureContentHash `
        -ExtraArguments @("-AllowUnsigned")
    Assert-Condition ($sidecarFailureResult.ExitCode -ne 0) "sidecar publication failure is reported"
    Assert-Condition (-not (Test-Path -LiteralPath ($sidecarFailurePath.Substring(0, $sidecarFailurePath.Length - ".nstu-package.json".Length)) -PathType Container)) "sidecar failure rolls back a newly published directory"
    Assert-NoTransientArtifacts -Publish $sidecarFailurePublish

    Write-Host "NSTU exam package staging tests passed."
} finally {
    if ($createdRoot -and -not $KeepArtifacts -and (Test-Path -LiteralPath $root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    } elseif ($createdRoot) {
        Write-Host "Artifacts retained at $root"
    }
}
