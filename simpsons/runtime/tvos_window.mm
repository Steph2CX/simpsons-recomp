// Apple TV : le jeu occupe toujours tout l'ecran (pas de fenetre, de menus ni de plein ecran a basculer).
#include <SDL3/SDL.h>

bool PlatformIsFullscreen(SDL_Window*)
{
    return true;
}

void PlatformCoverScreen(SDL_Window*)
{
}

void PlatformToggleFullscreen(SDL_Window*)
{
}

int PlatformMenuCount()
{
    return 0;
}

// Inconnu sur tvOS (renderer.cpp choisit la resolution de l'Apple TV)
int PlatformGpuCoreCount()
{
    return 0;
}
