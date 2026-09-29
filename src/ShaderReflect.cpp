#include "ShaderReflect.hpp"

#include "Log.hpp"

#include <spirv_reflect.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace origin {
namespace {

// SPIRV-Reflect 的枚举在头文件里就注明了 "= VK_DESCRIPTOR_TYPE_*"，
// 数值一一对应，直接转换是设计意图。这里加几条静态断言，
// 万一将来某边改了值，编译期就会失败而不是运行时出怪问题。
static_assert(static_cast<int>(SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER) ==
                  static_cast<int>(VK_DESCRIPTOR_TYPE_SAMPLER),
              "SPIRV-Reflect 与 Vulkan 的 descriptor type 枚举已不一致");
static_assert(static_cast<int>(SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ==
                  static_cast<int>(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
              "SPIRV-Reflect 与 Vulkan 的 descriptor type 枚举已不一致");
static_assert(static_cast<int>(SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) ==
                  static_cast<int>(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
              "SPIRV-Reflect 与 Vulkan 的 descriptor type 枚举已不一致");
static_assert(static_cast<int>(SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) ==
                  static_cast<int>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
              "SPIRV-Reflect 与 Vulkan 的 descriptor type 枚举已不一致");

// stage 这边不用直接转换，写成显式 switch：
// 一是能在遇到还没支持的阶段时给出明确报错，
// 二是把「我们打算支持哪些阶段」这件事写在代码里。
VkShaderStageFlagBits toVkStage(SpvReflectShaderStageFlagBits s, const std::string& name) {
    switch (s) {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:   return VK_SHADER_STAGE_VERTEX_BIT;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:  return VK_SHADER_STAGE_COMPUTE_BIT;
        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT: return VK_SHADER_STAGE_TASK_BIT_EXT;
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT: return VK_SHADER_STAGE_MESH_BIT_EXT;
        default:
            throw std::runtime_error("尚未支持的着色器阶段: " + name + " (0x" +
                                     std::to_string(static_cast<uint32_t>(s)) + ")");
    }
}

const char* descriptorTypeName(VkDescriptorType t) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:                return "SAMPLER";
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "COMBINED_IMAGE_SAMPLER";
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:          return "SAMPLED_IMAGE";
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:          return "STORAGE_IMAGE";
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         return "UNIFORM_BUFFER";
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:         return "STORAGE_BUFFER";
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:       return "INPUT_ATTACHMENT";
        default:                                        return "OTHER";
    }
}

const char* stageName(VkShaderStageFlags s) {
    if (s == VK_SHADER_STAGE_VERTEX_BIT)   return "VS";
    if (s == VK_SHADER_STAGE_FRAGMENT_BIT) return "PS";
    if (s == VK_SHADER_STAGE_COMPUTE_BIT)  return "CS";
    if (s == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) return "VS|PS";
    return "MIXED";
}

}  // namespace

ShaderStage createShaderStage(VkDevice device, const std::vector<uint8_t>& spirv,
                              std::string name) {
    if (spirv.empty() || spirv.size() % 4 != 0) {
        throw std::runtime_error("SPIR-V 大小非法: " + name);
    }

    ShaderStage out;
    out.name = std::move(name);

    // ---- 反射 ---------------------------------------------------------------
    SpvReflectShaderModule mod{};
    if (spvReflectCreateShaderModule(spirv.size(), spirv.data(), &mod) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("SPIR-V 反射失败: " + out.name);
    }

    out.stage = toVkStage(mod.shader_stage, out.name);

    if (out.stage == VK_SHADER_STAGE_COMPUTE_BIT && mod.entry_point_count > 0) {
        out.localSize[0] = mod.entry_points[0].local_size.x;
        out.localSize[1] = mod.entry_points[0].local_size.y;
        out.localSize[2] = mod.entry_points[0].local_size.z;
    }

    uint32_t count = 0;
    if (spvReflectEnumerateDescriptorBindings(&mod, &count, nullptr) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&mod);
        throw std::runtime_error("枚举 descriptor binding 失败: " + out.name);
    }
    std::vector<SpvReflectDescriptorBinding*> refBindings(count);
    spvReflectEnumerateDescriptorBindings(&mod, &count, refBindings.data());

    out.bindings.reserve(count);
    for (const SpvReflectDescriptorBinding* b : refBindings) {
        // 目前只支持 set 0。多 set（bindless 纹理数组之类）等真的需要时再扩，
        // 与其悄悄忽略，不如在这里明确报错。
        if (b->set != 0) {
            spvReflectDestroyShaderModule(&mod);
            throw std::runtime_error("暂不支持 set > 0（" + out.name + " 的 " +
                                     (b->name ? b->name : "?") + " 在 set " +
                                     std::to_string(b->set) + "）");
        }

        ShaderStage::Binding sb;
        sb.set      = b->set;
        sb.binding  = b->binding;
        sb.type     = static_cast<VkDescriptorType>(b->descriptor_type);
        sb.count    = b->count;
        sb.accessed = b->accessed != 0;
        sb.name     = b->name ? b->name : "";
        out.bindings.push_back(std::move(sb));
    }

    // ---- push constant ------------------------------------------------------
    uint32_t pcCount = 0;
    spvReflectEnumeratePushConstantBlocks(&mod, &pcCount, nullptr);
    if (pcCount > 1) {
        spvReflectDestroyShaderModule(&mod);
        throw std::runtime_error("每个阶段只支持一个 push constant 块: " + out.name);
    }
    if (pcCount == 1) {
        SpvReflectBlockVariable* block = nullptr;
        spvReflectEnumeratePushConstantBlocks(&mod, &pcCount, &block);
        // 我们只支持从偏移 0 开始的单个块。offset 非 0 说明 shader 里做了
        // 分段 push constant，那会牵扯到 VkPushConstantRange 的多段管理。
        if (block->offset != 0) {
            spvReflectDestroyShaderModule(&mod);
            throw std::runtime_error("push constant 块必须从偏移 0 开始: " + out.name);
        }
        out.pushConstantSize = block->size;
    }

    spvReflectDestroyShaderModule(&mod);

    // ---- 创建 VkShaderModule ------------------------------------------------
    // pCode 要求 uint32_t 对齐，vector<uint8_t> 标准上不保证，显式拷一份
    std::vector<uint32_t> code(spirv.size() / 4);
    std::memcpy(code.data(), spirv.data(), spirv.size());

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size();
    info.pCode    = code.data();
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &out.handle));

    return out;
}

void destroyShaderStage(VkDevice device, ShaderStage& stage) {
    if (stage.handle != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, stage.handle, nullptr);
        stage.handle = VK_NULL_HANDLE;
    }
}

Program createProgram(VkDevice device, std::initializer_list<const ShaderStage*> stages) {
    Program p;

    // ---- 合并各阶段的 binding ----------------------------------------------
    // 同一个 binding 出现在多个阶段是正常的（比如 VS 和 PS 都读同一张纹理），
    // 此时类型必须一致，stageFlags 取并集 —— 手写这个 OR 极易漏，
    // 漏了的后果是校验层报 "descriptor not accessible from this stage"。
    for (const ShaderStage* s : stages) {
        for (const auto& b : s->bindings) {
            auto it = std::find_if(p.bindings.begin(), p.bindings.end(),
                                   [&](const VkDescriptorSetLayoutBinding& e) {
                                       return e.binding == b.binding;
                                   });
            if (it != p.bindings.end()) {
                if (it->descriptorType != b.type || it->descriptorCount != b.count) {
                    throw std::runtime_error(
                        "binding " + std::to_string(b.binding) +
                        " 在不同阶段的类型/数量不一致（" + s->name + " 的 " + b.name + "）");
                }
                it->stageFlags |= s->stage;
            } else {
                VkDescriptorSetLayoutBinding e{};
                e.binding         = b.binding;
                e.descriptorType  = b.type;
                e.descriptorCount = b.count;
                e.stageFlags      = s->stage;
                p.bindings.push_back(e);
            }
        }

        if (s->pushConstantSize > 0) {
            p.pushConstantStages |= s->stage;
            p.pushConstantSize = std::max(p.pushConstantSize, s->pushConstantSize);
        }
    }

    std::sort(p.bindings.begin(), p.bindings.end(),
              [](const VkDescriptorSetLayoutBinding& a, const VkDescriptorSetLayoutBinding& b) {
                  return a.binding < b.binding;
              });

    // ---- VkDescriptorSetLayout ---------------------------------------------
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<uint32_t>(p.bindings.size());
    setInfo.pBindings    = p.bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &p.setLayout));

    // ---- VkPipelineLayout ---------------------------------------------------
    VkPushConstantRange range{};
    range.stageFlags = p.pushConstantStages;
    range.offset     = 0;
    range.size       = p.pushConstantSize;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &p.setLayout;
    if (p.pushConstantSize > 0) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &range;
    }
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &p.layout));

    return p;
}

void destroyProgram(VkDevice device, Program& program) {
    if (program.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, program.layout, nullptr);
        program.layout = VK_NULL_HANDLE;
    }
    if (program.setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, program.setLayout, nullptr);
        program.setLayout = VK_NULL_HANDLE;
    }
    program.bindings.clear();
}

void logProgram(const Program& program, std::initializer_list<const ShaderStage*> stages) {
    std::string names;
    for (const ShaderStage* s : stages) {
        if (!names.empty()) {
            names += " + ";
        }
        names += s->name;
    }
    spdlog::info("[反射] {} -> push constant {} 字节 ({}), {} 个 binding", names,
                 program.pushConstantSize, stageName(program.pushConstantStages),
                 program.bindings.size());

    for (const auto& b : program.bindings) {
        // 顺带把各阶段报告的名字与 accessed 标志拼出来 —— accessed 是手写解析器
        // 拿不到的信息，能看出某个 binding 是否真被 shader 读写。
        std::string detail;
        for (const ShaderStage* s : stages) {
            for (const auto& sb : s->bindings) {
                if (sb.binding == b.binding) {
                    if (!detail.empty()) {
                        detail += ", ";
                    }
                    detail += s->name + ":" + (sb.name.empty() ? "?" : sb.name) +
                              (sb.accessed ? "" : "(未使用)");
                }
            }
        }
        spdlog::info("[反射]   binding {} = {} x{} [{}]  {}", b.binding,
                     descriptorTypeName(b.descriptorType), b.descriptorCount,
                     stageName(b.stageFlags), detail);
    }
}

}  // namespace origin
