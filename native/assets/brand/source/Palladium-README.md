<picture>
  <source media="(prefers-color-scheme: dark)" srcset="banners/palladium-launcher-hero-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="banners/palladium-launcher-hero-light.png">
  <img alt="Palladium Launcher — 钯 / Pd，Nickel Launcher 的 C++ Reworked 版" src="banners/palladium-launcher-hero-light.png" width="100%">
</picture>

# Palladium · 品牌素材

**Palladium 是 Nickel Launcher 的 C++ Reworked 版。** 本套素材使用独立设计的 **Pd 双腔几何简标**，不复用 Ni 的 N ＋方点造型。字体与横幅网格延续 Nickel 的设计语言，配色为深海蓝与冷银。提供 **Palladium Launcher、Pd Launcher、Palladium、Pd、钯** 五种名称组合。

[整体视觉](preview.png) · [横幅总览](banners-preview.png) · [彩色透明名称](wordmarks-preview.png) · [黑白透明名称](wordmarks-mono-preview.png) · [图标小尺寸检查](small-size-check.png)

## 配色与保留规则

| 用途                                   | 色值                  |
| -------------------------------------- | --------------------- |
| 深色背景、浅色背景上的品牌文字         | `#0F1B2E` 深海蓝      |
| 深色背景上的品牌文字、浅色程序图标底色 | `#E2EAF4` 冷银        |
| 浅色横幅背景                           | `#F1F5FA`             |
| 深色／浅色横幅辅助文字                 | `#91A3BA` / `#53667E` |

- 维持平面、简洁的数字世界感，不添加透视、渐变、阴影或发光。
- 独立简标保留 P 与 d 的两个圆角腔体，在中央交界处留一条斜向窄缝，避免连体观感；缝宽约为主笔画的四分之一，小尺寸做最低 1.5 像素的光学校正。没有 N 的斜笔，也没有独立方点。
- 程序图标在每个目标尺寸上直接从 Pd 几何绘制；不使用 Nickel 图标像素，不反复缩放小图。两种配色共用同一圆角透明边界。
- 现有 Nickel、NoWheRE 素材、源图、根目录 README 和旧压缩包不改动；N ＋方点继续归 Ni 使用。
- Glass 方块图及其小尺寸导出保持不变，仍为 **R / N / A / B**，字高保持此前确认的 **37.5%**。Other 上的 N 是渠道识别字母，不是 Palladium 品牌简标。

## 四款 README 横幅

每款提供深色和浅色两张 RGB PNG，全部使用新的独立 Pd 简标。图片里的文字已经栅格化，不依赖阅读者安装字体。`dark` 表示深色底，`light` 表示浅色底。

| 排版                      | 尺寸       | 深色                                             | 浅色                                               | 适用位置               |
| ------------------------- | ---------- | ------------------------------------------------ | -------------------------------------------------- | ---------------------- |
| Palladium Launcher 主横幅 | 1800 × 600 | [dark](banners/palladium-launcher-hero-dark.png) | [light](banners/palladium-launcher-hero-light.png) | README 顶部，推荐      |
| Pd Launcher 紧凑横条      | 1800 × 360 | [dark](banners/pd-launcher-compact-dark.png)     | [light](banners/pd-launcher-compact-light.png)     | 希望减少纵向占用的页首 |
| Palladium 元素款          | 1800 × 520 | [dark](banners/palladium-element-dark.png)       | [light](banners/palladium-element-light.png)       | 项目封面、品牌介绍     |
| Pd／钯 中英款             | 1800 × 600 | [dark](banners/pd-ba-bilingual-dark.png)         | [light](banners/pd-ba-bilingual-light.png)         | 中文 README、品牌介绍  |

**Pd** 是钯的元素符号，**46** 是原子序数，不是软件版本号。横幅中的 `Nickel Launcher / C++ Reworked` 用于说明项目关系。

### Pd Launcher · 紧凑款

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="banners/pd-launcher-compact-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="banners/pd-launcher-compact-light.png">
  <img alt="Pd Launcher — Palladium / 钯" src="banners/pd-launcher-compact-light.png" width="100%" loading="lazy">
</picture>

### Palladium · 元素款

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="banners/palladium-element-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="banners/palladium-element-light.png">
  <img alt="Palladium，Pd 和原子序数 46" src="banners/palladium-element-light.png" width="100%" loading="lazy">
</picture>

### Pd／钯 · 中英款

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="banners/pd-ba-bilingual-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="banners/pd-ba-bilingual-light.png">
  <img alt="Pd／钯，Palladium Launcher" src="banners/pd-ba-bilingual-light.png" width="100%" loading="lazy">
</picture>

## 在项目 README 中使用

如果 `Palladium-assets/` 与项目 `README.md` 位于同一层，可使用以下相对路径。支持媒体查询的阅读器按主题切换；不支持 `<picture>` 的阅读器回退到 `<img>` 指定的浅色图。

```html
<picture>
  <source
    media="(prefers-color-scheme: dark)"
    srcset="Palladium-assets/banners/palladium-launcher-hero-dark.png"
  />
  <source
    media="(prefers-color-scheme: light)"
    srcset="Palladium-assets/banners/palladium-launcher-hero-light.png"
  />
  <img
    alt="Palladium Launcher — 钯 / Pd"
    src="Palladium-assets/banners/palladium-launcher-hero-light.png"
    width="100%"
  />
</picture>
```

只需固定深色版本时：

```md
![Palladium Launcher](Palladium-assets/banners/palladium-launcher-hero-dark.png)
```

如果 README 位于素材目录内部，请去掉路径开头的 `Palladium-assets/`，如本文件的实际展示。横幅保持原比例缩放，不裁切、不加滤镜。建议桌面显示宽度 800–1000 像素；窄屏上的细小辅助标签是装饰性信息，项目关系也保留在 README 正文中。

[900／375 像素深色预览](previews/readme-900-375-dark.png) · [900／375 像素浅色预览](previews/readme-900-375-light.png)。这些是本地缩放检查，不代表已测试远程 GitHub 渲染。

## 五种透明名称组合

每种名称都有 **蓝、银、黑、白** 四个版本，共 20 张 **1280 × 320 RGBA PNG**，全部使用独立 Pd 简标。四种颜色共用完全相同的 alpha 轮廓；蓝字适合浅色背景，银字适合深色背景，黑白版用于通用单色场景。

| 名称               | 蓝字                                          | 银字                                              | 黑字                                            | 白字                                            |
| ------------------ | --------------------------------------------- | ------------------------------------------------- | ----------------------------------------------- | ----------------------------------------------- |
| Palladium Launcher | [blue](wordmarks/palladium-launcher-blue.png) | [silver](wordmarks/palladium-launcher-silver.png) | [black](wordmarks/palladium-launcher-black.png) | [white](wordmarks/palladium-launcher-white.png) |
| Pd Launcher        | [blue](wordmarks/pd-launcher-blue.png)        | [silver](wordmarks/pd-launcher-silver.png)        | [black](wordmarks/pd-launcher-black.png)        | [white](wordmarks/pd-launcher-white.png)        |
| Palladium          | [blue](wordmarks/palladium-blue.png)          | [silver](wordmarks/palladium-silver.png)          | [black](wordmarks/palladium-black.png)          | [white](wordmarks/palladium-white.png)          |
| Pd                 | [blue](wordmarks/pd-blue.png)                 | [silver](wordmarks/pd-silver.png)                 | [black](wordmarks/pd-black.png)                 | [white](wordmarks/pd-white.png)                 |
| 钯                 | [blue](wordmarks/ba-blue.png)                 | [silver](wordmarks/ba-silver.png)                 | [black](wordmarks/ba-black.png)                 | [white](wordmarks/ba-white.png)                 |

## 程序图标与简标

- 1024 × 1024 程序图标：[深蓝底银色 Pd](logo/Palladium.png) / [冷银底深蓝 Pd](logo/Palladium-light.png)。圆角以透明 alpha 保留。
- Windows ICO：[深蓝版](logo/Palladium.ico) / [冷银版](logo/Palladium-light.ico)。每个包含 **16、20、24、32、40、48、64、128、256** 九个尺寸。
- 独立 PNG 位于 `logo/sizes/`，深蓝版如 [32px](logo/sizes/Palladium-32.png)，浅色版如 [32px](logo/sizes/Palladium-light-32.png)。
- 透明 Pd 简标：[蓝](logo/Palladium-mark-blue.png) / [银](logo/Palladium-mark-silver.png) / [黑](logo/Palladium-mark-black.png) / [白](logo/Palladium-mark-white.png)。四种颜色均由独立 Pd 几何生成，不是 Nickel 简标副本。
- [简标深浅对照](logo/variants.png)；小尺寸应用图标不要挤入完整品牌名称。

## 方块图与检查

四个 300 × 300 Glass 主图：[Release／R](blocks/Release.png)、[Other／N](blocks/Other.png)、[EarlyAlpha／A](blocks/EarlyAlpha.png)、[Beta／B](blocks/Beta.png)。另有九种尺寸的 36 张小图，位于 `blocks/sizes/`。

[方块总览](blocks-preview.png) · [方块小尺寸检查](blocks-small-size-check.png) · [字高参考对照](blocks-reference-check.png)。这些图片沿用 Nickel 文件，不属于此次品牌简标改画的范围。

`verification.json` 记录本地自动检查结果：原素材 SHA-256、方块字节级副本、Pd 简标与 Ni 轮廓不同、九种尺寸的双腔负空间、图标 alpha、ICO 帧与独立 PNG 的一致性、透明名称边距、横幅配色与对比度、文字／图片边界、README 本地链接。压缩包另做完整性及逐文件字节比对。

本目录是视觉素材交付，不包含应用源码，也不宣称已经编译 C++ 项目；不添加未经确认的功能、平台支持、下载地址或软件许可证。
