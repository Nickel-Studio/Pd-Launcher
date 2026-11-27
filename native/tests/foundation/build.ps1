param([string]$BuildDirectory = '.foundation-build', [switch]$Fresh)
$ErrorActionPreference = 'Stop'
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 Build Tools are required' }
$output = cmd /c ('call "' + $vs + '\VC\Auxiliary\Build\vcvars64.bat" >nul & set')
foreach ($line in $output) {
    $pair = $line -split '=',2
    if ($pair.Count -eq 2) { [Environment]::SetEnvironmentVariable($pair[0],$pair[1],'Process') }
}
$env:VSLANG = '1033'
$cmake = $vs + '/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = $vs + '/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$configure = @('-S', 'native/tests/foundation', '-B', $BuildDirectory, '-G', 'Ninja',
    ('-DCMAKE_MAKE_PROGRAM=' + $ninja), '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_PREFIX_PATH=D:/workon/nickel-native-tools/install/windows-x64-static')
if ($Fresh) { $configure += '--fresh' }
# PowerShell 5 can treat redirected native stderr as a terminating error.
# Native tools report failure through their exit code; preserve that code.
$ErrorActionPreference = 'Continue'
& $cmake @configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build $BuildDirectory --clean-first --parallel 1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$ctest = $vs + '/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
& $ctest --test-dir $BuildDirectory --output-on-failure
exit $LASTEXITCODE
