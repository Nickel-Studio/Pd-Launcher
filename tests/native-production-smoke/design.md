# 生产 AppServices / Qt Widgets 联调验收设计

本目录是一个独立的联调 harness，用来在真实编译产物上验证 `AppServices` 与 Qt Widgets 界面的行为。
它不修改生产模块、根 CMake 和其他测试；具体覆盖清单见同目录的 `matrix.json`。

## 验证对象

- 链接真实的 `AppServices`（继承真实 `UiServices` / `QObject`）、真实的 repository、task、事务、安装器与平台适配层。
- 可以注入受控的 HTTP、文件系统与自有子进程边界，但不把 `AppServices`、`Task`、事务或信号替换成 Fake。
- 域服务单测、工具自测、静态源码检查、仅 UI mock 或"能链接"本身都不算联调通过。

## 判定

每个用例在独立合成根目录中运行，结果只有四种：

| 结果      | 含义                                             |
| --------- | ------------------------------------------------ |
| `PASS`    | 真实入口行为与断言一致                           |
| `FAIL`    | 行为与断言不符，需要修代码                       |
| `BLOCKED` | 缺少源码、接口、环境或前置产物，无法开始         |
| `NOT_RUN` | 用例未被执行                                     |

单个用例通过不能推断完整登录、完整游戏/加载器矩阵、单文件部署、视觉保真、三平台或性能已验证。

## Harness 结构

- 独立 CMake 工程：链接已构建的 `nickel_foundation`、`nickel_common` 等真实目标，不修改根工程。
- 使用真实 `QApplication` 事件循环；fixture 在 QObject 所属线程调用 `request` / `submit`，用带 context 的 signal connection 捕获 `queryReady`、`commandFinished`、`stateChanged` 与媒体回复，对象销毁后不保留悬空引用。
- 每个请求记录递增的 RequestId、实际 reply id、公开 JSON、错误码、线程归属、可见 revision/nonce、状态事件序列与时限；超时记为 `FAIL` 或前置 `BLOCKED`，不视为空成功。
- 并发场景使用 latch、受控 HTTP 延迟和确定性 barrier，不用固定 sleep 猜边界；事件循环只负责交付，服务内部的队列、取消与事务保持真实。
- 涉及窗体的用例直接使用 `MainWindow(AppServices&)`，在语义控件点击后检查真实 DTO 与磁盘结果；视觉与输入法验收由专门测试覆盖，不由本 harness 宣称。

## 输入与运行

1. `matrix.json` 是机器可读的用例清单；补充 `UiQuery` / `UiAction` 枚举时必须同时给出用例处理方式。
2. 构建前确认磁盘余量与工具链版本，并使用独立的构建目录。
3. 运行 CTest 时保存 `--show-only=json-v1` 的 inventory 与实际执行结果，二者必须匹配。
4. 每个用例保留：实际源码与库哈希、harness 源哈希、编译器/Qt/CMake 版本、完整 argv 与工作目录、构建与运行退出码、可执行文件 SHA256、逐用例判定、重建后的持久化验证、stdout/stderr 哈希。

## 边界

- 进程退出的权威判断是 `GameSession::wait(std::chrono::timeout)` 的 dual EOF / exited，不用任务终态、GUI 阶段或单一 stdout EOF 代替。
- `Task::commit_external` 是外部发布的线性化点；`complete_work` 不得在 `tx.commit` / `spawn` 之后补调用，把失败伪装成成功。
- 兜底终止进程时只匹配记录过的 PID 与进程创建时间，不按进程名批量结束。
- 本 harness 通过只说明真实服务联调通过；静态单文件部署、平台封装、性能与法务材料仍由各自流程单独验收。