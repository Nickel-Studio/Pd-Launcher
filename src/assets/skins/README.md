# 内置兜底头像（Steve / Alex）

本目录的 `steve-head.png` / `alex-head.png` 是**从官方皮肤裁切出来的头部正面**，
用于头像解析链的最终兜底（远程皮肤与本地缓存都失败时随机二选一）：

```
远程皮肤(skin_fetch / mc-heads) → 本地缓存(localStorage) → 本目录内置头像（随机）
```

## 裁切约定

标准皮肤的头部正面位于 `(8, 8)`，尺寸为 `8×8`；使用最近邻放大为 `256×256`，保持像素边缘。裁切不改变游戏纹理的原有许可。

完整默认皮肤可从 Mojang 的官方示例资源查看：[Steve](https://github.com/Mojang/bedrock-samples/blob/main/resource_pack/textures/entity/steve.png) / [Alex](https://github.com/Mojang/bedrock-samples/blob/main/resource_pack/textures/entity/alex.png)。这些纹理属于 Mojang / Microsoft，使用时遵循 [Minecraft Usage Guidelines](https://www.minecraft.net/en-us/usage-guidelines)，不纳入 Nickel 原创美术授权。

> 注意：远程皮肤（`skin_fetch`）拿到的完整皮肤由前端 `cropSkinHead()` 在 canvas 中裁切，
> 会额外把帽子/发型层 `(40, 8)` 叠加到面部之上，因此联网时的头像细节比内置兜底更完整。
