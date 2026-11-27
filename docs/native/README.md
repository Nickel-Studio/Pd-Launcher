# Palladium 原生文档

面向使用者的说明入口是 [native/README.md](../../native/README.md)；本目录放使用、开发、接口与发布相关文档。

平台现状：Windows x64 可完整构建并通过测试；Linux x64 仅核心业务域；macOS 尚未在真机验证。

- [用户指南](user-guide.md)：启动、账号、实例、数据位置、便携包与恢复。
- [开发与构建](development.md)：工具链、依赖供应、测试与各平台命令。
- [接口与数据约定](interfaces.md)：域职责、DTO、线程与兼容边界。
- [发布前检查清单](release-checklist.md)：发布前需要逐项确认的内容。
- [实现状态与已知限制](audit.md)：已实现、未验证与仍需注意的部分。

根层文档：[原生架构](../architecture.md)、[原生实现与兼容契约](../cpp-rewrite-plan.md)。
