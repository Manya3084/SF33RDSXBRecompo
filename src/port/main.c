/* The native entry. The game itself starts in sf_game_init. */

#include "port/port.h"

#include <SDL3/SDL.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    sf_game_init();
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                SDL_Quit();
                return 0;
            }
        }
        sf_game_frame();
    }
}
