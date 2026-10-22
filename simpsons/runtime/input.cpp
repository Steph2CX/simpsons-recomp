// Manettes : XamInputGetState & co, alimentes par SDL3 (manette Xbox, PlayStation, etc.) ou le clavier.
//
// L'etat est lu sur le thread principal (boucle d'evenements SDL) et copie dans une structure
// partagee que le jeu lit depuis ses threads. Jusqu'a 4 joueurs : chaque manette branchee prend la
// premiere place libre ; le clavier et SIMPSONS_TOUCHES commandent le joueur 1 (toujours connecte).
//
// Clavier (si aucune manette n'est branchee, ou en complement) :
//   fleches / ZQSD-WASD : stick gauche et croix   I J K L : stick droit (camera)
//   Entree : START   Tab : BACK   Espace : A   E : B   R : X   F : Y
//   1 : LB   3 : RB   Maj gauche : gachette gauche   Maj droite / C : gachette droite
#include "runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#define KERNEL_FUNC(name) PPC_FUNC(__imp__##name)

uint64_t GpuFrameCount();
void FilesSkipMovies(); // files.cpp

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
    constexpr uint32_t ERROR_EMPTY = 4306;

    enum : uint16_t
    {
        BUTTON_DPAD_UP = 0x0001,
        BUTTON_DPAD_DOWN = 0x0002,
        BUTTON_DPAD_LEFT = 0x0004,
        BUTTON_DPAD_RIGHT = 0x0008,
        BUTTON_START = 0x0010,
        BUTTON_BACK = 0x0020,
        BUTTON_LEFT_THUMB = 0x0040,
        BUTTON_RIGHT_THUMB = 0x0080,
        BUTTON_LEFT_SHOULDER = 0x0100,
        BUTTON_RIGHT_SHOULDER = 0x0200,
        BUTTON_A = 0x1000,
        BUTTON_B = 0x2000,
        BUTTON_X = 0x4000,
        BUTTON_Y = 0x8000,
    };

    struct PadState
    {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
        bool operator==(const PadState&) const = default;
    };

    constexpr uint32_t MAX_PLAYERS = 4;

    std::mutex g_mutex;
    PadState g_states[MAX_PLAYERS];
    uint32_t g_packetNumbers[MAX_PLAYERS]{};
    bool g_connected[MAX_PLAYERS] = { true }; // player 1: keyboard at least
    SDL_Gamepad* g_gamepads[MAX_PLAYERS]{}; // main thread only, except rumble (SDL locks internally)

    int16_t InvertAxis(int16_t value)
    {
        return int16_t(std::clamp(-int32_t(value), -32768, 32767));
    }

    void StoreGamepad(uint32_t address, const PadState& s)
    {
        StoreBE16(address + 0, s.buttons);
        *static_cast<uint8_t*>(GuestToHost(address + 2)) = s.leftTrigger;
        *static_cast<uint8_t*>(GuestToHost(address + 3)) = s.rightTrigger;
        StoreBE16(address + 4, uint16_t(s.thumbLX));
        StoreBE16(address + 6, uint16_t(s.thumbLY));
        StoreBE16(address + 8, uint16_t(s.thumbRX));
        StoreBE16(address + 10, uint16_t(s.thumbRY));
    }
}

// Main thread: gamepad hot-plug
void PostNotification(uint32_t id, uint32_t param);

void InputHandleEvent(const SDL_Event& event)
{
    constexpr uint32_t XN_SYS_INPUTDEVICESCHANGED = 0x00000012;
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
    {
        for (uint32_t player = 0; player < MAX_PLAYERS; player++)
        {
            if (g_gamepads[player] != nullptr && SDL_GetGamepadID(g_gamepads[player]) == event.gdevice.which)
                return; // already open
        }
        for (uint32_t player = 0; player < MAX_PLAYERS; player++)
        {
            if (g_gamepads[player] != nullptr)
                continue;
            g_gamepads[player] = SDL_OpenGamepad(event.gdevice.which);
            if (g_gamepads[player] == nullptr)
                return;
            fprintf(stderr, "[manette] joueur %u : %s\n", player + 1, SDL_GetGamepadName(g_gamepads[player]));
            SDL_SetGamepadPlayerIndex(g_gamepads[player], int(player));
            {
                std::lock_guard lock(g_mutex);
                g_connected[player] = true;
            }
            PostNotification(XN_SYS_INPUTDEVICESCHANGED, 0);
            return;
        }
    }
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
    {
        for (uint32_t player = 0; player < MAX_PLAYERS; player++)
        {
            if (g_gamepads[player] == nullptr || SDL_GetGamepadID(g_gamepads[player]) != event.gdevice.which)
                continue;
            fprintf(stderr, "[manette] joueur %u debranchee\n", player + 1);
            SDL_CloseGamepad(g_gamepads[player]);
            g_gamepads[player] = nullptr;
            {
                std::lock_guard lock(g_mutex);
                g_connected[player] = player == 0; // the keyboard stays on player 1
                g_states[player] = {};
                g_packetNumbers[player]++;
            }
            PostNotification(XN_SYS_INPUTDEVICESCHANGED, 0);
        }
    }
}

namespace
{
    PadState ReadGamepad(SDL_Gamepad* gamepad)
    {
        PadState s;
        if (gamepad == nullptr)
            return s;
        static const std::pair<SDL_GamepadButton, uint16_t> buttons[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, BUTTON_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, BUTTON_DPAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, BUTTON_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BUTTON_DPAD_RIGHT },
            { SDL_GAMEPAD_BUTTON_START, BUTTON_START }, { SDL_GAMEPAD_BUTTON_BACK, BUTTON_BACK },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, BUTTON_LEFT_THUMB }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, BUTTON_RIGHT_THUMB },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BUTTON_LEFT_SHOULDER }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BUTTON_RIGHT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_SOUTH, BUTTON_A }, { SDL_GAMEPAD_BUTTON_EAST, BUTTON_B },
            { SDL_GAMEPAD_BUTTON_WEST, BUTTON_X }, { SDL_GAMEPAD_BUTTON_NORTH, BUTTON_Y } };
        for (auto [button, bit] : buttons)
        {
            if (SDL_GetGamepadButton(gamepad, button))
                s.buttons |= bit;
        }
        s.leftTrigger = uint8_t(std::clamp<int32_t>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7, 0, 255));
        s.rightTrigger = uint8_t(std::clamp<int32_t>(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7, 0, 255));
        // SDL: Y axes point down; XInput: up
        s.thumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        s.thumbLY = InvertAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        s.thumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        s.thumbRY = InvertAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        return s;
    }
}

// Main thread: on the Apple TV, holding B for 2 s quits to the home screen (a short press stays the game's B, and
// tvOS gives no Menu button to the app while a gamepad is in use)
bool InputQuitRequested()
{
#ifdef SIMPSONS_TVOS
    static Uint64 heldSince = 0;
    bool held = false;
    for (SDL_Gamepad* gamepad : g_gamepads)
        held |= gamepad != nullptr && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST);
    if (!held)
    {
        heldSince = 0;
        return false;
    }
    if (heldSince == 0)
        heldSince = SDL_GetTicks();
    if (SDL_GetTicks() - heldSince < 2000)
        return false;
    fprintf(stderr, "[manette] B maintenu : retour a l'accueil\n");
    return true;
#else
    return false;
#endif
}

// Main thread: samples the gamepad and the keyboard
void InputPoll()
{
    PadState s = ReadGamepad(g_gamepads[0]);

    const bool* keys = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode code) { return keys[code]; };
    static const std::pair<SDL_Scancode, uint16_t> keyButtons[] = {
        { SDL_SCANCODE_UP, BUTTON_DPAD_UP }, { SDL_SCANCODE_DOWN, BUTTON_DPAD_DOWN },
        { SDL_SCANCODE_LEFT, BUTTON_DPAD_LEFT }, { SDL_SCANCODE_RIGHT, BUTTON_DPAD_RIGHT },
        { SDL_SCANCODE_RETURN, BUTTON_START }, { SDL_SCANCODE_TAB, BUTTON_BACK },
        { SDL_SCANCODE_SPACE, BUTTON_A }, { SDL_SCANCODE_E, BUTTON_B }, { SDL_SCANCODE_R, BUTTON_X }, { SDL_SCANCODE_F, BUTTON_Y },
        { SDL_SCANCODE_1, BUTTON_LEFT_SHOULDER }, { SDL_SCANCODE_3, BUTTON_RIGHT_SHOULDER } };
    for (auto [code, bit] : keyButtons)
    {
        if (key(code))
            s.buttons |= bit;
    }
    if (key(SDL_SCANCODE_LSHIFT))
        s.leftTrigger = 255;
    if (key(SDL_SCANCODE_RSHIFT) || key(SDL_SCANCODE_C))
        s.rightTrigger = 255;
    // Scancodes are positions on a QWERTY layout: W/A/S/D are Z/Q/S/D on an AZERTY keyboard
    int32_t lx = (key(SDL_SCANCODE_D) || key(SDL_SCANCODE_RIGHT) ? 32767 : 0) - (key(SDL_SCANCODE_A) || key(SDL_SCANCODE_LEFT) ? 32767 : 0);
    int32_t ly = (key(SDL_SCANCODE_W) || key(SDL_SCANCODE_UP) ? 32767 : 0) - (key(SDL_SCANCODE_S) || key(SDL_SCANCODE_DOWN) ? 32767 : 0);
    int32_t rx = (key(SDL_SCANCODE_L) ? 32767 : 0) - (key(SDL_SCANCODE_J) ? 32767 : 0);
    int32_t ry = (key(SDL_SCANCODE_I) ? 32767 : 0) - (key(SDL_SCANCODE_K) ? 32767 : 0);
    if (lx || ly)
    {
        s.thumbLX = int16_t(lx);
        s.thumbLY = int16_t(ly);
    }
    if (rx || ry)
    {
        s.thumbRX = int16_t(rx);
        s.thumbRY = int16_t(ry);
    }

    // SIMPSONS_TOUCHES="40:START,45:A,i1300:BAS,i6200:AVANCE*300": presses buttons at given times (seconds) for
    // 200 ms, or at a given frame ("i" prefix) for 10 frames (tests). "*N" holds for N seconds / frames.
    // "J2" before a name: virtual player 2 ("i6400:J2START").
    // Left stick: AVANCE, RECULE, VAGAUCHE, VADROITE; right stick (camera): CAMGAUCHE, CAMDROITE; triggers LT, RT;
    // ECHAP: the Escape key (skips the movie).
    static const char* script = getenv("SIMPSONS_TOUCHES");
    PadState scripted2;
    bool scriptedPlayer2 = false;
    if (script != nullptr)
    {
        static const uint64_t start = SDL_GetTicks();
        double now = (SDL_GetTicks() - start) / 1000.0;
        static const std::pair<const char*, uint16_t> names[] = { { "START", BUTTON_START }, { "BACK", BUTTON_BACK },
            { "A", BUTTON_A }, { "B", BUTTON_B }, { "X", BUTTON_X }, { "Y", BUTTON_Y }, { "HAUT", BUTTON_DPAD_UP },
            { "BAS", BUTTON_DPAD_DOWN }, { "GAUCHE", BUTTON_DPAD_LEFT }, { "DROITE", BUTTON_DPAD_RIGHT },
            { "LB", BUTTON_LEFT_SHOULDER }, { "RB", BUTTON_RIGHT_SHOULDER } };
        for (const char* p = script; *p;)
        {
            bool frames = *p == 'i';
            if (frames)
                p++;
            double at = strtod(p, const_cast<char**>(&p));
            double clock = frames ? double(GpuFrameCount()) : now;
            double duration = frames ? 10 : 0.2;
            if (*p == ':')
                p++;
            const char* end = strchr(p, ',');
            size_t length = end ? size_t(end - p) : strlen(p);
            std::string name(p, length);
            size_t star = name.find('*');
            if (star != std::string::npos)
            {
                duration = atof(name.c_str() + star + 1);
                name.resize(star);
            }
            // "J2" prefix: virtual player 2 (connected as soon as the script mentions it)
            PadState* target = &s;
            if (name.rfind("J2", 0) == 0)
            {
                name.erase(0, 2);
                target = &scripted2;
                scriptedPlayer2 = true;
            }
            if (clock >= at && clock < at + duration)
            {
                PadState& s = *target;
                for (auto [buttonName, bit] : names)
                {
                    if (name == buttonName)
                        s.buttons |= bit;
                }
                if (name == "AVANCE")
                    s.thumbLY = 32767;
                else if (name == "RECULE")
                    s.thumbLY = -32767;
                else if (name == "VAGAUCHE")
                    s.thumbLX = -32767;
                else if (name == "VADROITE")
                    s.thumbLX = 32767;
                else if (name == "CAMGAUCHE")
                    s.thumbRX = -32767;
                else if (name == "CAMDROITE")
                    s.thumbRX = 32767;
                else if (name == "LT")
                    s.leftTrigger = 255;
                else if (name == "RT")
                    s.rightTrigger = 255;
                else if (name == "ECHAP") // Escape key: skips the movie, once per press like the keyboard
                {
                    static std::vector<double> pressed;
                    if (std::find(pressed.begin(), pressed.end(), at) == pressed.end())
                    {
                        pressed.push_back(at);
                        FilesSkipMovies();
                    }
                }
            }
            p += length;
            if (*p == ',')
                p++;
        }
    }

    PadState others[MAX_PLAYERS];
    for (uint32_t player = 1; player < MAX_PLAYERS; player++)
        others[player] = ReadGamepad(g_gamepads[player]);
    if (scriptedPlayer2 && g_gamepads[1] == nullptr)
        others[1] = scripted2;

    std::lock_guard lock(g_mutex);
    if (scriptedPlayer2)
        g_connected[1] = true;
    for (uint32_t player = 1; player < MAX_PLAYERS; player++)
    {
        if (!(others[player] == g_states[player]))
        {
            g_states[player] = others[player];
            g_packetNumbers[player]++;
        }
    }
    PadState& current = g_states[0];
    if (!(s == current))
    {
        static bool trace = getenv("SIMPSONS_TRACE_INPUT") != nullptr;
        if (trace && s.buttons != current.buttons)
            fprintf(stderr, "[manette] boutons 0x%04X\n", s.buttons);
        current = s;
        g_packetNumbers[0]++;
    }
}

// (user index, flags, XINPUT_STATE*): packet number then XINPUT_GAMEPAD
KERNEL_FUNC(XamInputGetState)
{
    uint32_t user = ctx.r3.u32;
    uint32_t stateAddress = ctx.r5.u32;
    std::lock_guard lock(g_mutex);
    if (user >= MAX_PLAYERS || !g_connected[user])
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    const PadState& current = g_states[user];
    if (stateAddress != 0)
    {
        static bool trace = getenv("SIMPSONS_TRACE_INPUT") != nullptr;
        static uint16_t lastRead = 0;
        if (trace && current.buttons != lastRead)
        {
            fprintf(stderr, "[manette] lu par le jeu : 0x%04X (lr=0x%llX)\n", current.buttons, (unsigned long long)GuestLR(ctx));
            lastRead = current.buttons;
        }
        StoreBE32(stateAddress, g_packetNumbers[user]);
        StoreGamepad(stateAddress + 4, current);
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// (user index, flags, XINPUT_CAPABILITIES*): type, subtype, flags, gamepad, vibration
KERNEL_FUNC(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32;
    uint32_t caps = ctx.r5.u32;
    bool connected;
    {
        std::lock_guard lock(g_mutex);
        connected = user < MAX_PLAYERS && g_connected[user];
    }
    if (!connected)
    {
        ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    if (caps != 0)
    {
        memset(GuestToHost(caps), 0, 20);
        *static_cast<uint8_t*>(GuestToHost(caps + 0)) = 1; // XINPUT_DEVTYPE_GAMEPAD
        *static_cast<uint8_t*>(GuestToHost(caps + 1)) = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        PadState all{ 0xFFFF, 0xFF, 0xFF, -64, -64, -64, -64 };
        StoreGamepad(caps + 4, all);
        StoreBE16(caps + 16, 0xFFFF);
        StoreBE16(caps + 18, 0xFFFF);
    }
    ctx.r3.u64 = ERROR_SUCCESS;
}

// (user index, flags, XINPUT_VIBRATION*): left (low frequency) and right motor speeds
KERNEL_FUNC(XamInputSetState)
{
    uint32_t user = ctx.r3.u32;
    uint32_t vibration = ctx.r5.u32;
    SDL_Gamepad* gamepad = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (user >= MAX_PLAYERS || !g_connected[user])
        {
            ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED;
            return;
        }
        gamepad = g_gamepads[user];
    }
    if (vibration != 0 && gamepad != nullptr)
        SDL_RumbleGamepad(gamepad, LoadBE16(vibration), LoadBE16(vibration + 2), 500);
    ctx.r3.u64 = ERROR_SUCCESS;
}

// Text input keystrokes: none
KERNEL_FUNC(XamInputGetKeystrokeEx)
{
    ctx.r3.u64 = ERROR_EMPTY;
}
