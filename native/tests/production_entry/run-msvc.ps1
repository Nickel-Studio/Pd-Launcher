param(
    [string]$QtPrefix = 'C:/Users/youtu/AppData/Local/nickel-native-tools/install/windows-x64-static',
    [string]$BuildDirectory = 'native/tests/production_entry/objects',
    [switch]$IncludeTests
)
$ErrorActionPreference = 'Stop'
function File-Sha256([string]$Name) {
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Name))
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 Build Tools are required' }
$output = cmd /c ('call "' + $vs + '\VC\Auxiliary\Build\vcvars64.bat" >nul & set')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($line in $output) {
    $pair = $line -split '=', 2
    if ($pair.Count -eq 2 -and $pair[0] -and $pair[0] -ne 'PSModulePath') {
        [Environment]::SetEnvironmentVariable($pair[0], $pair[1], 'Process')
    }
}
$env:VSLANG = '1033'
$env:C_INCLUDE_PATH = ''
$env:CPLUS_INCLUDE_PATH = ''
$root = (Get-Location).Path
$destination = [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
if (-not $destination.StartsWith((Join-Path $root 'native/tests/production_entry/'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'All objects and generated headers must remain in the owned production_entry directory'
}
if (Test-Path -LiteralPath $destination) { throw 'Use a new object receipt directory; never overwrite compiled evidence' }
New-Item -ItemType Directory -Path $destination | Out-Null
$metadata = Get-Content -LiteralPath 'native/metadata.json' -Raw | ConvertFrom-Json
$template = Get-Content -LiteralPath 'native/app/BuildInfo.hpp.in' -Raw
$template = $template.Replace('@PROJECT_VERSION@', $metadata.version).Replace('@_nickel_displayName@', $metadata.displayName)
$template = $template.Replace('@_nickel_applicationId@', $metadata.applicationId).Replace('@_nickel_executableName@', $metadata.executableName)
$template = $template.Replace('@_nickel_license@', $metadata.license)
[IO.File]::WriteAllText((Join-Path $destination 'BuildInfo.hpp'), $template, (New-Object Text.UTF8Encoding($false)))
$include = @('/I', (Join-Path $root 'native'), '/I', (Join-Path $root 'native/app'), '/I', $destination)
foreach ($name in @('', 'QtCore', 'QtGui', 'QtWidgets', 'QtSvg', 'QtTest')) { $include += @('/I', ($QtPrefix + '/include/' + $name)) }
$common = @('/nologo', '/c', '/std:c++20', '/O2', '/DNDEBUG', '/DQT_STATIC', '/DQT_NO_DEBUG', '/DUNICODE', '/D_UNICODE',
    '/DQT_CORE_LIB', '/DQT_GUI_LIB', '/DQT_WIDGETS_LIB', '/DQT_SVG_LIB', '/MT', '/W4', '/WX', '/utf-8', '/EHsc', '/permissive-', '/Zc:__cplusplus', '/bigobj')
$units = @('native/app/main.cpp', 'native/app/AcceptancePaths.cpp', 'native/app/PerformanceApplication.cpp', 'native/app/PerformanceProbe.cpp')
if ($IncludeTests) { $units += 'native/tests/production_entry/entry_tests.cpp' }
$receipts = @()
$code = 1
$ErrorActionPreference = 'Continue'
foreach ($unit in $units) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($unit)
    $object = Join-Path $destination ($stem + '.obj')
    $log = Join-Path $destination ($stem + '.raw.log')
    $command = @($common) + @($include)
    $command += ('/Fo' + $object)
    $command += (Join-Path $root $unit)
    $begin = [DateTime]::UtcNow.ToString('o')
    & cl.exe @command 2>&1 | Tee-Object -FilePath $log
    $code = $LASTEXITCODE
    $end = [DateTime]::UtcNow.ToString('o')
    $receipts += [ordered]@{ source = $unit; sourceSha256 = (File-Sha256 $unit);
        configuration = 'Release'; flags = $command; exitCode = $code; beganUtc = $begin; endedUtc = $end;
        object = $object; rawLog = $log; rawLogSha256 = (File-Sha256 $log) }
    if ($code -ne 0) { break }
}
$receipt = [ordered]@{ schemaVersion = 1; kind = 'strict-release-entry-object-compilation'; QtPrefix = $QtPrefix;
    buildInfoSha256 = (File-Sha256 (Join-Path $destination 'BuildInfo.hpp'));
    linked = $false; productionRuntimePassed = $false; units = $receipts }
[IO.File]::WriteAllText((Join-Path $destination 'receipt.json'), ($receipt | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
exit $code
