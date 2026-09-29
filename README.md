# Origin

Vulkan 渲染骨架，**Windows + Android 双平台**，共用同一份 C++ 与同一份 CMakeLists。

- 平台层零平台代码：`src/` 下没有任何 `android/*`、`windows.h`，也没有 `#ifdef` 平台判断
- Vulkan 走 **C API**（volk + 原生 `vulkan_core.h`），不使用 Vulkan-Hpp
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
│   ├── StbImage.cpp        stb_image 的唯一实现单元
│   └── Log.hpp             VK_CHECK
├── shaders/                HLSL 源（cube.vs.hlsl / cube.ps.hlsl）
├── assets/
│   ├── shaders/            SPIR-V（构建产物，两平台共用）
│   └── textures/           texture.jpg
├── third_party/            git submodule
│   ├── SDL/                release-3.4.16
│   ├── volk/               1.4.350
│   ├── Vulkan-Headers/     v1.4.350
│   ├── SPIRV-Reflect/      vulkan-sdk-1.4.350.1
│   ├── glm/                1.0.3
│   ├── imgui/              v1.92.9b（非 docking）
│   ├── stb/                master（只用 stb_image.h）
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

用 Android Studio 打开 **`Origin/android`**（注意不是仓库根），同步后 Run。

```bash
cd android
gradlew.bat assembleDebug
gradlew.bat installDebug
```

| 组件 | 版本 |
|---|---|
| NDK | `27.3.13750724` |
| CMake | `3.22.1`（SDK Manager 标准组件，Gradle 会自动下载） |
| AGP / Gradle | `8.11.0` / `9.5.1` |
| compileSdk / minSdk | 36 / 24 |

只编 `arm64-v8a`。改用 SDL 源码编译后每个 ABI 都要完整编一遍 SDL3，首次构建较慢。

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
