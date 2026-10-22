/* The mode menu's NETWORK page (in Xbox Live's place): a game with another
 * machine by its address. HOST MATCH waits for a player and shows this
 * machine's address; JOIN MATCH calls the address typed in (the pad's
 * up and down step a letter, left and right move along, A calls; a
 * keyboard types). INPUT DELAY is the frames each player's pad is late,
 * the host's for both. config/host.sqlite keeps the address and the
 * delay. The game itself is src/port/net.c. */

#include "port/port.h"

#include "types.h"
#include "structs.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void SSPutStr_Bigger(u16 x, u16 y, u8 atr, s8 *str, f32 sc, u8 gr, u16 priority);
void xb_ToneDownCopy(u8 a, u8 prio);
void Akaobi(void);
void SE_cursor_move(void);
void SE_selected(void);
void SE_dir_cursor_move(void);
extern u16 PLsw[2][2];

enum { COL_TEXT = 6, COL_CURSOR = 4, COL_VALUE = 9 };
enum { ROW_HOST, ROW_JOIN, ROW_ADDRESS, ROW_DELAY, ROW_EXIT, ROWS };
enum { PG_LIST, PG_EDIT, PG_HOSTING, PG_CALLING };

#define ADDRESS_MAX 46

static const char k_letters[] = "0123456789.:abcdefghijklmnopqrstuvwxyz-";

static struct {
    int open;
    int step;
    int row;
    int frame;
    char address[ADDRESS_MAX + 1];
    int caret;
    int delay;
    char mine[64];
    char note[96];
    int note_timer;
    int text_on;
} s_pg;

static void put(int x, int y, const char *s, int col)
{
    char buf[64];
    size_t i;
    for (i = 0; s[i] && i + 1 < sizeof buf; i++) {
        buf[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    }
    buf[i] = 0;
    SSPutStr_Bigger((u16)x, (u16)y, (u8)col, (s8 *)buf, 1.0f, 0, 1);
}

static void put_centred(int y, const char *s, int col)
{
    put((int)(192 - strlen(s) * 4), y, s, col);
}

static void note(const char *s)
{
    snprintf(s_pg.note, sizeof s_pg.note, "%s", s);
    s_pg.note_timer = 180;
}

static void text_input(int on)
{
    SDL_Window *w = sf_video_window();
    if (on == s_pg.text_on || !w) {
        return;
    }
    s_pg.text_on = on;
    if (on) {
        SDL_StartTextInput(w);
    } else {
        SDL_StopTextInput(w);
    }
}

static u16 pressed(void)
{
    int p;
    for (p = 0; p < 2; p++) {
        u16 sw = (u16)(~PLsw[p][1] & PLsw[p][0]);
        if (sw & 0x30F) {
            return sw;
        }
    }
    return 0;
}

static int letter_ix(char c)
{
    const char *at = strchr(k_letters, c);
    return at && c ? (int)(at - k_letters) : 0;
}

/* Typed on a keyboard (or a phone's) while the address is edited. */
void sf_net_page_text(const char *text)
{
    size_t n;
    if (!s_pg.open || s_pg.step != PG_EDIT) {
        return;
    }
    for (; *text; text++) {
        char c = *text >= 'A' && *text <= 'Z' ? (char)(*text + 32) : *text;
        if (!strchr(k_letters, c) || (n = strlen(s_pg.address)) >= ADDRESS_MAX) {
            continue;
        }
        memmove(s_pg.address + s_pg.caret + 1, s_pg.address + s_pg.caret, n - (size_t)s_pg.caret + 1);
        s_pg.address[s_pg.caret++] = c;
    }
}

static void start_call(void)
{
    if (!s_pg.address[0]) {
        note("TYPE THE OTHER MACHINE'S ADDRESS.");
        return;
    }
    sf_setting_write("net_address", s_pg.address);
    if (sf_net_join(s_pg.address) == 0) {
        s_pg.step = PG_CALLING;
    } else {
        note(sf_net_status());
    }
}

static void edit_input(u16 sw)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    static bool prev_back;
    static bool prev_enter;
    size_t n = strlen(s_pg.address);
    int back = k[SDL_SCANCODE_BACKSPACE] && !prev_back;
    int enter = (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) && !prev_enter;
    prev_back = k[SDL_SCANCODE_BACKSPACE];
    prev_enter = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];
    if (back && s_pg.caret > 0) {
        memmove(s_pg.address + s_pg.caret - 1, s_pg.address + s_pg.caret, n - (size_t)s_pg.caret + 1);
        s_pg.caret--;
        return;
    }
    if (enter) {
        sw = 0x100;
    }
    if (sw & 3) {
        /* up and down: the letter at the caret steps through the set */
        int len = (int)strlen(k_letters);
        int ix = s_pg.caret < (int)n ? letter_ix(s_pg.address[s_pg.caret]) : (sw & 1 ? len - 1 : 0);
        ix = (ix + (sw & 1 ? 1 : len - 1)) % len;
        if (s_pg.caret >= (int)n) {
            if (n >= ADDRESS_MAX) {
                return;
            }
            s_pg.address[n + 1] = 0;
        }
        s_pg.address[s_pg.caret] = k_letters[ix];
        SE_cursor_move();
    } else if (sw & 4) {
        if (s_pg.caret > 0) {
            s_pg.caret--;
            SE_cursor_move();
        }
    } else if (sw & 8) {
        if (s_pg.caret < (int)n) {
            s_pg.caret++;
            SE_cursor_move();
        }
    } else if (sw & 0x100) {
        SE_selected();
        s_pg.step = PG_LIST;
        text_input(0);
        start_call();
    } else if (sw & 0x200) {
        /* B rubs out the letter before the caret; on an empty line, back */
        if (s_pg.caret > 0) {
            memmove(s_pg.address + s_pg.caret - 1, s_pg.address + s_pg.caret, n - (size_t)s_pg.caret + 1);
            s_pg.caret--;
            SE_dir_cursor_move();
        } else {
            SE_selected();
            s_pg.step = PG_LIST;
            text_input(0);
        }
    }
}

static void draw(void)
{
    static const char *const label[ROWS] = { "HOST MATCH", "JOIN MATCH", "ADDRESS", "INPUT DELAY", "EXIT" };
    static const int y[ROWS] = { 56, 76, 96, 116, 148 };
    char line[96];
    int i;
    xb_ToneDownCopy(0xE8, 2);
    Akaobi();
    SSPutStr_Bigger(0x88, 0x16, 9, (s8 *)"NETWORK", 1.5f, 2, 1);
    for (i = 0; i < ROWS; i++) {
        int on = i == s_pg.row && s_pg.step == PG_LIST;
        put(64, y[i], label[i], on || (i == ROW_ADDRESS && s_pg.step == PG_EDIT) ? COL_CURSOR : COL_TEXT);
    }
    /* the address, with the caret while it is edited */
    put(176, y[ROW_ADDRESS], s_pg.address[0] ? s_pg.address : "-", COL_VALUE);
    if (s_pg.step == PG_EDIT && ((s_pg.frame >> 3) & 1)) {
        put(176 + s_pg.caret * 8, y[ROW_ADDRESS] + 6, "_", COL_CURSOR);
    }
    snprintf(line, sizeof line, "%d FRAME%s", s_pg.delay, s_pg.delay == 1 ? "" : "S");
    put(176, y[ROW_DELAY], line, COL_VALUE);

    switch (s_pg.step) {
    case PG_HOSTING:
        if ((s_pg.frame >> 4) & 1) {
            put_centred(172, "WAITING FOR A PLAYER", COL_CURSOR);
        }
        snprintf(line, sizeof line, "THIS MACHINE: %s  PORT %d", s_pg.mine, SF_NET_PORT);
        put_centred(188, line, COL_VALUE);
        put_centred(208, "B: CANCEL", COL_VALUE);
        break;
    case PG_CALLING:
        if ((s_pg.frame >> 4) & 1) {
            snprintf(line, sizeof line, "CALLING %s", s_pg.address);
            put_centred(180, line, COL_CURSOR);
        }
        put_centred(208, "B: CANCEL", COL_VALUE);
        break;
    case PG_EDIT:
        put_centred(188, "UP/DOWN: LETTER   LEFT/RIGHT: MOVE", COL_VALUE);
        put_centred(204, "A: CALL   B: RUB OUT", COL_VALUE);
        break;
    default:
        if (s_pg.note_timer > 0) {
            put_centred(188, s_pg.note, COL_CURSOR);
        }
        put_centred(208, "A: SELECT   B: BACK", COL_VALUE);
        break;
    }
}

/* From the mode menu (menu.c, patched): start 1 opens the page; then
 * each frame, 1 while it is open (it took the frame), 0 when closed. */
int sf_net_page(int start)
{
    u16 sw;
    if (start) {
        char v[ADDRESS_MAX + 1];
        memset(&s_pg, 0, sizeof s_pg);
        s_pg.open = 1;
        s_pg.delay = 2;
        if (sf_setting_read("net_address", v, sizeof v) == 0) {
            snprintf(s_pg.address, sizeof s_pg.address, "%s", v);
        }
        if (sf_setting_read("net_delay", v, sizeof v) == 0 && v[0] >= '0' && v[0] <= '6') {
            s_pg.delay = v[0] - '0';
        }
        sf_net_local_address(s_pg.mine, sizeof s_pg.mine);
        return 1;
    }
    sf_net_at_menu(); /* the mode menu runs: in a network game, it is over */
    if (!s_pg.open) {
        return 0;
    }
    if (sf_net_active()) {
        /* the game has begun: the menu is gone with the state it was in */
        s_pg.open = 0;
        text_input(0);
        return 0;
    }
    s_pg.frame++;
    if (s_pg.note_timer > 0) {
        s_pg.note_timer--;
    }
    sw = pressed();
    switch (s_pg.step) {
    case PG_EDIT:
        edit_input(sw);
        break;
    case PG_HOSTING:
    case PG_CALLING:
        if (!sf_net_waiting()) {
            /* the call failed or was turned down */
            s_pg.step = PG_LIST;
            note(sf_net_status());
        } else if (sw & 0x200) {
            SE_selected();
            sf_net_stop();
            s_pg.step = PG_LIST;
        }
        break;
    default:
        if (sw & 1) {
            s_pg.row = (s_pg.row + ROWS - 1) % ROWS;
            SE_cursor_move();
        } else if (sw & 2) {
            s_pg.row = (s_pg.row + 1) % ROWS;
            SE_cursor_move();
        } else if ((sw & 0xC) && s_pg.row == ROW_DELAY) {
            char v[4];
            s_pg.delay = (s_pg.delay + (sw & 4 ? 6 : 1)) % 7;
            snprintf(v, sizeof v, "%d", s_pg.delay);
            sf_setting_write("net_delay", v);
            SE_dir_cursor_move();
        } else if (sw & 0x200) {
            SE_selected();
            s_pg.open = 0;
            return 0;
        } else if (sw & 0x100) {
            SE_selected();
            switch (s_pg.row) {
            case ROW_HOST:
                if (sf_net_host_delay(SF_NET_PORT, s_pg.delay) == 0) {
                    s_pg.step = PG_HOSTING;
                } else {
                    note(sf_net_status());
                }
                break;
            case ROW_JOIN:
                start_call();
                break;
            case ROW_ADDRESS:
                s_pg.step = PG_EDIT;
                s_pg.caret = (int)strlen(s_pg.address);
                text_input(1);
                break;
            case ROW_EXIT:
                s_pg.open = 0;
                return 0;
            default:
                break;
            }
        }
        break;
    }
    draw();
    return 1;
}
