// Fixes to the game's own behaviour (hooks on guest functions).
#include "runtime.h"

#include <cstdio>

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
