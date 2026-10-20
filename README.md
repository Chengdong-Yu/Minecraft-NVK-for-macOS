# NVK Bootstrap 0.3.0

按实际图形 API 路由的 Minecraft 客户端启动模组：**Vulkan → NVK；OpenGL → Zink → NVK**。系统 WindowServer 继续合成桌面。

从 0.2.0 起，不再读取 Minecraft 版本来选路，也不再修改 `options.txt` 的渲染后端。Java 端同时准备两条图形库入口；游戏真正创建 OpenGL 上下文或 Vulkan swapchain 时，由本地接口决定路线。支持 LWJGL 2、GLFW 与 SDL3 窗口。`fabric.mod.json` 的 Minecraft 范围为 `*`；Forge/NeoForge 服务包不声明 Minecraft 版本范围。

## 安装包

- `dist/nvk-bootstrap-0.3.0-fabric.jar`：Fabric LanguageAdapter / preLaunch 接入。
- `dist/nvk-bootstrap-0.3.0-forge.jar`：Forge LaunchWrapper coremod / ModLauncher 启动服务，兼容 Java 8。
- `dist/nvk-bootstrap-0.3.0-neoforge.jar`：NeoForge GraphicsBootstrapper，另包含旧 ModLauncher 接入。

每个实例只安装与加载器对应的一个包。包内自带 Mesa EGL/Zink、定制 GLFW、SDL3 接入层、SDL3 原始库、OpenGL 函数解析入口、NVK 呈现器和 JNI 环境配置库。JAR 约 6.5 MiB；首次运行校验 SHA-256 后解包到实例的 `.nvk-bootstrap/macos-x86_64-<内容哈希>/`，无需原 Downloads 目录里的旧测试项目或额外 NVK JVM 参数。

当前 native 构建适用于 macOS x86_64，依赖系统已安装并加载的 NullMoth NVIDIA/NVK 驱动；本机使用 2.3.7 和 RTX 5060。不会安装或替换系统驱动。驱动路径是 `/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib`；启动时检查所需私有 IOSurface 接口。

**取消 Minecraft 版本白名单，不等于已验证所有历史版本。** 当前接入包括 LWJGL 2 / 3、GLFW 3 / SDL3、Fabric 引导接口、Forge LaunchWrapper / ModLauncher 8–11、新版 NeoForge FML 的图形启动接口。Forge 包的类文件最低 Java 8；Fabric / NeoForge 包最低 Java 17，游戏本身的 Java 要求仍须满足。未来加载器、LWJGL 或驱动 ABI 变化仍需测试。

## 0.3.0 修复

- 禁用此 macOS NVK 路径上的 Zink host image copy：动画纹理更新此前反复触发驱动设备打开和内存映射。同一个 1.20.1 存档、相同设置下，修复前稳定阶段约 1–1.5 次呈现/秒，修复后约 110–115 次/秒（上限 120）。没有靠降低视距或添加 Sodium 得到这个对比。
- GL shim 增加 Mach-O 符号解析器，支持在 macOS 上直接查找 OpenGL 符号的 LWJGL 3.2，解决 1.16.5 的 `Core OpenGL functions could not be found`。解析后的地址直接指向 Mesa 函数。
- Forge 包同时提供 Java 8 LaunchWrapper coremod 和 ModLauncher 服务；服务按扫描接口的 ABI 处理 ModLauncher 8 的旧入口，不以 Minecraft 版本号选路。
- LWJGL 2 接入使用 EGL compatibility context，固定管线 OpenGL 也由 Zink/NVK 渲染；原 LWJGL 继续处理窗口和输入，NVK 呈现器接管游戏画面输出。
- 旧 Forge 的 CGL splash、ModLauncher 8 的独立 splash 会自动关闭，原配置备份在实例 `.nvk-bootstrap/backups/`。Modern Forge 的提前窗口仍需安装工具预先关闭。

## 安装方式

现有 Swift Craft Launcher 的 Fabric / Forge / NeoForge 实例可运行：

```sh
python3 install.py
```

其他启动器的实例可显式指定：

```sh
python3 install.py --loader fabric --game-dir "/path/to/instance"
python3 install.py --loader forge --game-dir "/path/to/instance"
python3 install.py --loader neoforge --game-dir "/path/to/instance"
```

安装工具会备份旧 NVK JAR 并复制对应新包。它只读取启动器数据库，不改账号、游戏版本或后端选择。手动安装时，将对应 JAR 放入实例的 `mods/`，移除旧 `nvk-bootstrap-0.1.0.jar`、重复的 NVK 包和 `local-zink-bootstrap-1.0.0.jar`。

### Forge 的提前加载窗口

使用新版提前加载窗口的 Forge 会在发现本模组的 ModLauncher 服务前初始化原始 GLFW / OpenGL。**这类 Forge（包括本次 1.20.1）安装必须先在 `config/fml.toml` 中设置 `earlyWindowControl = false`。** 上述 `install.py --loader forge` 会备份配置并自动设置，其他配置不变。这仅关闭提前加载窗口，游戏本身照常创建窗口。LaunchWrapper 和 ModLauncher 8 的旧 splash 由模组按接口自动处理。

旧版 NeoForge 的 ModLauncher 路径同样需要关闭提前加载窗口，可使用：

```sh
python3 install.py --loader neoforge --legacy-neoforge --game-dir "/path/to/instance"
```

新版 NeoForge 使用 `GraphicsBootstrapper`，能在加载窗口创建前接入；加载画面本身也能走 Zink，无需关闭。已实测 FancyModLoader 12.0.12 / NeoForge 26.3.0.94。

如果某个加载器或其他模组已提前加载原始 GLFW / SDL3，新包会明确报错，避免表面上启动成功、实际仍在用旧图形库。关闭游戏，按上述方式安装，再重新启动。

## 后端与配置

在游戏中选择 OpenGL 或 Vulkan，重启后生效；模组跟随实际创建的图形 API，不替用户选择后端。

- 原生 Vulkan 或 Vulkan 渲染模组创建 swapchain：直接使用 NVK。
- LWJGL 2 / GLFW / SDL3 创建 OpenGL 上下文：使用 Mesa EGL/Zink，再交给 NVK 呈现。
- OpenGL 入口通过本地 `OSMesaGetProcAddress` 对接 Mesa 的函数解析，不在模组类加载器里预载 LWJGL，因此可用于不同加载器。
- SDL3 会将 OpenGL 上下文从探测窗口重新绑定到实际游戏窗口，处理 pbuffer 尺寸变化；Vulkan 窗口保留原始 SDL 行为。

首次启动生成 `config/nvk-bootstrap.properties`：

```properties
enabled=true
audit=false
```

`enabled=false` 在下次启动停用接入；`audit=true` 开启游戏进程的 Vulkan/Metal 计数。0.1.0 的 `mode` 设置不再决定路线。删除 JAR 即可卸载，退出游戏后可删除 `.nvk-bootstrap/` 缓存。

实际选路记录位于 `.nvk-bootstrap/renderer-route.properties`，日志包含 `[NVK-ROUTE] Actual game graphics API: opengl` 或 `vulkan`。NeoForge 的加载窗口可能先记录 OpenGL，进入 Vulkan 游戏后更新为 Vulkan。

Sodium / Iris 可在 OpenGL 路线上共存。Iris 光影仍不支持原生 Vulkan；选择 OpenGL 后可加载 Iris 的常规 OpenGL 路径。本次未安装或验证特定光影包。VulkanMod 与本包不再互斥，但不能与 Sodium 同时安装；用户原实例中已删除的 VulkanMod没有被恢复。

## 0.3.0 本次兼容测试

| 游戏与加载器 | Java | 结果 |
|---|---|---|
| 1.20.1 Fabric 0.20.0 | 17 | 进入世界，严重卡顿修复 |
| 1.20.1 Forge 47.4.28 | 17 | 进入世界，启动故障修复 |
| 1.8.9 Forge 11.15.1.2318 | 8u504 | 进入世界，窗口调整、正常退出通过 |
| 1.12.2 Forge 14.23.5.2864 | 8u504 | 进入世界，窗口调整、正常退出通过 |
| 1.16.5 Forge 36.2.42 | 8u504 | 进入世界，正常退出通过 |
| 1.21.1 Fabric 0.20.0 | 21 | 进入世界 |

六组均确认 OpenGL → Zink → NVK，审计中的 Metal 提交为 0。另回归通过 1.21.11 Fabric + Sodium/Iris、26.3 Fabric 原生 Vulkan、26.3 NeoForge OpenGL。更详细的逐次运行记录保留在开发机本地，不随源码发布。

旧版 Forge 应在启动器里选择 Java 8。JAR 不包含 Java 运行时。

## 0.2.0 历史游戏测试

| 加载器与游戏 | 图形后端 | 结果 |
|---|---|---|
| Fabric 0.20.0 / Minecraft 1.21.11 | OpenGL / GLFW | 进入世界，Zink → NVK |
| Fabric 0.20.0 / Minecraft 1.21.11 + 临时 VulkanMod 0.6.8 | Vulkan / GLFW | 进入世界，直接 NVK |
| Fabric 0.20.0 / Minecraft 26.3 | Vulkan / SDL3 | 进入世界，直接 NVK |
| Fabric 0.20.0 / Minecraft 26.3 | OpenGL / SDL3 | 进入世界，Zink → NVK |
| Fabric 0.20.0 / Minecraft 26.3 + Sodium 0.9.2 / Iris 1.11.7 | OpenGL / SDL3 | 进入世界，Zink → NVK，无光影包 |
| Forge 61.2.4 / Minecraft 1.21.11 | OpenGL / GLFW | 关闭提前加载窗口后进入世界，Zink → NVK |
| NeoForge 26.3.0.94 / FML 12.0.12 / Minecraft 26.3 | Vulkan / SDL3 | 加载画面走 Zink，游戏进入世界后走原生 NVK |
| NeoForge 26.3.0.94 / FML 12.0.12 / Minecraft 26.3 | OpenGL / SDL3 | 加载画面和游戏都走 Zink，进入世界 |

测试命令由现有启动元数据生成，清除外部 NVK 环境变量，不传额外 NVK JVM 参数，使用世界副本。原始世界没有被用于启动测试。临时 VulkanMod 仅复制到隔离测试目录，以验证“同一旧游戏也能根据实际 Vulkan 后端直走 NVK”。

成功测试有实际 Vulkan draw/submit，游戏进程 Metal commit/render/compute/blit 为零。系统加载 Metal.framework 不代表游戏用 Metal 提交 GPU 工作。离线测试的 HTTP 401 / Realms 错误、Iris refmap 警告和原有 `minecraft:end_of_frame` 警告仍存在。测试关闭 VSync，未覆盖所有多窗口或 VSync 行为。

旧 NeoForge ModLauncher 入口已加入包，尚未完成旧版 NeoForge 的整游戏验证。原生库和加载器接口的兼容性以实测为准。

## 源码与构建

```sh
python3 build.py
python3 build.py --skip-native
```

需要 macOS Command Line Tools、JDK 编译器与 JNI 头文件，以及官方加载器编译依赖。默认用本机 Java 25 编译、Java 21 JNI 头文件；Forge Java 产物使用 `--release 8`，Fabric / NeoForge 使用 `--release 17`。`--java-home`、`--jni-home`、`--libraries` 可指定本地路径。构建生成三个加载器包及 `dist/SHA256SUMS`。为保持仓库仅含源码，下载的加载器 JAR、Mesa 动态库和 Java 运行时不纳入 Git；准备本地构建输入时，加载器依赖放入 `vendor/compile/`，Mesa 运行库放入 `vendor/payload/`。

`src/main/java` 的公共引导代码不依赖 LWJGL；Fabric、Forge、NeoForge 只负责调用它的早期入口。native 源码在 `src/main/native`；GLFW、Vulkan/EGL 头文件随项目保存。完整 Mesa 源码和预编译运行库不在本仓库中；Mesa 版本、来源、补丁、构建记录和第三方许可见 `notices/`。构建过程修正 dylib 相对依赖和 RPATH，并作 ad hoc 签名；没有运行时下载。

原生运行时不依赖以前的 Zink / NativePresent 测试目录。Mesa 基于 `17ca6174`，来源、补丁、构建记录和第三方许可在 `notices/`。

参考：[NeoForge GraphicsBootstrapper 接口](https://github.com/neoforged/FancyModLoader/blob/main/loader/src/main/java/net/neoforged/neoforgespi/earlywindow/GraphicsBootstrapper.java)、[Forge 提前加载窗口代码](https://github.com/MinecraftForge/MinecraftForge/blob/1.21.11/fmlloader/src/main/java/net/minecraftforge/fml/loading/ModDirTransformerDiscoverer.java)、[ModLauncher 启动服务接口](https://github.com/McModLauncher/modlauncher/blob/main/src/main/java/cpw/mods/modlauncher/api/ITransformationService.java)。
