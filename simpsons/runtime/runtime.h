#pragma once

#include <atomic>

#include <ppc_config.h>
#include <ppc_context.h>

// Registers for traces and crash reports. With the fast recompilation settings (simpsons_opt.toml), most of them
// live in local variables of the recompiled functions and are not visible here: 0 is returned.
inline uint64_t GuestLR(const PPCContext& c)
{
#ifdef PPC_CONFIG_SKIP_LR
    (void)c;
    return 0;
#else
    return c.lr;
#endif
}

inline uint32_t GuestCTR(const PPCContext& c)
{
#ifdef PPC_CONFIG_CTR_AS_LOCAL
    (void)c;
    return 0;
#else
    return c.ctr.u32;
#endif
}

inline uint32_t GuestGPR(const PPCContext& c, int i)
{
    switch (i)
    {
    case 1: return c.r1.u32;
    case 3: return c.r3.u32;
    case 4: return c.r4.u32;
    case 5: return c.r5.u32;
    case 6: return c.r6.u32;
    case 7: return c.r7.u32;
    case 8: return c.r8.u32;
    case 9: return c.r9.u32;
    case 10: return c.r10.u32;
    case 13: return c.r13.u32;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    case 0: return c.r0.u32;
    case 2: return c.r2.u32;
    case 11: return c.r11.u32;
    case 12: return c.r12.u32;
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    case 14: return c.r14.u32; case 15: return c.r15.u32; case 16: return c.r16.u32; case 17: return c.r17.u32;
    case 18: return c.r18.u32; case 19: return c.r19.u32; case 20: return c.r20.u32; case 21: return c.r21.u32;
    case 22: return c.r22.u32; case 23: return c.r23.u32; case 24: return c.r24.u32; case 25: return c.r25.u32;
    case 26: return c.r26.u32; case 27: return c.r27.u32; case 28: return c.r28.u32; case 29: return c.r29.u32;
    case 30: return c.r30.u32; case 31: return c.r31.u32;
#endif
    default: return 0;
    }
}

#include <cstdint>
#include <cstdio>

// Guest address space layout (all inside the single 4 GB host reservation)
constexpr uint32_t GUEST_NULL_GUARD_SIZE = 0x10000;
constexpr uint32_t GUEST_HEAP_BEGIN = 0x40000000; // NtAllocateVirtualMemory
constexpr uint32_t GUEST_HEAP_END = 0x70000000;
constexpr uint32_t GUEST_SYSTEM_BEGIN = 0x70000000; // stacks, PCR, TLS, kernel objects
constexpr uint32_t GUEST_SYSTEM_END = 0x7F000000;
// 512 MB of physical memory, visible through three aliased views like on the console:
// 0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages), 0xE0000000 (4 KB pages).
// On the console the 0xE0000000 view is offset by 0x1000 physical bytes. Apple Silicon has 16 KB
// host pages, so the views are mapped identically and D3D's inline offset is removed by hooks
// (see E0NoPhysicalOffset in main.cpp).
constexpr uint32_t GUEST_PHYSICAL_BEGIN = 0xA0000000;
constexpr uint32_t PHYSICAL_MEMORY_SIZE = 0x20000000;

// Physical address as seen by the game and the GPU <-> guest virtual address
// Apple TV (about 2 GB for the application): smaller host caches and GPU buffers. SIMPSONS_MEMOIRE_REDUITE=1 does the
// same elsewhere (to measure)
bool RuntimeLowMemory();

uint32_t PhysicalToVirtual(uint32_t physical);
uint32_t VirtualToPhysical(uint32_t address);

extern uint8_t* g_base;

// Bump allocators over the regions above; return a zeroed guest address
uint32_t GuestAllocHeap(uint32_t size, uint32_t alignment = 0x1000);
uint32_t GuestAllocSystem(uint32_t size, uint32_t alignment = 0x10);
// Protect flags select the view: 0x20000000 = 64 KB pages, 0x40000000 = 16 MB pages, otherwise 4 KB pages
uint32_t GuestAllocPhysical(uint32_t size, uint32_t alignment, uint32_t protect);

inline void* GuestToHost(uint32_t address)
{
    return g_base + address;
}

inline uint32_t HostToGuest(const void* ptr)
{
    return uint32_t(static_cast<const uint8_t*>(ptr) - g_base);
}

inline uint32_t LoadBE32(uint32_t address)
{
    return __builtin_bswap32(*reinterpret_cast<uint32_t*>(g_base + address));
}

inline void StoreBE32(uint32_t address, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(g_base + address) = __builtin_bswap32(value);
}

inline uint16_t LoadBE16(uint32_t address)
{
    return __builtin_bswap16(*reinterpret_cast<uint16_t*>(g_base + address));
}

inline void StoreBE16(uint32_t address, uint16_t value)
{
    *reinterpret_cast<uint16_t*>(g_base + address) = __builtin_bswap16(value);
}

// Logging of kernel calls: "[kernel] Name(r3, r4, r5, r6)" for the first calls of each function, and
// call counts for SIMPSONS_STATS. LOG_CALL uses a counter per call site (no lock on the hot path).
struct ImportCounter
{
    const char* name;
    std::atomic<uint32_t> count{ 0 };
    ImportCounter* next = nullptr;
};
void LogImportCallCounted(ImportCounter& counter, const PPCContext& ctx, bool implemented);
void LogImportCall(const char* name, const PPCContext& ctx, bool implemented); // slower: looks the name up
#define LOG_CALL(name)                                           \
    do                                                           \
    {                                                            \
        static ImportCounter importCounter_{ #name };            \
        LogImportCallCounted(importCounter_, ctx, true);         \
    } while (0)

// Set up r1/r13 and per-thread structures for a new guest thread; returns its KTHREAD
uint32_t InitGuestThreadContext(PPCContext& ctx, uint32_t stackSize);

// Register the calling host thread as the guest main thread
PPCContext& InitMainGuestThread(uint32_t stackSize);
