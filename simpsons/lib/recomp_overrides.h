// Included before the recompiled code (CMake -include): overrides of ppc_context.h macros.
//
// Indirect calls (bctrl, bctr to a function) check
// their target. A rare race in the game (seen in sub_8269E980, a table of phase callbacks) leaves a data
// value where a function address is expected; calling it would crash the whole game. The call is skipped
// and reported instead (GuestInvalidIndirectCall, runtime/main.cpp).
#pragma once
#include <cstdint>

struct PPCContext;
void GuestInvalidIndirectCall(PPCContext& ctx, uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x)                                                                       \
    do                                                                                                  \
    {                                                                                                   \
        uint32_t indirectTarget_ = uint32_t(x);                                                         \
        PPCFunc* indirectFunc_ = uint32_t(indirectTarget_ - uint32_t(PPC_CODE_BASE)) < uint32_t(PPC_CODE_SIZE) \
            ? PPC_LOOKUP_FUNC(base, indirectTarget_) : nullptr;                                         \
        if (indirectFunc_ != nullptr) [[likely]]                                                        \
            indirectFunc_(ctx, base);                                                                   \
        else                                                                                            \
            GuestInvalidIndirectCall(ctx, indirectTarget_);                                             \
    } while (0)

// Accesses to GPU registers (MMIO, uncached and ordered on the console): the store is a release so that the
// command packets written before it (e.g. before the ring write pointer) are visible to the GPU thread first
#define PPC_MM_STORE_U32(x, y)                              \
    do                                                      \
    {                                                       \
        __atomic_thread_fence(__ATOMIC_RELEASE);            \
        PPC_STORE_U32(x, y);                                \
    } while (0)
#define PPC_MM_LOAD_U32(x) ([&] { auto mmValue_ = PPC_LOAD_U32(x); __atomic_thread_fence(__ATOMIC_ACQUIRE); return mmValue_; }())
