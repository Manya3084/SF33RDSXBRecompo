/* The native entry. The game itself starts in sf_game_init. */

#include "port/port.h"

#include <SDL3/SDL.h>

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "host/paths.h"

/* A crash writes where it happened to stderr and data/crash.log. */
static char s_crash_path[4096 + 32];

static void on_crash(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    char head[128];
    int fd;
    int len = snprintf(head, sizeof head, "crash: signal %d (%s) at game frame %d\n", sig, strsignal(sig), sf_port_frame);
    (void)!write(2, head, (size_t)len);
    backtrace_symbols_fd(frames, n, 2);
    fd = open(s_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)!write(fd, head, (size_t)len);
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_handler(void)
{
    char base[4096];
    if (sf_exe_dir(base, sizeof base) == 0) {
        snprintf(s_crash_path, sizeof s_crash_path, "%s/data/crash.log", base);
    }
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGBUS, on_crash);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    crash_handler();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    sf_game_init();
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                SDL_Quit();
                return 0;
            }
        }
        sf_game_frame();
    }
}
