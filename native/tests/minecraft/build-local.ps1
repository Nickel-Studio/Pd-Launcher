param(
    [string]$ToolsPrefix = 'D:/workon/nickel-native-tools/install/windows-x64-static',
    [string]$BuildDirectory = 'native/tests/minecraft/.build-resume',
    [switch]$SyntheticOnly
)
$ErrorActionPreference = 'Stop'
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
$vc = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
foreach ($tool in @($vc, $cmake, $ninja, $ctest)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required installed tool is absent: $tool" }
}
if (-not (Test-Path -LiteralPath $ToolsPrefix)) { throw 'Shared native dependency prefix is absent' }
$workspace = (Get-Location).Path
$build = [System.IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory))
$prefix = $workspace.TrimEnd('\') + '\'
if (-not $build.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build and temporary files must remain inside this worktree'
}
$drive = [System.IO.Path]::GetPathRoot($build)
if ([System.IO.DriveInfo]::new($drive).AvailableFreeSpace -lt 256MB) {
    throw 'Insufficient build space; no build or temporary write was attempted'
}
New-Item -ItemType Directory -Path $build -Force | Out-Null
$temp = Join-Path $build '.temp'
New-Item -ItemType Directory -Path $temp -Force | Out-Null
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $env:TEMP = $temp
    $env:TMP = $temp
    $native = if ($SyntheticOnly) { 'OFF' } else { 'ON' }
    $command = 'call "' + $vc + '" -arch=x64 -host_arch=x64 >nul && "' + $cmake + '" -S native/tests/minecraft/standalone -B "' + $build + '" -G Ninja -DCMAKE_MAKE_PROGRAM="' + $ninja + '" -DCMAKE_PREFIX_PATH="' + $ToolsPrefix + '" -DCMAKE_BUILD_TYPE=Release -DNICKEL_MINECRAFT_REQUIRE_NATIVE_SERVICES=' + $native + ' > "' + $build + '/configure.log" 2>&1 && "' + $cmake + '" --build "' + $build + '" --parallel 1 > "' + $build + '/build.log" 2>&1'
    & $env:ComSpec /d /s /c $command
    $code = $LASTEXITCODE
    Get-Content -LiteralPath ($build + '/configure.log') -Tail 12
    if (Test-Path -LiteralPath ($build + '/build.log')) {
        Select-String -LiteralPath ($build + '/build.log') -Pattern 'FAILED:|error [A-Z]+[0-9]+|ninja:|\[[0-9]+/[0-9]+\]' | ForEach-Object { $_.Line }
    }
    if ($code -ne 0) { exit $code }
    & $ctest --test-dir $build --output-on-failure
    exit $LASTEXITCODE
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}
