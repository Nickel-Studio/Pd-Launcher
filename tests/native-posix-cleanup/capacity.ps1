param([string]$Phase = 'unspecified')
$ErrorActionPreference = 'Stop'
$drives = @{}
foreach ($name in @('C','D')) {
    $drive = Get-PSDrive -Name $name
    $drives[$name] = [Int64]$drive.Free
}
Write-Output ('CAPACITY phase=' + $Phase + ' utc=' + [DateTime]::UtcNow.ToString('o') + ' C_free=' + $drives['C'] + ' D_free=' + $drives['D'])
if ($drives['C'] -lt 1073741824 -or $drives['D'] -lt 268435456) {
    Write-Output 'CAPACITY_BLOCKED C_reserve=1073741824 D_reserve=268435456 no_new_compile_or_fixture=true'
    exit 3
}
Write-Output 'CAPACITY_PASS'
exit 0
