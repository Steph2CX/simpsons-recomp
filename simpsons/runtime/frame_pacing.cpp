// Frame pacing: 60 images/s in levels, 30 in menus, other rates (SIMPSONS_IPS) with the game clock kept on real time.
//
// The game shipped at 30 images/s: its frame scheduler (the object at 0x82D576A0) waits <+144> vblank periods per
// frame, 2 as shipped. Levels run at 1 (60 images/s): their logic runs on measured time and the Havok step is evened
// out (game_fixes.cpp). Menus, the title screen, loading screens and the pause menu run their logic once per frame
// (held directions repeat every few frames, screens wait a few frames): they stay at 30. A level runs while the Havok
// world was stepped in the last 750 ms.
//
// Rates other than 60 and its whole fractions (SIMPSONS_IPS=90, 120, 144, 0 = unlimited) need more: the game clock
// counts frames in whole 59.94 Hz vblanks, at least one (above 60 images/s the game ran faster than real time), the
// frame limiter (sub_826B7B70) waits whole vblank periods, and swaps wait for the next 60 Hz vblank. For those rates
// frames are counted in fractional vblanks measured on the guest time base, the limiter waits for the target rate's
// period, the scheduler's deadline for background work (loading) moves with it, never before the game's own frame
// period, and swaps are released at once. The Havok step stays 1/59.94 s (game_fixes.cpp).
//
// Reverse engineering from github.com/YesterMester/TheSimpsonsGameRecomp (frame_pacing.cpp, game_clock.cpp): same
// game image, same addresses.
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

PPC_FUNC_IMPL(__imp__sub_82718710); // scheduler: time until the frame's deadline (r3 = scheduler)
PPC_FUNC_IMPL(__imp__sub_826B7B70); // frame limiter: waits r4 vblank periods after r3 (start time base, low 32 bits)
PPC_FUNC_IMPL(__imp__sub_82690D60); // game clock: measures the frame (r3 = clock, +12 = previous tick count)
PPC_EXTERN_FUNC(sub_8270CE10);      // the game's millisecond tick count (GetTickCount)
PPC_FUNC_IMPL(__imp__sub_82453EB0); // swap callback: r3 = front buffer | present interval << 8 | immediate threshold

extern std::atomic<int64_t> g_lastPhysicsStepNs; // game_fixes.cpp

namespace
{
    constexpr uint64_t TIMEBASE_HZ = 49875000;
    constexpr uint32_t SCHEDULER_PERIODS = 144;      // vblank periods per frame
    constexpr uint32_t SCHEDULER_START = 136;        // time base at the start of the frame (low 32 bits)
    constexpr uint32_t SCHEDULER_REFRESH_HZ = 0x82CF0404; // float the scheduler divides the time base by
    constexpr int64_t MENU_AFTER_NS = 750'000'000;   // without a physics step for this long, no level runs

    // Game clock (sub_82690D60)
    constexpr uint32_t CLOCK_RUNNING = 0x82CED760;
    constexpr uint32_t CLOCK_PAUSED = 0x82D572DC;
    constexpr uint32_t CLOCK_CARRY = 0x82D572AC;
    constexpr uint32_t MS_PER_VBLANK = 0x82D6CA9C;
    constexpr uint32_t MAX_VBLANKS = 0x82CED740;
    constexpr uint32_t TOTAL_VBLANKS = 0x82D572A8;
    constexpr uint32_t RAW_FRAME_DT = 0x82CED744;
    constexpr uint32_t TOTAL_MS = 0x82D572B0;
    constexpr uint32_t TOTAL_SECONDS = 0x82D572B4;
    constexpr uint32_t REFRESH_HZ = 0x82CF0394;
    constexpr uint32_t TIME_SCALE = 0x82CED750;
    constexpr uint32_t GAME_TIME_US = 0x82D572C8;
    constexpr uint32_t GAME_TIME_MS = 0x82D572BC;
    constexpr uint32_t GAME_TIME_SECONDS = 0x82D572C0;
    constexpr uint32_t FRAME_MS = 0x82CED74C;
    constexpr uint32_t FRAME_DT = 0x82CED748;
    constexpr uint32_t FRAME_COUNT = 0x82D572B8;
    constexpr float MIN_VBLANKS = 0.05f; // a frame counts at least this many vblanks (0.83 ms), never 0

    bool g_inMenu = true; // main thread (the scheduler and the clock run there)

    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // SIMPSONS_IPS=<rate> in levels (0 = unlimited), SIMPSONS_30IPS=1: 30 everywhere
    int LevelFrameRate()
    {
        static const int rate = getenv("SIMPSONS_30IPS") ? 30 : getenv("SIMPSONS_IPS") ? std::clamp(atoi(getenv("SIMPSONS_IPS")), 0, 1000) : 60;
        return rate;
    }

    // 30, or 60 with SIMPSONS_MENUS_60=1 (their logic then runs twice as fast), never above the level rate
    int MenuFrameRate()
    {
        static const int rate = getenv("SIMPSONS_MENUS_60") ? 60 : 30;
        int level = LevelFrameRate();
        return level > 0 && level < rate ? level : rate;
    }

    int ActiveFrameRate()
    {
        return g_inMenu ? MenuFrameRate() : LevelFrameRate();
    }

    // Rates the game's whole-vblank timing reaches by itself: 60 and its whole fractions
    bool WholeVblankRate(int rate)
    {
        return rate > 0 && rate <= 60 && 60 % rate == 0;
    }

    // Time base ticks per frame at the active rate, 0 when unlimited
    uint64_t FramePeriodTicks()
    {
        int rate = ActiveFrameRate();
        return rate > 0 ? TIMEBASE_HZ / uint64_t(rate) : 0;
    }

    float LoadBEFloat(uint32_t address)
    {
        uint32_t bits = LoadBE32(address);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    void StoreBEFloat(uint32_t address, float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        StoreBE32(address, bits);
    }

    uint64_t LoadBE64(uint32_t address)
    {
        return (uint64_t(LoadBE32(address)) << 32) | LoadBE32(address + 4);
    }

    void StoreBE64(uint32_t address, uint64_t value)
    {
        StoreBE32(address, uint32_t(value >> 32));
        StoreBE32(address + 4, uint32_t(value));
    }

    // fctidz then stfiwx: truncated, low 32 bits
    uint32_t Truncate32(double value)
    {
        return std::isnan(value) ? 0 : uint32_t(int64_t(std::clamp(value, -9.2e18, 9.2e18)));
    }
}

// Frames counted in fractional vblanks (rates other than 60 and its whole fractions)
bool FractionalVblanks()
{
    return !WholeVblankRate(ActiveFrameRate());
}

PPC_FUNC(sub_82718710)
{
    static uint32_t gamePeriods = 0, appliedPeriods = 0;
    static uint64_t ownPeriod = 0; // the game's own frame period (time base ticks)
    static int loggedRate = -1;
    static bool loggedMenu = false;

    uint32_t scheduler = ctx.r3.u32;
    if (scheduler != 0)
    {
        uint32_t address = scheduler + SCHEDULER_PERIODS;
        uint32_t current = LoadBE32(address);
        // Whatever the game itself set, unless it is still the value set here
        if (gamePeriods == 0 || current != appliedPeriods)
            gamePeriods = std::max(current, 1u);
        int64_t lastStep = g_lastPhysicsStepNs.load(std::memory_order_relaxed);
        g_inMenu = lastStep == 0 || NowNs() - lastStep > MENU_AFTER_NS;
        // 2 (30 images/s) as shipped: 1 is 60
        uint32_t base = gamePeriods == 2 ? 1 : gamePeriods;
        uint32_t refreshBits = LoadBE32(SCHEDULER_REFRESH_HZ);
        float refreshHz;
        memcpy(&refreshHz, &refreshBits, 4);
        if (!(refreshHz >= 20.0f && refreshHz <= 240.0f))
            refreshHz = 59.94f;
        ownPeriod = uint64_t(double(base) * TIMEBASE_HZ / refreshHz);
        int rate = ActiveFrameRate();
        uint32_t wanted = WholeVblankRate(rate) ? std::max(base, uint32_t(60 / rate)) : base;
        if (wanted != current)
            StoreBE32(address, wanted);
        appliedPeriods = wanted;
        if (rate != loggedRate || g_inMenu != loggedMenu)
        {
            loggedRate = rate;
            loggedMenu = g_inMenu;
            const char* state = g_inMenu ? "menus" : "niveau";
            if (WholeVblankRate(rate))
                fprintf(stderr, "[cadence] %.2f images/s (%s)\n", refreshHz / wanted, state);
            else if (rate > 0)
                fprintf(stderr, "[cadence] %d images/s (%s), images mesurees en fractions de vblank\n", rate, state);
            else
                fprintf(stderr, "[cadence] sans limite (%s), images mesurees en fractions de vblank\n", state);
        }
    }
    __imp__sub_82718710(ctx, base);
    if (scheduler != 0 && FractionalVblanks())
    {
        // The time left is what the scheduler's background tasks (loading, in slices after each frame) may use: until
        // the limiter's deadline, never less than the game's own frame period (with no limit, or a frame longer than
        // the limit, a loading screen would never end). Tasks with nothing to do return at once.
        uint64_t period = std::max(FramePeriodTicks(), ownPeriod);
        uint32_t elapsed = uint32_t(__rdtsc()) - LoadBE32(scheduler + SCHEDULER_START);
        ctx.r3.u64 = elapsed < period ? uint32_t(period - elapsed) : 0;
    }
}

PPC_FUNC(sub_826B7B70)
{
    if (!FractionalVblanks())
    {
        __imp__sub_826B7B70(ctx, base);
        return;
    }
    if (LoadBE32(CLOCK_RUNNING) == 0) // the shipped limiter returns at once too
        return;
    uint64_t period = FramePeriodTicks();
    if (period == 0)
        return;
    // Frames end on a grid of the target period, not a period after the frame started (the game marks the start a
    // little after the previous limiter returned). A frame late by more than a period starts the grid over.
    static uint64_t next = 0;
    uint64_t now = __rdtsc();
    if (next == 0 || now > next + period || next > now + 2 * period)
        next = now;
    while (now < next)
    {
        // Sleep while more than 2 ms remain, then yield: a coarse system timer cannot make the frame late
        double remaining = double(next - now) / TIMEBASE_HZ;
        if (remaining > 0.002)
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining - 0.002));
        else
            std::this_thread::yield();
        now = __rdtsc();
    }
    next += period;
}

// The game clock counts each frame in whole 59.94 Hz vblanks from the millisecond tick count:
//   n = trunc(ms / ms_per_vblank - carry + 0.5), clamped to 1..max_vblanks, carry = n - ms / ms_per_vblank.
// Everything a frame advances by comes from n. With fractional vblanks, a frame counts the vblanks it actually took,
// measured on the time base (clamped to 1/20..max_vblanks), and the whole-vblank total advances by the whole vblanks
// the fractions add up to. Every value is otherwise computed as the game computes it.
PPC_FUNC(sub_82690D60)
{
    static uint32_t lastClock = 0;
    static uint64_t lastTicks = 0;
    static double vblankFraction = 0.0;
    if (!FractionalVblanks())
    {
        lastClock = 0;
        __imp__sub_82690D60(ctx, base);
        return;
    }
    uint32_t clock = ctx.r3.u32;
    sub_8270CE10(ctx, base);
    uint32_t tickCount = ctx.r3.u32;
    uint64_t now = __rdtsc();
    if (LoadBE32(CLOCK_RUNNING) != 0)
    {
        float msPerVblank = LoadBEFloat(MS_PER_VBLANK);
        double seconds = lastClock == clock && now > lastTicks
            ? double(now - lastTicks) / TIMEBASE_HZ
            : double(tickCount - LoadBE32(clock + 12)) * 0.001; // first frame, or after whole vblanks
        float maxVblanks = float(std::max(LoadBE32(MAX_VBLANKS), 1u));
        float vblanks = std::clamp(float(seconds * 1000.0 / msPerVblank), MIN_VBLANKS, maxVblanks);
        StoreBEFloat(CLOCK_CARRY, 0.0f);
        vblankFraction += vblanks;
        uint32_t whole = uint32_t(vblankFraction);
        vblankFraction -= whole;
        uint32_t total = LoadBE32(TOTAL_VBLANKS) + whole;
        StoreBE32(TOTAL_VBLANKS, total);
        float rawDt = float(vblanks * msPerVblank) * 0.001f;
        StoreBEFloat(RAW_FRAME_DT, rawDt);
        StoreBE32(TOTAL_MS, Truncate32(std::fma(double(total), double(msPerVblank), 0.5)));
        StoreBEFloat(TOTAL_SECONDS, float(total) / LoadBEFloat(REFRESH_HZ));
        if (LoadBE32(CLOCK_PAUSED) != 1)
        {
            float scaledDt = LoadBEFloat(TIME_SCALE) * rawDt;
            uint64_t gameUs = LoadBE64(GAME_TIME_US) + uint64_t(int64_t(std::trunc(std::fma(scaledDt, 1000000.0f, 0.5f))));
            StoreBE64(GAME_TIME_US, gameUs);
            StoreBE32(GAME_TIME_MS, Truncate32(float(std::fma(float(double(gameUs)), 0.001f, 0.5f))));
            StoreBE32(FRAME_MS, Truncate32(float(std::fma(vblanks, msPerVblank, 0.5f))));
            StoreBEFloat(FRAME_DT, scaledDt);
            StoreBE32(FRAME_COUNT, LoadBE32(FRAME_COUNT) + 1);
            StoreBEFloat(GAME_TIME_SECONDS, float(double(gameUs) * 1e-6));
        }
    }
    lastClock = clock;
    lastTicks = now;
    StoreBE32(clock + 12, tickCount);
}

PPC_FUNC(sub_82453EB0)
{
    // Present interval 0 (bits 8-11): frames are paced by the limiter, not the 60 Hz vblank, so the swap is released
    // at once
    if (FractionalVblanks())
        ctx.r3.u64 = ctx.r3.u32 & ~uint32_t(0xF00);
    __imp__sub_82453EB0(ctx, base);
}
