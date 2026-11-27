param(
    [Parameter(Mandatory = $true)][string] $CMake,
    [Parameter(Mandatory = $true)][string] $Ninja,
    [Parameter(Mandatory = $true)][string] $Prefix,
    [Parameter(Mandatory = $true)][string] $DeveloperCommand,
    [switch] $Qt,
    [switch] $CoreOnly,
    [switch] $TaskFaults,
    [UInt64] $MinimumFreeBytes = 512MB
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($CMake, $Ninja, $Prefix, $DeveloperCommand)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required approved build path is absent: $path" }
}
if ($Qt -and $CoreOnly) { Write-Output 'Explicit document/core subset: shared HTTP/archive/install/export adapters and application integration are excluded.' }
if ($TaskFaults -and $CoreOnly) { throw 'Transaction faults require the real full standalone aggregation.' }
$Source = $PSScriptRoot
$Build = if ($CoreOnly) { Join-Path $Source '.build/core' } else { Join-Path $Source '.build' }
$Temporary = Join-Path $Build 'temp'
$Drive = [System.IO.DriveInfo]::new([System.IO.Path]::GetPathRoot($Source))
if ($Drive.AvailableFreeSpace -lt $MinimumFreeBytes) {
    throw "Insufficient available worktree space before build/test writes: $Source"
}
[System.IO.Directory]::CreateDirectory($Temporary) | Out-Null
$QtValue = if ($Qt) { 'ON' } else { 'OFF' }
$CoreValue = if ($CoreOnly) { 'ON' } else { 'OFF' }
$FaultValue = if ($TaskFaults) { 'ON' } else { 'OFF' }
$command = 'call "' + $DeveloperCommand + '" -arch=x64 -host_arch=x64 >nul && "' + $CMake +
    '" -S "' + $Source + '" -B "' + $Build + '" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="' + $Ninja +
    '" -DCMAKE_PREFIX_PATH="' + $Prefix + '" -DNICKEL_TEST_QT=' + $QtValue + ' -DNICKEL_TEST_CORE_ONLY=' + $CoreValue +
    ' -DNICKEL_TEST_TASK_FAULTS=' + $FaultValue +
    ' && "' + $CMake + '" --build "' + $Build + '" --parallel 1 && "' + $CMake +
    '" -E chdir "' + $Build + '" "' + (Join-Path (Split-Path $CMake) 'ctest.exe') + '" --output-on-failure --parallel 1'
$PreviousTemp, $PreviousTmp, $PreviousTmpDir = $env:TEMP, $env:TMP, $env:TMPDIR
try {
    $env:TEMP = $Temporary
    $env:TMP = $Temporary
    $env:TMPDIR = $Temporary
    Write-Output ('Build/test temporary root: ' + $Temporary)
    # PowerShell 5 promotes native stderr to ErrorRecord; use the process exit code.
    $ErrorActionPreference = 'Continue'
    & $env:ComSpec /d /s /c $command 2>&1
    $Code = $LASTEXITCODE
} finally {
    $env:TEMP = $PreviousTemp
    $env:TMP = $PreviousTmp
    $env:TMPDIR = $PreviousTmpDir
}
exit $Code
