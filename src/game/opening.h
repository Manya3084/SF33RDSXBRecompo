#ifndef SF_OPENING_H
#define SF_OPENING_H

#include <stdint.h>

/* opening_demo case 1 steps this clock. Trigger 101 holds background 0.
   The piece view draws the slogan and the first sketch while the index is 0.
   Time 79 posts status 11. The next frame drops both, shows the second
   sketch, and turns on the two Opening.ppg cells behind it. layer_y[0] is
   that layer's quake in pixels. Scene 102 stays out. */

typedef struct SfOpening {
    int16_t timer;
    int16_t scene;
    int16_t status;
    int16_t sound_status;
    int8_t r_no_0;
    int8_t r_no_1;
    int8_t r_no_2;
    int16_t free_work;
    int16_t index;
    int16_t obj_disp;
    int16_t pos_offset;
    uint16_t screen;
    int16_t quake_i;
    int8_t layer_r[3];
    int32_t layer_x[3];
    int32_t layer_y[3];
    int32_t f6_cal[2];
    int16_t f6_y[2];
    int8_t f6_stop[2];
    int f6_live;
    int show_next;
    int held;
} SfOpening;

void sf_opening_init(SfOpening *op);
void sf_opening_step(SfOpening *op);

#endif
