# 原生构建的第三方许可材料

本目录保存原生（C++ / Qt）构建分发时需要的第三方许可原文、版本与来源证据，以及
核对这些材料的脚本入口；只有文本、清单与校验记录，不含依赖安装树或构建产物。

## 目录内容

- `brand-materials.json` — 品牌素材与字体清单（`scope` 为 `asset-only`），记录每个
  文件的实际路径、SHA-256 与用途；由 `scripts/native/brand-license-input.py` 从
  `native/assets/brand/asset-manifest.json`、许可证原文与字体元数据生成。
- `bundle-input.schema.json` — 材料输入的 JSON Schema。`check-input` 与 `collect`
  按它校验组件名称/版本/类别/链接方式、许可证与版权文本、artifact 与许可证文件，
  以及 `build`（可执行文件、构建证据）和 `relink`（对应源码、链接命令、重链接记录）。
- `trusted-launcher-notice-input.json` 与 `trusted-launcher-notice-inputs/` — 随包
  第三方通知的输入清单，连同 `LICENSE`、`LICENSE-ASSETS`、`NOTICE.md`、`SOURCE.txt`
  及各依赖许可原文的逐字节副本，每条记录 `bytes` 与 `sha256`。
- `noto-subset-metadata.json` — 内置 Noto Sans SC 子集的名字表、版权文本与原始哈希。
- `qtimageformats-source-only/` — Qt ImageFormats 6.8.3 的对应源码归档、其 SHA-256
  与 `source-files.json`。
- `native-dependency-evidence.schema.json` — 只读依赖检查输出的格式定义。

## 如何校验

材料路径是仓库内的精确相对路径，必须是真实、非空的普通文件，不接受 URL、符号链接
或模板。命令在仓库根目录执行；Python 依赖见 `scripts/native/brand-requirements.txt`，
流程见 [开发与构建](../development.md) 与 [发布前检查清单](../release-checklist.md)。

```powershell
python -B scripts/native/brand-legal.py check-input --input docs/native/licenses/brand-materials.json
python -B scripts/native/brand-legal.py collect --input docs/native/licenses/brand-materials.json --output build/legal-brand-verified
python -B scripts/native/brand-legal.py verify --bundle build/legal-brand-verified
```

- `check-input` 只核对引用文件存在且哈希匹配，不生成包。
- `collect` 要求输出目录为空，把材料逐字节复制过去，生成 `input-materials.json`、
  `THIRD-PARTY-NOTICES.txt`、`dependencies.spdx.json`（SPDX-2.3，用 spdx-tools 校验）
  与 `materials-manifest.json`。
- `verify` 不依赖原始安装路径，按副本重新核对哈希、清单与 SPDX 内容。

通知输入与品牌清单可以单独复核（通知清单绑定生成时的提交，历史不一致会报不匹配）：

```powershell
python -B scripts/native/legal_input_verify.py inputs
python -B scripts/native/brand-license-input.py
```

## 许可义务

项目本体使用 AGPL-3.0-only，对外分发时要提供完整的对应源码（含构建配置）。Qt 与
其他第三方组件仍适用各自的许可，AGPL 不会覆盖它们的条款。

Windows 构建静态链接 Qt。按 LGPLv3 分发时，随包要提供 LGPLv3 与并入的 GPLv3 全文、
库版权声明、使用该库及其许可的显著声明、对应的库源码与构建配置及 patch；同时提供
应用对应源码、资源编译输入、完整链接命令和构建/安装说明。用户修改库并重新链接的
权利必须可用：仓库用「替换为非原库后重新链接」的实测记录（`relink.probe`）证明这一点；
适用 Installation Information 时也要提供。

Qt 各 module 的开源许可与所含第三方条款分别核对；`LicenseRef-*` 例外条款保留原文，
不替换成标准 BSD 标签。游戏纹理、加载器、默认 Mod 与第三方项目标识按根 `NOTICE.md`
处理，不属于 Palladium 原创美术。

## 边界

- `asset-only` 清单通过只说明品牌素材齐全，不代表原生发行材料齐全。
- 原生发行材料要由实际安装树与构建输出生成，包括组件版本、SPDX 表达式、artifact
  哈希、许可证全文、对应源码归档和链接证据，逐项核对后随包保存。
- `.gitattributes` 固定许可文本与二进制材料的换行和字节，改动会破坏哈希校验。
- 工具能发现缺项、路径漂移和哈希不符，但不能代替权利人授权或法律审查。
