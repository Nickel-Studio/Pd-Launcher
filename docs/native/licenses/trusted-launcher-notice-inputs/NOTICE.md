# 版权与第三方资源

Palladium（Pd / 钯）由 ItzArona、ItzPlana 与项目贡献者维护。

- 原创程序代码与项目文档：GNU Affero General Public License v3.0（AGPL-3.0-only），见 [LICENSE](LICENSE)。
- Palladium 品牌与美术：Creative Commons Attribution-ShareAlike 4.0 International（CC-BY-SA-4.0），署名 Nickel Studio（ItzArona、ItzPlana），见 [LICENSE-ASSETS](LICENSE-ASSETS)。
- 第三方代码、字体、纹理与标识按各自的许可与使用条款保留。

## 当前品牌与美术授权

| 资源                           | 文件范围                                                                       | 授权与来源                                                |
| ------------------------------ | ------------------------------------------------------------------------------ | --------------------------------------------------------- |
| Palladium 品牌图形、横幅与字标 | `native/assets/brand/` 下当前的标记、字标与 `banners/` 中的 Palladium / Pd PNG | CC-BY-SA-4.0； Nickel Studio 为本项目设计，原件逐字节复制 |
| Palladium 应用与网站图标       | `native/assets/brand/icon*.png`、`mark-*.png` 及其导出的网站图标               | 同上；逐字节复制提供的原件，未重绘、未重新着色            |

复制、改编或再分发这些美术时，请注明作者（Nickel Studio / ItzArona、ItzPlana）、项目地址
https://github.com/Nickel-Studio/Pd-Launcher、许可名称 CC-BY-SA-4.0 以及修改情况，并以相同许可分享改编作品。

## 沿用素材

以下为随源码保留的既有原始字节，不属于当前 Palladium 品牌与美术许可范围，也未按 CC-BY-SA-4.0 重新授权：

| 资源                      | 文件范围                                               | 说明                                                           |
| ------------------------- | ------------------------------------------------------ | -------------------------------------------------------------- |
| 功能图标与默认 Mod 占位图 | `src/assets/blocks/`、`src/assets/mod-placeholder.png` | 实例、加载器与发布状态图标，随 `functional-resources.qrc` 编译 |
| 默认皮肤头像              | `src/assets/skins/`                                    | Steve / Alex 头像裁自 Minecraft 默认皮肤                       |

## 第三方代码

原生程序按 `scripts/native/deps-lock.json` 锁定版本与 SHA256，下列组件均实际参与编译或链接：Qt 6.8.3（qtbase、qtsvg、qtimageformats）、curl 8.23.0、libarchive 3.8.9、zlib 1.3.1、liblzma（xz 5.8.1）、zstd 1.5.7、bzip2 1.0.8、nlohmann JSON 3.11.3、gumbo 0.10.1（Apache-2.0）、toml++ 3.4.0、libyaml 0.2.5，以及 OpenSSL（libcrypto，Apache-2.0）与 Qt 各模块内含的第三方代码。完整许可原文、版本与来源证据在 [docs/native/licenses/](docs/native/licenses/)，随发行物一并核验；第三方许可不由 Palladium 的 AGPL 或美术许可替换。

### 按需取得的 Java 组件

| 组件                | 版本             | 许可                                        | 上游                                                                            |
| ------------------- | ---------------- | ------------------------------------------- | ------------------------------------------------------------------------------- |
| authlib-injector    | 1.2.8 / build 56 | AGPL-3.0-or-later，附 AUTHLIB-INJECTOR 例外 | [yushijinhun/authlib-injector](https://github.com/yushijinhun/authlib-injector) |
| Java Launch Wrapper | 1.4.4            | MIT                                         | [00ll00/java_launch_wrapper](https://github.com/00ll00/java_launch_wrapper)     |
| LWJGL Unsafe Agent  | 2.0              | Apache-2.0                                  | [HMCL-dev/lwjgl-unsafe-agent](https://github.com/HMCL-dev/lwjgl-unsafe-agent)   |

这些组件按游戏版本、Java 和认证方式取得。Java Launch Wrapper 保留 `Copyright (c) 2022 00ll00`，LWJGL Unsafe Agent 保留 `Copyright 2026 Glavo`。完整许可随附在 `LICENSES/`，其中 authlib-injector 的例外原文为 [AGPLv3-with-authlib-injector-exception.txt](LICENSES/AGPLv3-with-authlib-injector-exception.txt)。重新分发组件时一并保留相应许可、版权与 NOTICE，并提供许可要求的对应源码。

## 字体

`src/assets/fonts/nickel-line.woff2` 与桌面资源使用的 `native/assets/brand/fonts/nickel-text-subset.ttf` 是 Noto Sans SC 的字符子集，保留 Adobe 的版权声明与 SIL Open Font License 1.1。详见 [字体说明](src/assets/fonts/NOTICE.md) 与 [完整 OFL](LICENSES/OFL-1.1.txt)。品牌横幅中的文字已栅格化，字体文件不随横幅分发。

## 鸣谢图标

`src/assets/credits/shittimchests.png` 是 [ShittimChests](https://github.com/ShittimChests) 的 GitHub 组织头像，用于「更多 → 关于」与项目 README 的组织识别；图标权利属该组织，不纳入 Palladium 原创美术授权。来源列表见 [鸣谢图标](src/assets/credits/README.md)。

## 游戏纹理与外部标识

- `src/assets/blocks/` 的实例、加载器与发布状态图标，以及 `src/assets/mod-placeholder.png`，保留既有功能资源和分配映射。游戏纹理与对应项目标识的权利属于各自权利人；品牌更名不替换这些功能图标，也不将其重新授予 Palladium 原创美术许可。
- `src/assets/skins/` 的 Steve / Alex 头像裁自 Minecraft 默认皮肤，相关纹理归 Mojang / Microsoft；见 [Minecraft Usage Guidelines](https://www.minecraft.net/en-us/usage-guidelines)。

以上第三方资源不列入 Palladium 原创美术授权范围。
