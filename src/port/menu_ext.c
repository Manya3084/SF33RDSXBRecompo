/* The port's additions to the game's menus, drawn with the game's own
 * pieces (its tone-down, the training menu's red band, its menu font and
 * menu sounds) so they look like the rest of the game.
 *
 * - The mode menu's SELECT GAME (retail's way back to the Anniversary
 *   Collection's game list) is EXIT GAME: its prompt asks about quitting,
 *   and Reboot_Program, where retail leaves, quits the port cleanly.
 * - BUTTON CONFIG has a keyboard page: F1 opens it over the screen, each
 *   Xbox button's key is shown and can be changed, and the keys are kept
 *   in config/keyboard.txt. While it is open the game sees nothing
 *   pressed, so a session recording stays a faithful replay. */

#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <SDL3/SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const s8 *Menu_Letter_Data[2][67];
extern struct _TASK task[11];
extern u8 No_Trans;
extern const s16 Fade_Pos_tbl[8];
extern f32 PrioBase[];

void njColorBlendingMode(s32 mode, s32 blend);
void njDrawPolygon2D(PAL_CURSOR *poly, s32 n, f32 pri, s32 attr, s32 kind);
void SSPutStr_Bigger(u16 x, u16 y, u8 atr, s8 *str, f32 sc, u8 gr, u16 priority);
void Akaobi(void);
void SE_cursor_move(void);
void SE_selected(void);
void SE_dir_cursor_move(void);

/* The menu font's palettes: 6 white, 4 yellow, 9 the light blue of the
 * game's PRESS START lines. COL_TITLE is 9 with the training menu's
 * title shading. */
enum { COL_TEXT = 6, COL_CURSOR = 4, COL_VALUE = 9, COL_TITLE = 0x109 };

/* The game's 384-pixel-wide screen; the menu font is 8 pixels a letter.
 * COL_TITLE is palette 9 with the training menu's title shading. */
static void put_text(int x, int y, const char *s, float sc, int col, int prio)
{
    char buf[64];
    size_t i;
    for (i = 0; s[i] && i + 1 < sizeof buf; i++) {
        buf[i] = (char)toupper((unsigned char)s[i]);
    }
    buf[i] = 0;
    if (col == COL_TITLE) {
        SSPutStr_Bigger((u16)x, (u16)y, 9, (s8 *)buf, sc, 2, (u16)prio);
    } else {
        SSPutStr_Bigger((u16)x, (u16)y, (u8)col, (s8 *)buf, sc, 0, (u16)prio);
    }
}

static void put_centred(int y, const char *s, float sc, int col, int prio)
{
    put_text((int)(192.0f - (float)strlen(s) * 4.0f * sc), y, s, sc, col, prio);
}

/* ---- retail's tone-down, as the pause and training menus draw it ---- */

/* Darkens the screen: black with alpha a at PrioBase[prio] (retail's
 * xb_ToneDownCopy is the game's ToneDown with polygon kind 2). */
void xb_ToneDownCopy(u8 a, u8 prio)
{
    PAL_CURSOR pc;
    PAL_CURSOR_P p[4];
    PAL_CURSOR_COL c[4];
    int i;
    if (No_Trans) {
        return;
    }
    njColorBlendingMode(0, 1);
    pc.p = p;
    pc.col = c;
    pc.tex = NULL;
    pc.num = 4;
    for (i = 0; i < 4; i++) {
        c[i].color = (u32)a << 24;
        p[i].x = (f32)Fade_Pos_tbl[i * 2];
        p[i].y = (f32)Fade_Pos_tbl[i * 2 + 1];
    }
    njDrawPolygon2D(&pc, 4, PrioBase[prio], 0x60, 2);
}

/* ---- EXIT GAME ---- */

/* A key's name as the menu font shows it, at most n - 1 letters. */
static void key_name(int key, char *out, size_t n)
{
    const char *name = SDL_GetScancodeName((SDL_Scancode)key);
    snprintf(out, n, "%s", name && *name ? name : "NONE");
}

/* "A (Z)": the Xbox button and the keyboard key that presses it. */
static void button_with_key(const char *button, int row, char *out, size_t n)
{
    char k[24];
    key_name(sf_keys_get(row), k, sizeof k);
    snprintf(out, n, "%s (%s)", button, k);
}

/* Retail prints these with its debug font and the A and B button
 * pictures; here they are the menu font. Type 0 is the mode menu's last
 * entry (EXIT GAME here), 1 the return to the title, 2 a disc error. */
void xb_DrawSystemMessage(s32 type, s32 x, s32 y, s32 cw, s32 ch, s32 lh)
{
    char yes[48];
    char no[48];
    char line[128];
    (void)x;
    (void)y;
    (void)cw;
    (void)ch;
    (void)lh;
    button_with_key("A", 6, yes, sizeof yes);
    button_with_key("B", 7, no, sizeof no);
    snprintf(line, sizeof line, "YES: %s   NO: %s", yes, no);
    switch (type) {
    case 0:
        put_centred(88, "EXIT THE GAME.", 1.0f, COL_TEXT, 1);
        put_centred(102, "IS THIS OK ?", 1.0f, COL_TEXT, 1);
        put_centred(124, line, 1.0f, COL_TEXT, 1);
        break;
    case 1:
        put_centred(80, "RETURNING TO TITLE SCREEN.", 1.0f, COL_TEXT, 1);
        put_centred(94, "ALL UNSAVED DATA WILL BE LOST.", 1.0f, COL_TEXT, 1);
        put_centred(108, "IS THIS OK ?", 1.0f, COL_TEXT, 1);
        put_centred(130, line, 1.0f, COL_TEXT, 1);
        break;
    default:
        put_centred(90, "THE GAME DATA COULD NOT BE READ.", 1.0f, COL_TEXT, 1);
        put_centred(104, "IMPORT THE DISC AGAIN.", 1.0f, COL_TEXT, 1);
        break;
    }
}

/* Where retail starts the next program, the port ends. */
s32 Reboot_Program(const char *path)
{
    (void)path;
    sf_port_quit();
    return 0;
}

/* Retail's exit screen and its last frames before it leaves
 * (xb_ExitToSelectGame draws them): the port just ends. */
void xb_LoadExitScreen(void)
{
}

void xb_ResetDeviceOneBackBuffer(void)
{
}

void xb_ApplyDisplaySize(void)
{
}

void xb_DrawExitScreen(void)
{
}

void xb_DrawButtonImages(void)
{
}

void xb_EndFrame(s32 a)
{
    (void)a;
}

/* ---- the keyboard page ---- */

/* Laid out as BUTTON CONFIG is, in two columns: the d-pad, START and
 * BACK (bindings 0 to 5) on the left, the eight buttons (6 to 13) on the
 * right, and DEFAULT SETTING and EXIT under them. */
enum { ROW_DEFAULT = 14, ROW_EXIT = 15 };
#define LEFT_ROWS 6
#define RIGHT_ROWS 8
#define ROW_Y(r) (50 + (r) * 16)
#define BOTTOM_Y 186

static int s_open;
static int s_row;
static int s_waiting; /* for the key of s_row */
static int s_hold;    /* blocking on after closing until every key is up */
static bool s_prev[SDL_SCANCODE_COUNT];

/* BUTTON CONFIG from the mode menu's OPTION: the menu task (3) in
 * After_Title, Button_Config, taking input. */
static int on_button_config(void)
{
    const struct _TASK *t = &task[3];
    return t->condition != 0 && t->r_no[0] == 0 && t->r_no[1] == 10 && t->r_no[2] == 3;
}

static int any_key_down(const bool *k)
{
    int i;
    for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (k[i]) {
            return 1;
        }
    }
    return 0;
}

/* The first key pressed this frame, or -1. */
static int key_pressed(const bool *k)
{
    int i;
    for (i = 1; i < SDL_SCANCODE_COUNT; i++) {
        if (k[i] && !s_prev[i]) {
            return i;
        }
    }
    return -1;
}

static void page_close(void)
{
    s_open = 0;
    s_waiting = 0;
    s_hold = 1;
}

/* The cursor's next row for a direction: up and down within a column
 * (the bottom row under each), left and right across. */
static int row_step(int row, SDL_Scancode dir)
{
    int right = row >= LEFT_ROWS && row < ROW_DEFAULT;
    int r = right ? row - LEFT_ROWS : row;
    switch (dir) {
    case SDL_SCANCODE_UP:
        if (row == ROW_DEFAULT) {
            return LEFT_ROWS - 1;
        }
        if (row == ROW_EXIT) {
            return LEFT_ROWS + RIGHT_ROWS - 1;
        }
        if (r == 0) {
            return right ? ROW_EXIT : ROW_DEFAULT;
        }
        return row - 1;
    case SDL_SCANCODE_DOWN:
        if (row == ROW_DEFAULT) {
            return 0;
        }
        if (row == ROW_EXIT) {
            return LEFT_ROWS;
        }
        if (r == (right ? RIGHT_ROWS : LEFT_ROWS) - 1) {
            return right ? ROW_EXIT : ROW_DEFAULT;
        }
        return row + 1;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
        if (row == ROW_DEFAULT) {
            return ROW_EXIT;
        }
        if (row == ROW_EXIT) {
            return ROW_DEFAULT;
        }
        return right ? (r < LEFT_ROWS ? r : LEFT_ROWS - 1) : LEFT_ROWS + r;
    default:
        return row;
    }
}

static void page_input(const bool *k)
{
    int key = key_pressed(k);
    if (key < 0) {
        return;
    }
    if (s_waiting) {
        if (key == SDL_SCANCODE_ESCAPE) {
            s_waiting = 0; /* keep the old key */
            SE_dir_cursor_move();
        } else if (key != SDL_SCANCODE_F1) {
            sf_keys_set(s_row, key);
            sf_keys_save();
            s_waiting = 0;
            SE_selected();
        }
        return;
    }
    switch (key) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
        s_row = row_step(s_row, (SDL_Scancode)key);
        SE_cursor_move();
        break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        SE_selected();
        if (s_row == ROW_EXIT) {
            page_close();
        } else if (s_row == ROW_DEFAULT) {
            sf_keys_default();
            sf_keys_save();
        } else {
            s_waiting = 1;
        }
        break;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_F1:
        SE_selected();
        page_close();
        break;
    default:
        break;
    }
}

/* One binding: its label, then its key (or the prompt while it waits). */
static void draw_binding(int i, int x, int y, int key_x, int frame)
{
    char k[16];
    int on = i == s_row;
    put_text(x, y, sf_keys_label(i), 1.0f, on ? COL_CURSOR : COL_TEXT, 1);
    if (on && s_waiting) {
        if ((frame >> 4) & 1) {
            put_text(key_x, y, "PRESS A KEY", 1.0f, COL_CURSOR, 1);
        }
        return;
    }
    key_name(sf_keys_get(i), k, sizeof k);
    put_text(key_x, y, k, 1.0f, on ? COL_CURSOR : COL_VALUE, 1);
}

static void page_draw(int frame)
{
    int i;
    xb_ToneDownCopy(0xE8, 2);
    Akaobi();
    put_text(0x58, 0x16, "KEYBOARD CONFIG", 1.5f, COL_TITLE, 1);
    for (i = 0; i < LEFT_ROWS; i++) {
        draw_binding(i, 24, ROW_Y(i), 80, frame);
    }
    for (i = 0; i < RIGHT_ROWS; i++) {
        draw_binding(LEFT_ROWS + i, 192, ROW_Y(i), 272, frame);
    }
    put_text(24, BOTTOM_Y, "DEFAULT SETTING", 1.0f, s_row == ROW_DEFAULT ? COL_CURSOR : COL_TEXT, 1);
    put_text(192, BOTTOM_Y, "EXIT", 1.0f, s_row == ROW_EXIT ? COL_CURSOR : COL_TEXT, 1);
    put_centred(208, s_waiting ? "ESC: CANCEL" : "ENTER: CHANGE   ESC: BACK", 1.0f, COL_VALUE, 1);
}

void sf_menu_ext_init(void)
{
    Menu_Letter_Data[0][7] = (const s8 *)"EXIT GAME";
    Menu_Letter_Data[1][7] = (const s8 *)"EXIT GAME";
}

/* Each frame, after the game's tasks and before the frame is drawn. */
void sf_menu_ext_frame(void)
{
    static int frame;
    const bool *k = SDL_GetKeyboardState(NULL);
    int here = on_button_config();
    frame++;
    if (s_open && !here) {
        page_close();
    }
    if (s_open) {
        page_input(k);
        if (s_open) {
            page_draw(frame);
        }
    } else if (here) {
        put_centred(214, "F1: KEYBOARD CONFIG", 1.0f, COL_VALUE, 1);
        /* SF_KEYPAGE=FRAME opens it on that frame (tests: no keyboard) */
        const char *test = getenv("SF_KEYPAGE");
        if ((k[SDL_SCANCODE_F1] && !s_prev[SDL_SCANCODE_F1] && !sf_input_replaying()) ||
            (test && atoi(test) == sf_port_frame)) {
            s_open = 1;
            s_row = 0;
            s_waiting = 0;
            SE_selected();
        }
    }
    if (s_hold && !any_key_down(k)) {
        s_hold = 0;
    }
    sf_input_block(s_open || s_hold);
    memcpy(s_prev, k, sizeof s_prev);
}
