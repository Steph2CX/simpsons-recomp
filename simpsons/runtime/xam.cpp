// XAM (profils, contenus/sauvegardes, notifications) et pilotes audio (XAudio, XMA).
//
// Etat volontairement simple pour l'instant : aucun profil connecte, aucun contenu sauvegarde,
// audio muet (contextes XMA valides mais jamais decodes).
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_NO_SUCH_USER = 0x525;

void SignalEventHandle(uint32_t handle);

static std::atomic<uint32_t> g_nextXamHandle{ 0xFA000004 };

static uint32_t NewXamHandle()
{
    return g_nextXamHandle.fetch_add(4);
}

// Completes an XOVERLAPPED: result, length, extended error, then signals its event
static void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
{
    StoreBE32(overlapped + 0x00, result);
    StoreBE32(overlapped + 0x04, length);
    StoreBE32(overlapped + 0x18, result);
    uint32_t event = LoadBE32(overlapped + 0x0C);
    if (event != 0)
        SignalEventHandle(event);
}

// ---------------------------------------------------------------------------
// Users: one local (offline) profile signed in as player 1

constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint64_t PROFILE_XUID = 0xE000000053494D50ull; // offline XUIDs start with 0xE
constexpr const char* PROFILE_NAME = "Joueur";

static void StoreBE64(uint32_t address, uint64_t value)
{
    StoreBE32(address, uint32_t(value >> 32));
    StoreBE32(address + 4, uint32_t(value));
}

KERNEL_FUNC(XamUserGetSigninState)
{
    // (DWORD User) -> 0 not signed in, 1 signed in locally, 2 signed in to Live
    LOG_CALL(XamUserGetSigninState);
    ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0;
}

KERNEL_FUNC(XamUserGetXUID)
{
    // (DWORD User, DWORD TypeMask, XUID* Xuid)
    LOG_CALL(XamUserGetXUID);
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = ERROR_NO_SUCH_USER;
        return;
    }
    if (ctx.r5.u32 != 0)
        StoreBE64(ctx.r5.u32, PROFILE_XUID);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamUserGetName)
{
    // (DWORD User, char* Buffer, DWORD BufferSize)
    LOG_CALL(XamUserGetName);
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = ERROR_NO_SUCH_USER;
        return;
    }
    if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0)
    {
        char* buffer = static_cast<char*>(GuestToHost(ctx.r4.u32));
        strncpy(buffer, PROFILE_NAME, ctx.r5.u32);
        buffer[ctx.r5.u32 - 1] = 0;
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamUserGetSigninInfo)
{
    // (DWORD User, DWORD Flags, XUSER_SIGNIN_INFO*): xuid, flags, state, guest number, sponsor, name[16]
    LOG_CALL(XamUserGetSigninInfo);
    if (ctx.r3.u32 != 0)
    {
        ctx.r3.u64 = ERROR_NO_SUCH_USER;
        return;
    }
    uint32_t info = ctx.r5.u32;
    if (info != 0)
    {
        memset(GuestToHost(info), 0, 40);
        StoreBE64(info, PROFILE_XUID);
        StoreBE32(info + 12, 1); // signed in locally
        strncpy(static_cast<char*>(GuestToHost(info + 24)), PROFILE_NAME, 15);
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamUserReadProfileSettings)
{
    // (DWORD TitleId, DWORD User, DWORD XuidCount, XUID* Xuids, DWORD SettingCount, DWORD* SettingIds,
    //  DWORD* BufferSize, XUSER_READ_PROFILE_SETTING_RESULT* Results, XOVERLAPPED* Overlapped [stack])
    // Every setting is returned as "not set": the game uses its defaults.
    LOG_CALL(XamUserReadProfileSettings);
    uint32_t xuidCount = std::max<uint32_t>(ctx.r5.u32, 1);
    uint32_t settingCount = ctx.r7.u32;
    uint32_t settingIds = ctx.r8.u32;
    uint32_t sizeAddress = ctx.r9.u32;
    uint32_t results = ctx.r10.u32;
    uint32_t overlapped = LoadBE32(ctx.r1.u32 + 0x54);

    uint32_t count = xuidCount * settingCount;
    uint32_t needed = 8 + count * 40;
    uint32_t result = ERROR_SUCCESS;
    if (sizeAddress == 0 || results == 0 || LoadBE32(sizeAddress) < needed)
    {
        if (sizeAddress != 0)
            StoreBE32(sizeAddress, needed);
        result = ERROR_INSUFFICIENT_BUFFER;
    }
    else
    {
        memset(GuestToHost(results), 0, needed);
        StoreBE32(results, count);
        StoreBE32(results + 4, results + 8);
        for (uint32_t x = 0; x < xuidCount; x++)
        {
            for (uint32_t i = 0; i < settingCount; i++)
            {
                // XUSER_PROFILE_SETTING: source, user/xuid, id, data (type in the top bits of the id)
                uint32_t setting = results + 8 + (x * settingCount + i) * 40;
                uint32_t id = LoadBE32(settingIds + i * 4);
                StoreBE32(setting + 0, 0); // XSOURCE_NO_VALUE
                if (ctx.r6.u32 != 0 && ctx.r5.u32 != 0)
                    StoreBE64(setting + 8, PROFILE_XUID);
                StoreBE32(setting + 16, id);
                *static_cast<uint8_t*>(GuestToHost(setting + 24)) = uint8_t(id >> 28);
            }
        }
    }
    if (overlapped != 0)
    {
        CompleteOverlapped(overlapped, result, 0);
        ctx.r3.u64 = ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = result;
}

KERNEL_FUNC(XamUserWriteProfileSettings)
{
    // (DWORD User, DWORD SettingCount, XUSER_PROFILE_SETTING* Settings, XOVERLAPPED* Overlapped)
    LOG_CALL(XamUserWriteProfileSettings);
    if (ctx.r6.u32 != 0)
    {
        CompleteOverlapped(ctx.r6.u32, ERROR_SUCCESS, 0);
        ctx.r3.u64 = ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XamUserCheckPrivilege)
{
    // (DWORD User, DWORD Privilege, BOOL* Result): offline profile, no Live privileges
    LOG_CALL(XamUserCheckPrivilege);
    if (ctx.r5.u32 != 0)
        StoreBE32(ctx.r5.u32, 0);
    ctx.r3.u64 = ctx.r3.u32 == 0 ? ERROR_SUCCESS : ERROR_NO_SUCH_USER;
}

void PostNotification(uint32_t id, uint32_t param);

KERNEL_FUNC(XamShowSigninUI)
{
    // (DWORD Panes, DWORD Flags): the profile is already signed in; report the UI as shown then closed
    LOG_CALL(XamShowSigninUI);
    PostNotification(0x00000009, 1); // XN_SYS_UI: shown
    PostNotification(0x00000009, 0); // hidden
    PostNotification(0x0000000A, 1); // XN_SYS_SIGNINCHANGED: player 1
    ctx.r3.u64 = ERROR_SUCCESS;
}

// Content (saves) and storage devices: see content.cpp

extern uint32_t g_executionId;

KERNEL_FUNC(XamGetExecutionId)
{
    // (XEX_EXECUTION_ID** ExecutionId): media id, versions, title id, platform, disc number...
    LOG_CALL(XamGetExecutionId);
    if (ctx.r3.u32 != 0)
        StoreBE32(ctx.r3.u32, g_executionId);
    ctx.r3.u64 = g_executionId ? ERROR_SUCCESS : 0x80070490; // E_NOTFOUND
}

// ---------------------------------------------------------------------------
// Notifications

uint32_t RuntimeCreateEvent(bool manualReset, bool signaled);
void RuntimeSetEvent(uint32_t handle, bool signaled);

// Pending notifications, delivered to every listener (one queue each). A listener handle is a manual-reset
// event, signaled while its queue is not empty (the game waits on it before calling XNotifyGetNext).
static std::mutex g_notifyMutex;
static std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_notifyQueues;

void PostNotification(uint32_t id, uint32_t param)
{
    std::lock_guard lock(g_notifyMutex);
    for (auto& [listener, queue] : g_notifyQueues)
    {
        queue.emplace_back(id, param);
        RuntimeSetEvent(listener, true);
    }
}

KERNEL_FUNC(XamNotifyCreateListener)
{
    // (QWORD AreaMask): the sign-in state is reported once, like at boot on the console
    LOG_CALL(XamNotifyCreateListener);
    uint32_t handle = RuntimeCreateEvent(true, true);
    {
        std::lock_guard lock(g_notifyMutex);
        g_notifyQueues[handle].emplace_back(0x0000000A, 1); // XN_SYS_SIGNINCHANGED
    }
    ctx.r3.u64 = handle;
}

KERNEL_FUNC(XNotifyGetNext)
{
    // (HANDLE Listener, DWORD Id (0 = any), DWORD* NotificationId, DWORD* Param) -> TRUE if one was returned
    LOG_CALL(XNotifyGetNext);
    std::lock_guard lock(g_notifyMutex);
    auto it = g_notifyQueues.find(ctx.r3.u32);
    if (it != g_notifyQueues.end())
    {
        auto& queue = it->second;
        for (size_t i = 0; i < queue.size(); i++)
        {
            if (ctx.r4.u32 != 0 && queue[i].first != ctx.r4.u32)
                continue;
            if (ctx.r5.u32 != 0)
                StoreBE32(ctx.r5.u32, queue[i].first);
            if (ctx.r6.u32 != 0)
                StoreBE32(ctx.r6.u32, queue[i].second);
            queue.erase(queue.begin() + i);
            if (queue.empty())
                RuntimeSetEvent(ctx.r3.u32, false);
            ctx.r3.u64 = 1;
            return;
        }
    }
    ctx.r3.u64 = 0; // nothing pending
}

// ---------------------------------------------------------------------------
// Audio: XAudio render driver and XMA decoder contexts (silent for now)

KERNEL_FUNC(XAudioGetSpeakerConfig)
{
    LOG_CALL(XAudioGetSpeakerConfig);
    StoreBE32(ctx.r3.u32, 0x00010001); // stereo
    ctx.r3.u64 = ERROR_SUCCESS;
}

// The audio hardware pulls a frame of 256 samples at 48 kHz from each registered client by
// calling its callback. Samples are discarded for now, but the callbacks drive the game's audio
// clock (movies and streaming wait on it).
struct AudioClient
{
    uint32_t callback;
    uint32_t argument;
};

static std::mutex g_audioMutex;
static std::vector<AudioClient> g_audioClients;
static bool g_audioThreadStarted = false;

static void AudioThread()
{
    PPCContext& ctx = InitMainGuestThread(0x10000);
    // Frame n is due at start + n * 256 / 48000 s, computed exactly (whole 5333 us steps drifted to 48004 Hz)
    const auto start = std::chrono::steady_clock::now();
    for (int64_t n = 1;; n++)
    {
        std::this_thread::sleep_until(start + std::chrono::nanoseconds(n * 256 * 1000000000 / 48000));

        std::vector<AudioClient> clients;
        {
            std::lock_guard lock(g_audioMutex);
            clients = g_audioClients;
        }
        for (auto& client : clients)
        {
            ctx.r3.u64 = client.argument;
            PPCFunc* callback = PPC_LOOKUP_FUNC(g_base, client.callback);
            callback(ctx, g_base);
        }
    }
}

KERNEL_FUNC(XAudioRegisterRenderDriverClient)
{
    // (DWORD* Callback = { function, argument }, DWORD* Driver)
    LOG_CALL(XAudioRegisterRenderDriverClient);
    AudioClient client{ LoadBE32(ctx.r3.u32), LoadBE32(ctx.r3.u32 + 4) };
    uint32_t index;
    {
        std::lock_guard lock(g_audioMutex);
        index = uint32_t(g_audioClients.size());
        g_audioClients.push_back(client);
        if (!g_audioThreadStarted)
        {
            g_audioThreadStarted = true;
            std::thread(AudioThread).detach();
        }
    }
    fprintf(stderr, "[audio] client %u : rappel 0x%X(0x%X)\n", index, client.callback, client.argument);
    StoreBE32(ctx.r4.u32, 0x41550000 | index);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XAudioUnregisterRenderDriverClient)
{
    LOG_CALL(XAudioUnregisterRenderDriverClient);
    ctx.r3.u64 = ERROR_SUCCESS;
}

void SubmitAudioFrame(uint32_t samples);

KERNEL_FUNC(XAudioSubmitRenderDriverFrame)
{
    // (DWORD Driver, FLOAT* Samples)
    LOG_CALL(XAudioSubmitRenderDriverFrame);
    SubmitAudioFrame(ctx.r4.u32);
    ctx.r3.u64 = ERROR_SUCCESS;
}




KERNEL_FUNC(XAudioGetVoiceCategoryVolumeChangeMask)
{
    // (DWORD Driver, DWORD* Mask) - every category "changed" once so the game reads the volumes
    LOG_CALL(XAudioGetVoiceCategoryVolumeChangeMask);
    static std::atomic<bool> first{ true };
    StoreBE32(ctx.r4.u32, first.exchange(false) ? 0xFFFFFFFF : 0);
    ctx.r3.u64 = ERROR_SUCCESS;
}

KERNEL_FUNC(XAudioGetVoiceCategoryVolume)
{
    // (DWORD Category, FLOAT* Volume) - dashboard volumes all at 100 %
    LOG_CALL(XAudioGetVoiceCategoryVolume);
    float volume = 1.0f;
    uint32_t bits;
    memcpy(&bits, &volume, 4);
    StoreBE32(ctx.r4.u32, bits);
    ctx.r3.u64 = ERROR_SUCCESS;
}
