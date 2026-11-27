# Palladium / Pd / 钯 native 品牌资源

当前生产品牌使用项目自有的 Palladium 美术原件，不重绘、不裁切、不重新着色。`asset-manifest.json` 记录出口文件与源哈希，`source/palladium-intake.json` 记录入库时的原件清单；仅复制所需 54 个品牌源文件，Glass44 与功能图标不进入品牌映射。

## 当前 Pd 接口

- QRC prefix 保留 `:/nickel/brand/`；应用图标为 `icon.png` / `icon-light.png`，简标为 `mark-{black,white,blue,silver}.png`。浅底用黑、深底用白；蓝/银保留原色，不改变主题逻辑。
- 字标为 `wordmarks/{palladium,pd,ba,palladium-launcher,pd-launcher}-{black,white}.png`，全部来自 1280×320 透明原件。共 16 PNG 加原 TTF、OFL、字体 NOTICE 与当前 NOTICE，共 20 QRC 条目，无旧 SVG alias。
- 原图按实际需求延迟加载，禁止启动时全部解码；16 PNG 压缩 269306 字节，全部 RGBA 展开 41549824 字节。像素画/方块/skin 继续使用最近邻，不改变页面布局、矩形、过渡和效果。
- 文档 hero 为 `banners/palladium-launcher-hero-light.png` / `palladium-launcher-hero-dark.png`，均 1800×600，磁盘文档使用、不进桌面 QRC。其余横幅和平台资源也不嵌入运行时。
- Windows `platform/app.ico` 与 `app-light.ico` 保持原容器字节，九个 PNG 帧为 16/20/24/32/40/48/64/128/256。帧像素与对应原 PNG 相同，但压缩容器字节不同；不是重绘或原件错误。
- macOS `platform/app.icns` 仅包装现有原 PNG，含 16/32/64/128/256/1024 与对应 Retina，未伪造缺失的 512 图；真实平台图标验收另行进行。
- 品牌美术以 [CC-BY-SA-4.0](../../../LICENSE-ASSETS) 发布，署名 Nickel Studio（ItzArona、ItzPlana）；再分发时请保留署名、许可名称与修改说明。`source/palladium-intake.json` 记录原件来源与逐字节哈希。
- `46` 是原子序数、不是软件版本；旧 `nickel` namespace、功能资源 prefix、数据路径与协议保持兼容。

## 当前复现与验证

运行 `py -3.12 scripts/native/brand-test.py` 与 `py -3.12 scripts/native/brand-repro.py`；复现只核验/复制原字节和组装容器。

C++ 入口仍为 `native/tests/brand/CMakeLists.txt` / `nickel.brand.qt-resources`。included build 必须明确给绝对 `ASSET_ORACLE_ROOT`，只从可信原 checkout 读功能图和字体；当前品牌/功能映射从测试源码 checkout 读取，不复制整个 `src`。这些测试只说明资源文件本身可用，不替代同代 EXE、真实平台、视觉性能与正式依赖许可的验收。

## Qt 接口

把 `brand.qrc` 和 `functional-resources.qrc` 加入 Qt target，并开启 CMake `AUTORCC`。`brand.qrc` 仅列桌面 mark/icon/字标、TTF 与必需声明；每项 `qtEmbedded` 和 `qtResourceFiles` 在 manifest 中显式登记。网站横幅、favicon、ICO/ICNS、备用高分辨率 PNG 和 WOFF2 首选源只交付、不嵌入桌面。测试使用 `Qt6::Svg` 验证 SVG；生产如使用 PNG 或等价原几何绘制，不据此自动新增 QtSvg，最终由 build-release 审计实际用途和成本。

| 用途                | 稳定 resource URL                                                                                          |
| ------------------- | ---------------------------------------------------------------------------------------------------------- |
| 浅底/深底透明简标   | `:/nickel/brand/mark-black.png` / `:/nickel/brand/mark-white.png`                                          |
| 蓝/银原色简标       | `:/nickel/brand/mark-blue.png` / `:/nickel/brand/mark-silver.png`                                          |
| 应用图标            | `:/nickel/brand/icon.png` / `:/nickel/brand/icon-light.png`                                                |
| 全名字标（黑/白）   | `:/nickel/brand/wordmarks/{palladium,pd,ba,palladium-launcher,pd-launcher}-{black,white}.png`              |
| README/官网横幅     | 磁盘出口 `banners/palladium-launcher-hero-light.png` / `palladium-launcher-hero-dark.png`；不进 Qt runtime |
| Noto 文本子集       | `:/nickel/brand/fonts/nickel-text-subset.ttf`                                                              |
| 原功能图标          | `:/nickel/functional/icons/<原icon_key>.png`                                                               |
| 原 OptiFabric badge | `:/nickel/functional/icons/optifabric.png`                                                                 |
| 原默认 Mod          | `:/nickel/functional/mod-placeholder.png`                                                                  |
| Steve/Alex          | `:/nickel/functional/skins/steve-head.png` / `alex-head.png`                                               |
| 原鸣谢头像/标识     | `:/nickel/functional/credits/<原名>.png`                                                                   |

`/nickel/brand/` 前缀是历史兼容命名，当前实体内容全部为 Palladium 素材。Pd 图标按目标尺寸直接提供 PNG 原字节，不做 SVG 等比缩放，也不要把字标拉伸到容器宽度。原 18 项功能 PNG、Steve/Alex 与第三方标识只在 QRC 中引用，文件字节不变。OptiFabric 的专用 badge 与实例的 vanilla/type 推断不是同一条规则。

## 平台与文档导出

- Windows：`platform/app.ico` / `app-light.ico`，各含 16/20/24/32/40/48/64/128/256 九个原 PNG 帧。
- macOS：`platform/app.icns`，由 16/32/64/128/256/1024 与 Retina 32/64/256 原 PNG 帧组成；供应商未提供 512，不伪造。
- Linux：`platform/icon-{16,20,24,32,40,48,64,128,256}.png` 与 `icon-light-*`；桌面文件由 build-release 管理。
- 官网 favicon：`platform/favicon-16.png` / `favicon-32.png` / `favicon-48.png`；`website/build.mjs` 直接复制这些 Pd 原字节。
- 横幅：`banners/palladium-launcher-hero-*`（1800×600）、`pd-launcher-compact-*`（1800×360）、`palladium-element-*`（1800×520）、`pd-ba-bilingual-*`（1800×600），各含浅/深版。根 README 和官网是否接入由 build-release 统一，不自动改写。

## 重建与验证

```powershell
py -3.12 -m pip install --target .snow/brand-tools -r scripts/native/brand-requirements.txt
py -3.12 scripts/native/brand-palladium.py rebuild --output .snow/brand-rebuild
py -3.12 scripts/native/brand-test.py --functional-only
py -3.12 scripts/native/brand-test.py
py -3.12 scripts/native/brand-repro.py
py -3.12 scripts/native/brand-audit.py --assets-only
```

Linux/macOS 可换为 `python3` 并安装相同依赖。生成脚本仅在本 checkout 内输出；Qt 使用 offscreen 平台。默认测试不登录账号、不打开游戏、不启动联网行为、不接触进程。`rebuild` 从已入库的原字节重建整套出口；`brand-repro.py` 在临时目录逐字节复现，不会覆盖当前资源。

C++ 真资源测试入口为 `native/tests/brand/CMakeLists.txt`，可独立配置或由 root CMake `add_subdirectory(native/tests/brand)`；CTest 名称 `nickel.brand.qt-resources`。需要 build-release 的真实 Qt development/toolchain，不以 PySide6 测试替代 C++ 实发链接验收。

最终差异门禁由 `brand-audit.py --changes <实际diff清单>` 执行。输入 JSON 必须为 `schemaVersion: 1`、`owner: "brand-release"`，包含真实 `base` / `candidate` commit 或 SHA-256 快照 ID，以及所有变化路径 `paths`；重命名的旧/新路径均登记。由 CI/lead 产生实际差异，不能把允许目录或本工作树新文件列表冒充差异。`--assets-only` 不要求 diff，但明确保持 final difference、native UI consumer、native release 未验收。

## 授权边界

Palladium 供应原件（PNG/ICO/ICNS 字节副本）以 **CC-BY-SA-4.0** 提供，署名 Nickel Studio（ItzArona、ItzPlana），见根 [LICENSE-ASSETS](../../../LICENSE-ASSETS)；来源与原件哈希记录在 `source/palladium-intake.json`。本包不含任何 Nickel 时代的品牌图形。程序/文档沿用根 AGPL-3.0-only。

Noto WOFF2 原件与 weight400 TTF 转换分别保留同一 109 字符，0 PUA，不含“镍”和“/”，不是 icon font，也不是完整 CJK 字体；缺字使用宿主字体 fallback。字体始终独立遵循 OFL-1.1，Adobe 原版权与 Reserved Font Name 保留于 `fonts/OFL-1.1.txt`；品牌授权不覆盖字体或第三方功能资源。

静态 Qt 与其他 native 依赖的真实版本、notices、corresponding source 与重链接材料由 `docs/native/licenses/` 的实物校验入口协同 build-release 生成。资产测试通过不表示发行包许可证验收已完成。
