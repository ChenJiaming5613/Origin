#pragma once

#include <spdlog/spdlog.h>
#include <volk.h>

#include <stdexcept>
#include <string>

namespace origin {

// 在 android_main 开头调用一次，把 spdlog 接到 logcat。
// 之后 spdlog::info/warn/error 都可以用 `adb logcat -s HelloTriangle` 看到。
void initLogging();

inline const char* vkResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        default:                                return "VK_ERROR_<other>";
    }
}

}  // namespace origin

// 所有 Vulkan 调用都应该套一层 VK_CHECK。
// Android 上没有控制台，一个被忽略的 VkResult 会表现为「黑屏但不崩溃」，
// 这是最难定位的一类问题，所以宁可直接抛异常在 logcat 里留下明确记录。
#define VK_CHECK(expr)                                                              \
    do {                                                                            \
        VkResult _r = (expr);                                                       \
        if (_r != VK_SUCCESS) {                                                     \
            spdlog::error("{} 失败: {} ({})", #expr, origin::vkResultString(_r),      \
                          static_cast<int>(_r));                                    \
            throw std::runtime_error(std::string(#expr) + " -> " +                   \
                                     origin::vkResultString(_r));                    \
        }                                                                           \
    } while (0)
