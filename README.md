# NVK Bootstrap 0.4.0

按游戏实际使用的图形 API 选择渲染路径：Vulkan 直接使用 NVK；OpenGL 使用 Zink → NVK。macOS WindowServer 仍负责窗口合成。

自有 Cocoa 对象与呈现层使用 Swift，Vulkan/JNI 保留 C 接口；第三方 GLFW 的上游 Cocoa 后端保持原实现。

## 环境需求

- macOS x86_64、受支持的 NVIDIA GPU，以及已安装并运行的 NullMoth NVIDIA 驱动。默认搜索系统/用户 `Library/GPUBundles`；非标准路径可通过 `driverLibrary` 和 `icdFile` 配置。模组不会安装或替换系统驱动；Apple Silicon 未支持。
- 游戏运行时：Forge 包最低 Java 8；Fabric / NeoForge 包最低 Java 17。仍须满足对应 Minecraft 版本自身的 Java 要求。
- 构建环境：macOS Command Line Tools（包含 Swift 编译器）、带 `javac` 的 JDK 和 JNI 头文件。
- 构建还需要官方加载器编译依赖，以及 Mesa EGL/Zink、Gallium 和 SDL3 动态库。源码仓库不包含这些下载或编译产物：加载器依赖放在 `vendor/compile/`，运行库放在 `vendor/payload/`。Mesa 来源、版本和补丁见 `notices/`。

## 兼容说明

模组依据运行时创建的图形 API 选路，不依赖 Minecraft 版本号。支持 Fabric、Forge、NeoForge，以及 LWJGL 2、GLFW 和 SDL3 窗口路径。OpenGL 走 Zink → NVK；Vulkan 走原生 NVK。下表为 0.3.0 的历史兼容记录；0.4.0 的重测范围见测试结果。未列出的游戏和加载器组合不代表已验证。

| Minecraft | 加载器 | Java | 测试结果 |
|---|---|---:|---|
| 1.8.9 | Forge 11.15.1.2318 | 8 | 进入世界、调整窗口、正常退出 |
| 1.12.2 | Forge 14.23.5.2864 | 8 | 进入世界、调整窗口、正常退出 |
| 1.16.5 | Forge 36.2.42 | 8 | 进入世界、正常退出 |
| 1.20.1 | Fabric 0.20.0 | 17 | 进入世界；严重卡顿已修复 |
| 1.20.1 | Forge 47.4.28 | 17 | 进入世界；启动故障已修复 |
| 1.21.1 | Fabric 0.20.0 | 21 | 进入世界 |

新版 Forge（包括已测的 1.20.1）需在 `config/fml.toml` 设置 `earlyWindowControl = false`，以便模组接管窗口初始化。旧版 Forge splash 由模组处理。新 NeoForge 使用 `GraphicsBootstrapper` 接入。

## 构建教程

1. 安装上述构建环境，并准备 `vendor/compile/` 中的 `modlauncher-8.1.3.jar`、`forge-legacy-api.jar`、`fml-loader-10.0.36.jar`，以及 `vendor/payload/` 中的 `libEGL.1.dylib`、`libgallium-26.3.0-devel.dylib`、`libSDL3_original.dylib`。
2. 在项目根目录运行：

   ```sh
   python3 build.py \
     --java-home /path/to/jdk \
     --jni-home /path/to/jdk \
     --libraries /path/to/launcher/libraries
   ```

   `--java-home` 指定 `javac` 所在 JDK；`--jni-home` 指定包含 JNI 头文件的 JDK；`--libraries` 指定含 Fabric Loader 0.20.0 的启动器库目录。路径与脚本默认值一致时可省略参数。

3. 如仅重建加载器 JAR，且已有 `build/payload/` 原生文件，可运行 `python3 build.py --skip-native`。
4. 输出位于 `dist/`：Fabric、Forge、NeoForge 三个 JAR 及 `SHA256SUMS`。

已发布的历史 JAR 与校验和见 [GitHub Release 0.3.0](https://github.com/Chengdong-Yu/Minecraft-NVK-for-macOS/releases/tag/v0.3.0)。

## 测试结果

0.4.0 Swift 版在 macOS 26.5 / RTX 5060 上实测：1280×720、视距 4、模拟距离 8、无光影，VSync 和 AFK 限帧关闭。进入世界后跳过约 20 秒预热，再统计 5 秒窗口的实际呈现速率。

| 游戏 | 渲染路线 | 平均 FPS | 5 秒采样范围 |
|---|---|---:|---|
| 1.21.11 Fabric | OpenGL → Zink → NVK（Sodium/Iris） | 576.0 | 520.5–599.6 |
| 26.3 Fabric | 原生 Vulkan → NVK | 798.5 | 711.1–842.4 |

两组均进入世界，确认加载 Swift 呈现库，Metal commit/render/compute/blit 计数为 0。帧率为固定存档场景的应用呈现速率，不等于显示器刷新率；两组场景不同。0.4.0 的 Forge / NeoForge 包仅完成构建，本轮未重测。

以下为 0.3.0 历史验证结果：

- 上表六组均完成启动并进入世界；OpenGL 测试确认走 Zink → NVK，审计中的 Metal GPU 提交为 0。
- 回归通过：1.21.11 Fabric + Sodium/Iris、26.3 Fabric 原生 Vulkan、26.3 NeoForge OpenGL；Vulkan 测试确认直走 NVK。
- 1.20.1 Fabric 的同存档、同设置对比中，修复前约 1–1.5 FPS，修复后约 110–115 FPS（帧率上限 120）。
- 测试关闭 VSync；未覆盖所有多窗口场景、VSync 行为、光影包或旧版 NeoForge 整游戏运行。Iris 光影未纳入本轮验证。
