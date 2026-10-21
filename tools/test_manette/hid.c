#include <SDL3/SDL.h>
#include <stdio.h>
int main(void){
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_Init(SDL_INIT_GAMEPAD);
    SDL_hid_device_info* d = SDL_hid_enumerate(0x2dc8, 0);
    for (SDL_hid_device_info* i = d; i; i = i->next)
        printf("HID %04x:%04x page 0x%x usage 0x%x interface %d : %ls\n", i->vendor_id, i->product_id, i->usage_page, i->usage, i->interface_number, i->product_string ? i->product_string : L"?");
    SDL_hid_free_enumeration(d);
    int n=0; SDL_JoystickID* j = SDL_GetJoysticks(&n);
    printf("%d joystick(s)\n", n);
    for (int k=0;k<n;k++) printf("  %s (manette: %d)\n", SDL_GetJoystickNameForID(j[k]), SDL_IsGamepad(j[k]));
    SDL_Quit(); return 0; }
