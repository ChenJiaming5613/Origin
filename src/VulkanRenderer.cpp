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
#include <utility>  // std::swap

namespace origin {
namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

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

    // command pool 必须先建：顶点缓冲与纹理都要走 staging 上传，
    // 而上传需要一个临时 command buffer。
    createCommandBuffers();

    createSwapchain();
    createDepthResources();
    createRenderPass();

    createCubeMesh();
    createTexture();

    createPipeline();       // 内部调 createProgram()，layout 由反射生成
    createDescriptorSet();  // 需要 program_.setLayout 与纹理都已就位

    createFramebuffers();
    createSyncObjects();

    initImGui();  // 需要 renderPass 与 swapchain image 数量都已确定

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
    if (textureImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, textureImage_, nullptr);
        textureImage_ = VK_NULL_HANDLE;
    }
    if (textureMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, textureMemory_, nullptr);
        textureMemory_ = VK_NULL_HANDLE;
    }

    if (indexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        indexBuffer_ = VK_NULL_HANDLE;
    }
    if (indexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, indexBufferMemory_, nullptr);
        indexBufferMemory_ = VK_NULL_HANDLE;
    }
    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vertexBuffer_ = VK_NULL_HANDLE;
    }
    if (vertexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        vertexBufferMemory_ = VK_NULL_HANDLE;
    }

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    // program_ 持有 pipelineLayout 与 descriptorSetLayout（都是反射生成的）
    destroyProgram(device_, program_);
    destroyShaderStage(device_, vertexStage_);
    destroyShaderStage(device_, pixelStage_);

    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
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
    VkApplicationInfo appInfo{};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Origin";
    appInfo.pEngineName      = "none";
    appInfo.apiVersion       = VK_API_VERSION_1_0;

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

    // 移动端基本只有一个集成 GPU，不需要打分排序：
    // 找到第一个「同时支持图形和呈现」的队列族即可。
    for (VkPhysicalDevice dev : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qProps.data());

        for (uint32_t i = 0; i < qCount; ++i) {
            if (!(qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            VkBool32 presentSupport = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport));
            if (presentSupport != VK_TRUE) {
                continue;
            }

            physicalDevice_   = dev;
            queueFamilyIndex_ = i;

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(dev, &props);
            spdlog::info("选中 GPU: {} (driver {}, API {}.{}.{})", props.deviceName,
                         props.driverVersion, VK_VERSION_MAJOR(props.apiVersion),
                         VK_VERSION_MINOR(props.apiVersion),
                         VK_VERSION_PATCH(props.apiVersion));
            return;
        }
    }
    throw std::runtime_error("没有找到同时支持图形与呈现的队列族");
}

void VulkanRenderer::createDevice() {
    const float priority = 1.0f;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex_;
    queueInfo.queueCount       = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceFeatures features{};  // hello triangle 不需要任何可选特性

    VkDeviceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount    = 1;
    info.pQueueCreateInfos       = &queueInfo;
    info.enabledExtensionCount   = 1;
    info.ppEnabledExtensionNames = deviceExtensions;
    info.pEnabledFeatures        = &features;

    VK_CHECK(vkCreateDevice(physicalDevice_, &info, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);
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

void VulkanRenderer::destroyDepthResources() {
    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroySwapchainDependents() {
    // 截图缓冲的大小跟 swapchain 尺寸绑定，尺寸变了必须重建
    if (screenshotBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, screenshotBuffer_, nullptr);
        vkFreeMemory(device_, screenshotMemory_, nullptr);
        screenshotBuffer_ = VK_NULL_HANDLE;
        screenshotMemory_ = VK_NULL_HANDLE;
        screenshotSize_   = 0;
    }

    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

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

    // 注意这里没有销毁 pipeline 与 renderPass：
    // pipeline 的 viewport/scissor 是动态状态，renderPass 只依赖格式（未变），
    // 因此两者都能跨 swapchain 重建复用 —— 这是避免转屏卡顿的关键。
    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView v : swapchainImageViews_) {
        vkDestroyImageView(device_, v, nullptr);
    }
    swapchainImageViews_.clear();

    if (screenshotBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, screenshotBuffer_, nullptr);
        vkFreeMemory(device_, screenshotMemory_, nullptr);
        screenshotBuffer_  = VK_NULL_HANDLE;
        screenshotMemory_  = VK_NULL_HANDLE;
        screenshotSize_    = 0;
        screenshotPending_ = false;  // 尺寸变了，这次截图作废
    }

    // 深度缓冲的尺寸跟 swapchain 绑定，必须一起重建。
    // renderPass 只依赖**格式**（颜色格式与深度格式都没变），所以仍可复用。
    destroyDepthResources();

    createSwapchain();
    createDepthResources();
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
// RenderPass / Pipeline / Framebuffer
// =============================================================================

void VulkanRenderer::createRenderPass() {
    VkAttachmentDescription color{};
    color.format         = swapchainFormat_;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // 深度附件：storeOp = DONT_CARE 很关键。
    // 深度值出了本 pass 就没人再读，声明为 DONT_CARE 能让 Mali/Adreno 的
    // tile 架构完全跳过把深度写回主存这一步，是移动端最省带宽的一处改动。
    VkAttachmentDescription depth{};
    depth.format         = depthFormat_;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

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

    // 这条依赖保证「呈现引擎读完上一帧」与「本帧开始写颜色附件」之间有正确屏障。
    // 少了它会出现间歇性撕裂，校验层也会报 layout transition 竞争。
    //
    // 加了深度后 stage/access 掩码必须一并扩上 EARLY_FRAGMENT_TESTS 与
    // DEPTH_STENCIL_ATTACHMENT_WRITE —— 只写颜色那份掩码会让校验层
    // 报深度附件的 layout transition 缺少同步。
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const VkAttachmentDescription attachments[] = {color, depth};

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments    = attachments;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(device_, &info, nullptr, &renderPass_));
}

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
    info.renderPass          = renderPass_;
    info.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_));

    // shader module 不在这里销毁：它们归 vertexStage_ / pixelStage_ 所有，
    // 在 shutdown() 里统一由 destroyShaderStage 处理。
}

void VulkanRenderer::createFramebuffers() {
    framebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        // 深度 image 只有一张、被所有 swapchain image 共用：
        // 同一时刻只有一帧在写深度（fence 保证），不需要每张一份。
        const VkImageView attachments[] = {swapchainImageViews_[i], depthView_};

        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments    = attachments;
        info.width           = swapchainExtent_.width;
        info.height          = swapchainExtent_.height;
        info.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]));
    }
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

    // clearValues 的下标必须与 renderPass 的 attachment 顺序一致
    VkClearValue clears[2]{};
    clears[0].color = {{0.05f, 0.06f, 0.09f, 1.0f}};  // 深蓝灰，区分「没画」与「画黑了」
    clears[1].depthStencil = {1.0f, 0};               // 常规深度：远平面是 1.0

    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = renderPass_;
    rp.framebuffer       = framebuffers_[imageIndex];
    rp.renderArea.extent = swapchainExtent_;
    rp.clearValueCount   = 2;
    rp.pClearValues      = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
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

    // ImGui 必须画在同一个 render pass 内、且在 cube 之后（它不写深度，
    // 要靠绘制顺序盖在场景上面）。
    if (imguiReady_) {
        if (ImDrawData* drawData = ImGui::GetDrawData()) {
            ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
        }
    }

    vkCmdEndRenderPass(cmd);

    // 截图的 copy 刻意放在这里、而不是单独提交一次一次性命令：
    // 此刻这张 swapchain image 是本帧 acquire 到的、由本 command buffer 合法持有，
    // 所以 PRESENT_SRC -> TRANSFER_SRC -> PRESENT_SRC 的来回转换是干净的。
    // 若改成在 present 之后另起一个 command buffer 去转换，校验层会报
    // 「performs a layout transition on presentable image ... has not been acquired」。
    if (screenshotPending_ && screenshotBuffer_ != VK_NULL_HANDLE) {
        VkImageMemoryBarrier toSrc{};
        toSrc.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout                   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toSrc.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image                       = swapchainImages_[imageIndex];
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        toSrc.srcAccessMask               = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toSrc);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, swapchainImages_[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, screenshotBuffer_, 1,
                               &region);

        VkImageMemoryBarrier back = toSrc;
        back.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        back.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask        = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &back);
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

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

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
        vkDestroyBuffer(device_, screenshotBuffer_, nullptr);
        vkFreeMemory(device_, screenshotMemory_, nullptr);
        screenshotBuffer_ = VK_NULL_HANDLE;
        screenshotMemory_ = VK_NULL_HANDLE;
    }

    createBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 screenshotBuffer_, screenshotMemory_);
    screenshotSize_ = need;
}

void VulkanRenderer::writePendingScreenshot() {
    screenshotPending_ = false;
    if (screenshotBuffer_ == VK_NULL_HANDLE) {
        return;
    }

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, screenshotMemory_, 0, screenshotSize_, 0, &mapped));

    std::vector<uint8_t> rgba(static_cast<size_t>(screenshotSize_));
    std::memcpy(rgba.data(), mapped, rgba.size());
    vkUnmapMemory(device_, screenshotMemory_);

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
    info.ApiVersion     = VK_API_VERSION_1_0;
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
    // 在 v1.92 上根本编不过。
    info.PipelineInfoMain.RenderPass  = renderPass_;
    info.PipelineInfoMain.Subpass     = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

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

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits,
                                        VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const bool typeOk  = (typeBits & (1u << i)) != 0;
        const bool propsOk = (memProps.memoryTypes[i].propertyFlags & props) == props;
        if (typeOk && propsOk) {
            return i;
        }
    }
    throw std::runtime_error("找不到满足要求的内存类型");
}

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
                                  VkMemoryPropertyFlags props, VkBuffer& buffer,
                                  VkDeviceMemory& memory) const {
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &memory));

    VK_CHECK(vkBindBufferMemory(device_, buffer, memory, 0));
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
                                      VkDeviceMemory& memory) {
    VkBuffer       staging       = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMemory);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, stagingMemory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(device_, stagingMemory);

    // 目标缓冲放在 DEVICE_LOCAL：顶点/索引数据每帧被读很多次、写一次，
    // 值得多做一次拷贝换取显存带宽。
    createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, memory);

    VkCommandBuffer cmd = beginOneTimeCommands();
    VkBufferCopy    region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, staging, buffer, 1, &region);
    endOneTimeCommands(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
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
    VK_CHECK(vkCreateImage(device_, &info, nullptr, &depthImage_));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, depthImage_, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &depthMemory_));
    VK_CHECK(vkBindImageMemory(device_, depthImage_, depthMemory_, 0));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                       = depthImage_;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &depthView_));

    // 初始 layout 转换交给 renderPass 的 loadOp=CLEAR + initialLayout=UNDEFINED，
    // 不需要额外的 barrier。
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

    uploadViaStaging(vtxData, vtxBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer_,
                     vertexBufferMemory_);
    uploadViaStaging(idxData, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer_,
                     indexBufferMemory_);
    indexCount_ = idxCount;
    indexType_  = model_.valid() ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;

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

    VkBuffer       staging       = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMemory);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &mapped));
    std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(device_, stagingMemory);

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
    VK_CHECK(vkCreateImage(device_, &imageInfo, nullptr, &textureImage_));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, textureImage_, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &textureMemory_));
    VK_CHECK(vkBindImageMemory(device_, textureImage_, textureMemory_, 0));

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
    toTransfer.srcAccessMask               = 0;
    toTransfer.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
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
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toShader);

    endOneTimeCommands(cmd);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);

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
