# 开发与构建

Palladium 使用 C++20 / CMake / Qt 6 Widgets。完整的工具链、依赖供应和平台差异见 [原生开发指南](native/development.md)；本文给出最短的上手路径。

## 环境

| 项目 | 要求 |
| --- | --- |
| C++ | C++20 |
| 构建 | CMake 3.25+、Ninja |
| Qt | 6.8.3，Core / Gui / Widgets / Svg；UI 测试另需 Qt6::Test |
| Python | 3.12+，脚本以 `python -B` 运行 |
| Windows | MSVC x64 + Windows SDK；`/MT` 运行时与静态 Qt |
| Linux | GCC/Clang、线程库、静态 OpenSSL、XCB 等图形依赖 |
| macOS | 匹配架构的 Apple host；交付脚本使用 Xcode 16.4 / SDK 15.5 |

第三方依赖由 `scripts/native/deps-lock.json` 锁定版本与 SHA256，`scripts/native/bootstrap-deps.py` 按锁供应并校验；不要换成未锁定的版本。

## 检查工具链

Windows 上由 `scripts/native/msvc.py` 加载 Visual Studio 开发环境，再执行命令：

```powershell
python -B scripts/native/msvc.py -- cmake --version
python -B scripts/native/msvc.py -- cmake --list-presets
```

`CMakePresets.json` 提供 `native-release`（完整 UI + 测试）与 `native-core`（纯业务域）。preset 的构建目录在 `.snow/` 下，适合本机试跑；可分发构建请指定源码树之外的目录。

## 构建与测试

完整命令（静态依赖前缀、`ASSET_ORACLE_ROOT` 与逐平台参数）见 [原生开发指南](native/development.md)。要点：

- `NICKEL_BUILD_TESTS=ON` 需要绝对路径的 `ASSET_ORACLE_ROOT`，且包含 `src/assets/` 与 `tests/brand/functional-icons.sha256`。
- 测试构建不能 `cmake --install`；单文件产物从单独的 `TESTS=OFF` 目录生成。
- 工作区存在 `.snow/native-shared/snapshot.json` 时，配置会改用共享源码；自包含构建请传 `-DNICKEL_SHARED_NATIVE_ROOT=`。

## 公共客户端配置

Microsoft 登录需要公共客户端 ID。通过 `-DNICKEL_PUBLIC_BUILD_CONFIG=<json>` 传入，格式可参考 `secrets.local.example.json` 的 `microsoft` 段；该文件只放公共客户端信息，不包含密钥。其他账号与密钥由用户在启动器内保存。

## 代码约定

- 业务逻辑放在 `native/core/` 对应域，界面放在 `native/ui/`；跨域调用通过公开接口。
- 错误用现有 `Result` / 错误类型返回，不用异常穿过域边界。
- 注释解释约束与边界；接口、持久化或行为变更同步更新文档与测试。
- 依赖变化在 Pull Request 中说明原因，并更新 `scripts/native/deps-lock.json`。

参与流程见 [贡献指南](../CONTRIBUTING.md)，架构见 [架构说明](architecture.md)。
