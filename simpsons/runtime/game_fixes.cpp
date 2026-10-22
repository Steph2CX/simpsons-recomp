// Fixes to the game's own behaviour (hooks on guest functions).
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PPC_FUNC_IMPL(__imp__sub_828B3E30); // HUD: show an event message (r3 = HUD panel, r4 = string id, f1 = duration)
PPC_FUNC_IMPL(__imp__sub_828B2B70); // HUD: per-frame update of a player's thought bubble
PPC_EXTERN_FUNC(sub_8282AC20);      // string manager: find a string id in the loaded tables

namespace
{
    constexpr uint32_t STRING_MANAGER = 0x82D08B0C; // pointer to the game's string manager
    constexpr uint32_t PENDING_FRAMES = 600;        // give up after ~10 s

    // Message posted while its string table was not registered yet
    struct PendingMessage
    {
        uint32_t panel = 0, id = 0, frames = 0;
        double duration = 0;
    } g_pending;

    bool StringLoaded(PPCContext& ctx, uint8_t* base, uint32_t id)
    {
        PPCContext saved = ctx;
        ctx.r1.u32 -= 128;
        ctx.r3.u64 = LoadBE32(STRING_MANAGER);
        ctx.r4.u64 = id;
        ctx.r5.u64 = ctx.r1.u32 + 80; // string (unused)
        ctx.r6.u64 = ctx.r1.u32 + 84; // flag (unused)
        StoreBE32(ctx.r1.u32 + 80, 0);
        StoreBE32(ctx.r1.u32 + 84, 0);
        sub_8282AC20(ctx, base);
        bool found = (ctx.r3.u32 & 0xFF) != 0;
        ctx = saved;
        return found;
    }

    void ShowMessage(PPCContext& ctx, uint8_t* base, const PendingMessage& message)
    {
        PPCContext saved = ctx;
        ctx.r3.u64 = message.panel;
        ctx.r4.u64 = message.id;
        ctx.f1.f64 = message.duration;
        __imp__sub_828B3E30(ctx, base);
        ctx = saved;
    }
}

// On arriving in Springfield the game shows "You unlocked Bart! Press [LDLeft] to switch characters" before the
// level's string table is registered (the lookup only sees two small tables): the message cloud stayed on screen
// with an empty text. Such a message now waits until its string can be found.
PPC_FUNC(sub_828B3E30)
{
    if (LoadBE32(STRING_MANAGER) != 0 && !StringLoaded(ctx, base, ctx.r4.u32))
    {
        g_pending = { ctx.r3.u32, ctx.r4.u32, 0, ctx.f1.f64 };
        return;
    }
    g_pending.id = 0;
    __imp__sub_828B3E30(ctx, base);
}

PPC_FUNC(sub_828B2B70)
{
    __imp__sub_828B2B70(ctx, base);
    if (g_pending.id == 0)
        return;
    if (StringLoaded(ctx, base, g_pending.id) || ++g_pending.frames >= PENDING_FRAMES)
    {
        PendingMessage message = g_pending;
        g_pending.id = 0;
        ShowMessage(ctx, base, message);
    }
}

// Havok physics step at 60 images/s. sub_827A55C0 moves the world's time marker on by f1 (the game clock's frame
// time smoothed by sub_827A66B8), then: f1 <= T (0x82159238, 1/29.97 s as shipped) -> one step of T while the
// simulation is behind the marker; f1 < H (static from 0x82159240, 4/59.94 s) -> steps of f1 / 2; else steps of
// 1/59.94 s. At 60 images/s that is one 33.4 ms step every other frame: physics objects moved at 30 Hz. Now
// T = 1/59.94 s and the step gets the clock's own frame time (whole vblanks x time scale, 0x82CED748) instead of the
// smoothed copy, which settles a few ulps off it: one 16.7 ms step per vblank, at 60 and at 30 images/s (the
// console's usual step). While the time scale is 0 the game's own path runs. Frames counted in fractional vblanks
// (frame_pacing.cpp, rates above 60) can be any length: one no longer than T gets a step of T when the simulation is
// behind the marker; H is then 0, so a longer one is stepped in steps of 1/59.94 s instead of halves of 8.3-12.5 ms.
// T then follows the frame time (below): one step per frame, as at 60. SIMPSONS_PHYSIQUE_ORIGINALE=1: as shipped. SIMPSONS_TRACE_PHYSIQUE=1:
// steps per frame every 5 s.
PPC_FUNC_IMPL(__imp__sub_827A55C0); // Havok world step (r3 = owner, f1 = frame time)
PPC_FUNC_IMPL(__imp__sub_82AA2070); // hkWorld::stepDeltaTime (f1 = step)

std::atomic<int64_t> g_lastPhysicsStepNs{ 0 }; // read by frame_pacing.cpp: a level is running
bool FractionalVblanks();                       // frame_pacing.cpp

namespace
{
    constexpr uint32_t STEP_THRESHOLD = 0x82159238;          // T, read only by sub_827A55C0
    constexpr uint32_t STEP_THRESHOLD_SHIPPED = 0x3D08AB86;  // 1/29.97 s
    constexpr uint32_t STEP_VBLANK = 0x3C88AB86;             // 1/59.94 s
    constexpr uint32_t STEP_VBLANK_CONSTANT = 0x8215923C;    // 1/59.94 s, the step of long frames
    constexpr uint32_t CLOCK_FRAME_DT = 0x82CED748;
    constexpr uint32_t HALF_STEP_LIMIT = 0x82DFFA58;         // H, a static of sub_827A55C0
    constexpr uint32_t HALF_STEP_LIMIT_GUARD = 0x82DFFA5C;   // bit 0: H initialized
    constexpr uint32_t HALF_STEP_LIMIT_SHIPPED = 0x3D88AB86; // 4/59.94 s

    bool g_tracePhysics = false;
    bool g_inStep = false; // steps counted only from sub_827A55C0
    uint32_t g_frameSteps = 0;
    struct PhysicsStats
    {
        int64_t sinceNs = 0;
        uint32_t frames = 0, steps = 0, stepsPerFrame[4] = {};
        float minStep = 0, maxStep = 0;
    } g_physicsStats;

    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    float LoadBEFloat(uint32_t address)
    {
        uint32_t bits = LoadBE32(address);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }
}

PPC_FUNC(sub_827A55C0)
{
    int64_t now = NowNs();
    g_lastPhysicsStepNs.store(now, std::memory_order_relaxed);
    static const bool steady = getenv("SIMPSONS_PHYSIQUE_ORIGINALE") == nullptr;
    static const bool trace = (g_tracePhysics = getenv("SIMPSONS_TRACE_PHYSIQUE") != nullptr);
    (void)trace;
    if (steady)
    {
        static bool checked = false, imageOk = false; // T as shipped: this game image is the expected one
        if (!checked)
        {
            checked = true;
            imageOk = LoadBE32(STEP_THRESHOLD) == STEP_THRESHOLD_SHIPPED;
            if (imageOk)
                StoreBE32(STEP_THRESHOLD, STEP_VBLANK);
            else
                fprintf(stderr, "[physique] seuil inattendu %08X : physique d'origine\n", LoadBE32(STEP_THRESHOLD));
        }
        // H: 0 while frames are fractional, as shipped otherwise (once the step initialized it)
        bool fractional = FractionalVblanks();
        uint32_t guard = LoadBE32(HALF_STEP_LIMIT_GUARD);
        if (fractional || (guard & 1))
        {
            StoreBE32(HALF_STEP_LIMIT_GUARD, guard | 1);
            StoreBE32(HALF_STEP_LIMIT, fractional ? 0 : HALF_STEP_LIMIT_SHIPPED);
        }
        float frameDt = LoadBEFloat(CLOCK_FRAME_DT);
        if (imageOk && frameDt > 0.0f && std::isfinite(frameDt))
        {
            ctx.f1.f64 = frameDt;
            // Above 60 images/s, one step of the frame's length every frame (T = the frame time): the trampolines
            // (Land of Chocolate) let the player through when the world is only stepped every other frame, as they
            // did at 60 images/s with the shipped 33 ms steps. Frames longer than 1/59.94 s keep steps of 1/59.94 s.
            // SIMPSONS_PHYSIQUE_60=1: 60 steps a second at any rate.
            static const bool sixtySteps = getenv("SIMPSONS_PHYSIQUE_60") != nullptr;
            uint32_t threshold = STEP_VBLANK;
            if (fractional && !sixtySteps && frameDt < LoadBEFloat(STEP_VBLANK_CONSTANT))
                memcpy(&threshold, &frameDt, 4);
            StoreBE32(STEP_THRESHOLD, threshold);
        }
    }
    if (!g_tracePhysics)
    {
        __imp__sub_827A55C0(ctx, base);
        return;
    }
    g_frameSteps = 0;
    g_inStep = true;
    __imp__sub_827A55C0(ctx, base);
    g_inStep = false;
    PhysicsStats& s = g_physicsStats;
    if (s.sinceNs == 0 || now - s.sinceNs > 10'000'000'000ll)
        s = { now };
    s.frames++;
    s.stepsPerFrame[std::min(g_frameSteps, 3u)]++;
    if (now - s.sinceNs >= 5'000'000'000ll)
    {
        double seconds = (now - s.sinceNs) * 1e-9;
        fprintf(stderr, "[physique] %.1f s : %u images (%.1f/s), %u pas (%.1f/s) de %.2f a %.2f ms, pas par image 0:%u 1:%u 2:%u 3+:%u\n",
            seconds, s.frames, s.frames / seconds, s.steps, s.steps / seconds, s.minStep * 1000, s.maxStep * 1000,
            s.stepsPerFrame[0], s.stepsPerFrame[1], s.stepsPerFrame[2], s.stepsPerFrame[3]);
        s = { now };
    }
}

PPC_FUNC(sub_82AA2070)
{
    if (g_inStep)
    {
        float step = float(ctx.f1.f64);
        PhysicsStats& s = g_physicsStats;
        s.minStep = s.steps ? std::min(s.minStep, step) : step;
        s.maxStep = s.steps ? std::max(s.maxStep, step) : step;
        s.steps++;
        g_frameSteps++;
    }
    __imp__sub_82AA2070(ctx, base);
}

// Escape skips the movie playing (files.cpp): the movie update (sub_826B92F0) asks sub_8282D720 every frame whether the
// global movie player (0x82E06F5C) has reached the end; answered yes once, the game stops the player, then plays the
// next movie of the list or goes back to the game flow, as at the natural end.
PPC_FUNC_IMPL(__imp__sub_8282D720); // has the movie ended? (r3 = 1 when it has)
bool FilesTakeMovieSkip();

PPC_FUNC(sub_8282D720)
{
    __imp__sub_8282D720(ctx, base);
    if (FilesTakeMovieSkip())
        ctx.r3.u64 = 1;
}
