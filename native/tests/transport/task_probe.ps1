param([string]$Prefix='D:\workon\nickel-native-tools\install\windows-x64-static')
$ErrorActionPreference = 'Stop'
$lines = cmd.exe /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul & set'
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
New-Item -ItemType Directory -Force native/tests/transport/build-probe | Out-Null
$args = @('/nologo','/std:c++20','/MT','/EHsc','/W4','/WX','/utf-8','/I','native','/I',($Prefix+'\include'))
foreach ($source in @('native/core/tasks/tasks.cpp','native/core/tasks/secure_fs.cpp','native/core/network/control.cpp','native/core/network/hash.cpp')) {
    $name = [IO.Path]::GetFileNameWithoutExtension($source)
    & cl.exe @args /c $source ('/Fonative/tests/transport/build-probe/'+$name+'.obj')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& cl.exe @args native/tests/transport/task_test.cpp native/tests/transport/build-probe/tasks.obj native/tests/transport/build-probe/secure_fs.obj native/tests/transport/build-probe/control.obj native/tests/transport/build-probe/hash.obj /Fonative/tests/transport/build-probe/task_test.obj /Fenative/tests/transport/build-probe/task_test.exe /link bcrypt.lib
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& native/tests/transport/build-probe/task_test.exe
exit $LASTEXITCODE
