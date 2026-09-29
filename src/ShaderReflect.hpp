#pragma once

#include <volk.h>

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

// SPIR-V 反射驱动的管线布局生成。
//
// 目标：让 shader 成为资源布局的**唯一真源**。
// C++ 侧不再手写 VkDescriptorSetLayoutBinding 数组、不再手填 VkPushConstantRange，
// 全部从编译好的 SPIR-V 反推出来。改 HLSL 里的 binding 号或 push constant 结构，
// C++ 不需要跟着改任何一行；对不上的时候在启动期就报错，而不是等校验层在某帧报警。
//
// 底层用 KhronosGroup/SPIRV-Reflect（third_party/SPIRV-Reflect）。
// 选它而不是 spirv-cross 的原因见 README：我们只需要反射不需要反编译，
// 而且它对 HLSL 的资源类型映射是明确支持过的 —— cbuffer 与 StructuredBuffer
// 在 SPIR-V 里同为 OpTypeStruct，只有 StorageClass 不同，
// 手写解析器很容易把 cbuffer 误判成 storage buffer。

namespace origin {

// 单个着色器阶段：SPIR-V 模块 + 从中反射出的接口信息
struct ShaderStage {
    VkShaderModule        handle = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage{};
    std::string           name;  // 仅用于日志与报错

    struct Binding {
        uint32_t         set     = 0;
        uint32_t         binding = 0;
        VkDescriptorType type{};
        uint32_t         count = 1;  // 数组长度；runtime array 会是 0
        bool             accessed = false;  // shader 里是否真的读写了它
        std::string      name;
    };
    std::vector<Binding> bindings;

    // push constant 块的总字节数（0 表示该阶段不用 push constant）
    uint32_t pushConstantSize = 0;

    // 仅 compute 有意义；用于把 dispatch 的「线程数」换算成「工作组数」
    uint32_t localSize[3] = {1, 1, 1};
};

// 一组阶段合并后的产物。setLayout / layout 完全由反射生成。
struct Program {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout      layout    = VK_NULL_HANDLE;

    VkShaderStageFlags pushConstantStages = 0;
    uint32_t           pushConstantSize   = 0;

    // 合并后的 binding 表（已按 binding 号排序），保留下来便于分配描述符池
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

// 从 SPIR-V 字节流创建阶段并完成反射。spirv 长度必须是 4 的倍数。
ShaderStage createShaderStage(VkDevice device, const std::vector<uint8_t>& spirv,
                              std::string name);
void        destroyShaderStage(VkDevice device, ShaderStage& stage);

// 合并多个阶段：同一 binding 出现在多个阶段时会校验类型一致并 OR 其 stageFlags。
// pushConstantSize 取各阶段的最大值，stages 为用到它的阶段的并集。
Program createProgram(VkDevice device, std::initializer_list<const ShaderStage*> stages);
void    destroyProgram(VkDevice device, Program& program);

// 把反射结果打到日志，便于和 HLSL 声明肉眼对照
void logProgram(const Program& program, std::initializer_list<const ShaderStage*> stages);

// push constant 结构体一致性校验。
//
// 这是反射真正能挡住的一类 bug：C++ 结构体和 HLSL 的 push constant 块
// 一旦大小不一致，在启动时就抛异常，而不是渲染出鬼再去猜。
// 注意它只能校验**总大小**，字段顺序错了但大小相同仍然查不出来 ——
// 那需要逐字段比对 offset，SPIRV-Reflect 提供了 members[] 可以做，
// 目前规模还不值得。
template <typename T>
void checkPushConstantSize(const Program& program, const char* what) {
    if (program.pushConstantSize != sizeof(T)) {
        throw std::runtime_error(std::string("push constant 大小不匹配 (") + what +
                                 "): shader 反射出 " +
                                 std::to_string(program.pushConstantSize) + " 字节, C++ 是 " +
                                 std::to_string(sizeof(T)) + " 字节");
    }
}

}  // namespace origin
