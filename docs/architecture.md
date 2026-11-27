# 架构

Palladium 用 C++20 与 Qt 6 Widgets 实现，界面自绘，业务逻辑与界面分离。

## 分层

| 目录 | 职责 |
| --- | --- |
| `native/core/` | 纯 C++ 业务域：配置与路径、网络、任务、归档、账号、实例、资源、Minecraft 元数据与启动计划 |
| `native/app/` | 应用层：启动编排、动作管线、服务装配、通知资源 |
| `native/ui/` | Qt Widgets 窗口、页面、控件与自绘组件 |
| `native/platform/` | 平台适配：进程、Java 探测、内存与 GPU 偏好 |
| `native/assets/` | 品牌素材、图标与 QRC 资源清单 |
| `native/tests/` | 域测试与组件测试 |
| `tests/native-*` | 跨域验收与工具脚本 |
| `cmake/` | 构建辅助：依赖、域注册、Qt 配置、生产 smoke |
| `scripts/native/` | 依赖供应、构建包装、许可校验与打包脚本 |

## 业务域

`native/core` 按域划分：

- `config`：配置根选择与 JSON 仓库。
- `network`：HTTP 客户端与共享传输预算。
- `tasks`：任务协调、取消与提交。
- `archive`：ZIP/TAR 安全解包。
- `accounts`：微软、Yggdrasil 与离线账号。
- `media`：皮肤与图片。
- `minecraft`：版本元数据、安装器、启动计划与进程启动。
- `resources`：Modrinth / CurseForge 检索。
- `documents`：受限 Markdown / HTML 正文。
- `instances`、`packs`：实例、整合包与便携包。
- `diagnostics`：有界日志与清理。

域之间通过公开头文件与 DTO 交互，不直接包含其他域的内部实现。

## 线程与取消

域服务不依赖 QApplication，可以脱离界面单独测试；Qt 类型只在 UI 边界转换。耗时操作在 GUI 线程之外运行，接收取消令牌与进度回调，并保留唯一的取消/提交线性化点。配置持久化成功后，才更新网络预算。

## 数据与兼容

- 配置根按“显式便携标记 → 已有系统配置 → 程序旁目录”选择，一次只使用一个完整根。
- 应用标识为 `studio.nickel.launcher`，便携标记为 `nickel.portable.json`（`format: nickel-portable`，`schemaVersion: 1`）。
- 实例设置与档案使用 `nickel.settings.json`、`nickel.profile.json`；未知 JSON 字段保留，整数值不被浮点化。
- 实例身份是“游戏根 + 版本 ID”，修改展示名不移动目录。
- 账号、密钥与缓存必须来自同一配置根；损坏配置或独立密钥不会被静默替换为空根。

## 下载与任务

版本、文件和社区 API 各自使用来源快照，一次操作不临时改写全局来源。所有文件下载共享传输预算，认证与皮肤请求不经过下载镜像。实例目录占用与任务生命周期统一互斥：取消先进入收尾，最终提交与取消互斥，失败清理只处理本事务预留的目录。

## 便携布局

便携包固定为 `nickel.portable.json` + `Nickel/pack.mrpack` + `.minecraft/`，可选 `Nickel/personalization.json` 与启动器程序。导出按白名单投影设置；账号、密钥、本机 Java/缓存路径与启动前命令不进入包。相对路径保留到落盘，发送界面时才解析为绝对路径。

## 界面

窗口保留四个顶栏入口与七个路由，首页为左侧启动卡加右侧背景。明暗模式与颜色主题相互独立，动效尊重系统“减少动态效果”设置，键盘焦点与弹窗行为由共享控件统一管理。关于页展示版本、许可与鸣谢。

## 相关文档

[接口与数据约定](native/interfaces.md) · [原生开发指南](native/development.md) · [使用指南](native/user-guide.md) · [原生入口](../native/README.md)
