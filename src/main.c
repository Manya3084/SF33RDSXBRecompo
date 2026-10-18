#include "common.h"
#include "game/adx.h"
#include "game/fs.h"
#include "game/tracks.h"
#include "game/xwb.h"
#include "game/memman.h"
#include "game/ppg.h"
#include "game/task.h"
#include "game/title.h"
#include "mem/heap.h"
#include "host/paths.h"
#include "host/settings.h"
#include "iso/extract.h"
#include "platform/audio.h"
#include "platform/input.h"
#include "render/renderer.h"
#include "ui/ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *k_title = "Street Fighter III: 3rd Strike";

static const SDL_DialogFileFilter k_filters[] = {
    {"Xbox disc or archive", "iso;zip;7z"},
    {"All files", "*"},
};

typedef struct App {
    SDL_Window *window;
    SfRenderer *renderer;
    SfSettings settings;
    SfCanvas canvas;
    SfUi ui;
    char install[4096];
    char tool[4096];
    char status[512];
    char note[512];
    char chosen[4096];
    int chosen_ready;
    int dialog_open;
    SDL_Mutex *lock;
    int gl_fallback;
    int quit;
    uint8_t *picture;
    int picture_w;
    int picture_h;
    uint8_t *opening;
    int opening_w;
    int opening_h;
    SfTrack *tracks;
    int track_count;
    int track_index;
    char track_label[48];
    SfWave fx[4];
} App;

static void set_status(App *app, const char *text)
{
    snprintf(app->status, sizeof app->status, "%s", text ? text : "");
    app->ui.status = app->status;
}

static int open_window(App *app, SfRendererKind kind, char *err, size_t err_n)
{
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    int w = app->settings.width > 0 ? app->settings.width : 1280;
    int h = app->settings.height > 0 ? app->settings.height : 720;
    if (app->settings.width == 0 || app->settings.height == 0) {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (mode && mode->w > 0 && mode->h > 0) {
            w = mode->w;
            h = mode->h;
        }
    }
    if (kind == SF_RENDER_OPENGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        flags |= SDL_WINDOW_OPENGL;
    } else {
        flags |= SDL_WINDOW_VULKAN;
    }
    app->window = SDL_CreateWindow(k_title, w, h, flags);
    if (!app->window) {
        sf_set_err(err, err_n, "%s", SDL_GetError());
        return -1;
    }
    if (sf_render_create(&app->renderer, app->window, kind, err, err_n) != 0) {
        SDL_DestroyWindow(app->window);
        app->window = NULL;
        return -1;
    }
    app->ui.active = kind;
    return 0;
}

static void close_window(App *app)
{
    sf_render_destroy(app->renderer);
    app->renderer = NULL;
    if (app->window) {
        SDL_DestroyWindow(app->window);
        app->window = NULL;
    }
}

static int boot_renderer(App *app)
{
    char err[512];
    if (open_window(app, app->settings.renderer, err, sizeof err) == 0) {
        app->gl_fallback = 0;
        return 0;
    }
    fprintf(stderr, "%s\n", err);
    if (app->settings.renderer == SF_RENDER_VULKAN && open_window(app, SF_RENDER_OPENGL, err, sizeof err) == 0) {
        app->gl_fallback = 1;
        app->ui.gl_fallback = 1;
        app->ui.active = SF_RENDER_OPENGL;
        return 0;
    }
    fprintf(stderr, "%s\n", err);
    return -1;
}

static void on_file(void *userdata, const char *const *filelist, int filter)
{
    App *app = userdata;
    (void)filter;
    SDL_LockMutex(app->lock);
    app->dialog_open = 0;
    app->chosen[0] = 0;
    app->chosen_ready = 0;
    if (filelist && filelist[0]) {
        snprintf(app->chosen, sizeof app->chosen, "%s", filelist[0]);
        app->chosen_ready = 1;
    }
    SDL_UnlockMutex(app->lock);
}

static void progress(const char *path, void *user)
{
    App *app = user;
    char line[512];
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path;
    snprintf(line, sizeof line, "READING %s", base ? base : "");
    set_status(app, line);
    SDL_SetWindowTitle(app->window, line);
    sf_ui_draw(&app->canvas, &app->ui);
    sf_render_present(app->renderer, &app->canvas, NULL, 0);
    SDL_PumpEvents();
}

static void clear_picture(App *app)
{
    sf_free(app->picture);
    app->picture = NULL;
    app->picture_w = 0;
    app->picture_h = 0;
    app->ui.picture = NULL;
    app->ui.picture_w = 0;
    app->ui.picture_h = 0;
    sf_free(app->opening);
    app->opening = NULL;
    app->opening_w = 0;
    app->opening_h = 0;
    app->ui.opening = NULL;
    app->ui.opening_w = 0;
    app->ui.opening_h = 0;
}

static void load_logo(App *app)
{
    SfFsFile file;
    SfPpgTex tex;
    char err[256];
    uint32_t fnum = sf_title_fnum(Country);
    const char *want = fnum == 0x54 ? "Title_R.ppg" : "TitleTM.ppg";
    memset(&file, 0, sizeof file);
    memset(&tex, 0, sizeof tex);
    err[0] = 0;
    if (fsOpen(app->install, fnum, &file, err, sizeof err) != 0) {
        return;
    }
    if (strcmp(file.name, want) != 0) {
        fsClose(&file);
        return;
    }
    if (sf_ppg_tex(file.data, file.size, 0, &tex, err, sizeof err) != 0 || !tex.rgba) {
        fprintf(stderr, "%s\n", err[0] ? err : "the title logo did not decode");
        sf_ppg_tex_free(&tex);
        fsClose(&file);
        return;
    }
    app->picture = tex.rgba;
    app->picture_w = tex.width;
    app->picture_h = tex.height;
    tex.rgba = NULL;
    sf_ppg_tex_free(&tex);
    fsClose(&file);
    app->ui.picture = app->picture;
    app->ui.picture_w = app->picture_w;
    app->ui.picture_h = app->picture_h;
}

/* OPBG_Init at 0x6A400 consumes Opening.ppg once RAMCNT type 0x1D holds it.
   color_file[154] names that member. put_chr2 passes palette 300, and the
   type-2 load of title.bin fills ColorRAM from row 300, so the rectangle
   byte is the row inside title.bin. Texture 0 has one rectangle. */
static int color_row(const uint8_t *file, uint32_t size, int row, uint16_t out[64])
{
    uint32_t at;
    int i;
    if (!file || !out || row < 0 || size < 128u || (size % 128u) != 0) {
        return -1;
    }
    if ((uint32_t)row >= size / 128u) {
        return -1;
    }
    at = (uint32_t)row * 128u;
    for (i = 0; i < 64; i++) {
        out[i] = (uint16_t)(file[at + (uint32_t)i * 2u] | ((uint16_t)file[at + (uint32_t)i * 2u + 1u] << 8));
    }
    return 0;
}

static void load_opening(App *app)
{
    SfFsFile image;
    SfFsFile colors;
    SfPpgTex tex;
    uint16_t row[64];
    char err[256];
    memset(&image, 0, sizeof image);
    memset(&colors, 0, sizeof colors);
    memset(&tex, 0, sizeof tex);
    err[0] = 0;
    if (fsOpen(app->install, 81, &image, err, sizeof err) != 0) {
        return;
    }
    if (strcmp(image.name, "Opening.ppg") != 0) {
        fsClose(&image);
        return;
    }
    if (sf_ppg_tex(image.data, image.size, 0, &tex, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "the opening tile did not decode");
        sf_ppg_tex_free(&tex);
        fsClose(&image);
        return;
    }
    if (fsOpen(app->install, 82, &colors, err, sizeof err) != 0 || strcmp(colors.name, "title.bin") != 0) {
        sf_ppg_tex_free(&tex);
        fsClose(&image);
        fsClose(&colors);
        return;
    }
    if (color_row(colors.data, colors.size, tex.pal, row) != 0 || sf_ppg_expand_ci(&tex, row, 64, err, sizeof err) != 0 || !tex.rgba) {
        fprintf(stderr, "%s\n", err[0] ? err : "the opening palette did not expand");
        sf_ppg_tex_free(&tex);
        fsClose(&image);
        fsClose(&colors);
        return;
    }
    app->opening = tex.rgba;
    app->opening_w = tex.width;
    app->opening_h = tex.height;
    tex.rgba = NULL;
    sf_ppg_tex_free(&tex);
    fsClose(&image);
    fsClose(&colors);
    app->ui.opening = app->opening;
    app->ui.opening_w = app->opening_w;
    app->ui.opening_h = app->opening_h;
}

static void clear_tracks(App *app)
{
    int i;
    sf_tracks_free(app->tracks);
    app->tracks = NULL;
    app->track_count = 0;
    app->track_index = 0;
    app->track_label[0] = 0;
    app->ui.track = NULL;
    for (i = 0; i < 4; i++) {
        sf_wave_free(&app->fx[i]);
    }
}

static int track_named(const App *app, const char *name)
{
    int i;
    for (i = 0; i < app->track_count; i++) {
        if (strcmp(app->tracks[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}

static void play_track(App *app, int index)
{
    SfFsFile file;
    SfAdx adx;
    char err[256];
    if (!app->tracks || app->track_count <= 0) {
        return;
    }
    if (index < 0) {
        index = app->track_count - 1;
    }
    if (index >= app->track_count) {
        index = 0;
    }
    memset(&file, 0, sizeof file);
    memset(&adx, 0, sizeof adx);
    err[0] = 0;
    if (fsOpen(app->install, app->tracks[index].fnum, &file, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "the track did not open");
        return;
    }
    if (strcmp(file.name, app->tracks[index].name) != 0 || sf_adx_decode(file.data, file.size, &adx, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "the track did not decode");
        sf_adx_free(&adx);
        fsClose(&file);
        return;
    }
    if (sf_audio_play(adx.pcm, adx.total, adx.rate, adx.channels, adx.loop, adx.loop_start, adx.loop_end) != 0) {
        fprintf(stderr, "the track did not play\n");
        sf_adx_free(&adx);
        fsClose(&file);
        return;
    }
    app->track_index = index;
    snprintf(app->track_label, sizeof app->track_label, "%s", app->tracks[index].name);
    app->ui.track = app->track_label;
    sf_adx_free(&adx);
    fsClose(&file);
}

static void load_effects(App *app)
{
    SfFsFile file;
    char err[256];
    int i;
    memset(&file, 0, sizeof file);
    err[0] = 0;
    if (fsOpen(app->install, 8, &file, err, sizeof err) != 0 || strcmp(file.name, "SE1.xwb") != 0) {
        fsClose(&file);
        return;
    }
    for (i = 0; i < 4; i++) {
        if (sf_xwb_entry(file.data, file.size, i, &app->fx[i], err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err[0] ? err : "an effect did not decode");
            sf_wave_free(&app->fx[i]);
        }
    }
    fsClose(&file);
}

static void play_effect(App *app, int slot)
{
    SfWave *wave;
    if (slot < 0 || slot >= 4) {
        return;
    }
    wave = &app->fx[slot];
    if (!wave->pcm) {
        return;
    }
    if (sf_audio_effect(slot, wave->pcm, wave->samples, wave->rate, wave->channels) != 0) {
        fprintf(stderr, "an effect did not play\n");
    }
}

static void load_bgm(App *app)
{
    char err[256];
    err[0] = 0;
    clear_tracks(app);
    if (sf_tracks_load(app->install, &app->tracks, &app->track_count, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "the track list did not load");
        return;
    }
    load_effects(app);
    play_track(app, track_named(app, "o01_A_NYC.adx"));
}

static void load_title(App *app)
{
    clear_picture(app);
    load_logo(app);
    load_opening(app);
    load_bgm(app);
}

static void run_import(App *app, const char *source)
{
    SfExtractResult result;
    const char *tool = app->tool[0] ? app->tool : NULL;
    set_status(app, "READING XBOX DATA");
    if (sf_extract(source, app->install, tool, progress, app, &result) == 0 && sf_data_ready(app->install)) {
        app->ui.screen = SF_UI_TITLE;
        set_status(app, "");
        snprintf(app->note, sizeof app->note, "%s", result.note);
        app->ui.note = app->note;
        load_title(app);
        SDL_SetWindowTitle(app->window, k_title);
        return;
    }
    set_status(app, result.error[0] ? result.error : "XBOX DATA WAS NOT READ");
}

static int preset_index(const SfSettings *settings)
{
    int i;
    for (i = 0; i < SF_RES_COUNT; i++) {
        if (sf_res_presets[i].width == settings->width && sf_res_presets[i].height == settings->height) {
            return i;
        }
    }
    return 1;
}

static void apply_display(App *app)
{
    SfSettings next = app->settings;
    SfRendererKind want = app->ui.menu_renderer == 1 ? SF_RENDER_OPENGL : SF_RENDER_VULKAN;
    char err[512];
    SDL_Window *old_window = app->window;
    SfRenderer *old_renderer = app->renderer;
    next.width = sf_res_presets[app->ui.menu_res].width;
    next.height = sf_res_presets[app->ui.menu_res].height;
    next.renderer = want;
    app->settings = next;
    app->window = NULL;
    app->renderer = NULL;
    if (open_window(app, want, err, sizeof err) == 0) {
        sf_render_destroy(old_renderer);
        SDL_DestroyWindow(old_window);
        app->gl_fallback = 0;
        app->ui.gl_fallback = 0;
        sf_settings_save(&app->settings, app->install);
        set_status(app, "");
        return;
    }
    if (want == SF_RENDER_VULKAN && open_window(app, SF_RENDER_OPENGL, err, sizeof err) == 0) {
        sf_render_destroy(old_renderer);
        SDL_DestroyWindow(old_window);
        app->settings.renderer = SF_RENDER_VULKAN;
        app->gl_fallback = 1;
        app->ui.gl_fallback = 1;
        app->ui.active = SF_RENDER_OPENGL;
        sf_settings_save(&app->settings, app->install);
        set_status(app, "VULKAN FAILED. OPENGL THIS SESSION.");
        return;
    }
    app->window = old_window;
    app->renderer = old_renderer;
    app->settings.renderer = app->ui.active == SF_RENDER_OPENGL && !app->gl_fallback ? SF_RENDER_OPENGL : app->settings.renderer;
    set_status(app, err);
}

static int extract_cli(int argc, char **argv)
{
    const char *source = NULL;
    const char *dir = NULL;
    char tool[4096];
    char cwd[4096];
    SfExtractResult result;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--extract") == 0 && i + 1 < argc) {
            source = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: sf33rdsxb [--extract SOURCE --data DIR]\n");
            return 0;
        }
    }
    if (!source && !dir) {
        return -1;
    }
    if (!source || !dir) {
        fprintf(stderr, "usage: sf33rdsxb --extract SOURCE --data DIR\n");
        return 1;
    }
    tool[0] = 0;
    if (getcwd(cwd, sizeof cwd) && sf_find_shader_tool(cwd, tool, sizeof tool) != 0) {
        tool[0] = 0;
    }
    if (sf_extract(source, dir, tool[0] ? tool : NULL, NULL, NULL, &result) != 0) {
        fprintf(stderr, "%s\n", result.error);
        return 1;
    }
    printf("files %d\nshaders %d\nspirv %d\nspirv_failed %d\n", result.files, result.shaders, result.spirv, result.spirv_failed);
    if (result.note[0]) {
        printf("%s\n", result.note);
    }
    return sf_data_ready(dir) ? 0 : 1;
}

int main(int argc, char **argv)
{
    App app;
    int cli = extract_cli(argc, argv);
    char err[256];
    if (cli >= 0) {
        return cli;
    }
    memset(&app, 0, sizeof app);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    if (sf_exe_dir(app.install, sizeof app.install) != 0) {
        fprintf(stderr, "cannot find the install directory\n");
        SDL_Quit();
        return 1;
    }
    sf_find_shader_tool(app.install, app.tool, sizeof app.tool);
    sf_settings_load(&app.settings, app.install);
    if (sf_audio_open(err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
    }
    sf_input_init();
    if (sf_canvas_open(&app.canvas, 960, 540) != 0 || boot_renderer(&app) != 0) {
        fprintf(stderr, "window failed\n");
        sf_audio_close();
        sf_input_quit();
        SDL_Quit();
        return 1;
    }
    mmSystemInitialize();
    xb_ClearTasks();
    app.lock = SDL_CreateMutex();
    app.ui.screen = sf_data_ready(app.install) ? SF_UI_TITLE : SF_UI_SETUP;
    app.ui.menu_res = preset_index(&app.settings);
    app.ui.menu_renderer = app.settings.renderer == SF_RENDER_OPENGL ? 1 : 0;
    app.ui.active = sf_render_kind(app.renderer);
    app.ui.gl_fallback = app.gl_fallback;
    app.ui.status = app.status;
    app.ui.note = app.note;
    if (app.ui.screen == SF_UI_TITLE) {
        load_title(&app);
    }
    if (app.ui.screen == SF_UI_TITLE && app.tool[0]) {
        SfExtractResult retry;
        if (sf_retry_shaders(app.install, app.tool, &retry) == 0 && retry.note[0]) {
            snprintf(app.note, sizeof app.note, "%s", retry.note);
        }
    }
    while (!app.quit) {
        SDL_Event event;
        sf_input_poll();
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                app.quit = 1;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE) {
                if (app.ui.screen == SF_UI_DISPLAY || app.ui.screen == SF_UI_LIVE) {
                    app.ui.screen = SF_UI_TITLE;
                } else {
                    app.quit = 1;
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && app.ui.screen == SF_UI_TITLE && (event.key.key == SDLK_A || event.key.key == SDLK_S || event.key.key == SDLK_D || event.key.key == SDLK_F)) {
                int slot = event.key.key == SDLK_A ? 0 : event.key.key == SDLK_S ? 1 : event.key.key == SDLK_D ? 2 : 3;
                play_effect(&app, slot);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F && app.ui.screen == SF_UI_SETUP && !app.dialog_open) {
                app.dialog_open = 1;
                SDL_ShowOpenFolderDialog(on_file, &app, app.window, NULL, false);
            } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                SDL_LockMutex(app.lock);
                snprintf(app.chosen, sizeof app.chosen, "%s", event.drop.data);
                app.chosen_ready = 1;
                SDL_UnlockMutex(app.lock);
            } else {
                sf_input_event(&event);
            }
        }
        if (app.ui.screen == SF_UI_SETUP && sf_pressed(SF_START) && !app.dialog_open) {
            app.dialog_open = 1;
            SDL_ShowOpenFileDialog(on_file, &app, app.window, k_filters, 2, NULL, false);
        }
        SDL_LockMutex(app.lock);
        if (app.chosen_ready) {
            char path[4096];
            snprintf(path, sizeof path, "%s", app.chosen);
            app.chosen_ready = 0;
            SDL_UnlockMutex(app.lock);
            run_import(&app, path);
        } else {
            SDL_UnlockMutex(app.lock);
        }
        if (app.ui.screen == SF_UI_TITLE) {
            if (sf_pressed(SF_START)) {
                app.ui.screen = SF_UI_DISPLAY;
                app.ui.menu_row = 0;
                app.ui.menu_res = preset_index(&app.settings);
                app.ui.menu_renderer = app.settings.renderer == SF_RENDER_OPENGL ? 1 : 0;
            } else if (sf_pressed(SF_BACK)) {
                app.ui.screen = SF_UI_LIVE;
            } else if (sf_pressed(SF_LEFT) || sf_pressed(SF_DOWN)) {
                play_track(&app, app.track_index - 1);
            } else if (sf_pressed(SF_RIGHT) || sf_pressed(SF_UP)) {
                play_track(&app, app.track_index + 1);
            }
        } else if (app.ui.screen == SF_UI_DISPLAY) {
            if (sf_pressed(SF_UP) || sf_pressed(SF_DOWN)) {
                app.ui.menu_row ^= 1;
            } else if (sf_pressed(SF_LEFT) || sf_pressed(SF_RIGHT)) {
                int step = sf_pressed(SF_RIGHT) ? 1 : -1;
                if (app.ui.menu_row == 0) {
                    app.ui.menu_res = (app.ui.menu_res + step + SF_RES_COUNT) % SF_RES_COUNT;
                } else {
                    app.ui.menu_renderer ^= 1;
                }
            } else if (sf_pressed(SF_A)) {
                apply_display(&app);
            } else if (sf_pressed(SF_B)) {
                app.ui.screen = SF_UI_TITLE;
            }
        } else if (app.ui.screen == SF_UI_LIVE && sf_pressed(SF_B)) {
            app.ui.screen = SF_UI_TITLE;
        }
        app.ui.gl_fallback = app.gl_fallback;
        app.ui.active = app.gl_fallback ? SF_RENDER_OPENGL : sf_render_kind(app.renderer);
        xb_TurboFrameStep();
        sf_audio_pump();
        sf_ui_draw(&app.canvas, &app.ui);
        if (sf_render_present(app.renderer, &app.canvas, err, sizeof err) != 0 && !app.gl_fallback) {
            close_window(&app);
            app.settings.renderer = SF_RENDER_VULKAN;
            if (open_window(&app, SF_RENDER_OPENGL, err, sizeof err) == 0) {
                app.gl_fallback = 1;
                app.ui.gl_fallback = 1;
            } else {
                app.quit = 1;
            }
        }
    }
    close_window(&app);
    clear_picture(&app);
    sf_canvas_close(&app.canvas);
    if (app.lock) {
        SDL_DestroyMutex(app.lock);
    }
    sf_input_quit();
    sf_audio_close();
    clear_tracks(&app);
    SDL_Quit();
    return 0;
}
