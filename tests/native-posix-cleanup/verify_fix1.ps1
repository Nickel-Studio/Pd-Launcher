param([string]$SourceRoot, [string]$ArchiveRoot)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrEmpty($SourceRoot)) { throw 'Exact source root required' }
$worktree = (Get-Location).Path
function File-Digest([string]$Path) {
    if ([string]::IsNullOrEmpty($Path)) { throw 'Empty hash path' }
    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Check-OwnedPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($worktree + '\.snow\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside owner metadata' }
    $current = $full
    while ($current -ne $worktree) {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse input rejected' }
        $current = [System.IO.Path]::GetDirectoryName($current)
    }
}
$root = [System.IO.Path]::GetFullPath($SourceRoot)
Check-OwnedPath $root
if (Test-Path -LiteralPath (Join-Path $root '.publisher.lock')) { throw 'Publisher lock present' }
$markerPath = Join-Path $root 'publication-complete.json'
$manifestPath = Join-Path $root 'snapshot.json'
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($marker.publicationComplete -ne $true -or $manifest.metadata.receiver -ne 'posix-cleanup-acceptance' -or $manifest.schemaVersion -ne 1) { throw 'Incomplete or foreign receiver/schema' }
$manifestSha = File-Digest $manifestPath
if ($manifestSha -ne $marker.snapshotFileSha256) { throw 'Portal snapshot SHA mismatch' }
Write-Output ('FIX1_MANIFEST_SHA256 ' + $manifestSha + ' source=' + $root)
Write-Output ('FIX1_MARKER_SHA256 ' + (File-Digest $markerPath))
$sourceCount = 0
$sourceBytes = 0
$evidenceCount = 0
foreach ($section in @('files','evidence')) {
    foreach ($entry in $manifest.$section.PSObject.Properties) {
        $relative = $entry.Name.Replace('/','\')
        if ([System.IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or $relative.Contains(':')) { throw 'Unsafe entry path' }
        $path = Join-Path $root $relative
        Check-OwnedPath $path
        $item = Get-Item -LiteralPath $path -Force
        $sha = File-Digest $path
        if ($sha -ne $entry.Value.sha256 -or $item.Length -ne $entry.Value.bytes) { throw ('Frozen entry mismatch: ' + $entry.Name) }
        Write-Output ('FIX1_INPUT_SHA256 ' + $sha + ' ' + $entry.Name + ' bytes=' + $item.Length + ' section=' + $section)
        if ($section -eq 'files') { $sourceCount++; $sourceBytes += $item.Length } else { $evidenceCount++ }
    }
}
if ($sourceCount -notin @(11,24) -or $sourceCount -ne $marker.sourceFiles -or $sourceBytes -ne $marker.sourceBytes -or $evidenceCount -ne $marker.evidenceFiles) { throw 'Completion count/bytes mismatch' }
$archiveSource = $root
$archiveManifest = $manifest
if ($sourceCount -eq 24) {
    if ([string]::IsNullOrEmpty($ArchiveRoot)) { throw 'Explicit 11-source archive evidence root required' }
    $archiveSource = [System.IO.Path]::GetFullPath($ArchiveRoot)
    Check-OwnedPath $archiveSource
    if (Test-Path -LiteralPath (Join-Path $archiveSource '.publisher.lock')) { throw 'Archive publisher lock present' }
    $archiveManifestPath = Join-Path $archiveSource 'snapshot.json'
    $archiveMarkerPath = Join-Path $archiveSource 'publication-complete.json'
    $archiveManifest = Get-Content -LiteralPath $archiveManifestPath -Raw | ConvertFrom-Json
    $archiveMarker = Get-Content -LiteralPath $archiveMarkerPath -Raw | ConvertFrom-Json
    if ($archiveMarker.publicationComplete -ne $true -or $archiveManifest.metadata.receiver -ne 'posix-cleanup-acceptance' -or (File-Digest $archiveManifestPath) -ne $archiveMarker.snapshotFileSha256) { throw 'Archive publication mismatch' }
    if ((File-Digest $archiveManifestPath) -ne $manifest.metadata.fix1SnapshotSha256) { throw 'Composed archive identity mismatch' }
    foreach ($section in @('files','evidence')) {
        foreach ($entry in $archiveManifest.$section.PSObject.Properties) {
            $path = Join-Path $archiveSource $entry.Name.Replace('/','\')
            Check-OwnedPath $path
            if ((File-Digest $path) -ne $entry.Value.sha256 -or (Get-Item -LiteralPath $path).Length -ne $entry.Value.bytes) { throw 'Archive input mismatch' }
            Write-Output ('ARCHIVE_INPUT_SHA256 ' + $entry.Value.sha256 + ' ' + $entry.Name)
        }
    }
    $provenance = Join-Path $root 'provenance'
    if ((File-Digest (Join-Path $provenance 'fix1-snapshot.json')) -ne (File-Digest $archiveManifestPath)) { throw 'Fixed provenance mismatch' }
    $oldPath = Join-Path $provenance 'old-snapshot.json'
    $old = Get-Content -LiteralPath $oldPath -Raw | ConvertFrom-Json
    $oldMarker = Get-Content -LiteralPath (Join-Path $provenance 'old-publication-complete.json') -Raw | ConvertFrom-Json
    if ($oldMarker.publicationComplete -ne $true -or (File-Digest $oldPath) -ne $oldMarker.receiverManifestSha256 -or (File-Digest $oldPath) -ne $manifest.metadata.originalSnapshotSha256) { throw 'Original provenance mismatch' }
    $expectedFiles = @{}
    foreach ($entry in $old.files.PSObject.Properties) { $expectedFiles[$entry.Name] = $entry.Value }
    foreach ($entry in $archiveManifest.files.PSObject.Properties) { $expectedFiles[$entry.Name] = $entry.Value }
    if ($expectedFiles.Count -ne 24 -or $manifest.metadata.preservedEntries -ne 13 -or $manifest.metadata.repairedEntries -ne 11) { throw '13-plus-11 composition count mismatch' }
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $expected = $expectedFiles[$entry.Name]
        if ($null -eq $expected -or $expected.sha256 -ne $entry.Value.sha256 -or $expected.bytes -ne $entry.Value.bytes) { throw 'Composed source mismatch' }
    }
    Write-Output 'FIX1_COMPOSED_ROOT_PASS files=24 preserved=13 fixed=11 archive_evidence_only=true'
}
$archiveEntries = @($archiveManifest.files.PSObject.Properties)
if ($archiveEntries.Count -ne 11) { throw 'Archive source closure must be 11' }
$upstreamRoot = Join-Path $archiveSource 'upstream'
$upstreamManifestPath = Join-Path $upstreamRoot 'posix-leaf-fix1-manifest.json'
$upstream = Get-Content -LiteralPath (Join-Path $upstreamRoot 'posix-leaf-fix1-complete.json') -Raw | ConvertFrom-Json
$upstreamManifest = Get-Content -LiteralPath $upstreamManifestPath -Raw | ConvertFrom-Json
if ($upstream.publicationComplete -ne $true -or $upstream.runtimePassed -ne $false -or $upstream.owner -ne 'transport' -or $upstream.entries -ne 11) { throw 'Wrong upstream completion' }
if ($upstream.secureFsCppSha256 -ne $archiveManifest.files.'native/core/tasks/secure_fs.cpp'.sha256) { throw 'Upstream code mismatch' }
$zipPath = Join-Path $upstreamRoot 'posix-leaf-fix1-sources.zip'
if ((File-Digest $zipPath) -ne $upstream.zipSha256 -or (Get-Item -LiteralPath $zipPath).Length -ne $upstream.bytes) { throw 'ZIP SHA/bytes mismatch' }
if ((File-Digest $upstreamManifestPath) -ne $upstream.manifestSha256 -or @($upstreamManifest.files).Count -ne 11) { throw 'Upstream manifest mismatch' }
foreach ($entry in $upstreamManifest.files) {
    $expected = $archiveManifest.files.PSObject.Properties[$entry.path]
    if ($null -eq $expected -or $expected.Value.sha256 -ne $entry.sha256 -or $expected.Value.bytes -ne $entry.bytes) { throw 'Upstream selected source mismatch' }
}
Add-Type -AssemblyName System.IO.Compression
$stream = [System.IO.File]::OpenRead($zipPath)
$archive = [System.IO.Compression.ZipArchive]::new($stream, [System.IO.Compression.ZipArchiveMode]::Read, $false)
try {
    if ($archive.Entries.Count -ne 12) { throw 'ZIP 11-source-plus-manifest count mismatch' }
    $seen = @{}
    foreach ($entry in $archive.Entries) {
        if ($seen.ContainsKey($entry.FullName)) { throw 'Duplicate ZIP member' }
        $seen[$entry.FullName] = $true
        if ($entry.FullName -eq 'posix-leaf-fix1-manifest.json') {
            $expectedValue = $archiveManifest.evidence.'upstream/posix-leaf-fix1-manifest.json'
        } else {
            $expected = $archiveManifest.files.PSObject.Properties[$entry.FullName]
            if ($null -eq $expected) { throw 'Unexpected ZIP member' }
            $expectedValue = $expected.Value
        }
        if ($entry.Length -ne $expectedValue.bytes) { throw 'ZIP member size mismatch' }
        $member = $entry.Open()
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($sha.ComputeHash($member)).Replace('-','').ToLowerInvariant() }
        finally { $member.Dispose(); $sha.Dispose() }
        if ($digest -ne $expectedValue.sha256) { throw 'ZIP member SHA mismatch' }
        Write-Output ('FIX1_ZIP_MEMBER_SHA256 ' + $digest + ' ' + $entry.FullName)
    }
} finally { $archive.Dispose(); $stream.Dispose() }
Write-Output ('FIX1_PURE_D_VERIFICATION_PASS sources=' + $sourceCount + ' bytes=' + $sourceBytes + ' evidence=' + $evidenceCount + ' ZIP_sources=11 embedded_manifest=1 runtimePassed=false')
exit 0
