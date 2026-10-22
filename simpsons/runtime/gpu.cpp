// Faux GPU Xenos : consomme le tampon de commandes (PM4) sans dessiner.
//
// Il fait avancer le pointeur de lecture, execute les paquets qui ecrivent en memoire
// (barrieres, compteurs, registres scratch), declenche les interruptions demandees
// et envoie une interruption vblank a 60 Hz. Suffisant pour que le jeu croie avoir un GPU.
#include "runtime.h"
#include "renderer.h"
#include "textures.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

constexpr uint32_t GPU_REGISTER_BASE = 0x7FC80000;
constexpr uint32_t REG_CP_RB_RPTR = 0x01C4;
constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
constexpr uint32_t REG_SCRATCH_UMSK = 0x0576;
constexpr uint32_t REG_SCRATCH_ADDR = 0x0577;
constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
constexpr uint32_t REG_D1MODE_VBLANK_STATUS = 0x1951; // bit 0: vblank pending, checked by the interrupt handler
constexpr uint32_t REG_D1MODE_VIEWPORT_SIZE = 0x1961;

// PM4 type 3 opcodes handled here
enum Pm4Opcode : uint32_t
{
    PM4_NOP = 0x10,
    PM4_WAIT_REG_MEM = 0x3C,
    PM4_MEM_WRITE = 0x3D,
    PM4_REG_RMW = 0x21,
    PM4_INDIRECT_BUFFER = 0x37,
    PM4_INDIRECT_BUFFER_PFD = 0x3F,
    PM4_COND_WRITE = 0x45,
    PM4_EVENT_WRITE_SHD = 0x58,
    PM4_INTERRUPT = 0x54,
    PM4_DRAW_INDX = 0x22,
    PM4_DRAW_INDX_2 = 0x36,
    PM4_XE_SWAP = 0x64, // runtime-specific, written by VdSwap
    PM4_IM_LOAD = 0x27,
    PM4_IM_LOAD_IMMEDIATE = 0x2B,
    PM4_SET_CONSTANT = 0x2D,
    PM4_LOAD_ALU_CONSTANT = 0x2F,
};

void GpuLoadShader(bool pixelShader, const uint8_t* microcode, uint32_t sizeBytes);
void GpuSetConstants(uint32_t type, uint32_t index, const uint32_t* bigEndianValues, uint32_t count);
void GpuDraw(uint32_t drawInitiator, uint32_t indexBase, uint32_t indexInfo, const uint32_t* immediateIndices);

struct KnownShader;
const KnownShader* LookupShaderMicrocode(const uint8_t* microcode, uint32_t size);

static uint32_t g_interruptCallback = 0;
static uint32_t g_interruptUserData = 0;
static uint32_t g_ringBase = 0;
static uint32_t g_ringSizeDwords = 0;
static uint32_t g_readPointer = 0;
static uint32_t g_readPointerWriteBack = 0;

static std::atomic<uint64_t> g_frameCount{ 0 };
static std::atomic<uint64_t> g_drawCount{ 0 };
static std::atomic<uint32_t> g_swapCounter{ 0 };

static std::mutex g_interruptMutex;
static std::condition_variable g_interruptCondition;
struct PendingInterrupt
{
    uint32_t source;
    uint32_t cpu;
};
static std::deque<PendingInterrupt> g_pendingInterrupts;
static std::atomic<bool> g_gpuStarted{ false };

// SIMPSONS_GPU_STATS: count packets per opcode and register writes per register
static std::mutex g_statsMutex;
static std::unordered_map<uint32_t, uint64_t> g_opcodeCounts;
static std::unordered_map<uint32_t, uint64_t> g_registerCounts;
static const bool g_gpuStats = getenv("SIMPSONS_GPU_STATS") != nullptr;

void PrintGpuStats()
{
    std::lock_guard lock(g_statsMutex);
    std::vector<std::pair<uint64_t, uint32_t>> ops, regs;
    for (auto [k, v] : g_opcodeCounts) ops.emplace_back(v, k);
    for (auto [k, v] : g_registerCounts) regs.emplace_back(v, k);
    std::sort(ops.rbegin(), ops.rend());
    std::sort(regs.rbegin(), regs.rend());
    fprintf(stderr, "[gpu] opcodes :");
    for (auto [v, k] : ops) fprintf(stderr, " %02X=%llu", k, (unsigned long long)v);
    fprintf(stderr, "\n[gpu] %zu registres differents, les plus ecrits :", regs.size());
    for (size_t i = 0; i < std::min<size_t>(regs.size(), 40); i++) fprintf(stderr, " %04X=%llu", regs[i].second, (unsigned long long)regs[i].first);
    fprintf(stderr, "\n");
}

static uint32_t ReadRegister(uint32_t index)
{
    return LoadBE32(GPU_REGISTER_BASE + index * 4);
}

static bool TraceGpu();
void GpuProfileReport(uint64_t frame);

static void WriteRegister(uint32_t index, uint32_t value)
{
    if (g_gpuStats)
    {
        std::lock_guard lock(g_statsMutex);
        g_registerCounts[index]++;
    }
    if (TraceGpu() && index >= REG_SCRATCH_UMSK && index < REG_SCRATCH_REG0 + 8)
        fprintf(stderr, "[gpu] registre 0x%X <- 0x%X\n", index, value);

    StoreBE32(GPU_REGISTER_BASE + index * 4, value);

    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        // Scratch registers are mirrored to memory when enabled in the mask
        uint32_t scratch = index - REG_SCRATCH_REG0;
        if (ReadRegister(REG_SCRATCH_UMSK) & (1u << scratch))
        {
            StoreBE32(PhysicalToVirtual(ReadRegister(REG_SCRATCH_ADDR)) + scratch * 4, value);
        }
        else if (g_interruptUserData != 0)
        {
            // The game never programs SCRATCH_ADDR (the console kernel does). Its Direct3D device,
            // passed as the interrupt callback user data, points to the scratch mirror at 0x2A94
            // (April 2007 XDK layout). The game polls and resets that mirror itself.
            uint32_t mirror = LoadBE32(g_interruptUserData + 0x2A94);
            if (mirror != 0)
                StoreBE32(mirror + scratch * 4, value);
        }
    }
}

// Consecutive registers from big-endian words (ring buffer data): copied as a block when no register of the range
// has side effects (scratch registers) and no statistic or trace is requested
static void WriteRegisterBlock(uint32_t index, const uint32_t* bigEndianValues, uint32_t count)
{
    bool touchesScratch = index <= REG_SCRATCH_REG0 + 7 && index + count > REG_SCRATCH_UMSK;
    if (!g_gpuStats && !TraceGpu() && !touchesScratch && index + count <= 0x10000)
    {
        memcpy(GuestToHost(GPU_REGISTER_BASE + index * 4), bigEndianValues, size_t(count) * 4);
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        WriteRegister(index + i, __builtin_bswap32(bigEndianValues[i]));
}

void GpuWriteRegisterBlock(uint32_t index, const uint32_t* bigEndianValues, uint32_t count)
{
    WriteRegisterBlock(index, bigEndianValues, count);
}

// Source 1 (command processor) interrupts target the CPUs in the mask; the handler acknowledges
// by clearing the bit of the CPU it runs on (KPCR+0x10C), so it is called once per CPU.
uint64_t GpuFrameCount()
{
    return g_frameCount;
}

// Register file (big-endian words) for block reads
const uint32_t* GpuRegisterPointer(uint32_t index)
{
    return static_cast<const uint32_t*>(GuestToHost(GPU_REGISTER_BASE + index * 4));
}

uint32_t GpuReadRegister(uint32_t index)
{
    return ReadRegister(index);
}

void GpuWriteRegister(uint32_t index, uint32_t value)
{
    WriteRegister(index, value);
}

void InterruptEnter(uint32_t cpu);
void InterruptLeave(uint32_t cpu);

// Runs the graphics interrupt callback on the calling host thread (its own guest context, created once)
static void CallInterruptCallback(uint32_t source, uint32_t cpu)
{
    if (g_interruptCallback == 0)
        return;
    thread_local PPCContext* context = nullptr;
    if (context == nullptr)
        context = &InitMainGuestThread(0x10000);
    PPCContext& ctx = *context;
    g_base[ctx.r13.u32 + 0x10C] = uint8_t(cpu);
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_interruptUserData;
    PPCFunc* callback = PPC_LOOKUP_FUNC(g_base, g_interruptCallback);
    InterruptEnter(cpu); // waits while a thread of that CPU is at DISPATCH_LEVEL
    // Safety net: the game's handler calls the function of its pending request (device + 10900, fields 16/20) and
    // then acknowledges by clearing its CPU bit in the request's first word. Rarely, the request holds unrelated
    // data when the interrupt arrives (timing of the game's Direct3D): calling it would crash. In that case only
    // the acknowledgement is done.
    uint32_t request = source == 1 && g_interruptUserData ? LoadBE32(g_interruptUserData + 10900) : 0;
    uint32_t function = request ? LoadBE32(request + 16) : 0;
    bool validFunction = function == 0 || (function >= PPC_CODE_BASE && function < PPC_CODE_BASE + PPC_CODE_SIZE &&
        (function & 3) == 0 && PPC_LOOKUP_FUNC(g_base, function) != nullptr);
    if (request != 0 && !validFunction)
    {
        static std::atomic<uint32_t> reports{ 0 };
        if (reports++ < 5)
            fprintf(stderr, "[gpu] interruption ignoree : requete 0x%08X sans fonction valide (0x%08X)\n", request, function);
        auto* word = static_cast<uint32_t*>(GuestToHost(request));
        uint32_t clear = __builtin_bswap32(1u << cpu);
        __atomic_fetch_and(word, ~clear, __ATOMIC_SEQ_CST);
    }
    else
        callback(ctx, g_base);
    InterruptLeave(cpu);
}

static void DeliverCommandProcessorInterrupt(uint32_t cpuMask)
{
    for (uint32_t cpu = 0; cpu < 6; cpu++)
    {
        if (cpuMask & (1u << cpu))
            CallInterruptCallback(1, cpu);
    }
}

static void RaiseInterrupt(uint32_t source, uint32_t cpuMask)
{
    std::lock_guard lock(g_interruptMutex);
    for (uint32_t cpu = 0; cpu < 6; cpu++)
    {
        if (cpuMask & (1u << cpu))
            g_pendingInterrupts.push_back({ source, cpu });
    }
    g_interruptCondition.notify_one();
}

static bool CompareWait(uint32_t function, uint32_t value, uint32_t reference)
{
    switch (function & 0x7)
    {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;
    }
}

static void LogUnknownOpcode(uint32_t opcode)
{
    static std::mutex mutex;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard lock(mutex);
    if (seen.insert(opcode).second)
        fprintf(stderr, "[gpu] opcode PM4 non gere 0x%02X (ignore)\n", opcode);
}

static void ExecuteBuffer(uint32_t address, uint32_t sizeDwords);

static bool TraceGpu()
{
    static bool enabled = getenv("SIMPSONS_TRACE_GPU") != nullptr;
    return enabled;
}

// Returns the packet length in dwords
static uint32_t ExecutePacket(uint32_t address)
{
    uint32_t header = LoadBE32(address);
    auto arg = [&](uint32_t i) { return LoadBE32(address + 4 + i * 4); };

    switch (header >> 30)
    {
    case 0:
    {
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t index = header & 0x7FFF;
        bool singleRegister = header & 0x8000;
        if (!singleRegister)
            WriteRegisterBlock(index, static_cast<const uint32_t*>(GuestToHost(address + 4)), count);
        else
            for (uint32_t i = 0; i < count; i++)
                WriteRegister(index, arg(i));
        return count + 1;
    }
    case 1:
        WriteRegister(header & 0x7FF, arg(0));
        WriteRegister((header >> 11) & 0x7FF, arg(1));
        return 3;
    case 2:
        return 1;
    default:
        break;
    }

    uint32_t opcode = (header >> 8) & 0x7F;
    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (g_gpuStats)
    {
        std::lock_guard lock(g_statsMutex);
        g_opcodeCounts[opcode]++;
    }
    if (TraceGpu() && opcode != PM4_NOP)
    {
        fprintf(stderr, "[gpu] @%08X type3 op=0x%02X n=%u :", address, opcode, count);
        for (uint32_t i = 0; i < std::min<uint32_t>(count, opcode == 0x48 ? 18u : 6u); i++)
            fprintf(stderr, " %08X", arg(i));
        fprintf(stderr, "\n");
    }

    switch (opcode)
    {
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
        ExecuteBuffer(PhysicalToVirtual(arg(0)), arg(1) & 0xFFFFF);
        break;

    case PM4_MEM_WRITE:
    {
        uint32_t target = PhysicalToVirtual(arg(0) & ~3u);
        for (uint32_t i = 1; i < count; i++)
            StoreBE32(target + (i - 1) * 4, arg(i));
        break;
    }

    case PM4_EVENT_WRITE_SHD:
    {
        // (initiator, address, value) - bit 31 of the initiator selects the swap counter
        uint32_t value = (arg(0) & 0x80000000) ? g_swapCounter.load() : arg(2);
        StoreBE32(PhysicalToVirtual(arg(1) & ~3u), value);
        break;
    }

    case PM4_COND_WRITE:
    {
        // (wait_info, poll address/register, reference, mask, write address/register, value)
        uint32_t info = arg(0);
        uint32_t polled = (info & 0x10) ? LoadBE32(PhysicalToVirtual(arg(1) & ~3u)) : ReadRegister(arg(1));
        if (CompareWait(info, polled & arg(3), arg(2)))
        {
            if (info & 0x100)
                StoreBE32(PhysicalToVirtual(arg(4) & ~3u), arg(5));
            else
                WriteRegister(arg(4), arg(5));
        }
        break;
    }

    case PM4_WAIT_REG_MEM:
    {
        // (wait_info, poll address/register, reference, mask, wait interval)
        uint32_t info = arg(0);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (true)
        {
            uint32_t polled = (info & 0x10) ? LoadBE32(PhysicalToVirtual(arg(1) & ~3u)) : ReadRegister(arg(1));
            if (CompareWait(info, polled & arg(3), arg(2)))
                break;
            if (std::chrono::steady_clock::now() > deadline)
            {
                fprintf(stderr, "[gpu] WAIT_REG_MEM bloque 2 s : info=0x%X %s=0x%X valeur=0x%X masque=0x%X attendu=0x%X\n",
                    info, (info & 0x10) ? "adresse" : "registre", arg(1), polled, arg(3), arg(2));
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        break;
    }

    case PM4_REG_RMW:
    {
        uint32_t index = arg(0) & 0x1FFF;
        WriteRegister(index, (ReadRegister(index) & arg(1)) | arg(2));
        break;
    }

    case PM4_INTERRUPT:
        // Command processor interrupt: delivered synchronously, before the following packets, like the console's
        // GPU. Direct3D reuses the interrupt request (callback, argument) once the GPU has moved past this packet:
        // a delayed delivery (other thread) could see the next request half written.
        DeliverCommandProcessorInterrupt(arg(0));
        break;

    case PM4_IM_LOAD:
    {
        // (address | shader type, start << 16 | size in dwords): microcode in memory; type 0 vertex, 1 pixel
        uint32_t sizeDwords = arg(1) & 0xFFFF;
        auto* microcode = static_cast<uint8_t*>(GuestToHost(PhysicalToVirtual(arg(0) & ~3u)));
        LookupShaderMicrocode(microcode, sizeDwords * 4);
        GpuLoadShader((arg(0) & 3) == 1, microcode, sizeDwords * 4);
        break;
    }

    case PM4_IM_LOAD_IMMEDIATE:
    {
        // (shader type, start << 16 | size in dwords, microcode...)
        uint32_t sizeDwords = arg(1) & 0xFFFF;
        auto* microcode = static_cast<uint8_t*>(GuestToHost(address + 12));
        LookupShaderMicrocode(microcode, sizeDwords * 4);
        GpuLoadShader((arg(0) & 3) == 1, microcode, sizeDwords * 4);
        break;
    }

    case PM4_SET_CONSTANT:
    {
        // (index | type << 16, values...)
        uint32_t offsetType = arg(0);
        GpuSetConstants((offsetType >> 16) & 0xFF, offsetType & 0x7FF,
            static_cast<const uint32_t*>(GuestToHost(address + 8)), count - 1);
        break;
    }

    case PM4_LOAD_ALU_CONSTANT:
    {
        // (address, index | type << 16, size in dwords): values copied from memory
        uint32_t source = PhysicalToVirtual(arg(0) & 0x3FFFFFFF);
        uint32_t offsetType = arg(1);
        GpuSetConstants((offsetType >> 16) & 0xFF, offsetType & 0x7FF,
            static_cast<const uint32_t*>(GuestToHost(source)), arg(2) & 0xFFF);
        break;
    }

    case PM4_DRAW_INDX:
        // (viz query, draw initiator, index base, index size | endian)
        g_drawCount++;
        GpuDraw(arg(1), count > 2 ? arg(2) : 0, count > 3 ? arg(3) : 0, nullptr);
        break;

    case PM4_DRAW_INDX_2:
        // (draw initiator, immediate indices...)
        g_drawCount++;
        GpuDraw(arg(0), 0, 0, static_cast<const uint32_t*>(GuestToHost(address + 8)));
        break;

    case PM4_XE_SWAP:
    {
        uint64_t frame = ++g_frameCount;
        RendererPresent(frame);
        TexturesNewFrame();
        GpuProfileReport(g_frameCount);
        // Frame rate (60 images/s in levels, 30 in menus): frame_pacing.cpp
        g_swapCounter++;
        if (frame <= 5 || frame % 300 == 0)
        {
            // Frames per second since the previous report
            static auto start = std::chrono::steady_clock::now();
            static auto last = start;
            static uint64_t lastFrame = 0;
            auto now = std::chrono::steady_clock::now();
            double seconds = std::chrono::duration<double>(now - last).count();
            fprintf(stderr, "[gpu] image %llu affichee (%llu dessins) a %.1f s, %.1f images/s\n", (unsigned long long)frame,
                (unsigned long long)g_drawCount.load(), std::chrono::duration<double>(now - start).count(),
                seconds > 0 ? (frame - lastFrame) / seconds : 0.0);
            last = now;
            lastFrame = frame;
        }
        break;
    }

    case PM4_NOP:
        break;

    default:
        LogUnknownOpcode(opcode);
        break;
    }

    return count + 1;
}

static void ExecuteBuffer(uint32_t address, uint32_t sizeDwords)
{
    uint32_t offset = 0;
    while (offset < sizeDwords)
        offset += ExecutePacket(address + offset * 4);
}

static void GpuThread()
{
    SetGameThreadPriority();
    while (true)
    {
        uint32_t writePointer = ReadRegister(REG_CP_RB_WPTR) % std::max<uint32_t>(g_ringSizeDwords, 1);
        if (g_ringSizeDwords == 0 || writePointer == g_readPointer)
        {
            // Nothing to do. Note: polling faster (yield) exposes a timing assumption of the game's Direct3D (its
            // command processor interrupt handler asserts on 0x0BADF00D when the GPU answers too quickly)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // The game writes its packets, then (after an eieio/sync, now a real fence) the write pointer: this
        // acquire fence keeps the packet reads below from seeing data older than that pointer
        std::atomic_thread_fence(std::memory_order_acquire);

        static int traces = 0;
        if (TraceGpu() && traces++ < 20)
            fprintf(stderr, "[gpu] WPTR=%u RPTR=%u writeback=0x%X\n", writePointer, g_readPointer, g_readPointerWriteBack);

        while (g_readPointer != writePointer)
        {
            uint32_t length = ExecutePacket(g_ringBase + g_readPointer * 4);
            g_readPointer = (g_readPointer + length) % g_ringSizeDwords;
        }

        WriteRegister(REG_CP_RB_RPTR, g_readPointer);
        if (g_readPointerWriteBack != 0)
            StoreBE32(g_readPointerWriteBack, g_readPointer);
    }
}

void InterruptEnter(uint32_t cpu);
void InterruptLeave(uint32_t cpu);

static void InterruptThread()
{
    // Interrupt callbacks run guest code: give this host thread its own guest context
    PPCContext& ctx = InitMainGuestThread(0x10000);
    auto nextVblank = std::chrono::steady_clock::now();

    while (true)
    {
        uint32_t source = 0;
        uint32_t cpu = 0;
        {
            std::unique_lock lock(g_interruptMutex);
            if (g_interruptCondition.wait_until(lock, nextVblank, [] { return !g_pendingInterrupts.empty(); }))
            {
                source = g_pendingInterrupts.front().source;
                cpu = g_pendingInterrupts.front().cpu;
                g_pendingInterrupts.pop_front();
            }
            else
            {
                // SIMPSONS_VBLANK_HZ: vertical blank frequency (default 60)
                static const int hz = getenv("SIMPSONS_VBLANK_HZ") ? std::max(1, atoi(getenv("SIMPSONS_VBLANK_HZ"))) : 60;
                nextVblank += std::chrono::microseconds(1000000 / hz);
            }
        }

        static uint32_t delivered = 0;
        if (g_interruptCallback != 0 && TraceGpu() && delivered++ < 5)
            fprintf(stderr, "[gpu] interruption source %u -> 0x%X(0x%X)\n", source, g_interruptCallback, g_interruptUserData);

        if (source == 0)
            WriteRegister(REG_D1MODE_VBLANK_STATUS, ReadRegister(REG_D1MODE_VBLANK_STATUS) | 1);

        if (g_interruptCallback != 0)
        {
            g_base[ctx.r13.u32 + 0x10C] = uint8_t(cpu);
            ctx.r3.u64 = source;
            ctx.r4.u64 = g_interruptUserData;
            PPCFunc* callback = PPC_LOOKUP_FUNC(g_base, g_interruptCallback);
            InterruptEnter(cpu); // waits while a thread of that CPU is at DISPATCH_LEVEL
            callback(ctx, g_base);
            InterruptLeave(cpu);
        }
    }
}

static void StartGpu()
{
    bool expected = false;
    if (!g_gpuStarted.compare_exchange_strong(expected, true))
        return;
    WriteRegister(REG_D1MODE_VIEWPORT_SIZE, (1280u << 16) | 720u);
    std::thread(GpuThread).detach();
    std::thread(InterruptThread).detach();
    fprintf(stderr, "[gpu] faux GPU demarre\n");
}

// ---------------------------------------------------------------------------
// Vd* kernel functions

KERNEL_FUNC(VdInitializeRingBuffer)
{
    // (VOID* PhysicalAddress, ULONG SizeLog2)
    LOG_CALL(VdInitializeRingBuffer);
    g_ringBase = PhysicalToVirtual(ctx.r3.u32);
    g_ringSizeDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    g_readPointer = 0;
    WriteRegister(REG_CP_RB_WPTR, 0);
    fprintf(stderr, "[gpu] tampon de commandes a 0x%X (%u mots)\n", g_ringBase, g_ringSizeDwords);
    StartGpu();
}

KERNEL_FUNC(VdEnableRingBufferRPtrWriteBack)
{
    // (VOID* PhysicalAddress, ULONG BlockSizeLog2)
    LOG_CALL(VdEnableRingBufferRPtrWriteBack);
    g_readPointerWriteBack = PhysicalToVirtual(ctx.r3.u32);
    StoreBE32(g_readPointerWriteBack, g_readPointer);
}

KERNEL_FUNC(VdSetGraphicsInterruptCallback)
{
    // (VOID* Callback, VOID* UserData)
    LOG_CALL(VdSetGraphicsInterruptCallback);
    g_interruptCallback = ctx.r3.u32;
    g_interruptUserData = ctx.r4.u32;
}

KERNEL_FUNC(VdInitializeEngines)
{
    LOG_CALL(VdInitializeEngines);
    ctx.r3.u64 = 1;
}

KERNEL_FUNC(VdShutdownEngines)
{
    LOG_CALL(VdShutdownEngines);
}

KERNEL_FUNC(VdSwap)
{
    // (DWORD* Buffer, VOID* Fetch, VOID*, DWORD*, DWORD*, DWORD* FrontBuffer, DWORD* Format, DWORD* ColorSpace, ...)
    // The game reserved 64 dwords in its command stream for the swap packet.
    LOG_CALL(VdSwap);
    uint32_t buffer = ctx.r3.u32;
    memset(GuestToHost(buffer), 0, 64 * 4);
    StoreBE32(buffer, (3u << 30) | (63u - 1) << 16 | (PM4_XE_SWAP << 8));
    StoreBE32(buffer + 4, 0x53574150); // 'SWAP'
    if (ctx.r8.u32 != 0)
        StoreBE32(buffer + 8, LoadBE32(ctx.r8.u32) & 0x1FFFFFFF);
    ctx.r3.u64 = 0;
}

static void FillVideoMode(uint32_t mode)
{
    memset(GuestToHost(mode), 0, 0x30);
    StoreBE32(mode + 0x00, 1280);
    StoreBE32(mode + 0x04, 720);
    StoreBE32(mode + 0x08, 0); // progressive
    StoreBE32(mode + 0x0C, 1); // widescreen
    StoreBE32(mode + 0x10, 1); // HD
    float refreshRate = 60.0f;
    uint32_t refreshBits;
    memcpy(&refreshBits, &refreshRate, 4);
    StoreBE32(mode + 0x14, refreshBits);
    StoreBE32(mode + 0x18, 1); // NTSC
    StoreBE32(mode + 0x1C, 0x4A);
    StoreBE32(mode + 0x20, 0x01);
}

KERNEL_FUNC(XGetVideoMode)
{
    LOG_CALL(XGetVideoMode);
    FillVideoMode(ctx.r3.u32);
}

KERNEL_FUNC(VdQueryVideoMode)
{
    LOG_CALL(VdQueryVideoMode);
    FillVideoMode(ctx.r3.u32);
}

KERNEL_FUNC(VdQueryVideoFlags)
{
    LOG_CALL(VdQueryVideoFlags);
    ctx.r3.u64 = 0x3; // widescreen, 720p or more
}

KERNEL_FUNC(VdGetCurrentDisplayGamma)
{
    // (DWORD* Type, FLOAT* Power)
    LOG_CALL(VdGetCurrentDisplayGamma);
    StoreBE32(ctx.r3.u32, 1);
    float power = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &power, 4);
    StoreBE32(ctx.r4.u32, bits);
}

KERNEL_FUNC(VdIsHSIOTrainingSucceeded)
{
    LOG_CALL(VdIsHSIOTrainingSucceeded);
    ctx.r3.u64 = 1;
}

KERNEL_FUNC(VdInitializeScalerCommandBuffer)
{
    // 12 arguments: Destination is the 11th and Count the 12th (stack slots at r1+0x64 and r1+0x6C,
    // checked at the call site 0x8246A57C). Filled with type 2 NOP packets.
    LOG_CALL(VdInitializeScalerCommandBuffer);
    uint32_t destination = PPC_LOAD_U32(ctx.r1.u32 + 0x64);
    uint32_t count = PPC_LOAD_U32(ctx.r1.u32 + 0x6C);
    if (count > 0x1000)
    {
        fprintf(stderr, "[gpu] VdInitializeScalerCommandBuffer : taille suspecte %u, ignore\n", count);
        ctx.r3.u64 = 0;
        return;
    }
    for (uint32_t i = 0; i < count; i++)
        StoreBE32(destination + i * 4, 0x80000000);
    ctx.r3.u64 = count;
}

KERNEL_FUNC(VdGetSystemCommandBuffer)
{
    // (VOID* Buffer, DWORD* Value)
    LOG_CALL(VdGetSystemCommandBuffer);
    memset(GuestToHost(ctx.r3.u32), 0, 0x94);
    StoreBE32(ctx.r3.u32, 0xBEEF0000);
    StoreBE32(ctx.r4.u32, 0xBEEF0001);
}

KERNEL_FUNC(VdRetrainEDRAM)
{
    LOG_CALL(VdRetrainEDRAM);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(VdRetrainEDRAMWorker)
{
    LOG_CALL(VdRetrainEDRAMWorker);
    ctx.r3.u64 = 0;
}
