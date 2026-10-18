#ifndef SF_INPUT_H
#define SF_INPUT_H

#include <SDL3/SDL.h>

typedef enum SfButton {
    SF_UP = 0,
    SF_DOWN,
    SF_LEFT,
    SF_RIGHT,
    SF_A,
    SF_B,
    SF_X,
    SF_Y,
    SF_START,
    SF_BACK,
    SF_LB,
    SF_RB,
    SF_BTN_COUNT
} SfButton;

void sf_input_init(void);
void sf_input_quit(void);
void sf_input_event(const SDL_Event *event);
void sf_input_poll(void);
int sf_pressed(SfButton button);

#endif
