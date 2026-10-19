/* The port's additions to the game's menus, drawn with the game's own
 * pieces (its tone-down, the training menu's red band, its menu font and
 * menu sounds) so they look like the rest of the game.
 *
 * - The mode menu's SELECT GAME (retail's way back to the Anniversary
 *   Collection's game list) is EXIT GAME: its prompt asks about quitting,
 *   and Reboot_Program, where retail leaves, quits the port cleanly.
 * - REPLAY's save and load pick one of ten slots on a page of their own
 *   (sf_replay_page, run by SaveMove), read with the game's pad input.
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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const s8 *Menu_Letter_Data[2][70]; /* 67 to 69: the port's window rows */
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

/* ---- retail's debug-font print, in the menu font ---- */

/* Retail prints a few messages (the pause screen's "Please reconnect the
 * controller") with its debug font at 640x448 positions, in cells cw by ch
 * pixels. Here the same text is the menu font on the 384x224 screen. */
static struct {
    int x, y; /* 640x448 */
    int prio;
} s_pr = { 0, 0, 1 };

void xb_PrintSetAttr(s16 attr)
{
    (void)attr;
}

void xb_PrintSetScale(f32 z)
{
    int i;
    s_pr.prio = 1;
    for (i = 0; i < 8; i++) {
        if (PrioBase[i] == z) {
            s_pr.prio = i;
        }
    }
}

void xb_PrintSetColor(s32 cw, s32 ch)
{
    (void)cw;
    (void)ch;
}

void xb_PrintSetPos(s16 x, s16 y)
{
    s_pr.x = x;
    s_pr.y = y;
}

s32 xb_PrintString(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    float sc;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* the menu font, 8 pixels a letter on the 384-wide screen, fits the
     * boxes retail draws round its 22-pixel cells (13 here) */
    sc = 1.0f;
    put_text(s_pr.x * 384 / 640, s_pr.y * 224 / 448, buf, sc, COL_TEXT, s_pr.prio);
    return (s32)strlen(buf);
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

/* ---- the replay slots ---- */

/* The characters by the game's index (My_char). */
static const char *const k_char_name[20] = { "GILL",  "ALEX", "RYU",  "YUN",   "DUDLEY",  "NECRO",  "HUGO",
                                             "IBUKI", "ELENA", "ORO", "YANG",  "KEN",     "SEAN",   "URIEN",
                                             "AKUMA", "CHUN-LI", "MAKOTO", "Q", "TWELVE", "REMY" };

extern u16 PLsw[2][2];
extern u8 D_007AA5EF;

enum { RP_LIST, RP_CONFIRM, RP_DONE };

static struct {
    int save;
    int step;
    int row;
    int timer;
    int has[SF_REPLAY_SLOTS];
    int chars[SF_REPLAY_SLOTS][2];
    long long when[SF_REPLAY_SLOTS];
    const char *note; /* a line under the list for a while */
    int note_timer;
} s_rp;

static const char *char_name(int c)
{
    return c >= 0 && c < 20 ? k_char_name[c] : "?";
}

static void rp_scan(void)
{
    int i;
    for (i = 0; i < SF_REPLAY_SLOTS; i++) {
        s_rp.has[i] = sf_replay_slot(i, s_rp.chars[i], &s_rp.when[i]);
    }
}

/* The pads' new presses, as the game's menus read them, and who pressed. */
static u16 rp_pressed(int *who)
{
    int p;
    for (p = 0; p < 2; p++) {
        u16 sw = (u16)(~PLsw[p][1] & PLsw[p][0]);
        if (sw & 0x303) {
            *who = p;
            return sw;
        }
    }
    return 0;
}

static void rp_draw(int frame)
{
    int i;
    xb_ToneDownCopy(0xE8, 2);
    Akaobi();
    put_text(0x68, 0x16, s_rp.save ? "SAVE REPLAY" : "LOAD REPLAY", 1.5f, COL_TITLE, 1);
    for (i = 0; i < SF_REPLAY_SLOTS; i++) {
        int y = 50 + i * 15;
        int on = i == s_rp.row;
        int col = on ? COL_CURSOR : COL_TEXT;
        char num[4];
        char line[48];
        snprintf(num, sizeof num, "%02d", i + 1);
        put_text(32, y, num, 1.0f, col, 1);
        if (s_rp.has[i] > 0) {
            char date[24];
            time_t t = (time_t)s_rp.when[i];
            struct tm tm;
            snprintf(line, sizeof line, "%s VS %s", char_name(s_rp.chars[i][0]), char_name(s_rp.chars[i][1]));
            put_text(64, y, line, 1.0f, col, 1);
            if (t && localtime_r(&t, &tm)) {
                strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
                put_text(232, y, date, 1.0f, on ? COL_CURSOR : COL_VALUE, 1);
            }
        } else {
            put_text(64, y, s_rp.has[i] < 0 ? "DAMAGED DATA" : "NO DATA", 1.0f, on ? COL_CURSOR : COL_VALUE, 1);
        }
    }
    if (s_rp.step == RP_CONFIRM) {
        if ((frame >> 4) & 1) {
            put_centred(204, "OVERWRITE THIS REPLAY ?", 1.0f, COL_CURSOR, 1);
        }
        put_centred(214, "YES: A   NO: B", 1.0f, COL_VALUE, 1);
    } else if (s_rp.note_timer > 0) {
        put_centred(208, s_rp.note, 1.0f, COL_CURSOR, 1);
    } else {
        put_centred(208, s_rp.save ? "A: SAVE   B: BACK" : "A: LOAD   B: BACK", 1.0f, COL_VALUE, 1);
    }
}

int sf_replay_page(int start, int save)
{
    static int frame;
    int who = 0;
    u16 sw;
    if (start) {
        memset(&s_rp, 0, sizeof s_rp);
        s_rp.save = save;
        rp_scan();
        /* loading starts on the first replay there is */
        if (!save) {
            int i;
            for (i = 0; i < SF_REPLAY_SLOTS && s_rp.has[i] <= 0; i++) {
            }
            s_rp.row = i < SF_REPLAY_SLOTS ? i : 0;
        }
        return 1;
    }
    frame++;
    if (s_rp.note_timer > 0) {
        s_rp.note_timer--;
    }
    if (s_rp.step == RP_DONE) {
        /* "REPLAY SAVED." stays a moment before the menu goes on */
        rp_draw(frame);
        return --s_rp.timer > 0 ? 1 : 0;
    }
    sw = rp_pressed(&who);
    if (s_rp.step == RP_CONFIRM) {
        if (sw & 0x100) {
            s_rp.step = RP_LIST;
            if (sf_replay_save(s_rp.row) == 0) {
                SE_selected();
                s_rp.note = "REPLAY SAVED.";
                s_rp.step = RP_DONE;
                s_rp.timer = 60;
                rp_scan();
            } else {
                s_rp.note = "THE REPLAY COULD NOT BE SAVED.";
                s_rp.note_timer = 120;
            }
        } else if (sw & 0x200) {
            SE_dir_cursor_move();
            s_rp.step = RP_LIST;
        }
        rp_draw(frame);
        return 1;
    }
    if (sw & 1) {
        s_rp.row = (s_rp.row + SF_REPLAY_SLOTS - 1) % SF_REPLAY_SLOTS;
        SE_cursor_move();
    } else if (sw & 2) {
        s_rp.row = (s_rp.row + 1) % SF_REPLAY_SLOTS;
        SE_cursor_move();
    } else if (sw & 0x200) {
        SE_selected();
        return -1;
    } else if (sw & 0x100) {
        if (save) {
            if (s_rp.has[s_rp.row] != 0) {
                SE_selected();
                s_rp.step = RP_CONFIRM;
            } else if (sf_replay_save(s_rp.row) == 0) {
                SE_selected();
                s_rp.note = "REPLAY SAVED.";
                s_rp.step = RP_DONE;
                s_rp.timer = 60;
                rp_scan();
            } else {
                s_rp.note = "THE REPLAY COULD NOT BE SAVED.";
                s_rp.note_timer = 120;
            }
        } else if (s_rp.has[s_rp.row] > 0 && sf_replay_load(s_rp.row) == 0) {
            SE_selected();
            D_007AA5EF = (u8)who;
            return 0;
        } else {
            SE_dir_cursor_move();
            s_rp.note = s_rp.has[s_rp.row] < 0 ? "THIS REPLAY IS DAMAGED." : "THERE IS NO REPLAY HERE.";
            s_rp.note_timer = 90;
        }
    }
    rp_draw(frame);
    return 1;
}
