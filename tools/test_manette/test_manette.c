// Liste les manettes vues par SDL (la meme bibliotheque que le jeu) et affiche les boutons appuyes.
#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        printf("SDL_Init : %s\n", SDL_GetError());
        return 1;
    }
    Uint64 end = SDL_GetTicks() + 20000;
    while (SDL_GetTicks() < end)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
            {
                SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which);
                printf("manette connectee : %s\n", g ? SDL_GetGamepadName(g) : SDL_GetError());
            }
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
                printf("manette debranchee\n");
            else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                printf("bouton : %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
            else if (e.type == SDL_EVENT_JOYSTICK_ADDED && !SDL_IsGamepad(e.jdevice.which))
                printf("joystick non reconnu comme manette : %s\n", SDL_GetJoystickNameForID(e.jdevice.which));
        }
        fflush(stdout);
        SDL_Delay(10);
    }
    SDL_Quit();
    return 0;
}
