# Test-only WMI process start/stop trace. Never terminate a process by name.
param(
    [Parameter(Mandatory=$true)][string]$Events,
    [Parameter(Mandatory=$true)][string]$Ready,
    [Parameter(Mandatory=$true)][string]$Stop,
    [Parameter(Mandatory=$true)][int]$DurationSeconds
)
$ErrorActionPreference = 'Stop'
$writer = $null
$startId = 'nickel-release-start-' + [Guid]::NewGuid().ToString('N')
$stopId = 'nickel-release-stop-' + [Guid]::NewGuid().ToString('N')
$utf8 = New-Object System.Text.UTF8Encoding($false)
try {
    if ([System.IO.File]::Exists($Events) -or [System.IO.File]::Exists($Ready)) {
        throw 'Trace output already exists'
    }
    $stream = New-Object System.IO.FileStream($Events, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    $writer = New-Object System.IO.StreamWriter($stream, $utf8)
    $writer.AutoFlush = $true
    Register-WmiEvent -Namespace root/cimv2 -Query 'SELECT * FROM Win32_ProcessStartTrace' -SourceIdentifier $startId | Out-Null
    Register-WmiEvent -Namespace root/cimv2 -Query 'SELECT * FROM Win32_ProcessStopTrace' -SourceIdentifier $stopId | Out-Null
    [System.IO.File]::WriteAllText($Ready, (@{status='subscribed';startedUtc=[DateTime]::UtcNow.ToString('o');collectorPid=$PID;source='Win32_ProcessStartTrace/StopTrace'} | ConvertTo-Json -Compress), $utf8)
    $deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
    while (([DateTime]::UtcNow -lt $deadline) -and (-not [System.IO.File]::Exists($Stop))) {
        Wait-Event -Timeout 1 | Out-Null
        foreach ($item in @(Get-Event | Where-Object { ($_.SourceIdentifier -eq $startId) -or ($_.SourceIdentifier -eq $stopId) })) {
            $raw = $item.SourceEventArgs.NewEvent
            $kind = 'stop'
            if ($item.SourceIdentifier -eq $startId) { $kind = 'start' }
            $record = @{kind=$kind;pid=[int]$raw.ProcessID;parentPid=[int]$raw.ParentProcessID;processName=[string]$raw.ProcessName;eventTime100ns=[UInt64]$raw.TIME_CREATED;receivedUtc=[DateTime]::UtcNow.ToString('o')}
            if ($kind -eq 'stop') { $record.exitStatus=[UInt32]$raw.ExitStatus }
            $writer.WriteLine(($record | ConvertTo-Json -Compress))
            Remove-Event -EventIdentifier $item.EventIdentifier
        }
        if ($writer.BaseStream.Length -gt 16777216) { throw 'Trace exceeds 16 MiB bounded output' }
    }
    foreach ($item in @(Get-Event | Where-Object { ($_.SourceIdentifier -eq $startId) -or ($_.SourceIdentifier -eq $stopId) })) {
        $raw = $item.SourceEventArgs.NewEvent
        $kind = 'stop'
        if ($item.SourceIdentifier -eq $startId) { $kind = 'start' }
        $record = @{kind=$kind;pid=[int]$raw.ProcessID;parentPid=[int]$raw.ParentProcessID;processName=[string]$raw.ProcessName;eventTime100ns=[UInt64]$raw.TIME_CREATED;receivedUtc=[DateTime]::UtcNow.ToString('o')}
        $writer.WriteLine(($record | ConvertTo-Json -Compress))
        Remove-Event -EventIdentifier $item.EventIdentifier
    }
    $writer.WriteLine((@{kind='collector-complete';finishedUtc=[DateTime]::UtcNow.ToString('o');source='Win32_ProcessStartTrace/StopTrace';lossDetection='WMI provider delivery has no zero-loss guarantee; bounded polling snapshots supplement it'} | ConvertTo-Json -Compress))
} catch {
    if ($null -ne $writer) {
        $writer.WriteLine((@{kind='collector-error';error=$_.Exception.Message;capturedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress))
    }
    exit 1
} finally {
    Unregister-Event -SourceIdentifier $startId -ErrorAction SilentlyContinue
    Unregister-Event -SourceIdentifier $stopId -ErrorAction SilentlyContinue
    if ($null -ne $writer) { $writer.Dispose() }
}
