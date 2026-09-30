#pragma once

#include "GltfModel.hpp"
#include "ShaderReflect.hpp"
#include "Vertex.hpp"
#include "camera/OrbitCameraController.h"
#include "camera/PerspectiveCamera.h"

#include <volk.h>

// VMA 的编译宏统一由 CMake 的 vma target 以 PUBLIC 传播，不在源码里 define
#include <vk_mem_alloc.h>

#include <SDL3/SDL_video.h>

// GLM_FORCE_DEPTH_ZERO_TO_ONE / GLM_FORCE_RADIANS 等宏由 CMake 的 glm target
// 统一传播（见顶层 CMakeLists）—— 不要在这里重复 define，
// 否则一旦两处写得不一致，同一个 glm::mat4 在不同翻译单元里语义就不同了。
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace origin
{

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
class OriginRenderer {
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

    // 渲染器持有相机与控制器（而不是由 main.cpp 注入）。
    //
    // 换成持有是被顺序逼出来的：createCubeMesh 里加载完 glTF 就要按包围盒
    // 调 FrameBounds 设初始距离，而那时候 main.cpp 还没机会创建控制器。
    // 旧版把控制器放在 main.cpp、用 setCameraController 弱引用注入，
    // 于是"设初始距离"只能塞进 Camera（让相机类背上了轨道参数）。
    // 现在相机与控制器的生命周期都归渲染器，main.cpp 只负责转发事件。
    //
    // 返回基类引用：需要 vp 矩阵的地方不该关心是透视还是正交。
    Camera&       camera() { return camera_; }
    const Camera& camera() const { return camera_; }

    // 事件转发与面板调参都通过它。可能为 nullptr（init 之前）。
    OrbitCameraController* cameraController() { return cameraController_.get(); }

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createDepthResources();
    void createRenderPass();
    void createFramebuffers();
    void createCubeMesh();
    void createTexture();
    void createPipeline();
    void createDescriptorSet();
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
    void destroyFramebuffers();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);

    std::vector<uint8_t> readAsset(std::string_view path);

    // ---- 底层资源辅助（内存交给 VMA）----------------------------------------
    // 换掉裸 vkAllocateMemory 的实际理由不是"少写几行"，而是：
    //   1. 每个资源一次 vkAllocateMemory 会很快撞上 maxMemoryAllocationCount
    //      （移动端常见 4096），VMA 会把小分配合并进大块内存里做 suballocation；
    //   2. 内存类型的选择从"手写 propertyFlags"变成声明用途（VmaMemoryUsage），
    //      由 VMA 按设备实际的 memory heap 布局去挑 —— 桌面独显与移动端
    //      统一内存架构的最优选择本来就不同，手写必然要写平台分支。
    void createAllocator();
    VkFormat findDepthFormat() const;

    // 缓冲与它的 VMA 分配句柄成对出现。
    // VmaAllocation 取代了原先的 VkDeviceMemory —— 它不是"一块显存"，
    // 而是"某块大显存里的一段"，所以不能再对它调 vkFreeMemory。
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaAllocationCreateFlags allocFlags, VkBuffer& buffer,
                      VmaAllocation& allocation) const;
    void uploadViaStaging(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer& buffer, VmaAllocation& allocation);

    VkCommandBuffer beginOneTimeCommands();
    void            endOneTimeCommands(VkCommandBuffer cmd);

    // 窗口由 SDL 持有，本类不负责释放
    SDL_Window* window_ = nullptr;

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;

    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_   = VK_NULL_HANDLE;
    uint32_t         queueFamilyIndex_ = 0;
    VkDevice         device_           = VK_NULL_HANDLE;
    VkQueue          queue_            = VK_NULL_HANDLE;

    // ---- 可选能力 -----------------------------------------------------------
    //
    // 在 pickPhysicalDevice 里探测、createDevice 里启用。两者都是 Vulkan 1.2
    // 核心化的特性，但在 1.1 基线下**以扩展形式**可用 —— vk.xml 里它们的
    // depends 都写着 `..., VK_VERSION_1_1`，即 1.1 单独就满足依赖
    // （前置的 get_physical_device_properties2 / maintenance3 / device_group
    //   都已在 1.1 核心化）。
    //
    // 关键约束：因为 VkApplicationInfo::apiVersion 是 1.1，**必须走带 KHR/EXT
    // 后缀的入口**（vkGetBufferDeviceAddressKHR），不能用 1.2 的核心名
    // （vkGetBufferDeviceAddress）—— 后者在 apiVersion < 1.2 时是未定义行为，
    // volk 也不会为它填函数指针。这一点在所有设备上一致，包括报 1.4 的 RTX 3080，
    // 所以仍然是单一代码路径。
    //
    // 这两项是后续 bindless / GPU-driven 的地基：descriptorIndexing 管
    // 「一个大贴图数组 + 运行期算出的下标」，bufferDeviceAddress 管
    // 「把 buffer 当裸指针传进 shader」。
    struct OptionalCaps {
        bool descriptorIndexing  = false;  // VK_EXT_descriptor_indexing
        bool bufferDeviceAddress = false;  // VK_KHR_buffer_device_address
    };
    OptionalCaps caps_{};

    VkSwapchainKHR                swapchain_       = VK_NULL_HANDLE;
    VkFormat                      swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                    swapchainExtent_{};
    VkSurfaceTransformFlagBitsKHR swapchainTransform_ =
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage>     swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    // 深度缓冲跟着 swapchain 尺寸走，重建 swapchain 时必须一起重建
    VkFormat      depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage       depthImage_  = VK_NULL_HANDLE;
    VmaAllocation depthAlloc_  = VK_NULL_HANDLE;
    VkImageView   depthView_   = VK_NULL_HANDLE;

    // ---- render pass / framebuffer -----------------------------------------
    //
    // 工程基线是 Vulkan 1.1，所以走传统的 VkRenderPass 而非 dynamic rendering。
    //
    // 这个选择是被真机逼出来的、不是偏好：实测一加 Ace 竞速版（天玑 8100 /
    // Mali-G610，Android 15 但 GPU 驱动还是 2021 年的 r32p1）只报 Vulkan
    // 1.1.177，且 VK_KHR_dynamic_rendering 与 VK_KHR_synchronization2
    // **连扩展形式都没有**。而该机的 bindless（descriptor indexing 全绿、
    // 50 万 sampled image 上限）与 GPU-driven（multiDrawIndirect +
    // drawIndirectCount + shaderDrawParameters）能力齐全 —— 也就是说
    // dynamic rendering 只是写法糖，挡住的却是整台真机的实验能力。
    //
    // renderPass_ 只依赖**附件格式**（颜色格式 + 深度格式），跨 swapchain
    // 重建不变，所以和 pipeline 一样只建一次。
    // framebuffers_ 绑定具体的 image view 与尺寸，必须随 swapchain 重建 ——
    // 这正是 dynamic rendering 想省掉的那部分样板。
    VkRenderPass               renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // pipelineLayout 与 descriptorSetLayout 都在 program_ 里，由反射生成
    Program    program_{};
    ShaderStage vertexStage_{};
    ShaderStage pixelStage_{};
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer      vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_  = VK_NULL_HANDLE;
    VkBuffer      indexBuffer_  = VK_NULL_HANDLE;
    VmaAllocation indexAlloc_   = VK_NULL_HANDLE;
    uint32_t       indexCount_         = 0;
    // glTF 导入统一用 uint32；只有退回内置 cube 时才是 uint16
    VkIndexType    indexType_          = VK_INDEX_TYPE_UINT16;

    VkImage       textureImage_ = VK_NULL_HANDLE;
    VmaAllocation textureAlloc_ = VK_NULL_HANDLE;
    VkImageView   textureView_  = VK_NULL_HANDLE;
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

    // 具体类型是 PerspectiveCamera；对外只暴露 Camera&。
    // 将来要支持运行时切正交相机，把它换成 std::unique_ptr<Camera> 即可 ——
    // 使用侧（camera().ViewProj() / SetAspect）不用改，因为那些都在基类上。
    PerspectiveCamera camera_;

    // 控制器持有 camera_ 的引用，所以必须在它之后声明（销毁顺序相反）。
    // 用 unique_ptr 而非直接持有：构造时要传 SDL_Window*，而那要等 init()。
    std::unique_ptr<OrbitCameraController> cameraController_;

    // 启动时加载的 glTF 模型。加载失败会退回内置 cube（valid() == false），
    // 这样链路依然可见，而不是黑屏。
    GltfModel model_;
    // 模型包围盒中心。自转前先把它搬到原点，否则不以原点为中心的模型
    // （很多 glTF 资产如此）会绕原点公转而不是自转。
    glm::vec3 modelCenter_{0.0f};

    // 屏幕密度缩放系数（桌面 1.0，手机常见 2.5~3.5）。
    // initImGui 里从 SDL_GetWindowDisplayScale() 取得，buildUi 里手写的
    // 绝对像素值都要乘它。
    float uiDpiScale_ = 1.0f;

    // 截图：目标缓冲跟 swapchain 尺寸绑定，重建 swapchain 时一起销毁
    std::string    screenshotPath_;
    bool           screenshotPending_ = false;
    VkBuffer      screenshotBuffer_ = VK_NULL_HANDLE;
    VmaAllocation screenshotAlloc_  = VK_NULL_HANDLE;
    VkDeviceSize  screenshotSize_   = 0;
};

}  // namespace origin
