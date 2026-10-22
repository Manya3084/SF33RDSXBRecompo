#ifndef SF_PORT_TOUCH_H
#define SF_PORT_TOUCH_H

/* On-screen controls for a phone (src/port/touch.c). */

enum {
    TOUCH_UP = 1 << 0,
    TOUCH_DOWN = 1 << 1,
    TOUCH_LEFT = 1 << 2,
    TOUCH_RIGHT = 1 << 3,
    TOUCH_START = 1 << 4,
    TOUCH_BACK = 1 << 5,
    TOUCH_A = 1 << 6,
    TOUCH_B = 1 << 7,
    TOUCH_X = 1 << 8,
    TOUCH_Y = 1 << 9,
    TOUCH_BLACK = 1 << 10,
    TOUCH_WHITE = 1 << 11,
};

/* What the fingers hold now (TOUCH_*); 0 when the controls are off. */
unsigned sf_touch_read(void);

/* Once a frame, before it is drawn: the controls' picture into the list. */
void sf_touch_frame(void);

/* SCREEN ADJUST's TOUCH PAD (on by default on Android, never elsewhere). */
int sf_touch_on(void);
void sf_touch_set_on(int on);

#endif
