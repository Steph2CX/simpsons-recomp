#pragma once

// Traduction de microcode Xenos (vertex ou pixel shader) en SPIR-V, avec XenosRecomp et DXC.
// Interface volontairement independante des types de XenosRecomp (compile a part).

#include <cstdint>
#include <string>
#include <vector>

struct XenosVertexAttribute
{
    uint32_t location; // vertex input location in the SPIR-V
    uint32_t fetchConstant; // vertex fetch constant slot (0-95)
    uint32_t format; // Xenos vertex format (k_8_8_8_8, k_32_32_32_FLOAT...)
    uint32_t offsetDwords; // offset inside the vertex
    uint32_t strideDwords; // vertex stride
    bool isSigned;
    bool isNormalized; // numFormatAll == 0
    bool computedIndex = false; // fetched at an index computed by the shader: whole stream decoded to a buffer
};

struct XenosShader
{
    bool valid = false;
    std::vector<uint32_t> spirv;
    std::vector<XenosVertexAttribute> attributes; // vertex shaders only
    uint32_t pixelOutputs = 0; // bit 0-3: color targets, bit 4: depth
    uint32_t textureSlots = 0; // texture fetch constants read by tfetch (bit N = slot N)
    uint8_t textureDimensions[32]{}; // per slot: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube
    // Per slot, filters chosen by the tfetch instruction: mag bits 0-1, min 2-3, mip 4-5 (3 = from the fetch constant)
    uint8_t textureFilters[32]{ 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
        0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F };
    std::string hlsl; // kept for debugging
    std::string error;
};

// microcode: big-endian dwords as stored in guest memory
XenosShader TranslateXenosShader(const uint8_t* microcode, uint32_t sizeBytes, bool isPixelShader);
