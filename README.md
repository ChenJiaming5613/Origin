# Origin

Vulkan 渲染骨架，**Windows + Android 双平台**，共用同一份 C++ 与同一份 CMakeLists。

- 平台层零平台代码：`src/` 下没有任何 `android/*`、`windows.h`，也没有 `#ifdef` 平台判断
- Vulkan 走 **C API**（volk + 原生 `vulkan_core.h`），不使用 Vulkan-Hpp
- 基线 **Vulkan 1.3**，渲染用 **dynamic rendering**（无 `VkRenderPass` / `VkFramebuffer`）
- 同步用 **Synchronization2**（`vkCmdPipelineBarrier2` / `vkQueueSubmit2`）
- 显存交给 **VMA**，不写裸 `vkAllocateMemory`
- Shader 用 **HLSL**，构建期由 **DXC** 编成 SPIR-V
- descriptor / pipeline layout 由 **SPIRV-Reflect 反射自动生成**，shader 是布局的唯一真源
- 窗口/输入/资源/日志统一交给 SDL3，调试面板用 **Dear ImGui**（非 docking 分支）
- 三方库全部为 **git submodule**

当前内容是一个带贴图与深度测试的旋转 cube，ImGui 面板可实时调节光照方向、强度、颜色与环境光。

---

## 目录结构

```
Origin/
├── CMakeLists.txt          双平台唯一构建真源
├── CMakePresets.json       Windows 构建预设
├── src/                    全部 C++，两平台共享
│   ├── main.cpp            SDL main callbacks + spdlog→SDL_Log 的 sink
│   ├── VulkanRenderer.*    渲染逻辑 + ImGui 生命周期与面板
│   ├── ShaderReflect.*     SPIR-V 反射 -> VkDescriptorSetLayout / VkPipelineLayout
│   ├── Camera.*            轨道相机（纯数学，无输入/无 Vulkan）
│   ├── CameraController.*  arcball 输入：鼠标 + 多点触摸
│   ├── GltfModel.*         glTF 导入（tinygltf v3，纯 CPU 侧）
│   ├── Vertex.hpp          顶点格式（独立出来供导入器复用）
│   ├── StbImage.cpp        stb_image 的唯一实现单元
│   ├── VmaImpl.cpp         VMA 的唯一实现单元（VMA_IMPLEMENTATION）
│   └── Log.hpp             VK_CHECK
├── shaders/                HLSL 源（cube.vs.hlsl / cube.ps.hlsl）
├── assets/
│   ├── shaders/            SPIR-V（构建产物，两平台共用）
│   ├── models/             DamagedHelmet.glb（启动时加载）
│   └── textures/           texture.jpg（模型无贴图时的退路）
├── sample-assets/          大体积参考素材，**不打包、不入库**
├── scripts/
│   ├── build_apk.ps1       一键出 APK（自动定位 JDK/SDK/dxc）
│   └── build_apk.cmd       可双击的包装器
├── third_party/            git submodule
│   ├── SDL/                release-3.4.16
│   ├── volk/               1.4.350
│   ├── Vulkan-Headers/     v1.4.350
│   ├── SPIRV-Reflect/      vulkan-sdk-1.4.350.1
│   ├── glm/                1.0.3
│   ├── imgui/              v1.92.9b（非 docking）
│   ├── stb/                master（只用 stb_image.h）
│   ├── tinygltf/           v3.0.1（纯 C 版）
│   ├── VulkanMemoryAllocator/ v3.4.0
│   └── spdlog/             v1.15.3
└── android/                Android 壳（无 C++ 代码、无 Java 代码）
    ├── settings.gradle, build.gradle, gradle.properties, gradlew*
    └── app/
        ├── build.gradle    指向 ../../CMakeLists.txt
        └── src/main/{AndroidManifest.xml, res/}
```

`android/` 下没有任何源码：CMake 指向顶层，Java 层直接引用 `third_party/SDL` 里的
`org.libsdl.app.*`，assets 指向顶层 `assets/`。

## 首次准备

```bash
git submodule update --init --recursive
```

三方库版本锁在 submodule 的 commit 上。`volk` 与 `Vulkan-Headers` 必须保持同版本号
（都是 `1.4.350`），混用会出符号不一致。`SPIRV-Reflect` 的 tag 也对齐到同一个 SDK
版本（`vulkan-sdk-1.4.350.1`）。

### 关于 SPIRV-Reflect

用于从 SPIR-V 反推 descriptor binding、push constant 布局与 compute 工作组尺寸，
据此自动生成 `VkDescriptorSetLayout` / `VkPipelineLayout` /
`VkDescriptorUpdateTemplate`，让 shader 成为资源布局的唯一真源。

**它没有走 `add_subdirectory`**，顶层 CMakeLists 里自建了一个 `spirv_reflect`
静态库 target，只编 `spirv_reflect.c` 一个文件。原因是上游那份 CMakeLists 会：
默认构建两个我们不需要的命令行工具、默认开 install、把产物写进 submodule 自己的
源码目录（导致 git 变脏）、给工具加 `/W4 /WX` 与 `-Werror`、并在目录作用域设置
`CMAKE_CXX_STANDARD 14`。上游 README 给的集成建议本来就是直接把那两个文件加进构建。

它自带 `include/spirv/unified1/spirv.h`，不依赖 Vulkan SDK，也不依赖
`Vulkan-Headers`（后者并不提供 `spirv.h`），所以两个平台都能直接编。

---

## Windows

需要 Vulkan SDK（提供 `glslc`）与 VS 2022。

```bash
cmake --preset windows
cmake --build --preset windows-debug
build/windows/bin/Debug/Origin.exe
```

`VULKAN_SDK` 环境变量没设也没关系——CMake 会扫 `C:/VulkanSDK/*` 并取版本号最大的那个。
想手动指定就 `-DORIGIN_GLSLC=<path/to/glslc.exe>`（Android NDK 的
`shader-tools/windows-x86_64/glslc.exe` 同样能用）。

桌面端 SDL3 编译为**静态库**，所以不用把 `SDL3.dll` 拷到 exe 旁边。
可执行文件是控制台子系统，日志直接打在终端里。

资源读取用的是源码树 `assets/` 的绝对路径（由 CMake 注入 `ORIGIN_ASSET_ROOT`），
所以从任意工作目录启动都能跑。发版时把这个宏改成 exe 同级目录即可。

## Android

### 一键构建（推荐，不必打开 Android Studio）

```powershell
.\scripts\build_apk.ps1                            # 出 debug APK
.\scripts\build_apk.ps1 -Run -Logcat               # 出包 + 装机 + 启动 + 跟日志
.\scripts\build_apk.ps1 -Config Release -Verify    # release（自动补签）+ 对齐校验
.\scripts\build_apk.ps1 -Clean                     # 先 clean（native 全量重编，较慢）
```

也可以直接双击 `scripts\build_apk.cmd`（等价于不带参数运行）。

脚本会把 Android Studio 在背后做的事显式化：定位 JDK（优先 Android Studio
自带的 JBR）、定位 SDK 并在缺失时生成 `local.properties`、检查 9 个 submodule
是否就位、检查 dxc 是否存在，然后调 `gradlew` 出包。
`JAVA_HOME` / `ANDROID_HOME` / `VULKAN_SDK` **都不需要预先设置**。

常用开关：

| 开关 | 作用 |
|---|---|
| `-Install` | 构建后 `adb install -r` |
| `-Run` | 安装并启动（隐含 `-Install`） |
| `-Logcat` | 启动后跟随日志（隐含 `-Run`） |
| `-Verify` | 校验 zip 层与 ELF 层的 16 KB 对齐 |
| `-LogFile <path>` | 完整输出另存一份（含 gradle 原始输出） |
| `-GradleArgs --info` | 透传参数给 gradlew |

### 用 Android Studio

打开 **`Origin/android`**（注意不是仓库根），同步后 Run。

### 手动 gradle

```bash
cd android
gradlew.bat assembleDebug
gradlew.bat installDebug
```

⚠️ 手动跑 `gradlew` 需要自己设 `JAVA_HOME`，例如：
`$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'`

| 组件 | 版本 |
|---|---|
| NDK | `27.3.13750724` |
| CMake | `3.22.1`（SDK Manager 标准组件，Gradle 会自动下载） |
| AGP / Gradle | `8.11.0` / `9.5.1` |
| compileSdk / minSdk | 36 / **33** |

只编 `arm64-v8a`。改用 SDL 源码编译后每个 ABI 都要完整编一遍 SDL3，首次构建较慢。

⚠️ **APK 产物在 `android/app/build/outputs/apk/<variant>/`**。
不要去看 `build/intermediates/apk/` —— 那里的同名文件是历史构建残留，
AGP 不保证覆盖，会拿到过期的包（曾被一个 1.1 GB 的旧包误导过）。

看日志：

```bash
adb logcat -s SDL
```

（日志经 `SDL_Log` 输出，Android 上的 logcat tag 是 `SDL`。）

正常启动依次出现：

```
SDL_AppInit
SDL 要求的 instance 扩展: VK_KHR_surface
SDL 要求的 instance 扩展: VK_KHR_android_surface
选中 GPU: <设备名> (driver ..., API 1.x.x)
Vulkan 初始化完成: 1080x2400, 3 张 swapchain image
```

### 转屏（preTransform 与 imageExtent 必须成套）

Manifest 里**没有** `android:screenOrientation` —— 写了也不生效。
SDL 在 `SDL_CreateWindow` 时会调 `Android_JNI_SetOrientation()` →
`setRequestedOrientation()`，在运行时覆盖 Manifest：窗口带
`SDL_WINDOW_RESIZABLE` 且未设 `SDL_HINT_ORIENTATIONS` 时，SDL 请求的是
`SCREEN_ORIENTATION_FULL_USER`（允许所有方向，同时尊重系统的旋转锁定开关）。
要真正锁方向得在 C++ 侧设 hint：`SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait")`。

`caps.currentTransform` 是「把 app surface 的内容映射到屏幕物理朝向」所需的旋转。
竖屏 app 跑在竖屏手机上是 `IDENTITY`，转成横屏后变成 `ROTATE_90` / `ROTATE_270`。
**只有两种合法组合，不能混搭：**

| | `preTransform` | `imageExtent` | 额外工作 | 代价 |
|---|---|---|---|---|
| **A（当前）** | `IDENTITY` | `currentExtent` | 无 | 合成器一次全屏旋转 pass |
| **B** | `currentTransform` | 设备 identity 分辨率（90/270 时宽高对调） | MVP 左乘 clip space 旋转 | 无 |

⚠️ **错误组合 `preTransform = currentTransform` + `imageExtent = currentExtent`
会让呈现引擎按旋转后的尺寸去解释一张未旋转的图像 —— 表现是转屏后画面被拉伸。**
这是本工程早期版本的真实 bug，也是网上不少教程的默认写法（因为竖屏跑竖屏时
`currentTransform` 恰好是 `IDENTITY`，问题被掩盖了）。

B 是官方 [pre-rotation 文档](https://developer.android.com/games/optimize/vulkan-prerotation)
推荐的性能最优解（避免 SurfaceFlinger 抢占 GPU）。**当前选 A 不是因为 A 更好，
而是 B 会把 ImGui 一起转歪**：`imgui_impl_vulkan` 在内部自建正交矩阵、
自己调 `vkCmdSetViewport`，拿不到我们的 pre-rotation 矩阵。

B 的补偿代码已经写在 `recordCommandBuffer()` 里（`rotateDeg` / `fbWidth`
那一段），走 A 时 `rotateDeg` 恒为 0、整段跳过，零开销。
将来不需要调试面板时，把 `createSwapchain()` 里选 transform 的那个 `if` 改掉即可。
注意切 B 时**两件事都要做**，漏一个就错：

1. 可见宽高比要用**对调后**的值（否则横屏时 cube 被压扁）；
2. clip space 里补一次 Z 轴旋转（否则画面躺倒）。

### ImGui 的 DPI 自适应

不缩放的话面板在手机上只有指甲盖大小 —— ImGui 默认尺寸是按约 96 DPI 桌面屏定的，
手机 density 通常 2.5x~3.5x。缩放系数取自 **`SDL_GetWindowDisplayScale()`**，
而不是 Android 的 `AConfiguration_getDensity()`：SDL 已经把 Java 侧的
`DisplayMetrics.density` 填进了 `display->content_scale`
（`SDL_androidvideo.c`），所以这段代码 Windows / Android 共用一份，
不需要平台分支，也不用 include `<android/configuration.h>`。

```cpp
float dpiScale = SDL_GetWindowDisplayScale(window_);
if (!(dpiScale > 0.0f)) dpiScale = 1.0f;

ImGuiStyle& style = ImGui::GetStyle();
style.ScaleAllSizes(dpiScale);   // padding / 圆角 / 滚动条宽度…（累积，只能调一次）
style.FontScaleDpi = dpiScale;   // 1.92 起 io.FontGlobalScale 已废弃
style.TouchExtraPadding = ImVec2(4.0f * dpiScale, 4.0f * dpiScale);
```

三点要注意：

- `ScaleAllSizes()` 是**累积**的，对同一个 style 调两次就放大两倍 ——
  只能在 `initImGui()` 里调一次，不能进每帧逻辑。
- 它只管 style 里的几何量，**管不到代码里手写的绝对像素**。
  `SetNextWindowPos/Size` 这类调用要自己乘 `uiDpiScale_`。
- 不需要额外打包 TTF。ImGui 的默认字体 ProggyClean 是以 TTF 形式内嵌的，
  1.92 的动态字体系统会按实际像素大小重新光栅化，放大后是清晰的。
  （参考的 Vulkan-glTF-PBR 用 `screenDensity / ACONFIGURATION_DENSITY_MEDIUM`
  算同一个系数，但它跑在旧 ImGui 上，必须自带 `Roboto-Medium.ttf`。）

面板上另有 `Font scale` 滑条，绑定 `style.FontScaleMain`（叠加在 DPI 系数之上的
用户偏好），可在设备上直接微调，改完立即生效、无需重建字体图集。

### 16 KB 页面对齐

Android 15 起有设备使用 16 KB 内存页。自 **2025-11-01** 起，提交到 Google Play
且 `targetSdk >= 35` 的应用必须支持。本工程 `targetSdk 36`，所以是硬性要求。

这件事有**两层**，容易混为一谈：

| 层 | 检查方式 | 谁负责 |
|---|---|---|
| APK 内 `.so` 的 **zip 条目**对齐 | `zipalign -c -P 16 -v 4 app.apk` | AGP ≥ 8.5.1 自动处理 |
| `.so` 自身的 **ELF LOAD 段**对齐 | `llvm-readelf --program-headers x.so \| grep LOAD` | 链接器标志，需手动加 |

Android Studio 报 *"Some libraries have LOAD segments not aligned at 16 KB boundaries"*
指的是**第二层**。NDK **r28 及以上默认已对齐**，r27 及以下默认 4096，必须显式加：

```cmake
add_link_options(
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)
```

本工程在 `CMakeLists.txt` 顶部的 `if(ANDROID)` 块里加了它。两个细节很关键：

- **用 `add_link_options()` 而不是官方文档示例里的 `target_link_options()`。**
  需要对齐的不只是 `libmain.so`，还有 `third_party/SDL` 经 `add_subdirectory`
  产出的 `libSDL3.so`。`target_link_options` 只作用于单个 target，
  用它会漏掉所有 submodule 里的共享库 —— 而警告恰恰会把它们逐个列出来。
- **必须放在任何 `add_subdirectory()` 之前。** `add_link_options` 是目录作用域，
  只被其后添加的子目录继承；放在 `add_subdirectory(SDL)` 之后就不生效。

自查（`Align` 列应为 `0x4000`）：

```bash
llvm-readelf --program-headers \
  android/app/build/intermediates/merged_native_libs/debug/mergeDebugNativeLibs/out/lib/arm64-v8a/libSDL3.so \
  | grep LOAD
```

> 注：这个标志在 NDK r28+ 上是冗余但无害的，所以升级 NDK 后不需要回头删。

---

## 命名约定

命名空间、宏前缀、产物名统一为 `origin` / `ORIGIN_` / `Origin`：

| 位置 | 名称 |
|---|---|
| C++ 命名空间 | `origin` |
| 编译期宏 | `ORIGIN_ASSET_ROOT`、`ORIGIN_ENABLE_VALIDATION` |
| CMake 工程 | `project(Origin)` |
| Windows 产物 | `Origin.exe` |
| Android 包名 | `com.origin.app` |
| 应用显示名 | `Origin` |

**唯一的例外：Android 的 native 库必须叫 `main`（`libmain.so`）。**
`SDLActivity.getLibraries()` 默认返回 `{"SDL3", "main"}`，Java 侧按这个名字
`dlopen`。要改成 `liborigin.so` 就得自建一个继承 `SDLActivity` 的 Java 类并重写
`getLibraries()`——为一个名字引入一个 Java 文件不值得，所以保留 `main`
并在 CMakeLists 里注明了原因。

## Vulkan 基线是 1.3 —— 由 dynamic rendering 决定

`src/VulkanRenderer.cpp` 里的 `kRequiredApiVersion` 是全工程唯一的版本真源，
instance / 设备过滤 / ImGui 三处都引用它。

### 为什么必须是 1.3，而不是 1.1 或 1.2

`vk.xml` 里 `VK_KHR_dynamic_rendering` 的声明是：

```
depends="((VK_KHR_get_physical_device_properties2,VK_VERSION_1_1)+VK_KHR_depth_stencil_resolve),VK_VERSION_1_2"
promotedto="VK_VERSION_1_3"
```

所以理论上有三条路：核心（1.3）、1.2 + 扩展、1.1 + 一串前置扩展。
**但后两条在 Android 上都不存在：**

1. Android 平台只暴露 **1.0.3 / 1.1 / 1.3** 三档，**没有 1.2**。
   其中 1.3 从 Android 13 / API 33 起可用，1.1 从 API 29，1.0.3 从 API 24。
   而且不允许请求超过当前 Android 版本的上限 —— Android 12 的机器即使
   驱动报 1.3 也拿不到。
2. 没有「扩展兜底」这条退路。实测（跨 94 个设备/OS 组合的设备农场数据）
   不暴露 1.3 的设备**连 `VK_KHR_dynamic_rendering` 扩展都不暴露**。
   支持它们意味着写第二条完整的 renderpass 路径，不是加个 shim。

要注意 **OS 新不等于驱动新**：Galaxy S22 跑 Android 16，
Adreno 730 驱动仍只报 Vulkan 1.1。所以这不是「等老机器淘汰」能解决的。

代价就是 `minSdk` 从 24 抬到 **33**。

### 两道独立的过滤，都需要

| 位置 | 过滤什么 |
|---|---|
| `android/app/build.gradle` 的 `minSdk 33` | **Android 版本**（1.3 从 API 33 起才暴露） |
| Manifest 的 `vulkan.version="0x403000"` | **设备驱动能力**（Android 13 的机器也可能只有 1.1 驱动） |

少了后者，Android 13 上的老 GPU 装得上但一启动就抛异常。
`0x403000` = `VK_MAKE_VERSION(1,3,0)`，与 Android
`PackageManager.FEATURE_VULKAN_HARDWARE_VERSION` 对 1.3 的取值一致。

运行期还有第三道：`pickPhysicalDevice()` 会硬过滤掉 `apiVersion < 1.3`
或 `dynamicRendering == false` 的设备，并按 `deviceType` 打分（独显优先）。
日志里会逐个打出候选与被跳过的原因：

```
Vulkan loader 版本: 1.4.357
跳过 GPU Intel(R) UHD Graphics 630：只支持 API 1.2.148，需要 1.3
选中 GPU: NVIDIA GeForce RTX 3080 (得分 3)
```

## 从 renderpass 迁到 dynamic rendering 的四个坑

都是实际踩过并修掉的。

### 1. layout 转换要自己写

以前 `VkAttachmentDescription` 的 `initialLayout` / `finalLayout` 让驱动在
pass 边界免费插入转换。现在必须手写两组 barrier：

```
帧开头：颜色 UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
        深度 UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL
帧结尾：颜色 COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR
```

`oldLayout` 用 `UNDEFINED` 表示丢弃原内容，与 `loadOp = CLEAR` 一致，
在 tile 架构上也避免把旧内容读回 tile 内存。
帧开头那条 barrier 同时替代了原先的 `VkSubpassDependency(EXTERNAL -> 0)`，
所以 `srcStageMask` 取 `COLOR_ATTACHMENT_OUTPUT` 而非 `TOP_OF_PIPE`
—— 后者校验层不报错，但 swapchain image 被复用时存在写后写竞争。

漏掉结尾那条的症状是 `vkQueuePresentKHR` 报 layout 不是 `PRESENT_SRC_KHR`。

### 2. barrier 的 aspectMask 与 image view 的不一样

深度 `VkImageView` 只标 `VK_IMAGE_ASPECT_DEPTH_BIT` 是合法的（只当深度附件用），
但 `VkImageMemoryBarrier` 在格式同时含深度与 stencil、且 layout 是
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` 时，**必须两位都标**
（VUID-VkImageMemoryBarrier-image-03319）。

两处不一致是正常的，不要「统一」成一个值。见 `depthAspectMask()`。
本机选到 `D32_SFLOAT` 所以撞不上，但部分 Adreno 只给
`D24_UNORM_S8_UINT`，那时就会报。

### 3. ⚠️ ImGui 的管线也必须声明深度格式

这一条最有迷惑性。ImGui 压根不读写深度，直觉上不用管，但：

**管线声明的附件格式要和 `vkCmdBeginRendering` 传入的 `VkRenderingInfo`
逐个附件对齐，与该管线是否真的使用那个附件无关。**

我们的 `VkRenderingInfo` 带了 `pDepthAttachment`，于是在同一次 rendering 里
绘制的每一条管线都得声明同样的深度格式，包括 ImGui 的：

```cpp
info.UseDynamicRendering = true;
info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;   // sType 不填则整个字段被忽略
info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat_;
info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = depthFormat_;  // ← 必填
```

漏了的症状：**画面完全正常**，但校验层每帧刷

```
vkCmdDrawIndexed(): VkRenderingInfo::pDepthAttachment->imageView format
(VK_FORMAT_D32_SFLOAT) must match ... depthAttachmentFormat (VK_FORMAT_UNDEFINED)
```

而 `vkCmdDrawIndexed` 我们自己和 ImGui 都在调，极易误判成自己的管线配错。

另两个 ImGui 侧细节：`pColorAttachmentFormats` 是裸指针且 ImGui **不深拷贝**，
必须指向生命周期长于 ImGui 的对象（这里是成员 `swapchainFormat_`）；
`sType` 必须填对，ImGui 靠它判断这个字段有没有被设置过。

真想「不声明用不到的附件」需要 `VK_EXT_dynamic_rendering_unused_attachments`，
移动端覆盖率差，不值得为此引入。

### 4. `VkRenderingInfo::layerCount` 必须 >= 1

`VkFramebufferCreateInfo` 里这个字段叫 `layers`，改名后很容易漏填，
留 0 会被校验层拦下。

### 迁移后的收益

- 少了 `VkRenderPass` 与 N 个 `VkFramebuffer` 两类对象；
  swapchain 重建时不必再销毁重建 framebuffer
- 截图路径少一次 layout 转换：
  从 `COLOR_ATTACHMENT -> PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC`
  变成 `COLOR_ATTACHMENT -> TRANSFER_SRC -> PRESENT_SRC`
- `dynamicRendering` 特性即便是 1.3 核心，**仍必须在 `vkCreateDevice` 显式启用**。
  用 `VkPhysicalDeviceFeatures2` 挂 pNext 时 `pEnabledFeatures` 必须为 `NULL`
  （VUID-VkDeviceCreateInfo-pNext-00373）。

**深度的 `storeOp = DONT_CARE` 在 `VkRenderingAttachmentInfo` 里照样能表达**，
迁移时务必保留 —— 这是 Mali/Adreno tile 架构上最省带宽的一处声明。

## 显存管理：VMA

`src/` 下没有一处裸 `vkAllocateMemory`。换掉它的理由不是"少写几行"：

1. **`maxMemoryAllocationCount`**。每个资源一次 `vkAllocateMemory` 会很快撞上这个
   上限（移动端常见 4096）。VMA 把小分配合并进大块内存做 suballocation ——
   面板上 `Memory (VMA)` 那一行的 `blocks` 数明显小于 `allocs` 数就是它在起作用。
2. **内存类型的选择不再手写**。原先那段「遍历 `memoryTypes` 找第一个
   `propertyFlags` 匹配的」是教程写法，它忽略堆大小、是否 cached、以及设备可能
   有多个同样满足条件但性能不同的内存类型。

### 与 volk 对接的三个宏（都必须是 PUBLIC）

```cmake
target_compile_definitions(vma PUBLIC
    VMA_VULKAN_VERSION=1003000
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
)
```

| 宏 | 为什么 |
|---|---|
| `VMA_VULKAN_VERSION=1003000` | **必须显式钉住**。VMA 默认按**头文件**能力自动探测，而 `Vulkan-Headers` 是 1.4.350，它会选 `1004000`，比我们请求的 instance 版本高一档 |
| `VMA_STATIC_VULKAN_FUNCTIONS=0` | VMA 直接 `include <vulkan/vulkan.h>`（不是 `volk.h`），`VK_NO_PROTOTYPES` 是否可见取决于 target 传播链。不显式设 0 会在链接期报一堆 `vkAllocateMemory` 未定义 |
| `VMA_DYNAMIC_VULKAN_FUNCTIONS=1` | 保持默认。代价是必须把 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` 填进 `VmaVulkanFunctions`（新版 VMA 有 `VMA_ASSERT` 强制检查），但比手填三十多个函数指针省得多 |

⚠️ **`VMA_VULKAN_VERSION` 必须 PUBLIC，不能 PRIVATE。** 它会改变
`VmaVulkanFunctions` 结构体里**有哪些字段**，即影响 ABI。实现 TU 与调用方 TU
看到的取值不同就是一个安静的 ODR 违规 —— 编译链接都过，运行时踩内存。

另外 `src/VmaImpl.cpp` **只能**出现在 `vma` target 的源列表里，不能同时列进主
target，否则 `VMA_IMPLEMENTATION` 展开两份、链接期一堆重复符号。

### 用法上几处刻意的选择

| 资源 | flags | 理由 |
|---|---|---|
| staging（上传用） | `HOST_ACCESS_SEQUENTIAL_WRITE` + `MAPPED` | 只顺序写一遍不读 → VMA 挑 uncached 内存，写合并快、不占 cache |
| 截图缓冲（下载用） | `HOST_ACCESS_RANDOM` + `MAPPED` | 是给 CPU **读**的。**方向填反是个容易犯的错** —— 用 `SEQUENTIAL_WRITE` 会挑到 uncached 内存，CPU 读它慢到离谱 |
| 深度缓冲 | `DEDICATED_MEMORY` | 尺寸大、生命周期与 swapchain 绑定。独占一块内存，重建时释放整块不留碎片 |
| 顶点/索引/纹理 | 无（纯 `AUTO`） | 让 VMA suballocate 进共享块，这才是它的主场 |

`VMA_MEMORY_USAGE_AUTO` 在双平台上是实质收益：桌面独显有独立显存、staging 必须
走 HOST_VISIBLE 再拷贝；移动端是统一内存，同一块既 DEVICE_LOCAL 又 HOST_VISIBLE。
AUTO 让 VMA 各自选最优，**我们不必写 `#ifdef __ANDROID__`**。

两个容易漏的 API：映射地址要从 `vmaGetAllocationInfo().pMappedData` 取，
**不能再调 `vkMapMemory`**（那会映射整块大内存而不是我们那一段）；
写完要 `vmaFlushAllocation`、读前要 `vmaInvalidateAllocation` ——
AUTO 有可能挑到 non-coherent 的内存类型，漏了就读到旧数据。
这两个函数内部会判断是否真有必要，coherent 时是空操作。

顺带一个免费的收尾检查：`vmaDestroyAllocator` 时若还有未释放的 allocation，
VMA 会在 Debug 下断言并打印泄漏清单 —— 裸 `vkFreeMemory` 时代漏了是什么都不报的。

## 同步：Synchronization2

`sync2` 提升到核心的版本正好是 **1.3**，所以上了 dynamic rendering 之后它是免费的
（只需在 `VkPhysicalDeviceVulkan13Features` 里多开一个 `synchronization2`）。
`src/` 下没有 `vkCmdPipelineBarrier` / `vkQueueSubmit` 的 sync1 调用。

它不改变同步的**语义**，价值在于能表达以前表达不了的精确依赖：

**1. stage 掩码是每个 barrier 独立的。** sync1 只能给整条
`vkCmdPipelineBarrier` 一组 `src/dstStage`，所以帧开头那两个 barrier（颜色 +
深度）被迫共享 `COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS` 的**并集** ——
等于告诉驱动「颜色附件也要等 early-Z 阶段」，是过度同步。sync2 下各自只声明
自己真正涉及的阶段。

**2. `VK_PIPELINE_STAGE_2_NONE` / `VK_ACCESS_2_NONE`。** 「没有在前的访问需要等待」
和「没有后续访问需要等它」以前只能拿 `TOP_OF_PIPE` / `BOTTOM_OF_PIPE` 凑，
语义上是含糊的。

**3. stage 粒度更细。** `VK_PIPELINE_STAGE_2_COPY_BIT` 比 sync1 只有的
`TRANSFER_BIT` 精确 —— 后者是 copy/blit/resolve/clear 四类的并集。

**4. `vkQueueSubmit2` 去掉了平行数组。** sync1 是
`pWaitSemaphores` + 一个靠下标对应的 `pWaitDstStageMask`，长度写错编译器发现不了；
sync2 每个信号量自带 `stageMask`。而且**触发**阶段也能指定了 ——
sync1 的 `pSignalSemaphores` 没有对应字段，语义等价于「整个 command buffer 跑完」。

## 平台差异的收敛方式

平台差异只体现在两处，都在构建系统里，C++ 代码零分支：

| 差异 | 处理方式 |
|---|---|
| 产物类型 | Android `add_library(main SHARED)` / 桌面 `add_executable(Origin)` |
| 资源根 | `ORIGIN_ASSET_ROOT` 宏：Android 空串（走 APK assets），桌面为绝对路径 |

其余全部由 SDL 抹平：

| 事项 | 实现 |
|---|---|
| 入口与主循环 | `SDL_AppInit/Iterate/Event/Quit`（main callbacks） |
| surface 创建 | `SDL_Vulkan_CreateSurface` |
| instance 扩展 | `SDL_Vulkan_GetInstanceExtensions` |
| 读资源 | `SDL_LoadFile`（Android 自动走 AAssetManager） |
| 日志 | 自定义 spdlog sink 转发到 `SDL_Log` |
| 后台不渲染 | `SDL_HINT_ANDROID_BLOCK_ON_PAUSE` 默认开启，SDL 自行阻塞回调 |

## 启用校验层（可选，建议尽早开）

Debug 构建已定义 `ORIGIN_ENABLE_VALIDATION`。

- **Windows**：Vulkan SDK 自带校验层，直接生效
- **Android**：需从 [Vulkan-ValidationLayers releases](https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases)
  下载 `android-binaries-*.zip`，把 `arm64-v8a/libVkLayer_khronos_validation.so`
  放到 `android/app/src/main/jniLibs/arm64-v8a/`

没放也能跑——代码检测到不可用会打 warning 后降级，不会像直接塞 layer 名那样
让 `vkCreateInstance` 返回 `VK_ERROR_LAYER_NOT_PRESENT`。

---

## 代码里刻意做对的几处

**`volkInitializeCustom` 而非 `volkInitialize`**
窗口以 `SDL_WINDOW_VULKAN` 创建后 SDL 已加载好 loader。若用 `volkInitialize()`，
volk 会自己再 `dlopen`/`LoadLibrary` 一次，进程里出现两份 loader 句柄。
这里让 volk 复用 SDL 的 `vkGetInstanceProcAddr`。

**`renderFinished` 按 swapchain image 分配，不是按帧**
经典实现按帧分配，当 image 数与 frames-in-flight 不等时，`vkQueuePresentKHR`
可能等待一个已被后续帧重新提交的信号量。新版校验层会报同步错误，
部分移动 GPU 上会真的卡住。

**`vkResetFences` 放在 `vkAcquireNextImageKHR` 成功之后**
若在 acquire 之前 reset，一旦返回 `VK_ERROR_OUT_OF_DATE_KHR` 提前 return，
该 fence 永不 signal，下一轮 `vkWaitForFences` 直接死锁。

**viewport/scissor 用动态状态**
swapchain 重建时无需重建 pipeline 与 renderPass。移动端 pipeline 创建涉及
shader 编译，是昂贵操作。

**shader 在 CMake configure 阶段先编一次**
AGP 的 `mergeAssets` 与 `externalNativeBuild` 没有保证的先后关系，
只靠 build 阶段的 `add_custom_command` 可能打出不含 shader 的 APK。
configure 必然早于任何打包任务。（build 阶段的增量重编也保留了。）

**不选 `*_SRGB` swapchain 格式**
三角形直接输出线性色，SRGB 格式会让硬件再做一次 gamma 编码，
颜色偏亮容易被误判成渲染错误。

**Android 锁竖屏以规避 pre-rotation**
`preTransform` 与 `currentTransform` 不一致时合成器会做全屏旋转，移动端是实打实的
带宽浪费。解除锁定后需在顶点着色器补偿旋转，`createSwapchain` 已埋 warning 日志。

## 下一步

1. 接 tinygltf：`git submodule add https://github.com/syoyo/tinygltf.git third_party/tinygltf`，
   `readAsset` 已是字节流接口，直接 `LoadBinaryFromMemory`，
   不需要 `TINYGLTF_ANDROID_LOAD_FROM_ASSETS`
2. 升级 Synchronization2 + Dynamic Rendering（两者都有 KHR 扩展形式，
   不必抬高 Vulkan 基线），趁代码规模小时改成本最低
3. 顶点/索引缓冲 + VMA

---

## 相机：Camera + CameraController

两个类刻意分开，各自零依赖外扩：

- **`Camera`**（`src/Camera.{hpp,cpp}`）纯数学，不含输入处理也不含任何 Vulkan 调用。
- **`CameraController`**（`src/CameraController.{hpp,cpp}`）只把 SDL 事件翻译成 Camera 操作。

状态用「目标点 + **朝向四元数** + 距离」而不是 yaw/pitch 欧拉角，理由有两条：
arcball 的自然输出就是一个任意轴的旋转增量，四元数可直接左乘累积，
用欧拉角必须先拆成 yaw/pitch，斜向拖动会失真；另外四元数没有万向锁，
可以越过天顶继续转。

手势映射（两端语义一致）：

| Windows / 鼠标 | Android / 触摸 | 动作 |
|---|---|---|
| 左键拖动 | 单指拖动 | 旋转 |
| 右/中键拖动 | 双指平移 | 平移 |
| 滚轮 | 双指捏合 | 缩放 |

### ⚠️ 触摸会额外合成一份鼠标事件

SDL 默认开启 `SDL_HINT_TOUCH_MOUSE_EVENTS`，触摸除了发 `SDL_EVENT_FINGER_*`
还会**再合成一份鼠标事件**。不滤掉的话，Android 上单指拖动会被
「触摸路径」和「鼠标路径」各处理一次 —— 旋转速度翻倍，而且双指手势
会和合成出的鼠标拖动打架。识别方式是 `which == SDL_TOUCH_MOUSEID`：

```cpp
if (event.motion.which == SDL_TOUCH_MOUSEID) return false;
```

另外三处容易漏的细节：

- `SDL_EVENT_FINGER_CANCELED` 必须和 `FINGER_UP` 一起处理（来电、下拉通知栏会触发）。
  漏了它手指状态会永久停在按下态，回到应用后第一次触摸就跳变。
- 双指变单指时要把剩下那根手指的位置重设为新的旋转基准，否则松开一根手指
  的瞬间会以另一根手指的**旧**位置为起点，产生一次突兀旋转。
- 事件必须先给 ImGui，并在 `io.WantCaptureMouse` 为真时跳过相机 ——
  否则拖面板上的滑条会同时把模型转起来。

鼠标坐标换算用 `SDL_GetWindowSize`（逻辑坐标）而不是 `SDL_GetWindowSizeInPixels`：
鼠标事件给的就是逻辑坐标，高 DPI 下两者不等，混用会让灵敏度偏掉。
触摸事件的 `tfinger.x/y` 本来就是归一化的 0..1，直接线性映射即可。

## glTF 导入

`src/GltfModel.{hpp,cpp}`，启动时加载 `assets/models/DamagedHelmet.glb`
（换模型只改 `VulkanRenderer.cpp` 里的 `kModelAssetPath`）。
加载失败会**退回内置 cube** 并打日志 —— 有退路时链路依然可见，
不会黑屏让人分不清是资源问题还是渲染问题。

### 为什么用 tinygltf v3 而不是大家更熟的 v2.9.x

v3 是纯 C 的重写版（`tiny_gltf_v3.{c,h}`），不再是 header-only 的 C++ 版。
对本工程有两个实打实的好处：

1. **文件 IO 与图像解码都是 opt-in（默认关闭）。**
   v2 默认自带 stb_image 实现，会和 `src/StbImage.cpp` 里的
   `STB_IMAGE_IMPLEMENTATION` 撞成重复符号，得靠一串
   `TINYGLTF_NO_INCLUDE_STB_IMAGE` 之类的宏小心绕开。
   v3 压根不碰 stb —— 我们设 `images_as_is = 1`，它只交出原始 PNG/JPEG 字节，
   解码统一走工程里唯一那份 stb 实现。
2. **v2 要读 APK 内的资源必须替换它的 `FsCallbacks`**；
   v3 只提供 `tg3_parse_glb/auto(内存指针)`，我们用 `SDL_LoadFile`
   读进内存直接喂给它，平台差异自然消失。

顺带也和工程「Vulkan 用 C API、不用 Vulkan-Hpp」的取向一致。

### 导入器里两处容易写错的地方

**顶点属性可能是交错存放的。** 多个属性共享一个 bufferView，各自用
`byteOffset` + `byteStride` 区分，**绝不能**假设数据紧密排列后按
`index * elementSize` 索引。`byteStride == 0` 才表示紧密排列。

**四元数分量顺序不同。** glTF 的 `node.rotation` 是 `[x, y, z, w]`，
而 `glm::quat` 的构造函数是 `(w, x, y, z)`。照下标顺序传会得到一个
完全错误的旋转：

```cpp
const glm::quat r(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]);
```

另外节点层级用**显式栈**展开而不是函数递归：层级深度由文件决定，
异常文件可以做出几万层嵌套把调用栈爆掉。accessor 也自己做了
`offset + stride * count` 的越界检查 —— tinygltf 的 `validate_indices`
只校验索引字段范围，不保证 bufferView 切片落在 buffer 内。

### 当前支持的子集

支持：默认场景的节点层级（展平成每 primitive 一份世界变换）、
POSITION / NORMAL / TEXCOORD_0、三角形拓扑、索引统一转 uint32、
baseColorTexture。

不支持（都会明确打日志，不静默出错画面）：动画、蒙皮、morph target、
Draco 压缩、KTX2/basisu、多材质切换、以及除 baseColor 外的 PBR 贴图。
因为没开 FS 回调，**只支持自包含的 `.glb`** —— 引用外部 `.bin` 或
外部贴图的 `.gltf` 读不到附属文件。

## ⚠️ assets/ 里不要放大体积素材

Android 的 `sourceSets.main.assets.srcDirs` 指向整个 `assets/`，
放进去的东西会被**原样打进 APK**。曾经把 3 GB 的
`glTF-Sample-Assets` 放在 `assets/` 下，结果 APK 体积 **1.1 GB**、
含 2506 个样例资产条目。

所以参考素材放在 **`sample-assets/`**（与 `assets/` 平级，已 gitignore），
只有真正要在设备上加载的文件才进 `assets/`。当前 `assets/` 只有 3.7 MB。

## HLSL + DXC 的三个坑（都已踩过并验证）

### 1. binding 必须用 `[[vk::binding(N)]]`，不要用 `register(tN)`

DXC 只取 register 的**数字**，忽略 `b`/`t`/`s`/`u` 类别字母。
`register(b0)` + `register(t0)` + `register(u0)` 会全部落到 `Binding 0`，
**且不报任何警告**，要到 `vkCreateDescriptorSetLayout` 才炸。

另一个方案 `-fvk-{b,t,s,u}-shift` 会把 binding 号顶到 100/200，
反射侧若用位掩码管理 binding 就会溢出，所以本工程统一用显式标注。

### 2. 矩阵约定要同时看两件事

必须同时确认，只看一件会得出完全相反的结论：

1. DXC 给 push constant 里的 `float4x4` 打 **RowMajor** 装饰；
2. `mul(M, v)` 被编译成 **`OpVectorTimesMatrix`**，不是 `OpMatrixTimesVector`。

两者叠加的净效果是 `result[i] = Σ_k v[k] * mem[k][i]`，
即按**列主序**内存布局做 `M * v` —— 和 GLSL / glm 完全一致。

所以用 `glm::mat4` 可以直接上传，**不需要转置**。
（早期自建的行主序 `Mat4` 需要 `transpose()` 一次；只看 RowMajor 装饰
就下结论的那一版表现为 cube 缩成一小块偏在屏幕角落。）

用 `spirv-dis` 自查：

```bash
spirv-dis assets/shaders/cube.vs.spv | grep -E "RowMajor|ColMajor|TimesMatrix|TimesVector"
```

### 3. Android NDK 不自带 dxc

NDK 的 `shader-tools/` 里只有 `glslc` 和 `spirv-*`。
两个平台都用宿主机 Vulkan SDK 里的 `dxc`——反正 shader 是构建期编的，
APK 里只有 `.spv`，设备上不需要任何编译器。

## 绕序：先查几何数据，别先动 frontFace

如果画面看起来「只显示内壁」，**第一反应不该是去改 `frontFace`**。
更常见的原因是顶点数据里各个面的绕序**本身就不一致** ——
那样无论 `CW` 还是 `CCW` 都只能画出一半的面，两种结果都像是内外翻转，
很容易在两个值之间反复横跳而找不到问题。

本工程就踩过：±Z 两个面是外侧 CCW，±X / ±Y 四个面却是外侧 CW，
于是设 `CCW` 只剩两片薄板、设 `CW` 只剩四片凹角。

排查手段是**对每个三角形验算**，与肉眼判断无关：

```
对每个三角形 (v0, v1, v2):
    geometricNormal = (v1 - v0) × (v2 - v0)
    assert dot(geometricNormal, declaredNormal) > 0     // 必须全为正
```

正确做法是逐面推导，让每个面的 right / up 满足 `cross(right, up) == 法线`：

| 面 | 法线 | right | up |
|---|---|---|---|
| +X | ( 1, 0, 0) | -Z | +Y |
| -X | (-1, 0, 0) | +Z | +Y |
| +Y | ( 0, 1, 0) | +X | -Z |
| -Y | ( 0,-1, 0) | +X | +Z |
| +Z | ( 0, 0, 1) | +X | +Y |
| -Z | ( 0, 0,-1) | -X | +Y |

按「左下 → 右下 → 右上 → 左上」排四个顶点，索引 `0,1,2` 与 `2,3,0`
就天然是外侧 CCW，配 `frontFace = COUNTER_CLOCKWISE` + `cullMode = BACK`。

顺带说明：投影矩阵里的 Y 翻转（`proj[1][1] *= -1`）**不影响** `frontFace` 的取值。
翻转只是把 NDC 的 Y 对齐到 Vulkan framebuffer 的 Y 向下，两者方向一致，
屏幕空间绕序不会再被反一次。

## 截图 / golden image

面板上有 `Screenshot` 按钮，也可以用环境变量在第 40 帧自动存一张：

```bash
ORIGIN_SHOT=out.png ./Origin.exe
```

实现上有一个要点：**copy 命令必须并进当帧的 command buffer**
（`recordCommandBuffer` 里 `vkCmdEndRenderPass` 之后），
走 `PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC`。
如果改成 present 之后另起一个一次性 command buffer 去转换，校验层会报：

```
performs a layout transition on presentable VkImage ...
but the image has not been acquired from VkSwapchainKHR
```

## glm 的一个宏陷阱

`GLM_FORCE_*` 系列宏是用 `#ifdef` 判断的，**不看值**。
写 `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES=0` 不是「关掉」而是「打开」，
会走进 aligned SIMD 特化并在 MSVC 上直接编译失败
（`type_vec4.inl`: `compute_vec_mul<4,T,aligned_highp,true>` 没有 `call` 成员）。

本工程把 `GLM_FORCE_DEPTH_ZERO_TO_ONE` 与 `GLM_FORCE_RADIANS` 挂在
CMake 的 `glm` INTERFACE target 上统一传播，避免某个 `.cpp` 漏定义导致
同一个 `glm::mat4` 在不同翻译单元里语义不同。

注意 glm 只负责深度范围，**Y 翻转要自己做**：

```cpp
glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
proj[1][1] *= -1.0f;   // Vulkan 的 NDC 里 Y 朝下
```

## 反射层做了什么

`src/ShaderReflect.*` 从 SPIR-V 反推，自动生成：

| 产物 | 来源 |
|---|---|
| `VkDescriptorSetLayout` | 各 binding 的号、类型、数量 |
| `VkPipelineLayout` | 上面那个 + push constant 大小与 stage |
| `binding.stageFlags` | 同一 binding 出现在多个阶段时自动 OR |
| 描述符池大小 | 按反射出的类型归并计数 |
| `VkPipelineShaderStageCreateInfo::stage` | `OpEntryPoint` 的 execution model |

`createPipeline()` 里没有一处手写的 binding 号或 push constant 大小。
另外 `checkPushConstantSize<T>()` 会在启动期校验反射出的块大小与 C++
结构体的 `sizeof` 一致，不一致直接抛异常并打出两边字节数。

运行时会把反射结果打到日志，也显示在 ImGui 面板上：

```
[反射] cube.vs + cube.ps -> push constant 96 字节 (VS|PS), 2 个 binding
[反射]   binding 0 = SAMPLED_IMAGE x1 [PS]  cube.ps:baseColor
[反射]   binding 1 = SAMPLER x1 [PS]  cube.ps:baseSampler
```

已知限制：只支持 set 0、每阶段一个从偏移 0 开始的 push constant 块，
越界会明确报错而不是静默忽略。顶点输入布局**不走反射**（反射推不出
你想怎么打包顶点），仍在 `createPipeline()` 里手写。

## ImGui 集成要点

- `IMGUI_IMPL_VULKAN_USE_VOLK` 必须打开。默认情况下 imgui 的 Vulkan 后端
  直接引用 `vkCreateBuffer` 等静态原型，而 volk 全局定义了
  `VK_NO_PROTOTYPES`，两者叠加会在链接期报一堆未定义符号。
- imgui 在 **2025/09/26** 之后把 `RenderPass` / `Subpass` / `MSAASamples`
  从 `InitInfo` 顶层挪进了 `PipelineInfoMain`。照老教程写 `info.RenderPass`
  在 v1.92 上编不过。
- `DescriptorPoolSize > 0` 时后端自建描述符池，不用自己管生命周期。
- `ImGui_ImplSDL3_ProcessEvent()` 必须在 `SDL_AppEvent` 里调用，
  否则面板画得出来但完全点不动。
- 面板文字用英文：ImGui 默认字体不含中文字形，直接写中文会渲染成方框。
  要中文界面需额外加 TTF 资源 + `ImFontGlyphRangesBuilder`，
  且 Android 侧要把字体打进 APK。
