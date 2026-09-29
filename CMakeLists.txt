# Origin —— 双平台构建真源（Windows / Android 共用本文件）
#
# Windows: cmake --preset windows-debug && cmake --build --preset windows-debug
# Android: cd android && gradlew.bat assembleDebug
#          （Gradle 的 externalNativeBuild 调用的就是本文件，不存在第二份构建脚本）
#
# 三方库全部走 git submodule，位于 third_party/。首次克隆后需要：
#   git submodule update --init --recursive

cmake_minimum_required(VERSION 3.22)
project(Origin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 把产物集中到 bin/，方便桌面端定位 exe
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# =============================================================================
# 三方库（git submodule）
# =============================================================================
set(ORIGIN_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# 每个依赖挑一个必定存在的文件做哨兵（stb 是单头库，没有 CMakeLists.txt）
foreach(_dep
        "SDL/CMakeLists.txt"
        "volk/CMakeLists.txt"
        "Vulkan-Headers/CMakeLists.txt"
        "spdlog/CMakeLists.txt"
        "SPIRV-Reflect/spirv_reflect.c"
        "stb/stb_image.h"
        "glm/glm/glm.hpp"
        "imgui/imgui.cpp")
    if(NOT EXISTS "${ORIGIN_THIRD_PARTY}/${_dep}")
        message(FATAL_ERROR
            "缺少 submodule 文件: third_party/${_dep}\n"
            "请在仓库根目录执行:  git submodule update --init --recursive")
    endif()
endforeach()

# ---- Vulkan-Headers ---------------------------------------------------------
# 不使用 NDK 或 Vulkan SDK 自带的头：那两者的版本会随安装环境漂移，
# 锁在 submodule 上才能保证两个平台看到完全相同的 Vulkan API。
add_subdirectory("${ORIGIN_THIRD_PARTY}/Vulkan-Headers")

# ---- volk -------------------------------------------------------------------
# volk 负责运行时加载 Vulkan 函数指针。
# Android 上这是必需的（不允许直接链接 libvulkan.so），Windows 上顺带统一了
# vulkan-1.dll 的加载方式，于是上层调用代码两平台完全一致。
#
# VULKAN_HEADERS_INSTALL_DIR 让 volk 使用上面锁定版本的头。
# volk 内部会自行拼接 "/include"，所以这里传目录根，不要带 include。
set(VULKAN_HEADERS_INSTALL_DIR "${ORIGIN_THIRD_PARTY}/Vulkan-Headers" CACHE PATH "" FORCE)
#
# 这里刻意不设置 VK_USE_PLATFORM_*_KHR：
# surface 创建交给 SDL，我们从不直接调用 vkCreateAndroidSurfaceKHR /
# vkCreateWin32SurfaceKHR，因此整个构建脚本没有任何平台专有的 Vulkan 宏。
add_subdirectory("${ORIGIN_THIRD_PARTY}/volk")

# ---- spdlog -----------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
add_subdirectory("${ORIGIN_THIRD_PARTY}/spdlog")

# ---- SDL3 -------------------------------------------------------------------
# Android 侧必须是动态库：SDLActivity.getLibraries() 会去 dlopen "SDL3"，
# 即需要 APK 里存在 libSDL3.so。
# 桌面侧用静态库，省掉把 SDL3.dll 拷到 exe 旁边的步骤。
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
if(ANDROID)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
endif()
add_subdirectory("${ORIGIN_THIRD_PARTY}/SDL")

# ---- SPIRV-Reflect ----------------------------------------------------------
# 用途：从编译好的 SPIR-V 反推 descriptor binding / push constant / 工作组尺寸，
# 据此自动生成 VkDescriptorSetLayout、VkPipelineLayout 与 VkDescriptorUpdateTemplate。
# 目的是让 shader 成为资源布局的唯一真源，消掉 C++ 侧的手工镜像声明。
#
# 刻意不用 add_subdirectory —— 它那份 CMakeLists 有五处会污染本工程：
#   1. SPIRV_REFLECT_EXECUTABLE 默认 ON，会连带构建 spirv-reflect /
#      spirv-reflect-pp 两个我们不需要的命令行工具（SDK 里已自带）；
#   2. SPIRV_REFLECT_INSTALL 默认 ON，往我们的 install 规则里塞东西；
#   3. 它把 CMAKE_RUNTIME_OUTPUT_DIRECTORY 指向自己的**源码目录** bin/，
#      会把产物写进 submodule 工作区，导致 git status 变脏；
#   4. 那两个工具带 /W4 /WX 和 -Werror，换新编译器出个新警告就会让我们构建失败；
#   5. 它在目录作用域 set(CMAKE_CXX_STANDARD 14)，会往下泄漏。
# 而 SPIRV_REFLECT_STATIC_LIB 默认是 OFF，我们真正想要的那个 target 反而不会被建，
# 也没有提供 ALIAS target。
#
# 上游 README 给的集成建议本来就是「把 spirv_reflect.h / .c 直接加进你的构建」，
# 所以这里自建一个 target。它自带 include/spirv/unified1/spirv.h，
# 不依赖 Vulkan SDK 也不依赖 Vulkan-Headers（后者不提供 spirv.h），两平台都能直接编。
set(ORIGIN_SPIRV_REFLECT_DIR "${ORIGIN_THIRD_PARTY}/SPIRV-Reflect")
add_library(spirv_reflect STATIC "${ORIGIN_SPIRV_REFLECT_DIR}/spirv_reflect.c")
target_include_directories(spirv_reflect PUBLIC "${ORIGIN_SPIRV_REFLECT_DIR}")
if(MSVC)
    # spirv_reflect.c 内部用了 sprintf 一类函数
    target_compile_definitions(spirv_reflect PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# Debug 下打开它自己的断言：拿到畸形 SPIR-V 时当场停住，而不是返回错误码后继续跑
target_compile_definitions(spirv_reflect PRIVATE
    $<$<CONFIG:Debug>:SPIRV_REFLECT_ENABLE_ASSERTS>)

# ---- stb ---------------------------------------------------------------------
# 单头库，只用到 stb_image.h 做 jpg/png 解码。
# INTERFACE target 只传播 include 目录，不产生编译单元 ——
# STB_IMAGE_IMPLEMENTATION 必须且只能在一个 .cpp 里定义，
# 本工程放在 src/StbImage.cpp（独占一个 TU，避免把它的实现混进业务代码）。
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${ORIGIN_THIRD_PARTY}/stb")

# ---- glm ---------------------------------------------------------------------
# 同样不走 add_subdirectory：glm 是纯头文件库，自建 INTERFACE target 更可控。
#
# 真正的理由是下面那几个 GLM_FORCE_* 宏：它们会改变 glm 类型的**行为与布局**，
# 必须在所有翻译单元里保持一致。挂在 INTERFACE target 上由 CMake 统一传播，
# 就不存在"某个 .cpp 忘了 define"导致同一个 glm::mat4 在不同 TU 里语义不同的问题
# —— 这是 glm 最经典的一类隐蔽 bug。
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${ORIGIN_THIRD_PARTY}/glm")
target_compile_definitions(glm INTERFACE
    # Vulkan 的 NDC 深度是 [0,1]，不是 OpenGL 的 [-1,1]。
    # 不加这个宏，近处物体会被裁掉一半，且深度测试的精度分布也不对。
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    # 所有角度参数一律按弧度解释，禁掉已废弃的角度重载
    GLM_FORCE_RADIANS
)
# ⚠️ 注意不要加 GLM_FORCE_DEFAULT_ALIGNED_GENTYPES=0 试图"关掉"对齐类型：
# glm 的这类宏是用 #ifdef 判断的，**不看值**。写 =0 等于把它打开，
# 会走进 aligned 的 SIMD 特化路径并在 MSVC 上直接编译失败
# （type_vec4.inl: compute_vec_mul<4,T,aligned_highp,true> 没有 call 成员）。
# 不定义就是默认的非对齐行为，正是我们要的。

# ---- Dear ImGui --------------------------------------------------------------
# 用 master 线（非 docking 分支）。imgui 自己不提供 CMakeLists，官方建议就是
# 把那几个 .cpp 直接加进工程，所以这里自建 STATIC target。
#
# 后端选 SDL3 + Vulkan，和本工程的平台层一致。
set(ORIGIN_IMGUI_DIR "${ORIGIN_THIRD_PARTY}/imgui")
add_library(imgui STATIC
    "${ORIGIN_IMGUI_DIR}/imgui.cpp"
    "${ORIGIN_IMGUI_DIR}/imgui_draw.cpp"
    "${ORIGIN_IMGUI_DIR}/imgui_tables.cpp"
    "${ORIGIN_IMGUI_DIR}/imgui_widgets.cpp"
    "${ORIGIN_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${ORIGIN_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(imgui PUBLIC
    "${ORIGIN_IMGUI_DIR}"
    "${ORIGIN_IMGUI_DIR}/backends"
)
# IMGUI_IMPL_VULKAN_USE_VOLK 是关键：
# 默认情况下 imgui 的 Vulkan 后端会直接引用 vkCreateBuffer 等静态原型符号，
# 而 volk 全局定义了 VK_NO_PROTOTYPES —— 两者叠加会在链接期报一堆未定义符号。
# 打开这个宏后 imgui 改为 include volk.h 并走函数指针，和我们的调用路径一致。
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC SDL3::SDL3 volk::volk)

# =============================================================================
# Shader 编译：HLSL -> SPIR-V -> assets/shaders/
# =============================================================================
# 用 DXC 而不是 glslangValidator：后者的 HLSL 前端是 legacy 状态，
# 官方的 HLSL->SPIR-V 路径是 DXC（-spirv 后端）。
#
# 注意与 glslc 的一个重要差别：**Android NDK 不自带 dxc**
# （NDK 的 shader-tools 里只有 glslc/spirv-*），所以两个平台都得用
# 宿主机 Vulkan SDK 里的 dxc。反正 shader 是构建期在宿主机上编的，
# APK 里只有 .spv，设备上不需要任何编译器。
set(_dxc_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
# VULKAN_SDK 环境变量经常没设，所以额外扫一遍默认安装目录并取版本号最大的那个
file(GLOB _vk_sdk_dirs "C:/VulkanSDK/*" "$ENV{HOME}/VulkanSDK/*")
if(_vk_sdk_dirs)
    list(SORT _vk_sdk_dirs)
    list(REVERSE _vk_sdk_dirs)
    foreach(_d IN LISTS _vk_sdk_dirs)
        list(APPEND _dxc_hints "${_d}/Bin" "${_d}/bin")
    endforeach()
endif()

find_program(ORIGIN_DXC dxc
    HINTS ${_dxc_hints}
    NO_CMAKE_FIND_ROOT_PATH   # 交叉编译时别把搜索限制在 NDK sysroot 内
)
if(NOT ORIGIN_DXC)
    message(FATAL_ERROR
        "找不到 dxc。\n"
        "  安装 Vulkan SDK（自带 dxc），或用 -DORIGIN_DXC=<path> 指定。\n"
        "  注意 Android NDK 不提供 dxc，交叉编译时同样依赖宿主机的 Vulkan SDK。")
endif()
message(STATUS "dxc: ${ORIGIN_DXC}")

set(ORIGIN_SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(ORIGIN_SHADER_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
file(MAKE_DIRECTORY "${ORIGIN_SHADER_OUT_DIR}")

# "源文件:profile" —— DXC 不像 glslangValidator 那样能从扩展名推断阶段，
# 必须显式给 -T。约定文件名里带 .vs/.ps 只是给人看的。
set(ORIGIN_SHADERS
    "cube.vs.hlsl:vs_6_0"
    "cube.ps.hlsl:ps_6_0"
)

# -fvk-invert-y 之类的选项刻意不用：Y 翻转放在投影矩阵里（见 src/Math.hpp），
# 这样 shader 侧不依赖任何 DXC 专有开关，将来换编译器也不会静默变形。
set(ORIGIN_DXC_FLAGS -spirv -fspv-target-env=vulkan1.0 -E main -WX)

# 为什么在 configure 阶段先编一次：
# AGP 的 mergeAssets 与 externalNativeBuild 之间没有保证的先后关系，
# 只靠 add_custom_command 在 build 阶段产出 .spv，首次构建可能打出不含 shader 的 APK。
# configure 必然早于任何打包任务，所以这里先编一遍兜底。
set(_spv_files "")
foreach(_entry IN LISTS ORIGIN_SHADERS)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _src)
    list(GET _parts 1 _profile)
    # cube.vs.hlsl -> cube.vs.spv（只剥最后一层扩展名）
    string(REGEX REPLACE "\\.hlsl$" ".spv" _out_name "${_src}")

    set(_src_path "${ORIGIN_SHADER_SRC_DIR}/${_src}")
    set(_out_path "${ORIGIN_SHADER_OUT_DIR}/${_out_name}")

    execute_process(
        COMMAND "${ORIGIN_DXC}" ${ORIGIN_DXC_FLAGS} -T ${_profile} "${_src_path}" -Fo "${_out_path}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _out
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "编译 shader ${_src} (${_profile}) 失败:\n${_out}")
    endif()

    # build 阶段的增量重编：改完 shader 直接 Build 即可生效
    add_custom_command(
        OUTPUT  "${_out_path}"
        COMMAND "${ORIGIN_DXC}" ${ORIGIN_DXC_FLAGS} -T ${_profile} "${_src_path}" -Fo "${_out_path}"
        DEPENDS "${_src_path}"
        COMMENT "dxc ${_src} (${_profile})"
        VERBATIM
    )
    list(APPEND _spv_files "${_out_path}")
endforeach()
add_custom_target(origin_shaders DEPENDS ${_spv_files})

# =============================================================================
# 目标产物
# =============================================================================
set(ORIGIN_SOURCES
    src/main.cpp
    src/VulkanRenderer.cpp
    src/ShaderReflect.cpp
    src/StbImage.cpp
)

if(ANDROID)
    # 目标名必须是 main：SDLActivity.getLibraries() 默认返回 {"SDL3", "main"}，
    # Java 侧会去 dlopen libmain.so。要改名得自建 SDLActivity 子类重写该方法。
    add_library(main SHARED ${ORIGIN_SOURCES})
    set(ORIGIN_TARGET main)
else()
    # 刻意不加 WIN32 关键字：保留控制台子系统，方便直接看到 SDL_Log 输出。
    # 发版时改成 add_executable(Origin WIN32 ...) 可去掉控制台窗口。
    add_executable(Origin ${ORIGIN_SOURCES})
    set(ORIGIN_TARGET Origin)
endif()

add_dependencies(${ORIGIN_TARGET} origin_shaders)

target_include_directories(${ORIGIN_TARGET} PRIVATE src)

target_link_libraries(${ORIGIN_TARGET} PRIVATE
    SDL3::SDL3
    volk::volk
    spdlog::spdlog
    spirv_reflect
    stb
    glm
    imgui
)

# 把三方库的头目录标记为 system include，这样 /W4 与 -Wall -Wextra 不会对它们报警。
# （spdlog 内置的 fmt 会触发一堆 C4459，淹没我们自己代码的警告。）
#
# 用 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES 而不是 add_subdirectory(... SYSTEM)：
# 后者需要 CMake 3.25+，而 Android 侧用的是 SDK 自带的 3.22.1。
foreach(_tp_target spdlog volk SDL3-static SDL3-shared spirv_reflect glm imgui stb)
    if(TARGET ${_tp_target})
        get_target_property(_tp_inc ${_tp_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_tp_inc)
            set_target_properties(${_tp_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_tp_inc}")
        endif()
    endif()
endforeach()

# 资源根目录，作为编译期常量注入 —— 这样 C++ 代码里一个 #ifdef 都不需要。
#   Android: 留空。APK 内的 asset 没有文件系统路径，
#            SDL_LoadFile 收到相对路径时会自动走 AAssetManager。
#   桌面:    指向源码树的 assets/，开发期免去拷贝步骤。
#            发版时改成可执行文件同级目录即可。
if(ANDROID)
    set(ORIGIN_ASSET_ROOT "")
else()
    set(ORIGIN_ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets/")
endif()

target_compile_definitions(${ORIGIN_TARGET} PRIVATE
    ORIGIN_ASSET_ROOT="${ORIGIN_ASSET_ROOT}"
    $<$<CONFIG:Debug>:ORIGIN_ENABLE_VALIDATION=1>
)

if(MSVC)
    target_compile_options(${ORIGIN_TARGET} PRIVATE /W4)
    # MSVC 默认不按标准处理 __cplusplus，spdlog 的部分特性检测依赖它
    target_compile_options(${ORIGIN_TARGET} PRIVATE /Zc:__cplusplus /utf-8)
else()
    target_compile_options(${ORIGIN_TARGET} PRIVATE -Wall -Wextra)
endif()

message(STATUS "Origin 目标: ${ORIGIN_TARGET}  资源根: '${ORIGIN_ASSET_ROOT}'")
