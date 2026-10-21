// Threads et objets de synchronisation du noyau Xbox 360.
//
// Les objets "dispatcher" (evenements, semaphores, mutants, threads) vivent en memoire
// invite avec un DISPATCHER_HEADER big-endian ; leur etat (SignalState) y est la reference.
// Toutes les attentes passent par un seul mutex + variable de condition : simple et correct.
#include "runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef __APPLE__
#include <pthread.h>
#endif

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;

enum DispatcherType : uint8_t
{
    NotificationEvent = 0,
    SynchronizationEvent = 1,
    Mutant = 2,
    Semaphore = 5,
    Thread = 6,
    NotificationTimer = 8,
    SynchronizationTimer = 9,
};

// DISPATCHER_HEADER offsets
constexpr uint32_t HEADER_TYPE = 0x00;
constexpr uint32_t HEADER_SIGNAL_STATE = 0x04;
constexpr uint32_t SEMAPHORE_LIMIT = 0x10;
constexpr uint32_t MUTANT_OWNER = 0x18;

// RTL_CRITICAL_SECTION offsets (after its embedded synchronization event header)
constexpr uint32_t CS_LOCK_COUNT = 0x10;
constexpr uint32_t CS_RECURSION_COUNT = 0x14;
constexpr uint32_t CS_OWNING_THREAD = 0x18;

// KTHREAD offsets used by the runtime
constexpr uint32_t KTHREAD_THREAD_ID = 0x14C;

static std::mutex g_dispatcherMutex;
static std::condition_variable g_dispatcherCondition;
// Threads currently blocked on g_dispatcherCondition (g_dispatcherMutex held): releasing an object wakes the others
// only when someone waits (most releases of the game's locks have no waiter, and waking is a system call)
static uint32_t g_dispatcherWaiters = 0;
struct DispatcherWaiter
{
    DispatcherWaiter() { g_dispatcherWaiters++; }
    ~DispatcherWaiter() { g_dispatcherWaiters--; }
};

struct GuestThread
{
    uint32_t kthread = 0;
    uint32_t handle = 0;
    uint32_t threadId = 0;
    int32_t suspendCount = 0;
    PPCContext ctx{};
};

static thread_local GuestThread* t_currentThread = nullptr;

PPCContext* CurrentGuestContext(uint32_t* threadId)
{
    if (t_currentThread == nullptr)
        return nullptr;
    *threadId = t_currentThread->threadId;
    return &t_currentThread->ctx;
}

// ---------------------------------------------------------------------------
// Handles

static std::mutex g_handleMutex;
static std::unordered_map<uint32_t, uint32_t> g_handleObjects; // handle -> guest object
static std::unordered_map<uint32_t, GuestThread*> g_threadsByObject; // KTHREAD -> thread
static uint32_t g_nextHandle = 0xF8000004;
static std::atomic<uint32_t> g_nextThreadId{ 1 };

constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;

static uint32_t CreateHandle(uint32_t object)
{
    std::lock_guard lock(g_handleMutex);
    uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handleObjects[handle] = object;
    return handle;
}

static uint32_t CurrentKThread()
{
    return t_currentThread != nullptr ? t_currentThread->kthread : 0;
}

// Accepts a handle or a guest object pointer
static uint32_t ResolveObject(uint32_t handleOrObject)
{
    if (handleOrObject == CURRENT_THREAD_HANDLE)
        return CurrentKThread();

    std::lock_guard lock(g_handleMutex);
    auto it = g_handleObjects.find(handleOrObject);
    return it != g_handleObjects.end() ? it->second : 0;
}

static uint32_t AllocDispatcherObject(DispatcherType type, int32_t signalState, uint32_t size = 0x20)
{
    uint32_t object = GuestAllocSystem(size, 0x10);
    g_base[object + HEADER_TYPE] = type;
    g_base[object + 2] = uint8_t(size / 4);
    StoreBE32(object + HEADER_SIGNAL_STATE, uint32_t(signalState));
    return object;
}

// ---------------------------------------------------------------------------
// Dispatcher (call with g_dispatcherMutex held)

static int32_t SignalState(uint32_t object)
{
    return int32_t(LoadBE32(object + HEADER_SIGNAL_STATE));
}

static void SetSignalState(uint32_t object, int32_t value)
{
    StoreBE32(object + HEADER_SIGNAL_STATE, uint32_t(value));
}

static bool IsSignaled(uint32_t object, uint32_t thread)
{
    if (g_base[object + HEADER_TYPE] == Mutant)
        return SignalState(object) > 0 || LoadBE32(object + MUTANT_OWNER) == thread;
    return SignalState(object) > 0;
}

static void Acquire(uint32_t object, uint32_t thread)
{
    switch (g_base[object + HEADER_TYPE])
    {
    case SynchronizationEvent:
    case SynchronizationTimer:
        SetSignalState(object, 0);
        break;
    case Semaphore:
        SetSignalState(object, SignalState(object) - 1);
        break;
    case Mutant:
        StoreBE32(object + MUTANT_OWNER, thread);
        SetSignalState(object, SignalState(object) - 1);
        break;
    default:
        break;
    }
}

// Timeout: null = infinite, negative = relative, positive = absolute (100 ns units)
static std::chrono::steady_clock::time_point TimeoutDeadline(uint32_t timeoutPtr, bool& infinite)
{
    infinite = timeoutPtr == 0;
    if (infinite)
        return {};

    int64_t value = int64_t(__builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(timeoutPtr))));
    auto now = std::chrono::steady_clock::now();
    if (value <= 0)
        return now + std::chrono::nanoseconds(-value * 100);

    // Absolute FILETIME: convert against the system clock
    int64_t nowFiletime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
    return now + std::chrono::nanoseconds(std::max<int64_t>(0, value - nowFiletime) * 100);
}

static uint32_t WaitForObjects(const uint32_t* objects, uint32_t count, bool waitAll, uint32_t timeoutPtr)
{
    bool infinite;
    auto deadline = TimeoutDeadline(timeoutPtr, infinite);
    uint32_t thread = CurrentKThread();
    auto waitStart = std::chrono::steady_clock::now();
    bool reported = false;

    std::unique_lock lock(g_dispatcherMutex);
    while (true)
    {
        if (waitAll)
        {
            bool all = true;
            for (uint32_t i = 0; i < count && all; i++)
                all = IsSignaled(objects[i], thread);
            if (all)
            {
                for (uint32_t i = 0; i < count; i++)
                    Acquire(objects[i], thread);
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (IsSignaled(objects[i], thread))
                {
                    Acquire(objects[i], thread);
                    return STATUS_SUCCESS + i;
                }
            }
        }

        if (infinite)
        {
            // SIMPSONS_TRACE_BLOCKS: report waits blocked for more than 3 seconds
            static const bool traceBlocks = getenv("SIMPSONS_TRACE_BLOCKS") != nullptr;
            if (traceBlocks && !reported && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(3))
            {
                reported = true;
                fprintf(stderr, "[debug] thread %u bloque depuis 3 s sur", t_currentThread ? t_currentThread->threadId : 0);
                for (uint32_t i = 0; i < count; i++)
                {
                    fprintf(stderr, " 0x%X(type %u, etat %d)", objects[i], g_base[objects[i]], SignalState(objects[i]));
                    if (g_base[objects[i]] == Mutant)
                    {
                        uint32_t owner = LoadBE32(objects[i] + MUTANT_OWNER);
                        fprintf(stderr, " proprietaire KTHREAD 0x%X (thread %u)", owner, owner ? LoadBE32(owner + KTHREAD_THREAD_ID) : 0);
                    }
                }
                fprintf(stderr, " lr=0x%llX\n", t_currentThread ? (unsigned long long)GuestLR(t_currentThread->ctx) : 0ull);
            }
            DispatcherWaiter waiter;
            g_dispatcherCondition.wait_for(lock, std::chrono::seconds(1));
        }
        else
        {
            DispatcherWaiter waiter;
            if (g_dispatcherCondition.wait_until(lock, deadline) == std::cv_status::timeout)
                return STATUS_TIMEOUT;
        }
    }
}

static void NotifyWaiters()
{
    if (g_dispatcherWaiters != 0)
        g_dispatcherCondition.notify_all();
}

// ---------------------------------------------------------------------------
// Threads

struct ThreadExit
{
    uint32_t exitCode;
};

static void RunGuestThread(GuestThread* thread, uint32_t startup, uint32_t start, uint32_t context)
{
    t_currentThread = thread;
    SetGameThreadPriority();
    {
        std::unique_lock lock(g_dispatcherMutex);
        DispatcherWaiter waiter;
        g_dispatcherCondition.wait(lock, [&] { return thread->suspendCount <= 0; });
    }

    auto& ctx = thread->ctx;
    PPCFunc* function;
    if (startup != 0)
    {
        ctx.r3.u64 = start;
        ctx.r4.u64 = context;
        function = PPC_LOOKUP_FUNC(g_base, startup);
    }
    else
    {
        ctx.r3.u64 = context;
        function = PPC_LOOKUP_FUNC(g_base, start);
    }

    uint32_t exitCode = 0;
    try
    {
        function(ctx, g_base);
        exitCode = ctx.r3.u32;
    }
    catch (const ThreadExit& exit)
    {
        exitCode = exit.exitCode;
    }

    fprintf(stderr, "[runtime] thread %u termine (code 0x%X)\n", thread->threadId, exitCode);
    std::lock_guard lock(g_dispatcherMutex);
    SetSignalState(thread->kthread, 1);
    NotifyWaiters();
}

static GuestThread* RegisterThread(uint32_t stackSize)
{
    auto* thread = new GuestThread();
    thread->kthread = InitGuestThreadContext(thread->ctx, stackSize);
    thread->threadId = g_nextThreadId++;
    g_base[thread->kthread + HEADER_TYPE] = Thread;
    StoreBE32(thread->kthread + KTHREAD_THREAD_ID, thread->threadId);
    thread->handle = CreateHandle(thread->kthread);

    std::lock_guard lock(g_handleMutex);
    g_threadsByObject[thread->kthread] = thread;
    return thread;
}

static GuestThread* ThreadFromObject(uint32_t object)
{
    std::lock_guard lock(g_handleMutex);
    auto it = g_threadsByObject.find(object);
    return it != g_threadsByObject.end() ? it->second : nullptr;
}

// Each guest thread runs on one of the six hardware threads; code indexes per-CPU data with
// the CPU number stored in its KPCR (offset 0x10C)
static void SetThreadProcessor(GuestThread* thread, uint32_t affinity)
{
    if (thread != nullptr && affinity != 0)
        g_base[thread->ctx.r13.u32 + 0x10C] = uint8_t(__builtin_ctz(affinity) % 6);
}

void SetGameThreadPriority()
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

PPCContext& InitMainGuestThread(uint32_t stackSize)
{
    SetGameThreadPriority();
    t_currentThread = RegisterThread(stackSize);
    return t_currentThread->ctx;
}

KERNEL_FUNC(ExCreateThread)
{
    // (HANDLE* Handle, DWORD StackSize, DWORD* ThreadId, VOID* XapiThreadStartup, VOID* StartAddress, VOID* StartContext, DWORD CreationFlags)
    LOG_CALL(ExCreateThread);
    uint32_t handlePtr = ctx.r3.u32;
    uint32_t stackSize = std::max<uint32_t>(ctx.r4.u32, 0x10000);
    uint32_t threadIdPtr = ctx.r5.u32;
    uint32_t startup = ctx.r6.u32;
    uint32_t start = ctx.r7.u32;
    uint32_t context = ctx.r8.u32;
    uint32_t flags = ctx.r9.u32;

    auto* thread = RegisterThread(stackSize);
    thread->suspendCount = (flags & 0x1) ? 1 : 0;
    SetThreadProcessor(thread, flags >> 24);

    if (handlePtr != 0)
        StoreBE32(handlePtr, thread->handle);
    if (threadIdPtr != 0)
        StoreBE32(threadIdPtr, thread->threadId);

    fprintf(stderr, "[runtime] thread %u cree : entree 0x%X (startup 0x%X, contexte 0x%X)%s\n",
        thread->threadId, start, startup, context, thread->suspendCount ? " suspendu" : "");

    std::thread(RunGuestThread, thread, startup, start, context).detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(ExTerminateThread)
{
    LOG_CALL(ExTerminateThread);
    throw ThreadExit{ ctx.r3.u32 };
}


static uint32_t ResumeThread(GuestThread* thread)
{
    std::lock_guard lock(g_dispatcherMutex);
    uint32_t previous = uint32_t(std::max(thread->suspendCount, 0));
    if (thread->suspendCount > 0)
        thread->suspendCount--;
    NotifyWaiters();
    return previous;
}

KERNEL_FUNC(KeResumeThread)
{
    LOG_CALL(KeResumeThread);
    auto* thread = ThreadFromObject(ctx.r3.u32);
    ctx.r3.u64 = thread ? ResumeThread(thread) : 0;
}

KERNEL_FUNC(NtResumeThread)
{
    // (HANDLE Thread, ULONG* PreviousSuspendCount)
    LOG_CALL(NtResumeThread);
    auto* thread = ThreadFromObject(ResolveObject(ctx.r3.u32));
    if (thread == nullptr)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    uint32_t previous = ResumeThread(thread);
    if (ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, previous);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtSuspendThread)
{
    // Suspending a running thread is not supported; count it so resume stays balanced
    LOG_CALL(NtSuspendThread);
    auto* thread = ThreadFromObject(ResolveObject(ctx.r3.u32));
    if (thread != nullptr && ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, uint32_t(thread->suspendCount));
    ctx.r3.u64 = thread ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

KERNEL_FUNC(KeSetBasePriorityThread)
{
    LOG_CALL(KeSetBasePriorityThread);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(KeQueryBasePriorityThread)
{
    LOG_CALL(KeQueryBasePriorityThread);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(KeSetAffinityThread)
{
    // (KTHREAD* Thread, ULONG Affinity, ULONG* PreviousAffinity)
    LOG_CALL(KeSetAffinityThread);
    auto* thread = ThreadFromObject(ctx.r3.u32);
    if (ctx.r5.u32 != 0)
        StoreBE32(ctx.r5.u32, thread ? 1u << g_base[thread->ctx.r13.u32 + 0x10C] : 0x3F);
    SetThreadProcessor(thread, ctx.r4.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(KeSetDisableBoostThread)
{
    LOG_CALL(KeSetDisableBoostThread);
    ctx.r3.u64 = 0;
}

KERNEL_FUNC(ObReferenceObjectByHandle)
{
    // (HANDLE Handle, VOID* ObjectType, VOID** Object)
    LOG_CALL(ObReferenceObjectByHandle);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (ctx.r5.u32 != 0)
        StoreBE32(ctx.r5.u32, object);
    ctx.r3.u64 = object ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

KERNEL_FUNC(ObDereferenceObject)
{
    LOG_CALL(ObDereferenceObject);
}

KERNEL_FUNC(ObLookupThreadByThreadId)
{
    // (DWORD ThreadId, KTHREAD** Thread)
    LOG_CALL(ObLookupThreadByThreadId);
    std::lock_guard lock(g_handleMutex);
    for (auto& [object, thread] : g_threadsByObject)
    {
        if (thread->threadId == ctx.r3.u32)
        {
            StoreBE32(ctx.r4.u32, object);
            ctx.r3.u64 = STATUS_SUCCESS;
            return;
        }
    }
    ctx.r3.u64 = STATUS_INVALID_HANDLE;
}

KERNEL_FUNC(KeDelayExecutionThread)
{
    // (MODE WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Interval)
    LOG_CALL(KeDelayExecutionThread);
    static std::atomic<uint32_t> delays{ 0 };
    static const bool traceDelays = getenv("SIMPSONS_TRACE_WAITS") != nullptr;
    if (traceDelays && (delays++ % 1000000) == 0)
        fprintf(stderr, "[debug] pause %lld (lr=0x%llX, pile 0x%X 0x%X 0x%X, thread %u)\n",
            ctx.r5.u32 ? (long long)int64_t(__builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(ctx.r5.u32)))) : -1LL,
            (unsigned long long)GuestLR(ctx), LoadBE32(ctx.r1.u32 + 104), LoadBE32(ctx.r1.u32 + 112 + 0), LoadBE32(ctx.r1.u32 + 112 - 4),
            t_currentThread ? t_currentThread->threadId : 0);
    bool infinite;
    auto deadline = TimeoutDeadline(ctx.r5.u32, infinite);
    if (infinite)
        std::this_thread::sleep_for(std::chrono::hours(24));
    else if (deadline <= std::chrono::steady_clock::now())
    {
        // Sleep(0) in a loop (the game's frame limiter waits like this): after a series of consecutive calls,
        // sleep 100 us instead of yielding, so that the host core is not kept busy
        thread_local uint32_t consecutive = 0;
        thread_local auto lastCall = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        consecutive = now - lastCall < std::chrono::microseconds(200) ? consecutive + 1 : 0;
        lastCall = now;
        if (consecutive >= 32)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        else
            std::this_thread::yield();
    }
    else
        std::this_thread::sleep_until(deadline);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtYieldExecution)
{
    LOG_CALL(NtYieldExecution);
    std::this_thread::yield();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Events

KERNEL_FUNC(NtCreateEvent)
{
    // (HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, EVENT_TYPE Type, BOOLEAN InitialState)
    LOG_CALL(NtCreateEvent);
    auto type = ctx.r5.u32 == 0 ? NotificationEvent : SynchronizationEvent;
    uint32_t object = AllocDispatcherObject(type, ctx.r6.u32 ? 1 : 0);
    StoreBE32(ctx.r3.u32, CreateHandle(object));
    ctx.r3.u64 = STATUS_SUCCESS;
}

static int32_t SetEvent(uint32_t object)
{
    std::lock_guard lock(g_dispatcherMutex);
    int32_t previous = SignalState(object);
    SetSignalState(object, 1);
    NotifyWaiters();
    return previous;
}

static int32_t ResetEvent(uint32_t object)
{
    std::lock_guard lock(g_dispatcherMutex);
    int32_t previous = SignalState(object);
    SetSignalState(object, 0);
    return previous;
}

// Waitable events for other runtime parts (XAM notification listeners)
uint32_t RuntimeCreateEvent(bool manualReset, bool signaled)
{
    uint32_t object = AllocDispatcherObject(manualReset ? NotificationEvent : SynchronizationEvent, signaled ? 1 : 0);
    return CreateHandle(object);
}

void RuntimeSetEvent(uint32_t handle, bool signaled)
{
    uint32_t object = ResolveObject(handle);
    if (object == 0)
        return;
    if (signaled)
        SetEvent(object);
    else
        ResetEvent(object);
}

static int32_t PulseEvent(uint32_t object)
{
    // Wake current waiters: signal, let them run, then reset
    int32_t previous = SetEvent(object);
    std::this_thread::yield();
    ResetEvent(object);
    return previous;
}

void SignalEventHandle(uint32_t handle)
{
    if (uint32_t object = ResolveObject(handle))
        SetEvent(object);
}

bool CloseObjectHandle(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    return g_handleObjects.erase(handle) != 0;
}

KERNEL_FUNC(NtSetEvent)
{
    LOG_CALL(NtSetEvent);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = SetEvent(object);
    if (ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtClearEvent)
{
    LOG_CALL(NtClearEvent);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object != 0)
        ResetEvent(object);
    ctx.r3.u64 = object ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

KERNEL_FUNC(NtPulseEvent)
{
    LOG_CALL(NtPulseEvent);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous = PulseEvent(object);
    if (ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(KeSetEvent)
{
    // (KEVENT* Event, KPRIORITY Increment, BOOLEAN Wait)
    LOG_CALL(KeSetEvent);
    ctx.r3.u64 = uint32_t(SetEvent(ctx.r3.u32));
}

KERNEL_FUNC(KeResetEvent)
{
    LOG_CALL(KeResetEvent);
    ctx.r3.u64 = uint32_t(ResetEvent(ctx.r3.u32));
}

KERNEL_FUNC(KePulseEvent)
{
    LOG_CALL(KePulseEvent);
    ctx.r3.u64 = uint32_t(PulseEvent(ctx.r3.u32));
}

// ---------------------------------------------------------------------------
// Semaphores

static uint32_t ReleaseSemaphore(uint32_t object, int32_t count, int32_t& previous)
{
    std::lock_guard lock(g_dispatcherMutex);
    previous = SignalState(object);
    int32_t limit = int32_t(LoadBE32(object + SEMAPHORE_LIMIT));
    if (int64_t(previous) + count > limit)
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    SetSignalState(object, previous + count);
    NotifyWaiters();
    return STATUS_SUCCESS;
}

KERNEL_FUNC(NtCreateSemaphore)
{
    // (HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, LONG InitialCount, LONG MaximumCount)
    LOG_CALL(NtCreateSemaphore);
    uint32_t object = AllocDispatcherObject(Semaphore, ctx.r5.s32);
    StoreBE32(object + SEMAPHORE_LIMIT, ctx.r6.u32);
    StoreBE32(ctx.r3.u32, CreateHandle(object));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(KeInitializeSemaphore)
{
    // (KSEMAPHORE* Semaphore, LONG Count, LONG Limit)
    LOG_CALL(KeInitializeSemaphore);
    uint32_t object = ctx.r3.u32;
    memset(GuestToHost(object), 0, 0x14);
    g_base[object + HEADER_TYPE] = Semaphore;
    g_base[object + 2] = 0x14 / 4;
    StoreBE32(object + HEADER_SIGNAL_STATE, ctx.r4.u32);
    StoreBE32(object + SEMAPHORE_LIMIT, ctx.r5.u32);
}

KERNEL_FUNC(NtReleaseSemaphore)
{
    // (HANDLE Semaphore, LONG ReleaseCount, LONG* PreviousCount)
    LOG_CALL(NtReleaseSemaphore);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    int32_t previous;
    uint32_t status = ReleaseSemaphore(object, ctx.r4.s32, previous);
    if (ctx.r5.u32 != 0)
        StoreBE32(ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = status;
}

KERNEL_FUNC(KeReleaseSemaphore)
{
    // (KSEMAPHORE* Semaphore, KPRIORITY Increment, LONG Adjustment, BOOLEAN Wait)
    LOG_CALL(KeReleaseSemaphore);
    int32_t previous;
    ReleaseSemaphore(ctx.r3.u32, ctx.r5.s32, previous);
    ctx.r3.u64 = uint32_t(previous);
}

// ---------------------------------------------------------------------------
// Mutants

KERNEL_FUNC(NtCreateMutant)
{
    // (HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, BOOLEAN InitialOwner)
    LOG_CALL(NtCreateMutant);
    bool owned = ctx.r5.u32 != 0;
    uint32_t object = AllocDispatcherObject(Mutant, owned ? 0 : 1);
    if (owned)
        StoreBE32(object + MUTANT_OWNER, CurrentKThread());
    StoreBE32(ctx.r3.u32, CreateHandle(object));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtReleaseMutant)
{
    // (HANDLE Mutant, LONG* PreviousCount)
    LOG_CALL(NtReleaseMutant);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    std::lock_guard lock(g_dispatcherMutex);
    if (LoadBE32(object + MUTANT_OWNER) != CurrentKThread())
    {
        ctx.r3.u64 = STATUS_MUTANT_NOT_OWNED;
        return;
    }
    int32_t previous = SignalState(object);
    SetSignalState(object, previous + 1);
    if (previous + 1 > 0)
        StoreBE32(object + MUTANT_OWNER, 0);
    if (ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, uint32_t(previous));
    NotifyWaiters();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Waits

KERNEL_FUNC(KeWaitForSingleObject)
{
    // (VOID* Object, KWAIT_REASON Reason, MODE WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
    LOG_CALL(KeWaitForSingleObject);
    uint32_t object = ctx.r3.u32;
    ctx.r3.u64 = WaitForObjects(&object, 1, true, ctx.r7.u32);
}

KERNEL_FUNC(NtWaitForSingleObjectEx)
{
    // (HANDLE Handle, MODE WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
    LOG_CALL(NtWaitForSingleObjectEx);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        static std::atomic<uint32_t> invalid{ 0 };
        if (getenv("SIMPSONS_TRACE_WAITS") && (invalid++ % 1000000) == 0)
            fprintf(stderr, "[debug] attente sur poignee invalide 0x%X (lr=0x%llX, appelant 0x%X, thread %u)\n", ctx.r3.u32,
                (unsigned long long)GuestLR(ctx), GuestLR(ctx) == 0x82434FC8 ? LoadBE32(ctx.r1.u32 + 128 - 8) : 0,
                t_currentThread ? t_currentThread->threadId : 0);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    ctx.r3.u64 = WaitForObjects(&object, 1, true, ctx.r6.u32);

    static std::atomic<uint32_t> samples{ 0 };
    static const bool traceWaits = getenv("SIMPSONS_TRACE_WAITS") != nullptr;
    if (traceWaits && (samples++ % 200000) == 0)
    {
        int64_t timeout = ctx.r6.u32 ? int64_t(__builtin_bswap64(*static_cast<uint64_t*>(GuestToHost(ctx.r6.u32)))) : -1;
        uint32_t handle = ctx.r3.u32;
        fprintf(stderr, "[debug] attente objet 0x%X type %u etat %d delai %lld -> 0x%llX (lr=0x%llX, appelant 0x%X, thread %u)\n",
            object, g_base[object], SignalState(object), (long long)timeout, (unsigned long long)ctx.r3.u64,
            (unsigned long long)GuestLR(ctx), GuestLR(ctx) == 0x82434FC8 ? LoadBE32(ctx.r1.u32 + 128 - 8) : 0,
            t_currentThread ? t_currentThread->threadId : 0);
        (void)handle;
    }
}

KERNEL_FUNC(KeWaitForMultipleObjects)
{
    // (ULONG Count, VOID** Objects, WAIT_TYPE WaitType, KWAIT_REASON Reason, MODE WaitMode,
    //  BOOLEAN Alertable, LARGE_INTEGER* Timeout, KWAIT_BLOCK* WaitBlocks)
    LOG_CALL(KeWaitForMultipleObjects);
    uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64);
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; i++)
        objects[i] = LoadBE32(ctx.r4.u32 + i * 4);
    ctx.r3.u64 = WaitForObjects(objects, count, ctx.r5.u32 == 0, ctx.r9.u32);
}

KERNEL_FUNC(NtWaitForMultipleObjectsEx)
{
    // (ULONG Count, HANDLE* Handles, WAIT_TYPE WaitType, MODE WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
    LOG_CALL(NtWaitForMultipleObjectsEx);
    uint32_t count = std::min<uint32_t>(ctx.r3.u32, 64);
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; i++)
    {
        objects[i] = ResolveObject(LoadBE32(ctx.r4.u32 + i * 4));
        if (objects[i] == 0)
        {
            ctx.r3.u64 = STATUS_INVALID_HANDLE;
            return;
        }
    }
    ctx.r3.u64 = WaitForObjects(objects, count, ctx.r5.u32 == 0, ctx.r8.u32);
}

KERNEL_FUNC(NtSignalAndWaitForSingleObjectEx)
{
    // (HANDLE Signal, HANDLE Wait, MODE WaitMode, BOOLEAN Alertable, LARGE_INTEGER* Timeout)
    LOG_CALL(NtSignalAndWaitForSingleObjectEx);
    uint32_t signal = ResolveObject(ctx.r3.u32);
    uint32_t wait = ResolveObject(ctx.r4.u32);
    if (signal == 0 || wait == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }

    switch (g_base[signal + HEADER_TYPE])
    {
    case Semaphore:
    {
        int32_t previous;
        ReleaseSemaphore(signal, 1, previous);
        break;
    }
    default:
        SetEvent(signal);
        break;
    }
    ctx.r3.u64 = WaitForObjects(&wait, 1, true, ctx.r7.u32);
}

// ---------------------------------------------------------------------------
// Critical sections

static void InitializeCriticalSection(uint32_t cs)
{
    memset(GuestToHost(cs), 0, 0x1C);
    g_base[cs + HEADER_TYPE] = SynchronizationEvent;
    g_base[cs + 2] = 0x1C / 4;
    StoreBE32(cs + CS_LOCK_COUNT, 0xFFFFFFFF);
}

KERNEL_FUNC(RtlInitializeCriticalSection)
{
    LOG_CALL(RtlInitializeCriticalSection);
    InitializeCriticalSection(ctx.r3.u32);
}

KERNEL_FUNC(RtlInitializeCriticalSectionAndSpinCount)
{
    LOG_CALL(RtlInitializeCriticalSectionAndSpinCount);
    InitializeCriticalSection(ctx.r3.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

static void EnterCriticalSection(uint32_t cs, uint32_t thread)
{
    std::unique_lock lock(g_dispatcherMutex);
    DispatcherWaiter waiter;
    g_dispatcherCondition.wait(lock, [&]
        {
            uint32_t owner = LoadBE32(cs + CS_OWNING_THREAD);
            return owner == 0 || owner == thread;
        });
    StoreBE32(cs + CS_OWNING_THREAD, thread);
    StoreBE32(cs + CS_RECURSION_COUNT, LoadBE32(cs + CS_RECURSION_COUNT) + 1);
    StoreBE32(cs + CS_LOCK_COUNT, LoadBE32(cs + CS_LOCK_COUNT) + 1);
}

KERNEL_FUNC(RtlEnterCriticalSection)
{
    LOG_CALL(RtlEnterCriticalSection);
    EnterCriticalSection(ctx.r3.u32, CurrentKThread());
}

KERNEL_FUNC(RtlTryEnterCriticalSection)
{
    LOG_CALL(RtlTryEnterCriticalSection);
    uint32_t cs = ctx.r3.u32;
    uint32_t thread = CurrentKThread();
    std::lock_guard lock(g_dispatcherMutex);
    uint32_t owner = LoadBE32(cs + CS_OWNING_THREAD);
    if (owner != 0 && owner != thread)
    {
        ctx.r3.u64 = 0;
        return;
    }
    StoreBE32(cs + CS_OWNING_THREAD, thread);
    StoreBE32(cs + CS_RECURSION_COUNT, LoadBE32(cs + CS_RECURSION_COUNT) + 1);
    StoreBE32(cs + CS_LOCK_COUNT, LoadBE32(cs + CS_LOCK_COUNT) + 1);
    ctx.r3.u64 = 1;
}

KERNEL_FUNC(RtlLeaveCriticalSection)
{
    LOG_CALL(RtlLeaveCriticalSection);
    uint32_t cs = ctx.r3.u32;
    std::lock_guard lock(g_dispatcherMutex);
    uint32_t recursion = LoadBE32(cs + CS_RECURSION_COUNT);
    if (recursion == 0)
        return;
    StoreBE32(cs + CS_RECURSION_COUNT, recursion - 1);
    StoreBE32(cs + CS_LOCK_COUNT, LoadBE32(cs + CS_LOCK_COUNT) - 1);
    if (recursion == 1)
    {
        StoreBE32(cs + CS_OWNING_THREAD, 0);
        NotifyWaiters();
    }
}

// ---------------------------------------------------------------------------
// Spin locks and IRQL (the guest lock word holds the owner KTHREAD)

// ---------------------------------------------------------------------------
// IRQL: a thread at DISPATCH_LEVEL (KeRaiseIrqlToDpcLevel, KfAcquireSpinLock) cannot be interrupted on its CPU. The
// game relies on it to update structures shared with the graphics interrupt callback: interrupt delivery for a CPU
// takes that CPU's gate exclusively, threads raised on it hold it shared.
static std::shared_mutex g_interruptGates[6];
static thread_local uint8_t t_irql = 0;
static thread_local int t_gateCpu = -1;

static uint8_t RaiseIrqlToDpc(PPCContext& ctx)
{
    uint8_t previous = t_irql;
    if (previous < 2)
    {
        int cpu = g_base[ctx.r13.u32 + 0x10C] % 6;
        g_interruptGates[cpu].lock_shared();
        t_gateCpu = cpu;
        t_irql = 2;
    }
    return previous;
}

static void LowerIrql(uint8_t irql)
{
    if (t_irql >= 2 && irql < 2 && t_gateCpu >= 0)
    {
        g_interruptGates[t_gateCpu].unlock_shared();
        t_gateCpu = -1;
    }
    if (t_irql < 3) // interrupt callbacks keep their level
        t_irql = irql;
}

// Called by the interrupt thread around a callback for a CPU
void InterruptEnter(uint32_t cpu)
{
    g_interruptGates[cpu % 6].lock();
    t_irql = 3;
}

void InterruptLeave(uint32_t cpu)
{
    t_irql = 0;
    g_interruptGates[cpu % 6].unlock();
}

static void AcquireSpinLock(uint32_t lockAddress)
{
    auto* lock = static_cast<uint32_t*>(GuestToHost(lockAddress));
    uint32_t owner = __builtin_bswap32(std::max<uint32_t>(CurrentKThread(), 1));
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(lock, &expected, owner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        expected = 0;
        std::this_thread::yield();
    }
}

static void ReleaseSpinLock(uint32_t lockAddress)
{
    __atomic_store_n(static_cast<uint32_t*>(GuestToHost(lockAddress)), 0, __ATOMIC_RELEASE);
}

KERNEL_FUNC(KfAcquireSpinLock)
{
    LOG_CALL(KfAcquireSpinLock);
    uint8_t previous = RaiseIrqlToDpc(ctx);
    AcquireSpinLock(ctx.r3.u32);
    ctx.r3.u64 = previous;
}

KERNEL_FUNC(KfReleaseSpinLock)
{
    LOG_CALL(KfReleaseSpinLock);
    ReleaseSpinLock(ctx.r3.u32);
    LowerIrql(uint8_t(ctx.r4.u32)); // (lock, previous IRQL)
}

KERNEL_FUNC(KeAcquireSpinLockAtRaisedIrql)
{
    LOG_CALL(KeAcquireSpinLockAtRaisedIrql);
    AcquireSpinLock(ctx.r3.u32);
}

KERNEL_FUNC(KeReleaseSpinLockFromRaisedIrql)
{
    LOG_CALL(KeReleaseSpinLockFromRaisedIrql);
    ReleaseSpinLock(ctx.r3.u32);
}

KERNEL_FUNC(KeTryToAcquireSpinLockAtRaisedIrql)
{
    LOG_CALL(KeTryToAcquireSpinLockAtRaisedIrql);
    auto* lock = static_cast<uint32_t*>(GuestToHost(ctx.r3.u32));
    uint32_t expected = 0;
    uint32_t owner = __builtin_bswap32(std::max<uint32_t>(CurrentKThread(), 1));
    ctx.r3.u64 = __atomic_compare_exchange_n(lock, &expected, owner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

KERNEL_FUNC(KeRaiseIrqlToDpcLevel)
{
    LOG_CALL(KeRaiseIrqlToDpcLevel);
    ctx.r3.u64 = RaiseIrqlToDpc(ctx);
}

KERNEL_FUNC(KfLowerIrql)
{
    LOG_CALL(KfLowerIrql);
    LowerIrql(uint8_t(ctx.r3.u32));
}

KERNEL_FUNC(KeEnterCriticalRegion)
{
    LOG_CALL(KeEnterCriticalRegion);
}

KERNEL_FUNC(KeLeaveCriticalRegion)
{
    LOG_CALL(KeLeaveCriticalRegion);
}

KERNEL_FUNC(KeLockL2)
{
    LOG_CALL(KeLockL2);
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(KeUnlockL2)
{
    LOG_CALL(KeUnlockL2);
}

// ---------------------------------------------------------------------------
// Debugging: SIMPSONS_DUMP=<seconds> prints the main thread registers after a delay

const char* XmaDescribePhysical(uint32_t physical);

// SIMPSONS_SURVEILLE=<hex address>:<hex length>: a thread polls that guest range every 100 us and logs
// every dword change (finds who overwrites a structure; the XMA contexts are searched for the new value)
static void StartWatch()
{
    const char* spec = getenv("SIMPSONS_SURVEILLE");
    if (spec == nullptr)
        return;
    uint32_t address = uint32_t(strtoul(spec, nullptr, 16)) & ~3u;
    const char* colon = strchr(spec, ':');
    uint32_t length = colon ? uint32_t(strtoul(colon + 1, nullptr, 16)) : 0x40;
    std::thread([address, length]
        {
            std::vector<uint32_t> last(length / 4);
            for (uint32_t i = 0; i < length / 4; i++)
                last[i] = LoadBE32(address + i * 4);
            auto start = std::chrono::steady_clock::now();
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                for (uint32_t i = 0; i < length / 4; i++)
                {
                    uint32_t value = LoadBE32(address + i * 4);
                    if (value == last[i])
                        continue;
                    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    const char* xma = XmaDescribePhysical(value);
                    fprintf(stderr, "[surveille] %.4f s 0x%08X : %08X -> %08X%s%s\n", t, address + i * 4, last[i], value,
                        xma ? " = " : "", xma ? xma : "");
                    last[i] = value;
                }
            }
        }).detach();
}

void StartDebugDump()
{
    StartWatch();
    const char* delay = getenv("SIMPSONS_DUMP");
    if (delay == nullptr)
        return;
    GuestThread* main = t_currentThread;
    std::thread([main, seconds = atoi(delay)]
        {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            auto& c = main->ctx;
            fprintf(stderr, "[debug] thread principal lr=0x%llX r1=%08X r3=%08X r4=%08X r5=%08X r29=%08X r30=%08X r31=%08X\n",
                (unsigned long long)GuestLR(c), c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, GuestGPR(c, 29), GuestGPR(c, 30), GuestGPR(c, 31));
            if (const char* needle = getenv("SIMPSONS_FIND"))
            {
                // Search the heap and the physical memory for a byte string (Windows-1252 text)
                std::string text = needle;
                const std::pair<uint32_t, uint32_t> ranges[] = { { 0x40000000, 0x70000000 }, { 0xA0000000, 0xC0000000 } };
                int found = 0;
                for (auto [begin, end] : ranges)
                {
                    auto* start = g_base + begin;
                    auto* stop = g_base + end - text.size();
                    for (auto* p = start; p < stop && found < 8; p++)
                    {
                        p = static_cast<uint8_t*>(memchr(p, text[0], stop - p));
                        if (p == nullptr)
                            break;
                        if (memcmp(p, text.data(), text.size()) == 0)
                        {
                            fprintf(stderr, "[debug] \"%s\" trouve en 0x%08X\n", needle, uint32_t(p - g_base));
                            found++;
                        }
                    }
                }
                if (found == 0)
                    fprintf(stderr, "[debug] \"%s\" introuvable en memoire\n", needle);
            }
            if (const char* addresses = getenv("SIMPSONS_DUMP_MEM"))
            {
                // Comma separated guest addresses (hex); "r31+2A90" style offsets are relative to r31
                std::string list = addresses;
                size_t start = 0;
                while (start < list.size())
                {
                    size_t end = list.find(',', start);
                    std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    uint32_t address = item.rfind("r31+", 0) == 0 ? GuestGPR(c, 31) + uint32_t(strtoul(item.c_str() + 4, nullptr, 16))
                                                                   : uint32_t(strtoul(item.c_str(), nullptr, 16));
                    fprintf(stderr, "[debug] [%s] 0x%08X = 0x%08X\n", item.c_str(), address, LoadBE32(address));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
        }).detach();
}

// ---------------------------------------------------------------------------
// Timers: dispatcher objects signalled by a host scheduler thread

struct TimerState
{
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point due;
    std::chrono::milliseconds period{ 0 };
};

static std::unordered_map<uint32_t, TimerState> g_timers; // guest object -> state (g_dispatcherMutex)
static std::condition_variable g_timerCondition;
static bool g_timerThreadStarted = false;

static void TimerThread()
{
    std::unique_lock lock(g_dispatcherMutex);
    while (true)
    {
        auto now = std::chrono::steady_clock::now();
        auto next = now + std::chrono::hours(1);
        bool signalled = false;

        for (auto& [object, timer] : g_timers)
        {
            if (timer.generation == 0)
                continue;
            if (timer.due <= now)
            {
                SetSignalState(object, 1);
                signalled = true;
                if (timer.period.count() > 0)
                    timer.due += timer.period;
                else
                    timer.generation = 0;
            }
            if (timer.generation != 0)
                next = std::min(next, timer.due);
        }

        if (signalled)
            NotifyWaiters();
        g_timerCondition.wait_until(lock, next);
    }
}

KERNEL_FUNC(NtCreateTimer)
{
    // (HANDLE* Handle, OBJECT_ATTRIBUTES* Attributes, TIMER_TYPE Type)
    LOG_CALL(NtCreateTimer);
    auto type = ctx.r5.u32 == 0 ? NotificationTimer : SynchronizationTimer;
    uint32_t object = AllocDispatcherObject(type, 0, 0x28);
    {
        std::lock_guard lock(g_dispatcherMutex);
        g_timers[object] = {};
        if (!g_timerThreadStarted)
        {
            g_timerThreadStarted = true;
            std::thread(TimerThread).detach();
        }
    }
    StoreBE32(ctx.r3.u32, CreateHandle(object));
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtSetTimerEx)
{
    // (HANDLE Timer, LARGE_INTEGER* DueTime, VOID* ApcRoutine, MODE ApcMode, VOID* ApcContext,
    //  BOOLEAN Resume, LONG PeriodMs, BOOLEAN* PreviousState)
    LOG_CALL(NtSetTimerEx);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r5.u32 != 0)
        fprintf(stderr, "[runtime] NtSetTimerEx avec APC non gere\n");

    bool infinite;
    auto due = TimeoutDeadline(ctx.r4.u32, infinite);
    uint32_t periodMs = ctx.r9.u32;
    uint32_t previousPtr = ctx.r10.u32;

    std::lock_guard lock(g_dispatcherMutex);
    if (previousPtr != 0)
        StoreBE32(previousPtr, SignalState(object) > 0);
    SetSignalState(object, 0);
    auto& timer = g_timers[object];
    timer.generation++;
    timer.due = due;
    timer.period = std::chrono::milliseconds(periodMs);
    g_timerCondition.notify_all();
    ctx.r3.u64 = STATUS_SUCCESS;
}

KERNEL_FUNC(NtCancelTimer)
{
    // (HANDLE Timer, BOOLEAN* CurrentState)
    LOG_CALL(NtCancelTimer);
    uint32_t object = ResolveObject(ctx.r3.u32);
    if (object == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    std::lock_guard lock(g_dispatcherMutex);
    if (ctx.r4.u32 != 0)
        StoreBE32(ctx.r4.u32, SignalState(object) > 0);
    g_timers[object].generation = 0;
    ctx.r3.u64 = STATUS_SUCCESS;
}
