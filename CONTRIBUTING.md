# 参与 Palladium

欢迎报告问题、改进交互、补充翻译或贡献代码。

## 报告问题

先搜索 [已有 Issue](https://github.com/Nickel-Studio/Pd-Launcher/issues)。遇到故障时，请提供：

- Palladium 版本与操作系统；
- 游戏版本、加载器和相关 Mod；
- 复现步骤，以及问题附近的日志。

上传日志前移除令牌、密码、邮箱和本机用户名。涉及账号认证、凭据泄露或任意文件读写的漏洞，请按 [安全报告](SECURITY.md) 私下提交。

## 提交改动

从 `main` 创建用途明确的分支，提交信息使用 `feat:`、`fix:`、`docs:`、`refactor:` 等前缀。Pull Request 说明改动内容、复现或操作路径和验证结果；涉及界面时附明暗两种模式的截图。

- 业务逻辑放在 `native/core/` 对应域，界面放在 `native/ui/`；跨域调用通过公开接口，不直接包含其他域的内部头文件。
- 错误用现有 `Result` / 错误类型返回，不要用异常穿过域边界；新增状态与 DTO 时同步更新 [接口约定](docs/native/interfaces.md)。
- 接口、持久化或行为变更要同时更新文档与相关回归测试；依赖变化在 Pull Request 中说明原因，并更新 `scripts/native/deps-lock.json`。
- 注释解释约束、需求和边界；能直接读懂的代码不需要逐行翻译。
- 界面文案用中文；明暗模式与颜色主题分别表述，动效尊重系统的“减少动态效果”设置。

## 开发环境

依赖、工具链和平台差异见 [原生开发指南](docs/native/development.md)。改动前先读 [原生入口](native/README.md)，再看所在域的直接调用方。

Windows 上最短的检查路径：

```powershell
python -B scripts/native/msvc.py -- cmake --version
python -B scripts/native/msvc.py -- cmake --list-presets
```

第二条命令应列出 `native-core` 与 `native-release`。真正的配置、构建与 CTest 需要已供应的静态 Qt 前缀，完整命令见开发指南。

## 许可

原创代码沿用 [AGPL-3.0-only](LICENSE)，Palladium 原创美术按 [native/assets/brand/NOTICE.txt](native/assets/brand/NOTICE.txt) 中的项目授权使用。引入第三方内容时补充来源和许可，并更新 [NOTICE.md](NOTICE.md)。
