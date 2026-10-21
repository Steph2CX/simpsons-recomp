// Guest library routines replaced by host code (same result, much faster than the recompiled PowerPC loops).
// SIMPSONS_SANS_NATIF=1 (tests): the recompiled versions run instead.
#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

PPC_FUNC_IMPL(__imp__sub_82B74110);

bool NativeFunctionsEnabled()
{
    static const bool enabled = getenv("SIMPSONS_SANS_NATIF") == nullptr;
    return enabled;
}

// Copies bytes between guest addresses: a byte copy does not depend on endianness. memmove also covers overlapping
// ranges, which the guest copies do not handle (they zero destination cache lines ahead with dcbzl).
void GuestMemcpy(uint32_t destination, uint32_t source, uint32_t size)
{
    if (size != 0)
        memmove(GuestToHost(destination), GuestToHost(source), size);
}

// memcpy(dest, source, size) with VMX and dcbzl (XMemCpy-like), returns dest. Its byte tail loop steps by a global
// (0x82CFE4C0) that should hold 1: anything else keeps the original code.
PPC_FUNC(sub_82B74110)
{
    static const bool native = [] {
        uint32_t step = LoadBE32(0x82CFE4C0);
        if (NativeFunctionsEnabled() && step != 1)
            fprintf(stderr, "[natif] copie 82B74110 gardee recompilee (pas %u)\n", step);
        return NativeFunctionsEnabled() && step == 1;
    }();
    if (!native)
    {
        __imp__sub_82B74110(ctx, base);
        return;
    }
    GuestMemcpy(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
