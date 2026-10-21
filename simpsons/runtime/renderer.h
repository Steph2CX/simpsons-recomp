#pragma once

#include <cstdint>
#include <vector>

#include "xenos/xenos_translate.h"

// Fenetre et Vulkan (via MoltenVK). L'initialisation et la boucle d'evenements tournent sur le
// thread principal (exigence de macOS) ; le faux GPU appelle les autres fonctions depuis son thread.

bool RendererInit();
void RendererShowStartupError(); // RendererInit failed: tells the player (no window otherwise) before quitting
void RendererRunEventLoop(); // returns when the window is closed
bool RendererIsRunning();

// ---------------------------------------------------------------------------
// Called from the command processor thread

enum class RendererTopology : uint8_t
{
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip,
};

struct RendererDrawState
{
    uint64_t vertexShader = 0; // microcode hashes, see RendererSetShader
    uint64_t pixelShader = 0;
    RendererTopology topology = RendererTopology::TriangleList;
    bool primitiveRestart = false;

    // Raw Xenos registers, decoded by the renderer
    uint32_t blendControl = 0; // RB_BLENDCONTROL0
    uint32_t depthControl = 0; // RB_DEPTHCONTROL
    uint32_t modeControl = 0; // PA_SU_SC_MODE_CNTL
    uint32_t colorMask = 0xF; // RB_COLOR_MASK (render target 0)
    bool alphaTest = false;
    float alphaReference = 0.0f;

    float positionScale[4]{ 1, 1, 1, 0 }; // Xenos viewport transform (window coordinates, pixels)
    float positionOffset[4]{ 0, 0, 0, 0 };

    const float* vertexConstants = nullptr; // 256 float4, host byte order
    const float* pixelConstants = nullptr; // 256 float4
    uint32_t booleans = 0;
    uint32_t loopConstants[32]{}; // count | start << 8 | step << 16
    uint32_t vertexIndexBase = 0; // added to the host vertex index to get the guest one (r0.x)
    uint64_t fetchBuffers[8]{}; // device addresses of streams decoded for fetches at computed indices
    uint64_t vertexDataKey = 0; // identifies the vertex data (0: unique): uploaded once per frame
    uint64_t indexDataKey = 0; // same for the indices

    // Descriptor heap indices per texture fetch slot (0 = placeholder), see RendererUploadTexture
    uint32_t textureIndices[16]{};
    uint32_t samplerIndices[16]{};
};

enum class RendererTextureFormat : uint8_t
{
    RGBA8, // component X in the lowest byte
    R8,
    RG8,
    RGB10A2,
    R16,
    RG16,
    RGBA16,
    R16F,
    RG16F,
    RGBA16F,
    R32F,
    RG32F,
    RGBA32F,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
};

struct RendererTextureDesc
{
    uint32_t width = 0, height = 0;
    RendererTextureFormat format = RendererTextureFormat::RGBA8;
    uint8_t swizzle[4]{ 0, 1, 2, 3 }; // per output component: 0-3 source component, 4 = 0, 5 = 1
    const void* data = nullptr; // rows of blocks, tightly packed
    size_t size = 0;
    // Mip levels: level N (max(1, width >> N) x max(1, height >> N)) starts at levelOffsets[N] in data,
    // its layers (6 faces of a cube) follow each other
    uint32_t levels = 1;
    size_t levelOffsets[16]{};
    bool cube = false;
};

// Creates or replaces the texture identified by key; returns its descriptor index (0 on failure), in the
// 2D heap or the cube heap
uint32_t RendererUploadTexture(uint64_t key, const RendererTextureDesc& desc);
// Copies data to the frame's upload ring; returns its device address (0 if full)
uint64_t RendererUploadBuffer(const void* data, size_t size);
// Changes each time the upload ring restarts: buffers uploaded with another value are no longer valid
uint64_t RendererUploadEpoch();
// Descriptor index of an already uploaded texture, 0 if unknown
uint32_t RendererFindTexture(uint64_t key);
bool RendererSupportsFormat(RendererTextureFormat format);

// Selects the EDRAM surfaces receiving the next draws, clears and copies (RB_SURFACE_INFO
// multisampling: 0 = 1x, 1 = 2x, 2 = 4x; RB_COLOR_INFO and RB_DEPTH_INFO bases in EDRAM tiles)
// scissorWidth/Height: bottom-right corner of the game's window scissor (console pixels): limits the rendering area
void RendererSetTargets(uint32_t msaa, uint32_t colorBase, uint32_t colorFormat, uint32_t depthBase, uint32_t scissorWidth,
    uint32_t scissorHeight);

// Resolve ("copy") of the top-left width x height pixels of the color target into a texture that
// replaces the guest memory at address (swapRedBlue: the copy exchanged red and blue)
// Copies the rectangle (x, y, copyWidth x copyHeight) of the colour surface to the same place in the texture
// (width x height) at a physical address
void RendererResolve(uint32_t address, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t copyWidth,
    uint32_t copyHeight, bool swapRedBlue);
// Depth copy (copy source 4): the depth of the top-left width x height samples becomes a float texture
void RendererResolveDepth(uint32_t address, uint32_t width, uint32_t height);
// Descriptor index of a resolved texture at that address with these dimensions, 0 if none.
// swizzle: fetch constant swizzle over the components as stored in memory
uint32_t RendererFindResolved(uint32_t address, uint32_t width, uint32_t height, const uint8_t swizzle[4]);

// Address modes: 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 mirror clamp to edge, 4 clamp to border
// Filters: 0 point, 1 linear
uint32_t RendererGetSampler(uint32_t addressU, uint32_t addressV, uint32_t addressW, uint32_t magFilter,
    uint32_t minFilter, uint32_t mipFilter);

// Translates (once) and selects a shader; returns its hash, 0 if it cannot be used
uint64_t RendererSetShader(bool pixelShader, const uint8_t* microcode, uint32_t sizeBytes);
const XenosShader* RendererGetShader(uint64_t hash);

// vertices: attributeCount float4 per vertex, in the order of the vertex shader attributes
void RendererDraw(const RendererDrawState& state, const float* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount);
// Clears the top-left width x height pixels of the bound surfaces
void RendererClear(bool clearColor, const float color[4], bool clearDepth, float depth, uint8_t stencil,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// Called by the command processor at each frame swap
void RendererPresent(uint64_t frame);
