# 开发与构建

本文面向构建与运行 Palladium 原生版。产品说明见 [原生入口](../../native/README.md)，接口见 [接口与数据约定](interfaces.md)；用户侧操作见 [使用指南](user-guide.md)。

## 工具链

| 项目        | 要求                                                                                         |
| ----------- | -------------------------------------------------------------------------------------------- |
| C++ / CMake | C++20；CMake 3.25+；Ninja                                                                    |
| Windows     | MSVC x64 + Windows SDK；生产 CRT 与依赖统一 `/MT`；不要混用 MinGW/MSVC 或 Debug/Release 前缀 |
| Linux       | GCC/Clang、线程库、静态 OpenSSL、XCB 等图形依赖                                              |
| macOS       | 匹配架构的 Apple host；交付脚本使用 Xcode 16.4 / SDK 15.5                                    |
| Python      | 3.12+；脚本以 `python -B` 执行                                                               |
| Qt          | QtBase / QtSvg / QtImageFormats 6.8.3；Widgets / Core / Gui / Svg，UI 测试另需 Qt6::Test     |

第三方版本与 SHA256 由 `scripts/native/deps-lock.json` 锁定，`scripts/native/bootstrap-deps.py` 按锁下载、构建并校验。依赖变化时更新锁定文件并说明原因，不要临时换成 latest。

## 检查工具链

Windows 上由 `scripts/native/msvc.py` 读取 Visual Studio 开发环境，把 VS 自带的 CMake/Ninja 补进当前命令：

```powershell
python -B scripts/native/msvc.py --help
python -B scripts/native/msvc.py -- cmake --version
python -B scripts/native/msvc.py -- ctest --version
python -B scripts/native/msvc.py -- ninja --version
python -B scripts/native/bootstrap-deps.py --help
```

`msvc.py` 支持 `--vs-root`；命令参数放在 `--` 之后。可选 `--log` 与 `--evidence` 必须同时提供，且目标文件尚不存在。

Linux：

```sh
python3 -B scripts/native/bootstrap-deps.py --help
cmake --version
ctest --version
ninja --version
```

## 依赖供应

Windows 示例（路径按实际磁盘替换）：

```powershell
python -B scripts/native/bootstrap-deps.py --tools-root 'D:/pd-tools' --work-root 'D:/pd-work' --build-root 'D:/pd-build' --target windows-x64-static --only core --jobs 1
python -B scripts/native/bootstrap-deps.py --tools-root 'D:/pd-tools' --work-root 'D:/pd-work' --build-root 'D:/pd-build' --target windows-x64-static --only parsers --jobs 1
python -B scripts/native/bootstrap-deps.py --tools-root 'D:/pd-tools' --work-root 'D:/pd-work' --build-root 'D:/pd-build' --target windows-x64-static --only qt --jobs 1
```

`--target` 只命名 install/build 子目录，安装前缀为 `<tools-root>/install/<target>`。Qt 阶段已包含 imageformats；单独 `imageformats` 用于给已有 Qt 补充。Linux 还需已有的静态 OpenSSL 前缀（下例中的 `$TOOLS`、`$WORK`、`$BUILD`、`$OPENSSL` 代表实际路径）：

```sh
python3 -B scripts/native/bootstrap-deps.py --tools-root "$TOOLS" --work-root "$WORK" --build-root "$BUILD" --target linux-x64-static --only core --jobs 2 --openssl-prefix "$OPENSSL"
python3 -B scripts/native/bootstrap-deps.py --tools-root "$TOOLS" --work-root "$WORK" --build-root "$BUILD" --target linux-x64-static --only parsers --jobs 2 --openssl-prefix "$OPENSSL"
```

## Windows

### 测试构建（UI + 测试）

`$prefix` 换成真实静态依赖前缀，`$oracle` 指向完整 checkout（含 `src/assets/` 与 `tests/brand/functional-icons.sha256`）：

```powershell
$prefix = 'D:/pd-tools/install/windows-x64-static'
$oracle = (Resolve-Path .).Path

python -B scripts/native/msvc.py -- cmake -S . -B build/native-windows-tests -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" "-DASSET_ORACLE_ROOT=$oracle" -DNICKEL_SHARED_NATIVE_ROOT= -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DNICKEL_BUILD_UI=ON -DNICKEL_BUILD_TESTS=ON -DNICKEL_ENABLE_IPO=OFF
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
python -B scripts/native/msvc.py -- cmake --build build/native-windows-tests --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
python -B scripts/native/msvc.py -- ctest --test-dir build/native-windows-tests --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
```

### 生产构建与单文件

`cmake --install` 会被 TESTS ON 构建拒绝；单文件产物从单独的 `TESTS=OFF` 目录生成：

```powershell
python -B scripts/native/msvc.py -- cmake -S . -B build/native-windows-prod -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" -DNICKEL_SHARED_NATIVE_ROOT= -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DNICKEL_BUILD_UI=ON -DNICKEL_BUILD_TESTS=OFF -DNICKEL_ENABLE_IPO=OFF
if ($LASTEXITCODE -ne 0) { throw 'Production configure failed' }
python -B scripts/native/msvc.py -- cmake --build build/native-windows-prod --target ni-launcher --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Production link failed' }

if (Test-Path -LiteralPath build/native-windows-prod/package) { throw 'Fresh package directory required' }
python -B scripts/native/msvc.py -- cmake --install build/native-windows-prod --config Release --prefix build/native-windows-prod/package
if ($LASTEXITCODE -ne 0) { throw 'Staging failed' }

$metadata = Get-Content -LiteralPath native/metadata.json -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json
$exe = Join-Path './build/native-windows-prod/package' ($metadata.executableName + '.exe')
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Installed executable is missing' }
python -B scripts/native/audit_binary.py $exe --output build/native-windows-prod/evidence/imports.json
python -B tests/native-acceptance/audit_windows.py $exe --output build/native-windows-prod/evidence/independent-imports.json
```

产物是 `pd-launcher.exe`，普通位置在构建目录的 `native/app/` 或安装目录。CMake target 仍名为 `ni-launcher`，输出名取自 `native/metadata.json`。Windows 静态程序不能用 `windeployqt` 的动态 DLL 部署替代单文件验收。

```powershell
& ./build/native-windows-prod/native/app/pd-launcher.exe
```

## Linux

当前已验证的范围是 core-only（`NICKEL_BUILD_UI=OFF`）。源码与构建目录放在 ext4 上；DrvFS / `/mnt/d` 会因 `renameat2` 语义出现假失败。

```sh
set -eu
cmake -S . -B build/native-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNICKEL_BUILD_UI=OFF -DNICKEL_BUILD_TESTS=ON -DNICKEL_ENABLE_IPO=OFF \
  "-DCMAKE_PREFIX_PATH=$PREFIX;$OPENSSL" \
  -DOPENSSL_ROOT_DIR="$OPENSSL" -DOPENSSL_INCLUDE_DIR="$OPENSSL/include" \
  -DOPENSSL_SSL_LIBRARY="$OPENSSL/lib/libssl.a" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL/lib/libcrypto.a" -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build build/native-linux --parallel 4
ctest --test-dir build/native-linux --no-tests=error --output-on-failure
```

Linux 的 Qt/UI 全量构建、安装包与真实窗口尚未验证；core-only 测试不能代替 Linux 产品验收，也不提供 AppImage/deb。

## macOS

`scripts/native/macos-delivery.py` 提供 `discover`、`verify-sources`、`build-qt`、`build-app`、`deploy`、`verify-materials` 与 `verify-runtime` 阶段，必须在匹配 arm64 / x86_64 的真实 Apple host 上运行；脚本拒绝非 Darwin、架构不匹配与 Rosetta。参数见 `python3 -B scripts/native/macos-delivery.py --help`。

当前没有 macOS 真机编译、CTest、部署或签名结果，也没有可用的 CI 分钟数；不要把脚本存在当成 macOS 可用。Apple 交付的 Qt 是动态 framework，静态 Qt 前缀只适用于 Windows。

## 测试

完整测试在 root 构建中注册，用 CTest 列出并运行；`ctest -N` 为空或跳过的项目记为未覆盖。可按域过滤：

```powershell
python -B scripts/native/msvc.py -- ctest --test-dir build/native-windows-tests -R '^foundation\.' --no-tests=error --output-on-failure
python -B scripts/native/msvc.py -- ctest --test-dir build/native-windows-tests -R '^minecraft_' --no-tests=error --output-on-failure
```

无需重新构建的轻量检查：

```powershell
python -B native/tests/brand/test_functional.py
python -B scripts/native/brand-test.py --help
python -B scripts/native/audit_binary.py --help
python -B tests/native-acceptance/audit_windows.py --help
```

mock Java、受控 HTTP 与 fixture 只能证明域或适配器行为；真实游戏、账号、完整 UI 与便携包流程需要单独验收。

## 打包与发布资产

- `scripts/native/release_package.py` 生成程序与源码 ZIP。
- `scripts/native/notice_resource_verify.py` 核对 EXE 中的 compiled notices。
- `scripts/native/legal_input_verify.py` 核对许可输入与嵌入结果。
- `scripts/native/audit_binary.py` 与 `tests/native-acceptance/audit_windows.py` 检查 import/模块。

这些工具只能证明它们检查的具体项目；发布前仍需按 [发布前检查清单](release-checklist.md) 逐项确认。

## 配置与日志

Microsoft 登录需要公共客户端 ID，通过 `-DNICKEL_PUBLIC_BUILD_CONFIG=<json>` 传入，格式参考 `secrets.local.example.json` 的 `microsoft` 段。该输入只放公共客户端信息，不要提交真实密钥；账号令牌由启动器在本地保存。

日志与错误报告应有界并先脱敏，分块输出不能漏掉跨块的秘密，stdout/stderr 都要读到结束。报告只引用属于本次 launch/task 的路径。

## 已知限制

- 工作区存在 `.snow/native-shared/snapshot.json` 时，配置会改用共享源码；自包含构建显式传 `-DNICKEL_SHARED_NATIVE_ROOT=`。
- `NICKEL_BUILD_TESTS=ON` 需要绝对路径的 `ASSET_ORACLE_ROOT`；缺失会在配置阶段失败。
- 测试构建不能 `cmake --install`；生产 staging 使用单独的 `TESTS=OFF` 目录。
- Windows 单文件、Linux UI 与 macOS 真机均未完成发布前验收。
