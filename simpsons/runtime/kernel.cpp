// Implementations des fonctions systeme Xbox 360 (xboxkrnl / xam).
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;

// ---------------------------------------------------------------------------
// Memory

KERNEL_FUNC(NtAllocateVirtualMemory)
{
    // (PVOID* BaseAddress, SIZE_T* RegionSize, ULONG AllocationType, ULONG Protect)
    LOG_CALL(NtAllocateVirtualMemory);
    uint32_t requested = LoadBE32(ctx.r3.u32);
    uint32_t size = (LoadBE32(ctx.r4.u32) + 0xFFF) & ~0xFFF;
    uint32_t address = requested != 0 ? requested : GuestAllocHeap(size, 0x10000);
    fprintf(stderr, "[memoire] NtAllocateVirtualMemory demande=0x%X taille=0x%X type=0x%X -> 0x%X\n", requested, size, ctx.r5.u32, address);
    StoreBE32(ctx.r3.u32, address);
    StoreBE32(ctx.r4.u32, size);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtFreeVirtualMemory)
{
    LOG_CALL(NtFreeVirtualMemory);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(MmAllocatePhysicalMemoryEx)
{
    // (ULONG Flags, SIZE_T Size, ULONG Protect, ULONG MinAddress, ULONG MaxAddress, ULONG Alignment)
    LOG_CALL(MmAllocatePhysicalMemoryEx);
    uint32_t alignment = std::max<uint32_t>(ctx.r8.u32, 0x1000);
    ctx.r3.u64 = GuestAllocPhysical((ctx.r4.u32 + 0xFFF) & ~0xFFF, alignment, ctx.r5.u32);
}

KERNEL_FUNC(MmAllocatePhysicalMemory)
{
    // (ULONG Flags, SIZE_T Size, ULONG Protect)
    LOG_CALL(MmAllocatePhysicalMemory);
    ctx.r3.u64 = GuestAllocPhysical((ctx.r4.u32 + 0xFFF) & ~0xFFF, 0x1000, ctx.r5.u32);
}

KERNEL_FUNC(MmFreePhysicalMemory)
{
    LOG_CALL(MmFreePhysicalMemory);
}

KERNEL_FUNC(MmGetPhysicalAddress)
{
    LOG_CALL(MmGetPhysicalAddress);
    ctx.r3.u64 = VirtualToPhysical(ctx.r3.u32);
}

KERNEL_FUNC(MmQueryStatistics)
{
    // (MM_STATISTICS* Stats) - first field is the structure size, set by the caller
    LOG_CALL(MmQueryStatistics);
    uint32_t stats = ctx.r3.u32;
    uint32_t length = LoadBE32(stats);
    memset(GuestToHost(stats + 4), 0, length > 4 ? length - 4 : 0);

    // 512 MB console, 4 KB pages; most of the memory is free for the title
    const uint32_t values[] =
    {
        0x00020000, // TotalPhysicalPages
        0x00000300, // KernelPages
        0x00018000, // TitleAvailablePages (384 MB)
        0x2FFF0000, // TitleTotalVirtualMemoryBytes
        0x00160000, // TitleReservedVirtualMemoryBytes
        0x00001000, // TitlePhysicalPages
        0x00000800, // TitlePoolPages
        0x00000400, // TitleStackPages
        0x00000800, // TitleImagePages
        0x00000800, // TitleHeapPages
        0x00000800, // TitleVirtualPages
        0x00000800, // TitlePageTablePages
        0x00000800, // TitleCachePages
        0x00000000, // SystemAvailablePages
        0x2FFF0000, // SystemTotalVirtualMemoryBytes
    };
    for (uint32_t i = 0; i < std::size(values) && 4 + i * 4 + 4 <= length; i++)
        StoreBE32(stats + 4 + i * 4, values[i]);

    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(MmQueryAddressProtect)
{
    LOG_CALL(MmQueryAddressProtect);
    ctx.r3.u64 = 0x04; // PAGE_READWRITE
}

// ---------------------------------------------------------------------------
// Thread local storage (dynamic slots)

static constexpr uint32_t TLS_SLOT_COUNT = 64;
static std::atomic<uint64_t> g_tlsUsedSlots{ 0 };
static thread_local uint32_t t_tlsValues[TLS_SLOT_COUNT];

KERNEL_FUNC(KeTlsAlloc)
{
    LOG_CALL(KeTlsAlloc);
    uint64_t used = g_tlsUsedSlots.load();
    for (uint32_t i = 0; i < TLS_SLOT_COUNT; i++)
    {
        if (!(used & (1ull << i)) && g_tlsUsedSlots.compare_exchange_strong(used, used | (1ull << i)))
        {
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

KERNEL_FUNC(KeTlsFree)
{
    LOG_CALL(KeTlsFree);
    if (ctx.r3.u32 < TLS_SLOT_COUNT)
        g_tlsUsedSlots &= ~(1ull << ctx.r3.u32);
    ctx.r3.u64 = 1;
}

KERNEL_FUNC(KeTlsGetValue)
{
    LOG_CALL(KeTlsGetValue);
    ctx.r3.u64 = ctx.r3.u32 < TLS_SLOT_COUNT ? t_tlsValues[ctx.r3.u32] : 0;
}

KERNEL_FUNC(KeTlsSetValue)
{
    LOG_CALL(KeTlsSetValue);
    bool valid = ctx.r3.u32 < TLS_SLOT_COUNT;
    if (valid)
        t_tlsValues[ctx.r3.u32] = ctx.r4.u32;
    ctx.r3.u64 = valid;
}

// ---------------------------------------------------------------------------
// Strings and debug output

KERNEL_FUNC(RtlInitAnsiString)
{
    // (ANSI_STRING* Destination, const char* Source)
    LOG_CALL(RtlInitAnsiString);
    uint32_t length = ctx.r4.u32 ? uint32_t(strlen(static_cast<char*>(GuestToHost(ctx.r4.u32)))) : 0;
    StoreBE16(ctx.r3.u32 + 0, uint16_t(length));
    StoreBE16(ctx.r3.u32 + 2, uint16_t(ctx.r4.u32 ? length + 1 : 0));
    StoreBE32(ctx.r3.u32 + 4, ctx.r4.u32);
}

KERNEL_FUNC(DbgPrint)
{
    LOG_CALL(DbgPrint);
    fprintf(stderr, "[jeu] %s", static_cast<char*>(GuestToHost(ctx.r3.u32)));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(KeBugCheck)
{
    LOG_CALL(KeBugCheck);
    fprintf(stderr, "[runtime] KeBugCheck(0x%X) : le jeu s'est arrete volontairement\n", ctx.r3.u32);
    abort();
}

KERNEL_FUNC(KeBugCheckEx)
{
    LOG_CALL(KeBugCheckEx);
    fprintf(stderr, "[runtime] KeBugCheckEx(0x%X, 0x%X, 0x%X, 0x%X)\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    abort();
}

// ---------------------------------------------------------------------------
// Time

KERNEL_FUNC(KeQueryPerformanceFrequency)
{
    LOG_CALL(KeQueryPerformanceFrequency);
    ctx.r3.u64 = 49875000; // Xbox 360 timebase
}

KERNEL_FUNC(KeQuerySystemTime)
{
    // FILETIME: 100 ns intervals since 1601-01-01
    LOG_CALL(KeQuerySystemTime);
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t filetime = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100) + 116444736000000000ull;
    *static_cast<uint64_t*>(GuestToHost(ctx.r3.u32)) = __builtin_bswap64(filetime);
}

KERNEL_FUNC(RtlTimeToTimeFields)
{
    // (LARGE_INTEGER* Time, TIME_FIELDS* Fields): FILETIME -> year, month, day, hour, minute, second,
    // milliseconds, weekday (16-bit each)
    LOG_CALL(RtlTimeToTimeFields);
    uint64_t filetime = __builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(ctx.r3.u32)));
    int64_t unixMs = int64_t(filetime / 10000) - 11644473600000ll;
    time_t seconds = time_t(unixMs >= 0 ? unixMs / 1000 : (unixMs - 999) / 1000);
    struct tm fields;
#ifdef _WIN32
    gmtime_s(&fields, &seconds);
#else
    gmtime_r(&seconds, &fields);
#endif
    uint16_t values[8] = { uint16_t(fields.tm_year + 1900), uint16_t(fields.tm_mon + 1), uint16_t(fields.tm_mday),
        uint16_t(fields.tm_hour), uint16_t(fields.tm_min), uint16_t(fields.tm_sec), uint16_t(unixMs - int64_t(seconds) * 1000),
        uint16_t(fields.tm_wday) };
    for (uint32_t i = 0; i < 8; i++)
        StoreBE16(ctx.r4.u32 + i * 2, values[i]);
}

// ---------------------------------------------------------------------------
// Process / title

KERNEL_FUNC(KeGetCurrentProcessType)
{
    LOG_CALL(KeGetCurrentProcessType);
    ctx.r3.u64 = 1; // title process
}

KERNEL_FUNC(XexCheckExecutablePrivilege)
{
    LOG_CALL(XexCheckExecutablePrivilege);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(XamLoaderTerminateTitle)
{
    LOG_CALL(XamLoaderTerminateTitle);
    fprintf(stderr, "[runtime] le jeu a demande a quitter\n");
    exit(0);
}


// ---------------------------------------------------------------------------
// Console configuration

// Xbox language ids: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian
static uint32_t ConsoleLanguage()
{
    const char* language = getenv("SIMPSONS_LANGUE");
    return language != nullptr ? uint32_t(atoi(language)) : 1;
}

KERNEL_FUNC(ExGetXConfigSetting)
{
    // (WORD Category, WORD Setting, VOID* Buffer, WORD BufferSize, WORD* RequiredSize)
    LOG_CALL(ExGetXConfigSetting);
    uint32_t category = ctx.r3.u32, setting = ctx.r4.u32, buffer = ctx.r5.u32, bufferSize = ctx.r6.u32;
    uint32_t value = 0;
    bool known = true;

    if (category == 3) // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x01: value = 0; break; // time zone bias
        case 0x09: value = ConsoleLanguage(); break;
        case 0x0A: value = 0x00040000; break; // video flags: widescreen
        case 0x0B: value = 0; break; // audio flags
        case 0x0C: value = 0x40; break; // retail flags
        case 0x0E: value = 103; break; // country: United States
        default: known = false; break;
        }
    }
    else if (category == 2 && setting == 0x02) // XCONFIG_SECURED_AV_REGION
    {
        value = 0x00001000; // NTSC-M
    }
    else
    {
        known = false;
    }

    if (!known)
    {
        fprintf(stderr, "[kernel] ExGetXConfigSetting(%u, 0x%X) inconnu\n", category, setting);
        ctx.r3.u64 = 0xC0000034;
        return;
    }

    if (buffer != 0 && bufferSize >= 4)
        StoreBE32(buffer, value);
    if (ctx.r7.u32 != 0)
        StoreBE16(ctx.r7.u32, 4);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(XGetGameRegion)
{
    LOG_CALL(XGetGameRegion);
    ctx.r3.u64 = 0xFFFF; // all regions
}

KERNEL_FUNC(XGetLanguage)
{
    LOG_CALL(XGetLanguage);
    ctx.r3.u64 = ConsoleLanguage();
}

KERNEL_FUNC(XGetAVPack)
{
    LOG_CALL(XGetAVPack);
    ctx.r3.u64 = 6; // HDMI
}

// ---------------------------------------------------------------------------
// Runtime library helpers

KERNEL_FUNC(RtlFillMemoryUlong)
{
    // (VOID* Destination, SIZE_T Length, ULONG Pattern)
    LOG_CALL(RtlFillMemoryUlong);
    uint32_t pattern = __builtin_bswap32(ctx.r5.u32);
    auto* destination = static_cast<uint32_t*>(GuestToHost(ctx.r3.u32));
    for (uint32_t i = 0; i < ctx.r4.u32 / 4; i++)
        destination[i] = pattern;
}

KERNEL_FUNC(RtlNtStatusToDosError)
{
    LOG_CALL(RtlNtStatusToDosError);
    switch (ctx.r3.u32)
    {
    case 0x00000000: ctx.r3.u64 = 0; break;
    case 0x00000102: ctx.r3.u64 = 0x5B4; break; // ERROR_TIMEOUT
    case 0x80000006: ctx.r3.u64 = 18; break; // ERROR_NO_MORE_FILES
    case 0xC0000008: ctx.r3.u64 = 6; break; // ERROR_INVALID_HANDLE
    case 0xC000000D: ctx.r3.u64 = 87; break; // ERROR_INVALID_PARAMETER
    case 0xC0000011: ctx.r3.u64 = 38; break; // ERROR_HANDLE_EOF
    case 0xC0000022: ctx.r3.u64 = 5; break; // ERROR_ACCESS_DENIED
    case 0xC0000034: ctx.r3.u64 = 2; break; // ERROR_FILE_NOT_FOUND
    case 0xC000003A: ctx.r3.u64 = 3; break; // ERROR_PATH_NOT_FOUND
    default: ctx.r3.u64 = (ctx.r3.u32 & 0xC0000000) ? 0x13D : 0; break;
    }
}

KERNEL_FUNC(RtlRaiseException)
{
    // (EXCEPTION_RECORD* Record). 0x406D1388 is the Visual C++ "set thread name" convention.
    LOG_CALL(RtlRaiseException);
    uint32_t code = LoadBE32(ctx.r3.u32);
    if (code == 0x406D1388)
    {
        uint32_t info = ctx.r3.u32 + 0x14; // ExceptionInformation: type, name, thread id, flags
        uint32_t name = LoadBE32(info + 4);
        if (name != 0)
            fprintf(stderr, "[runtime] thread 0x%X nomme \"%s\"\n", LoadBE32(info + 8), static_cast<char*>(GuestToHost(name)));
        return;
    }
    fprintf(stderr, "[runtime] RtlRaiseException code 0x%08X ignoree\n", code);
}

KERNEL_FUNC(MmSetAddressProtect)
{
    LOG_CALL(MmSetAddressProtect);
}

KERNEL_FUNC(MmQueryAllocationSize)
{
    LOG_CALL(MmQueryAllocationSize);
    ctx.r3.u64 = 0;
}
