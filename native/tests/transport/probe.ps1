param([switch]$CompileOnly)
$ErrorActionPreference = 'Stop'
$lines = cmd.exe /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul & set'
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
New-Item -ItemType Directory -Force native/tests/transport/build-probe | Out-Null
& cl.exe /nologo /std:c++20 /MT /EHsc /W4 /utf-8 /I native /c native/core/tasks/secure_fs.cpp /Fonative/tests/transport/build-probe/secure_fs.obj
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cl.exe /nologo /std:c++20 /MT /EHsc /W4 /utf-8 /I native /c native/core/network/hash.cpp /Fonative/tests/transport/build-probe/hash.obj
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cl.exe /nologo /std:c++20 /MT /EHsc /W4 /utf-8 /I native /c native/core/network/budget.cpp /Fonative/tests/transport/build-probe/budget.obj
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($CompileOnly) { exit 0 }
& cl.exe /nologo /std:c++20 /MT /EHsc /W4 /utf-8 /I native native/tests/transport/safety_test.cpp native/tests/transport/build-probe/secure_fs.obj native/tests/transport/build-probe/hash.obj native/tests/transport/build-probe/budget.obj /Fonative/tests/transport/build-probe/safety_test.obj /Fenative/tests/transport/build-probe/safety_test.exe /link bcrypt.lib
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& native/tests/transport/build-probe/safety_test.exe
exit $LASTEXITCODE
