#include "game/opening.h"

#include <string.h>

/* 0xAF4C2 is Bg_Off. Background 0 clears screen bits. It does not place a cell.
   101 and 0x65 are the same status, so op_101_move stays on background 0.
   The piece view draws the slogan and the sketch. Layer 2 keeps walking
   on this hold, and the slogan follows that camera. Go_BGM and ToneDown
   stay for a later beat. Triggers after 101 stay there too. */

static void bg_off(SfOpening *op, uint16_t bits)
{
    op->screen = (uint16_t)(op->screen & (uint16_t)~bits);
}

static void move_bg0(SfOpening *op)
{
    if (op->layer_r[0] == 0) {
        op->layer_r[0] = 1;
        bg_off(op, 1);
        op->layer_x[0] = 0x02000000;
        op->layer_y[0] = 0;
    }
}

static void move_bg1(SfOpening *op)
{
    if (op->layer_r[1] == 0) {
        op->layer_r[1] = 1;
        op->layer_x[1] = 0x02000000;
        op->layer_y[1] = 0;
        bg_off(op, 2);
    }
}

static void move_bg2(SfOpening *op)
{
    if (op->layer_r[2] == 0) {
        op->layer_r[2] = 1;
        op->layer_x[2] = 0x02000000;
        op->layer_y[2] = 0;
        return;
    }
    if (op->layer_r[2] == 1) {
        op->layer_x[2] -= 0x10000;
    }
}

static void move_background(SfOpening *op)
{
    move_bg0(op);
    move_bg1(op);
    move_bg2(op);
    op->index = 0;
}

static void sound_trigger(SfOpening *op)
{
    if (op->status == 101) {
        return;
    }
    if (op->scene == 0 && op->timer >= 4) {
        op->scene = 1;
        op->status = 101;
    }
}

static void op_101(SfOpening *op)
{
    if (op->r_no_2 == 0) {
        op->r_no_2 = 1;
        move_background(op);
        return;
    }
    move_background(op);
}

static void op_100(SfOpening *op)
{
    op->r_no_1 = (int8_t)(op->r_no_1 + 1);
    op_101(op);
}

static void opening_move(SfOpening *op)
{
    op->timer = (int16_t)(op->timer + 1);
    sound_trigger(op);
    if (op->r_no_1 == 0) {
        op_100(op);
    } else {
        op_101(op);
    }
    op->sound_status = op->status;
    if (op->status == 101) {
        op->held = 1;
    }
}

static void init_screens(SfOpening *op)
{
    int i;
    op->r_no_1 = 1;
    op->pos_offset = 192;
    for (i = 0; i < 3; i++) {
        op->layer_x[i] = 0x02000000;
        op->layer_y[i] = 0;
        op->layer_r[i] = 0;
    }
    op->r_no_2 = 0;
}

static void init_wait(SfOpening *op)
{
    op->r_no_1 = 2;
    bg_off(op, 0xF);
    bg_off(op, 0xF);
    op->free_work = 8;
}

static void init_countdown(SfOpening *op)
{
    op->free_work = (int16_t)(op->free_work - 1);
    if (op->free_work < 0) {
        op->r_no_0 = 1;
        op->r_no_1 = 0;
        op->r_no_2 = 0;
    }
    op->timer = 2;
    op->status = 0;
    op->sound_status = 0;
    op->index = 0;
}

void sf_opening_init(SfOpening *op)
{
    if (!op) {
        return;
    }
    memset(op, 0, sizeof *op);
}

void sf_opening_step(SfOpening *op)
{
    if (!op) {
        return;
    }
    if (op->r_no_0 == 0) {
        if (op->r_no_1 == 0) {
            init_screens(op);
        } else if (op->r_no_1 == 1) {
            init_wait(op);
        } else {
            init_countdown(op);
        }
        return;
    }
    opening_move(op);
}
