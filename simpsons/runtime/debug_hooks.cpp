// Debug hooks on guest functions (enabled with environment variables).
#include "runtime.h"

#include <cstdio>
#include <cstdlib>

bool NativeFunctionsEnabled();
void GuestMemcpy(uint32_t destination, uint32_t source, uint32_t size);

PPC_FUNC_IMPL(__imp__sub_82A3CD80); // memcpy(dest, source, size)

// SIMPSONS_TRACE_IMLOAD: shader microcode copied into command buffers by D3D (SetVertexShader and
// the precompiled command buffer builders); showed the 0xE0000000 view offset problem
PPC_FUNC(sub_82A3CD80)
{
    static bool trace = getenv("SIMPSONS_TRACE_IMLOAD") != nullptr;
    if (trace)
    {
        uint32_t lr = uint32_t(GuestLR(ctx));
        if (lr == 0x8245F784 || lr == 0x825F9038 || lr == 0x825F9E70 || lr == 0x825F9F0C)
        {
            static int traced = 0;
            uint32_t source = ctx.r4.u32, size = ctx.r5.u32;
            bool empty = true;
            for (uint32_t i = 0; i < size; i += 4)
                empty &= LoadBE32(source + i) == 0;
            if (traced < 60 && (empty || traced < 10))
            {
                traced++;
                fprintf(stderr, "[imload] copie lr=0x%08X source=0x%08X taille=%u %s : en-tete %08X %08X, mots %08X %08X %08X\n",
                    lr, source, size, empty ? "VIDE" : "ok", LoadBE32(source - 8), LoadBE32(source - 4),
                    LoadBE32(source), LoadBE32(source + 4), LoadBE32(source + 8));
            }
        }
    }
    // Replaced by the host copy (native_functions.cpp); returns dest, left in r3
    if (NativeFunctionsEnabled())
        GuestMemcpy(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    else
        __imp__sub_82A3CD80(ctx, base);
}
