#pragma once

#include "ShaderReflect.hpp"

#include <volk.h>

#include <SDL3/SDL_video.h>

// GLM_FORCE_DEPTH_ZERO_TO_ONE / GLM_FORCE_RADIANS 等宏由 CMake 的 glm target
// 统一传播（见顶层 CMakeLists）—— 不要在这里重复 define，
// 否则一旦两处写得不一致，同一个 glm::mat4 在不同翻译单元里语义就不同了。
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace origin {

// 顶点格式。顶点输入描述是**手写**的，没有走反射：
// SPIRV-Reflect 能反射出 input variable 的 location 和类型，但推不出
// 我们想怎么打包（interleaved 还是分流、是否压缩法线），
// 所以顶点布局留在 C++ 侧显式声明，descriptor 布局才交给反射。
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

// 必须和 shaders/cube.vs.hlsl 里的 PushConstants 严格一致。
// 这个一致性由 ShaderReflect 的 checkPushConstantSize 在启动期校验 ——
// 改了 HLSL 里的结构而忘了改这里，启动就会抛异常并打出两边的字节数。
//
// glm::mat4 是列主序，正好是 shader 期望的内存布局，**不需要转置**。
// （早期自建的行主序 Mat4 需要在上传前 transpose 一次，换 glm 后这步消失了。
//   原因见 cube.vs.hlsl 顶部关于 OpVectorTimesMatrix + RowMajor 的推导。）
struct CubePushConstants {
    glm::mat4 mvp;          // 64 字节
    glm::vec4 lightDirObj;  // 16 字节；xyz = 物体空间光源方向，w = 环境光强度
    glm::vec4 lightColor;   // 16 字节；rgb = 颜色，a = 强度
};                          // 96 字节

// 可由 ImGui 面板实时调节的光照参数。
//
// 方向用方位角/仰角两个标量而不是裸的 xyz：滑条拖起来直观得多，
// 且天然保证是单位向量，不会出现拖成零向量导致 normalize 出 NaN 的情况。
struct LightingParams {
    float azimuthDeg   = 35.0f;   // 绕 Y 轴，0 = +Z 方向
    float elevationDeg = 40.0f;   // 抬升角，90 = 正上方
    float intensity    = 1.6f;
    float ambient      = 0.20f;
    float color[3]     = {1.0f, 0.96f, 0.90f};

    float rotateSpeed = 0.9f;  // cube 自转速度（弧度/秒）
    bool  animate     = true;
};

// 带纹理与深度测试的旋转 cube。
//
// 本文件没有任何平台头文件：surface 交给 SDL_Vulkan_CreateSurface，
// 资源读取交给 SDL_LoadFile（Android 上自动走 APK assets），
// 因此同一份代码在 Windows 与 Android 上都能直接用。
class VulkanRenderer {
public:
    void init(SDL_Window* window);
    void shutdown();

    bool ready() const { return device_ != VK_NULL_HANDLE; }

    void drawFrame();

    // 窗口尺寸变化时由外部通知（转屏、分屏、桌面端拖拽窗口）
    void notifyResize() { framebufferDirty_ = true; }

    // 把下一帧的画面存成 PNG。
    // copy 命令会并进当帧的 command buffer，所以不会产生
    // 「转换未 acquire 的 presentable image」那类校验层报错。
    void requestScreenshot(const char* path);

    LightingParams&       lighting() { return lighting_; }
    const LightingParams& lighting() const { return lighting_; }

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createDepthResources();
    void createRenderPass();
    void createCubeMesh();
    void createTexture();
    void createPipeline();
    void createDescriptorSet();
    void createFramebuffers();
    void createCommandBuffers();
    void createSyncObjects();

    void ensureScreenshotBuffer();
    void writePendingScreenshot();

    void initImGui();
    void shutdownImGui();
    void buildUi();  // 每帧构建调试面板

    void recreateSwapchain();
    void destroySwapchainDependents();
    void destroyDepthResources();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);

    std::vector<uint8_t> readAsset(std::string_view path);

    // ---- 底层资源辅助（没有引入 VMA，先用裸 vkAllocateMemory） --------------
    // 单个 cube 只需要 4 次分配（顶点/索引/纹理/深度），显式写出来更易读。
    // 等资源多起来（每个 mesh 一次分配会很快撞上 maxMemoryAllocationCount）
    // 再换 VMA 或自建 suballocator。
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    VkFormat findDepthFormat() const;

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer& buffer,
                      VkDeviceMemory& memory) const;
    void uploadViaStaging(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer& buffer, VkDeviceMemory& memory);

    VkCommandBuffer beginOneTimeCommands();
    void            endOneTimeCommands(VkCommandBuffer cmd);

    // 窗口由 SDL 持有，本类不负责释放
    SDL_Window* window_ = nullptr;

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_   = VK_NULL_HANDLE;
    uint32_t         queueFamilyIndex_ = 0;
    VkDevice         device_           = VK_NULL_HANDLE;
    VkQueue          queue_            = VK_NULL_HANDLE;

    VkSwapchainKHR                swapchain_       = VK_NULL_HANDLE;
    VkFormat                      swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                    swapchainExtent_{};
    VkSurfaceTransformFlagBitsKHR swapchainTransform_ =
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage>     swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    // 深度缓冲跟着 swapchain 尺寸走，重建 swapchain 时必须一起重建
    VkFormat       depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage        depthImage_  = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView    depthView_   = VK_NULL_HANDLE;

    VkRenderPass               renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // pipelineLayout 与 descriptorSetLayout 都在 program_ 里，由反射生成
    Program    program_{};
    ShaderStage vertexStage_{};
    ShaderStage pixelStage_{};
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer       vertexBuffer_       = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer       indexBuffer_        = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory_  = VK_NULL_HANDLE;
    uint32_t       indexCount_         = 0;

    VkImage        textureImage_  = VK_NULL_HANDLE;
    VkDeviceMemory textureMemory_ = VK_NULL_HANDLE;
    VkImageView    textureView_   = VK_NULL_HANDLE;
    VkSampler      textureSampler_ = VK_NULL_HANDLE;
    uint32_t       textureMipLevels_ = 1;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet  descriptorSet_  = VK_NULL_HANDLE;

    VkCommandPool                commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    static constexpr uint32_t kMaxFramesInFlight = 2;
    std::vector<VkSemaphore>  imageAvailableSemaphores_;
    std::vector<VkFence>      inFlightFences_;

    // renderFinished 必须按「swapchain image」分配，而不是按帧。
    //
    // 这是经典 hello-triangle 实现里的一个真实缺陷：
    // 按帧分配时，若 swapchain image 数量与 frames-in-flight 不相等，
    // vkQueuePresentKHR 可能等待一个已被后续帧重新提交的信号量，
    // 新版校验层会报同步错误，部分移动 GPU 上还会真的卡住。
    std::vector<VkSemaphore> renderFinishedSemaphores_;

    uint32_t currentFrame_     = 0;
    bool     framebufferDirty_ = false;

    // 旋转角按帧累加而不是用绝对时间：这样面板上暂停/改速度时角度是连续的，
    // 用绝对时间的话一改速度就会跳变。
    uint64_t lastTicks_    = 0;
    float    rotationRad_  = 0.0f;
    float    frameMs_      = 0.0f;  // 面板上显示的帧耗时

    LightingParams lighting_{};

    bool imguiReady_ = false;

    // 屏幕密度缩放系数（桌面 1.0，手机常见 2.5~3.5）。
    // initImGui 里从 SDL_GetWindowDisplayScale() 取得，buildUi 里手写的
    // 绝对像素值都要乘它。
    float uiDpiScale_ = 1.0f;

    // 截图：目标缓冲跟 swapchain 尺寸绑定，重建 swapchain 时一起销毁
    std::string    screenshotPath_;
    bool           screenshotPending_ = false;
    VkBuffer       screenshotBuffer_  = VK_NULL_HANDLE;
    VkDeviceMemory screenshotMemory_  = VK_NULL_HANDLE;
    VkDeviceSize   screenshotSize_    = 0;
};

}  // namespace origin
