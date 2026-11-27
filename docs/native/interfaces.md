# 接口与数据约定

本文描述 Palladium 原生版各业务域的公开接口约定。实现以头文件为准，这里是写代码和审阅时需要遵守的边界。

## 通用约定

- C++20，命名空间 `nickel`。路径用 `std::filesystem::path`，UTF-8 公共字符串用 `std::string`，不跟随本地代码页。
- 持久化 JSON 使用 `nickel::Json`（`native/core/common/json.hpp` 中的 nlohmann::json 别名）；保留整数精度与未知字段。
- 同步服务调用可以抛 `nickel::Error`（含 code/context）；异常不穿过线程或 GUI 边界。
- 文件系统路径不根据展示名推导身份。
- 域服务不依赖 QApplication，可以单独测试；Qt 类型只在 UI 边界转换。
- 耗时操作接收取消令牌与类型化进度回调，离开 GUI 线程运行，并保留唯一的取消/提交线性化点。
- 秘密不进入公开 DTO、事件或日志。

## 域职责

| 域 | 实现位置 | 本地构建目标 |
| --- | --- | --- |
| foundation | `native/core/config`、`native/platform`、`native/tests/foundation` | `nickel_foundation` |
| transport | `native/core/network`、`tasks`、`archive`、`native/tests/transport` | `nickel_transport` |
| minecraft | `native/core/minecraft`、`native/tests/minecraft` | `nickel_minecraft` |
| identity | `native/core/accounts`、`media`、`native/tests/identity` | `nickel_identity` |
| community | `native/core/resources`、`documents`、`native/tests/community` | `nickel_community` |
| instances | `native/core/instances`、`packs`、`diagnostics`、`native/tests/instances` | `nickel_instances` |
| interface | `native/ui`、`native/tests/ui` | `nickel_ui` |

每个域有自己的 `CMakeLists.txt` 与测试可执行文件；根构建负责汇总。域库导出 `native/` 下的公开头文件，并链接 `nickel_common`（提供 `native/` include 目录与 nlohmann_json）。Qt Test 仅用于测试。

## 关键服务边界

1. **foundation**：配置根选择、路径与仓库；便携标记无效时直接报错；AES-256-GCM `v1:` 凭据存储，Windows 用 CNG，其他平台用系统或成熟加密库。
2. **transport**：同步 HTTP 客户端支持有界响应与流式下载；所有下载共享连接与预算。来源设置是快照，传输限额只在持久化成功后更新。任务协调器提供快照、取消与增量事件；归档服务显式接收配额、链接与路径策略。
3. **minecraft**：版本元数据与安装器；元数据/启动计划可在无网络时测试。启动请求包含游戏根、版本 ID、内部获取的账号凭据、有效设置与 launch ID；保留版本继承、规则、加载器、Java、natives 与参数语义。
4. **identity**：存储模型保持私有，公开账号不含秘密；离线、微软与 Yggdrasil 认证通过 HTTP 与仓库依赖完成。PNG/皮肤几何可测试，需要 Qt 的解码放在 UI。
5. **community**：与提供方无关的搜索/版本/依赖/正文 DTO，包含 source/id/revision；HTML 转为有界安全节点或 Markdown，使用成熟解析器而非正则；正文中的链接与资源按显式信任策略校验，不从描述加载本地/`file`/`qrc`/UNC 地址。
6. **instances**：整根路径、既有 JSON schema 与未知字段行为、命名档案事务；mrpack 与便携 schema 1 白名单；有界且脱敏的日志与进程归属。
7. **interface**：`MainWindow(AppServices&)` 只负责界面；应用 API 在 `native/app/services.hpp`。UI 不自行实现域、网络或存储服务，使用原生语义控件与自绘效果匹配既有布局。

## 改动约定

- 新增状态或 DTO 时同步更新本文档与对应测试。
- 需要新接口时先在所属域提供公开头文件与提案，不在 UI 里复制一份服务。
- 持久化格式或行为变化必须带回归测试，并说明对旧数据的处理。
