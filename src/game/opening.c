#include "game/opening.h"

#include <string.h>

/* 0xAF4C2 is Bg_Off. 0xAF4AA is Bg_On. 101 and 0x65 are the same status.
   Background 0 walks layer 2 until status 11. op_work_clear then lets
   background 1 arm layer 2 back to 0x200 and stop. effect_F6 steps in
   Basic_Sub, before opening_move. 0xEF088 is add_y_sub. 0xEF072 is add_x_sub.
   Go_BGM stays out. Time 184 would post status 9 and op_102 would enter
   index 3. That pair stays unposted. */

/* Posted through the status 8 that follows 102. The next pair is time 184, status 9. */
static const int16_t k_time[24] = {
    4, 11, 19, 26, 34, 41, 49, 56, 64, 71, 79, 86, 94, 101, 109, 116,
    124, 131, 139, 146, 154, 161, 169, 176
};
static const int16_t k_trg[24] = {
    101, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    102, 2, 3, 4, 5, 6, 7, 8
};

/* op_quake_y_tbl0. op_bg0_0001 adds entry 1 on the arm frame, then walks. */
static const int16_t k_quake[16] = {4, -8, 2, 1, -6, -3, 9, -3, 8, -2, 6, 3, -4, -9, 3, -1};

static void bg_off(SfOpening *op, uint16_t bits)
{
    op->screen = (uint16_t)(op->screen & (uint16_t)~bits);
}

static void quake_bg0(SfOpening *op)
{
    op->quake_i = (int16_t)((op->quake_i + 1) & 15);
    op->layer_y[0] += k_quake[op->quake_i];
}

static void move_bg0(SfOpening *op, int r_index)
{
    if (r_index != 1) {
        if (op->layer_r[0] != 0) {
            return;
        }
        op->layer_r[0] = 1;
        op->layer_x[0] = 0x02000000;
        op->layer_y[0] = 0;
        bg_off(op, 1);
        return;
    }
    /* op_bg0_0001 case 0 falls through into the quake. Later calls stay there. */
    if (op->layer_r[0] == 0) {
        op->layer_r[0] = 1;
        op->layer_x[0] = 0x02000000;
        op->layer_y[0] = 0;
        op->quake_i = 0;
        op->screen = (uint16_t)(op->screen | 1u);
    }
    quake_bg0(op);
}

static void move_bg1(SfOpening *op)
{
    if (op->layer_r[1] != 0) {
        return;
    }
    op->layer_r[1] = 1;
    op->layer_x[1] = 0x02000000;
    op->layer_y[1] = 0;
    bg_off(op, 2);
}

static void move_bg2(SfOpening *op, int r_index)
{
    if (op->layer_r[2] == 0) {
        op->layer_r[2] = 1;
        op->layer_x[2] = 0x02000000;
        op->layer_y[2] = 0;
        return;
    }
    if (r_index == 0 && op->layer_r[2] == 1) {
        op->layer_x[2] -= 0x10000;
    }
    /* op_bg2_0002 case 1. The arm frame does not fall through into this add. */
    if (r_index == 2 && op->layer_r[2] == 1) {
        op->layer_y[2] += 0x10000;
    }
}

static void move_background(SfOpening *op, int r_index)
{
    move_bg0(op, r_index);
    move_bg1(op);
    move_bg2(op, r_index);
}

static void work_clear(SfOpening *op)
{
    op->layer_r[0] = 0;
    op->layer_r[1] = 0;
    op->layer_r[2] = 0;
}

static void sound_trigger(SfOpening *op)
{
    if (op->scene < 0 || op->scene >= 24) {
        return;
    }
    if (op->timer >= k_time[op->scene]) {
        op->status = k_trg[op->scene];
        op->scene = (int16_t)(op->scene + 1);
    }
}

static void step_f6(SfOpening *op, int which, int32_t speed, int16_t limit, int toward_low)
{
    int16_t pos;
    if (op->f6_stop[which]) {
        return;
    }
    op->f6_cal[which] += speed;
    pos = (int16_t)((uint32_t)op->f6_cal[which] >> 16);
    if (toward_low) {
        if (pos <= limit) {
            op->f6_cal[which] = (int32_t)limit * 65536;
            pos = limit;
            op->f6_stop[which] = 1;
        }
    } else if (pos >= limit) {
        op->f6_cal[which] = (int32_t)limit * 65536;
        pos = limit;
        op->f6_stop[which] = 1;
    }
    op->f6_y[which] = pos;
}

/* Direction 0x10. 98304 is 1.5 pixels in 16.16. The high word is the position. */
static void step_line(SfOpening *op)
{
    int16_t pos;
    if (!op->line_live || op->line_stop) {
        return;
    }
    op->line_cal += 98304;
    pos = (int16_t)((uint32_t)op->line_cal >> 16);
    if (pos >= 538) {
        op->line_cal = 538 * 65536;
        pos = 538;
        op->line_stop = 1;
    }
    op->line_x = pos;
}

/* Basic_Sub runs before Title, so a live effect steps on the index from last frame. */
static void effects_before(SfOpening *op)
{
    if (op->index >= 2 && op->line_live) {
        step_line(op);
        op->show_next = 0;
        op->show_line = 1;
        return;
    }
    if (!op->f6_live) {
        return;
    }
    if (op->index >= 1 && op->obj_disp) {
        step_f6(op, 1, 131072, -136, 0);
        op->show_next = 1;
    } else {
        step_f6(op, 0, -32768, -32, 1);
    }
}

static void op_101(SfOpening *op)
{
    if (op->r_no_2 == 0) {
        op->r_no_2 = 1;
        move_background(op, 0);
        op->obj_disp = 0;
        op->f6_cal[0] = 16 * 65536;
        op->f6_y[0] = 16;
        op->f6_cal[1] = (int32_t)-224 * 65536;
        op->f6_y[1] = -224;
        op->f6_live = 1;
        step_f6(op, 0, -32768, -32, 1);
        return;
    }
    if (op->r_no_2 == 1) {
        if (op->status >= 11 && op->status != 101) {
            op->r_no_2 = 2;
            op->index = 1;
            op->obj_disp = 1;
            work_clear(op);
            return;
        }
        move_background(op, 0);
        return;
    }
    move_background(op, 1);
}

/* op_102_move case 0. Index is already 2, so type 2 steps on this frame.
   Types 3 and 4 stay put until index 3 and 4. Status 9 would leave this case. */
static void op_102(SfOpening *op)
{
    if (op->r_no_2 == 0) {
        op->r_no_2 = 1;
        op->index = 2;
        op->obj_disp = 0;
        op->show_next = 0;
        op->show_line = 1;
        op->line_cal = 448 * 65536;
        op->line_x = 448;
        op->line_stop = 0;
        op->line_live = 1;
        step_line(op);
        work_clear(op);
        move_background(op, 2);
        return;
    }
    move_background(op, 2);
}

static void op_100(SfOpening *op)
{
    op->r_no_1 = (int8_t)(op->r_no_1 + 1);
    op_101(op);
}

static void opening_move(SfOpening *op)
{
    op->timer = (int16_t)(op->timer + 1);
    effects_before(op);
    sound_trigger(op);
    if (op->r_no_1 == 1 && op->status != op->sound_status && op->status == 102) {
        op->r_no_1 = 2;
        op->r_no_2 = 0;
        work_clear(op);
    }
    if (op->r_no_1 == 0) {
        op_100(op);
    } else if (op->r_no_1 == 1) {
        op_101(op);
    } else {
        op_102(op);
    }
    op->sound_status = op->status;
    if (op->status == 101 || (op->status != 0 && op->r_no_2 >= 1)) {
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
