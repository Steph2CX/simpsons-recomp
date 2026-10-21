// Partie "processeur de commandes" du rendu : constantes, shaders actifs, dessins.
//
// Lit l'etat du GPU Xenos dans ses registres, convertit les tampons de sommets et d'indices
// (big-endian, formats Xenos) en donnees hote, adapte les primitives que Vulkan ne connait pas
// (rectangles, quads, eventails) et transmet le tout au moteur Vulkan.
#include "runtime.h"
#include "renderer.h"
#include "textures.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

uint32_t GpuReadRegister(uint32_t index);
uint64_t GpuFrameCount();
const uint32_t* GpuRegisterPointer(uint32_t index);
void GpuWriteRegisterBlock(uint32_t index, const uint32_t* bigEndianValues, uint32_t count);
void GpuWriteRegister(uint32_t index, uint32_t value);

namespace
{
    // Register indices
    constexpr uint32_t REG_RB_DEPTHCONTROL = 0x2200;
    constexpr uint32_t REG_RB_BLENDCONTROL0 = 0x2201;
    constexpr uint32_t REG_RB_COLORCONTROL = 0x2202;
    constexpr uint32_t REG_RB_COLOR_MASK = 0x2104;
    constexpr uint32_t REG_RB_ALPHA_REF = 0x210E;
    constexpr uint32_t REG_PA_CL_VPORT_XSCALE = 0x210F;
    constexpr uint32_t REG_PA_CL_VTE_CNTL = 0x2206;
    constexpr uint32_t REG_PA_SU_SC_MODE_CNTL = 0x2205;
    constexpr uint32_t REG_RB_MODECONTROL = 0x2208;
    constexpr uint32_t REG_RB_COPY_CONTROL = 0x2318;
    constexpr uint32_t REG_VGT_INDX_OFFSET = 0x2102;
    constexpr uint32_t REG_ALU_CONSTANTS = 0x4000;
    constexpr uint32_t REG_FETCH_CONSTANTS = 0x4800;
    constexpr uint32_t REG_LOOP_CONSTANTS = 0x4908;
    constexpr uint32_t REG_BOOL_CONSTANTS = 0x4900;

    // SIMPSONS_TRACE_TARGETS=<frame>: surface changes during that frame, with the number of draws in each
    void SelectTargets(bool draw = false)
    {
        uint32_t surface = GpuReadRegister(0x2000), color = GpuReadRegister(0x2001), depth = GpuReadRegister(0x2002);
        static const char* traceFrame = getenv("SIMPSONS_TRACE_TARGETS");
        if (traceFrame && GpuFrameCount() + 1 == uint64_t(atoll(traceFrame)))
        {
            static uint32_t last[3]{}, draws = 0;
            if (surface != last[0] || color != last[1] || depth != last[2])
            {
                fprintf(stderr, "[surfaces] %u dessins ; puis surface %08X couleur %08X profondeur %08X, RB_MODECONTROL %u, VTE 0x%X, ecran %08X %08X\n",
                    draws, surface, color, depth, GpuReadRegister(0x2208) & 7, GpuReadRegister(0x2206), GpuReadRegister(0x200E), GpuReadRegister(0x200F));
                last[0] = surface;
                last[1] = color;
                last[2] = depth;
                draws = 0;
            }
            draws += draw;
        }
        // PA_SC_WINDOW_SCISSOR_BR: x bits 0-14, y bits 16-30 (pixels)
        uint32_t scissor = GpuReadRegister(0x2082);
        uint32_t scissorWidth = scissor & 0x7FFF, scissorHeight = (scissor >> 16) & 0x7FFF;
        if (scissorWidth == 0)
            scissorWidth = 8192;
        if (scissorHeight == 0)
            scissorHeight = 8192;
        // Shadow passes leave the scissor at 8192x8192: the rendering area (loaded and stored whole at each pass on
        // Apple GPUs) is limited to what the surface can hold in EDRAM: its pitch, and the rows of 16-sample tiles
        // (80 samples wide, 40 at 64 bits per pixel) between its base and the next surface or the end of EDRAM
        uint32_t pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
        uint32_t colorBase = color & 0xFFF, depthBase = depth & 0xFFF, colorFormat = (color >> 16) & 0xF;
        static const bool unbounded = getenv("SIMPSONS_SANS_BORNE_EDRAM") != nullptr; // tests: previous behaviour
        if (pitch != 0 && !unbounded)
        {
            uint32_t samplesX = msaa >= 2 ? 2 : 1, samplesY = msaa >= 1 ? 2 : 1;
            bool wide = colorFormat == 5 || colorFormat == 7 || colorFormat == 15;
            uint32_t colorTiles = (pitch * samplesX + (wide ? 39 : 79)) / (wide ? 40 : 80);
            uint32_t depthTiles = (pitch * samplesX + 79) / 80;
            uint32_t colorEnd = depthBase > colorBase ? depthBase : 2048, depthEnd = colorBase > depthBase ? colorBase : 2048;
            uint32_t colorRows = (colorEnd - std::min(colorBase, colorEnd)) / colorTiles * 16 / samplesY;
            uint32_t depthRows = (depthEnd - std::min(depthBase, depthEnd)) / depthTiles * 16 / samplesY;
            scissorWidth = std::min(scissorWidth, pitch);
            scissorHeight = std::min(scissorHeight, std::max({ colorRows, depthRows, 16u }));
        }
        RendererSetTargets(msaa, colorBase, colorFormat, depthBase, scissorWidth, scissorHeight);
    }

    uint64_t g_vertexShader = 0;

    // Why draws are not rendered (printed with SIMPSONS_STATS)
    enum Rejection { RejectCopy, RejectPoints, RejectNoVertexShader, RejectNoPixelShader, RejectRange, RejectPrimitive, RejectCount };
    const char* g_rejectionNames[RejectCount] = { "copie", "points", "vertex shader absent", "pixel shader absent", "plage d'indices", "primitive" };
    uint64_t g_rejections[RejectCount]{};
    uint64_t g_drawsSubmitted = 0;
    uint64_t g_pixelShader = 0;

    float RegisterFloat(uint32_t index)
    {
        uint32_t bits = GpuReadRegister(index);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    uint32_t ReadDword(uint32_t address, uint32_t endian)
    {
        uint32_t raw;
        memcpy(&raw, GuestToHost(address), 4);
        switch (endian)
        {
        case 1: return ((raw & 0x00FF00FF) << 8) | ((raw >> 8) & 0x00FF00FF); // 8 in 16
        case 2: return __builtin_bswap32(raw); // 8 in 32
        case 3: return (raw >> 16) | (raw << 16); // 16 in 32
        default: return raw;
        }
    }

    template <uint32_t Endian>
    inline uint32_t LoadDword(const uint8_t* p)
    {
        uint32_t raw;
        memcpy(&raw, p, 4);
        if constexpr (Endian == 1)
            return ((raw & 0x00FF00FF) << 8) | ((raw >> 8) & 0x00FF00FF); // 8 in 16
        else if constexpr (Endian == 2)
            return __builtin_bswap32(raw); // 8 in 32
        else if constexpr (Endian == 3)
            return (raw >> 16) | (raw << 16); // 16 in 32
        else
            return raw;
    }

    inline float HalfToFloat(uint16_t half)
    {
        _Float16 value;
        memcpy(&value, &half, 2);
        return float(value); // one hardware conversion on arm64
    }

    // One packed component; the width is a constant once inlined, the scale a multiplication
    template <uint32_t Width>
    inline float Component(uint32_t bits, bool isSigned, bool normalized)
    {
        constexpr uint32_t mask = Width == 32 ? 0xFFFFFFFF : (1u << Width) - 1;
        bits &= mask;
        if (isSigned)
        {
            int32_t value = int32_t(bits << (32 - Width)) >> (32 - Width);
            constexpr float scale = 1.0f / float((1u << (Width - 1)) - 1);
            return normalized ? std::max(-1.0f, float(value) * scale) : float(value);
        }
        constexpr float scale = 1.0f / float(mask);
        return normalized ? float(bits) * scale : float(bits);
    }

    // Decodes one attribute of <count> vertices into float4s (missing components default to 0, 0, 0, 1);
    // the format and endianness are chosen once for the whole run
    template <uint32_t Endian>
    void DecodeRun(const XenosVertexAttribute& attribute, const uint8_t* source, size_t strideBytes, uint32_t count,
        float* out, size_t outStride)
    {
        const bool s = attribute.isSigned, n = attribute.isNormalized;
        auto asFloat = [](uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; };
        auto run = [&](auto decode)
        {
            for (uint32_t v = 0; v < count; v++, source += strideBytes, out += outStride)
            {
                out[0] = out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                decode(source, out);
            }
        };
        auto dword = [](const uint8_t* p, uint32_t i) { return LoadDword<Endian>(p + i * 4); };

        switch (attribute.format)
        {
        case 6: // k_8_8_8_8
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                for (uint32_t i = 0; i < 4; i++)
                    o[i] = Component<8>(v >> (i * 8), s, n);
            });
            break;
        case 7: // k_2_10_10_10
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                o[0] = Component<10>(v, s, n);
                o[1] = Component<10>(v >> 10, s, n);
                o[2] = Component<10>(v >> 20, s, n);
                o[3] = Component<2>(v >> 30, s, n);
            });
            break;
        case 16: // k_10_11_11
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                o[0] = Component<11>(v, s, n);
                o[1] = Component<11>(v >> 11, s, n);
                o[2] = Component<10>(v >> 22, s, n);
            });
            break;
        case 17: // k_11_11_10
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                o[0] = Component<10>(v, s, n);
                o[1] = Component<11>(v >> 10, s, n);
                o[2] = Component<11>(v >> 21, s, n);
            });
            break;
        case 25: // k_16_16
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                o[0] = Component<16>(v, s, n);
                o[1] = Component<16>(v >> 16, s, n);
            });
            break;
        case 26: // k_16_16_16_16
            run([&](const uint8_t* p, float* o) {
                for (uint32_t i = 0; i < 2; i++)
                {
                    uint32_t v = dword(p, i);
                    o[i * 2] = Component<16>(v, s, n);
                    o[i * 2 + 1] = Component<16>(v >> 16, s, n);
                }
            });
            break;
        case 31: // k_16_16_FLOAT
            run([&](const uint8_t* p, float* o) {
                uint32_t v = dword(p, 0);
                o[0] = HalfToFloat(uint16_t(v));
                o[1] = HalfToFloat(uint16_t(v >> 16));
            });
            break;
        case 32: // k_16_16_16_16_FLOAT
            run([&](const uint8_t* p, float* o) {
                for (uint32_t i = 0; i < 2; i++)
                {
                    uint32_t v = dword(p, i);
                    o[i * 2] = HalfToFloat(uint16_t(v));
                    o[i * 2 + 1] = HalfToFloat(uint16_t(v >> 16));
                }
            });
            break;
        case 33: case 34: case 35: // k_32, k_32_32, k_32_32_32_32
        {
            uint32_t components = attribute.format - 32;
            run([&](const uint8_t* p, float* o) {
                for (uint32_t i = 0; i < components; i++)
                    o[i] = Component<32>(dword(p, i), s, false);
            });
            break;
        }
        case 36: run([&](const uint8_t* p, float* o) { o[0] = asFloat(dword(p, 0)); }); break; // k_32_FLOAT
        case 37: run([&](const uint8_t* p, float* o) { for (uint32_t i = 0; i < 2; i++) o[i] = asFloat(dword(p, i)); }); break; // k_32_32_FLOAT
        case 38: run([&](const uint8_t* p, float* o) { for (uint32_t i = 0; i < 4; i++) o[i] = asFloat(dword(p, i)); }); break; // k_32_32_32_32_FLOAT
        case 57: run([&](const uint8_t* p, float* o) { for (uint32_t i = 0; i < 3; i++) o[i] = asFloat(dword(p, i)); }); break; // k_32_32_32_FLOAT
        default:
        {
            run([](const uint8_t*, float*) {});
            static bool reported[64]{};
            if (attribute.format < 64 && !reported[attribute.format])
            {
                reported[attribute.format] = true;
                fprintf(stderr, "[rendu] format de sommet %u non gere\n", attribute.format);
            }
            break;
        }
        }
    }

    void DecodeAttributes(const XenosVertexAttribute& attribute, uint32_t address, uint32_t endian, uint32_t count,
        float* out, size_t outStride)
    {
        const uint8_t* source = static_cast<const uint8_t*>(GuestToHost(address));
        size_t stride = size_t(attribute.strideDwords) * 4;
        switch (endian)
        {
        case 1: DecodeRun<1>(attribute, source, stride, count, out, outStride); break;
        case 2: DecodeRun<2>(attribute, source, stride, count, out, outStride); break;
        case 3: DecodeRun<3>(attribute, source, stride, count, out, outStride); break;
        default: DecodeRun<0>(attribute, source, stride, count, out, outStride); break;
        }
    }

    void ReadConstants(uint32_t base, float* out)
    {
        // Registers are big-endian words in guest memory: byte-swapped as a block
        const uint32_t* source = GpuRegisterPointer(base);
        uint32_t* destination = reinterpret_cast<uint32_t*>(out);
        for (uint32_t i = 0; i < 256 * 4; i++)
            destination[i] = __builtin_bswap32(source[i]);
    }

    // Converts the clip space of the Xenos viewport (PA_CL_VPORT / PA_CL_VTE_CNTL) to Vulkan's
    void ComputeViewportTransform(RendererDrawState& state)
    {
        uint32_t vte = GpuReadRegister(REG_PA_CL_VTE_CNTL);
        float scale[3], offset[3];
        for (uint32_t i = 0; i < 3; i++)
        {
            scale[i] = (vte & (1u << (i * 2))) ? RegisterFloat(REG_PA_CL_VPORT_XSCALE + i * 2) : 1.0f;
            offset[i] = (vte & (2u << (i * 2))) ? RegisterFloat(REG_PA_CL_VPORT_XSCALE + i * 2 + 1) : 0.0f;
        }
        // Converted to clip space by the renderer, which knows the size of the bound surface
        for (uint32_t i = 0; i < 3; i++)
        {
            state.positionScale[i] = scale[i];
            state.positionOffset[i] = offset[i];
        }

        static bool trace = getenv("SIMPSONS_TRACE_VIEWPORT") != nullptr;
        if (trace)
        {
            static std::vector<std::array<float, 7>> seen;
            std::array<float, 7> key{ float(vte), scale[0], scale[1], scale[2], offset[0], offset[1], offset[2] };
            if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 32)
            {
                seen.push_back(key);
                fprintf(stderr, "[rendu] viewport VTE 0x%X echelle %g %g %g decalage %g %g %g\n", vte, scale[0], scale[1], scale[2],
                    offset[0], offset[1], offset[2]);
            }
        }
    }

    // Source rectangle of a resolve: the copy is a rectangle-list draw whose 3 vertices (float x, y in pixels)
    // give the corners. Returns false if they cannot be read.
    bool CopyRectangle(uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height)
    {
        const XenosShader* vs = RendererGetShader(g_vertexShader);
        if (vs == nullptr || vs->attributes.empty())
            return false;
        auto& attribute = vs->attributes[0];
        uint32_t fetch0 = GpuReadRegister(REG_FETCH_CONSTANTS + attribute.fetchConstant * 2);
        uint32_t fetch1 = GpuReadRegister(REG_FETCH_CONSTANTS + attribute.fetchConstant * 2 + 1);
        uint32_t base = PhysicalToVirtual(fetch0 & 0xFFFFFFFC);
        if (base == 0)
            return false;
        uint32_t stride = std::max<uint32_t>(attribute.strideDwords, 2) * 4;
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (uint32_t v = 0; v < 3; v++)
        {
            uint32_t address = base + v * stride + attribute.offsetDwords * 4;
            float px, py;
            uint32_t bx = ReadDword(address, fetch1 & 3), by = ReadDword(address + 4, fetch1 & 3);
            memcpy(&px, &bx, 4);
            memcpy(&py, &by, 4);
            if (!std::isfinite(px) || !std::isfinite(py))
                return false;
            minX = std::min(minX, px); maxX = std::max(maxX, px);
            minY = std::min(minY, py); maxY = std::max(maxY, py);
        }
        if (GpuReadRegister(REG_PA_SU_SC_MODE_CNTL) & (1u << 16)) // window offset applies to vertices
        {
            uint32_t offset = GpuReadRegister(0x2080);
            int32_t ox = int32_t(offset << 17) >> 17, oy = int32_t(offset << 1) >> 17;
            minX += ox; maxX += ox; minY += oy; maxY += oy;
        }
        minX = std::max(minX, 0.0f);
        minY = std::max(minY, 0.0f);
        if (maxX <= minX || maxY <= minY || maxX > 8192 || maxY > 8192)
            return false;
        x = uint32_t(minX);
        y = uint32_t(minY);
        width = uint32_t(maxX) - x;
        height = uint32_t(maxY) - y;
        return true;
    }

    void HandleCopy()
    {
        SelectTargets();
        // Resolve ("copy") with optional clears of the render target
        uint32_t copyControl = GpuReadRegister(REG_RB_COPY_CONTROL);
        bool clearColor = (copyControl >> 8) & 1;
        bool clearDepth = (copyControl >> 9) & 1;
        // SIMPSONS_TRACE_COPY=<first copy index>: prints 60 copies from that index
        static int traced = 0;
        static const char* traceFrom = getenv("SIMPSONS_TRACE_COPY");
        int index = traced++;
        // =autres: only copies to destinations other than the first two seen (front buffers)
        static uint32_t seenDestinations[3]{};
        bool unusual = false;
        if (traceFrom && strcmp(traceFrom, "autres") == 0)
        {
            uint32_t destination = GpuReadRegister(0x2319);
            unusual = true;
            for (uint32_t& seen : seenDestinations)
            {
                if (seen == destination)
                {
                    unusual = false;
                    break;
                }
                if (seen == 0)
                {
                    seen = destination;
                    unusual = false;
                    break;
                }
            }
        }
        static int unusualTraced = 0;
        // =i<frame>: every copy of that frame
        bool frameMatch = traceFrom && traceFrom[0] == 'i' && GpuFrameCount() + 1 == uint64_t(atoll(traceFrom + 1));
        if (traceFrom && (frameMatch || (unusual && unusualTraced++ < 60) ||
            (isdigit(traceFrom[0]) && index >= atoi(traceFrom) && index < atoi(traceFrom) + 60)))
        {
            fprintf(stderr, "[rendu] copie %d (dessins transmis %llu) : surface %08X couleur %08X profondeur %08X, controle 0x%X, 0x2318-0x2320 :", index, (unsigned long long)g_drawsSubmitted, GpuReadRegister(0x2000), GpuReadRegister(0x2001), GpuReadRegister(0x2002), copyControl);
            for (uint32_t r = 0x2318; r <= 0x2320; r++)
                fprintf(stderr, " %08X", GpuReadRegister(r));
            uint32_t rx = 0, ry = 0, rw = 0, rh = 0;
            bool ok = CopyRectangle(rx, ry, rw, rh);
            fprintf(stderr, " rectangle %s %u,%u %ux%u\n", ok ? "" : "?", rx, ry, rw, rh);
        }
        // Colour copies (source 0-3; 4 = depth) become host textures, before the optional clears.
        // RB_COPY_DEST_BASE (physical), RB_COPY_DEST_PITCH (pitch 0-13, height 16-29), RB_COPY_DEST_INFO (bit 24: red/blue swap)
        uint32_t source = copyControl & 7;
        uint32_t command = (copyControl >> 20) & 3; // 0 raw, 1 convert, 2 constant, 3 null
        uint32_t pitch = GpuReadRegister(0x231A);
        uint32_t destWidth = pitch & 0x3FFF, destHeight = (pitch >> 16) & 0x3FFF;
        uint32_t x = 0, y = 0, width = destWidth, height = destHeight;
        if (!CopyRectangle(x, y, width, height))
        {
            x = y = 0;
            width = destWidth;
            height = destHeight;
        }
        if (source < 4 && command <= 1)
            RendererResolve(GpuReadRegister(0x2319) & 0x1FFFF000, destWidth, destHeight, x, y, width, height,
                (GpuReadRegister(0x231B) >> 24) & 1);
        else if (source == 4 && command <= 1)
            RendererResolveDepth(GpuReadRegister(0x2319) & 0x1FFFF000, destWidth, destHeight);

        uint32_t clearValue = GpuReadRegister(0x231E); // RB_COLOR_CLEAR (A8R8G8B8)
        float color[4] = { ((clearValue >> 16) & 0xFF) / 255.0f, ((clearValue >> 8) & 0xFF) / 255.0f,
            (clearValue & 0xFF) / 255.0f, ((clearValue >> 24) & 0xFF) / 255.0f };
        uint32_t depthValue = GpuReadRegister(0x231D); // RB_DEPTH_CLEAR (24-bit depth, 8-bit stencil)
        RendererClear(clearColor, color, clearDepth, float(depthValue >> 8) / 16777215.0f, uint8_t(depthValue), x, y, width, height);
    }
}

uint32_t GpuShaderMicrocodeDebugType(bool pixel);

void GpuLoadShader(bool pixelShader, const uint8_t* microcode, uint32_t sizeBytes)
{
    uint64_t hash = RendererSetShader(pixelShader, microcode, sizeBytes);
    (pixelShader ? g_pixelShader : g_vertexShader) = hash;
}

void GpuSetConstants(uint32_t type, uint32_t index, const uint32_t* bigEndianValues, uint32_t count)
{
    // Constant types: 0 = ALU, 1 = fetch, 2 = bool, 3 = loop, 4 = registers
    static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
    if (type >= std::size(bases))
        return;
    GpuWriteRegisterBlock(bases[type] + index, bigEndianValues, count);
}

// SIMPSONS_PROFIL_GPU=1: time spent in each stage of GpuDraw, printed every 600 frames
namespace
{
    const bool g_profileGpu = getenv("SIMPSONS_PROFIL_GPU") != nullptr;
    uint64_t g_stageTime[8]{};
    uint64_t g_profiledDraws = 0;
    uint64_t g_hashedBytes[3]{}; // indices, vertices, computed-index streams
    const char* g_stageNames[8] = { "indices", "sommets", "primitives", "etat", "textures", "cibles", "rendu", "autre" };
    uint64_t NowNanoseconds()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    struct StageTimer
    {
        uint64_t start = g_profileGpu ? NowNanoseconds() : 0;
        void Next(int stage)
        {
            if (!g_profileGpu)
                return;
            uint64_t now = NowNanoseconds();
            g_stageTime[stage] += now - start;
            start = now;
        }
    };
}

void GpuProfileReport(uint64_t frame)
{
    if (!g_profileGpu || frame % 600 != 0 || g_profiledDraws == 0)
        return;
    fprintf(stderr, "[profil] %llu dessins :", (unsigned long long)g_profiledDraws);
    for (int i = 0; i < 7; i++)
        fprintf(stderr, " %s %.0f ms", g_stageNames[i], g_stageTime[i] / 1e6);
    fprintf(stderr, " | hache Mo : indices %.0f sommets %.0f flux %.0f\n", g_hashedBytes[0] / 1e6, g_hashedBytes[1] / 1e6,
        g_hashedBytes[2] / 1e6);
    memset(g_hashedBytes, 0, sizeof(g_hashedBytes));
    memset(g_stageTime, 0, sizeof(g_stageTime));
    g_profiledDraws = 0;
}

void GpuDraw(uint32_t drawInitiator, uint32_t indexBase, uint32_t indexInfo, const uint32_t* immediateIndices)
{
    uint32_t primitive = drawInitiator & 0x3F;
    uint32_t sourceSelect = (drawInitiator >> 6) & 3; // 0 = DMA, 1 = immediate, 2 = auto
    bool indices32 = (drawInitiator >> 11) & 1;
    uint32_t count = drawInitiator >> 16;

    uint32_t mode = GpuReadRegister(REG_RB_MODECONTROL) & 7;
    if (mode == 6) // copy (resolve)
    {
        g_rejections[RejectCopy]++;
        HandleCopy();
        return;
    }
    if (count == 0 || primitive == 1) // nothing, or points (not supported yet)
    {
        g_rejections[RejectPoints]++;
        return;
    }

    const XenosShader* vs = RendererGetShader(g_vertexShader);
    if (vs == nullptr)
    {
        g_rejections[RejectNoVertexShader]++;
        return;
    }
    if (RendererGetShader(g_pixelShader) == nullptr)
    {
        g_rejections[RejectNoPixelShader]++;
        return;
    }

    StageTimer timer;
    g_profiledDraws++;
    // Indices, rebased on the smallest used vertex. Index buffers in memory are cached: decoded again only when
    // their bytes changed
    uint32_t offset = GpuReadRegister(REG_VGT_INDX_OFFSET);
    uint32_t restart = indices32 ? 0xFFFFFFFF : 0xFFFF;
    uint32_t endian = (indexInfo >> 30) & 3;
    struct CachedIndices
    {
        uint64_t contentHash = 0;
        std::vector<uint32_t> indices; // rebased
        uint32_t minIndex = 0, maxIndex = 0;
    };
    static std::unordered_map<uint64_t, CachedIndices> indexCache;
    static size_t indexCacheBytes = 0;
    uint64_t indexKey = 0, indexContent = 0;
    CachedIndices* cachedIndices = nullptr;
    if (sourceSelect == 0)
    {
        uint64_t keyData[3] = { (uint64_t(indexBase) << 32) | count, (uint64_t(offset) << 32) | (uint64_t(indices32) << 2) | endian,
            0 };
        indexKey = XXH3_64bits(keyData, sizeof(keyData));
        indexContent = XXH3_64bits(GuestToHost(PhysicalToVirtual(indexBase)), size_t(count) * (indices32 ? 4 : 2));
        g_hashedBytes[0] += size_t(count) * (indices32 ? 4 : 2);
        if (indexCache.size() > 20000 || (RuntimeLowMemory() && indexCacheBytes > (64u << 20)))
        {
            indexCache.clear();
            indexCacheBytes = 0;
        }
        cachedIndices = &indexCache[indexKey];
    }
    std::vector<uint32_t> indices;
    const std::vector<uint32_t>* indexList = &indices; // cached indices are read in place
    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    bool indicesReady = false;
    if (cachedIndices && !cachedIndices->indices.empty() && cachedIndices->contentHash == indexContent)
    {
        indexList = &cachedIndices->indices;
        minIndex = cachedIndices->minIndex;
        maxIndex = cachedIndices->maxIndex;
        indicesReady = true;
    }
    else
        indices.resize(count);
    auto store = [&](uint32_t i, uint32_t index)
    {
        if (index == restart)
        {
            indices[i] = 0xFFFFFFFF;
            return;
        }
        index += offset;
        indices[i] = index;
        minIndex = std::min(minIndex, index);
        maxIndex = std::max(maxIndex, index);
    };
    if (indicesReady)
    {
    }
    else if (sourceSelect == 2) // auto-generated
    {
        for (uint32_t i = 0; i < count; i++)
            store(i, i);
    }
    else if (sourceSelect == 0 && indices32)
    {
        const uint8_t* source = static_cast<const uint8_t*>(GuestToHost(PhysicalToVirtual(indexBase)));
        for (uint32_t i = 0; i < count; i++)
            store(i, ReadDword(HostToGuest(source + i * 4), endian));
    }
    else
    {
        // 16-bit indices from memory (swapped by 8-in-16 or 8-in-32) or immediate (big-endian)
        const uint16_t* source = sourceSelect == 0 ? static_cast<const uint16_t*>(GuestToHost(PhysicalToVirtual(indexBase)))
                                                   : reinterpret_cast<const uint16_t*>(immediateIndices);
        bool swap = sourceSelect != 0 || endian == 1 || endian == 2;
        for (uint32_t i = 0; i < count; i++)
        {
            uint16_t raw;
            memcpy(&raw, source + i, 2);
            store(i, swap ? __builtin_bswap16(raw) : raw);
        }
    }
    if (minIndex == UINT32_MAX || maxIndex - minIndex > 1u << 20)
    {
        g_rejections[RejectRange]++;
        return;
    }
    if (!indicesReady)
    {
        for (uint32_t& index : indices)
        {
            if (index != 0xFFFFFFFF)
                index -= minIndex;
        }
        if (cachedIndices)
        {
            indexCacheBytes += (indices.size() - cachedIndices->indices.size()) * sizeof(uint32_t);
            cachedIndices->indices = indices;
            cachedIndices->minIndex = minIndex;
            cachedIndices->maxIndex = maxIndex;
            cachedIndices->contentHash = indexContent;
        }
    }

    timer.Next(0);
    // Vertices: every attribute decoded to float4. Decoded arrays are cached: the raw bytes are
    // hashed (cheap) and decoded again only when they changed
    uint32_t attributeCount = uint32_t(vs->attributes.size());
    uint32_t vertexCount = maxIndex - minIndex + 1;
    struct Stream
    {
        uint32_t base, endian;
    };
    Stream streams[16];
    uint64_t layout[2 + 16 * 3];
    uint32_t layoutWords = 0;
    layout[layoutWords++] = g_vertexShader;
    layout[layoutWords++] = (uint64_t(minIndex) << 32) | vertexCount;
    uint64_t contentHash = 0;
    for (uint32_t a = 0; a < attributeCount && a < 16; a++)
    {
        auto& attribute = vs->attributes[a];
        uint32_t fetch0 = GpuReadRegister(REG_FETCH_CONSTANTS + attribute.fetchConstant * 2);
        uint32_t fetch1 = GpuReadRegister(REG_FETCH_CONSTANTS + attribute.fetchConstant * 2 + 1);
        streams[a] = { PhysicalToVirtual(fetch0 & 0xFFFFFFFC), fetch1 & 3 };
        layout[layoutWords++] = (uint64_t(streams[a].base) << 32) | (uint64_t(streams[a].endian) << 30) | attribute.offsetDwords;
        // A stream shared by several attributes is hashed once
        bool hashed = false;
        for (uint32_t b = 0; b < a; b++)
            hashed |= streams[b].base == streams[a].base && vs->attributes[b].strideDwords == attribute.strideDwords;
        if (!hashed)
        {
            size_t stride = size_t(std::max<uint32_t>(attribute.strideDwords, 1)) * 4;
            contentHash = (contentHash * 0x9E3779B97F4A7C15ull) ^
                XXH3_64bits(GuestToHost(streams[a].base + uint32_t(minIndex * stride)), vertexCount * stride);
            g_hashedBytes[1] += vertexCount * stride;
        }
    }
    uint64_t layoutHash = XXH3_64bits(layout, layoutWords * sizeof(uint64_t));

    struct CachedVertices
    {
        uint64_t contentHash = 0;
        std::vector<float> data;
    };
    static std::unordered_map<uint64_t, CachedVertices> vertexCache;
    static size_t vertexCacheBytes = 0;
    if (vertexCacheBytes > (RuntimeLowMemory() ? (128u << 20) : (512u << 20)))
    {
        vertexCache.clear();
        vertexCacheBytes = 0;
    }
    CachedVertices& cached = vertexCache[layoutHash];
    if (cached.data.empty() || cached.contentHash != contentHash)
    {
        vertexCacheBytes -= cached.data.size() * sizeof(float);
        cached.data.resize(size_t(vertexCount) * attributeCount * 4); // every float is written below
        cached.contentHash = contentHash;
        for (uint32_t a = 0; a < attributeCount; a++)
        {
            auto& attribute = vs->attributes[a];
            uint32_t address = streams[a].base + (minIndex * attribute.strideDwords + attribute.offsetDwords) * 4;
            DecodeAttributes(attribute, address, streams[a].endian, vertexCount, &cached.data[size_t(a) * 4],
                size_t(attributeCount) * 4);
        }
        vertexCacheBytes += cached.data.size() * sizeof(float);
    }
    // Decoded vertices are used in place; rectangle lists add vertices to a copy
    std::vector<float> extended;
    const float* vertexData = cached.data.data();

    timer.Next(1);
    // Primitives Vulkan does not have
    RendererDrawState state;
    state.vertexIndexBase = minIndex; // r0.x = host vertex index + base

    // Fetches at an index computed by the shader (particles: one element per quad): the whole stream is decoded
    // and read by the shader at that index
    for (uint32_t a = 0; a < attributeCount && a < 8; a++)
    {
        auto& attribute = vs->attributes[a];
        if (!attribute.computedIndex || attribute.strideDwords == 0)
            continue;
        uint32_t fetch1 = GpuReadRegister(REG_FETCH_CONSTANTS + attribute.fetchConstant * 2 + 1);
        uint32_t elements = std::min<uint32_t>(((fetch1 >> 2) & 0xFFFFFF) / attribute.strideDwords, 1u << 20);
        // The game's particle ring hands each draw "the rest of the ring" (~16000 elements of 64 bytes) while a draw
        // reads one element per quad (at most a few dozen). The computed index never exceeds the vertex index, so
        // the stream is cut to maxIndex + 1 elements (plus a margin): hashing and decoding the whole ring per draw
        // cost up to half of the GPU thread in NEVERQUEST.
        elements = std::min(elements, maxIndex + 1 + 256);
        // Decoded streams are cached (raw bytes hashed) and uploaded once per frame
        struct DecodedStream
        {
            uint64_t contentHash = 0;
            std::vector<float> data;
            uint64_t uploadEpoch = UINT64_MAX;
            uint64_t address = 0;
        };
        static std::unordered_map<uint64_t, DecodedStream> streamCache;
        static size_t streamCacheBytes = 0;
        if (streamCache.size() > 4096 || (RuntimeLowMemory() && streamCacheBytes > (64u << 20)))
        {
            streamCache.clear();
            streamCacheBytes = 0;
        }
        uint64_t keyData[3] = { (uint64_t(streams[a].base) << 32) | elements,
            (uint64_t(attribute.strideDwords) << 32) | (uint64_t(attribute.offsetDwords) << 16) | (attribute.format << 2) | streams[a].endian,
            uint64_t(attribute.isSigned) | (uint64_t(attribute.isNormalized) << 1) };
        DecodedStream& stream = streamCache[XXH3_64bits(keyData, sizeof(keyData))];
        uint64_t raw = XXH3_64bits(GuestToHost(streams[a].base), size_t(std::max(elements, 1u)) * attribute.strideDwords * 4);
        g_hashedBytes[2] += size_t(std::max(elements, 1u)) * attribute.strideDwords * 4;
        if (stream.data.empty() || stream.contentHash != raw)
        {
            streamCacheBytes -= stream.data.size() * sizeof(float);
            stream.data.assign(size_t(std::max(elements, 1u)) * 4, 0.0f);
            streamCacheBytes += stream.data.size() * sizeof(float);
            DecodeAttributes(attribute, streams[a].base + attribute.offsetDwords * 4, streams[a].endian, elements,
                stream.data.data(), 4);
            stream.contentHash = raw;
            stream.uploadEpoch = UINT64_MAX;
        }
        uint64_t epoch = RendererUploadEpoch();
        if (stream.uploadEpoch != epoch || stream.address == 0)
        {
            stream.address = RendererUploadBuffer(stream.data.data(), stream.data.size() * sizeof(float));
            stream.uploadEpoch = epoch;
        }
        state.fetchBuffers[a] = stream.address;
    }
    std::vector<uint32_t> converted;
    const std::vector<uint32_t>& in = *indexList;
    switch (primitive)
    {
    case 2: state.topology = RendererTopology::LineList; break;
    case 3: state.topology = RendererTopology::LineStrip; state.primitiveRestart = true; break;
    case 6: state.topology = RendererTopology::TriangleStrip; state.primitiveRestart = true; break;
    case 5: // triangle fan
        for (uint32_t i = 1; i + 1 < count; i++)
            converted.insert(converted.end(), { in[0], in[i], in[i + 1] });
        indices.swap(converted);
        indexList = &indices;
        break;
    case 13: // quad list
        for (uint32_t i = 0; i + 3 < count; i += 4)
            converted.insert(converted.end(), { in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3] });
        indices.swap(converted);
        indexList = &indices;
        break;
    case 8: // rectangle list: 3 corners per rectangle, the fourth is opposite the right angle
    {
        extended = cached.data;
        auto& vertices = extended;
        for (uint32_t i = 0; i + 2 < count; i += 3)
        {
            uint32_t v[3] = { in[i], in[i + 1], in[i + 2] };
            size_t stride = size_t(attributeCount) * 4;
            // The longest edge (measured on the first attribute, the position) is the diagonal
            auto distance = [&](uint32_t a, uint32_t b)
            {
                float dx = vertices[a * stride] - vertices[b * stride], dy = vertices[a * stride + 1] - vertices[b * stride + 1];
                return dx * dx + dy * dy;
            };
            uint32_t corner = 0;
            float d01 = distance(v[0], v[1]), d02 = distance(v[0], v[2]), d12 = distance(v[1], v[2]);
            if (d01 >= d02 && d01 >= d12)
                corner = 2;
            else if (d02 >= d01 && d02 >= d12)
                corner = 1;
            uint32_t a = v[(corner + 1) % 3], b = v[(corner + 2) % 3], c = v[corner];
            uint32_t d = vertexCount++;
            for (uint32_t k = 0; k < attributeCount * 4; k++)
                vertices.push_back(vertices[a * stride + k] + vertices[b * stride + k] - vertices[c * stride + k]);
            converted.insert(converted.end(), { c, a, b, a, d, b });
        }
        indices.swap(converted);
        indexList = &indices;
        vertexData = extended.data();
        break;
    }
    case 4: break;
    default:
    {
        static bool reported[64]{};
        if (!reported[primitive])
        {
            reported[primitive] = true;
            fprintf(stderr, "[rendu] primitive %u non geree\n", primitive);
        }
        g_rejections[RejectPrimitive]++;
        return;
    }
    }

    timer.Next(2);
    // Render state
    static float vertexConstants[256 * 4], pixelConstants[256 * 4];
    ReadConstants(REG_ALU_CONSTANTS, vertexConstants);
    ReadConstants(REG_ALU_CONSTANTS + 256 * 4, pixelConstants);
    state.vertexShader = g_vertexShader;
    state.pixelShader = g_pixelShader;
    state.vertexConstants = vertexConstants;
    state.pixelConstants = pixelConstants;
    state.booleans = (GpuReadRegister(REG_BOOL_CONSTANTS) & 0xFFFF) | ((GpuReadRegister(REG_BOOL_CONSTANTS + 4) & 0xFFFF) << 16);
    state.blendControl = GpuReadRegister(REG_RB_BLENDCONTROL0);
    state.depthControl = GpuReadRegister(REG_RB_DEPTHCONTROL);
    state.modeControl = GpuReadRegister(REG_PA_SU_SC_MODE_CNTL);
    state.colorMask = mode == 5 ? 0 : GpuReadRegister(REG_RB_COLOR_MASK) & 0xF; // mode 5: depth only
    uint32_t colorControl = GpuReadRegister(REG_RB_COLORCONTROL);
    uint32_t alphaFunction = colorControl & 7;
    state.alphaTest = (colorControl & 8) && (alphaFunction == 4 || alphaFunction == 6); // greater, greater or equal
    state.alphaReference = RegisterFloat(REG_RB_ALPHA_REF);
    ComputeViewportTransform(state);
    for (uint32_t i = 0; i < 32; i++)
        state.loopConstants[i] = GpuReadRegister(REG_LOOP_CONSTANTS + i);

    timer.Next(3);
    // Textures read by either shader (slot dimensions come from the tfetch instructions)
    const XenosShader* ps = RendererGetShader(g_pixelShader);
    uint8_t dimensions[32], filters[32];
    for (uint32_t i = 0; i < 32; i++)
    {
        bool fromPixel = ps->textureSlots & (1u << i);
        dimensions[i] = fromPixel ? ps->textureDimensions[i] : vs->textureDimensions[i];
        filters[i] = fromPixel ? ps->textureFilters[i] : vs->textureFilters[i];
    }
    TexturesBind(vs->textureSlots | ps->textureSlots, dimensions, filters, state);

    timer.Next(4);
    SelectTargets(true);
    timer.Next(5);
    static const char* traceDraws = getenv("SIMPSONS_TRACE_TARGETS");
    if (traceDraws && GpuFrameCount() + 1 == uint64_t(atoll(traceDraws)))
        fprintf(stderr, "[dessin] surface %08X couleur %08X prof %08X mode %u masque %X profctl %08X VTE %X echelle %g,%g decalage %g,%g prim %u n=%zu VS %016llX PS %016llX fenetre %08X scissor %08X %08X modecntl %08X couleurctl %08X\n",
            GpuReadRegister(0x2000), GpuReadRegister(0x2001), GpuReadRegister(0x2002), mode, state.colorMask, state.depthControl,
            GpuReadRegister(REG_PA_CL_VTE_CNTL), state.positionScale[0], state.positionScale[1], state.positionOffset[0], state.positionOffset[1],
            primitive, indexList->size(), (unsigned long long)state.vertexShader, (unsigned long long)state.pixelShader,
            GpuReadRegister(0x2080), GpuReadRegister(0x2081), GpuReadRegister(0x2082), GpuReadRegister(REG_PA_SU_SC_MODE_CNTL), GpuReadRegister(REG_RB_COLORCONTROL));
    state.vertexDataKey = vertexData == cached.data.data() ? (layoutHash ^ (contentHash * 0x9E3779B97F4A7C15ull)) | 1 : 0;
    // Index lists not converted (lists, strips) can be uploaded once per frame too
    bool indicesConverted = primitive == 5 || primitive == 13 || primitive == 8;
    state.indexDataKey = cachedIndices && !indicesConverted ? (indexKey ^ (indexContent * 0xC2B2AE3D27D4EB4Full)) | 1 : 0;
    RendererDraw(state, vertexData, vertexCount, indexList->data(), uint32_t(indexList->size()));
    timer.Next(6);
    g_drawsSubmitted++;
}

void PrintDrawStats()
{
    fprintf(stderr, "[rendu] dessins transmis %llu, rejetes :", (unsigned long long)g_drawsSubmitted);
    for (uint32_t i = 0; i < RejectCount; i++)
        fprintf(stderr, " %s=%llu", g_rejectionNames[i], (unsigned long long)g_rejections[i]);
    fprintf(stderr, "\n");
}
