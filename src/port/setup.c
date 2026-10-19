/* The first start: no game data yet. The window shows where to get it
 * (the Xbox disc as an image, a zip or 7z of it, or the extracted folder:
 * picked with a click or Enter, F for a folder, or dropped on the window),
 * imports it into data/ next to the program (src/iso/extract.c: the
 * archive, the opening theme, the fonts and the 21 sound banks out of
 * sf33rd.xbe), and the game then starts in the same window.
 *
 * The command line can do the same: --extract SOURCE --data DIR, and
 * --sound-banks sf33rd.xbe --data DIR for data imported before the sound
 * banks were. */

#include "common.h"
#include "host/paths.h"
#include "iso/archive.h"
#include "iso/extract.h"
#include "iso/xbe.h"
#include "port/port.h"
#include "ui/ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const SDL_DialogFileFilter k_filters[] = {
    { "Xbox disc or archive", "iso;zip;7z" },
    { "All files", "*" },
};

static struct {
    SfCanvas canvas;
    SfUi ui;
    char status[512];
    char note[512];
    SDL_Mutex *lock;
    char chosen[4096];
    int chosen_ready;
    int dialog_open;
} s;

static void draw(void)
{
    sf_ui_draw(&s.canvas, &s.ui);
    sf_video_present_canvas(&s.canvas);
}

static void on_file(void *user, const char *const *files, int filter)
{
    (void)user;
    (void)filter;
    SDL_LockMutex(s.lock);
    s.dialog_open = 0;
    if (files && files[0]) {
        snprintf(s.chosen, sizeof s.chosen, "%s", files[0]);
        s.chosen_ready = 1;
    }
    SDL_UnlockMutex(s.lock);
}

/* each file the import writes: its name on the screen */
static void progress(const char *path, void *user)
{
    const char *base = path ? strrchr(path, '/') : NULL;
    (void)user;
    base = base ? base + 1 : path;
    snprintf(s.status, sizeof s.status, "READING %s", base ? base : "");
    draw();
    SDL_PumpEvents();
}

static int import(const char *install, const char *source)
{
    SfExtractResult result;
    snprintf(s.status, sizeof s.status, "READING XBOX DATA");
    s.note[0] = 0;
    draw();
    if (sf_extract(source, install, NULL, progress, NULL, &result) == 0 && sf_data_ready(install)) {
        return 0;
    }
    snprintf(s.status, sizeof s.status, "%s", result.error[0] ? result.error : "XBOX DATA WAS NOT READ");
    return -1;
}

int sf_setup_run(const char *install)
{
    SDL_Window *win = sf_video_window();
    int done = 0;
    if (sf_canvas_open(&s.canvas, 960, 540) != 0) {
        return -1;
    }
    s.lock = SDL_CreateMutex();
    s.ui.status = s.status;
    s.ui.note = s.note;
    if (!sf_sound_banks_ready(install)) {
        char afs[4096];
        if (sf_join(afs, sizeof afs, install, "data/afs/SF33RDXB.AFS") == 0 && access(afs, R_OK) == 0) {
            /* imported before the sound effects were read from the XBE */
            snprintf(s.note, sizeof s.note, "IMPORT THE DISC AGAIN: THE SOUND EFFECTS COME FROM SF33RD.XBE");
        }
    }
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !s.dialog_open)) {
                done = -1;
            } else if (!s.dialog_open && ((ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
                                           (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER)) ||
                                          ev.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
                s.dialog_open = 1;
                SDL_ShowOpenFileDialog(on_file, NULL, win, k_filters, 2, NULL, false);
            } else if (!s.dialog_open && ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F) {
                s.dialog_open = 1;
                SDL_ShowOpenFolderDialog(on_file, NULL, win, NULL, false);
            } else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
                SDL_LockMutex(s.lock);
                snprintf(s.chosen, sizeof s.chosen, "%s", ev.drop.data);
                s.chosen_ready = 1;
                SDL_UnlockMutex(s.lock);
            }
        }
        if (done) {
            break;
        }
        SDL_LockMutex(s.lock);
        if (s.chosen_ready) {
            char path[4096];
            snprintf(path, sizeof path, "%s", s.chosen);
            s.chosen_ready = 0;
            SDL_UnlockMutex(s.lock);
            if (import(install, path) == 0) {
                done = 1;
            }
        } else {
            SDL_UnlockMutex(s.lock);
        }
        draw();
        SDL_Delay(16);
    }
    sf_canvas_close(&s.canvas);
    SDL_DestroyMutex(s.lock);
    return done > 0 ? 0 : -1;
}

/* --extract SOURCE --data DIR, --sound-banks XBE --data DIR: -1 when the
 * command line asks for neither, else the exit status. */
int sf_setup_cli(int argc, char **argv)
{
    /* SF_SETUP_SHOT=PATH: the first start's screen as a PPM, no window
     * (for the README) */
    if (getenv("SF_SETUP_SHOT")) {
        SfCanvas c;
        SfUi ui;
        FILE *f;
        int i;
        memset(&ui, 0, sizeof ui);
        if (sf_canvas_open(&c, 960, 540) != 0) {
            return 1;
        }
        sf_ui_draw(&c, &ui);
        f = fopen(getenv("SF_SETUP_SHOT"), "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", c.width, c.height);
            for (i = 0; i < c.width * c.height; i++) {
                fwrite(&c.rgba[i * 4], 1, 3, f);
            }
            fclose(f);
        }
        sf_canvas_close(&c);
        return f ? 0 : 1;
    }
    const char *source = NULL;
    const char *dir = NULL;
    const char *xbe = NULL;
    SfExtractResult result;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            source = argv[++i];
        } else if (strcmp(argv[i], "--sound-banks") == 0 && i + 1 < argc) {
            xbe = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: sf33rdsxb                         play (the first time, import the disc)\n"
                            "       sf33rdsxb --extract SOURCE --data DIR\n"
                            "       sf33rdsxb --sound-banks sf33rd.xbe --data DIR\n");
            return 0;
        }
    }
    if (!source && !dir && !xbe) {
        return -1;
    }
    if (xbe) {
        char out[4096];
        char err[256];
        err[0] = 0;
        if (!dir || sf_join(out, sizeof out, dir, "data/audio/sdbk") != 0) {
            fprintf(stderr, "usage: sf33rdsxb --sound-banks sf33rd.xbe --data DIR\n");
            return 1;
        }
        if (sf_mkdir_p(out, err, sizeof err) != 0 || sf_xbe_sound_banks(xbe, out, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        printf("sound banks %d\n", SF_SOUND_BANKS);
        return 0;
    }
    if (!source || !dir) {
        fprintf(stderr, "usage: sf33rdsxb --extract SOURCE --data DIR\n");
        return 1;
    }
    if (sf_extract(source, dir, NULL, NULL, NULL, &result) != 0) {
        fprintf(stderr, "%s\n", result.error);
        return 1;
    }
    printf("files %d\n", result.files);
    if (result.note[0]) {
        printf("%s\n", result.note);
    }
    return sf_data_ready(dir) ? 0 : 1;
}
