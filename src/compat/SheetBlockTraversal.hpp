#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wxl_modern_m2::sheet
{
// BC1 still uses 4x4 blocks at 2x1 and 1x1 mip sizes. Pad only the input tile,
// never enlarge the source image or let the compressor read outside its rows.
template<class Encode>
bool CompressLevel(const uint8_t* source, uint32_t width, uint32_t height,
                   uint8_t* destination, Encode encode)
{
    if (!source || !destination || !width || !height || width > 4096 || height > 4096 ||
        (width & (width - 1)) || (height & (height - 1))) return false;
    const uint32_t pitch = width * 4;
    for (uint32_t y = 0; y < height; y += 4)
        for (uint32_t x = 0; x < width; x += 4)
        {
            const uint8_t* block = source + size_t(y) * pitch + x * 4;
            if (x + 4 <= width && y + 4 <= height && !(reinterpret_cast<uintptr_t>(block) & 15))
                encode(block, pitch, destination);
            else
            {
                alignas(16) uint8_t tile[64];
                for (uint32_t row = 0; row < 4; ++row)
                    for (uint32_t col = 0; col < 4; ++col)
                    {
                        const uint32_t sx = x + col < width ? x + col : width - 1;
                        const uint32_t sy = y + row < height ? y + row : height - 1;
                        std::memcpy(tile + row * 16 + col * 4, source + size_t(sy) * pitch + sx * 4, 4);
                    }
                encode(tile, 16, destination);
            }
            destination += 8;
        }
    return true;
}
}
