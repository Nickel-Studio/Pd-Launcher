# 使用指南

Palladium（Pd / 钯）是 Minecraft Java 版启动器。当前可用的程序是 Windows x64 构建；Linux 只验证过核心业务域，macOS 尚未在真机验证。

## 获取与启动

只运行来自可信发行清单、或你自己构建的程序，不要运行开发目录里来源不明的 EXE。把程序放在可写目录。

Windows：

```powershell
Get-FileHash -LiteralPath .\pd-launcher.exe -Algorithm SHA256
& .\pd-launcher.exe
```

单文件程序仍会在外部写配置、日志与缓存；Minecraft、Java、Mod 与存档不包含在 EXE 内。在只读目录写入失败时，启动器会报错，不会静默换到别的配置根。

Linux：

```sh
sha256sum ./pd-launcher
chmod u+x ./pd-launcher
./pd-launcher
```

Linux 当前只有核心业务域的构建与测试结果，尚无经过验证的 Qt/UI 程序，也没有 AppImage 或 deb。offscreen 自动化不等于桌面窗口验收。

macOS：尚无经过验证的构建。自行构建时使用完整 `.app`，整体移动到可写位置；首次数据写在 `.app` 同级目录，而不是签名包内部。

## 首次启动

首次使用需完成 EULA/首启确认；拒绝即退出。便携导入与页面按钮走同一后端流程。

可用的命令行参数只有 `--launch-instance` 与 `--game-root`：

```powershell
& .\pd-launcher.exe --launch-instance 'vanilla-test' --game-root 'D:\Pd-Test\game'
```

`--game-root` 必须是绝对路径，两个参数各只接受一次；它不选择配置根。没有 `--portable`、`--import`、`--recover` 或关闭 EULA 的参数。

## 账号

- **微软正版**：选择“正版”，按提示在浏览器完成设备码授权。需要构建时提供公共客户端 ID；未配置时无法实际登录。浏览器只在授权或用户明确点击外链时打开。
- **皮肤站**：填写 LittleSkin 或兼容 Yggdrasil 的站点地址，地址可以是站点根或 API 根，留空使用 LittleSkin；登录后选择角色。
- **离线**：在“设置 → 账号与隐私”填写用户名。离线账号不授予正版服务权限。

首页登录态可以切换已保存账号；多个角色时在角色入口重新获取列表。

## 实例与设置

在“选择版本”登记已有游戏根，再选择实例。“版本设置”可以按项继承或覆盖全局 Java、内存、窗口和参数。实例身份是“游戏根 + 版本 ID”，重命名展示名不会移动目录。

“移出列表”只解除登记；“删除目录”会删除磁盘内容，操作前会确认。删除前请自行备份存档与 Mod。

## 下载

“资源 → 原版游戏”安装原版与加载器；“资源 → 社区资源”检索 Modrinth / CurseForge。下载中心显示任务，可按任务取消并等待收尾。设置中可以分别调整版本列表、文件与社区 API 来源，以及线程数与限速。

## 外观

明暗模式（跟随系统 / 亮 / 暗）与颜色主题相互独立，共 12 个颜色主题。可以设置本地背景、模糊与遮罩；系统开启“减少动态效果”时，界面动画随之减少。

## 数据位置

按顺序选择唯一的完整配置根：

1. 程序旁有效的 `nickel.portable.json` 指定的 `Nickel`。
2. 已有系统配置：
   - Windows：Roaming AppData 下 `studio.nickel.launcher`。
   - Linux：`$XDG_CONFIG_HOME/studio.nickel.launcher`，否则 `~/.config/studio.nickel.launcher`。
   - macOS：`~/Library/Application Support/studio.nickel.launcher`。
3. 程序旁 `Nickel`。

不会把系统根的账号和程序旁的设置拼接；损坏的便携标记也不会静默回退。游戏目录优先使用已登记路径，不自动移动实例或存档。

| 文件 / 目录                                                      | 含义与注意事项                                       |
| ---------------------------------------------------------------- | ---------------------------------------------------- |
| `settings.json` / `game.json`                                    | 外观、下载、账号相关偏好与全局游戏设置；未知字段保留 |
| `accounts.json` / `active_account.json`                          | 账号记录与当前选择，可能包含敏感字段                 |
| `credentials.key`                                                | 本地加密密钥；备份账号必须同时保留，禁止公开         |
| `instances.json` / `onboarding.json`                             | 游戏根登记与首启状态                                 |
| `nickel.settings.json` / `nickel.profile.json`                   | 实例级设置、档案与扩展字段                           |
| `.nickel-transaction-*` / `.nickel-stage-*` / `.nickel-backup-*` | 未完成事务与恢复材料；出错时不要手工删除或覆盖       |

备份时先停止所有使用同一配置根的启动器，完整备份配置根与 `credentials.key`，再用副本验证新版本。`credentials.key` 是 AES-256-GCM `v1:` 格式的本地密钥，不是设备绑定加密；缺少或损坏时请恢复备份，不要删除 key 来“修复”账号。

## 中断与恢复

配置仓库在读/写前检查事务日志；有效日志可在校验后前滚。出现 `Conflict` / `RecoveryRequired` 时应停止普通写入，保留恢复材料并定位冲突，而不是手工删除事务文件后重试。目前没有公开的一键回滚命令。

## 便携包与分享

schema 1 的固定布局：

```text
nickel.portable.json
Nickel/pack.mrpack
Nickel/personalization.json    可选
.minecraft/                    目标游戏根
启动器程序                      可选，文件名沿用导出时的登记名
```

marker 的 `schemaVersion` 为 1、`format` 为 `nickel-portable`，严格拒绝未知字段与不匹配路径。导出只包含白名单设置，不含账号、`credentials.key`、本机 Java / 缓存路径、启动前命令、事务材料或测试输出。macOS 需保留完整 `.app`，不提供单二进制便携形式。

## 取消与进程边界

| 操作            | 正确理解                                                             |
| --------------- | -------------------------------------------------------------------- |
| 下载 / 安装取消 | 按任务 ID 取消，等待终态与临时目录收尾；窗口关闭不等于取消完成       |
| 启动准备取消    | 只取消准备阶段；当前没有停止运行中游戏的界面按钮，请在游戏中正常退出 |
| 账号登录取消    | 关闭设备码 / 角色选择，本次会话不再等待                              |

不要按进程名结束全部 Java、Node 或启动器，可能误伤其他游戏与工具。只管理自己拥有的句柄、launch ID 与任务 ID。

## 日志与清理

启动与下载错误会保存错误码、游戏 / 加载器 / Java 版本与操作步骤；下载详情提供“复制详情”“导出错误报告”。导出后仍需人工检查，移除账号文件、令牌、密钥、密码与敏感命令行。

“更多 → 前沿计划 → 清理游戏垃圾”会先生成预览，确认后才删除；范围限于已登记有效目录的日志、崩溃报告与临时文件，不包括游戏本体、Mod、资源包与存档。先导出要保留的日志，预览过期或被登记路径变更失效时需要重新预览。

## 正文与外观限制

社区详情以受限 Markdown / HTML 显示，不执行脚本、iframe、表单、任意 CSS 与视频；不可信正文不能读取本机文件或私网地址，图片走受控服务。支持范围不等于完整网页兼容。

外观的字体、DPI、输入法、辅助功能与高分辨率图片仍需按实际环境检查；窗口尺寸与空数据截图不能代替这些验收。

## 反馈

附版本、系统、游戏与加载器版本、复现步骤以及问题附近的日志，密码与令牌先移除。安全漏洞请按 [安全报告](../../SECURITY.md) 私下提交。
