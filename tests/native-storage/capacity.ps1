param(
    [string[]] $JunctionPath = @('D:\workon\nickel-native-tools'),
    [int[]] $OwnedWriterPid = @()
)
$ErrorActionPreference = 'Stop'

# Only DriveInfo and explicit link/process metadata; no directory enumeration or WSL invocation.
$root = [System.IO.Path]::GetFullPath($PSScriptRoot)
if (-not $root.StartsWith('D:\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The storage probe must run from a pure-D owned tool directory.'
}
$cursor = Get-Item -LiteralPath $root -Force
while ($null -ne $cursor) {
    if (($cursor.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw ('Tool ancestor is a reparse point: ' + $cursor.FullName)
    }
    $cursor = $cursor.Parent
}

$drives = @{}
foreach ($name in @('C', 'D')) {
    $drive = New-Object System.IO.DriveInfo ($name + ':\')
    if (-not $drive.IsReady) { throw ('Volume unavailable: ' + $name) }
    $drives[$name] = [ordered]@{
        availableBytes = $drive.AvailableFreeSpace
        totalFreeBytes = $drive.TotalFreeSpace
        totalBytes = $drive.TotalSize
        format = $drive.DriveFormat
    }
}
$links = @()
foreach ($name in $JunctionPath) {
    if ($name -notmatch '^[CD]:\\' -or $name -match '(?:^|\\)\.\.(?:\\|$)' -or
        $name.Substring(3).Contains(':')) { throw 'An explicit local junction path is required.' }
    $item = Get-Item -LiteralPath $name -Force
    $links += [ordered]@{
        path = $item.FullName
        linkType = $item.LinkType
        isReparsePoint = (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        targets = @($item.Target)
    }
}
$processes = @()
foreach ($id in $OwnedWriterPid) {
    if ($id -le 0) { throw 'Explicit positive owner PID required.' }
    $item = Get-CimInstance Win32_Process -Filter ('ProcessId = ' + $id) -ErrorAction Stop
    if ($null -ne $item) {
        $processes += $item | Select-Object ProcessId, ParentProcessId, Name, CommandLine
    }
}
[ordered]@{
    schemaVersion = 1
    capturedAt = [DateTime]::UtcNow.ToString('o')
    source = 'DriveInfo.AvailableFreeSpace'
    drives = $drives
    junctions = $links
    ownerProcessMetadata = $processes
    recursiveReads = 0
    writes = 0
    leaseAbsenceProven = $false
} | ConvertTo-Json -Depth 8
