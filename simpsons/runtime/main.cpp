// Runtime minimal : charge The Simpsons Game et execute son point d'entree recompile.
#include "runtime.h"
#include "renderer.h"

#include <file.h>
#include <image.h>
#include <xex.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <execinfo.h>
#ifdef SIMPSONS_TVOS
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <SDL3/SDL_main.h> // SDL starts the application (UIKit) and calls main on the main thread
#endif
#endif

extern std::unordered_map<size_t, const char*> XboxKernelExports;
extern std::unordered_map<size_t, const char*> XamExports;

uint8_t* g_base = nullptr;

void InitFileSystem(const std::filesystem::path& gameRoot);

struct BumpAllocator
{
    std::mutex mutex;
    uint32_t next;
    uint32_t end;
    const char* name;

    uint32_t Alloc(uint32_t size, uint32_t alignment)
    {
        std::lock_guard lock(mutex);
        uint32_t address = (next + alignment - 1) & ~(alignment - 1);
        if (uint64_t(address) + size > end)
        {
            fprintf(stderr, "[runtime] plus de memoire dans la region %s (demande 0x%X)\n", name, size);
            abort();
        }
        next = address + size;
        memset(g_base + address, 0, size);
        return address;
    }
};

static BumpAllocator g_heap{ {}, GUEST_HEAP_BEGIN, GUEST_HEAP_END, "heap" };
static BumpAllocator g_system{ {}, GUEST_SYSTEM_BEGIN, GUEST_SYSTEM_END, "system" };
// Bump allocator over physical offsets (first page reserved so that 0 stays invalid)
static BumpAllocator g_physical{ {}, GUEST_PHYSICAL_BEGIN + 0x10000, GUEST_PHYSICAL_BEGIN + PHYSICAL_MEMORY_SIZE, "physical" };

uint32_t GuestAllocHeap(uint32_t size, uint32_t alignment) { return g_heap.Alloc(size, alignment); }
uint32_t GuestAllocSystem(uint32_t size, uint32_t alignment) { return g_system.Alloc(size, alignment); }
// The console offsets the 0xE0000000 view by 0x1000 physical bytes. Host pages are 16 KB, so the
// views cannot be offset here: all views share offset 0, and the inline conversions that D3D makes
// (bit 0x1000 computed from address >> 20) are neutralized by the E0NoPhysicalOffset hooks.
uint32_t GuestAllocPhysical(uint32_t size, uint32_t alignment, uint32_t protect)
{
    uint32_t backing = g_physical.Alloc(size, alignment) - GUEST_PHYSICAL_BEGIN;
    if (protect & 0x40000000)
        return 0xC0000000 + backing;
    if (protect & 0x20000000)
        return 0xA0000000 + backing;
    return 0xE0000000 + backing;
}

uint32_t VirtualToPhysical(uint32_t address)
{
    return address & 0x1FFFFFFF;
}

uint32_t PhysicalToVirtual(uint32_t physical)
{
    return GUEST_PHYSICAL_BEGIN + (physical & 0x1FFFFFFF);
}

// Hook placed after each `addi rX, rY, 512` of the inline conversions: removes the +512 so that
// bit 0x1000 of (address >> 20) + 512 is never set
void E0NoPhysicalOffset(PPCRegister& r)
{
    r.u64 -= 512;
}

#ifdef _WIN32
// Windows: the 4 GB guest space is reserved as a placeholder, split into the three physical views (one shared
// section mapped three times) and the rest (ordinary committed memory). VirtualAlloc2/MapViewOfFile3 (Windows 10
// 1803+) are looked up at run time.
static uint8_t* ReserveGuestMemory()
{
    using VirtualAlloc2Func = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Func = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto virtualAlloc2 = kernelBase ? reinterpret_cast<VirtualAlloc2Func>(GetProcAddress(kernelBase, "VirtualAlloc2")) : nullptr;
    auto mapViewOfFile3 = kernelBase ? reinterpret_cast<MapViewOfFile3Func>(GetProcAddress(kernelBase, "MapViewOfFile3")) : nullptr;
    if (virtualAlloc2 == nullptr || mapViewOfFile3 == nullptr)
    {
        fprintf(stderr, "[runtime] Windows 10 version 1803 ou plus recent necessaire\n");
        return nullptr;
    }

    auto* base = static_cast<uint8_t*>(virtualAlloc2(nullptr, nullptr, PPC_MEMORY_SIZE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (base == nullptr)
    {
        fprintf(stderr, "[runtime] reservation 4 Go a echoue (%lu)\n", GetLastError());
        return nullptr;
    }

    // The three 512 MB physical views are contiguous at the top of the 4 GB space (0xA0000000..0x100000000).
    // A placeholder can only be split at one of its own boundaries (not carved from the middle), so: first split
    // off the whole top region, commit the guest RAM below it, then carve each view off the front of what remains.
    const uint32_t PHYS_BASE = 0xA0000000;
    const uint32_t views[] = { 0xA0000000, 0xC0000000, 0xE0000000 };

    // Split the 4 GB placeholder in two at PHYS_BASE: [0, PHYS_BASE) guest RAM, [PHYS_BASE, 4 GB) the views.
    if (!VirtualFree(base + PHYS_BASE, PPC_MEMORY_SIZE - PHYS_BASE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
        fprintf(stderr, "[runtime] decoupe du haut a echoue (%lu)\n", GetLastError());
        return nullptr;
    }
    if (virtualAlloc2(nullptr, base, PHYS_BASE, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != base)
    {
        fprintf(stderr, "[runtime] commit de la RAM a echoue (%lu)\n", GetLastError());
        return nullptr;
    }

    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, PHYSICAL_MEMORY_SIZE, nullptr);
    if (section == nullptr)
    {
        fprintf(stderr, "[runtime] CreateFileMapping a echoue (%lu)\n", GetLastError());
        return nullptr;
    }
    for (size_t i = 0; i < 3; i++)
    {
        // For the first two views, split a 512 MB placeholder off the front of the remaining top region (this
        // shares its start boundary, so it is allowed); the last view is exactly what is left, so no split.
        if (i + 1 < 3 && !VirtualFree(base + views[i], PHYSICAL_MEMORY_SIZE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            fprintf(stderr, "[runtime] decoupe vue %zu a echoue (%lu)\n", i, GetLastError());
            return nullptr;
        }
        if (mapViewOfFile3(section, GetCurrentProcess(), base + views[i], 0, PHYSICAL_MEMORY_SIZE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != base + views[i])
        {
            fprintf(stderr, "[runtime] projection vue %zu a echoue (%lu)\n", i, GetLastError());
            return nullptr;
        }
    }
    return base;
}
#elif defined(SIMPSONS_TVOS)
// tvOS forbids shm_open: the memory at 0xA0000000 is remapped (shared, not copied) at 0xC0000000 and 0xE0000000
static bool MapPhysicalViews()
{
    for (uint32_t address : { 0xC0000000u, 0xE0000000u })
    {
        vm_address_t target = vm_address_t(g_base + address);
        vm_prot_t current, maximum;
        if (vm_remap(mach_task_self(), &target, PHYSICAL_MEMORY_SIZE, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                vm_address_t(g_base + 0xA0000000), FALSE, &current, &maximum, VM_INHERIT_NONE) != KERN_SUCCESS ||
            target != vm_address_t(g_base + address))
            return false;
    }
    return true;
}
#else
// Map the same physical memory at 0xA0000000, 0xC0000000 and 0xE0000000
static bool MapPhysicalViews()
{
    char name[64];
    snprintf(name, sizeof(name), "/simpsons-recomp-%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    shm_unlink(name);
    if (ftruncate(fd, PHYSICAL_MEMORY_SIZE) != 0)
        return false;

    struct View { uint32_t address; uint32_t offset; };
    const View views[] = { { 0xA0000000, 0 }, { 0xC0000000, 0 }, { 0xE0000000, 0 } };
    for (auto& view : views)
    {
        void* mapped = mmap(g_base + view.address, PHYSICAL_MEMORY_SIZE - view.offset, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED, fd, view.offset);
        if (mapped == MAP_FAILED)
            return false;
    }
    close(fd);
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Logging

static std::mutex g_logMutex;
static std::atomic<ImportCounter*> g_importCounters{ nullptr }; // every counter used at least once
static std::unordered_map<std::string, ImportCounter*> g_namedCounters; // LogImportCall (g_logMutex)
static const uint32_t MAX_LOGS_PER_IMPORT = getenv("SIMPSONS_LOG_MAX") ? uint32_t(atoi(getenv("SIMPSONS_LOG_MAX"))) : 8;

void LogImportCallCounted(ImportCounter& counter, const PPCContext& ctx, bool implemented)
{
    uint32_t count = counter.count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > MAX_LOGS_PER_IMPORT + 1)
        return;
    std::lock_guard lock(g_logMutex);
    if (count == 1)
    {
        counter.next = g_importCounters.load();
        g_importCounters.store(&counter);
    }
    if (count <= MAX_LOGS_PER_IMPORT)
        fprintf(stderr, "[kernel]%s %s(0x%X, 0x%X, 0x%X, 0x%X) lr=0x%llX\n", implemented ? "" : " STUB", counter.name,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (unsigned long long)GuestLR(ctx));
    else
        fprintf(stderr, "[kernel] %s : appels suivants masques\n", counter.name);
}

void LogImportCall(const char* name, const PPCContext& ctx, bool implemented)
{
    ImportCounter* counter;
    {
        std::lock_guard lock(g_logMutex);
        auto& slot = g_namedCounters[name];
        if (slot == nullptr)
            slot = new ImportCounter{ name };
        counter = slot;
    }
    LogImportCallCounted(*counter, ctx, implemented);
}

static void PrintCallSummary()
{
    size_t count = 0;
    for (ImportCounter* c = g_importCounters.load(); c != nullptr; c = c->next)
        count++;
    fprintf(stderr, "\n[runtime] %zu fonctions systeme differentes appelees\n", count);
}

// SIMPSONS_STATS=<seconds>: periodically print the most called kernel functions since the last report
void PrintGpuStats();
void PrintShaderStats();
void PrintDrawStats();
void IndexShaderContainers(const uint8_t* image, size_t size);

static void StartStatsReporter()
{
    const char* interval = getenv("SIMPSONS_STATS");
    if (interval == nullptr)
        return;
    std::thread([seconds = atoi(interval)]
        {
            std::unordered_map<const ImportCounter*, uint32_t> previous;
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                std::vector<std::pair<uint32_t, std::string>> deltas;
                for (ImportCounter* c = g_importCounters.load(); c != nullptr; c = c->next)
                {
                    uint32_t count = c->count.load(std::memory_order_relaxed);
                    if (count != previous[c])
                        deltas.emplace_back(count - previous[c], c->name);
                    previous[c] = count;
                }
                std::sort(deltas.rbegin(), deltas.rend());
                fprintf(stderr, "[stats] appels pendant les %d dernieres secondes :", seconds);
                for (size_t i = 0; i < std::min<size_t>(deltas.size(), 14); i++)
                    fprintf(stderr, " %s=%u", deltas[i].second.c_str(), deltas[i].first);
                fprintf(stderr, "\n");
                PrintGpuStats();
                PrintShaderStats();
                PrintDrawStats();
            }
        }).detach();
}

// ---------------------------------------------------------------------------
// Crash handler: translate host faults into guest addresses

PPCContext* CurrentGuestContext(uint32_t* threadId);
void StartDebugDump();
void PrintGpuStats();

// Indirect call to an address that is not a function (see lib/recomp_overrides.h): skipped, registers unchanged
void GuestInvalidIndirectCall(PPCContext& ctx, uint32_t target)
{
    static std::atomic<uint32_t> reports{ 0 };
    if (reports++ < 10)
        fprintf(stderr, "[runtime] appel ignore : 0x%08X n'est pas une fonction (lr=0x%llX)\n", target,
            (unsigned long long)GuestLR(ctx));
}

static void ReportCrash(int sig, void* faultAddress)
{
    auto address = reinterpret_cast<uint8_t*>(faultAddress);
    char message[256];
    int length;
    if (address >= g_base && address < g_base + PPC_MEMORY_SIZE)
        length = snprintf(message, sizeof(message), "\n[runtime] CRASH signal %d : acces a l'adresse invite 0x%08X\n", sig, uint32_t(address - g_base));
    else
        length = snprintf(message, sizeof(message), "\n[runtime] CRASH signal %d : adresse hote %p (memoire invitee a %p)\n", sig, faultAddress,
            static_cast<void*>(g_base));
#ifdef _WIN32
    fwrite(message, 1, length, stderr);
#else
    write(STDERR_FILENO, message, length);
    void* frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif

    uint32_t threadId = 0;
    if (PPCContext* ctx = CurrentGuestContext(&threadId))
    {
        fprintf(stderr, "[runtime] thread invite %u, lr=0x%llX\n", threadId, (unsigned long long)GuestLR(*ctx));
        for (int i = 0; i < 32; i++)
            fprintf(stderr, "r%-2d=%08X%s", i, GuestGPR(*ctx, i), i % 8 == 7 ? "\n" : " ");

        // SIMPSONS_CRASH_MEM="r23,r1+60,82000000,r23+C@0@20": 16 words at each address (register + hex
        // offset; "@hex" dereferences then adds an offset)
        if (const char* list = getenv("SIMPSONS_CRASH_MEM"))
        {
            for (const char* p = list; *p;)
            {
                uint32_t address = 0;
                if (*p == 'r')
                {
                    int index = int(strtol(p + 1, const_cast<char**>(&p), 10));
                    address = index >= 0 && index < 32 ? GuestGPR(*ctx, index) : 0;
                    if (*p == '+')
                        address += uint32_t(strtoul(p + 1, const_cast<char**>(&p), 16));
                }
                else
                    address = uint32_t(strtoul(p, const_cast<char**>(&p), 16));
                // "@hex": follow the pointer at the current address, then add the offset
                while (*p == '@' && address >= 0x10000)
                    address = LoadBE32(address) + uint32_t(strtoul(p + 1, const_cast<char**>(&p), 16));
                fprintf(stderr, "[memoire] %08X :", address);
                for (uint32_t i = 0; i < 16 && address >= 0x10000; i++)
                    fprintf(stderr, " %08X", LoadBE32(address + i * 4));
                fprintf(stderr, "\n");
                while (*p && *p != ',')
                    p++;
                if (*p == ',')
                    p++;
            }
        }
    }
    PrintCallSummary();
    fflush(stderr);
    _exit(1);
}

#ifdef _WIN32
// Windows: the exception code takes the place of the signal number (0xC0000005 = access violation), the
// faulting data address is the second parameter of an access violation
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception)
{
    const EXCEPTION_RECORD* record = exception->ExceptionRecord;
    void* address = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
        ? reinterpret_cast<void*>(record->ExceptionInformation[1]) : record->ExceptionAddress;
    fprintf(stderr, "[runtime] exception 0x%08lX a %p\n", record->ExceptionCode, record->ExceptionAddress);
    ReportCrash(int(record->ExceptionCode), address);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void CrashHandler(int sig, siginfo_t* info, void*)
{
    ReportCrash(sig, info->si_addr);
}
#endif

// ---------------------------------------------------------------------------
// Indirect calls to addresses that are not recompiled functions land here

static void UnknownFunctionTrap(PPCContext& __restrict ctx, uint8_t* base)
{
    fprintf(stderr, "[runtime] appel indirect vers 0x%08X, inconnu (lr=0x%llX, r3=0x%X)\n",
        GuestCTR(ctx), (unsigned long long)GuestLR(ctx), ctx.r3.u32);
    abort();
}

// ---------------------------------------------------------------------------
// Per-thread structures (PCR, TLS, KTHREAD, stack)

static uint32_t g_tlsTemplateAddress = 0;
static uint32_t g_tlsTemplateSize = 0;
static uint32_t g_tlsTotalSize = 0x1000;

uint32_t InitGuestThreadContext(PPCContext& ctx, uint32_t stackSize)
{
    stackSize = (stackSize + 0xFFFF) & ~0xFFFF;
    uint32_t stack = GuestAllocSystem(stackSize, 0x10000);
    uint32_t pcr = GuestAllocSystem(0x1000, 0x1000);
    uint32_t thread = GuestAllocSystem(0x1000, 0x10);
    uint32_t tls = GuestAllocSystem(g_tlsTotalSize, 0x10);

    if (g_tlsTemplateSize != 0)
        memcpy(g_base + tls, g_base + g_tlsTemplateAddress, g_tlsTemplateSize);

    // KPCR layout (subset): TLS pointer, self pointer, stack bounds, current thread, CPU number
    StoreBE32(pcr + 0x000, tls);
    StoreBE32(pcr + 0x030, pcr);
    StoreBE32(pcr + 0x070, stack + stackSize);
    StoreBE32(pcr + 0x074, stack);
    StoreBE32(pcr + 0x100, thread);
    g_base[pcr + 0x10C] = 0;

    ctx.r1.u64 = stack + stackSize - 0x100;
    ctx.r13.u64 = pcr;
    return thread;
}

// ---------------------------------------------------------------------------
// XEX loading

uint32_t g_executionId = 0; // guest address of the XEX_EXECUTION_ID

struct Xex2TlsInfo
{
    be<uint32_t> slotCount;
    be<uint32_t> rawDataAddress;
    be<uint32_t> dataSize;
    be<uint32_t> rawDataSize;
};

// KeTimeStampBundle: interrupt time (100 ns since boot), system time (FILETIME), tick count (ms).
// GetTickCount reads it directly, so it is refreshed every millisecond.
static void StartTimeStampBundle(uint32_t address)
{
    std::thread([address]
        {
            auto boot = std::chrono::steady_clock::now();
            while (true)
            {
                auto elapsed = std::chrono::steady_clock::now() - boot;
                uint64_t interrupt = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
                uint64_t system = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / 100) + 116444736000000000ull;
                StoreBE32(address + 0, uint32_t(interrupt >> 32));
                StoreBE32(address + 4, uint32_t(interrupt));
                StoreBE32(address + 8, uint32_t(system >> 32));
                StoreBE32(address + 12, uint32_t(system));
                StoreBE32(address + 16, uint32_t(interrupt / 10000));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
}

static void ResolveVariableImports(const uint8_t* xex, const Image& image)
{
    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
    if (imports == nullptr)
        return;

    std::unordered_set<std::string> functionNames;
    for (auto& symbol : image.symbols)
        functionNames.insert(symbol.name);

    std::vector<std::string_view> libraryNames;
    auto* strings = reinterpret_cast<const char*>(imports + 1);
    size_t offset = 0;
    for (size_t i = 0; i < imports->numImports; i++)
    {
        libraryNames.emplace_back(strings + offset);
        offset += ((libraryNames.back().length() + 1) + 3) & ~3;
    }

    auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    for (size_t i = 0; i < libraryNames.size(); i++)
    {
        auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
        auto* names = libraryNames[i] == "xam.xex" ? &XamExports : &XboxKernelExports;

        for (size_t j = 0; j < library->numberOfImports; j++)
        {
            uint32_t slot = descriptors[j].firstThunk;
            // XenonUtils left variable thunks byte-swapped to host order
            uint32_t data = *reinterpret_cast<uint32_t*>(g_base + slot);
            uint32_t type = data >> 24;
            uint32_t ordinal = data & 0xFFFF;
            // Function thunks were already replaced with code by XenonUtils
            if (type != 0 || data == 0x00000060)
                continue;

            // Address table entry of an imported function: indirect calls through it are not supported yet
            auto name = names->find(ordinal);
            if (name != names->end() && functionNames.contains(name->second))
                continue;

            uint32_t variable = GuestAllocSystem(0x1000, 0x10);
            StoreBE32(slot, variable);
            if (name != names->end() && std::string_view(name->second).ends_with("KeTimeStampBundle"))
                StartTimeStampBundle(variable);
            fprintf(stderr, "[runtime] variable importee %.*s!%s -> 0x%X\n", int(libraryNames[i].size()), libraryNames[i].data(),
                name != names->end() ? name->second : "?", variable);
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
}

// Profile-guided optimization: the instrumented build writes its counters at exit (weak: absent otherwise)
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

// SIMPSONS_DUREE=<seconds>: quits after that time, writing the profile counters first (PGO training runs)
bool RuntimeLowMemory()
{
#ifdef SIMPSONS_TVOS
    return true;
#else
    static const bool low = getenv("SIMPSONS_MEMOIRE_REDUITE") != nullptr;
    return low;
#endif
}

static void StartQuitTimer()
{
    const char* seconds = getenv("SIMPSONS_DUREE");
    if (seconds == nullptr)
        return;
    std::thread([delay = atoi(seconds)]
        {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            if (__llvm_profile_write_file)
                __llvm_profile_write_file();
            fprintf(stderr, "[runtime] duree ecoulee, fin\n");
            _exit(0);
        }).detach();
}

int main(int argc, char** argv)
{
    StartQuitTimer();
#ifdef _WIN32
    // Started without arguments (double click): the game data is in the "game" folder next to the program, the
    // saves and the log (jeu.log) in %LOCALAPPDATA%\Les Simpson - Le Jeu, which is also the working directory
    std::string defaultXex = "game/default.xex";
    if (argc <= 1)
    {
        wchar_t program[MAX_PATH];
        GetModuleFileNameW(nullptr, program, MAX_PATH);
        defaultXex = (std::filesystem::path(program).parent_path() / "game" / "default.xex").string();
        if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"))
        {
            std::filesystem::path support = std::filesystem::path(local) / "Les Simpson - Le Jeu";
            std::filesystem::create_directories(support / "donnees");
            std::filesystem::create_directories(support / "recomp");
            std::filesystem::current_path(support);
            if (getenv("SIMPSONS_DONNEES") == nullptr)
                _putenv_s("SIMPSONS_DONNEES", (support / "donnees").string().c_str());
            if (getenv("SIMPSONS_CONSOLE") == nullptr)
                _wfreopen((support / "jeu.log").c_str(), L"w", stderr);
        }
    }
    setvbuf(stderr, nullptr, _IONBF, 0);
    const char* xexPath = argc > 1 ? argv[1] : defaultXex.c_str();
#elif defined(SIMPSONS_TVOS)
    // tvOS: the game data is in the application ("game" next to the program); only Library/Caches is writable
    // (tvOS may empty it when storage runs low): saves, shader cache and log (jeu.log, SIMPSONS_CONSOLE=1 keeps
    // stderr) go to Library/Caches/Les Simpson - Le Jeu, which is also the working directory
    std::string defaultXex = "game/default.xex";
    {
        char program[4096];
        uint32_t size = sizeof(program);
        if (_NSGetExecutablePath(program, &size) == 0)
            defaultXex = (std::filesystem::path(program).parent_path() / "game" / "default.xex").string();
        if (const char* home = getenv("HOME"))
        {
            std::filesystem::path support = std::filesystem::path(home) / "Library/Caches/Les Simpson - Le Jeu";
            std::filesystem::create_directories(support / "donnees");
            std::filesystem::create_directories(support / "recomp");
            std::filesystem::current_path(support);
            setenv("SIMPSONS_DONNEES", (support / "donnees").c_str(), 0);
            if (getenv("SIMPSONS_CONSOLE") == nullptr)
                freopen((support / "jeu.log").c_str(), "w", stderr);
        }
    }
    setvbuf(stderr, nullptr, _IOLBF, 0);
    const char* xexPath = argc > 1 ? argv[1] : defaultXex.c_str();
#elif defined(__APPLE__)
    setvbuf(stderr, nullptr, _IOLBF, 0);
    const char* xexPath = argc > 1 ? argv[1] : "game/default.xex";
#else
    // Linux, started without arguments: the game data is in the "game" folder next to the program, the saves and
    // the log (jeu.log) in ~/.local/share/les-simpson-le-jeu, which is also the working directory
    std::string defaultXex = "game/default.xex";
    if (argc <= 1)
    {
        std::error_code error;
        std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
            defaultXex = (program.parent_path() / "game" / "default.xex").string();
        const char* dataHome = getenv("XDG_DATA_HOME");
        const char* home = getenv("HOME");
        if (dataHome != nullptr || home != nullptr)
        {
            std::filesystem::path support = dataHome ? std::filesystem::path(dataHome) / "les-simpson-le-jeu"
                                                     : std::filesystem::path(home) / ".local/share/les-simpson-le-jeu";
            std::filesystem::create_directories(support / "donnees");
            std::filesystem::create_directories(support / "recomp");
            std::filesystem::current_path(support);
            setenv("SIMPSONS_DONNEES", (support / "donnees").c_str(), 0);
            if (getenv("SIMPSONS_CONSOLE") == nullptr)
                freopen((support / "jeu.log").c_str(), "w", stderr);
        }
    }
    setvbuf(stderr, nullptr, _IOLBF, 0);
    const char* xexPath = argc > 1 ? argv[1] : defaultXex.c_str();
#endif
    auto xex = LoadFile(xexPath);
    if (xex.empty())
    {
        fprintf(stderr, "Impossible de lire %s\n", xexPath);
#ifdef _WIN32
        MessageBoxW(nullptr, L"Les fichiers du jeu sont introuvables : le dossier \"game\" doit etre a cote du programme.",
            L"Les Simpson - Le Jeu", MB_ICONERROR);
#endif
        return 1;
    }

#ifdef _WIN32
    g_base = ReserveGuestMemory();
    if (g_base == nullptr)
    {
        fprintf(stderr, "[runtime] impossible de reserver la memoire du jeu (erreur %lu)\n", GetLastError());
        return 1;
    }
    SetUnhandledExceptionFilter(CrashFilter);
    // Sleeps and timed waits (frame pacing, vblank, GPU polling) to the millisecond instead of 15.6 ms
    timeBeginPeriod(1);
#else
    g_base = static_cast<uint8_t*>(mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0));
    if (g_base == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    if (!MapPhysicalViews())
    {
        perror("memoire physique");
        return 1;
    }

    struct sigaction action{};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
#endif

    Image image = Image::ParseImage(xex.data(), xex.size());
    memcpy(g_base + image.base, image.data.get(), image.size);
    // XEX_EXECUTION_ID (optional header 0x00040006), returned by XamGetExecutionId
    {
        uint32_t count = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(xex.data() + 0x14));
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t* entry = reinterpret_cast<const uint32_t*>(xex.data() + 0x18 + i * 8);
            if (__builtin_bswap32(entry[0]) == 0x00040006)
            {
                g_executionId = GuestAllocSystem(24, 8);
                memcpy(GuestToHost(g_executionId), xex.data() + __builtin_bswap32(entry[1]), 24);
            }
        }
    }
    fprintf(stderr, "[runtime] image chargee a 0x%zX (0x%X octets), point d'entree 0x%zX\n", image.base, image.size, image.entry_point);

    ResolveVariableImports(xex.data(), image);
    IndexShaderContainers(g_base + image.base, image.size);
    InitFileSystem(std::filesystem::absolute(xexPath).parent_path());

    if (auto* tls = reinterpret_cast<const Xex2TlsInfo*>(getOptHeaderPtr(xex.data(), XEX_HEADER_TLS_INFO)))
    {
        g_tlsTemplateAddress = tls->rawDataAddress;
        g_tlsTemplateSize = tls->rawDataSize;
        g_tlsTotalSize = std::max<uint32_t>(0x1000, (tls->dataSize + 0xF) & ~0xF);
        fprintf(stderr, "[runtime] TLS : %u slots, modele 0x%X (%u octets), taille %u\n", uint32_t(tls->slotCount), g_tlsTemplateAddress, g_tlsTemplateSize, uint32_t(tls->dataSize));
    }

    size_t functionCount = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++, functionCount++)
        PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;
    for (uint32_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
    {
        if (PPC_LOOKUP_FUNC(g_base, address) == nullptr)
            PPC_LOOKUP_FUNC(g_base, address) = UnknownFunctionTrap;
    }
    fprintf(stderr, "[runtime] %zu fonctions enregistrees\n", functionCount);

    // The first 64 KB stay readable and writable: the game itself dereferences an out-of-range element in
    // some levels (Shadow of the Colossal Donut: a 3-element path read at index 3), which the console survives.
    // SIMPSONS_GARDE_NULL=1 makes them inaccessible to catch null pointer accesses while debugging.
    if (getenv("SIMPSONS_GARDE_NULL"))
    {
#ifdef _WIN32
        DWORD previous;
        VirtualProtect(g_base, GUEST_NULL_GUARD_SIZE, PAGE_NOACCESS, &previous);
#else
        mprotect(g_base, GUEST_NULL_GUARD_SIZE, PROT_NONE);
#endif
    }

    uint32_t stackSize = 0x40000;
    if (auto* size = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(xex.data(), XEX_HEADER_DEFAULT_STACK_SIZE)))
        stackSize = std::max<uint32_t>(stackSize, *size);

    // macOS requires the window and its events on the process main thread: the game runs on its own
    // thread, with a large host stack for deeply nested recompiled calls
    static uint32_t s_stackSize = stackSize;
    static uint32_t s_entryPoint = uint32_t(image.entry_point);
    static auto runGame = [](void*) -> void*
    {
        PPCContext& ctx = InitMainGuestThread(s_stackSize);
        StartDebugDump();
        StartStatsReporter();

        fprintf(stderr, "[runtime] demarrage du jeu...\n");
        PPCFunc* entry = PPC_LOOKUP_FUNC(g_base, s_entryPoint);
        entry(ctx, g_base);

        fprintf(stderr, "[runtime] le point d'entree est revenu (r3=0x%X)\n", ctx.r3.u32);
        PrintCallSummary();
        _exit(0);
        return nullptr;
    };

    bool windowed = getenv("SIMPSONS_SANS_FENETRE") == nullptr && RendererInit();
    if (!windowed && getenv("SIMPSONS_SANS_FENETRE") == nullptr)
    {
        fprintf(stderr, "[runtime] affichage impossible, arret\n");
        RendererShowStartupError();
        return 1;
    }

#ifdef _WIN32
    HANDLE gameThread = CreateThread(nullptr, 256 * 1024 * 1024, [](LPVOID) -> DWORD { runGame(nullptr); return 0; }, nullptr,
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024 * 1024);
    pthread_t gameThread;
    pthread_create(&gameThread, &attributes, runGame, nullptr);
#endif

    if (windowed)
    {
        RendererRunEventLoop();
        fprintf(stderr, "[runtime] fenetre fermee\n");
        _exit(0);
    }
#ifdef _WIN32
    WaitForSingleObject(gameThread, INFINITE);
#else
    pthread_join(gameThread, nullptr);
#endif
    return 0;
}
