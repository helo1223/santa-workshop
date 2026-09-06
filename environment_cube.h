#pragma once
#include "raylib.h"
#include "rlgl.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

// Original environment DDS files: six DXT faces, each followed by its mips.
// Gather the base level explicitly; LoadImage's 2D DDS path loses face boundaries.
inline TextureCubemap LoadEnvironmentCube(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto length = file.tellg();
    if (length < 128 || length > 64 * 1024 * 1024) return {};
    std::vector<unsigned char> bytes((size_t)length);
    file.seekg(0);
    if (!file.read((char*)bytes.data(), bytes.size())) return {};
    const auto word = [&](size_t at) {
        return uint32_t(bytes[at]) | uint32_t(bytes[at+1]) << 8 |
            uint32_t(bytes[at+2]) << 16 | uint32_t(bytes[at+3]) << 24;
    };
    const uint32_t size = word(16), mips = word(28) ? word(28) : 1;
    if (std::memcmp(bytes.data(), "DDS ", 4) || word(4) != 124 ||
        word(76) != 32 || !(word(80) & 4) || (word(112) & 0xfe00) != 0xfe00 ||
        (word(112) & 0x200000) || !size || size > 4096 || word(12) != size ||
        (size & (size-1)) || mips > 13) return {};
    unsigned block;
    int format;
    switch (word(84)) {
    case 0x31545844: block=8; format=PIXELFORMAT_COMPRESSED_DXT1_RGB; break;
    case 0x33545844: block=16; format=PIXELFORMAT_COMPRESSED_DXT3_RGBA; break;
    case 0x35545844: block=16; format=PIXELFORMAT_COMPRESSED_DXT5_RGBA; break;
    default: return {};
    }
    size_t faceBytes=0, baseBytes=0;
    uint32_t dimension=size;
    for (uint32_t mip=0; mip<mips; ++mip) {
        size_t n=size_t((dimension+3)/4)*((dimension+3)/4)*block;
        if (!mip) baseBytes=n;
        faceBytes+=n;
        if (dimension==1 && mip+1<mips) return {};
        dimension=dimension>1 ? dimension/2 : 1;
    }
    if (faceBytes > (bytes.size()-128)/6) return {};
    std::vector<unsigned char> faces(baseBytes*6);
    for (size_t face=0; face<6; ++face)
        std::memcpy(faces.data()+baseBytes*face, bytes.data()+128+faceBytes*face, baseBytes);
    TextureCubemap cube{};
    cube.id=rlLoadTextureCubemap(faces.data(), size, format, 1);
    if (cube.id) { cube.width=cube.height=size; cube.mipmaps=1; cube.format=format; }
    return cube;
}
