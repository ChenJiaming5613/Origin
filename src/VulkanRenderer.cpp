#include "VulkanRenderer.hpp"

#include "CameraController.hpp"
#include "ImageWrite.hpp"
#include "Log.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_vulkan.h>

#include <stb_image.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstddef>  // offsetof
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>  // std::swap

namespace origin {
namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

// 全工程唯一的 Vulkan 版本基线，instance / device / imgui 三处都引用它。
//
// 为什么是 1.1 而不是 1.3：
//
// 工程一度以 1.3 为基线（为了 dynamic rendering + synchronization2），
// 但真机实测把这个决策否掉了 —— 一加 Ace 竞速版（天玑 8100 / Mali-G610，
// Android 15）只报 Vulkan 1.1.177，GPU 驱动停在 2021 年的 ARM r32p1，
// 而 VK_KHR_dynamic_rendering 的发布日是 2021-11-15，比驱动还晚，
// 所以**连扩展兜底都没有**（实测两个扩展都是 false）。
//
// 关键判断：这两个特性都只是**写法现代化**，不提供任何新的 GPU 能力 ——
// dynamic rendering 省掉 VkRenderPass/VkFramebuffer 的样板，
// synchronization2 把 stage/access 掩码扩到 64 位并让每个 barrier 带独立 stage。
// 而同一台设备的 bindless（descriptor indexing 全绿）与 GPU-driven
// （multiDrawIndirect + drawIndirectCount + shaderDrawParameters）能力完全齐全，
// 那才是本工程真正要研究的东西。用几百行样板代码换一台能跑真机实验的设备，划算。
//
// 为什么不是 1.0：1.1 是 Android 侧实际的下限（CDD 对 64 位设备强制 1.1，
// 平台只暴露 1.0.3 / 1.1 / 1.3 三档），而且 1.1 白送
// vkGetPhysicalDeviceFeatures2 / vkBindBufferMemory2 / shaderDrawParameters
// 这些后续要用的东西，没有理由再往下退。
//
// 注意：**不要**假设 minSdk 能担保 Vulkan 版本。Android CDD 对 1.3 的强制
// 只针对 Android 14 起新上市的机型，存量机器升级大版本系统不受约束 ——
// 上面那台就是 Android 15 + Vulkan 1.1 的活例子。
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_1;

// 深度图在 VkImageMemoryBarrier 里需要的 aspectMask。
//
// 规范要求（VUID-VkImageMemoryBarrier-image-03319）：当格式同时含深度与
// stencil、且 layout 是 DEPTH_STENCIL_ATTACHMENT_OPTIMAL 时，
// aspectMask 必须两位都标上。
//
// 注意这和 VkImageView 的 aspectMask 不同 —— 后者只标 DEPTH_BIT 是合法的
// （我们只把它当深度附件用），两处不一致是正常的，不要"统一"成一个值。
//
// 回到 render pass 之后每帧的深度 barrier 由 render pass 自己处理了，
// 这个函数目前没有调用点，但保留：一旦加后处理、阴影图等需要手动转换深度
// layout 的路径就会立刻用上，而这条 VUID 极易踩。
[[maybe_unused]] constexpr VkImageAspectFlags depthAspectMask(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
}

// 启动时加载的模型。路径相对资源根（桌面是 assets/，Android 是 APK 的 assets/）。
// 换模型只改这一行即可；.gltf 和 .glb 都能喂（tg3_parse_auto 自动识别），
// 但因为没开 tinygltf 的 FS 回调，只支持**自包含**的文件 ——
// 引用外部 .bin / 外部贴图的 .gltf 读不到那些附属文件。
constexpr const char* kModelAssetPath = "models/DamagedHelmet.glb";

// Cube 几何：24 个顶点而不是 8 个。
// 因为每个面需要独立的法线和 UV，共享顶点会让法线在棱边被插值成圆角、
// 且一个顶点无法同时持有三个面的不同 UV。
//
// ⚠️ 绕序必须**逐面推导**，凭感觉排一定会错。
// 每个面统一按「从该面外侧看过去」的 左下 -> 右下 -> 右上 -> 左上 排列，
// 于是索引 0,1,2 与 2,3,0 天然是外侧逆时针（CCW），
// 叉积 (v1-v0) x (v2-v0) 必然等于该面的外向法线。
//
// 关键是各面的「右」「上」方向不能都套 +X/+Y，必须满足 cross(right, up) == 法线：
//
//   面    法线      right   up     左下角坐标
//   +X   ( 1, 0, 0)  -Z     +Y     (+0.5, -0.5, +0.5)
//   -X   (-1, 0, 0)  +Z     +Y     (-0.5, -0.5, -0.5)
//   +Y   ( 0, 1, 0)  +X     -Z     (-0.5, +0.5, +0.5)
//   -Y   ( 0,-1, 0)  +X     +Z     (-0.5, -0.5, -0.5)
//   +Z   ( 0, 0, 1)  +X     +Y     (-0.5, -0.5, +0.5)
//   -Z   ( 0, 0,-1)  -X     +Y     (+0.5, -0.5, -0.5)
//
// 之前这份数据里 ±X / ±Y 四个面的 right 取错了，8 个三角形的绕序与 ±Z 的 4 个相反。
// 症状是**无论 frontFace 设 CW 还是 CCW 都只能画出一半的面**：
// 设 CCW 只剩 ±Z 两片薄板，设 CW 只剩 X/Y 四片呈凹角 —— 两种都像「只显示内壁」，
// 很容易误判成 frontFace 配错了而在那两个值之间反复横跳。
//
// 排查手段：对每个三角形算 (v1-v0) x (v2-v0)，与声明法线点乘必须全为正。
//
// UV 按 Vulkan 约定 (0,0) 在左上：左下(0,1) 右下(1,1) 右上(1,0) 左上(0,0)。
constexpr Vertex kCubeVertices[] = {
    // +X 面：right = -Z, up = +Y
    {{ 0.5f, -0.5f,  0.5f}, { 1,  0,  0}, {0, 1}},
    {{ 0.5f, -0.5f, -0.5f}, { 1,  0,  0}, {1, 1}},
    {{ 0.5f,  0.5f, -0.5f}, { 1,  0,  0}, {1, 0}},
    {{ 0.5f,  0.5f,  0.5f}, { 1,  0,  0}, {0, 0}},
    // -X 面：right = +Z, up = +Y
    {{-0.5f, -0.5f, -0.5f}, {-1,  0,  0}, {0, 1}},
    {{-0.5f, -0.5f,  0.5f}, {-1,  0,  0}, {1, 1}},
    {{-0.5f,  0.5f,  0.5f}, {-1,  0,  0}, {1, 0}},
    {{-0.5f,  0.5f, -0.5f}, {-1,  0,  0}, {0, 0}},
    // +Y 面：right = +X, up = -Z
    {{-0.5f,  0.5f,  0.5f}, { 0,  1,  0}, {0, 1}},
    {{ 0.5f,  0.5f,  0.5f}, { 0,  1,  0}, {1, 1}},
    {{ 0.5f,  0.5f, -0.5f}, { 0,  1,  0}, {1, 0}},
    {{-0.5f,  0.5f, -0.5f}, { 0,  1,  0}, {0, 0}},
    // -Y 面：right = +X, up = +Z
    {{-0.5f, -0.5f, -0.5f}, { 0, -1,  0}, {0, 1}},
    {{ 0.5f, -0.5f, -0.5f}, { 0, -1,  0}, {1, 1}},
    {{ 0.5f, -0.5f,  0.5f}, { 0, -1,  0}, {1, 0}},
    {{-0.5f, -0.5f,  0.5f}, { 0, -1,  0}, {0, 0}},
    // +Z 面：right = +X, up = +Y
    {{-0.5f, -0.5f,  0.5f}, { 0,  0,  1}, {0, 1}},
    {{ 0.5f, -0.5f,  0.5f}, { 0,  0,  1}, {1, 1}},
    {{ 0.5f,  0.5f,  0.5f}, { 0,  0,  1}, {1, 0}},
    {{-0.5f,  0.5f,  0.5f}, { 0,  0,  1}, {0, 0}},
    // -Z 面：right = -X, up = +Y
    {{ 0.5f, -0.5f, -0.5f}, { 0,  0, -1}, {0, 1}},
    {{-0.5f, -0.5f, -0.5f}, { 0,  0, -1}, {1, 1}},
    {{-0.5f,  0.5f, -0.5f}, { 0,  0, -1}, {1, 0}},
    {{ 0.5f,  0.5f, -0.5f}, { 0,  0, -1}, {0, 0}},
};

// 每个面两个三角形。配合上面的顶点排列，两个三角形都是外侧 CCW，
// 对应 pipeline 里的 frontFace = COUNTER_CLOCKWISE + cullMode = BACK。
constexpr uint16_t kCubeIndices[] = {
    0,  1,  2,  2,  3,  0,   // +X
    4,  5,  6,  6,  7,  4,   // -X
    8,  9,  10, 10, 11, 8,   // +Y
    12, 13, 14, 14, 15, 12,  // -Y
    16, 17, 18, 18, 19, 16,  // +Z
    20, 21, 22, 22, 23, 20,  // -Z
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*                                       /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        spdlog::error("[vk] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        spdlog::warn("[vk] {}", data->pMessage);
    } else {
        spdlog::info("[vk] {}", data->pMessage);
    }
    return VK_FALSE;
}

bool instanceLayerAvailable(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> props(count);
    vkEnumerateInstanceLayerProperties(&count, props.data());
    return std::any_of(props.begin(), props.end(), [name](const VkLayerProperties& p) {
        return std::strcmp(p.layerName, name) == 0;
    });
}

bool instanceExtensionAvailable(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    return std::any_of(props.begin(), props.end(), [name](const VkExtensionProperties& p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

// 一台物理设备暴露的全部设备级扩展。
//
// 一次枚举、存成 set 后反复查，而不是每查一个名字就枚举一遍 ——
// 能力报告要查十几个扩展，后者会把 vkEnumerateDeviceExtensionProperties
// 调十几次（Mali 驱动上每次约 200 个条目）。
class DeviceExtensionSet {
public:
    explicit DeviceExtensionSet(VkPhysicalDevice dev) {
        uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr) != VK_SUCCESS) {
            return;
        }
        std::vector<VkExtensionProperties> props(count);
        if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data()) !=
            VK_SUCCESS) {
            return;
        }
        for (const VkExtensionProperties& p : props) {
            names_.emplace(p.extensionName);
        }
    }

    bool has(const char* name) const { return names_.find(name) != names_.end(); }
    size_t count() const { return names_.size(); }

private:
    std::set<std::string> names_;
};

// 打印一台物理设备的关键能力报告。
//
// 对**每个**候选设备无条件调用，不管最后是否被选中。理由：
//   1. 区分「版本号不够」和「能力真的没有」—— dynamicRendering / synchronization2
//      在 1.1、1.2 上都能以 KHR 扩展提供，光看 apiVersion 会把这两种处境混为一谈，
//      而它们的应对成本差一个数量级（启用扩展 vs 重写整套 render pass）。
//   2. bindless 与 GPU-driven 的可行性完全取决于这几个特性位，
//      而且**不随 apiVersion 走** —— 1.1 设备可能有 descriptor indexing，
//      1.3 设备也可能没有 descriptorBuffer。必须逐台实测，不能按版本推断。
//   3. driverVersion 是移动端的关键变量：Mali 的 Vulkan 能力几乎完全由厂商
//      ROM 里的 GPU 驱动决定，同一颗 G610 在 r32 上只有 1.1、r38+ 才有 1.3。
void logDeviceCapabilities(VkPhysicalDevice dev, const VkPhysicalDeviceProperties& props) {
    const DeviceExtensionSet exts(dev);

    spdlog::info("── GPU: {} ─────────────────────────────", props.deviceName);
    spdlog::info("   API {}.{}.{} | driver {}.{}.{} (0x{:08X}) | vendor 0x{:04X} | 类型 {} | {} 个扩展",
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion),
                 VK_API_VERSION_MAJOR(props.driverVersion),
                 VK_API_VERSION_MINOR(props.driverVersion),
                 VK_API_VERSION_PATCH(props.driverVersion), props.driverVersion, props.vendorID,
                 static_cast<int>(props.deviceType), exts.count());

    // vkGetPhysicalDeviceFeatures2 会忽略 pNext 链里它不认识的结构体（规范要求，
    // 不是 UB），所以可以把所有候选结构体一次挂上去、一次查完。
    // 关键点：用**扩展版**结构体而不是 VkPhysicalDeviceVulkan1XFeatures ——
    // 后者在低版本设备上会被整体忽略，字段全留 0，无法区分「不支持」和「没查到」。
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynRender{};
    dynRender.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{};
    sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
    descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufAddr{};
    bufAddr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    VkPhysicalDeviceShaderDrawParametersFeatures drawParams{};
    drawParams.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descBuf{};
    descBuf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;

    dynRender.pNext  = &sync2;
    sync2.pNext      = &descIdx;
    descIdx.pNext    = &bufAddr;
    bufAddr.pNext    = &drawParams;
    drawParams.pNext = &descBuf;

    VkPhysicalDeviceFeatures2 feats{};
    feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats.pNext = &dynRender;
    vkGetPhysicalDeviceFeatures2(dev, &feats);

    // ---- 本工程当前基线 ----
    spdlog::info("   [基线] dynamicRendering={} sync2={}  (扩展: dynamic_rendering={} sync2={})",
                 dynRender.dynamicRendering == VK_TRUE, sync2.synchronization2 == VK_TRUE,
                 exts.has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME),
                 exts.has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));

    // ---- bindless ----
    // descriptor indexing 是 bindless 的地基：没有 runtimeDescriptorArray +
    // nonUniformIndexing，shader 里就不能用「一个巨大的贴图数组 + 运行期算出来的下标」，
    // 也就无法做「一次 draw 画完整个场景、材质靠 instance 数据索引」。
    // 它是 1.2 核心，但在 1.0/1.1 上也能通过 VK_EXT_descriptor_indexing 拿到。
    const bool hasDescIdx = exts.has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) ||
                            props.apiVersion >= VK_API_VERSION_1_2;
    if (hasDescIdx && descIdx.runtimeDescriptorArray == VK_TRUE) {
        spdlog::info("   [bindless] runtimeDescriptorArray={} partiallyBound={} variableCount={}",
                     descIdx.runtimeDescriptorArray == VK_TRUE,
                     descIdx.descriptorBindingPartiallyBound == VK_TRUE,
                     descIdx.descriptorBindingVariableDescriptorCount == VK_TRUE);
        spdlog::info("   [bindless] nonUniformIndexing: sampledImage={} storageBuffer={} storageImage={}",
                     descIdx.shaderSampledImageArrayNonUniformIndexing == VK_TRUE,
                     descIdx.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE,
                     descIdx.shaderStorageImageArrayNonUniformIndexing == VK_TRUE);
        spdlog::info("   [bindless] updateAfterBind: sampledImage={} storageBuffer={} uniformBuffer={}",
                     descIdx.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE,
                     descIdx.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE,
                     descIdx.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE);

        // 上限同样关键：特性报 true 但只允许几百个描述符，就撑不起真实场景的
        // 「全场景贴图一次绑定」。桌面独显普遍是 1048576 起。
        VkPhysicalDeviceDescriptorIndexingProperties idxProps{};
        idxProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idxProps;
        vkGetPhysicalDeviceProperties2(dev, &props2);
        spdlog::info("   [bindless] maxUpdateAfterBind: sampledImages={} storageBuffers={} 总计={}",
                     idxProps.maxPerStageDescriptorUpdateAfterBindSampledImages,
                     idxProps.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                     idxProps.maxUpdateAfterBindDescriptorsInAllPools);
    } else {
        spdlog::warn("   [bindless] **不支持** —— 无 VK_EXT_descriptor_indexing "
                     "(runtimeDescriptorArray={})",
                     descIdx.runtimeDescriptorArray == VK_TRUE);
    }

    // bufferDeviceAddress：把 buffer 当裸指针传进 shader，是 bindless 的另一半
    // （descriptor indexing 管贴图，buffer address 管几何/材质数据）。
    // niagara 的 GPU-driven 路径重度依赖它。1.2 核心，也有 KHR 扩展形式。
    spdlog::info("   [bindless] bufferDeviceAddress={} (扩展={}) | descriptorBuffer={} (扩展={})",
                 bufAddr.bufferDeviceAddress == VK_TRUE,
                 exts.has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
                 descBuf.descriptorBuffer == VK_TRUE,
                 exts.has(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME));

    // ---- GPU-driven ----
    // multiDrawIndirect 决定能不能一条命令发多个 draw（SIGGRAPH 2015 那套的前提）；
    // drawIndirectCount 让 draw 数量本身也来自 GPU buffer（真正的 GPU-driven）；
    // shaderDrawParameters 提供 gl_DrawID，是在一次 multi-draw 里区分 instance 的关键。
    VkPhysicalDeviceFeatures base{};
    vkGetPhysicalDeviceFeatures(dev, &base);
    spdlog::info("   [GPU-driven] multiDrawIndirect={} firstInstance={} maxDrawIndirectCount={}",
                 base.multiDrawIndirect == VK_TRUE, base.drawIndirectFirstInstance == VK_TRUE,
                 props.limits.maxDrawIndirectCount);
    spdlog::info("   [GPU-driven] drawIndirectCount={} shaderDrawParameters={} | 计算: "
                 "maxWorkGroupInvocations={}",
                 exts.has(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) ||
                     props.apiVersion >= VK_API_VERSION_1_2,
                 drawParams.shaderDrawParameters == VK_TRUE,
                 props.limits.maxComputeWorkGroupInvocations);
    spdlog::info("   [GPU-driven] meshShader(EXT)={} | timelineSemaphore={}",
                 exts.has("VK_EXT_mesh_shader"),
                 exts.has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) ||
                     props.apiVersion >= VK_API_VERSION_1_2);
}

}  // namespace

// =============================================================================
// 生命周期
// =============================================================================

void VulkanRenderer::init(SDL_Window* window) {
    window_ = window;

    // 关键：用 volkInitializeCustom 而不是 volkInitialize。
    //
    // 窗口是以 SDL_WINDOW_VULKAN 创建的，SDL 此时已经加载好了 Vulkan loader
    // 并能给出 vkGetInstanceProcAddr。若改用 volkInitialize()，volk 会自己再
    // dlopen 一次 libvulkan.so，于是进程里出现两份 loader 句柄 ——
    // 在装有多套 Vulkan runtime 的环境下会产生难以定位的行为差异。
    // 这里让 volk 复用 SDL 的入口，两者共享同一个 loader。
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (gipa == nullptr) {
        throw std::runtime_error(std::string("取 vkGetInstanceProcAddr 失败: ") +
                                 SDL_GetError());
    }
    volkInitializeCustom(gipa);

    createInstance();
    volkLoadInstance(instance_);

    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createDevice();

    // 加载 device 级函数指针，绕过 loader 的 dispatch 直接指向驱动函数
    volkLoadDevice(device_);

    // 必须在任何资源创建之前：下面所有 buffer/image 都走它分配
    createAllocator();

    // command pool 必须先建：顶点缓冲与纹理都要走 staging 上传，
    // 而上传需要一个临时 command buffer。
    createCommandBuffers();

    createSwapchain();
    createDepthResources();

    // renderPass 依赖 swapchainFormat_ 与 depthFormat_，所以在上面两步之后；
    // framebuffer 又依赖 renderPass_ 与两者的 image view。
    createRenderPass();
    createFramebuffers();

    createCubeMesh();
    createTexture();

    createPipeline();       // 内部调 createProgram()，layout 由反射生成
    createDescriptorSet();  // 需要 program_.setLayout 与纹理都已就位

    createSyncObjects();

    // initImGui 需要 renderPass_（它要建自己的管线）与 swapchain image 数量
    initImGui();

    lastTicks_ = SDL_GetTicks();

    spdlog::info("Vulkan 初始化完成: {}x{}, {} 张 swapchain image, 深度格式 {}",
                 swapchainExtent_.width, swapchainExtent_.height,
                 swapchainImages_.size(), static_cast<int>(depthFormat_));
}

void VulkanRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    shutdownImGui();  // 必须在销毁 device 之前

    for (VkSemaphore s : renderFinishedSemaphores_) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    renderFinishedSemaphores_.clear();

    for (VkSemaphore s : imageAvailableSemaphores_) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    imageAvailableSemaphores_.clear();

    for (VkFence f : inFlightFences_) {
        vkDestroyFence(device_, f, nullptr);
    }
    inFlightFences_.clear();

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
        commandBuffers_.clear();
    }

    destroySwapchainDependents();

    // 描述符池销毁时会连带释放从它分配出去的 set，不需要单独释放 descriptorSet_
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_  = VK_NULL_HANDLE;
    }

    if (textureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textureSampler_, nullptr);
        textureSampler_ = VK_NULL_HANDLE;
    }
    if (textureView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, textureView_, nullptr);
        textureView_ = VK_NULL_HANDLE;
    }
    // vmaDestroyImage / vmaDestroyBuffer 同时销毁句柄与归还那段分配。
    // 注意不能再配对 vkFreeMemory —— VmaAllocation 是某块大内存里的一段，
    // 对它调 vkFreeMemory 会释放整块（含别人的资源）。
    if (textureImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, textureImage_, textureAlloc_);
        textureImage_ = VK_NULL_HANDLE;
        textureAlloc_ = VK_NULL_HANDLE;
    }
    if (indexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, indexBuffer_, indexAlloc_);
        indexBuffer_ = VK_NULL_HANDLE;
        indexAlloc_  = VK_NULL_HANDLE;
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, vertexBuffer_, vertexAlloc_);
        vertexBuffer_ = VK_NULL_HANDLE;
        vertexAlloc_  = VK_NULL_HANDLE;
    }

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    // renderPass 不在 destroySwapchainDependents 里 —— 它只依赖附件格式，
    // 跨 swapchain 重建存活，生命周期和 pipeline 一样长。
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    // program_ 持有 pipelineLayout 与 descriptorSetLayout（都是反射生成的）
    destroyProgram(device_, program_);
    destroyShaderStage(device_, vertexStage_);
    destroyShaderStage(device_, pixelStage_);

    // allocator 必须在 device 之前销毁。
    // 若此时还有未释放的 allocation，VMA 会在 Debug 下断言并打印泄漏清单 ——
    // 这本身就是个有用的收尾检查，比裸 vkFreeMemory 时代什么都不报要好。
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        // 用 SDL 的配对函数销毁，而不是直接 vkDestroySurfaceKHR：
        // surface 是 SDL 创建的，某些后端会附带自己的记账信息。
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    window_         = nullptr;
    physicalDevice_ = VK_NULL_HANDLE;
    currentFrame_   = 0;
    spdlog::info("Vulkan 已清理");
}

// =============================================================================
// Instance / Device
// =============================================================================

void VulkanRenderer::createInstance() {
    // 先问 loader 支不支持 1.1，再去填 appInfo.apiVersion。
    //
    // 这一步不能省：在纯 1.0 loader 上直接把 apiVersion 填 1.1，
    // vkCreateInstance 会返回 VK_ERROR_INCOMPATIBLE_DRIVER —— 那个错误码
    // 读起来像「这台机器没有 Vulkan 驱动」，会把排查方向带偏很远。
    // 显式检查能给出准确的原因。
    //
    // vkEnumerateInstanceVersion 本身是 1.1 引入的，但它是 global 级函数，
    // volk 在 volkLoadLoader 阶段（早于 instance 创建）就已加载；
    // 真正的 1.0 loader 上它会是 nullptr，那种情况按 1.0 处理。
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        VK_CHECK(vkEnumerateInstanceVersion(&loaderVersion));
    }
    spdlog::info("Vulkan loader 版本: {}.{}.{}", VK_API_VERSION_MAJOR(loaderVersion),
                 VK_API_VERSION_MINOR(loaderVersion), VK_API_VERSION_PATCH(loaderVersion));

    if (loaderVersion < kRequiredApiVersion) {
        throw std::runtime_error(
            "本机 Vulkan loader 只支持 " + std::to_string(VK_API_VERSION_MAJOR(loaderVersion)) +
            "." + std::to_string(VK_API_VERSION_MINOR(loaderVersion)) +
            "，本工程需要 1.1。\n"
            "  Windows: 更新显卡驱动，或装新版 Vulkan Runtime\n"
            "  Android: CDD 对 64 位设备强制 1.1，走到这里说明是极老的 32 位设备\n"
            "           或模拟器的软件实现");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Origin";
    appInfo.pEngineName      = "none";
    appInfo.apiVersion       = kRequiredApiVersion;

    // 平台相关的 surface 扩展由 SDL 给出。
    // Android 上会返回 VK_KHR_surface + VK_KHR_android_surface，
    // Windows 上返回 VK_KHR_surface + VK_KHR_win32_surface。
    // 这是「平台层归零」的核心一环：我们不再手写任何平台扩展名。
    uint32_t           sdlExtCount = 0;
    char const* const* sdlExts     = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (sdlExts == nullptr) {
        throw std::runtime_error(std::string("取 Vulkan instance 扩展失败: ") +
                                 SDL_GetError());
    }
    std::vector<const char*> extensions(sdlExts, sdlExts + sdlExtCount);
    for (const char* e : extensions) {
        spdlog::info("SDL 要求的 instance 扩展: {}", e);
    }

    std::vector<const char*> layers;

#if ORIGIN_ENABLE_VALIDATION
    // 校验层在 Android 上需要把 libVkLayer_khronos_validation.so 打进 APK
    // （app/src/main/jniLibs/<abi>/）。没放就静默降级，
    // 否则 vkCreateInstance 会直接返回 VK_ERROR_LAYER_NOT_PRESENT 导致启动失败。
    if (instanceLayerAvailable(kValidationLayerName)) {
        layers.push_back(kValidationLayerName);
        spdlog::info("已启用校验层");
    } else {
        spdlog::warn("校验层不可用，跳过（需将 libVkLayer_khronos_validation.so 放入 jniLibs）");
    }
    if (instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#endif

    VkInstanceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &appInfo;
    info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames     = layers.data();

    VK_CHECK(vkCreateInstance(&info, nullptr, &instance_));
}

void VulkanRenderer::setupDebugMessenger() {
#if ORIGIN_ENABLE_VALIDATION
    if (vkCreateDebugUtilsMessengerEXT == nullptr) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance_, &info, nullptr, &debugMessenger_));
#endif
}

void VulkanRenderer::createSurface() {
    // 整个工程里唯一一次「窗口系统绑定」，且已经是平台无关的。
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface 失败: ") +
                                 SDL_GetError());
    }
}

void VulkanRenderer::pickPhysicalDevice() {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        throw std::runtime_error("没有找到支持 Vulkan 的物理设备");
    }
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &count, devices.data()));

    // 必须打分，不能取第一个。
    //
    // 早期版本的注释写的是「移动端基本只有一个集成 GPU，不需要打分」——
    // 这在桌面双显卡机器上不成立，而且 vkEnumeratePhysicalDevices 的顺序
    // 并不保证稳定。本机就有 RTX 3080（报 1.4）与 Intel UHD 630（只报 1.2），
    // 取第一个会在两者之间随机漂移。
    //
    // 所以这里做两件事：**先按能力硬过滤，再按类型打分**。
    struct Candidate {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        uint32_t         queueFamily = 0;
        int              score = -1;
        std::string      name;
    };
    Candidate best;

    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        // 先无条件打一份能力报告，再做筛选。
        // 顺序很重要：如果放到筛选之后，被跳过的设备就什么都看不到了，
        // 而"被跳过的那台到底差在哪"恰恰是最需要知道的信息。
        logDeviceCapabilities(dev, props);

        // ---- 硬过滤 1：设备自身的 API 版本 ----------------------------------
        // instance 支持某个版本不代表每个物理设备都支持。Intel UHD 630 就是
        // 典型反例：同一台机器上 loader 报 1.4，它自己只报 1.2.148。
        if (props.apiVersion < kRequiredApiVersion) {
            spdlog::info("跳过 GPU {}：只支持 API {}.{}.{}，需要 {}.{}", props.deviceName,
                         VK_API_VERSION_MAJOR(props.apiVersion),
                         VK_API_VERSION_MINOR(props.apiVersion),
                         VK_API_VERSION_PATCH(props.apiVersion),
                         VK_API_VERSION_MAJOR(kRequiredApiVersion),
                         VK_API_VERSION_MINOR(kRequiredApiVersion));
            continue;
        }

        // 基线降到 1.1 后不再有"特性硬过滤"这一环：render pass 路径只用
        // Vulkan 1.0 就有的东西，1.1 设备必然满足。
        // 上面的能力报告仍然逐台打印 —— 将来上 bindless / GPU-driven 时
        // 需要按 descriptorIndexing 等特性位再加过滤，那时在这里补。

        // ---- 硬过滤 2：同时支持图形与呈现的队列族 ---------------------------
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qProps.data());

        uint32_t family      = 0;
        bool     familyFound = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (!(qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            VkBool32 presentSupport = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport));
            if (presentSupport == VK_TRUE) {
                family      = i;
                familyFound = true;
                break;
            }
        }
        if (!familyFound) {
            continue;
        }

        // ---- 打分：独显优先 --------------------------------------------------
        // Android 上只有集成 GPU，自然落到 2 分，不受影响。
        int score = 0;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 1; break;
            default:                                     score = 0; break;  // CPU / OTHER
        }

        spdlog::info("候选 GPU: {} (API {}.{}.{}, 类型 {}, 得分 {})", props.deviceName,
                     VK_API_VERSION_MAJOR(props.apiVersion),
                     VK_API_VERSION_MINOR(props.apiVersion),
                     VK_API_VERSION_PATCH(props.apiVersion),
                     static_cast<int>(props.deviceType), score);

        if (score > best.score) {
            best = Candidate{dev, family, score, props.deviceName};
        }
    }

    if (best.device == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "没有找到满足要求的 GPU：需要 Vulkan 1.1 + 同时支持图形与呈现的队列族。"
            "上面的日志列出了每个设备的完整能力报告与被跳过的原因。");
    }

    physicalDevice_   = best.device;
    queueFamilyIndex_ = best.queueFamily;
    spdlog::info("选中 GPU: {} (得分 {})", best.name, best.score);

    // ---- 探测可选能力 -------------------------------------------------------
    // 对选中的设备再查一次，结果记进 caps_ 供 createDevice / createAllocator 使用。
    //
    // 判定必须"扩展存在"与"特性位为 true"两者都满足：扩展被列出只代表
    // 入口可用，具体特性仍可能报 false（driver 用扩展声明了一部分子特性的情况
    // 真实存在）。
    const DeviceExtensionSet exts(physicalDevice_);

    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
    descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufAddr{};
    bufAddr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    descIdx.pNext = &bufAddr;

    VkPhysicalDeviceFeatures2 feats{};
    feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats.pNext = &descIdx;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &feats);

    // descriptorIndexing 的子特性很多，这里只要求 bindless 最小可用集：
    // 运行期长度的描述符数组 + 非统一索引 + 部分绑定（数组里允许有空洞）。
    caps_.descriptorIndexing = exts.has(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) &&
                               descIdx.runtimeDescriptorArray == VK_TRUE &&
                               descIdx.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
                               descIdx.descriptorBindingPartiallyBound == VK_TRUE;

    caps_.bufferDeviceAddress = exts.has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) &&
                                bufAddr.bufferDeviceAddress == VK_TRUE;

    spdlog::info("可选能力: descriptorIndexing={} bufferDeviceAddress={}",
                 caps_.descriptorIndexing, caps_.bufferDeviceAddress);
}

void VulkanRenderer::createDevice() {
    const float priority = 1.0f;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex_;
    queueInfo.queueCount       = 1;
    queueInfo.pQueuePriorities = &priority;

    // swapchain 是必需的；另两个按 pickPhysicalDevice 探测到的能力条件加入。
    //
    // 不需要显式列出 VK_KHR_get_physical_device_properties2 / maintenance3 /
    // device_group：它们是这两个扩展在 vk.xml 里的前置依赖，但都已在 Vulkan 1.1
    // 核心化，而我们的基线正是 1.1 —— depends 里的 `, VK_VERSION_1_1` 分支即此意。
    // 在 1.0 上就必须逐个启用了。
    std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (caps_.descriptorIndexing) {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    if (caps_.bufferDeviceAddress) {
        deviceExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }

    // 特性要**显式 opt-in**，扩展名只让入口可用、不会自动打开特性。
    //
    // 用扩展版结构体（...FeaturesKHR / 无后缀但 sType 相同的那个），
    // 不能用 VkPhysicalDeviceVulkan12Features —— 后者要求 apiVersion >= 1.2，
    // 在 1.1 的 device 上会被驱动整体忽略，于是"以为开了其实没开"。
    //
    // 规范约束：用 Features2 挂 pNext 时 pEnabledFeatures **必须为 NULL**
    // （VUID-VkDeviceCreateInfo-pNext-00373）。1.0 级特性改填 features2.features。
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceDescriptorIndexingFeatures descIdx{};
    descIdx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures bufAddr{};
    bufAddr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;

    if (caps_.descriptorIndexing) {
        // 只开实际要用的子特性。全开没有额外好处，而且部分驱动对
        // updateAfterBind 会切到较慢的描述符路径。
        descIdx.runtimeDescriptorArray                    = VK_TRUE;
        descIdx.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        descIdx.descriptorBindingPartiallyBound           = VK_TRUE;
        descIdx.pNext                                     = features2.pNext;
        features2.pNext                                   = &descIdx;
    }
    if (caps_.bufferDeviceAddress) {
        bufAddr.bufferDeviceAddress = VK_TRUE;
        bufAddr.pNext               = features2.pNext;
        features2.pNext             = &bufAddr;
    }

    VkDeviceCreateInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext                = &features2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos    = &queueInfo;
    info.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());
    info.ppEnabledExtensionNames = deviceExtensions.data();
    info.pEnabledFeatures        = nullptr;

    VK_CHECK(vkCreateDevice(physicalDevice_, &info, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);

    for (const char* e : deviceExtensions) {
        spdlog::info("已启用 device 扩展: {}", e);
    }
}

void VulkanRenderer::createAllocator() {
    // VMA 需要一组 Vulkan 函数指针。因为工程用 volk（VK_NO_PROTOTYPES），
    // 不能让它去引用静态原型，所以走 VMA_DYNAMIC_VULKAN_FUNCTIONS=1：
    // 只把两个 getter 交给它，剩下三十多个由它自己 fetch。
    //
    // 新版 VMA 里这两个字段有 VMA_ASSERT 强制检查，漏填会在 Debug 下直接断言，
    // Release 下则是空指针调用 —— 不是"可选优化"。
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.instance         = instance_;
    info.physicalDevice   = physicalDevice_;
    info.device           = device_;
    info.pVulkanFunctions = &fns;
    // 必须和实际创建 instance 时用的版本一致。填高了 VMA 会尝试调用
    // 设备并未暴露的入口；填低了则用不上 1.1 起的 vkBindBufferMemory2 /
    // vkGetBufferMemoryRequirements2（dedicated allocation 依赖它们）。
    info.vulkanApiVersion = kRequiredApiVersion;

    // 启用 buffer device address 时**必须**同时告诉 VMA，否则它分配内存时不会
    // 挂 VkMemoryAllocateFlagsInfo{VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT}，
    // 而带 SHADER_DEVICE_ADDRESS usage 的 buffer 绑定到这种内存上会被校验层
    // 拦下（VUID-vkBindBufferMemory-bufferDeviceAddress-03339）。
    //
    // 这里有个容易误判的点：VMA 的 VMA_BUFFER_DEVICE_ADDRESS 宏条件是
    // `VK_KHR_buffer_device_address || VMA_VULKAN_VERSION >= 1002000`，
    // 前者是 Vulkan 头文件里的扩展宏（恒为 1）—— 所以即便我们把
    // VMA_VULKAN_VERSION 钉在 1001000，这条功能**仍然是编进来的**，
    // 不需要额外 define。VMA 自己也不调 vkGetBufferDeviceAddress，
    // 取地址是应用的事，它只负责分配时加那个 flag。
    if (caps_.bufferDeviceAddress) {
        info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    VK_CHECK(vmaCreateAllocator(&info, &allocator_));

    // 打一下堆信息：移动端往往只有一个 DEVICE_LOCAL | HOST_VISIBLE 的堆
    // （统一内存），桌面独显则是分离的。做内存相关的性能判断时这是前提。
    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(allocator_, &memProps);
    spdlog::info("VMA 已就绪: {} 个 memory type, {} 个 heap", memProps->memoryTypeCount,
                 memProps->memoryHeapCount);
    for (uint32_t i = 0; i < memProps->memoryHeapCount; ++i) {
        const bool deviceLocal =
            (memProps->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        spdlog::info("  heap {}: {} MB{}", i,
                     memProps->memoryHeaps[i].size / (1024 * 1024),
                     deviceLocal ? "  [DEVICE_LOCAL]" : "");
    }
}

// =============================================================================
// Swapchain
// =============================================================================

void VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps));

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                  nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                                  formats.data()));

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        // 移动端常见 R8G8B8A8_UNORM，桌面端常见 B8G8R8A8_UNORM，两者都接受。
        // 刻意不选 *_SRGB：三角形直接输出线性色，选 SRGB 格式会让硬件再做一次
        // gamma 编码，颜色偏亮，容易被误判成渲染错误。
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    swapchainFormat_ = chosen.format;

    swapchainExtent_ = caps.currentExtent;
    if (swapchainExtent_.width == std::numeric_limits<uint32_t>::max()) {
        // 部分后端（如 Wayland）会返回 0xFFFFFFFF 表示「由应用决定」，
        // 此时用 SDL 报告的像素尺寸。注意要用 InPixels 版本而非逻辑尺寸，
        // 高 DPI 下两者不相等。
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        swapchainExtent_.width  = static_cast<uint32_t>(w);
        swapchainExtent_.height = static_cast<uint32_t>(h);
    }
    swapchainExtent_.width =
        std::clamp(swapchainExtent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    swapchainExtent_.height = std::clamp(swapchainExtent_.height, caps.minImageExtent.height,
                                         caps.maxImageExtent.height);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // ---- Android 转屏：preTransform 与 imageExtent 必须成套选择 --------------
    //
    // caps.currentTransform 表示「把 app surface 的内容映射到屏幕物理朝向」
    // 所需的旋转。竖屏 app 跑在竖屏手机上是 IDENTITY，转成横屏后变为
    // ROTATE_90 / ROTATE_270。
    //
    // 只有两种合法组合，**不能混搭**：
    //
    //   A. preTransform = IDENTITY          + imageExtent = currentExtent
    //      按 surface 的当前朝向渲染，旋转交给 Android 合成器。
    //      代价：一次全屏合成 pass（SurfaceFlinger 要读整个 framebuffer）。
    //      好处：所有内容（含 ImGui）自动跟随屏幕方向，零额外代码。
    //
    //   B. preTransform = currentTransform  + imageExtent = 设备 identity 分辨率
    //      即 currentTransform 含 90/270 时把 currentExtent 的宽高**对调**，
    //      再在 MVP 左乘一个 clip space 旋转自行补偿。
    //      这是移动端性能最优解（官方 pre-rotation 文档推荐）。
    //
    // 之前的实现混搭成了 preTransform = currentTransform + imageExtent =
    // currentExtent —— 呈现引擎会按「已旋转」的尺寸去解释一张未旋转的图像，
    // 表现就是转屏后画面被拉伸。这就是那个 bug 的根因。
    //
    // 这里默认走 A。选它不是因为 A 更优，而是 B 会把 ImGui 一起转歪：
    // imgui_impl_vulkan 在内部自建正交矩阵、并自己调 vkCmdSetViewport，
    // 拿不到我们的 pre-rotation 矩阵。等调试面板不再需要时可切到 B ——
    // 下面 recordCommandBuffer 里的补偿代码已经写好，改这里一行即可。
    if ((caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        swapchainTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        // Android 实现理论上必然支持 IDENTITY，走到这里说明遇到了异常驱动。
        // 退回方案 B：宽高对调 + MVP 补偿（3D 内容正确，ImGui 会转歪）。
        swapchainTransform_ = caps.currentTransform;
        if ((swapchainTransform_ & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                                    VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != 0) {
            std::swap(swapchainExtent_.width, swapchainExtent_.height);
        }
        spdlog::warn("surface 不支持 IDENTITY transform，回退 pre-rotation 路径"
                     "（currentTransform = 0x{:x}），ImGui 面板会随之转向",
                     static_cast<uint32_t>(swapchainTransform_));
    }

    VkSwapchainKHR oldSwapchain = swapchain_;

    VkSwapchainCreateInfoKHR info{};
    info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface          = surface_;
    info.minImageCount    = imageCount;
    info.imageFormat      = chosen.format;
    info.imageColorSpace  = chosen.colorSpace;
    info.imageExtent      = swapchainExtent_;
    info.imageArrayLayers = 1;
    info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform     = swapchainTransform_;
    info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO 是规范保证必然支持的呈现模式，移动端最省电（等垂直同步，不丢帧）。
    // MAILBOX 能降延迟但会持续满载 GPU，手机上不合适。
    info.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped          = VK_TRUE;
    info.oldSwapchain     = oldSwapchain;

    VK_CHECK(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_));

    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
    }

    uint32_t actualCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, nullptr));
    swapchainImages_.resize(actualCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device_, swapchain_, &actualCount, swapchainImages_.data()));

    swapchainImageViews_.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = swapchainImages_[i];
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[i]));
    }
}

void VulkanRenderer::createRenderPass() {
    // 只依赖附件的**格式**，不依赖尺寸 —— 所以和 pipeline 一样只建一次，
    // swapchain 重建时不用动（重建的只有 framebuffer）。
    //
    // 必须在 createDepthResources 之后调用：depthFormat_ 在那里才确定。

    VkAttachmentDescription attachments[2]{};

    // ---- 颜色附件 ----
    attachments[0].format  = swapchainFormat_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // initialLayout=UNDEFINED 表示「不保留原有内容」。配合 loadOp=CLEAR 语义正确，
    // 而且在 tile 架构上明确告诉驱动不必把旧内容读进 tile 内存。
    // 写成 PRESENT_SRC_KHR（想当然地"上一帧结束时它就是这个 layout"）是常见错误：
    // 那会让驱动以为需要保留内容，白白多一次 tile load。
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // finalLayout 直接给 PRESENT_SRC_KHR —— 这就是 render pass 相对
    // dynamic rendering 省事的地方：转换由 render pass 在 pass 结束时自动插入，
    // 不需要像之前那样在 vkCmdEndRendering 后手写一条 barrier。
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // ---- 深度附件 ----
    attachments[1].format  = depthFormat_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // storeOp=DONT_CARE 是移动端最省带宽的一处声明，务必保留：
    // 深度值出了本 pass 就没人再读，Mali/Adreno 的 tile 架构据此完全跳过
    // 「把 tile 内的深度写回主存」这一步。
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // ---- EXTERNAL -> 0 的依赖 ----
    //
    // 作用等同于之前 dynamic rendering 路径里手写的那两条 barrier：把本帧的
    // 附件写入排在「上一帧对同一张 swapchain image 的写入」之后。
    // srcStageMask 取 COLOR_ATTACHMENT_OUTPUT 而不是 TOP_OF_PIPE —— 后者
    // 校验层不报错，但 swapchain image 被复用时存在写后写竞争。
    //
    // 这里能直观看到 synchronization2 被拿掉的代价：sync1 的一条 dependency
    // 只有**一组** src/dstStageMask，颜色与深度被迫共享
    // `COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS` 的并集，
    // 等于告诉驱动「颜色附件也要等 early-Z 阶段」——属于过度同步。
    // sync2 的 VkImageMemoryBarrier2 可以让两个 barrier 各自只声明自己的阶段。
    // 实际影响很小（一帧一次、且驱动多半会自己优化掉），但这是真实的精度损失，
    // 不是等价替换。
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments    = attachments;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dependency;

    VK_CHECK(vkCreateRenderPass(device_, &info, nullptr, &renderPass_));
}

void VulkanRenderer::createFramebuffers() {
    // 每张 swapchain image 一个 framebuffer，深度图是所有 framebuffer 共用的
    // （只有一帧在飞时深度可以复用；将来若做多帧并行写深度，这里要改成每帧一份）。
    framebuffers_.resize(swapchainImageViews_.size());

    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        // 顺序必须与 createRenderPass 里 pAttachments 的下标一一对应
        const VkImageView views[2] = {swapchainImageViews_[i], depthView_};

        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments    = views;
        info.width           = swapchainExtent_.width;
        info.height          = swapchainExtent_.height;
        // layers 必须 >= 1（非数组渲染就是 1）。留 0 会被校验层拦下。
        info.layers = 1;

        VK_CHECK(vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]));
    }
}

void VulkanRenderer::destroyFramebuffers() {
    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();
}

void VulkanRenderer::destroyDepthResources() {
    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, depthImage_, depthAlloc_);
        depthImage_ = VK_NULL_HANDLE;
        depthAlloc_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroySwapchainDependents() {
    // 截图缓冲的大小跟 swapchain 尺寸绑定，尺寸变了必须重建
    if (screenshotBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, screenshotBuffer_, screenshotAlloc_);
        screenshotBuffer_ = VK_NULL_HANDLE;
        screenshotAlloc_  = VK_NULL_HANDLE;
        screenshotSize_   = 0;
    }

    // framebuffer 绑定了具体的 image view 与尺寸，必须先于 image view 销毁
    destroyFramebuffers();

    destroyDepthResources();

    for (VkImageView v : swapchainImageViews_) {
        vkDestroyImageView(device_, v, nullptr);
    }
    swapchainImageViews_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::recreateSwapchain() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) {
        return;  // 尺寸为 0 时不能建 swapchain，跳过本次
    }

    vkDeviceWaitIdle(device_);

    // 注意这里既没有销毁 pipeline 也没有销毁 renderPass：
    // viewport/scissor 是动态状态，而 renderPass 只记录**附件格式**
    // （颜色格式与深度格式跨重建都没变），pipeline 又只引用 renderPass 的兼容性，
    // 所以两者都能直接复用 —— 这是避免转屏卡顿的关键
    // （移动端 pipeline 创建要编译 shader，是昂贵操作）。
    //
    // 必须重建的只有 framebuffer：它绑定了具体的 image view 和尺寸。
    // 这就是 dynamic rendering 想省掉的那部分样板 —— 回退的实际代价之一。
    destroyFramebuffers();

    for (VkImageView v : swapchainImageViews_) {
        vkDestroyImageView(device_, v, nullptr);
    }
    swapchainImageViews_.clear();

    if (screenshotBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, screenshotBuffer_, screenshotAlloc_);
        screenshotBuffer_  = VK_NULL_HANDLE;
        screenshotAlloc_   = VK_NULL_HANDLE;
        screenshotSize_    = 0;
        screenshotPending_ = false;  // 尺寸变了，这次截图作废
    }

    // 深度缓冲的尺寸跟 swapchain 绑定，必须一起重建
    destroyDepthResources();

    createSwapchain();
    createDepthResources();
    // renderPass_ 复用（格式没变），只重建 framebuffer
    createFramebuffers();

    // swapchain image 数量可能变化，按 image 分配的信号量必须跟着重建
    for (VkSemaphore s : renderFinishedSemaphores_) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    renderFinishedSemaphores_.resize(swapchainImages_.size());
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& s : renderFinishedSemaphores_) {
        VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &s));
    }

    // swapchain image 数量变了要告知 imgui，否则它内部的帧资源环会和实际不符
    if (imguiReady_) {
        ImGui_ImplVulkan_SetMinImageCount(
            static_cast<uint32_t>(swapchainImages_.size()));
    }

    spdlog::info("swapchain 重建: {}x{}", swapchainExtent_.width, swapchainExtent_.height);
}

// =============================================================================
// Pipeline
// =============================================================================

void VulkanRenderer::createPipeline() {
    // ---- 反射驱动的 layout 生成 --------------------------------------------
    // createShaderStage 内部用 SPIRV-Reflect 解析 SPIR-V，
    // createProgram 据此生成 VkDescriptorSetLayout 与 VkPipelineLayout。
    // 本函数里没有一处手写的 binding 号或 push constant 大小。
    vertexStage_ = createShaderStage(device_, readAsset("shaders/cube.vs.spv"), "cube.vs");
    pixelStage_  = createShaderStage(device_, readAsset("shaders/cube.ps.spv"), "cube.ps");

    program_ = createProgram(device_, {&vertexStage_, &pixelStage_});
    logProgram(program_, {&vertexStage_, &pixelStage_});

    // 反射出的 push constant 大小必须和 C++ 结构体对得上。
    // 对不上就在这里抛异常 —— 而不是等渲染出鬼再去猜是哪儿错了。
    checkPushConstantSize<CubePushConstants>(program_, "cube.vs");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = vertexStage_.stage;  // 阶段也是反射出来的
    stages[0].module = vertexStage_.handle;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = pixelStage_.stage;
    stages[1].module = pixelStage_.handle;
    stages[1].pName  = "main";

    // ---- 顶点输入（手写，刻意不反射）---------------------------------------
    // location 必须与 cube.vs.hlsl 里 VSInput 的语义顺序一致：
    // POSITION -> 0, NORMAL -> 1, TEXCOORD0 -> 2。
    // DXC 按 VSInput 结构体成员的声明顺序依次分配 location。
    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding   = 0;
    vertexBinding.stride    = sizeof(Vertex);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertexAttrs[3]{};
    vertexAttrs[0].location = 0;
    vertexAttrs[0].binding  = 0;
    vertexAttrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    vertexAttrs[0].offset   = offsetof(Vertex, position);
    vertexAttrs[1].location = 1;
    vertexAttrs[1].binding  = 0;
    vertexAttrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    vertexAttrs[1].offset   = offsetof(Vertex, normal);
    vertexAttrs[2].location = 2;
    vertexAttrs[2].binding  = 0;
    vertexAttrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
    vertexAttrs[2].offset   = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions    = vertexAttrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // viewport/scissor 用动态状态：swapchain 重建后无需重建 pipeline。
    // 移动端 pipeline 创建涉及 shader 编译，是昂贵操作，能省必须省。
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth   = 1.0f;
    raster.cullMode    = VK_CULL_MODE_BACK_BIT;
    // 顶点数据按「从面外侧看逆时针」排列（kCubeVertices 处有逐面推导），
    // 所以这里就是标准的 COUNTER_CLOCKWISE，没有任何反直觉之处。
    //
    // 投影矩阵里的 Y 翻转（proj[1][1] *= -1）**不影响**这一项：
    // 翻转只是把 NDC 的 Y 方向对齐到 Vulkan framebuffer 的 Y 向下，
    // 两者方向一致，屏幕空间绕序不会再被反一次。
    //
    // 排查提示：如果画面看起来「只显示内壁」，先别动这一项 ——
    // 更常见的原因是顶点数据里各个面的绕序**本身不一致**
    // （那样无论 CW 还是 CCW 都只能画出一半的面）。
    // 先对每个三角形验算 (v1-v0) x (v2-v0) 是否等于其外向法线。
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAttachment;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamics;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    // 投影矩阵产出的是常规 [0,1] 深度（近处 0），所以「更近」就是「更小」。
    // 若日后改用 reverse-Z（近处 1，精度分布更好），这里要一并换成 GREATER。
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;
    depthStencil.minDepthBounds   = 0.0f;
    depthStencil.maxDepthBounds   = 1.0f;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState      = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &msaa;
    info.pDepthStencilState  = &depthStencil;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamicState;
    info.layout              = program_.layout;  // 反射生成
    // ---- render pass 绑定 ---------------------------------------------------
    // pipeline 只和 renderPass 的「兼容性」挂钩，不和具体的 framebuffer 挂钩
    // （兼容 = 附件数量、格式、采样数一致）。所以 swapchain 重建后 pipeline
    // 仍然可用 —— 这一点和 dynamic rendering 路径的效果相同。
    info.renderPass = renderPass_;
    info.subpass    = 0;

    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_));

    // shader module 不在这里销毁：它们归 vertexStage_ / pixelStage_ 所有，
    // 在 shutdown() 里统一由 destroyShaderStage 处理。
}

// =============================================================================
// 命令与同步
// =============================================================================

void VulkanRenderer::createCommandBuffers() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndex_;
    VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));

    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = commandPool_;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()));
}

void VulkanRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // 初始就置为 signaled，否则第一帧的 vkWaitForFences 会永久阻塞
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &imageAvailableSemaphores_[i]));
        VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]));
    }

    // 见头文件注释：按 swapchain image 数量分配，不是按帧
    renderFinishedSemaphores_.resize(swapchainImages_.size());
    for (auto& s : renderFinishedSemaphores_) {
        VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &s));
    }
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    // ---- 开始 render pass ---------------------------------------------------
    //
    // 这里不需要任何手写的 layout barrier：附件的 initialLayout/finalLayout
    // 已经写在 createRenderPass 的 VkAttachmentDescription 里，驱动会在 pass
    // 边界自动插入转换，EXTERNAL->0 的 subpass dependency 负责跨帧排序。
    // 这正是 render pass 相对 dynamic rendering 唯一的**便利**之处
    // （代价是多了 renderPass + framebuffer 两类对象要维护）。
    //
    // clearValues 的下标必须与 pAttachments 对应：0 颜色、1 深度。
    // 这是回到 render pass 后新增的一处隐式耦合 —— dynamic rendering 下
    // clearValue 是直接挂在各自的 VkRenderingAttachmentInfo 上的，不会错位。
    VkClearValue clearValues[2]{};
    // 深蓝灰，便于区分「没画」与「画黑了」
    clearValues[0].color        = {{0.05f, 0.06f, 0.09f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};  // 常规深度：远平面 1.0

    VkRenderPassBeginInfo passBegin{};
    passBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passBegin.renderPass        = renderPass_;
    passBegin.framebuffer       = framebuffers_[imageIndex];
    passBegin.renderArea.extent = swapchainExtent_;
    passBegin.clearValueCount   = 2;
    passBegin.pClearValues      = clearValues;

    vkCmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(swapchainExtent_.width);
    viewport.height   = static_cast<float>(swapchainExtent_.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // ---- 旋转与 MVP（glm，列主序，不需要转置）-------------------------------
    // 自转只绕 Y 轴。内置 cube 时代是绕两个轴转（为了看清六个面），
    // 但对一个有明确"正面"的模型来说双轴自转会让人看不清朝向。
    //
    // 右乘 translate(-center)：把模型包围盒中心搬到原点再旋转，
    // 否则偏离原点的模型（很多 glTF 资产并不以原点为中心）会绕着原点公转。
    const glm::mat4 spin =
        glm::rotate(glm::mat4(1.0f), rotationRad_, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::translate(glm::mat4(1.0f), -modelCenter_);

    // 转屏补偿。走默认的 IDENTITY 路径时 rotateDeg 恒为 0、下面的旋转整段跳过，
    // 所以这段对桌面端和默认 Android 路径都是零开销；只有 createSwapchain
    // 退回 pre-rotation 路径时才生效。
    //
    // 两点都要处理，漏一个就出问题：
    //   1. framebuffer 是设备 identity 朝向的，可见宽高比要用**对调后**的值，
    //      否则横屏时 cube 会被压扁；
    //   2. clip space 里补一次 Z 轴旋转，否则画面躺倒。
    float fbWidth  = static_cast<float>(swapchainExtent_.width);
    float fbHeight = static_cast<float>(swapchainExtent_.height);
    float rotateDeg = 0.0f;
    switch (swapchainTransform_) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            rotateDeg = 90.0f;
            std::swap(fbWidth, fbHeight);
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            rotateDeg = 180.0f;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            rotateDeg = 270.0f;
            std::swap(fbWidth, fbHeight);
            break;
        default:
            break;
    }
    // 注意喂给相机的是**可见区域**的宽高比（已按 transform 对调），
    // 不是 framebuffer 的宽高比 —— 两者在 pre-rotation 路径下并不相同。
    camera_.setAspect(fbWidth / fbHeight);

    // 投影的 Y 翻转与 [0,1] 深度都在 Camera::proj() 里处理。
    // 但 pre-rotation 的那次旋转刻意留在渲染器：它属于呈现层的事，
    // 和「相机怎么看世界」无关，放进 Camera 会让相机类耦合 Android 的呈现细节。
    glm::mat4 viewProj = camera_.viewProj();
    if (rotateDeg != 0.0f) {
        // 左乘：等价于官方文档写的 MVP = pre_rotate * MVP
        viewProj = glm::rotate(glm::mat4(1.0f), glm::radians(rotateDeg),
                               glm::vec3(0.0f, 0.0f, 1.0f)) *
                   viewProj;
    }

    CubePushConstants pc{};

    // 方位角/仰角 -> 世界空间方向
    const float az = glm::radians(lighting_.azimuthDeg);
    const float el = glm::radians(lighting_.elevationDeg);
    const glm::vec3 lightWorld{std::cos(el) * std::sin(az), std::sin(el),
                               std::cos(el) * std::cos(az)};

    pc.lightColor = glm::vec4(lighting_.color[0], lighting_.color[1], lighting_.color[2],
                              lighting_.intensity);

    // stageFlags 与 size 都取自反射结果，没有手写常量。
    // 下面按 primitive 逐个覆盖 pc.mvp 后重新 push —— push constant 的
    // 写入是记录进 command buffer 的，每次 push 只影响其后的 draw。
    auto pushAndDraw = [&](const glm::mat4& nodeTransform, uint32_t firstIndex,
                           uint32_t count, uint32_t vertexOffset) {
        const glm::mat4 world = spin * nodeTransform;
        pc.mvp               = viewProj * world;

        // 光源方向变换到该 primitive 的物体空间，于是 shader 里可以直接和
        // 物体空间法线点乘，push constant 里不必再带一条法线矩阵。
        //
        // 这里用 inverse 而不是 transpose：cube 时代 model 只含旋转，
        // 逆等于转置；但 glTF 的节点普遍带缩放，那时转置就不是逆了，
        // 光照方向会随缩放而偏。
        //
        // 已知不足：非等比缩放下法线本身也会被 shader 里的 normalize 扭曲，
        // 严格做法是在世界空间打光并传 inverse-transpose 法线矩阵。
        // 对当前的单模型展示够用，等做 PBR 时一并改。
        const glm::vec3 lightObj = glm::inverse(glm::mat3(world)) * lightWorld;
        pc.lightDirObj = glm::vec4(lightObj, lighting_.ambient);

        vkCmdPushConstants(cmd, program_.layout, program_.pushConstantStages, 0,
                           program_.pushConstantSize, &pc);
        vkCmdDrawIndexed(cmd, count, 1, firstIndex, static_cast<int32_t>(vertexOffset), 0);
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, program_.layout, 0, 1,
                            &descriptorSet_, 0, nullptr);

    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &vertexOffset);
    // glTF 导入统一转成 uint32（模型顶点数常常超过 65535），
    // 内置 cube 的索引数组仍是 uint16 —— 类型在 createCubeMesh 里定好。
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, indexType_);

    if (model_.valid()) {
        // 每个 primitive 带自己由节点层级累乘出的变换。
        // 这里没有做材质切换 —— 当前只有一条 pipeline、一张 baseColor 贴图，
        // 多材质需要按材质分组并重绑 descriptor，属于下一步的事。
        for (const auto& prim : model_.primitives()) {
            pushAndDraw(prim.transform, prim.firstIndex, prim.indexCount, prim.vertexOffset);
        }
    } else {
        pushAndDraw(glm::mat4(1.0f), 0, indexCount_, 0);
    }

    // ImGui 必须画在同一个 render pass 内、且在 cube 之后
    // （它不写深度，要靠绘制顺序盖在场景上面）。
    //
    // 这也是本工程 swapchain 走 preTransform=IDENTITY 而非 pre-rotation 的原因：
    // imgui 后端内部自建正交矩阵并自己调 vkCmdSetViewport，拿不到我们的
    // pre-rotation 矩阵，走 pre-rotation 会把面板转歪。
    if (imguiReady_) {
        if (ImDrawData* drawData = ImGui::GetDrawData()) {
            ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
        }
    }

    vkCmdEndRenderPass(cmd);

    // 到这里颜色附件已经是 PRESENT_SRC_KHR 了 —— render pass 的
    // finalLayout 自动完成，不需要手写 barrier。非截图帧就此结束。

    // ---- 截图路径 -----------------------------------------------------------
    // 代价：render pass 已经把 image 转成 PRESENT_SRC_KHR，要读它必须
    //   PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC
    // 比 dynamic rendering 路径多一次转换（那边能插在 COLOR_ATTACHMENT ->
    // PRESENT_SRC 中间，只需两次）。只在截图帧发生，可以接受；真要省掉，
    // 得把 finalLayout 改成 COLOR_ATTACHMENT_OPTIMAL 并自己写 present 转换，
    // 那就把 render pass 的便利也一起丢了，不值得。
    //
    // copy 仍然刻意放在当帧的 command buffer 里而不是另起一次性提交：
    // 此刻这张 swapchain image 是本帧 acquire 到的、由本 command buffer 合法持有。
    // 若在 present 之后另起 command buffer 去转换，校验层会报
    // 「performs a layout transition on presentable image ... has not been acquired」。
    if (screenshotPending_ && screenshotBuffer_ != VK_NULL_HANDLE) {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image               = swapchainImages_[imageIndex];
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        toTransfer.oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toTransfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        // sync1 的 stage 掩码属于整条 vkCmdPipelineBarrier，不能按 barrier 指定。
        // 另外 sync1 没有 PIPELINE_STAGE_2_COPY_BIT 这种细分，只有 TRANSFER_BIT
        // （copy/blit/resolve/clear 四类的并集），精度上不如 sync2。
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toTransfer);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, swapchainImages_[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer_, 1,
                               &region);

        VkImageMemoryBarrier toPresent = toTransfer;
        toPresent.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask        = 0;
        // 转到 PRESENT_SRC 之后没有后续 GPU 访问需要等它。sync2 可以准确写
        // PIPELINE_STAGE_2_NONE，sync1 只能拿 BOTTOM_OF_PIPE 凑。
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &toPresent);
    }

    VK_CHECK(vkEndCommandBuffer(cmd));
}

// =============================================================================
// 每帧绘制
// =============================================================================

void VulkanRenderer::drawFrame() {
    // ---- 时间推进 -----------------------------------------------------------
    // 旋转角按 dt 累加而不是用绝对时间：面板上暂停或改速度时角度才是连续的，
    // 用 (now - start) * speed 的话一改速度就会跳变。
    const uint64_t now = SDL_GetTicks();
    const float    dt  = (lastTicks_ == 0)
                             ? 0.0f
                             : static_cast<float>(now - lastTicks_) * 0.001f;
    lastTicks_ = now;
    frameMs_   = dt * 1000.0f;

    if (lighting_.animate) {
        rotationRad_ += dt * lighting_.rotateSpeed;
    }

    // ---- ImGui 一帧 ---------------------------------------------------------
    // NewFrame/Render 必须成对，所以放在函数最前面，
    // 保证后面即使因为 swapchain 过期提前 return 也不会失配。
    if (imguiReady_) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        buildUi();
        ImGui::Render();
    }

    VK_CHECK(vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailableSemaphores_[currentFrame_],
                                       VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(r);
    }

    // 关键顺序：fence 必须在确定要提交工作之后才 reset。
    // 若在 acquire 之前 reset，一旦 acquire 返回 OUT_OF_DATE 提前 return，
    // 这个 fence 就再也没机会被 signal，下一轮 vkWaitForFences 直接死锁。
    // 这是 hello triangle 教程里非常常见的一个 bug。
    VK_CHECK(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]));

    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    recordCommandBuffer(cmd, imageIndex);

    // sync1 的 vkQueueSubmit：等待信号量与它们的等待阶段是**两个平行数组**，
    // 靠下标对应。数组长度写错编译器不会发现，这是 sync2 的
    // VkSemaphoreSubmitInfo（把信号量和阶段绑成一个结构体）想解决的问题之一。
    //
    // 另外 sync1 的 pSignalSemaphores 没有"触发阶段"字段，语义固定等价于
    // ALL_COMMANDS，即整个 command buffer 跑完才触发；sync2 能写成
    // 「颜色附件写完即可触发」，让 present 早一点开始。这里是第二处精度损失。
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &imageAvailableSemaphores_[currentFrame_];
    submit.pWaitDstStageMask    = &waitStage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &renderFinishedSemaphores_[imageIndex];  // 按 image 取

    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, inFlightFences_[currentFrame_]));

    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &renderFinishedSemaphores_[imageIndex];
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain_;
    present.pImageIndices      = &imageIndex;

    r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || framebufferDirty_) {
        framebufferDirty_ = false;
        recreateSwapchain();
    } else if (r != VK_SUCCESS) {
        VK_CHECK(r);
    }

    if (screenshotPending_) {
        // 调试路径，直接等队列空闲最简单；正式的 golden-image 测试里
        // 应该改成等本帧的 fence，避免整条队列停顿。
        VK_CHECK(vkQueueWaitIdle(queue_));
        writePendingScreenshot();
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

// =============================================================================
// 截图
// =============================================================================

void VulkanRenderer::requestScreenshot(const char* path) {
    screenshotPath_    = path;
    screenshotPending_ = true;
    ensureScreenshotBuffer();
}

void VulkanRenderer::ensureScreenshotBuffer() {
    const VkDeviceSize need =
        static_cast<VkDeviceSize>(swapchainExtent_.width) * swapchainExtent_.height * 4;
    if (screenshotBuffer_ != VK_NULL_HANDLE && screenshotSize_ == need) {
        return;
    }

    if (screenshotBuffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, screenshotBuffer_, screenshotAlloc_);
        screenshotBuffer_ = VK_NULL_HANDLE;
        screenshotAlloc_  = VK_NULL_HANDLE;
    }

    // HOST_ACCESS_RANDOM 而非 SEQUENTIAL_WRITE：这个 buffer 是给 CPU **读**的
    // （GPU 写进来、我们 memcpy 出去）。用 SEQUENTIAL_WRITE 会让 VMA 挑
    // uncached 内存，CPU 读它会慢到离谱 —— 方向填反是个很容易犯的错。
    createBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
                 screenshotBuffer_, screenshotAlloc_);
    screenshotSize_ = need;
}

void VulkanRenderer::writePendingScreenshot() {
    screenshotPending_ = false;
    if (screenshotBuffer_ == VK_NULL_HANDLE) {
        return;
    }

    // GPU 刚写完这块内存，CPU 读之前必须 invalidate。
    // 内存恰好是 HOST_COHERENT 时它是空操作，但不能因此省掉 ——
    // VMA 的 AUTO 有可能挑到 non-coherent 的类型，那时漏了就会读到旧数据。
    VK_CHECK(vmaInvalidateAllocation(allocator_, screenshotAlloc_, 0, screenshotSize_));

    VmaAllocationInfo shotInfo{};
    vmaGetAllocationInfo(allocator_, screenshotAlloc_, &shotInfo);

    std::vector<uint8_t> rgba(static_cast<size_t>(screenshotSize_));
    std::memcpy(rgba.data(), shotInfo.pMappedData, rgba.size());

    // swapchain 在桌面端通常是 BGRA，写 PNG 前换成 RGBA
    if (swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM ||
        swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            std::swap(rgba[i], rgba[i + 2]);
        }
    }

    const bool ok = writePngRgba(screenshotPath_.c_str(),
                                 static_cast<int>(swapchainExtent_.width),
                                 static_cast<int>(swapchainExtent_.height), rgba.data());
    if (ok) {
        spdlog::info("截图已保存: {} ({}x{})", screenshotPath_, swapchainExtent_.width,
                     swapchainExtent_.height);
    } else {
        spdlog::error("截图写入失败: {}", screenshotPath_);
    }
}

// =============================================================================
// 资源读取
// =============================================================================

std::vector<uint8_t> VulkanRenderer::readAsset(std::string_view path) {
    // SDL_LoadFile 在 Android 上会自动走 APK 的 AAssetManager，
    // 在桌面端则是普通文件读取 —— 于是这里不需要任何平台分支。
    //
    // 这也是改用 SDL 后最实在的一处收益：原先必须手写 AAssetManager_open，
    // 且不能用 std::ifstream（APK 内的 asset 没有文件系统路径）。
    //
    // 资源根由构建系统以 ORIGIN_ASSET_ROOT 注入，所以这里连 #ifdef 都不需要：
    //   Android  -> 空串，传相对路径，SDL 走 APK assets
    //   桌面     -> 源码树 assets/ 的绝对路径（开发期免拷贝；发版改成 exe 同级目录）
    const std::string full = std::string(ORIGIN_ASSET_ROOT) + std::string(path);

    size_t size = 0;
    void*  data = SDL_LoadFile(full.c_str(), &size);
    if (data == nullptr) {
        throw std::runtime_error("打不开资源 " + full + ": " + SDL_GetError());
    }

    const auto*          bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> out(bytes, bytes + size);
    SDL_free(data);
    return out;
}

// createShaderModule 已经不需要了：SPIR-V 的加载、反射与 VkShaderModule 创建
// 统一收到 ShaderReflect.cpp 的 createShaderStage() 里。

// =============================================================================
// ImGui
// =============================================================================

namespace {
void imguiCheckVkResult(VkResult r) {
    if (r != VK_SUCCESS) {
        spdlog::error("[imgui] VkResult = {} ({})", vkResultString(r), static_cast<int>(r));
    }
}
}  // namespace

void VulkanRenderer::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 不落盘 imgui.ini：面板位置这种状态没必要在工程目录里留文件，
    // Android 上当前工作目录也不一定可写。
    io.IniFilename = nullptr;

    // ---- DPI 自适应 ---------------------------------------------------------
    // 不做缩放的话面板在手机上只有指甲盖大小：ImGui 的默认尺寸是按约 96 DPI
    // 的桌面显示器定的，而手机的 density 通常是 2.5x ~ 3.5x。
    //
    // 缩放系数取自 SDL_GetWindowDisplayScale()，而不是 Android 的
    // AConfiguration_getDensity() —— SDL 已经把 Java 侧的
    // DisplayMetrics.density 填进了 display->content_scale
    // （见 SDL_androidvideo.c: display->content_scale = Android_ScreenDensity），
    // 所以这段代码在 Windows 和 Android 上是同一份，不需要平台分支，
    // 也不用为了拿密度去 include <android/configuration.h>。
    //
    // 参考的 Vulkan-glTF-PBR 用的是
    //   scale = screenDensity / ACONFIGURATION_DENSITY_MEDIUM   // 即 dpi / 160
    // 与 SDL 给出的 density 是同一个量，只是它必须走 Android 专用头。
    float dpiScale = SDL_GetWindowDisplayScale(window_);
    if (!(dpiScale > 0.0f)) {
        dpiScale = 1.0f;  // 查询失败/返回 0 时不缩放，至少不会把 UI 缩没
    }
    uiDpiScale_ = dpiScale;

    ImGuiStyle& style = ImGui::GetStyle();

    // ScaleAllSizes 就地放大 padding / 圆角 / 滚动条宽度等全部几何量。
    // 它是**累积**的（对同一个 style 调两次就放大两次），所以只能在这里调一次，
    // 不能放进每帧的 buildUi()。
    style.ScaleAllSizes(dpiScale);

    // 字体缩放。1.92 起 io.FontGlobalScale 已废弃，拆成两级：
    //   FontScaleDpi  —— 显示器 DPI，由程序按设备算出（就是这里）
    //   FontScaleMain —— 用户偏好，留给面板上的滑条
    // 默认字体 ProggyClean 是以 TTF 形式内嵌的，1.92 的动态字体系统会按实际
    // 像素大小重新光栅化，所以放大后是清晰的，不会变成马赛克 ——
    // 这也是不必像 glTF-PBR 那样额外打包一个 Roboto-Medium.ttf 的原因。
    style.FontScaleDpi = dpiScale;

    // 触摸屏上手指比鼠标钝得多，给控件命中区留额外余量。
    // 注意这个值要自己乘 dpiScale：上面的 ScaleAllSizes 已经执行过了。
    if (dpiScale > 1.5f) {
        style.TouchExtraPadding = ImVec2(4.0f * dpiScale, 4.0f * dpiScale);
    }

    if (!ImGui_ImplSDL3_InitForVulkan(window_)) {
        throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan 失败");
    }

    ImGui_ImplVulkan_InitInfo info{};
    // 必须和 createInstance 里 VkApplicationInfo::apiVersion 一致。
    // 填高了 imgui 会去用当前 instance 并不支持的入口，运行期才炸。
    info.ApiVersion     = kRequiredApiVersion;
    info.Instance       = instance_;
    info.PhysicalDevice = physicalDevice_;
    info.Device         = device_;
    info.QueueFamily    = queueFamilyIndex_;
    info.Queue          = queue_;

    // DescriptorPoolSize > 0 时后端自建描述符池，我们不用管它的生命周期。
    // 它只用来放字体/用户纹理的 combined image sampler，16 个足够。
    info.DescriptorPoolSize = 16;

    info.MinImageCount = 2;
    info.ImageCount    = static_cast<uint32_t>(swapchainImages_.size());

    // 注意：imgui 在 2025/09/26 之后把 RenderPass / Subpass / MSAASamples
    // 从 InitInfo 顶层挪进了 PipelineInfoMain。照老教程写 info.RenderPass
    // 在 v1.92 上根本编不过 —— 这是回退到 render pass 路径时最容易被旧资料
    // 带偏的一处。
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.RenderPass  = renderPass_;
    info.PipelineInfoMain.Subpass     = 0;

    // UseDynamicRendering = false 时 imgui 用 PipelineInfoMain.RenderPass 建管线，
    // PipelineRenderingCreateInfo 整块被忽略。
    //
    // 回到 render pass 后顺带消掉了一个坑：dynamic rendering 路径下必须给
    // imgui 也填 depthAttachmentFormat（哪怕它压根不读写深度），因为管线声明的
    // 附件格式要和 VkRenderingInfo 逐个附件对齐；漏填时画面完全正常但校验层
    // 每帧刷 format mismatch。render pass 的兼容性判定以 renderPass 对象为准，
    // 不存在这个问题。
    info.UseDynamicRendering = false;

    // 让校验层的 best-practices 检查不再抱怨小块分配
    info.MinAllocationSize = 1024 * 1024;
    info.CheckVkResultFn   = imguiCheckVkResult;

    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init 失败");
    }

    imguiReady_ = true;
    spdlog::info("ImGui {} 已初始化（SDL3 + Vulkan 后端，volk 模式），DPI 缩放 {:.2f}x",
                 IMGUI_VERSION, uiDpiScale_);
}

void VulkanRenderer::shutdownImGui() {
    if (!imguiReady_) {
        return;
    }
    imguiReady_ = false;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void VulkanRenderer::buildUi() {
    // 界面文字用英文：ImGui 默认字体（ProggyClean）不含中文字形，
    // 直接写中文会渲染成方框。要中文界面得额外加一个 TTF 资源 +
    // ImFontGlyphRangesBuilder，且 Android 侧还要把字体打进 APK，
    // 对一个调试面板来说不划算。
    // 位置和宽度必须乘 DPI 系数：ScaleAllSizes 只管 style 里的几何量，
    // 管不到这里手写的绝对像素值 —— 漏乘的话手机上面板会挤在角落且过窄。
    const float s = uiDpiScale_;
    ImGui::SetNextWindowPos(ImVec2(16.0f * s, 16.0f * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f * s, 0.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Origin")) {
        ImGui::Text("%.2f ms  (%.0f FPS)", frameMs_,
                    frameMs_ > 0.0f ? 1000.0f / frameMs_ : 0.0f);

        ImGui::SeparatorText("Lighting");
        ImGui::SliderFloat("Azimuth", &lighting_.azimuthDeg, -180.0f, 180.0f, "%.0f deg");
        ImGui::SliderFloat("Elevation", &lighting_.elevationDeg, -89.0f, 89.0f, "%.0f deg");
        ImGui::SliderFloat("Intensity", &lighting_.intensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Ambient", &lighting_.ambient, 0.0f, 1.0f, "%.2f");
        ImGui::ColorEdit3("Color", lighting_.color);

        ImGui::SeparatorText("Animation");
        ImGui::Checkbox("Spin", &lighting_.animate);
        ImGui::SliderFloat("Speed", &lighting_.rotateSpeed, 0.0f, 3.0f, "%.2f rad/s");

        // 把反射结果也显示出来：改了 HLSL 重新构建后，
        // 不用翻日志就能直接在画面上确认 binding 表和 push constant 大小。
        if (ImGui::Button("Screenshot -> origin_shot.png")) {
            requestScreenshot("origin_shot.png");
        }

        // 设备上再微调字号。FontScaleDpi 已经按屏幕密度自动设好，这一项是
        // 叠加在它之上的用户偏好（style.FontScaleMain），改完立即生效 ——
        // 1.92 的字体是动态光栅化的，不需要重建字体图集。
        ImGui::SeparatorText("Camera");
        ImGui::Text("distance %.2f  (drag=orbit, wheel/pinch=zoom)", camera_.distance());
        if (cameraController_ != nullptr) {
            ImGui::SliderFloat("Orbit speed", &cameraController_->rotateSpeed, 0.2f, 3.0f,
                               "%.2f");
            ImGui::SliderFloat("Zoom speed", &cameraController_->zoomSpeed, 0.2f, 3.0f,
                               "%.2f");
            ImGui::Checkbox("Invert Y", &cameraController_->invertY);
        }
        if (ImGui::Button("Reset view")) {
            camera_.reset();
            if (model_.valid()) {
                const float radius = std::max(model_.boundsRadius(), 0.001f);
                camera_.setDistance(radius / std::tan(glm::radians(30.0f)) * 1.6f);
            }
        }

        ImGui::SeparatorText("UI");
        ImGui::SliderFloat("Font scale", &ImGui::GetStyle().FontScaleMain, 0.5f, 3.0f,
                           "%.2fx");
        ImGui::Text("auto DPI scale: %.2fx", uiDpiScale_);

        ImGui::SeparatorText("Model");
        if (model_.valid()) {
            ImGui::Text("%zu primitives, %zu verts", model_.primitives().size(),
                        model_.vertices().size());
            ImGui::Text("bounds radius %.3f", model_.boundsRadius());
        } else {
            ImGui::TextUnformatted("builtin cube (glTF load failed)");
        }

        ImGui::SeparatorText("Memory (VMA)");
        {
            // vmaGetHeapBudgets 要按 heap 数量给数组，MAX_MEMORY_HEAPS 是上限。
            // usage  = 我们自己分配出去的量
            // budget = 驱动认为当前可用的量（会随其他进程的占用而变化，
            //          比 heap 的物理大小更有参考价值）
            VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
            vmaGetHeapBudgets(allocator_, budgets);

            const VkPhysicalDeviceMemoryProperties* mp = nullptr;
            vmaGetMemoryProperties(allocator_, &mp);

            for (uint32_t i = 0; i < mp->memoryHeapCount; ++i) {
                if (budgets[i].statistics.blockBytes == 0) {
                    continue;  // 这个堆我们没用到，不占面板空间
                }
                const bool deviceLocal =
                    (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
                ImGui::Text("heap %u%s: %.2f / %.2f MB  (%u blocks, %u allocs)", i,
                            deviceLocal ? " [dev]" : "",
                            static_cast<double>(budgets[i].statistics.allocationBytes) /
                                (1024.0 * 1024.0),
                            static_cast<double>(budgets[i].statistics.blockBytes) /
                                (1024.0 * 1024.0),
                            budgets[i].statistics.blockCount,
                            budgets[i].statistics.allocationCount);
            }
            // allocationBytes < blockBytes 的差值就是 suballocation 的内部碎片。
            // blockCount 明显小于 allocationCount 正是 VMA 在起作用的证据：
            // 多个资源共享同一块 VkDeviceMemory。
            ImGui::TextDisabled("allocs < blocks = VMA suballocation");
        }

        ImGui::SeparatorText("Reflected layout");
        ImGui::Text("push constant: %u bytes", program_.pushConstantSize);
        for (const auto& b : program_.bindings) {
            ImGui::BulletText("binding %u  type=%d  x%u", b.binding,
                              static_cast<int>(b.descriptorType), b.descriptorCount);
        }
    }
    ImGui::End();
}

// =============================================================================
// 底层资源辅助
// =============================================================================

// findMemoryType 已删除：内存类型的挑选交给 VMA。
// 原先那段「遍历 memoryTypes 找第一个 propertyFlags 匹配的」是教程写法，
// 它忽略了堆大小、是否 cached、以及设备可能有多个同样满足条件但性能不同的
// 内存类型 —— VMA 的 AUTO 会把这些都考虑进去。

VkFormat VulkanRenderer::findDepthFormat() const {
    // 按偏好顺序试：优先纯深度（不带 stencil，移动端带宽更省）。
    // D32_SFLOAT 在桌面上必然支持，但移动端不保证，所以要真去查。
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_X8_D24_UNORM_PACK32,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return f;
        }
    }
    throw std::runtime_error("没有可用的深度格式");
}

void VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VmaAllocationCreateFlags allocFlags, VkBuffer& buffer,
                                  VmaAllocation& allocation) const {
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    // VMA_MEMORY_USAGE_AUTO 是 VMA 3.x 推荐的用法：不再由调用方指定
    // "要 DEVICE_LOCAL 还是 HOST_VISIBLE"，而是让 VMA 结合 usage 标志位
    // 与设备实际的 memory heap 布局去选。
    //
    // 这一条在双平台上是实质收益：桌面独显有独立显存、staging 必须走
    // HOST_VISIBLE 再拷贝；移动端是统一内存，同一块内存既 DEVICE_LOCAL
    // 又 HOST_VISIBLE，多这一次拷贝纯属浪费。AUTO 让 VMA 各自选最优，
    // 我们不必写 #ifdef __ANDROID__。
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = allocFlags;

    // 一次调用完成 vkCreateBuffer + 分配 + vkBindBufferMemory。
    // 注意 VMA 内部是 suballocation：多个 buffer 可能落在同一块
    // VkDeviceMemory 的不同偏移上，所以绝不能对 VmaAllocation 调 vkFreeMemory。
    VK_CHECK(vmaCreateBuffer(allocator_, &info, &allocInfo, &buffer, &allocation, nullptr));
}

VkCommandBuffer VulkanRenderer::beginOneTimeCommands() {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool        = commandPool_;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

void VulkanRenderer::endOneTimeCommands(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));

    // 初始化期的一次性上传，直接等队列空闲即可。
    // 真实场景该用 fence + 传输队列异步化，但那属于资源流式加载的范畴。
    VK_CHECK(vkQueueWaitIdle(queue_));
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void VulkanRenderer::uploadViaStaging(const void* data, VkDeviceSize size,
                                      VkBufferUsageFlags usage, VkBuffer& buffer,
                                      VmaAllocation& allocation) {
    VkBuffer      staging      = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    // HOST_ACCESS_SEQUENTIAL_WRITE 是在告诉 VMA「我只会顺序写一遍，不读」，
    // 它据此优先挑 uncached 的 HOST_VISIBLE 内存（写合并快、不占 cache）。
    // MAPPED_BIT 让 VMA 建完就保持映射，省掉我们自己 map/unmap。
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
                 staging, stagingAlloc);

    // 映射地址直接从 allocation 信息里取，不再调 vkMapMemory ——
    // 后者对 suballocation 是错的（会映射整块大内存而不是我们那一段）。
    VmaAllocationInfo stagingInfo{};
    vmaGetAllocationInfo(allocator_, stagingAlloc, &stagingInfo);
    std::memcpy(stagingInfo.pMappedData, data, static_cast<size_t>(size));
    // 内存可能不是 HOST_COHERENT（VMA 的 AUTO 允许挑 non-coherent），
    // 所以必须显式 flush。vmaFlushAllocation 内部会判断是否真的需要，
    // coherent 的情况下是空操作 —— 这正是不该自己写 vkFlushMappedMemoryRanges 的原因。
    VK_CHECK(vmaFlushAllocation(allocator_, stagingAlloc, 0, size));

    // 目标缓冲不带 HOST_ACCESS 标志，VMA 会挑 DEVICE_LOCAL：
    // 顶点/索引数据每帧被读很多次、只写一次，值得多做一次拷贝换带宽。
    createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, buffer, allocation);

    VkCommandBuffer cmd = beginOneTimeCommands();
    VkBufferCopy    region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, staging, buffer, 1, &region);
    endOneTimeCommands(cmd);

    // 一次调用同时销毁 buffer 与归还那段分配
    vmaDestroyBuffer(allocator_, staging, stagingAlloc);
}

// =============================================================================
// 深度缓冲
// =============================================================================

void VulkanRenderer::createDepthResources() {
    depthFormat_ = findDepthFormat();

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = depthFormat_;
    info.extent        = {swapchainExtent_.width, swapchainExtent_.height, 1};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // 深度缓冲是典型的"只有 GPU 碰、CPU 永不访问"，而且尺寸大、生命周期与
    // swapchain 绑定。给 DEDICATED_MEMORY_BIT 让它独占一块 VkDeviceMemory：
    // 这样 swapchain 重建时释放的是整块，不会在共享块里留下碎片。
    // 移动端还有额外好处 —— 独立分配更容易被驱动放进 lazily-allocated 内存。
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VK_CHECK(vmaCreateImage(allocator_, &info, &allocInfo, &depthImage_, &depthAlloc_,
                            nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = depthImage_;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &depthView_));

    // 这里刻意只标 VK_IMAGE_ASPECT_DEPTH_BIT：我们只把它当深度附件用，
    // 即使格式带 stencil 位也不需要在 view 里暴露。
    // 注意 recordCommandBuffer 里那条 barrier 的 aspectMask 不同 ——
    // 规范要求 barrier 对组合格式必须两位都标（见 depthAspectMask 的说明）。
    //
    // 不需要在这里做初始 layout 转换：每帧开头的 barrier 用
    // oldLayout=UNDEFINED 转到 DEPTH_STENCIL_ATTACHMENT_OPTIMAL，
    // 配合 loadOp=CLEAR 语义正确（UNDEFINED 表示丢弃原内容，本就要清）。
}

// =============================================================================
// Cube 网格与纹理
// =============================================================================

void VulkanRenderer::createCubeMesh() {
    // 优先加载 glTF 模型；失败则退回内置 cube。
    //
    // 这个退路不是摆设：模型文件可能没被打进 APK、或者用户换了个
    // 带不支持特性的文件。有退路的话链路依然可见（画面出 cube），
    // 而不是黑屏让人无从判断是资源问题还是渲染问题。
    const void*  vtxData  = kCubeVertices;
    VkDeviceSize vtxBytes = sizeof(kCubeVertices);
    const void*  idxData  = kCubeIndices;
    VkDeviceSize idxBytes = sizeof(kCubeIndices);
    uint32_t     idxCount = static_cast<uint32_t>(sizeof(kCubeIndices) / sizeof(kCubeIndices[0]));

    try {
        std::vector<uint8_t> bytes = readAsset(kModelAssetPath);
        model_.loadFromMemory(bytes.data(), bytes.size(), kModelAssetPath);
    } catch (const std::exception& e) {
        spdlog::warn("glTF 模型加载失败，退回内置 cube: {}", e.what());
    }

    if (model_.valid()) {
        vtxData  = model_.vertices().data();
        vtxBytes = model_.vertices().size() * sizeof(Vertex);
        idxData  = model_.indices().data();
        idxBytes = model_.indices().size() * sizeof(uint32_t);
        idxCount = static_cast<uint32_t>(model_.indices().size());

        // 让相机自动取一个能把模型完整收进画面的距离。
        // 不这么做的话，DamagedHelmet（半径约 1）和 Sponza（半径几十）
        // 用同一个固定距离会一个偏小一个塞满屏幕。
        // /tan(fov/2) 是让包围球正好内切于竖直视野，再留 1.6 倍余量。
        const float radius = std::max(model_.boundsRadius(), 0.001f);
        camera_.setDistanceLimits(radius * 0.1f, radius * 50.0f);
        camera_.setDistance(radius / std::tan(glm::radians(30.0f)) * 1.6f);
        modelCenter_ = model_.boundsCenter();
    }

    // 能取地址时给两个 buffer 加上 SHADER_DEVICE_ADDRESS usage。
    //
    // 不是为了现在用，而是因为 usage 只能在 vkCreateBuffer 时定、事后改不了：
    // GPU-driven 路径要在 compute/vertex shader 里直接按指针读顶点与索引
    // （niagara 就是这么做的），到那时再想加就得重建 buffer。
    // 不带 device address 能力的设备照旧，只是少这一位 usage。
    VkBufferUsageFlags meshUsageExtra = 0;
    if (caps_.bufferDeviceAddress) {
        meshUsageExtra = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    uploadViaStaging(vtxData, vtxBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | meshUsageExtra,
                     vertexBuffer_, vertexAlloc_);
    uploadViaStaging(idxData, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | meshUsageExtra,
                     indexBuffer_, indexAlloc_);
    indexCount_ = idxCount;
    indexType_  = model_.valid() ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

    // ---- 端到端验证 ---------------------------------------------------------
    // 走完整条链路才算"能用"：扩展启用 -> VMA 分配时挂上 DEVICE_ADDRESS flag
    // -> buffer 带 SHADER_DEVICE_ADDRESS usage -> 取到非零地址。
    // 前面任何一环漏了，这里就会拿到 nullptr 函数指针或 0 地址。
    //
    // 注意用的是带 KHR 后缀的入口：appInfo.apiVersion 是 1.1，无后缀的
    // vkGetBufferDeviceAddress 属于 1.2 核心，volk 不会为它填指针。
    if (caps_.bufferDeviceAddress) {
        if (vkGetBufferDeviceAddressKHR == nullptr) {
            spdlog::error("bufferDeviceAddress 已启用，但 vkGetBufferDeviceAddressKHR "
                          "函数指针为空 —— volk 没加载到该入口");
        } else {
            VkBufferDeviceAddressInfo addrInfo{};
            addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addrInfo.buffer = vertexBuffer_;
            const VkDeviceAddress vtxAddr = vkGetBufferDeviceAddressKHR(device_, &addrInfo);
            addrInfo.buffer               = indexBuffer_;
            const VkDeviceAddress idxAddr = vkGetBufferDeviceAddressKHR(device_, &addrInfo);
            spdlog::info("bufferDeviceAddress 验证通过: vertex=0x{:016X} index=0x{:016X}",
                         vtxAddr, idxAddr);
        }
    }

    if (!model_.valid()) {
        spdlog::info("内置 cube 网格: {} 顶点, {} 索引",
                     sizeof(kCubeVertices) / sizeof(kCubeVertices[0]), indexCount_);
    }
}

void VulkanRenderer::createTexture() {
    // 先把整个文件读进内存（Android 上走 APK assets），再交给 stb 解码。
    // 用 stbi_load_from_memory 而不是 stbi_load：后者要 fopen，
    // 而 APK 内的 asset 没有文件系统路径 —— 这也是 StbImage.cpp 里
    // 定义 STBI_NO_STDIO 的原因。
    // 优先用 glTF 内嵌的基础色贴图；模型没有贴图时才退回工程自带的 texture.jpg。
    //
    // 注意这里体现了 GltfModel 的设计取舍：它刻意**不**解码图像
    // （tinygltf 的 images_as_is=1），只把原始 PNG/JPEG 字节交出来，
    // 于是解码统一走工程里唯一的那份 stb 实现（src/StbImage.cpp），
    // 不会出现第二个 STB_IMAGE_IMPLEMENTATION。
    std::vector<uint8_t> fileBytes;
    const uint8_t*       encoded     = nullptr;
    size_t               encodedSize = 0;
    const char*          texSource   = nullptr;

    if (model_.baseColorImage().data != nullptr) {
        encoded     = model_.baseColorImage().data;
        encodedSize = model_.baseColorImage().size;
        texSource   = "glTF baseColorTexture";
    } else {
        fileBytes   = readAsset("textures/texture.jpg");
        encoded     = fileBytes.data();
        encodedSize = fileBytes.size();
        texSource   = "textures/texture.jpg";
    }

    int width = 0, height = 0, channels = 0;
    // 强制解成 4 通道：RGB 三通道纹理在很多 GPU 上不被支持为采样格式，
    // 而 R8G8B8A8 是 Vulkan 保证支持的。
    stbi_uc* pixels = stbi_load_from_memory(encoded, static_cast<int>(encodedSize), &width,
                                            &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        throw std::runtime_error(std::string("解码纹理失败 (") + texSource + "): " +
                                 stbi_failure_reason());
    }
    spdlog::info("纹理[{}]: {}x{}, 源通道数 {} (已强制转为 RGBA)", texSource, width, height,
                 channels);

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer      staging      = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT,
                 staging, stagingAlloc);

    VmaAllocationInfo stagingInfo{};
    vmaGetAllocationInfo(allocator_, stagingAlloc, &stagingInfo);
    std::memcpy(stagingInfo.pMappedData, pixels, static_cast<size_t>(imageSize));
    VK_CHECK(vmaFlushAllocation(allocator_, stagingAlloc, 0, imageSize));

    stbi_image_free(pixels);

    textureMipLevels_ = 1;  // 先不做 mipmap，旋转 cube 的最小缩放比还看不出走样

    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent        = {static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height), 1};
    imageInfo.mipLevels     = textureMipLevels_;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo texAlloc{};
    texAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    // 纹理不给 DEDICATED：它尺寸相对固定、数量会随场景增长，
    // 让 VMA 把多张贴图 suballocate 进同一块大内存才是它的主场
    // （避免撞 maxMemoryAllocationCount，移动端常见上限 4096）。
    //
    // vmaCreateImage 内部会调 vkCreateImage，不要在它之前自己再建一次 ——
    // 那样第一个 image 句柄会被覆盖掉，成为一处静默泄漏。
    VK_CHECK(vmaCreateImage(allocator_, &imageInfo, &texAlloc, &textureImage_,
                            &textureAlloc_, nullptr));

    // ---- 上传：UNDEFINED -> TRANSFER_DST -> 拷贝 -> SHADER_READ_ONLY --------
    VkCommandBuffer cmd = beginOneTimeCommands();

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image                       = textureImage_;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = textureMipLevels_;
    toTransfer.subresourceRange.layerCount = 1;
    // 这是刚创建出来的 image，没有任何在前的访问需要等待。
    // sync2 能准确写成 srcStage/srcAccess = NONE；sync1 只能用
    // TOP_OF_PIPE + srcAccessMask=0 来表达同一件事。
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, staging, textureImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toTransfer;
    toShader.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toShader);

    endOneTimeCommands(cmd);

    vmaDestroyBuffer(allocator_, staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = textureImage_;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = textureMipLevels_;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &textureView_));

    // HLSL 里 Texture2D 与 SamplerState 是分开声明的（对应 Vulkan 的
    // SAMPLED_IMAGE + SAMPLER 两个 binding），所以这里建一个独立的 sampler。
    // anisotropy 需要设备特性支持，为保持移动端兼容先不开。
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod       = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler_));
}

// =============================================================================
// 描述符集
// =============================================================================

void VulkanRenderer::createDescriptorSet() {
    // 池的大小直接由反射出的 binding 表推出来 —— 不需要手写
    // 「1 个 sampled image + 1 个 sampler」这种会和 shader 脱节的常量。
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (const auto& b : program_.bindings) {
        auto it = std::find_if(poolSizes.begin(), poolSizes.end(),
                               [&](const VkDescriptorPoolSize& p) {
                                   return p.type == b.descriptorType;
                               });
        if (it != poolSizes.end()) {
            it->descriptorCount += b.descriptorCount;
        } else {
            poolSizes.push_back({b.descriptorType, b.descriptorCount});
        }
    }
    if (poolSizes.empty()) {
        return;  // shader 不用任何 descriptor，例如纯 push constant 的情况
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &program_.setLayout;
    VK_CHECK(vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_));

    // 纹理与 sampler 在整个生命周期内不变，所以只在这里写一次。
    // 写入时按反射出的 type 分派，binding 号也来自反射 ——
    // 改 HLSL 里的 binding 号，这段代码不用动。
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView   = textureView_;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler = textureSampler_;

    std::vector<VkWriteDescriptorSet> writes;
    for (const auto& b : program_.bindings) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = descriptorSet_;
        w.dstBinding      = b.binding;
        w.descriptorCount = 1;
        w.descriptorType  = b.descriptorType;

        switch (b.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                w.pImageInfo = &imageInfo;
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                w.pImageInfo = &samplerInfo;
                break;
            default:
                throw std::runtime_error(
                    "binding " + std::to_string(b.binding) +
                    " 的类型还没有对应的资源绑定逻辑（需要在 createDescriptorSet 里补）");
        }
        writes.push_back(w);
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

}  // namespace origin
