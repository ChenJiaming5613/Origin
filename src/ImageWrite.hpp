#pragma once

// 由 StbImage.cpp（stb 的唯一实现单元）提供。
// 单独开一个头是为了不让业务代码去 include stb_image_write.h ——
// 那个头只能在一个 TU 里展开实现，暴露出去容易被误用。
bool writePngRgba(const char* path, int width, int height, const void* rgba);
