// Correspondance entre le microcode charge par le GPU (IM_LOAD) et les conteneurs de shaders
// du SDK trouves dans l'executable, que XenosRecomp a traduits a l'avance (shader_cache).
#include "runtime.h"

#include <xbox.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#define XXH_INLINE_ALL
#include <xxhash.h>

struct ShaderContainerHeader
{
    be<uint32_t> flags;
    be<uint32_t> virtualSize;
    be<uint32_t> physicalSize;
    be<uint32_t> fieldC;
    be<uint32_t> constantTableOffset;
    be<uint32_t> definitionTableOffset;
    be<uint32_t> shaderOffset;
    be<uint32_t> field1C;
    be<uint32_t> field20;
};

struct ShaderHeader
{
    be<uint32_t> physicalOffset;
    be<uint32_t> size;
};

struct KnownShader
{
    uint64_t containerHash; // key of the precompiled shader in shader_cache
    bool pixel;
};

static std::unordered_map<uint64_t, KnownShader> g_shadersByMicrocode;
static std::mutex g_shaderStatsMutex;
static std::unordered_set<uint64_t> g_seenMicrocode;
static std::unordered_set<uint64_t> g_unknownMicrocode;

void IndexShaderContainers(const uint8_t* image, size_t size)
{
    for (size_t i = 0; i + sizeof(ShaderContainerHeader) < size; i += 4)
    {
        auto* container = reinterpret_cast<const ShaderContainerHeader*>(image + i);
        if ((container->flags & 0xFFFFFF00) != 0x102A1100 || container->field1C != 0 || container->field20 != 0)
            continue;
        size_t dataSize = size_t(container->virtualSize) + container->physicalSize;
        if (dataSize > size - i || container->shaderOffset + sizeof(ShaderHeader) > container->virtualSize)
            continue;

        auto* shader = reinterpret_cast<const ShaderHeader*>(image + i + container->shaderOffset);
        size_t microcodeOffset = i + container->virtualSize + shader->physicalOffset;
        if (microcodeOffset + shader->size > size || shader->size == 0)
            continue;

        uint64_t microcodeHash = XXH3_64bits(image + microcodeOffset, shader->size);
        g_shadersByMicrocode[microcodeHash] = { XXH3_64bits(container, dataSize), (container->flags & 1) == 0 };
    }
    fprintf(stderr, "[shaders] %zu shaders indexes dans l'executable\n", g_shadersByMicrocode.size());
}

// Called for every microcode upload; returns the precompiled shader if known
const KnownShader* LookupShaderMicrocode(const uint8_t* microcode, uint32_t size)
{
    uint64_t hash = XXH3_64bits(microcode, size);
    auto it = g_shadersByMicrocode.find(hash);

    std::lock_guard lock(g_shaderStatsMutex);
    if (g_seenMicrocode.insert(hash).second && it == g_shadersByMicrocode.end())
    {
        g_unknownMicrocode.insert(hash);
        fprintf(stderr, "[shaders] microcode inconnu %016llX (%u octets)\n", (unsigned long long)hash, size);
        static const bool dump = getenv("SIMPSONS_DUMP_SHADERS") != nullptr; // recomp/microcode_<hash>.bin
        char name[96];
        snprintf(name, sizeof(name), "recomp/microcode_%016llX.bin", (unsigned long long)hash);
        if (FILE* f = dump ? fopen(name, "wb") : nullptr)
        {
            fwrite(microcode, 1, size, f);
            fclose(f);
        }
    }
    return it != g_shadersByMicrocode.end() ? &it->second : nullptr;
}

void PrintShaderStats()
{
    std::lock_guard lock(g_shaderStatsMutex);
    fprintf(stderr, "[shaders] %zu microcodes differents charges, %zu reconnus, %zu inconnus\n",
        g_seenMicrocode.size(), g_seenMicrocode.size() - g_unknownMicrocode.size(), g_unknownMicrocode.size());
}
