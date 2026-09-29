// stb 的唯一实现单元。
//
// STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION 必须且只能在一个
// 翻译单元里定义，所以单独给它们一个 .cpp，不要混进业务代码 ——
// 否则以后谁多 include 一次就是重复符号。
//
// 几个刻意的裁剪：
//   STBI_NO_STDIO      资源统一走 SDL_LoadFile 读进内存（Android 上 APK 内的
//                      asset 没有文件系统路径，用不了 fopen），
//                      关掉它顺带也去掉了 MSVC 对 fopen 的安全警告。
//   STBI_ONLY_JPEG/PNG 只留这两种解码器。默认还会编进 bmp/psd/tga/gif/hdr/pic/pnm，
//                      对移动端包体是白给的开销。
//
// 注意 stb_image_write 用的是独立的 STBI_WRITE_NO_STDIO 宏，
// 上面的 STBI_NO_STDIO 不会影响它 —— 截图确实要落盘，所以保留它的 stdio。

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG

#define STB_IMAGE_WRITE_IMPLEMENTATION

// stb 是第三方单头库，用 /W4 与 -Wall -Wextra 编会刷出大量与我们无关的警告
// （而且工程开了 -WX / 警告即错误的话会直接失败），所以在这里局部关掉。
#if defined(_MSC_VER)
#    pragma warning(push, 0)
#elif defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wall"
#    pragma GCC diagnostic ignored "-Wextra"
#endif

#include <stb_image.h>
#include <stb_image_write.h>

#if defined(_MSC_VER)
#    pragma warning(pop)
#elif defined(__clang__)
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

#include "ImageWrite.hpp"

bool writePngRgba(const char* path, int width, int height, const void* rgba) {
    return stbi_write_png(path, width, height, 4, rgba, width * 4) != 0;
}
