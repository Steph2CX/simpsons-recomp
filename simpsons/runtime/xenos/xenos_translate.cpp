// Construit un conteneur de shader "facon SDK" autour du microcode brut pour que ShaderRecompiler
// (XenosRecomp) puisse le traduire, puis compile le HLSL obtenu en SPIR-V avec DXC.
//
// Metadonnees deduites du microcode :
//  - chaque vfetch devient un element de sommet Position<N> (N = ordre d'apparition), lu en float4 ;
//  - l'interpolateur i correspond au registre i (VS : export i, PS : registre r<i>), comme le materiel ;
//  - les sorties du pixel shader viennent de ses instructions d'export.
#include "xenos_translate.h"

#include <pch.h>
#include <shader.h>
#include <shader_code.h>
#include <shader_recompiler.h>
#include <dxc_compiler.h>

#include <cstring>
#include <mutex>

static constexpr uint32_t INTERPOLATOR_COUNT = 16;
static constexpr uint32_t MAX_POSITION_ELEMENTS = 8; // Position0..7 have input locations (0, 15, 16-21)
static constexpr uint32_t POSITION_LOCATIONS[MAX_POSITION_ELEMENTS] = { 0, 15, 16, 17, 18, 19, 20, 21 };

extern const char g_shaderCommonHeader[];

namespace
{
    struct ParsedMicrocode
    {
        struct Fetch
        {
            uint32_t address; // instruction index
            VertexFetchInstruction instruction;
        };
        std::vector<Fetch> vertexFetches;
        uint32_t pixelOutputs = 0;
        uint32_t textureSlots = 0;
        uint8_t textureDimensions[32]{};
        uint8_t textureFilters[32];
        bool computedFetchIndex = false; // a vfetch reads a vertex chosen by the shader (not r0.x)
    };

    void PutBE32(std::vector<uint8_t>& out, size_t offset, uint32_t value)
    {
        if (out.size() < offset + 4)
            out.resize(offset + 4);
        out[offset + 0] = uint8_t(value >> 24);
        out[offset + 1] = uint8_t(value >> 16);
        out[offset + 2] = uint8_t(value >> 8);
        out[offset + 3] = uint8_t(value);
    }

    uint32_t GetBE32(const uint8_t* data, uint32_t index)
    {
        return (uint32_t(data[index * 4]) << 24) | (uint32_t(data[index * 4 + 1]) << 16) |
            (uint32_t(data[index * 4 + 2]) << 8) | uint32_t(data[index * 4 + 3]);
    }

    // Walks the control flow like ShaderRecompiler and collects vertex fetches and exports
    ParsedMicrocode Parse(const uint8_t* microcode, uint32_t sizeBytes)
    {
        ParsedMicrocode parsed;
        memset(parsed.textureFilters, 0x3F, sizeof(parsed.textureFilters));
        uint32_t sizeDwords = sizeBytes / 4;
        uint32_t controlFlowEnd = sizeDwords; // updated by the first exec address

        for (uint32_t cf = 0; cf * 3 + 2 < controlFlowEnd; cf++)
        {
            union
            {
                ControlFlowInstruction controlFlow[2];
                struct
                {
                    uint32_t code0, code1, code2, code3;
                };
            };
            uint32_t d0 = GetBE32(microcode, cf * 3), d1 = GetBE32(microcode, cf * 3 + 1), d2 = GetBE32(microcode, cf * 3 + 2);
            code0 = d0;
            code1 = d1 & 0xFFFF;
            code2 = (d1 >> 16) | (d2 << 16);
            code3 = d2 >> 16;

            for (auto& instr : controlFlow)
            {
                uint32_t address = 0, count = 0, sequence = 0;
                switch (instr.opcode)
                {
                case ControlFlowOpcode::Exec:
                case ControlFlowOpcode::ExecEnd:
                    address = instr.exec.address; count = instr.exec.count; sequence = instr.exec.sequence;
                    break;
                case ControlFlowOpcode::CondExec:
                case ControlFlowOpcode::CondExecEnd:
                case ControlFlowOpcode::CondExecPredClean:
                case ControlFlowOpcode::CondExecPredCleanEnd:
                    address = instr.condExec.address; count = instr.condExec.count; sequence = instr.condExec.sequence;
                    break;
                case ControlFlowOpcode::CondExecPred:
                case ControlFlowOpcode::CondExecPredEnd:
                    address = instr.condExecPred.address; count = instr.condExecPred.count; sequence = instr.condExecPred.sequence;
                    break;
                default:
                    continue;
                }

                // Instructions start after the control flow
                controlFlowEnd = std::min(controlFlowEnd, address * 3);

                for (uint32_t i = 0; i < count; i++, sequence >>= 2)
                {
                    uint32_t index = (address + i) * 3;
                    if (index + 2 >= sizeDwords)
                        break;
                    union
                    {
                        VertexFetchInstruction vertexFetch;
                        TextureFetchInstruction textureFetch;
                        AluInstruction alu;
                        struct
                        {
                            uint32_t c0, c1, c2;
                        };
                    };
                    c0 = GetBE32(microcode, index);
                    c1 = GetBE32(microcode, index + 1);
                    c2 = GetBE32(microcode, index + 2);

                    if (sequence & 0x1)
                    {
                        if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                        {
                            parsed.vertexFetches.push_back({ address + i, vertexFetch });
                            if (vertexFetch.srcRegister != 0 || vertexFetch.srcSwizzle != 0)
                                parsed.computedFetchIndex = true;
                        }
                        else if (textureFetch.opcode == FetchOpcode::TextureFetch)
                        {
                            parsed.textureSlots |= 1u << textureFetch.constIndex;
                            parsed.textureDimensions[textureFetch.constIndex] = uint8_t(textureFetch.dimension);
                            parsed.textureFilters[textureFetch.constIndex] = uint8_t(textureFetch.magFilter |
                                (textureFetch.minFilter << 2) | (textureFetch.mipFilter << 4));
                        }
                    }
                    else if (alu.exportData)
                    {
                        uint32_t dest = alu.vectorDest;
                        if (dest <= 3)
                            parsed.pixelOutputs |= 1u << dest;
                        else if (dest == uint32_t(ExportRegister::PSDepth))
                            parsed.pixelOutputs |= PIXEL_SHADER_OUTPUT_DEPTH;
                    }
                }
            }
        }
        return parsed;
    }

    uint32_t PackVertexElement(uint32_t address, DeclUsage usage, uint32_t usageIndex)
    {
        return (address & 0xFFF) | (uint32_t(usage) << 12) | ((usageIndex & 0xF) << 16);
    }

    uint32_t PackInterpolator(DeclUsage usage, uint32_t usageIndex, uint32_t reg)
    {
        return (usageIndex & 0xF) | (uint32_t(usage) << 4) | ((reg & 0xF) << 8);
    }

    std::vector<uint8_t> BuildContainer(const uint8_t* microcode, uint32_t sizeBytes, bool isPixelShader,
        const ParsedMicrocode& parsed)
    {
        // Layout: header | constant table (empty) | shader description | microcode (physical part)
        std::vector<uint8_t> out(0x24, 0);
        uint32_t constantTableOffset = 0x24;
        PutBE32(out, constantTableOffset, sizeof(ConstantTable)); // container size
        for (uint32_t i = 0; i < sizeof(ConstantTable) / 4; i++)
            PutBE32(out, constantTableOffset + 4 + i * 4, 0);

        uint32_t shaderOffset = uint32_t(out.size() + 15) & ~15u;
        out.resize(shaderOffset, 0);

        std::vector<uint32_t> words;
        words.push_back(0); // physicalOffset
        words.push_back(sizeBytes); // size
        words.push_back(0); // field8
        words.push_back(isPixelShader ? 0xFF00 : 0); // fieldC: no screen position register
        words.push_back(0); // field10
        words.push_back(INTERPOLATOR_COUNT << 5); // interpolatorInfo
        if (isPixelShader)
        {
            words.push_back(0); // field18
            words.push_back(parsed.pixelOutputs ? parsed.pixelOutputs : PIXEL_SHADER_OUTPUT_COLOR0);
            for (uint32_t i = 0; i < INTERPOLATOR_COUNT; i++)
                words.push_back(PackInterpolator(DeclUsage::TexCoord, i, i));
        }
        else
        {
            uint32_t elementCount = std::min<uint32_t>(uint32_t(parsed.vertexFetches.size()), MAX_POSITION_ELEMENTS);
            words.push_back(0); // field18: elements start at index 0
            words.push_back(elementCount);
            words.push_back(0); // field20
            for (uint32_t i = 0; i < elementCount; i++)
                words.push_back(PackVertexElement(parsed.vertexFetches[i].address, DeclUsage::Position, i));
            for (uint32_t i = 0; i < INTERPOLATOR_COUNT; i++)
                words.push_back(PackInterpolator(DeclUsage::TexCoord, i, i));
        }
        for (size_t i = 0; i < words.size(); i++)
            PutBE32(out, shaderOffset + i * 4, words[i]);

        uint32_t virtualSize = uint32_t(out.size() + 15) & ~15u;
        out.resize(virtualSize, 0);
        out.insert(out.end(), microcode, microcode + sizeBytes);

        PutBE32(out, 0x00, isPixelShader ? 0x102A1100 : 0x102A1101); // flags: bit 0 = vertex shader
        PutBE32(out, 0x04, virtualSize);
        PutBE32(out, 0x08, sizeBytes);
        PutBE32(out, 0x10, constantTableOffset);
        PutBE32(out, 0x14, 0); // no definition table: constants come from registers
        PutBE32(out, 0x18, shaderOffset);
        return out;
    }
}

XenosShader TranslateXenosShader(const uint8_t* microcode, uint32_t sizeBytes, bool isPixelShader)
{
    XenosShader result;
    ParsedMicrocode parsed = Parse(microcode, sizeBytes);
    result.pixelOutputs = parsed.pixelOutputs;
    result.textureSlots = parsed.textureSlots;
    memcpy(result.textureDimensions, parsed.textureDimensions, sizeof(result.textureDimensions));
    memcpy(result.textureFilters, parsed.textureFilters, sizeof(result.textureFilters));
    if (parsed.computedFetchIndex)
    {
        fprintf(stderr, "[shaders] vfetch a indice calcule (%u octets) :", sizeBytes);
        for (auto& fetch : parsed.vertexFetches)
            fprintf(stderr, " r%u.%c", fetch.instruction.srcRegister, "xyzw"[fetch.instruction.srcSwizzle]);
        fprintf(stderr, "\n");
    }

    if (!isPixelShader)
    {
        // Mini fetches reuse the stride and fetch constant of the previous full fetch
        uint32_t stride = 0, constant = 0;
        for (size_t i = 0; i < parsed.vertexFetches.size() && i < MAX_POSITION_ELEMENTS; i++)
        {
            auto& fetch = parsed.vertexFetches[i].instruction;
            if (!fetch.isMiniFetch)
            {
                stride = fetch.stride;
                constant = fetch.constIndex * 3 + fetch.constIndexSelect;
            }
            result.attributes.push_back({ POSITION_LOCATIONS[i], constant, uint32_t(fetch.format),
                uint32_t(fetch.offset), stride, fetch.formatCompAll != 0, fetch.numFormatAll == 0,
                fetch.srcRegister != 0 || fetch.srcSwizzle != 0 });
        }
        if (parsed.vertexFetches.size() > MAX_POSITION_ELEMENTS)
            result.error = "plus de 8 lectures de sommets";
    }

    std::vector<uint8_t> container = BuildContainer(microcode, sizeBytes, isPixelShader, parsed);

    ShaderRecompiler recompiler;
    recompiler.recompile(container.data(), g_shaderCommonHeader);
    result.hlsl = recompiler.out;

    if (!isPixelShader)
    {
        // Xenos viewport transform, applied on the host: clip.xyz = clip.xyz * scale + offset * clip.w
        // (scale/offset computed from the PA_CL_VPORT/VTE registers, stored after the shared constants)
        const std::string halfPixel = "oPos.xy += g_HalfPixelOffset * oPos.w;";
        const std::string hostTransform =
            "oPos.xyz = oPos.xyz * vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 288).xyz"
            " + vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 304).xyz * oPos.w;";
        for (size_t at = result.hlsl.find(halfPixel); at != std::string::npos; at = result.hlsl.find(halfPixel, at))
        {
            result.hlsl.replace(at, halfPixel.size(), hostTransform);
            at += hostTransform.size();
        }
    }

    // SIMPSONS_DEBUG_PS=1: each pixel shader outputs a flat color derived from its microcode,
    // =2: outputs interpolator 0, =3: interpolator 1
    static int debugPixelShaders = getenv("SIMPSONS_DEBUG_PS") ? atoi(getenv("SIMPSONS_DEBUG_PS")) : 0;
    if (isPixelShader && debugPixelShaders && result.hlsl.find("out float4 oC0") != std::string::npos)
    {
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < sizeBytes; i++)
            hash = (hash ^ microcode[i]) * 16777619u;
        char color[192];
        if (debugPixelShaders == 30) // placeholder texture sampled directly
            snprintf(color, sizeof(color), "{ oC0 = float4(g_Texture2DDescriptorHeap[0].Sample(g_SamplerDescriptorHeap[0], iTexCoord0.xy).xyz, 1.0); return; }");
        else if (debugPixelShaders == 31) // real output, forced opaque
            snprintf(color, sizeof(color), "{ oC0 = float4(oC0.xyz, 1.0); return; }");
        else if (debugPixelShaders == 32) // real output alpha
            snprintf(color, sizeof(color), "{ oC0 = float4(oC0.www * 0.5 + 0.25, 1.0); return; }");
        else if (debugPixelShaders >= 10) // =1N: alpha of interpolator N
            snprintf(color, sizeof(color), "{ oC0 = float4(iTexCoord%d.www, 1.0); return; }", debugPixelShaders - 10);
        else if (debugPixelShaders >= 2)
            snprintf(color, sizeof(color), "{ oC0 = float4(frac(iTexCoord%d.xyz), 1.0); return; }", debugPixelShaders - 2);
        else
            snprintf(color, sizeof(color), "{ oC0 = float4(%.2f, %.2f, %.2f, 1.0); return; }",
                0.3 + 0.7 * ((hash >> 0) & 0xFF) / 255.0, 0.3 + 0.7 * ((hash >> 8) & 0xFF) / 255.0, 0.3 + 0.7 * ((hash >> 16) & 0xFF) / 255.0);
        const std::string ret = "return;";
        for (size_t at = result.hlsl.find(ret, result.hlsl.find("void main(")); at != std::string::npos; at = result.hlsl.find(ret, at))
        {
            result.hlsl.replace(at, ret.size(), color);
            at += strlen(color);
        }
    }

    // DXC instances are not thread safe
    static std::mutex dxcMutex;
    std::lock_guard lock(dxcMutex);
    static DxcCompiler dxc;
    IDxcBlob* blob = dxc.compile(result.hlsl, isPixelShader, false, true);
    if (blob == nullptr)
    {
        result.error = "echec de compilation DXC";
        return result;
    }
    result.spirv.resize(blob->GetBufferSize() / 4);
    memcpy(result.spirv.data(), blob->GetBufferPointer(), result.spirv.size() * 4);
    blob->Release();
    result.valid = true;
    return result;
}
