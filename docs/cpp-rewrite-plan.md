# 原生实现与兼容契约

本文说明 Palladium 原生版的目标、结构划分与兼容约定。早期的分阶段重写计划已经完成，不再保留；当前实现以 [架构](architecture.md) 与 [接口约定](native/interfaces.md) 为准。

## 目标

- C++20 / CMake / Qt 6.8.3 Widgets，界面自绘；不使用 WebView、WebEngine、QML/Quick、JavaScript 运行时或 Rust 业务侧车。
- 保留既有界面布局与交互：四个顶栏入口、七个路由、首页启动卡、12 个颜色主题与独立明暗模式。
- Windows 交付单文件程序：静态 Qt、`/MT` 运行时、静态插件与资源闭合，不要求同目录 DLL 或 WebView2。

## 业务落点

| 职责 | 位置 |
| --- | --- |
| 配置根与仓库 | `native/core/config`、`native/platform` |
| HTTP、下载与任务 | `native/core/network`、`tasks`、`archive` |
| 账号与皮肤 | `native/core/accounts`、`media` |
| 版本、安装与启动 | `native/core/minecraft` |
| 社区检索与正文 | `native/core/resources`、`documents` |
| 实例、整合包与便携包 | `native/core/instances`、`packs` |
| 日志与诊断 | `native/core/diagnostics` |
| 界面与自绘 | `native/ui` |
| 应用编排 | `native/app` |

## 兼容约定

- 应用标识、数据根与文件格式保持原样：`studio.nickel.launcher`、`nickel.portable.json`、schema 1、现有 JSON 文件名与字段。
- 内部命名空间与 CMake 选项保留 `nickel` / `NICKEL_*`，避免脚本与构建记录失效。
- 应用 CMake target 为 `ni-launcher`，输出名取自 `native/metadata.json`。
- 未知 JSON 字段保留；实例身份基于游戏根与版本 ID；改名不迁移目录。
- 便携包只投影白名单设置，不包含账号、密钥、本机路径与启动前命令。

## 当前状态

- Windows x64：完整构建与测试通过，可产出单文件 `pd-launcher.exe`；尚未签名，未在干净机器验证。
- Linux x64：核心业务域可构建并通过测试；图形界面尚未构建。
- macOS：尚未在真机验证。

发布前应完成的检查见 [发布前检查清单](native/release-checklist.md)。

## 相关文档

[架构](architecture.md) · [接口与数据约定](native/interfaces.md) · [原生开发指南](native/development.md)
