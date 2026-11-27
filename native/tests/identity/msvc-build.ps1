[CmdletBinding()]
param(
    [ValidateSet('Owner', 'Foundation', 'Core')]
    [string]$Mode = 'Owner',
    [string]$SharedNativeRoot,
    [string]$Prefix,
    [string]$VsRoot,
    [switch]$Fresh,
    [switch]$CleanFirst,
    [switch]$DeepJournal
)

$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$workspace = [IO.Path]::GetFullPath((Join-Path $source '../../..'))
if ([IO.Path]::GetFullPath((Get-Location).Path) -ne $workspace) {
    throw 'Run this script from its owning identity worktree root.'
}
if ($DeepJournal -and $Mode -eq 'Core') {
    throw 'Deep-journal verification requires Owner or Foundation mode with the actual repository.'
}
$temporary = Join-Path $source '.tmp'
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($workspace))
if ($drive.AvailableFreeSpace -lt 256MB) {
    throw 'Identity verification requires at least 256 MiB free on its own worktree drive; no build files were written.'
}
[IO.Directory]::CreateDirectory($temporary) | Out-Null
$env:TEMP = $temporary
$env:TMP = $temporary
if (-not $VsRoot) {
    $VsRoot = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/18/BuildTools'
}
$vcvars = Join-Path $VsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$cmake = Join-Path $VsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path $VsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'
$ninja = Join-Path $VsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
foreach ($tool in @($vcvars, $cmake, $ctest, $ninja)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Required build tool is missing: $tool" }
}
if (-not $SharedNativeRoot) {
    $SharedNativeRoot = Join-Path $workspace '.snow/native-shared/native'
}
$buildName = if ($Mode -eq 'Core') { '.build-msvc' } else { '.build-msvc-owner' }
$build = Join-Path $source $buildName
$cache = Join-Path $build 'CMakeCache.txt'
if (-not $Prefix -and (Test-Path -LiteralPath $cache -PathType Leaf)) {
    $match = Select-String -LiteralPath $cache -Pattern '^CMAKE_PREFIX_PATH:[^=]+=(.+)$' | Select-Object -First 1
    if ($match) { $Prefix = $match.Matches[0].Groups[1].Value }
}
if ($Mode -ne 'Core' -and -not $Prefix) {
    throw 'Owner/Foundation verification requires an explicit existing -Prefix or a configured shared prefix in CMakeCache.txt.'
}
if ($Prefix) {
    foreach ($path in $Prefix.Split(';')) {
        if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Dependency prefix does not exist: $path" }
    }
}
if ($Mode -ne 'Core') {
    $exports = @('core/common/CMakeLists.txt', 'core/config/CMakeLists.txt',
        'core/config/repository.hpp', 'core/config/vault.hpp',
        'core/config/conditional_file.hpp', 'core/config/conditional_file_impl.inc',
        'core/config/atomic_file.hpp', 'core/config/atomic_file.cpp',
        'core/config/path_guard.hpp', 'core/config/path_guard.cpp',
        'core/network/CMakeLists.txt', 'core/network/http.hpp')
    if ($Mode -eq 'Owner') {
        $exports += @('core/tasks/CMakeLists.txt', 'core/tasks/tasks.cpp',
            'core/tasks/secure_fs.hpp', 'core/tasks/secure_fs.cpp',
            'core/archive/CMakeLists.txt', 'core/archive/archive.hpp',
            'core/archive/archive.cpp', 'core/archive/zip_index.cpp', 'platform/CMakeLists.txt')
    }
    foreach ($export in $exports) {
        if (-not (Test-Path -LiteralPath (Join-Path $SharedNativeRoot $export) -PathType Leaf)) {
            throw "Matched actual owner export is missing: $export"
        }
    }
}
function Quote-CmdArgument([string]$Text) {
    if ($Text.IndexOfAny([char[]]'"%') -ge 0 -or $Text.Contains([string][char]10) -or $Text.Contains([string][char]13)) {
        throw 'Command arguments must not contain quotes, percent signs or newlines.'
    }
    return '"' + $Text.Replace('\', '/') + '"'
}
function Invoke-CompilerCommand([string]$Command, [string]$Log) {
    $wrapper = 'chcp 65001 >nul && call ' + (Quote-CmdArgument $vcvars) + ' >nul && ' + $Command +
               ' > ' + (Quote-CmdArgument $Log) + ' 2>&1'
    & cmd.exe /d /s /c $wrapper
    $code = $LASTEXITCODE
    if (Test-Path -LiteralPath $Log -PathType Leaf) {
        Get-Content -LiteralPath $Log -Encoding UTF8 -Tail 18
    }
    if ($code -ne 0) { throw "Build command failed ($code). See $Log" }
}
function Assert-RegisteredIdentityTests([string[]]$Names) {
    $inventory = & $ctest --test-dir $build --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the actual registered identity tests.' }
    $inventoryText = $inventory -join "`n"
    [IO.File]::WriteAllText((Join-Path $build 'test-registration.json'), $inventoryText, [Text.UTF8Encoding]::new($false))
    $registered = ConvertFrom-Json -InputObject $inventoryText
    foreach ($name in $Names) {
        $tests = @($registered.tests | Where-Object { $_.name -eq $name })
        if ($tests.Count -ne 1) { throw "Required identity test is not registered exactly once: $name" }
        foreach ($property in @($tests[0].properties)) {
            if (($property.name -eq 'DISABLED' -and $property.value) -or
                $property.name -eq 'SKIP_RETURN_CODE' -or $property.name -eq 'SKIP_REGULAR_EXPRESSION') {
                throw "Required identity test may not be disabled or skipped: $name"
            }
        }
    }
}
New-Item -Path $build -ItemType Directory -Force | Out-Null
$arguments = @('-S', (Quote-CmdArgument $source), '-B', (Quote-CmdArgument $build), '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    ('-DCMAKE_MAKE_PROGRAM=' + (Quote-CmdArgument $ninja)))
if ($Fresh) { $arguments += '--fresh' }
if ($Prefix) { $arguments += '-DCMAKE_PREFIX_PATH=' + (Quote-CmdArgument $Prefix) }
$shared = if ($Mode -eq 'Core') { '' } else { [IO.Path]::GetFullPath($SharedNativeRoot) }
$arguments += '-DNICKEL_IDENTITY_SHARED_NATIVE_ROOT=' + (Quote-CmdArgument $shared)
$runtime = if ($Mode -eq 'Owner') { 'ON' } else { 'OFF' }
$controlled = if ($Mode -eq 'Owner') { 'OFF' } else { 'ON' }
$deep = if ($DeepJournal) { 'ON' } else { 'OFF' }
$arguments += @("-DNICKEL_IDENTITY_TEST_TRANSPORT_RUNTIME=$runtime", "-DNICKEL_IDENTITY_CONTROLLED_ONLY=$controlled",
    "-DNICKEL_IDENTITY_REQUIRE_NATIVE_TESTS=$runtime", "-DNICKEL_IDENTITY_TEST_DEEP_JOURNAL=$deep")
Invoke-CompilerCommand ((Quote-CmdArgument $cmake) + ' ' + ($arguments -join ' ')) (Join-Path $build 'configure-script.log')
$buildArgs = @('--build', (Quote-CmdArgument $build), '--parallel', '1')
if ($CleanFirst) { $buildArgs += '--clean-first' }
Invoke-CompilerCommand ((Quote-CmdArgument $cmake) + ' ' + ($buildArgs -join ' ')) (Join-Path $build 'build-script.log')

$compiler = Get-ChildItem -LiteralPath (Join-Path $build 'CMakeFiles') -Filter CMakeCXXCompiler.cmake -Recurse -File | Select-Object -First 1
$rulePath = Join-Path $build 'CMakeFiles/rules.ninja'
if (-not $compiler -or -not (Test-Path -LiteralPath $rulePath -PathType Leaf)) { throw 'Compiler/Ninja dependency metadata is missing.' }
$compilerMatch = [regex]::Match([IO.File]::ReadAllText($compiler.FullName, [Text.Encoding]::UTF8), 'set\(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "([^"]+)"\)')
$rulesMatch = [regex]::Match([IO.File]::ReadAllText($rulePath, [Text.Encoding]::UTF8), '(?m)^msvc_deps_prefix = (.+)\r?$')
if (-not $compilerMatch.Success -or -not $rulesMatch.Success -or
    $compilerMatch.Groups[1].Value.TrimEnd() -cne $rulesMatch.Groups[1].Value.TrimEnd()) {
    throw 'Compiler and Ninja include prefixes differ. Re-run -Fresh -CleanFirst with this UTF-8 wrapper.'
}
$deps = & $ninja -C $build -t deps
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Ninja dependency records.' }
$depText = $deps -join "`n"
if ($depText -notmatch 'core[\\/]media[\\/]images\.hpp' -or $depText -notmatch 'core[\\/]media[\\/]builtin_heads\.hpp') {
    throw 'Ninja did not record media header dependencies. Re-run -Fresh -CleanFirst before trusting incremental objects.'
}
if ($Mode -eq 'Owner') {
    foreach ($header in @('core[\\/]config[\\/]conditional_file\.hpp',
            'core[\\/]config[\\/]repository\.hpp', 'core[\\/]archive[\\/]archive\.hpp')) {
        if ($depText -notmatch $header) { throw "Ninja did not record an actual native owner header: $header" }
    }
    $requiredTests = @('nickel_identity_controlled', 'nickel_identity_original_asset_parity',
        'nickel_identity_offline_uuid_golden', 'nickel_identity_offline_uuid_repository',
        'nickel_identity_foundation_adapter', 'nickel_identity_transport_request_contract',
        'nickel_identity_transport_runtime', 'nickel_identity_offline_native', 'nickel_identity_offline_native_options')
    if ($DeepJournal) { $requiredTests += 'nickel_identity_foundation_deep_journal' }
    Assert-RegisteredIdentityTests $requiredTests
}
$testLogName = if ($DeepJournal) { 'ctest-deep-journal-script.log' } else { 'ctest-script.log' }
$testLog = Join-Path $build $testLogName
& $ctest --test-dir $build --output-on-failure --parallel 1 --output-log $testLog
if ($LASTEXITCODE -ne 0) { throw "Identity CTest verification failed. See $testLog" }
Write-Host "Identity $Mode verification passed. Logs: $build"
