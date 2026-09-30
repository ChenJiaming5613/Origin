// VulkanMemoryAllocator 的唯一实现单元。
//
// vk_mem_alloc.h 是 header-only 的，定义 VMA_IMPLEMENTATION 的那一次会把
// 一万多行实现展开进来。整个工程只能有一个 TU 这么做，否则重复符号。
// 其他地方正常 include <vk_mem_alloc.h> 即可（只拿声明）。
//
// 编译宏（VMA_VULKAN_VERSION / VMA_STATIC_VULKAN_FUNCTIONS /
// VMA_DYNAMIC_VULKAN_FUNCTIONS）统一挂在 CMake 的 vma target 上并以 PUBLIC
// 传播，**不要在这里 #define** —— 那样实现 TU 与调用方 TU 会看到不同取值，
// 而 VMA_VULKAN_VERSION 是影响结构体布局的，属于安静的 ODR 违规。

// VMA 内部大量使用「条件成立时才有意义」的表达式与未使用参数，
// 用工程主 target 的警告级别编它会刷很多噪音。这里单独压掉，
// 不影响我们自己的代码。
#if defined(_MSC_VER)
#    pragma warning(push)
// C4100 未引用的形参、C4127 条件表达式是常量、C4189 局部变量已初始化但未引用
#    pragma warning(disable : 4100 4127 4189)
#elif defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wunused-variable"
#    pragma clang diagnostic ignored "-Wunused-parameter"
#    pragma clang diagnostic ignored "-Wnullability-completeness"
#    pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunused-variable"
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#    pragma warning(pop)
#elif defined(__clang__)
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
