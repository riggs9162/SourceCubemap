# SourceCubemap

*这是翻译版本，最新内容请以[英文版 README](../../README.md) 为准。*

[English](../../README.md) | [Deutsch](README.de.md) | [Español](README.es.md) | [Français](README.fr.md) | [Português (Brasil)](README.pt-BR.md) | [Русский](README.ru.md) | 简体中文

几秒钟就能为 Source Engine 地图生成立方体贴图（cubemap），无需启动游戏。

SourceCubemap 会读取编译好的 `.bsp` 和你已安装的游戏内容，用显卡渲染所有立方体贴图，然后写出一个内置这些贴图的新 `.bsp`。原始地图不会被改动。

| 地图 | 立方体贴图 | 耗时 |
| --- | ---: | ---: |
| `gm_flatgrass` | 8 | 0.9 s |
| `gm_construct` | 28 | 0.9 s |
| Half-Life 2，76 张地图 | 6167 | 共 77 s |

测试配置：Ryzen 9 7900 和 RTX 3050。

> **实验性工具**：反射效果已经很接近真实效果，但与游戏 `buildcubemaps` 命令的结果并非逐像素一致。发布地图之前，请先在游戏里测试。

## 快速开始

你需要 64 位 Windows 10 或 11、支持 Vulkan 1.3 的显卡驱动，以及一款已安装的 Source 游戏。

1. 从[发布页面](https://github.com/riggs9162/SourceCubemap/releases)下载最新的 zip 压缩包，解压到任意位置。请把所有文件放在一起。
2. 运行 `sourcecubemap-gui.exe`。
3. 选择你的 `.bsp`，或把它拖到窗口里。如果地图就在游戏文件夹内，游戏文件夹会自动填好；否则请手动选择，例如 `GarrysMod\garrysmod`。
4. 点击 `Generate cubemaps`。
5. 查看预览和旁边的结果。`Options...` 里可以设置光照、画质、水面和额外的内容文件夹。
6. 把新的 `.bsp` 复制到游戏的 `maps` 文件夹。**不要更改生成的文件名**，因为立方体贴图正是按这个文件名存储的。

## 命令行

```powershell
sourcecubemap build map.bsp --game 'C:\Games\GarrysMod\garrysmod' --experimental --output map_built.bsp
sourcecubemap validate map_built.bsp
```

| 选项 | 作用 |
| --- | --- |
| `--game FOLDER` | 游戏内容文件夹。必填。 |
| `--profile gmod` 或 `hl2` | 遵循哪款游戏的规则。默认为 `gmod`。 |
| `--experimental` | 遇到无法精确还原的内容时继续运行，并在报告中列出。 |
| `--water planar` | 渲染水面反射。需要 `--experimental`。 |
| `--hdr auto`、`ldr`、`hdr` 或 `both` | 生成哪种光照。`auto` 会按地图已有的光照生成。 |
| `--quality fast`、`standard` 或 `high` | 在速度和纹理清晰度之间取舍。`high` 以双倍分辨率渲染。 |
| `--incremental` | 复用没有变化的立方体贴图。 |
| `--mount PATH` | 添加额外的内容文件夹或资源包。 |
| `--previews FOLDER` | 把立方体贴图的每个面保存为 PNG。 |
| `--json FILE` | 保存完整报告，包含所有警告。 |
| `--output FILE` | 地图的输出位置。默认为 `MAP_built.bsp`。 |
| `--overwrite` | 覆盖已有的输出文件。 |
| `--in-place` | 先备份，再替换原始地图。 |
| `--no-backup` | 跳过该备份，适用于自行保留地图副本的工具。 |

运行 `sourcecubemap build --help` 查看完整列表。`sourcecubemap inspect map.bsp` 可以查看地图的内容，不会构建任何东西。

## 构建中断时

默认情况下，如果地图用到了 SourceCubemap 无法还原的内容，它会停下来并告诉你具体是什么。加上 `--experimental` 就会改用最接近的替代效果继续运行。 Windows 程序默认就会这样做，可以在 `Options...` 里关闭。

使用 `--experimental` 时，缺失的材质或颜色纹理会渲染成纯灰色，缺失模型的道具会被跳过。每一处替换都会列在报告里，所以在信任结果之前请先检查报告。如果缺失的资源很多，请用 `--mount` 挂载地图需要的内容。

没有立方体贴图采样点的地图没有内容可生成，所以请先放置 `env_cubemap` 实体并重新编译。文件损坏时，构建总是会中止。

## 渲染内容

- 世界几何体、置换面、笔刷实体和静态道具
- 光照贴图，包括 LDR 和 HDR
- 3D 天空盒、雾效和 Garry's Mod 的绘制天空
- 常见的 Source 材质，包括 Phong 和细节纹理
- 平面反射水面、Sprite 和辉光
- 用 Valve 重打包工具压缩过的地图

仅支持桌面版 BSP 版本 20 的地图。游戏玩法、Lua、粒子、绳索和动画模型不会被渲染。具体覆盖范围请参阅[技术参考](../REFERENCE.md)（仅英文）。

## 从源码构建

安装 Visual Studio 并选择“使用 C++ 的桌面开发”工作负载，然后在 PowerShell 中运行：

```powershell
tools/build.ps1
```

程序会输出到 `build/release`。之后构建时可以使用 `tools/build.ps1 -SkipConfigure`。依赖项的版本已固定，也不包含任何游戏资源。

## 文档

- [技术参考](../REFERENCE.md)（仅英文）：渲染覆盖范围、缓存和报告
- [架构](../ARCHITECTURE.md)（仅英文）：整个流程如何协同工作及其局限
- [验证](../VALIDATION.md)（仅英文）：测试结果和测量数据

版本号格式为 `A.B.cc`：`A` 表示不兼容的更改，`B` 表示向后兼容的更改，`cc` 表示修复。

## 许可证

MIT。依赖项和参考资料见 [THIRD_PARTY.md](../../THIRD_PARTY.md)。
