#ifndef SF_PORT_PAD_H
#define SF_PORT_PAD_H

#include <stdint.h>

/* An Xbox gamepad as the XDK's input sample reads it: what the port reads
 * from a device, records, and sends to a netplay peer. */
enum { XB_DPAD_UP = 1, XB_DPAD_DOWN = 2, XB_DPAD_LEFT = 4, XB_DPAD_RIGHT = 8, XB_START = 0x10, XB_BACK = 0x20,
       XB_LTHUMB = 0x40, XB_RTHUMB = 0x80 };
enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LTRIG, XB_RTRIG };

typedef struct XbGamepad {
    int connected;
    uint16_t buttons;
    uint8_t analog[8];
    float lx;
    float ly; /* up is positive */
    float rx;
    float ry;
} XbGamepad;

/* This machine's player: the keyboard, the touch pad and the first
 * gamepad together (src/port/input.c). */
void sf_input_local(XbGamepad *out);
/* A netplay frame's pads (src/port/net.c): 1 when a session gives them. */
int sf_net_pads(XbGamepad *x, int ports);

#endif
