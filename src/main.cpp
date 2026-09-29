// main.cpp — 入口与主循环。
//
// 采用 SDL3 的 main callbacks 模式（SDL_MAIN_USE_CALLBACKS），而不是自己写
// while(true) 主循环。这是「平台层负担最小」的关键：
//
//   - Android 上 SDL 会在 SDLActivity 起的线程里驱动这些回调，
//     并把 Activity 生命周期翻译成 SDL 事件；
//   - SDL_HINT_ANDROID_BLOCK_ON_PAUSE 默认开启，应用进入后台时 SDL 自行阻塞
//     回调循环，因此我们不需要像 GameActivity 那样手写「无 surface 时跳过渲染」
//     的分支，也不需要自己 poll looper、清输入缓冲；
//   - 同一份代码在 Windows 上同样可用，SDL 会生成常规的 main()。
//
// 官方 README-android.md 明确写了：用 main callbacks 时，生命周期事件会直接
// 送到 SDL_AppEvent()，不需要额外 SDL_SetEventFilter。

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <imgui_impl_sdl3.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>

#include "Log.hpp"
#include "VulkanRenderer.hpp"

#include <spdlog/sinks/base_sink.h>

#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace origin {
namespace {

// 把 spdlog 的输出转发给 SDL_Log 的自定义 sink。
//
// 这样做而不是用 spdlog 自带的 android_sink，是为了让日志也变成平台无关的：
// SDL_Log 在 Android 上落到 logcat，在 Windows 上落到控制台/调试输出，
// 于是这里既不需要 #ifdef __ANDROID__，也不需要链接 Android 的 liblog。
template <typename Mutex>
class SdlSink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);

        // spdlog 的 formatter 默认会带换行，SDL_Log 自己也会加，去掉尾部空白
        std::string text(buf.data(), buf.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }

        SDL_LogPriority pri = SDL_LOG_PRIORITY_INFO;
        if (msg.level == spdlog::level::trace) {
            pri = SDL_LOG_PRIORITY_VERBOSE;
        } else if (msg.level == spdlog::level::debug) {
            pri = SDL_LOG_PRIORITY_DEBUG;
        } else if (msg.level == spdlog::level::warn) {
            pri = SDL_LOG_PRIORITY_WARN;
        } else if (msg.level == spdlog::level::err) {
            pri = SDL_LOG_PRIORITY_ERROR;
        } else if (msg.level == spdlog::level::critical) {
            pri = SDL_LOG_PRIORITY_CRITICAL;
        }

        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, pri, "%s", text.c_str());
    }

    void flush_() override {}
};

using SdlSinkMt = SdlSink<std::mutex>;

}  // namespace

void initLogging() {
    auto sink   = std::make_shared<SdlSinkMt>();
    auto logger = std::make_shared<spdlog::logger>("main", sink);
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    // logcat 与 SDL 都会自带时间戳和等级，这里只输出消息体
    spdlog::set_pattern("%v");
}

}  // namespace origin

namespace {

struct AppState {
    SDL_Window*           window = nullptr;
    origin::VulkanRenderer renderer;
};

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int /*argc*/, char** /*argv*/) {
    origin::initLogging();
    spdlog::info("SDL_AppInit");

    // SDL3 的初始化函数返回 bool（SDL2 返回 int，0 表示成功），注意别写反
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init 失败: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Android 上宽高会被忽略（窗口总是全屏），这里的值只对桌面构建生效。
    // SDL_WINDOW_VULKAN 会让 SDL 加载 Vulkan loader，
    // 之后 SDL_Vulkan_GetVkGetInstanceProcAddr() 才可用。
    SDL_Window* window = SDL_CreateWindow("Origin", 1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow 失败: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    auto state    = std::make_unique<AppState>();
    state->window = window;

    try {
        state->renderer.init(window);
    } catch (const std::exception& e) {
        spdlog::error("渲染器初始化失败: {}", e.what());
        state->renderer.shutdown();
        SDL_DestroyWindow(window);
        return SDL_APP_FAILURE;
    }

    // 交出所有权，之后由 SDL_AppQuit 负责释放
    *appstate = state.release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto* state = static_cast<AppState*>(appstate);

    if (state->renderer.ready()) {
        try {
            state->renderer.drawFrame();

            // 自动化/无人值守验证用：设了 ORIGIN_SHOT 就在第 40 帧存一张图
            // （等几帧是为了让 swapchain 重建、ImGui 首帧字体上传都完成）。
            // 交互时用面板上的 Screenshot 按钮即可。
            static int frame = 0;
            if (++frame == 40) {
                if (const char* p = SDL_getenv("ORIGIN_SHOT")) {
                    state->renderer.requestScreenshot(p);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("drawFrame 异常: {}", e.what());
            return SDL_APP_FAILURE;
        }
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* state = static_cast<AppState*>(appstate);

    // 先喂给 ImGui。不做这一步面板能画出来但完全点不动 ——
    // 鼠标位置、点击、滚轮、键盘全靠这里注入。
    //
    // 这里刻意不根据 io.WantCaptureMouse 提前 return：
    // 当前应用层没有自己的鼠标交互（没有相机控制），不存在争抢；
    // 等加了轨道相机再按 WantCaptureMouse / WantCaptureKeyboard 分流。
    ImGui_ImplSDL3_ProcessEvent(event);

    switch (event->type) {
        case SDL_EVENT_QUIT:
            spdlog::info("SDL_EVENT_QUIT");
            return SDL_APP_SUCCESS;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            // 用像素尺寸变化驱动 swapchain 重建。
            // 转屏、分屏、桌面端拖拽窗口都会走到这里。
            state->renderer.notifyResize();
            break;

        // 以下三个是移动端生命周期事件。
        // 因为 BLOCK_ON_PAUSE 默认开启，SDL 会在进入后台后暂停回调循环，
        // 所以这里只记录日志即可，不需要手动销毁 surface。
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            spdlog::info("即将进入后台");
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            spdlog::info("已回到前台");
            break;
        case SDL_EVENT_LOW_MEMORY:
            spdlog::warn("系统内存紧张");
            break;

        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/) {
    spdlog::info("SDL_AppQuit");

    if (appstate != nullptr) {
        auto* state = static_cast<AppState*>(appstate);
        state->renderer.shutdown();
        if (state->window != nullptr) {
            SDL_DestroyWindow(state->window);
        }
        delete state;
    }
    // SDL_Quit() 由 SDL 的 main callbacks 框架自动调用，这里不要重复调
}
