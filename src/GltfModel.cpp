#include "GltfModel.hpp"

#include "Log.hpp"
#include "Vertex.hpp"

#include <tiny_gltf_v3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace origin {

struct GltfModel::Impl {
    tg3_model       model{};
    tg3_error_stack errors{};
    bool            modelValid  = false;
    bool            errorsValid = false;
};

namespace {

// tg3_str 不是以 \0 结尾的，比较时必须带长度。
bool strEquals(const tg3_str& s, const char* literal) {
    const size_t n = std::strlen(literal);
    return s.len == n && s.data != nullptr && std::strncmp(s.data, literal, n) == 0;
}

std::string toStdString(const tg3_str& s) {
    return (s.data != nullptr) ? std::string(s.data, s.len) : std::string();
}

int findAttribute(const tg3_primitive& prim, const char* name) {
    for (uint32_t i = 0; i < prim.attributes_count; ++i) {
        if (strEquals(prim.attributes[i].key, name)) {
            return prim.attributes[i].value;
        }
    }
    return -1;
}

int componentCount(int32_t tg3Type) {
    switch (tg3Type) {
        case TG3_TYPE_SCALAR: return 1;
        case TG3_TYPE_VEC2:   return 2;
        case TG3_TYPE_VEC3:   return 3;
        case TG3_TYPE_VEC4:   return 4;
        default:              return 0;  // 矩阵类型这里用不到
    }
}

size_t componentSize(int32_t componentType) {
    switch (componentType) {
        case TG3_COMPONENT_TYPE_BYTE:
        case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:  return 1;
        case TG3_COMPONENT_TYPE_SHORT:
        case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TG3_COMPONENT_TYPE_INT:
        case TG3_COMPONENT_TYPE_UNSIGNED_INT:
        case TG3_COMPONENT_TYPE_FLOAT:          return 4;
        case TG3_COMPONENT_TYPE_DOUBLE:         return 8;
        default:                                return 0;
    }
}

// 定位一个 accessor 的数据起点与跨步。
//
// 这是 glTF 导入里最容易写错的一处：顶点属性可以是**交错存放**的
// （多个属性共享一个 bufferView，各自用 byteOffset + byteStride 区分），
// 所以绝不能假设数据是紧密排列的、直接按 index * elementSize 去索引。
// byteStride == 0 才表示紧密排列，此时步长等于单个元素的大小。
struct AccessorView {
    const uint8_t* base   = nullptr;
    size_t         stride = 0;
    size_t         count  = 0;
    int32_t        componentType = 0;
    int            comps  = 0;
    bool           normalized = false;

    bool valid() const { return base != nullptr && comps > 0 && count > 0; }
};

AccessorView makeAccessorView(const tg3_model& m, int accessorIndex) {
    AccessorView v{};
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(m.accessors_count)) {
        return v;
    }
    const tg3_accessor& acc = m.accessors[accessorIndex];
    if (acc.buffer_view < 0 || acc.buffer_view >= static_cast<int>(m.buffer_views_count)) {
        return v;  // 稀疏 accessor 或纯 sparse 覆盖，本子集不支持
    }
    const tg3_buffer_view& bv = m.buffer_views[acc.buffer_view];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(m.buffers_count)) {
        return v;
    }
    const tg3_buffer& buf = m.buffers[bv.buffer];
    if (buf.data.data == nullptr) {
        return v;  // 外部 .bin 未加载（我们没开 FS，.glb 不会走到这里）
    }

    v.comps         = componentCount(acc.type);
    v.componentType = acc.component_type;
    const size_t elemSize = componentSize(acc.component_type) * static_cast<size_t>(v.comps);
    if (elemSize == 0) {
        return AccessorView{};
    }

    v.stride     = (bv.byte_stride != 0) ? bv.byte_stride : elemSize;
    v.count      = static_cast<size_t>(acc.count);
    v.normalized = (acc.normalized != 0);

    const size_t offset = static_cast<size_t>(bv.byte_offset) +
                          static_cast<size_t>(acc.byte_offset);

    // 越界检查。tinygltf 的 validate_indices 只校验索引字段的范围，
    // 不保证 offset + stride*count 落在 buffer 内，所以这里自己兜一道 ——
    // 畸形文件会导致越界读，宁可放弃这个属性也不要读野内存。
    const size_t needed = (v.count > 0) ? offset + v.stride * (v.count - 1) + elemSize : offset;
    if (needed > buf.data.count) {
        spdlog::warn("[glTF] accessor {} 越界（需要 {} 字节，buffer 只有 {}），已跳过",
                     accessorIndex, needed, buf.data.count);
        return AccessorView{};
    }

    v.base = buf.data.data + offset;
    return v;
}

// 把第 i 个元素读成最多 4 个 float。
// 归一化整型按 glTF 规范换算到 [0,1] / [-1,1]。
void readAsFloat(const AccessorView& v, size_t i, float* out, int outComps) {
    for (int c = 0; c < outComps; ++c) {
        out[c] = 0.0f;
    }
    const uint8_t* p = v.base + v.stride * i;
    const int      n = std::min(outComps, v.comps);

    for (int c = 0; c < n; ++c) {
        switch (v.componentType) {
            case TG3_COMPONENT_TYPE_FLOAT: {
                float tmp;
                std::memcpy(&tmp, p + c * 4, 4);
                out[c] = tmp;
                break;
            }
            case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: {
                const uint8_t tmp = p[c];
                out[c] = v.normalized ? static_cast<float>(tmp) / 255.0f
                                      : static_cast<float>(tmp);
                break;
            }
            case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: {
                uint16_t tmp;
                std::memcpy(&tmp, p + c * 2, 2);
                out[c] = v.normalized ? static_cast<float>(tmp) / 65535.0f
                                      : static_cast<float>(tmp);
                break;
            }
            case TG3_COMPONENT_TYPE_BYTE: {
                int8_t tmp;
                std::memcpy(&tmp, p + c, 1);
                out[c] = v.normalized ? std::max(static_cast<float>(tmp) / 127.0f, -1.0f)
                                      : static_cast<float>(tmp);
                break;
            }
            case TG3_COMPONENT_TYPE_SHORT: {
                int16_t tmp;
                std::memcpy(&tmp, p + c * 2, 2);
                out[c] = v.normalized ? std::max(static_cast<float>(tmp) / 32767.0f, -1.0f)
                                      : static_cast<float>(tmp);
                break;
            }
            default:
                break;
        }
    }
}

uint32_t readIndex(const AccessorView& v, size_t i) {
    const uint8_t* p = v.base + v.stride * i;
    switch (v.componentType) {
        case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *p;
        case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t tmp;
            std::memcpy(&tmp, p, 2);
            return tmp;
        }
        case TG3_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t tmp;
            std::memcpy(&tmp, p, 4);
            return tmp;
        }
        default:
            return 0;
    }
}

glm::mat4 nodeLocalTransform(const tg3_node& n) {
    if (n.has_matrix != 0) {
        // glTF 的 matrix 是列主序，和 glm::mat4 的内存布局一致，可以直接喂。
        float f[16];
        for (int i = 0; i < 16; ++i) {
            f[i] = static_cast<float>(n.matrix[i]);
        }
        return glm::make_mat4(f);
    }
    const glm::vec3 t(static_cast<float>(n.translation[0]),
                      static_cast<float>(n.translation[1]),
                      static_cast<float>(n.translation[2]));
    // 注意四元数分量顺序：glTF 是 [x, y, z, w]，而 glm::quat 的构造函数
    // 是 (w, x, y, z)。照下标顺序传会得到一个完全错误的旋转。
    const glm::quat r(static_cast<float>(n.rotation[3]), static_cast<float>(n.rotation[0]),
                      static_cast<float>(n.rotation[1]), static_cast<float>(n.rotation[2]));
    const glm::vec3 s(static_cast<float>(n.scale[0]), static_cast<float>(n.scale[1]),
                      static_cast<float>(n.scale[2]));

    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) *
           glm::scale(glm::mat4(1.0f), s);
}

}  // namespace

GltfModel::~GltfModel() {
    release();
}

void GltfModel::release() {
    if (impl_ != nullptr) {
        if (impl_->modelValid) {
            tg3_model_free(&impl_->model);
        }
        if (impl_->errorsValid) {
            tg3_error_stack_free(&impl_->errors);
        }
        delete impl_;
        impl_ = nullptr;
    }
    baseColor_ = ImageBytes{};  // 指向 arena，已随之失效
}

float GltfModel::boundsRadius() const {
    return glm::length(boundsMax_ - boundsMin_) * 0.5f;
}

void GltfModel::loadFromMemory(const uint8_t* data, size_t size, std::string_view debugName) {
    release();
    vertices_.clear();
    indices_.clear();
    primitives_.clear();

    impl_ = new Impl();

    tg3_parse_options opts{};
    tg3_parse_options_init(&opts);
    // 图像不解码：我们要的是原始 PNG/JPEG 字节，用工程既有的 stb 实现去解，
    // 免得引入第二份 STB_IMAGE_IMPLEMENTATION。
    opts.images_as_is = 1;
    // 索引越界校验保持默认开启（默认就是 1，这里写出来是为了表明这是刻意选择）。
    opts.validate_indices = 1;

    tg3_error_stack_init(&impl_->errors);
    impl_->errorsValid = true;

    // parse_auto 会自己识别 JSON 还是 GLB，所以 .gltf 和 .glb 都能喂进来。
    // base_dir 传空：我们没开 FS 回调，外部 .bin / 外部贴图都读不到，
    // 因此只支持自包含的 .glb（或 data: URI 内嵌的 .gltf）。
    const tg3_error_code rc =
        tg3_parse_auto(&impl_->model, &impl_->errors, data, static_cast<uint64_t>(size),
                       nullptr, 0, &opts);

    if (rc != TG3_OK) {
        std::string msg = "解析 glTF 失败: " + std::string(debugName);
        for (uint32_t i = 0; i < impl_->errors.count; ++i) {
            const auto& e = impl_->errors.entries[i];
            msg += "\n  [";
            msg += std::to_string(static_cast<int>(e.severity));
            msg += "] ";
            msg += (e.message != nullptr) ? e.message : "(null)";
        }
        release();
        throw std::runtime_error(msg);
    }
    impl_->modelValid = true;

    const tg3_model& m = impl_->model;

    // ---- 展平节点层级 -------------------------------------------------------
    // 递归用显式栈而不是函数递归：glTF 的节点层级深度由文件决定，
    // 恶意/异常文件可以做出几万层嵌套把调用栈爆掉。
    struct StackEntry {
        int32_t   node;
        glm::mat4 parent;
    };
    std::vector<StackEntry> stack;

    const int sceneIndex =
        (m.default_scene >= 0 && m.default_scene < static_cast<int>(m.scenes_count))
            ? m.default_scene
            : (m.scenes_count > 0 ? 0 : -1);

    if (sceneIndex >= 0) {
        const tg3_scene& scene = m.scenes[sceneIndex];
        for (uint32_t i = 0; i < scene.nodes_count; ++i) {
            stack.push_back({scene.nodes[i], glm::mat4(1.0f)});
        }
    } else {
        // 没有 scene 的文件（规范允许）：退化成遍历所有 mesh
        spdlog::warn("[glTF] {} 没有 scene，退化为遍历全部 node", debugName);
        for (uint32_t i = 0; i < m.nodes_count; ++i) {
            stack.push_back({static_cast<int32_t>(i), glm::mat4(1.0f)});
        }
    }

    boundsMin_ = glm::vec3(std::numeric_limits<float>::max());
    boundsMax_ = glm::vec3(std::numeric_limits<float>::lowest());

    size_t skippedNonTriangle = 0;
    size_t skippedNoPosition  = 0;

    while (!stack.empty()) {
        const StackEntry entry = stack.back();
        stack.pop_back();

        if (entry.node < 0 || entry.node >= static_cast<int32_t>(m.nodes_count)) {
            continue;
        }
        const tg3_node& node  = m.nodes[entry.node];
        const glm::mat4 world = entry.parent * nodeLocalTransform(node);

        for (uint32_t c = 0; c < node.children_count; ++c) {
            stack.push_back({node.children[c], world});
        }

        if (node.mesh < 0 || node.mesh >= static_cast<int32_t>(m.meshes_count)) {
            continue;
        }
        const tg3_mesh& mesh = m.meshes[node.mesh];

        for (uint32_t p = 0; p < mesh.primitives_count; ++p) {
            const tg3_primitive& prim = mesh.primitives[p];

            // mode == -1 表示未指定，按规范默认就是 TRIANGLES
            if (prim.mode != -1 && prim.mode != TG3_MODE_TRIANGLES) {
                ++skippedNonTriangle;
                continue;
            }

            const AccessorView pos = makeAccessorView(m, findAttribute(prim, "POSITION"));
            if (!pos.valid()) {
                ++skippedNoPosition;
                continue;
            }
            const AccessorView nrm = makeAccessorView(m, findAttribute(prim, "NORMAL"));
            const AccessorView uv0 = makeAccessorView(m, findAttribute(prim, "TEXCOORD_0"));

            const uint32_t vertexBase = static_cast<uint32_t>(vertices_.size());
            const uint32_t firstIndex = static_cast<uint32_t>(indices_.size());

            for (size_t i = 0; i < pos.count; ++i) {
                Vertex v{};
                readAsFloat(pos, i, v.position, 3);

                if (nrm.valid() && i < nrm.count) {
                    readAsFloat(nrm, i, v.normal, 3);
                } else {
                    // 缺法线时给个朝上的默认值。规范说此时应按面法线自行计算，
                    // 但那需要先建好索引拓扑；对本子集来说不值得，
                    // 而留 {0,0,0} 会让 shader 里的 normalize 出 NaN。
                    v.normal[0] = 0.0f;
                    v.normal[1] = 1.0f;
                    v.normal[2] = 0.0f;
                }

                if (uv0.valid() && i < uv0.count) {
                    readAsFloat(uv0, i, v.uv, 2);
                }

                // 包围盒在**世界空间**里算：节点可能带缩放，
                // 用局部坐标算出来的半径会和实际显示大小不符。
                const glm::vec3 wp =
                    glm::vec3(world * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
                boundsMin_ = glm::min(boundsMin_, wp);
                boundsMax_ = glm::max(boundsMax_, wp);

                vertices_.push_back(v);
            }

            const AccessorView idx = makeAccessorView(m, prim.indices);
            if (idx.valid()) {
                indices_.reserve(indices_.size() + idx.count);
                for (size_t i = 0; i < idx.count; ++i) {
                    indices_.push_back(readIndex(idx, i));
                }
            } else {
                // 没有索引缓冲：按顶点顺序生成。规范允许 primitive 无 indices。
                indices_.reserve(indices_.size() + pos.count);
                for (size_t i = 0; i < pos.count; ++i) {
                    indices_.push_back(static_cast<uint32_t>(i));
                }
            }

            Primitive out{};
            out.firstIndex   = firstIndex;
            out.indexCount   = static_cast<uint32_t>(indices_.size() - firstIndex);
            out.vertexOffset = vertexBase;
            out.material     = prim.material;
            out.transform    = world;
            primitives_.push_back(out);
        }
    }

    if (primitives_.empty()) {
        release();
        throw std::runtime_error("glTF 中没有可渲染的三角形 primitive: " +
                                 std::string(debugName));
    }

    // ---- 基础色纹理的原始字节 ------------------------------------------------
    for (const Primitive& prim : primitives_) {
        if (prim.material < 0 || prim.material >= static_cast<int>(m.materials_count)) {
            continue;
        }
        const int texIndex = m.materials[prim.material].pbr_metallic_roughness
                                 .base_color_texture.index;
        if (texIndex < 0 || texIndex >= static_cast<int>(m.textures_count)) {
            continue;
        }
        const int imgIndex = m.textures[texIndex].source;
        if (imgIndex < 0 || imgIndex >= static_cast<int>(m.images_count)) {
            continue;
        }
        const tg3_image& img = m.images[imgIndex];

        // images_as_is 下 tinygltf 不解码，也不会填 img.image，
        // 所以自己从 bufferView 里切出那段压缩字节。
        if (img.buffer_view >= 0 && img.buffer_view < static_cast<int>(m.buffer_views_count)) {
            const tg3_buffer_view& bv = m.buffer_views[img.buffer_view];
            if (bv.buffer >= 0 && bv.buffer < static_cast<int>(m.buffers_count)) {
                const tg3_buffer& buf = m.buffers[bv.buffer];
                if (buf.data.data != nullptr &&
                    bv.byte_offset + bv.byte_length <= buf.data.count) {
                    baseColor_.data = buf.data.data + bv.byte_offset;
                    baseColor_.size = static_cast<size_t>(bv.byte_length);
                }
            }
        } else if (img.image.data != nullptr && img.image.count > 0) {
            // data: URI 内嵌的情况，tinygltf 会把解码前的字节放这里
            baseColor_.data = img.image.data;
            baseColor_.size = static_cast<size_t>(img.image.count);
        } else if (img.uri.len > 0) {
            spdlog::warn("[glTF] 基础色贴图是外部文件 '{}'，本子集只支持自包含的 .glb，"
                         "将使用默认贴图",
                         toStdString(img.uri));
        }
        if (baseColor_.data != nullptr) {
            break;
        }
    }

    spdlog::info("[glTF] {} 载入完成: {} primitive, {} 顶点, {} 索引, 贴图 {} 字节",
                 debugName, primitives_.size(), vertices_.size(), indices_.size(),
                 baseColor_.size);
    spdlog::info("[glTF]   包围盒 min=({:.3f},{:.3f},{:.3f}) max=({:.3f},{:.3f},{:.3f}) "
                 "半径={:.3f}",
                 boundsMin_.x, boundsMin_.y, boundsMin_.z, boundsMax_.x, boundsMax_.y,
                 boundsMax_.z, boundsRadius());

    if (skippedNonTriangle > 0) {
        spdlog::warn("[glTF]   跳过 {} 个非三角形 primitive（点/线拓扑未支持）",
                     skippedNonTriangle);
    }
    if (skippedNoPosition > 0) {
        spdlog::warn("[glTF]   跳过 {} 个缺少可用 POSITION 的 primitive", skippedNoPosition);
    }
    if (impl_->errors.count > 0) {
        // 解析成功但有 warning（例如用到了我们不认识的扩展），一并打出来
        for (uint32_t i = 0; i < impl_->errors.count; ++i) {
            const auto& e = impl_->errors.entries[i];
            spdlog::warn("[glTF]   解析告警: {}", e.message != nullptr ? e.message : "(null)");
        }
    }
}

}  // namespace origin
