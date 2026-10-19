#ifndef SF_OPENING_H
#define SF_OPENING_H

#include <stdint.h>

/* opening_demo case 1 steps this clock. Trigger 101 holds background 0.
   The slogan and the sketch are drawn by the piece view. Later scenes
   and the other Opening.ppg cells stay out. */

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
    int16_t pos_offset;
    uint16_t screen;
    int8_t layer_r[3];
    int32_t layer_x[3];
    int32_t layer_y[3];
    int held;
} SfOpening;

void sf_opening_init(SfOpening *op);
void sf_opening_step(SfOpening *op);

#endif
