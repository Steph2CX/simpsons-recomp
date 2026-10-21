#pragma once

// Textures Xenos : lecture des constantes de fetch, detilage et conversion vers un format hote.

#include <cstdint>

struct RendererDrawState;

// Fills the texture and sampler descriptor indices of the slots in slotMask (bit N = fetch constant N)
// filters: per slot, filters set by the tfetch instruction (mag bits 0-1, min 2-3, mip 4-5; 3 = fetch constant)
void TexturesBind(uint32_t slotMask, const uint8_t* dimensions, const uint8_t* filters, RendererDrawState& state);

// Called at each frame swap: guest memory of textures is hashed again on next use
void TexturesNewFrame();

// Byte offset of block (x, y) in a tiled 2D surface (pitch in blocks, bytesPerBlock = 1, 2, 4, 8 or 16)
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytesPerBlock);
