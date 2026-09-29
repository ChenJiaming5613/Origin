#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace origin {

struct Vertex;  // VulkanRenderer.hpp

// glTF 2.0 导入结果。
//
// 刻意只做 CPU 侧的解析与展平，**不含任何 Vulkan 调用** ——
// 上传 buffer / image 是渲染器的职责。这样这个类可以单独测试，
// 也不会因为要 include volk 而把 Vulkan 依赖扩散出去。
//
// 当前支持的子集（够渲染一个静态模型）：
//   - 默认场景的节点层级，展平成每个 primitive 一份世界变换
//   - POSITION / NORMAL / TEXCOORD_0 三个属性
//   - 三角形拓扑，索引统一转成 uint32
//   - 基础色纹理（baseColorTexture）的原始字节，交由调用方解码
//
// 尚未支持（都会在日志里明确说明，而不是静默产出错误画面）：
//   动画、蒙皮、morph target、Draco 压缩、KTX2/basisu、
//   以及除 baseColor 之外的 PBR 贴图。
class GltfModel {
public:
    // 一个可独立提交的绘制单元。
    // 顶点/索引都已经合并进下面的大数组，这里只存区间。
    struct Primitive {
        uint32_t  firstIndex  = 0;
        uint32_t  indexCount  = 0;
        uint32_t  vertexOffset = 0;  // 传给 vkCmdDrawIndexed 的 vertexOffset
        int       material    = -1;
        glm::mat4 transform{1.0f};   // 由节点层级累乘得出的世界变换
    };

    // 基础色纹理的**未解码**字节（PNG/JPEG 原样）。
    // 不在这里解码是为了不让本类依赖 stb —— 解码由渲染器用工程既有的
    // stb 实现完成，避免出现第二份 STB_IMAGE_IMPLEMENTATION。
    struct ImageBytes {
        const uint8_t* data = nullptr;  // 指向 tinygltf 的 arena，生命周期见下
        size_t         size = 0;
    };

    GltfModel() = default;
    ~GltfModel();

    GltfModel(const GltfModel&)            = delete;
    GltfModel& operator=(const GltfModel&) = delete;

    // assetPath 是相对资源根的路径（例如 "models/DamagedHelmet.glb"）。
    // 文件内容由调用方提供的 loader 读入内存 —— 这样 Android 上可以走
    // SDL_LoadFile（自动读 APK assets），而本类不需要知道平台的存在。
    //
    // 失败时抛 std::runtime_error，消息里带 tinygltf 的结构化错误栈。
    void loadFromMemory(const uint8_t* data, size_t size, std::string_view debugName);

    // ⚠️ 生命周期：baseColorImage() 返回的指针指向 tinygltf 的 arena，
    // 在本对象析构（tg3_model_free）后失效。渲染器必须在 release() 之前
    // 把纹理上传完。顶点/索引数组是我们自己的 std::vector，不受此限制。
    void release();

    const std::vector<Vertex>&    vertices() const { return vertices_; }
    const std::vector<uint32_t>&  indices() const { return indices_; }
    const std::vector<Primitive>& primitives() const { return primitives_; }

    // 第一个带 baseColorTexture 的材质对应的图像字节。
    // 没有则 data == nullptr（渲染器会退回到默认贴图）。
    const ImageBytes& baseColorImage() const { return baseColor_; }

    // 整个模型的包围盒，用于让相机自动取一个合适的初始距离 ——
    // 不做这件事的话，不同模型（几厘米的 Avocado 与几米的 Sponza）
    // 用同一个固定距离会一个看不见一个塞满屏幕。
    const glm::vec3& boundsMin() const { return boundsMin_; }
    const glm::vec3& boundsMax() const { return boundsMax_; }
    glm::vec3        boundsCenter() const { return (boundsMin_ + boundsMax_) * 0.5f; }
    float            boundsRadius() const;

    bool valid() const { return !primitives_.empty(); }

private:
    // pimpl：把 tg3_model 藏起来，避免 tiny_gltf_v3.h 进到本头文件
    // 而污染所有 include 者（它是个 4500 行的大头文件）。
    struct Impl;
    Impl* impl_ = nullptr;

    std::vector<Vertex>    vertices_;
    std::vector<uint32_t>  indices_;
    std::vector<Primitive> primitives_;
    ImageBytes             baseColor_{};

    glm::vec3 boundsMin_{0.0f};
    glm::vec3 boundsMax_{0.0f};
};

}  // namespace origin
