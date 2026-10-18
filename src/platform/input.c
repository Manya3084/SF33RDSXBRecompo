#include "platform/input.h"

#include <string.h>

static SDL_Gamepad *g_pads[4];
static int g_held[SF_BTN_COUNT];
static int g_prev[SF_BTN_COUNT];

static void remember(SDL_JoystickID id)
{
    SDL_Gamepad *pad;
    int i;
    if (!SDL_IsGamepad(id)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            return;
        }
    }
    pad = SDL_OpenGamepad(id);
    if (!pad) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (!g_pads[i]) {
            g_pads[i] = pad;
            return;
        }
    }
    SDL_CloseGamepad(pad);
}

void sf_input_init(void)
{
    int count = 0;
    SDL_JoystickID *ids;
    int i;
    memset(g_pads, 0, sizeof g_pads);
    memset(g_held, 0, sizeof g_held);
    memset(g_prev, 0, sizeof g_prev);
    ids = SDL_GetGamepads(&count);
    if (!ids) {
        return;
    }
    for (i = 0; i < count; i++) {
        remember(ids[i]);
    }
    SDL_free(ids);
}

void sf_input_quit(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (g_pads[i]) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
    }
}

void sf_input_event(const SDL_Event *event)
{
    int i;
    if (!event) {
        return;
    }
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        remember(event->gdevice.which);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (i = 0; i < 4; i++) {
            if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == event->gdevice.which) {
                SDL_CloseGamepad(g_pads[i]);
                g_pads[i] = NULL;
            }
        }
    }
}

static void hold_key(const bool *keys, SfButton button, SDL_Scancode code)
{
    if (keys[code]) {
        g_held[button] = 1;
    }
}

void sf_input_poll(void)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    int i;
    memcpy(g_prev, g_held, sizeof g_held);
    memset(g_held, 0, sizeof g_held);
    if (keys) {
        hold_key(keys, SF_UP, SDL_SCANCODE_UP);
        hold_key(keys, SF_DOWN, SDL_SCANCODE_DOWN);
        hold_key(keys, SF_LEFT, SDL_SCANCODE_LEFT);
        hold_key(keys, SF_RIGHT, SDL_SCANCODE_RIGHT);
        hold_key(keys, SF_A, SDL_SCANCODE_Z);
        hold_key(keys, SF_B, SDL_SCANCODE_X);
        hold_key(keys, SF_X, SDL_SCANCODE_A);
        hold_key(keys, SF_Y, SDL_SCANCODE_S);
        hold_key(keys, SF_START, SDL_SCANCODE_RETURN);
        hold_key(keys, SF_BACK, SDL_SCANCODE_BACKSPACE);
        hold_key(keys, SF_LB, SDL_SCANCODE_Q);
        hold_key(keys, SF_RB, SDL_SCANCODE_E);
    }
    for (i = 0; i < 4; i++) {
        SDL_Gamepad *pad = g_pads[i];
        if (!pad) {
            continue;
        }
        g_held[SF_UP] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        g_held[SF_DOWN] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        g_held[SF_LEFT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        g_held[SF_RIGHT] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        g_held[SF_A] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        g_held[SF_B] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
        g_held[SF_X] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
        g_held[SF_Y] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
        g_held[SF_START] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        g_held[SF_BACK] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        g_held[SF_LB] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        g_held[SF_RB] |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    }
}

int sf_pressed(SfButton button)
{
    if (button < 0 || button >= SF_BTN_COUNT) {
        return 0;
    }
    return g_held[button] && !g_prev[button];
}
