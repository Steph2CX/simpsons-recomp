// Textures Xenos -> textures Vulkan.
//
// Constante de fetch de texture (6 mots, registres 0x4800 + 6 * slot) :
//  mot 0 : type (bits 0-1, 2 = texture), modes d'adressage X/Y/Z (bits 10-12, 13-15, 16-18),
//          pas en unites de 32 texels (bits 22-30), texture tilee (bit 31)
//  mot 1 : format (bits 0-5), ordre des octets (bits 6-7), adresse physique >> 12 (bits 12-31)
//  mot 2 : dimensions 2D : largeur - 1 (bits 0-12), hauteur - 1 (bits 13-25)
//  mot 3 : swizzle (bits 1-12, 3 bits par composante), filtres mag/min/mip (bits 19-20, 21-22, 23-24)
//  mot 5 : dimension (bits 9-10 : 1D, 2D, 3D, cube), mips compactes (bit 11), adresse des mips
//
// Textures 2D et cubes avec leurs mipmaps (y compris la queue compactee) ; les 3D utilisent le
// remplacement blanc.
#include "textures.h"

#include "runtime.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

uint32_t GpuReadRegister(uint32_t index);
uint64_t GpuFrameCount();

namespace fs = std::filesystem;

fs::path ModificationsRoot(); // files.cpp

namespace
{
    constexpr uint32_t REG_TEXTURE_FETCH_CONSTANTS = 0x4800;

    enum class Convert : uint8_t
    {
        None,
        R5G6B5, // 16 bits -> RGBA8
        R5G5B5A1,
        R4G4B4A4,
    };

    struct FormatInfo
    {
        bool supported = false;
        uint8_t blockSize = 1; // texels per block side
        uint8_t bytesPerBlock = 4; // in guest memory
        RendererTextureFormat host = RendererTextureFormat::RGBA8;
        Convert convert = Convert::None;
    };

    FormatInfo GetFormatInfo(uint32_t format)
    {
        using F = RendererTextureFormat;
        switch (format)
        {
        case 2: return { true, 1, 1, F::R8 }; // k_8
        case 3: return { true, 1, 2, F::RGBA8, Convert::R5G5B5A1 }; // k_1_5_5_5
        case 4: return { true, 1, 2, F::RGBA8, Convert::R5G6B5 }; // k_5_6_5
        case 6: return { true, 1, 4, F::RGBA8 }; // k_8_8_8_8
        case 7: return { true, 1, 4, F::RGB10A2 }; // k_2_10_10_10
        case 10: return { true, 1, 2, F::RG8 }; // k_8_8
        case 14: return { true, 1, 4, F::RGBA8 }; // k_8_8_8_8_A
        case 15: return { true, 1, 2, F::RGBA8, Convert::R4G4B4A4 }; // k_4_4_4_4
        case 18: return { true, 4, 8, F::BC1 }; // k_DXT1
        case 19: return { true, 4, 16, F::BC2 }; // k_DXT2_3
        case 20: return { true, 4, 16, F::BC3 }; // k_DXT4_5
        case 24: return { true, 1, 2, F::R16 }; // k_16
        case 25: return { true, 1, 4, F::RG16 }; // k_16_16
        case 26: return { true, 1, 8, F::RGBA16 }; // k_16_16_16_16
        case 30: return { true, 1, 2, F::R16F }; // k_16_FLOAT
        case 31: return { true, 1, 4, F::RG16F }; // k_16_16_FLOAT
        case 32: return { true, 1, 8, F::RGBA16F }; // k_16_16_16_16_FLOAT
        case 36: return { true, 1, 4, F::R32F }; // k_32_FLOAT
        case 37: return { true, 1, 8, F::RG32F }; // k_32_32_FLOAT
        case 38: return { true, 1, 16, F::RGBA32F }; // k_32_32_32_32_FLOAT
        case 49: return { true, 4, 16, F::BC5 }; // k_DXN
        case 50: return { true, 1, 4, F::RGBA8 }; // k_8_8_8_8_AS_16_16_16_16
        case 51: return { true, 4, 8, F::BC1 }; // k_DXT1_AS_16_16_16_16
        case 52: return { true, 4, 16, F::BC2 };
        case 53: return { true, 4, 16, F::BC3 };
        case 59: return { true, 4, 8, F::BC4 }; // k_DXT5A
        default: return {};
        }
    }

    // Endianness of the fetch constant: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32
    void SwapEndian(uint8_t* data, size_t size, uint32_t endian)
    {
        switch (endian)
        {
        case 1:
            for (size_t i = 0; i + 1 < size; i += 2)
                std::swap(data[i], data[i + 1]);
            break;
        case 2:
            for (size_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
        case 3:
            for (size_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 2]);
                std::swap(data[i + 1], data[i + 3]);
            }
            break;
        }
    }

    uint32_t Expand(uint32_t value, uint32_t bits)
    {
        return (value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
    }

    void ConvertTexels(const uint8_t* source, uint8_t* dest, size_t texels, Convert convert)
    {
        for (size_t i = 0; i < texels; i++)
        {
            uint16_t v;
            memcpy(&v, source + i * 2, 2);
            uint8_t* out = dest + i * 4;
            switch (convert)
            {
            case Convert::R5G6B5:
                out[0] = Expand(v & 0x1F, 5); out[1] = Expand((v >> 5) & 0x3F, 6); out[2] = Expand(v >> 11, 5); out[3] = 255;
                break;
            case Convert::R5G5B5A1:
                out[0] = Expand(v & 0x1F, 5); out[1] = Expand((v >> 5) & 0x1F, 5); out[2] = Expand((v >> 10) & 0x1F, 5); out[3] = (v >> 15) ? 255 : 0;
                break;
            case Convert::R4G4B4A4:
                out[0] = Expand(v & 0xF, 4); out[1] = Expand((v >> 4) & 0xF, 4); out[2] = Expand((v >> 8) & 0xF, 4); out[3] = Expand(v >> 12, 4);
                break;
            case Convert::None:
                break;
            }
        }
    }

    struct CachedTexture
    {
        uint64_t contentHash = 0;
        uint64_t checkedFrame = UINT64_MAX;
        uint32_t descriptorIndex = 0;
        bool addedMips = false; // mip chain built here (modifications/textures/<hash>.mips)
    };
    std::unordered_map<uint64_t, CachedTexture> g_textures;
    uint64_t g_frame = 0;
    std::vector<uint8_t> g_decodeBuffer, g_convertBuffer, g_replacementBuffer;

    // Replaced images: modifications/textures/<hash>.bin holds new data for the texture whose decoded base level
    // has this XXH3 hash (shown in the SIMPSONS_DUMP_TEXTURES file names), in the same format: either the whole
    // decoded texture (any format, all mip levels), or the RGBA8 base level alone (mip levels rebuilt from it).
    // Example: "EA | HD" of the start screen -> "EA | 4K ULTRA HD".
    std::unordered_map<uint64_t, std::vector<uint8_t>> g_replacements;
    // modifications/textures/<hash>.mips: the game gives this texture a single level although it is drawn much
    // smaller (title logo: 2048x1024 shown about 600 pixels wide), so its edges look jagged. Its base level is
    // decoded to RGBA8 and a mip chain is built from it (see AddMips). The file is empty (whole chain) or holds the
    // number of levels to keep: few levels for a tightly packed atlas such as the font, whose small levels would
    // mix neighbouring glyphs.
    std::unordered_map<uint64_t, uint32_t> g_addMips; // hash -> maximum number of levels
    std::vector<uint8_t> g_mipsBuffer;
    std::vector<uint8_t> g_unpackBuffer; // BC textures decompressed for GPUs without BC (Apple TV)

    void LoadReplacements()
    {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(ModificationsRoot() / "textures", error))
        {
            uint64_t hash = strtoull(entry.path().stem().string().c_str(), nullptr, 16);
            if (entry.path().extension() == ".mips")
            {
                uint32_t levels = 16;
                if (FILE* file = fopen(entry.path().string().c_str(), "r"))
                {
                    if (fscanf(file, "%u", &levels) != 1 || levels < 2)
                        levels = 16;
                    fclose(file);
                }
                g_addMips[hash] = levels;
            }
            if (entry.path().extension() != ".bin")
                continue;
            std::vector<uint8_t> data(size_t(entry.file_size(error)));
            if (FILE* file = fopen(entry.path().string().c_str(), "rb"))
            {
                if (fread(data.data(), 1, data.size(), file) == data.size())
                    g_replacements[hash] = std::move(data);
                fclose(file);
            }
        }
        if (!g_replacements.empty() || !g_addMips.empty())
            fprintf(stderr, "[textures] %zu image(s) de remplacement, %zu image(s) lissee(s)\n", g_replacements.size(), g_addMips.size());
    }

    // Returns the decoded texture with its replacement, or nullptr
    const uint8_t* ReplaceTexture(const RendererTextureDesc& desc)
    {
        static bool loaded = (LoadReplacements(), true);
        if (g_replacements.empty() || desc.cube)
            return nullptr;
        size_t baseSize = desc.levels > 1 ? desc.levelOffsets[1] : desc.size;
        auto it = g_replacements.find(XXH3_64bits(desc.data, baseSize));
        if (it == g_replacements.end())
            return nullptr;
        if (it->second.size() == desc.size)
        {
            fprintf(stderr, "[textures] image remplacee %016llX (%ux%u)\n", (unsigned long long)it->first, desc.width, desc.height);
            return it->second.data();
        }
        if (desc.format != RendererTextureFormat::RGBA8 || it->second.size() != size_t(desc.width) * desc.height * 4)
            return nullptr;
        g_replacementBuffer.assign(static_cast<const uint8_t*>(desc.data), static_cast<const uint8_t*>(desc.data) + desc.size);
        memcpy(g_replacementBuffer.data(), it->second.data(), baseSize);
        for (uint32_t level = 1; level < desc.levels; level++)
        {
            uint32_t sourceWidth = std::max(1u, desc.width >> (level - 1)), sourceHeight = std::max(1u, desc.height >> (level - 1));
            uint32_t width = std::max(1u, desc.width >> level), height = std::max(1u, desc.height >> level);
            const uint8_t* source = g_replacementBuffer.data() + desc.levelOffsets[level - 1];
            uint8_t* out = g_replacementBuffer.data() + desc.levelOffsets[level];
            for (uint32_t y = 0; y < height; y++)
                for (uint32_t x = 0; x < width; x++)
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        uint32_t x0 = std::min(x * 2, sourceWidth - 1), x1 = std::min(x * 2 + 1, sourceWidth - 1);
                        uint32_t y0 = std::min(y * 2, sourceHeight - 1), y1 = std::min(y * 2 + 1, sourceHeight - 1);
                        uint32_t sum = source[(y0 * sourceWidth + x0) * 4 + c] + source[(y0 * sourceWidth + x1) * 4 + c] +
                            source[(y1 * sourceWidth + x0) * 4 + c] + source[(y1 * sourceWidth + x1) * 4 + c];
                        out[(y * width + x) * 4 + c] = uint8_t((sum + 2) / 4);
                    }
        }
        fprintf(stderr, "[textures] image remplacee %016llX (%ux%u)\n", (unsigned long long)it->first, desc.width, desc.height);
        return g_replacementBuffer.data();
    }

    void DecodeChannelBlock(const uint8_t* block, uint8_t* out, size_t stride);

    // BC1/BC2/BC3 block (little-endian, as uploaded) -> 4x4 RGBA8 texels
    void DecodeBlock(RendererTextureFormat format, const uint8_t* block, uint8_t out[16][4])
    {
        using F = RendererTextureFormat;
        const uint8_t* color = format == F::BC1 ? block : block + 8;
        uint16_t c0 = uint16_t(color[0] | (color[1] << 8)), c1 = uint16_t(color[2] | (color[3] << 8));
        uint8_t palette[4][4];
        for (int i = 0; i < 2; i++)
        {
            uint16_t c = i == 0 ? c0 : c1;
            palette[i][0] = uint8_t(Expand(c >> 11, 5));
            palette[i][1] = uint8_t(Expand((c >> 5) & 0x3F, 6));
            palette[i][2] = uint8_t(Expand(c & 0x1F, 5));
            palette[i][3] = 255;
        }
        bool fourColors = format != F::BC1 || c0 > c1;
        for (int c = 0; c < 3; c++)
        {
            palette[2][c] = uint8_t(fourColors ? (2 * palette[0][c] + palette[1][c] + 1) / 3 : (palette[0][c] + palette[1][c]) / 2);
            palette[3][c] = uint8_t(fourColors ? (palette[0][c] + 2 * palette[1][c] + 1) / 3 : 0);
        }
        palette[2][3] = 255;
        palette[3][3] = fourColors ? 255 : 0;
        uint32_t indices = uint32_t(color[4] | (color[5] << 8) | (color[6] << 16) | (uint32_t(color[7]) << 24));
        for (int i = 0; i < 16; i++)
            memcpy(out[i], palette[(indices >> (i * 2)) & 3], 4);

        if (format == F::BC2)
            for (int i = 0; i < 16; i++)
                out[i][3] = uint8_t(Expand((block[i / 2] >> ((i & 1) * 4)) & 0xF, 4));
        else if (format == F::BC3)
            DecodeChannelBlock(block, &out[0][3], 4);
    }

    // BC4 block (8 bytes, also the alpha of BC3 and each channel of BC5) -> 16 values, every stride bytes in out
    void DecodeChannelBlock(const uint8_t* block, uint8_t* out, size_t stride)
    {
        uint8_t values[8] = { block[0], block[1] };
        for (int i = 2; i < 8; i++)
            values[i] = block[0] > block[1] ? uint8_t(((8 - i) * block[0] + (i - 1) * block[1] + 3) / 7)
                : i < 6 ? uint8_t(((6 - i) * block[0] + (i - 1) * block[1] + 2) / 5) : (i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++)
            bits |= uint64_t(block[2 + i]) << (i * 8);
        for (int i = 0; i < 16; i++)
            out[i * stride] = values[(bits >> (i * 3)) & 7];
    }

    bool IsBlockCompressed(RendererTextureFormat format)
    {
        using F = RendererTextureFormat;
        return format == F::BC1 || format == F::BC2 || format == F::BC3 || format == F::BC4 || format == F::BC5;
    }

    // GPUs without BC compression (Apple TV A15): every level and layer of a BC1-5 texture decompressed to RGBA8
    // (BC1-3), R8 (BC4) or RG8 (BC5), the same components in the same order
    void UnpackBlockCompressed(RendererTextureDesc& desc)
    {
        using F = RendererTextureFormat;
        size_t blockBytes = desc.format == F::BC1 || desc.format == F::BC4 ? 8 : 16;
        uint32_t channels = desc.format == F::BC4 ? 1 : desc.format == F::BC5 ? 2 : 4;
        uint32_t layers = desc.cube ? 6 : 1;
        size_t total = 0, offsets[16];
        for (uint32_t level = 0; level < desc.levels; level++)
        {
            offsets[level] = total;
            total += size_t(std::max(desc.width >> level, 1u)) * std::max(desc.height >> level, 1u) * channels * layers;
        }
        g_unpackBuffer.resize(total);
        const uint8_t* source = static_cast<const uint8_t*>(desc.data);
        uint8_t texels[16][4];
        for (uint32_t level = 0; level < desc.levels; level++)
        {
            uint32_t width = std::max(desc.width >> level, 1u), height = std::max(desc.height >> level, 1u);
            uint32_t blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
            for (uint32_t layer = 0; layer < layers; layer++)
            {
                const uint8_t* in = source + desc.levelOffsets[level] + size_t(layer) * blocksWide * blocksHigh * blockBytes;
                uint8_t* out = g_unpackBuffer.data() + offsets[level] + size_t(layer) * width * height * channels;
                for (uint32_t by = 0; by < blocksHigh; by++)
                    for (uint32_t bx = 0; bx < blocksWide; bx++)
                    {
                        const uint8_t* block = in + (size_t(by) * blocksWide + bx) * blockBytes;
                        if (desc.format == F::BC4 || desc.format == F::BC5)
                        {
                            DecodeChannelBlock(block, &texels[0][0], 4);
                            if (desc.format == F::BC5)
                                DecodeChannelBlock(block + 8, &texels[0][1], 4);
                        }
                        else
                            DecodeBlock(desc.format, block, texels);
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                            if (x < width && y < height)
                                memcpy(out + (size_t(y) * width + x) * channels, texels[i], channels);
                        }
                    }
            }
        }
        for (uint32_t level = 0; level < desc.levels; level++)
            desc.levelOffsets[level] = offsets[level];
        desc.format = desc.format == F::BC4 ? F::R8 : desc.format == F::BC5 ? F::RG8 : F::RGBA8;
        desc.data = g_unpackBuffer.data();
        desc.size = total;
    }

    // Replaces desc (single-level RGBA8 or BC1-3 texture listed in g_addMips) with an RGBA8 copy carrying a whole
    // mip chain. Colours are averaged weighted by alpha so transparent texels do not darken the edges.
    bool AddMips(RendererTextureDesc& desc)
    {
        using F = RendererTextureFormat;
        if (g_addMips.empty() || desc.cube || desc.levels != 1 ||
            !(desc.format == F::RGBA8 || desc.format == F::BC1 || desc.format == F::BC2 || desc.format == F::BC3))
            return false;
        auto wanted = g_addMips.find(XXH3_64bits(desc.data, desc.size));
        if (wanted == g_addMips.end())
            return false;
        uint32_t width = desc.width, height = desc.height;
        uint32_t levels = std::min({ 32u - uint32_t(__builtin_clz(std::max(width, height))), wanted->second, 16u });
        size_t total = 0;
        for (uint32_t level = 0; level < levels; level++)
        {
            desc.levelOffsets[level] = total;
            total += size_t(std::max(width >> level, 1u)) * std::max(height >> level, 1u) * 4;
        }
        g_mipsBuffer.resize(total);
        const uint8_t* source = static_cast<const uint8_t*>(desc.data);
        if (desc.format == F::RGBA8)
            memcpy(g_mipsBuffer.data(), source, size_t(width) * height * 4);
        else
        {
            size_t blockBytes = desc.format == F::BC1 ? 8 : 16;
            uint32_t blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
            uint8_t texels[16][4];
            for (uint32_t by = 0; by < blocksHigh; by++)
                for (uint32_t bx = 0; bx < blocksWide; bx++)
                {
                    DecodeBlock(desc.format, source + (size_t(by) * blocksWide + bx) * blockBytes, texels);
                    for (uint32_t i = 0; i < 16; i++)
                    {
                        uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x < width && y < height)
                            memcpy(g_mipsBuffer.data() + (size_t(y) * width + x) * 4, texels[i], 4);
                    }
                }
        }
        for (uint32_t level = 1; level < levels; level++)
        {
            uint32_t sourceWidth = std::max(width >> (level - 1), 1u), sourceHeight = std::max(height >> (level - 1), 1u);
            uint32_t levelWidth = std::max(width >> level, 1u), levelHeight = std::max(height >> level, 1u);
            const uint8_t* in = g_mipsBuffer.data() + desc.levelOffsets[level - 1];
            uint8_t* out = g_mipsBuffer.data() + desc.levelOffsets[level];
            for (uint32_t y = 0; y < levelHeight; y++)
                for (uint32_t x = 0; x < levelWidth; x++)
                {
                    const uint8_t* texels[4] = {
                        in + (size_t(std::min(y * 2, sourceHeight - 1)) * sourceWidth + std::min(x * 2, sourceWidth - 1)) * 4,
                        in + (size_t(std::min(y * 2, sourceHeight - 1)) * sourceWidth + std::min(x * 2 + 1, sourceWidth - 1)) * 4,
                        in + (size_t(std::min(y * 2 + 1, sourceHeight - 1)) * sourceWidth + std::min(x * 2, sourceWidth - 1)) * 4,
                        in + (size_t(std::min(y * 2 + 1, sourceHeight - 1)) * sourceWidth + std::min(x * 2 + 1, sourceWidth - 1)) * 4 };
                    uint32_t alpha = texels[0][3] + texels[1][3] + texels[2][3] + texels[3][3];
                    uint8_t* texel = out + (size_t(y) * levelWidth + x) * 4;
                    for (int c = 0; c < 3; c++)
                    {
                        uint32_t weighted = 0, plain = 0;
                        for (const uint8_t* t : texels)
                        {
                            weighted += t[c] * t[3];
                            plain += t[c];
                        }
                        texel[c] = uint8_t(alpha ? (weighted + alpha / 2) / alpha : (plain + 2) / 4);
                    }
                    texel[3] = uint8_t((alpha + 2) / 4);
                }
        }
        fprintf(stderr, "[textures] image lissee %016llX (%ux%u, %u niveaux)\n", (unsigned long long)XXH3_64bits(desc.data, desc.size),
            width, height, levels);
        desc.format = F::RGBA8;
        desc.levels = levels;
        desc.data = g_mipsBuffer.data();
        desc.size = total;
        return true;
    }

    // SIMPSONS_DUMP_TEXTURES=<frame>[-<last frame>]: every texture uploaded during these frames is written to
    // recomp/textures/<address>_<width>x<height>_f<format>.dds (view with ffmpeg -i x.dds x.png)
    void DumpTexture(uint32_t address, uint32_t format, const RendererTextureDesc& desc)
    {
        using F = RendererTextureFormat;
        uint32_t fourcc = 0, bits = 0;
        switch (desc.format)
        {
        case F::BC1: fourcc = 0x31545844; break; // DXT1
        case F::BC2: fourcc = 0x33545844; break;
        case F::BC3: fourcc = 0x35545844; break;
        case F::BC4: fourcc = 0x31495441; break; // ATI1
        case F::BC5: fourcc = 0x32495441; break;
        case F::RGBA8: bits = 32; break;
        default: return;
        }
        fs::create_directories("recomp/textures");
        char name[96];
        snprintf(name, sizeof(name), "recomp/textures/%08X_%ux%u_f%u_%016llX.dds", address, desc.width, desc.height, format,
            (unsigned long long)XXH3_64bits(desc.data, desc.size));
        FILE* file = fopen(name, "wb");
        if (!file)
            return;
        uint32_t header[32]{};
        header[0] = 0x20534444; // "DDS "
        header[1] = 124;
        header[2] = 0x1007 | (fourcc ? 0x80000 : 0x8);
        header[3] = desc.height;
        header[4] = desc.width;
        header[5] = fourcc ? uint32_t(desc.size) : desc.width * 4;
        header[19] = 32;
        header[20] = fourcc ? 0x4 : 0x41;
        header[21] = fourcc;
        header[22] = bits;
        header[23] = 0x000000FF;
        header[24] = 0x0000FF00;
        header[25] = 0x00FF0000;
        header[26] = 0xFF000000;
        header[27] = 0x1000;
        fwrite(header, sizeof(header), 1, file);
        fwrite(desc.data, desc.size, 1, file);
        fclose(file);
    }

    uint32_t AddressMode(uint32_t xenos)
    {
        // 0 wrap, 1 mirror, 2 clamp to last texel, 3 mirror once to last texel, 4 clamp halfway,
        // 5 mirror once halfway, 6 clamp to border, 7 mirror once to border
        static const uint32_t modes[8] = { 0, 1, 2, 3, 2, 3, 4, 3 };
        return modes[xenos & 7];
    }

    uint32_t NextPow2(uint32_t value)
    {
        return value <= 1 ? 1 : 1u << (32 - __builtin_clz(value - 1));
    }

    uint32_t Log2Ceil(uint32_t value)
    {
        return value <= 1 ? 0 : 32 - __builtin_clz(value - 1);
    }

    uint32_t AlignUp(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    // Once the smallest side of a level is 16 texels or less, this level and the following ones share a single
    // 32x32 tile ("packed mips"). First level of that tail:
    uint32_t PackedTailLevel(uint32_t width, uint32_t height)
    {
        uint32_t log2Size = std::min(Log2Ceil(width), Log2Ceil(height));
        return log2Size > 4 ? log2Size - 4 : 0;
    }

    // Texel offset of a level inside the packed tail (width/height of the base level)
    void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t& x, uint32_t& y)
    {
        uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
        uint32_t tail = PackedTailLevel(width, height);
        uint32_t packed = level - tail;
        bool wide = log2Width > log2Height;
        if (packed < 3)
        {
            x = wide ? 0 : 16 >> packed;
            y = wide ? 16 >> packed : 0;
            return;
        }
        uint32_t offset = (1u << ((wide ? log2Width : log2Height) - tail)) >> (packed - 2);
        x = wide ? offset : 0;
        y = wide ? 0 : offset;
        if (offset < 4)
        {
            if (wide)
                y += 4;
            else
                x += 4;
        }
    }

    // SIMPSONS_CHECK_MIPS: compares each level with the previous one halved, using the mean color of each
    // block. A wrong guest layout gives unrelated images (large differences).
    void CheckMips(uint32_t address, uint32_t format, const FormatInfo& info, uint32_t width, uint32_t height, uint32_t levels,
        const size_t* offsets, bool packed)
    {
        if (format != 6 && format != 18 && format != 19 && format != 20)
            return;
        auto blockColor = [&](const uint8_t* block, float* rgb) {
            if (info.blockSize == 1)
            {
                for (int c = 0; c < 3; c++)
                    rgb[c] = block[c];
                return;
            }
            const uint8_t* colors = block + (info.bytesPerBlock == 16 ? 8 : 0);
            uint16_t c0 = colors[0] | colors[1] << 8, c1 = colors[2] | colors[3] << 8;
            rgb[0] = (Expand(c0 >> 11, 5) + Expand(c1 >> 11, 5)) * 0.5f;
            rgb[1] = (Expand((c0 >> 5) & 63, 6) + Expand((c1 >> 5) & 63, 6)) * 0.5f;
            rgb[2] = (Expand(c0 & 31, 5) + Expand(c1 & 31, 5)) * 0.5f;
        };
        static std::mutex mutex;
        static uint32_t histogram[2][6]{};
        static uint32_t checked = 0;
        std::lock_guard lock(mutex);
        for (uint32_t level = 1; level < levels; level++)
        {
            uint32_t bs = info.blockSize;
            uint32_t pw = (std::max(width >> (level - 1), 1u) + bs - 1) / bs, ph = (std::max(height >> (level - 1), 1u) + bs - 1) / bs;
            uint32_t cw = (std::max(width >> level, 1u) + bs - 1) / bs, ch = (std::max(height >> level, 1u) + bs - 1) / bs;
            if (cw < 2 || ch < 2 || pw < cw * 2 || ph < ch * 2)
                continue;
            double error = 0;
            for (uint32_t y = 0; y < ch; y++)
                for (uint32_t x = 0; x < cw; x++)
                {
                    float child[3], sum[3]{}, parent[3];
                    blockColor(g_decodeBuffer.data() + offsets[level] + (size_t(y) * cw + x) * info.bytesPerBlock, child);
                    for (uint32_t dy = 0; dy < 2; dy++)
                        for (uint32_t dx = 0; dx < 2; dx++)
                        {
                            blockColor(g_decodeBuffer.data() + offsets[level - 1] + (size_t(y * 2 + dy) * pw + x * 2 + dx) * info.bytesPerBlock, parent);
                            for (int c = 0; c < 3; c++)
                                sum[c] += parent[c] * 0.25f;
                        }
                    for (int c = 0; c < 3; c++)
                        error += std::abs(child[c] - sum[c]);
                }
            error /= double(cw) * ch * 3;
            bool inTail = packed && level >= PackedTailLevel(width, height);
            uint32_t bucket = error < 4 ? 0 : error < 8 ? 1 : error < 16 ? 2 : error < 32 ? 3 : error < 64 ? 4 : 5;
            histogram[inTail][bucket]++;
            static const bool verbose = atoi(getenv("SIMPSONS_CHECK_MIPS")) > 1;
            if (bucket >= 4 || (verbose && inTail))
                fprintf(stderr, "[mips] 0x%08X %ux%u format %u niveau %u%s : ecart %.1f\n", address, width, height, format, level,
                    inTail ? " (queue compactee)" : "", error);
        }
        if (++checked % 25 == 0)
            for (int tail = 0; tail < 2; tail++)
                fprintf(stderr, "[mips] %s ecart <4:%u <8:%u <16:%u <32:%u <64:%u >=64:%u\n", tail ? "queue compactee" : "niveaux normaux",
                    histogram[tail][0], histogram[tail][1], histogram[tail][2], histogram[tail][3], histogram[tail][4], histogram[tail][5]);
    }

    uint32_t BindTexture(uint32_t slot, uint32_t expectedDimension, bool& addedMips)
    {
        addedMips = false;
        uint32_t fetch[6];
        for (uint32_t i = 0; i < 6; i++)
            fetch[i] = GpuReadRegister(REG_TEXTURE_FETCH_CONSTANTS + slot * 6 + i);
        uint32_t dimension = (fetch[5] >> 9) & 3;
        if ((fetch[0] & 3) != 2 || dimension != expectedDimension || (dimension != 1 && dimension != 3)) // only 2D and cube
            return 0;
        bool cube = dimension == 3;

        uint32_t format = fetch[1] & 0x3F;
        uint32_t width = (fetch[2] & 0x1FFF) + 1;
        uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
        bool tiled = fetch[0] >> 31;
        uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
        uint8_t components[4];
        for (uint32_t c = 0; c < 4; c++)
        {
            uint32_t select = (swizzle >> (c * 3)) & 7;
            components[c] = uint8_t(select <= 5 ? select : c);
        }

        // Render target copied there by the GPU: use the host copy, guest memory was not written
        uint32_t resolved = cube ? 0 : RendererFindResolved(fetch[1] & 0x1FFFF000, width, height, components);
        static const char* traceFrame = getenv("SIMPSONS_TRACE_FETCH");
        if (traceFrame && GpuFrameCount() + 1 == uint64_t(atoll(traceFrame)))
            fprintf(stderr, "[fetch] slot %u : 0x%08X %ux%u format %u %s swizzle %03X -> %s %u\n", slot, fetch[1] & 0x1FFFF000, width,
                height, format, tiled ? "tilee" : "lineaire", swizzle, resolved ? "copie GPU" : "memoire", resolved);
        if (resolved)
            return resolved;

        FormatInfo info = GetFormatInfo(format);
        if (!info.supported || (!RendererSupportsFormat(info.host) && !IsBlockCompressed(info.host)))
        {
            static bool reported[64]{};
            if (!reported[format])
            {
                reported[format] = true;
                fprintf(stderr, "[textures] format %u non gere\n", format);
            }
            return 0;
        }

        uint32_t endian = (fetch[1] >> 6) & 3;
        uint32_t address = PhysicalToVirtual(fetch[1] & 0xFFFFF000);
        uint32_t mipAddress = fetch[5] >> 12 ? PhysicalToVirtual(fetch[5] & 0xFFFFF000) : 0;
        bool packedMips = (fetch[5] >> 11) & 1;
        uint32_t pitchTexels = ((fetch[0] >> 22) & 0x1FF) * 32;
        if (pitchTexels == 0)
            pitchTexels = (width + 31) & ~31u;
        uint32_t layers = cube ? 6 : 1;

        // Levels: 0 at the base address, the others at the mip address
        uint32_t maxLevel = (fetch[4] >> 6) & 15;
        uint32_t levels = 1;
        if (mipAddress != 0 && ((fetch[3] >> 23) & 3) != 2) // mip filter "base map": the mips are never sampled
            levels = std::min(maxLevel, 31u - __builtin_clz(std::max(width, height))) + 1;
        uint32_t tailLevel = packedMips ? PackedTailLevel(width, height) : 32;

        // Guest storage of a level (bytes per layer, 4 KB aligned) and its pitch in blocks
        uint32_t bs = info.blockSize, bpb = info.bytesPerBlock;
        auto storage = [&](uint32_t level, uint32_t& pitchBlocks) -> size_t {
            uint32_t levelWidth = level == 0 ? width : std::max(NextPow2(width) >> level, 1u);
            uint32_t levelHeight = level == 0 ? height : std::max(NextPow2(height) >> level, 1u);
            uint32_t blocksWide = (levelWidth + bs - 1) / bs, blocksHigh = (levelHeight + bs - 1) / bs;
            if (level == 0)
                pitchBlocks = std::max(pitchTexels / bs, blocksWide);
            else
                pitchBlocks = tiled ? AlignUp(blocksWide, 32) : AlignUp(blocksWide * bpb, 256) / bpb;
            if (tiled)
                pitchBlocks = AlignUp(pitchBlocks, 32);
            uint32_t rows = tiled ? AlignUp(blocksHigh, 32) : blocksHigh;
            return AlignUp(pitchBlocks * rows * bpb, 4096);
        };

        struct Source
        {
            const uint8_t* data;
            uint32_t pitchBlocks;
            size_t layerStride;
            uint32_t x, y; // block offset (packed tail)
        };
        Source sources[16];
        size_t baseSize = 0, mipsSize = 0, mipOffset = 0;
        for (uint32_t level = 0; level < levels; level++)
        {
            uint32_t storageLevel = std::min(level, tailLevel);
            Source& source = sources[level];
            if (storageLevel == 0)
            {
                source.layerStride = storage(0, source.pitchBlocks);
                source.data = static_cast<const uint8_t*>(GuestToHost(address));
                baseSize = source.layerStride * layers;
            }
            else
            {
                if (level == storageLevel) // new storage: placed after the previous level
                {
                    if (level > 1)
                    {
                        uint32_t unused;
                        mipOffset += storage(level - 1, unused) * layers;
                    }
                    mipsSize = mipOffset + storage(level, source.pitchBlocks) * layers;
                }
                source.layerStride = storage(storageLevel, source.pitchBlocks);
                source.data = static_cast<const uint8_t*>(GuestToHost(mipAddress)) + mipOffset;
            }
            source.x = source.y = 0;
            if (level >= tailLevel)
            {
                PackedMipOffset(width, height, level, source.x, source.y);
                source.x /= bs;
                source.y /= bs;
            }
        }
        if (address == 0 || baseSize > 64u * 1024 * 1024 || mipsSize > 64u * 1024 * 1024)
            return 0;

        uint64_t keyData[3] = { uint64_t(address) | (uint64_t(fetch[2]) << 32),
            uint64_t(fetch[1] & 0xFFF) | (uint64_t(fetch[0] & 0xFFC00000) << 16) | (uint64_t(swizzle) << 48),
            uint64_t(mipAddress) | (uint64_t(levels) << 32) | (uint64_t(dimension) << 40) | (uint64_t(packedMips) << 42) };
        uint64_t key = XXH3_64bits(keyData, sizeof(keyData));
        CachedTexture& cached = g_textures[key];
        addedMips = cached.addedMips;
        if (cached.checkedFrame == g_frame && cached.descriptorIndex != 0)
            return cached.descriptorIndex;

        // Content changed?
        uint64_t contentHash = XXH3_64bits(sources[0].data, baseSize);
        if (mipsSize != 0)
            contentHash ^= XXH3_64bits(GuestToHost(mipAddress), mipsSize) * 0x9E3779B97F4A7C15ull;
        cached.checkedFrame = g_frame;
        if (cached.descriptorIndex != 0 && contentHash == cached.contentHash)
            return cached.descriptorIndex;
        cached.contentHash = contentHash;

        // Untile every level and layer into rows of blocks, then fix the byte order
        RendererTextureDesc desc;
        size_t total = 0;
        for (uint32_t level = 0; level < levels; level++)
        {
            desc.levelOffsets[level] = total;
            uint32_t blocksWide = (std::max(width >> level, 1u) + bs - 1) / bs;
            uint32_t blocksHigh = (std::max(height >> level, 1u) + bs - 1) / bs;
            total += size_t(blocksWide) * blocksHigh * bpb * layers;
        }
        g_decodeBuffer.resize(total);
        for (uint32_t level = 0; level < levels; level++)
        {
            const Source& source = sources[level];
            uint32_t blocksWide = (std::max(width >> level, 1u) + bs - 1) / bs;
            uint32_t blocksHigh = (std::max(height >> level, 1u) + bs - 1) / bs;
            size_t rowBytes = size_t(blocksWide) * bpb;
            for (uint32_t layer = 0; layer < layers; layer++)
            {
                const uint8_t* guest = source.data + layer * source.layerStride;
                uint8_t* out = g_decodeBuffer.data() + desc.levelOffsets[level] + layer * rowBytes * blocksHigh;
                for (uint32_t y = 0; y < blocksHigh; y++)
                {
                    uint8_t* row = out + y * rowBytes;
                    if (!tiled)
                    {
                        memcpy(row, guest + (size_t(y + source.y) * source.pitchBlocks + source.x) * bpb, rowBytes);
                        continue;
                    }
                    for (uint32_t x = 0; x < blocksWide; x++)
                        memcpy(row + size_t(x) * bpb, guest + TiledOffset2D(x + source.x, y + source.y, source.pitchBlocks, bpb), bpb);
                }
            }
        }
        SwapEndian(g_decodeBuffer.data(), g_decodeBuffer.size(), endian);

        static const bool checkMips = getenv("SIMPSONS_CHECK_MIPS") != nullptr;
        if (checkMips && layers == 1)
            CheckMips(address, format, info, width, height, levels, desc.levelOffsets, packedMips);

        desc.width = width;
        desc.height = height;
        desc.format = info.host;
        desc.levels = levels;
        desc.cube = cube;
        desc.data = g_decodeBuffer.data();
        desc.size = g_decodeBuffer.size();
        if (info.convert != Convert::None)
        {
            // 16-bit texels -> RGBA8: every offset doubles
            g_convertBuffer.resize(g_decodeBuffer.size() * 2);
            ConvertTexels(g_decodeBuffer.data(), g_convertBuffer.data(), g_decodeBuffer.size() / 2, info.convert);
            for (uint32_t level = 0; level < levels; level++)
                desc.levelOffsets[level] *= 2;
            desc.data = g_convertBuffer.data();
            desc.size = g_convertBuffer.size();
        }
        memcpy(desc.swizzle, components, 4);
        RendererTextureDesc uploaded = desc;
        if (const uint8_t* replaced = ReplaceTexture(desc))
            uploaded.data = replaced;
        cached.addedMips = addedMips = AddMips(uploaded);
        if (IsBlockCompressed(uploaded.format) && !RendererSupportsFormat(uploaded.format))
            UnpackBlockCompressed(uploaded);
        cached.descriptorIndex = RendererUploadTexture(key, uploaded);

        static const char* dumpFrames = getenv("SIMPSONS_DUMP_TEXTURES");
        if (dumpFrames && !cube)
        {
            uint64_t frame = GpuFrameCount() + 1, first = strtoull(dumpFrames, nullptr, 10);
            const char* dash = strchr(dumpFrames, '-');
            if (frame >= first && frame <= (dash ? strtoull(dash + 1, nullptr, 10) : first))
            {
                RendererTextureDesc base = desc;
                base.size = levels > 1 ? desc.levelOffsets[1] : desc.size;
                DumpTexture(address, format, base);
            }
        }

        static bool trace = getenv("SIMPSONS_TRACE_TEXTURES") != nullptr;
        if (trace)
            fprintf(stderr, "[textures] slot %u : 0x%08X %ux%u%s format %u %s endian %u pitch %u niveaux %u%s swizzle %03X -> %u\n", slot,
                address, width, height, cube ? " cube" : "", format, tiled ? "tilee" : "lineaire", endian, pitchTexels, levels,
                packedMips ? " compactes" : "", swizzle, cached.descriptorIndex);
        return cached.descriptorIndex;
    }

    uint32_t BindSampler(uint32_t slot, uint8_t instructionFilters, bool addedMips)
    {
        uint32_t fetch0 = GpuReadRegister(REG_TEXTURE_FETCH_CONSTANTS + slot * 6);
        uint32_t fetch3 = GpuReadRegister(REG_TEXTURE_FETCH_CONSTANTS + slot * 6 + 3);
        // The tfetch instruction may override each filter (3 = use the fetch constant)
        for (uint32_t i = 0; i < 3; i++)
        {
            uint32_t filter = (instructionFilters >> (i * 2)) & 3;
            if (filter != 3)
                fetch3 = (fetch3 & ~(3u << (19 + i * 2))) | (filter << (19 + i * 2));
        }
        // Filters: 0 point, 1 linear, 2 base map (mips only), 3 use fetch constant (treated as linear)
        auto filter = [](uint32_t value) { return value == 0 ? 0u : 1u; };
        uint32_t mipFilter = addedMips ? 1 : (fetch3 >> 23) & 3; // our mips: trilinear even if the game asked for the base map
        return RendererGetSampler(AddressMode(fetch0 >> 10), AddressMode(fetch0 >> 13), AddressMode(fetch0 >> 16),
            filter((fetch3 >> 19) & 3), filter((fetch3 >> 21) & 3), mipFilter == 3 ? 1 : mipFilter);
    }
}

uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytesPerBlock)
{
    // Xbox 360 2D tiling (XGAddress2DTiledOffset), returns a byte offset
    uint32_t alignedPitch = (pitch + 31) & ~31u;
    uint32_t log2Bpp = (bytesPerBlock >> 2) + ((bytesPerBlock >> 1) >> (bytesPerBlock >> 2));
    uint32_t macro = ((x >> 5) + (y >> 5) * (alignedPitch >> 5)) << (log2Bpp + 7);
    uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log2Bpp;
    uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
    return ((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
        (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void TexturesBind(uint32_t slotMask, const uint8_t* dimensions, const uint8_t* filters, RendererDrawState& state)
{
    for (uint32_t slot = 0; slot < 16; slot++)
    {
        if (!(slotMask & (1u << slot)))
            continue;
        bool addedMips;
        state.textureIndices[slot] = BindTexture(slot, dimensions[slot], addedMips);
        state.samplerIndices[slot] = BindSampler(slot, filters[slot], addedMips);
    }
}

void TexturesNewFrame()
{
    g_frame++;
}
